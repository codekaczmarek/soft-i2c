#include "soft_i2c.h"

#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#if defined(USE_LIBRETINY)
// Per-instance GPIOPin for any pad number - the pair sweep does not know the pins from the YAML.
// ArduinoInternalGPIOPin is a thin wrapper over pinMode/digitalWrite/digitalRead (see
// components/libretiny/gpio_arduino.cpp), so the drive commands travel the same path as everywhere else.
#include "esphome/components/libretiny/gpio_arduino.h"
#endif

#if defined(USE_BK72XX)
// BK7231 SDK ships a "JTAG pad filter" (beken378/driver/gpio/gpio.c: gpio_ops_filter) that silently
// swallows gpio_config() and gpio_output() for GPIO20..23 - no error, the pad simply never moves.
// The SDK clears it only in gpio_ops_disable_filter(), and the only caller of that sits on the
// deep-sleep entry path (sys_ctrl.c, and LibreTiny's copy in sctrl_enter_rtos_deep_sleep_fix()).
// ESPHome on bk72xx has no deep sleep, so that path never runs and the filter stays active forever.
// This matters because the CHT8305 on the Tuya CBU T&H boards is hardwired to GPIO22/GPIO20: with
// the filter active every transaction times out (last_err=2) and the line probe reads driven=0/0.
// Declared locally, exactly like the SDK's own caller does - there is no public header for it.
extern "C" void gpio_ops_disable_filter(void);

// BK7231 pad configuration register: beken378/driver/gpio/gpio.h:17-19 defines
// GPIO_BASE_ADDR = 0x0802800 and REG_GPIO_n_CONFIG = base + n*4 (one word per pin).
// Bit map from the same header (GCFG_*_POS): b0 INPUT, b1 OUTPUT, b2 INPUT_ENABLE,
// b3 OUTPUT_ENABLE, b4 PULL_MODE, b5 PULL_ENABLE, b6 FUNCTION_ENABLE, b7 INPUT_MONITOR.
// What matters for this board: SDA=GPIO22 and SCL=GPIO20 are simultaneously the JTAG TDI/TCK lines and the
// flash FSI/FSCK lines (bk7231-qfn32 map), so a logic-level read alone proves nothing - you have to see
// what actually sits in the pad register.
static const uint32_t BK_GPIO_CFG_BASE = 0x00802800u;
static const uint32_t BK_PIN_UNKNOWN = 0xFFFFFFFFu;

static uint32_t bk_pin_cfg_(uint32_t pin) { return *(volatile uint32_t *) (BK_GPIO_CFG_BASE + pin * 4u); }

// GPIO number read from dump_summary() - the only public API of the base GPIOPin class that
// contains it (get_pin() only appears on InternalGPIOPin, and casting down is UB without RTTI,
// which ESPHome does not enable). LibreTiny stores the number itself here ("22"), so we take the first
// block of digits; no digits = unknown pin, in which case the cfgDrv fields stay zero.
static uint32_t bk_pin_number_(esphome::GPIOPin *pin) {
  char buf[esphome::GPIO_SUMMARY_MAX_LEN];
  if (pin == nullptr || pin->dump_summary(buf, sizeof(buf)) == 0) {
    return BK_PIN_UNKNOWN;
  }
  uint32_t value = 0;
  bool any = false;
  for (const char *p = buf; *p != '\0'; ++p) {
    if (*p >= '0' && *p <= '9') {
      value = value * 10u + (uint32_t) (*p - '0');
      any = true;
    } else if (any) {
      break;
    }
  }
  return any ? value : BK_PIN_UNKNOWN;
}
#endif

namespace esphome {
namespace soft_i2c {

static const char *const TAG = "soft_i2c";

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

void SoftI2CBus::set_frequency(uint32_t frequency) {
  this->frequency_ = frequency;
  // Half period of the SCL square wave. Never ask for less than 1 us: at 1 us half period
  // (400 kHz target) the GPIO + virtual-call overhead already dominates, so asking for more
  // only makes the bus run slower than requested, which is always safe.
  this->half_period_us_ = 500000u / frequency;
  if (this->half_period_us_ < 1) {
    this->half_period_us_ = 1;
  }
}

void SoftI2CBus::dump_config() {
  char sda_sum[GPIO_SUMMARY_MAX_LEN] = "?";
  char scl_sum[GPIO_SUMMARY_MAX_LEN] = "?";
  if (this->sda_ != nullptr) {
    this->sda_->dump_summary(sda_sum, sizeof(sda_sum));
  }
  if (this->scl_ != nullptr) {
    this->scl_->dump_summary(scl_sum, sizeof(scl_sum));
  }
  ESP_LOGCONFIG(TAG,
                "soft_i2c v%s | bus SDA=%s SCL=%s %uHz %s-pullup timeout=%uus | setup=%d | probe idle=%d/%d "
                "driven=%d/%d | loop=%d/%d | cfgdrv=0x%04X/0x%04X | scan=%d found=%d first=0x%02X | errors=%u last_err=%d@0x%02X "\
                "scl_stuck=%d stretch=%uus jtagfilter=%d",
                SOFT_I2C_VERSION, sda_sum, scl_sum, (unsigned) this->frequency_,
                this->internal_pullup_ ? "internal" : "external", (unsigned) this->timeout_us_,
                (int) this->setup_ran_, this->probe_idle_sda_, this->probe_idle_scl_, this->probe_drv_sda_,
                this->probe_drv_scl_, this->probe_loop_sda_, this->probe_loop_scl_,
                (unsigned) this->probe_cfg_sda_drv_, (unsigned) this->probe_cfg_scl_drv_, (int) this->scan_ran_,
                this->scan_found_, (unsigned) this->scan_first_, (unsigned) this->error_count_,
                this->last_err_, (unsigned) this->last_err_addr_, this->scl_stuck_,
                (unsigned) this->stretch_timeout_us_, (int) this->jtag_filter_off_);
#if defined(USE_BK72XX)
  // Read of the pad configuration register (GPIO_BASE_ADDR + n*4) + instrument controls.
  // pins=-1 means the number was not read, in which case cfgdrv is zero by definition.
  // ctl(uart2)=0 invalidates the whole read: even a pad configured by the logger came out zero.
  ESP_LOGCONFIG(TAG,
                "soft_i2c pads | pins=%d/%d cfgidle=0x%04X/0x%04X cfgdrv=0x%04X/0x%04X ctl(uart2)=0x%04X "
                "ref(free)=0x%04X",
                this->probe_pin_sda_, this->probe_pin_scl_, (unsigned) this->probe_cfg_sda_idle_,
                (unsigned) this->probe_cfg_scl_idle_, (unsigned) this->probe_cfg_sda_drv_,
                (unsigned) this->probe_cfg_scl_drv_,                (unsigned) this->probe_cfg_ctl_,
                (unsigned) this->probe_cfg_ref_);
#endif

  // Actuator self-verification. The words "power_pin: 8" in YAML mean nothing - what counts is what
  // sits in the pad register. On BK7231 gpio_output() sets GCFG_OUTPUT_BIT (bit 1) in the same
  // word as gpio_config(), therefore: HIGH = 0x0002, LOW = 0x0000, a pad not taken over = somebody else.
  // Safeguard against a conclusion drawn from an experiment that never actually ran.
  ESP_LOGCONFIG(TAG, "soft_i2c power | pin=%d applied=%d delay=%ums", (int) this->power_pin_num_,
                (int) this->power_applied_, (unsigned) this->power_on_delay_ms_);
#if defined(USE_BK72XX)
  if (this->power_pin_num_ >= 0) {
    ESP_LOGCONFIG(TAG, "soft_i2c power cfg=%s (HIGH=0x0002, LOW=0x0000)%s",
                  (this->power_cfg_low_ == this->power_cfg_high_)
                      ? "no transition - the pad is NOT ours"
                      : "transition 0x0002->0x0000 observed",
                  this->power_active_low_ ? " (active low)" : "");
    ESP_LOGCONFIG(TAG, "soft_i2c power cfg high=0x%04X final=0x%04X", (unsigned) this->power_cfg_high_,
                  (unsigned) this->power_cfg_low_);
  }
  if (!this->slow_pairs_.empty()) {
    ESP_LOGCONFIG(TAG,
                  "soft_i2c slow probe | probes=%d hits=%d | ACK: SDA=%u SCL=%u @0x%02X at %lu Hz (slowest ACK: %lu Hz)",
                  this->slow_tried_, this->slow_hits_, (unsigned) this->slow_hit_sda_,
                  (unsigned) this->slow_hit_scl_, (unsigned) this->slow_hit_addr_,
                  (unsigned long) this->slow_hit_freq_, (unsigned long) this->slow_min_freq_);
  }
  if (this->pair_sweep_) {
    ESP_LOGCONFIG(TAG,
                  "soft_i2c sweep | tried=%d hits=%d filtered=%d ms=%u budget_hit=%d | first: SDA=%u SCL=%u @0x%02X",
                  this->sweep_tried_, this->sweep_hits_, this->sweep_filtered_out_,
                  (unsigned) this->sweep_ms_, this->sweep_budget_hit_, (unsigned) this->sweep_sda_,
                  (unsigned) this->sweep_scl_, (unsigned) this->sweep_addr_);
  }
  if (!this->diag_pins_.empty()) {
    char buf[128];
    size_t off = 0;
    buf[0] = '\0';
    for (const uint8_t p : this->diag_pins_) {
      const int n = snprintf(buf + off, sizeof(buf) - off, " p%u=0x%04X", (unsigned) p,
                             (unsigned) bk_pin_cfg_((uint32_t) p));
      if (n < 0 || (size_t) n >= sizeof(buf) - off) {
        break;
      }
      off += (size_t) n;
    }
    ESP_LOGCONFIG(TAG, "soft_i2c diag pads |%s", buf);
  }
#endif
}

// ---------------------------------------------------------------------------
// line control
// ---------------------------------------------------------------------------

gpio::Flags SoftI2CBus::input_flags_() const {
  if (this->internal_pullup_) {
    return gpio::Flags(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
  }
  return gpio::FLAG_INPUT;
}

void SoftI2CBus::delay_() const { delayMicroseconds(this->half_period_us_); }

void SoftI2CBus::release_sda_() { this->sda_->pin_mode(this->input_flags_()); }

void SoftI2CBus::drive_sda_low_() {
  if (this->drive_mode_ == 1) {
    // A resistor instead of a driver: the pull-down is weak, but it is REAL - and GMODE_OUTPUT does
    // not guarantee a strong LOW at all, since the entire SDK never sets GCFG_OUTPUT_ENABLE_BIT anywhere.
    this->sda_->pin_mode(gpio::Flags(gpio::FLAG_INPUT | gpio::FLAG_PULLDOWN));
    return;
  }
  // Canonical Arduino order: the pad switches to OUTPUT, and ONLY then does the state go out.
  // The reverse order (digitalWrite -> pin_mode) was our battle-tested ESP32 practice and on
  // this platform it erased the emulated LOW: gpio_config() writes the whole word 0x00 after
  // pinSetOutputPull had managed to set INPUT_PULLDOWN.
  this->sda_->pin_mode(gpio::FLAG_OUTPUT);
  this->sda_->digital_write(false);
}

void SoftI2CBus::release_scl_() { this->scl_->pin_mode(this->input_flags_()); }

void SoftI2CBus::drive_scl_low_() {
  if (this->drive_mode_ == 1) {
    this->scl_->pin_mode(gpio::Flags(gpio::FLAG_INPUT | gpio::FLAG_PULLDOWN));
    return;
  }
  this->scl_->pin_mode(gpio::FLAG_OUTPUT);
  this->scl_->digital_write(false);
}

bool SoftI2CBus::read_sda_() {
  this->sda_->pin_mode(this->input_flags_());
  return this->sda_->digital_read();
}

bool SoftI2CBus::read_scl_() {
  this->scl_->pin_mode(this->input_flags_());
  return this->scl_->digital_read();
}


// ---------------------------------------------------------------------------
// SDA/SCL pair sweep
// ---------------------------------------------------------------------------

void SoftI2CBus::run_pair_sweep_() {
#if defined(USE_LIBRETINY)
  if (this->sweep_pins_.size() < 2) {
    return;
  }
  libretiny::ArduinoInternalGPIOPin sda_pin;
  libretiny::ArduinoInternalGPIOPin scl_pin;
  GPIOPin *const save_sda = this->sda_;
  GPIOPin *const save_scl = this->scl_;
  const uint32_t t0 = millis();

  // Preliminary filter: a pad that cannot deliver LOW (drive) or HIGH (pull-up after release)
  // cannot be an I2C line and there is no point paying a full scan for it. Without this filter every
  // pad occupied by another function costs a full SCL timeout (10 ms) times the number of addresses.
  std::vector<uint8_t> usable;
  for (const uint8_t p : this->sweep_pins_) {
    sda_pin.set_pin(p);
    // Same order as in drive_*_low_(): mode first, then state. Note - a read in OUTPUT mode
    // will always return zero anyway (GMODE_OUTPUT disables the input path), so what really
    // decides is the pull-up test below, not this one.
    sda_pin.pin_mode(gpio::FLAG_OUTPUT);
    sda_pin.digital_write(false);
    delayMicroseconds(20);
    const bool drives_low = !sda_pin.digital_read();
    sda_pin.pin_mode(this->input_flags_());
    delayMicroseconds(20);
    const bool pulls_high = sda_pin.digital_read();
    arch_feed_wdt();
    if (drives_low && pulls_high) {
      usable.push_back(p);
    } else {
      this->sweep_filtered_out_++;
    }
  }

  for (const uint8_t a : usable) {
    for (const uint8_t b : usable) {
      if (a == b) {
        continue;
      }
      if (millis() - t0 > this->sweep_budget_ms_) {
        this->sweep_budget_hit_ = 1;
        this->sda_ = save_sda;
        this->scl_ = save_scl;
        this->sweep_ms_ = millis() - t0;
        return;
      }
      arch_feed_wdt();
      sda_pin.set_pin(a);
      scl_pin.set_pin(b);
      this->sda_ = &sda_pin;
      this->scl_ = &scl_pin;
      this->release_sda_();
      this->release_scl_();
      this->sweep_tried_++;
      for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        bool ack = false;
        if (this->start_()) {
          ack = this->write_address_(addr, false);
        }
        this->stop_();
        if (ack) {
          this->sweep_hits_++;
          if (this->sweep_hits_ == 1) {
            this->sweep_sda_ = a;
            this->sweep_scl_ = b;
            this->sweep_addr_ = addr;
          }
          ESP_LOGI(TAG, "pair sweep: ACK on SDA=%u SCL=%u at 0x%02X", (unsigned) a, (unsigned) b,
                   (unsigned) addr);
        }
      }
    }
  }
  this->sda_ = save_sda;
  this->scl_ = save_scl;
  this->sweep_ms_ = millis() - t0;
#endif
}


// ---------------------------------------------------------------------------
// frequency ladder on the selected pairs
// ---------------------------------------------------------------------------

void SoftI2CBus::run_slow_probe_() {
#if defined(USE_LIBRETINY)
  if (this->slow_pairs_.size() < 2 || this->slow_freqs_.empty()) {
    return;
  }
  libretiny::ArduinoInternalGPIOPin sda_pin;
  libretiny::ArduinoInternalGPIOPin scl_pin;
  GPIOPin *const save_sda = this->sda_;
  GPIOPin *const save_scl = this->scl_;
  const uint32_t saved_half = this->half_period_us_;
  std::vector<uint8_t> addrs = this->slow_addrs_;
  if (addrs.empty()) {
    addrs.push_back(0x40);
  }

  for (size_t i = 0; i + 1 < this->slow_pairs_.size(); i += 2) {
    const uint8_t a = this->slow_pairs_[i];
    const uint8_t b = this->slow_pairs_[i + 1];
    sda_pin.set_pin(a);
    scl_pin.set_pin(b);
    this->sda_ = &sda_pin;
    this->scl_ = &scl_pin;
    for (const uint32_t f : this->slow_freqs_) {
      arch_feed_wdt();
      // Half period computed exactly as in set_frequency(), so we probe precisely the
      // frequency that would later be written into the YAML.
      uint32_t half = 500000u / (f == 0 ? 1u : f);
      this->half_period_us_ = half < 1 ? 1 : half;
      this->release_sda_();
      this->release_scl_();
      this->slow_tried_++;
      for (const uint8_t addr : addrs) {
        bool ack = false;
        if (this->start_()) {
          ack = this->write_address_(addr, false);
        }
        this->stop_();
        this->slow_hits_ += ack ? 1 : 0;
        if (ack) {
          this->slow_hit_sda_ = a;
          this->slow_hit_scl_ = b;
          this->slow_hit_addr_ = addr;
          this->slow_hit_freq_ = f;
          ESP_LOGI(TAG, "slow probe: ACK SDA=%u SCL=%u @0x%02X at %lu Hz", (unsigned) a,
                   (unsigned) b, (unsigned) addr, (unsigned long) f);
          if (this->slow_min_freq_ == 0 || f < this->slow_min_freq_) {
            this->slow_min_freq_ = f;
          }
        }
      }
    }
  }
  this->half_period_us_ = saved_half;
  this->sda_ = save_sda;
  this->scl_ = save_scl;
#endif
}

// ---------------------------------------------------------------------------
// raw T&H protocols and an electrical census of the pads
// ---------------------------------------------------------------------------

/// Hex into a static buffer, no heap allocation - the chip has 256 KB RAM and every needless
/// allocation on the logging path pays in fragmentation. There is a single buffer (logging is
/// single-threaded and the value is consumed on the same line).
static const char *hexbuf_(const uint8_t *data, size_t len) {
  static char buf[64];
  format_hex_to(buf, data, len > 25 ? 25 : len);
  return buf;
}

/// Sensirion CRC-8, shared by SHT3x and SHT4x: polynomial 0x31, init 0xFF, no reflection and no
/// XOR-out. ESPHome uses the same pattern in sht3xd; if our bytes pass its CRC,
/// the read is truly correct, not just "some" read.
static uint8_t sensirion_crc8_(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) != 0 ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

/// Three T&H protocols under the same address, with the bytes and a CRC verdict in the log.
///
/// Why: a scan only says "someone ACKs". Three popular chips on CBU (SHT30, SHT40, CHT8305)
/// ACK identically and speak three different languages - SHT3x has a CRC in every block,
/// SHT4x has a CRC-less command, CHT8305 is raw registers without CRC. Guessing at the sensor
/// platform gave "Retrying to reconnect" without any clue; here you see exactly which protocol
/// returns coherent data.
void SoftI2CBus::run_protocol_probe_() {
  std::vector<uint8_t> addrs = this->protocol_probe_addrs_;
  if (addrs.empty()) {
    addrs.push_back(0x44);
  }

  for (const uint8_t addr : addrs) {
    // --- SHT3x: command 0x2C06 (single-shot, high repeatability), 6 bytes: T CRC H CRC -------
    // Two transaction configurations. ESPHome sends write_bytes() and read_bytes() as a SECOND
    // transaction (stop + start), while our write_readv path uses a repeated start. Clones
    // react to this differently, so we log both - "one works, the other does not" is also a result.
    const uint8_t cmd3[2] = {0x2C, 0x06};
    for (int variant = 0; variant < 2; variant++) {
      arch_feed_wdt();
      uint8_t buf[6] = {0};
      i2c::ErrorCode err;
      if (variant == 0) {
        err = this->write_readv(addr, cmd3, sizeof(cmd3), buf, sizeof(buf));
      } else {
        err = this->write_readv(addr, cmd3, sizeof(cmd3), nullptr, 0);
        if (err == i2c::ERROR_OK) {
          err = this->write_readv(addr, nullptr, 0, buf, sizeof(buf));
        }
      }
      const bool crc_ok = err == i2c::ERROR_OK && sensirion_crc8_(buf, 2) == buf[2] &&
                          sensirion_crc8_(buf + 3, 2) == buf[5];
      const uint16_t raw_t = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
      const uint16_t raw_h = static_cast<uint16_t>((buf[3] << 8) | buf[4]);
      ESP_LOGI(TAG, "probe sht3x @0x%02X %s: err=%d data=%s crc=%s T=%.2f RH=%.1f", (unsigned) addr,
               variant == 0 ? "repeated-start" : "stop-start", (int) err,
               hexbuf_(buf, sizeof(buf)), crc_ok ? "OK" : "BAD",
               crc_ok ? (-46.85f + 175.72f * raw_t / 65536.0f) : 0.0f,
               crc_ok ? (100.0f * raw_h / 65536.0f) : 0.0f);
      delay(25);
    }

    // --- SHT4x: command 0xFD (measure, high repeatability) with no CRC in the command, 6 bytes -------
    arch_feed_wdt();
    const uint8_t cmd4[1] = {0xFD};
    uint8_t buf4[6] = {0};
    const i2c::ErrorCode err4 = this->write_readv(addr, cmd4, sizeof(cmd4), buf4, sizeof(buf4));
    const bool crc4 = err4 == i2c::ERROR_OK && sensirion_crc8_(buf4, 2) == buf4[2] &&
                      sensirion_crc8_(buf4 + 3, 2) == buf4[5];
    ESP_LOGI(TAG, "probe sht4x @0x%02X: err=%d data=%s crc=%s", (unsigned) addr, (int) err4,
             hexbuf_(buf4, sizeof(buf4)), crc4 ? "OK" : "BAD");
    delay(15);

    // --- register-based (CHT8305 and friends): 0x00 = temperature, 0x01 = humidity, 16 bits, no CRC -
    for (uint8_t reg = 0; reg <= 1; reg++) {
      arch_feed_wdt();
      const uint8_t cmdr[1] = {reg};
      uint8_t bufr[2] = {0};
      const i2c::ErrorCode errr = this->write_readv(addr, cmdr, sizeof(cmdr), bufr, sizeof(bufr));
      const uint16_t raw = static_cast<uint16_t>((bufr[0] << 8) | bufr[1]);
      ESP_LOGI(TAG, "probe reg 0x%02X @0x%02X: err=%d data=%s raw=0x%04X T=%.2f RH=%.1f",
               (unsigned) reg, (unsigned) addr, (int) errr, hexbuf_(bufr, 2),
               (unsigned) raw, -46.85f + 175.72f * raw / 65536.0f, 100.0f * raw / 65536.0f);
      delay(5);
    }
  }

  // The probe sends measurement commands, and that is exactly the moment when SHT3x stretches SCL. If any
  // transaction was aborted, the slave is left mid-transmission holding the clock -
  // and the next scan sees an empty bus (found=0 where a build without the probe had found=1).
  // Every probe pass therefore ends with a formal recovery so that the main path starts
  // from a clean bus.
  this->recover_();
  this->release_sda_();
  this->release_scl_();
}

/// Electrical census of the pads behind buttons and LEDs - without peeking at the schematic and without guessing.
///
/// The pad is read twice: with an internal pull-up and with an internal pull-down (both tens of
/// kOhm, a weak but REAL resistor - see the note on GMODE_INPUT_PULLDOWN). Interpretation:
///   0/0  pad permanently tied to ground   -> button to GND or a GND net
///   1/1  pad permanently tied to VDD      -> LED through a resistor, external pull-up, a line to 3V3
///   1/0  floating pad                      -> nothing holds it, fit for any use
void SoftI2CBus::run_pad_census_() {
#if defined(USE_LIBRETINY)
  libretiny::ArduinoInternalGPIOPin pin;
  for (const uint8_t p : this->census_pins_) {
    arch_feed_wdt();
    pin.set_pin(p);
    pin.pin_mode(gpio::Flags(gpio::FLAG_INPUT | gpio::FLAG_PULLUP));
    delayMicroseconds(120);
    const int with_up = pin.digital_read() ? 1 : 0;
    pin.pin_mode(gpio::Flags(gpio::FLAG_INPUT | gpio::FLAG_PULLDOWN));
    delayMicroseconds(120);
    const int with_dn = pin.digital_read() ? 1 : 0;
    // Return the pad to input WITHOUT external resistors. An earlier version ended on a
    // pull-down and the pad stayed loaded with ~50 kOhm to ground for the rest of the process lifetime -
    // enough for a pad later meant to be a button or an ADC input to be silently fooled.
    pin.pin_mode(gpio::Flags(gpio::FLAG_INPUT));

    const char *kind;
    if (!with_up && !with_dn) {
      kind = "GND";
      this->census_tied_low_++;
    } else if (with_up && with_dn) {
      kind = "VDD";
      this->census_tied_high_++;
    } else {
      kind = "FLOATING";
      this->census_floating_++;
    }
    ESP_LOGI(TAG, "census pad %2u: pullup=%d pulldown=%d -> %s", (unsigned) p, with_up, with_dn, kind);
  }
  ESP_LOGI(TAG, "census | pads=%d gnd=%d vdd=%d floating=%d", (int) this->census_pins_.size(),
           this->census_tied_low_, this->census_tied_high_, this->census_floating_);
  ESP_LOGW(TAG, "census cannot find an LED: a diode needs ~1.8 V, so an internal pull-down\n"
                "  (tens of kOhm) drives no current and an LED pad reads exactly like a floating one;\n"
                "  only driving the pad and watching the board settles it");
#endif
}

// ---------------------------------------------------------------------------
// low level protocol
// ---------------------------------------------------------------------------

/// Wait until SCL is actually high. A clock-stretching slave holds it low, a missing pull-up
/// keeps it low forever - hence the timeout.
bool SoftI2CBus::wait_scl_high_(uint32_t limit_us) {
  const uint32_t start = micros();
  const uint32_t limit = limit_us != 0 ? limit_us : this->timeout_us_;
  this->scl_->pin_mode(this->input_flags_());
  while (!this->scl_->digital_read()) {
    if (micros() - start > limit) {
      // Record the fact that SCL stayed low longer than our patience. Without this the log only says
      // "err=3", and "the slave finished its conversion late" looks identical in the log to "someone shorted SCL
      // to ground" - and those are two different diseases with opposite cures.
      this->scl_stuck_++;
      return false;
    }
    arch_feed_wdt();
  }
  return true;
}

/// START condition: SDA falls while SCL is high.
bool SoftI2CBus::start_() {
  this->release_sda_();
  this->release_scl_();
  if (!this->wait_scl_high_()) {
    return false;  // clock held low by a stuck slave
  }
  if (!this->read_sda_()) {
    return false;  // data line held low -> bus is stuck mid transfer
  }
  this->delay_();
  this->drive_sda_low_();
  this->delay_();
  this->drive_scl_low_();
  this->delay_();
  return true;
}

/// STOP condition: SDA rises while SCL is high.
void SoftI2CBus::stop_() {
  this->drive_sda_low_();
  this->delay_();
  this->release_scl_();
  this->delay_();
  this->release_sda_();
  this->delay_();
}

/// REPEATED START. Not the same as start_(): the bus is deliberately busy here, so the "is the
/// bus idle" pre-checks of start_() would report a false stuck-bus error. With SCL held low we
/// set SDA high, raise SCL, then drop SDA - that falling edge while SCL is high is the Sr
/// condition. SHT3xD reads need exactly this (write command, Sr, read measurement).
bool SoftI2CBus::restart_() {
  this->drive_sda_low_();
  this->delay_();
  this->release_sda_();
  this->delay_();
  this->release_scl_();
  if (!this->wait_scl_high_()) {
    return false;  // clock stretching longer than the timeout
  }
  this->delay_();
  this->drive_sda_low_();
  this->delay_();
  this->drive_scl_low_();
  this->delay_();
  return true;
}

/// Write one byte, MSB first, then read the ACK. Returns false on NACK or timeout.
bool SoftI2CBus::write_byte_(uint8_t data) {
  for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
    if (data & mask) {
      this->release_sda_();
    } else {
      this->drive_sda_low_();
    }
    this->delay_();
    this->release_scl_();
    if (!this->wait_scl_high_()) {
      return false;
    }
    this->delay_();
    this->drive_scl_low_();
    this->delay_();
  }

  // ACK slot: release SDA and let the slave pull it low
  this->release_sda_();
  this->delay_();
  this->release_scl_();
  if (!this->wait_scl_high_()) {
    return false;
  }
  this->delay_();
  const bool ack = this->read_sda_();
  this->delay_();
  this->drive_scl_low_();
  this->delay_();
  return !ack;
}

/// Read one byte, MSB first, then send ACK (ack=true) or NACK. Returns false on clock timeout.
bool SoftI2CBus::read_byte_(uint8_t *out, bool ack) {
  uint8_t data = 0;
  this->release_sda_();
  for (uint8_t bit = 0; bit < 8; bit++) {
    this->delay_();
    this->release_scl_();
    if (!this->wait_scl_high_()) {
      return false;
    }
    this->delay_();
    data = static_cast<uint8_t>(data << 1) | (this->read_sda_() ? 1u : 0u);
    this->drive_scl_low_();
    this->delay_();
  }

  if (ack) {
    this->drive_sda_low_();
  } else {
    this->release_sda_();
  }
  this->delay_();
  this->release_scl_();
  if (!this->wait_scl_high_()) {
    return false;
  }
  this->delay_();
  this->drive_scl_low_();
  this->delay_();
  this->release_sda_();
  *out = data;
  return true;
}

bool SoftI2CBus::write_address_(uint8_t address, bool read) {
  return this->write_byte_(static_cast<uint8_t>((address << 1) | (read ? 1 : 0)));
}

/// Free a bus left busy by a slave that died mid transfer: a slave that was interrupted while
/// sending waits for clocks it will never get and holds SDA low forever. Clock SCL up to 9 times
/// (enough to finish any half-sent byte), then issue a STOP.
bool SoftI2CBus::recover_() {
  this->release_sda_();
  this->release_scl_();
  delayMicroseconds(this->half_period_us_);

  // Two diseases, two procedures. (a) SDA low: the slave was waiting for a clock it never got -
  // nine pulses on SCL until SDA frees up cure that. (b) SCL low with SDA high: the slave
  // itself holds the clock. A version that began with "if (SDA high) return true" cured only
  // a healthy bus and returned on (b); a version that waited for SCL BEFORE the pulses
  // waited forever and never got to the pulses. Here the pulses always run, and SCL is
  // raised with its own, large patience - and only the final tally says what was hanging.
  const bool scl_low_at_start = !this->read_scl_();
  const bool sda_low_at_start = !this->read_sda_();

  if (sda_low_at_start) {
    // Data line free, clock free. If we aborted a transaction earlier, the slave may
    // still be waiting for the ending - STOP tidies its state machine even when the lines are
    // quiet, so we send it always, not only when SDA is low.
    this->stop_();
    return true;
  }

  uint8_t pulses = 0;
  for (; pulses < 9; pulses++) {
    this->drive_scl_low_();
    this->delay_();
    this->release_scl_();
    // Here, and only here, patience is paid for: a latched slave can hold SCL for a long time,
    // and 10 ms cut the conversion off halfway and latched it even harder.
    if (!this->wait_scl_high_(this->stretch_timeout_us_)) {
      break;  // SCL cannot be raised - further pulses will achieve nothing
    }
    this->delay_();
    if (this->read_sda_()) {
      break;
    }
    this->drive_scl_low_();
    this->delay_();
  }

  this->stop_();
  this->release_sda_();
  this->release_scl_();
  const bool sda_free = this->read_sda_();
  const bool scl_free = this->wait_scl_high_(500);
  const bool released = sda_free && scl_free;
  if (released) {
    if (sda_low_at_start || scl_low_at_start) {
      this->recoveries_++;
      ESP_LOGW(TAG, "Bus recovered after %d SCL pulses (was: SDA=%s SCL=%s; total: %u)", pulses + 1,
               sda_low_at_start ? "low" : "high", scl_low_at_start ? "low" : "high",
               (unsigned) this->recoveries_);
    }
  } else {
    ESP_LOGW(TAG,
             "Bus still stuck after %d pulses: SDA=%d SCL=%d (bylo SDA=%s SCL=%s, scl_stuck=%d) - "
             "who holds the line; if it is the slave, only power-cycling it helps",
             pulses + 1, (int) sda_free, (int) scl_free, sda_low_at_start ? "low" : "high",
             scl_low_at_start ? "low" : "high", this->scl_stuck_);
  }
  return released;
}

std::vector<uint8_t> SoftI2CBus::i2c_scan_() {
  this->scan_results_.clear();
  for (uint8_t address = 0x08; address <= 0x77; address++) {
    if (this->write_readv(address, nullptr, 0, nullptr, 0) == i2c::ERROR_OK) {
      this->scan_results_.push_back(address);
    }
  }
  return this->scan_results_;
}

void SoftI2CBus::log_scan_results_() {
  if (this->scan_results_.empty()) {
    ESP_LOGW(TAG, "I2C scan: no devices acknowledged");
    return;
  }
  for (uint8_t address : this->scan_results_) {
    ESP_LOGI(TAG, "I2C scan: found device at address 0x%02X", address);
  }
}

i2c::ErrorCode SoftI2CBus::fail_(i2c::ErrorCode err, uint8_t address) {
  this->error_count_++;
  this->last_err_ = (int) err;
  this->last_err_addr_ = address;
  // Rate limited: a dead sensor polled every 5 s should not turn the log into a firehose, but a
  // one-second window keeps the failure visible right when it starts happening. The first failure
  // is never dropped - last_error_log_ms_ starts at 0, so a boot-time error inside the first
  // second would otherwise be swallowed, and a boot-time error is exactly the one people debug.
  const uint32_t now = millis();
  if (this->error_count_ == 1 || now - this->last_error_log_ms_ >= 1000) {
    this->last_error_log_ms_ = now;
    ESP_LOGW(TAG, "I2C transaction failed: error=%d addr=0x%02X sda=%d scl=%d (total failures: %u)", (int) err,
             address, this->sda_->digital_read(), this->scl_->digital_read(), (unsigned) this->error_count_);
  }
  return err;
}

// ---------------------------------------------------------------------------
// component lifecycle
// ---------------------------------------------------------------------------

void SoftI2CBus::setup() {
  ESP_LOGI(TAG, "soft_i2c v%s", SOFT_I2C_VERSION);
  if (this->sda_ == nullptr || this->scl_ == nullptr) {
    ESP_LOGE(TAG, "SDA and SCL pins must both be configured");
    return;
  }
#if defined(USE_BK72XX)
  // Must happen before the pins are configured: with the filter active, every gpio_config() and
  // gpio_output() on GPIO20..23 is dropped, so the pad never moves and nothing on the bus can work.
  gpio_ops_disable_filter();
  this->jtag_filter_off_ = true;
#endif
  // Sensor power BEFORE anything touches the bus. Done inside setup(), not via
  // `esphome.on_boot` + `output:`, because an on_boot with higher priority runs before other
  // components' setup() - then output.gpio takes the pad over only later and its own initialization
  // wipes what was set earlier. Here the order is guaranteed: HIGH -> delay -> lines.
  if (this->power_pin_ != nullptr) {
    this->power_pin_->setup();
    // Always HIGH, then the target state. The 0x0002 -> 0x0000 transition in the pad register proves
    // the pad is really ours; a bare read of 0x0000 would prove nothing (an untouched pad can
    // also look like zero).
#if defined(USE_BK72XX)
    this->power_pin_->digital_write(true);
    delayMicroseconds(20);
    this->power_cfg_high_ = bk_pin_cfg_((uint32_t) this->power_pin_num_);
#endif
    this->power_pin_->digital_write(!this->power_active_low_);
    delayMicroseconds(20);
#if defined(USE_BK72XX)
    this->power_cfg_low_ = bk_pin_cfg_((uint32_t) this->power_pin_num_);
#endif
    if (this->power_on_delay_ms_ > 0) {
      delay(this->power_on_delay_ms_);
    }
    this->power_applied_ = true;
  }

  this->sda_->setup();
  this->scl_->setup();
  // Idle state: both lines released, so the bus is high as soon as we are up.
  this->release_sda_();
  this->release_scl_();
  delayMicroseconds(10);
  this->initialized_ = true;

  // The pair sweep runs here, after the JTAG filter is dropped and after the supply is raised, and BEFORE
  // the line probe and the normal scan - so the normal path sees the restored pads.
  if (this->pair_sweep_) {
    this->run_pair_sweep_();
  }
  if (!this->slow_pairs_.empty()) {
    this->run_slow_probe_();
  }

  // One-shot line probe, logged unconditionally because it is the only evidence that separates
  // "no slave answered" from "the pad never reaches the line". Released lines reading high prove
  // the pull-up and the input path; driving a line low and reading it back proves the output path.
  // A line that stays high while driven is a pad that is not usable as GPIO (its mux is still
  // owned by another function), not a missing sensor.
  const bool sda_idle = this->read_sda_();
  const bool scl_idle = this->read_scl_();
#if defined(USE_BK72XX)
  // The GPIO numbers go into diagnostics: a zero cfg may come from the pad OR from
  // dump_summary() parsing not returning a number. Without this pair of numbers the two cannot be told apart.
  const uint32_t sda_num = bk_pin_number_(this->sda_);
  const uint32_t scl_num = bk_pin_number_(this->scl_);
  this->probe_pin_sda_ = sda_num == BK_PIN_UNKNOWN ? -1 : (int32_t) sda_num;
  this->probe_pin_scl_ = scl_num == BK_PIN_UNKNOWN ? -1 : (int32_t) scl_num;
  // Idle state: pad as input with pull-up. Non-zero = gpio_config() reached here.
  if (sda_num != BK_PIN_UNKNOWN)
    this->probe_cfg_sda_idle_ = bk_pin_cfg_(sda_num);
  if (scl_num != BK_PIN_UNKNOWN)
    this->probe_cfg_scl_idle_ = bk_pin_cfg_(scl_num);
  // Instrument calibration, two controls with known values upfront:
  //  GPIO0 = UART2_TX, the logger configures it -> MUST be non-zero, otherwise the read is faulty;
  //  GPIO6 = unused on this board -> should be zero (baseline).
  this->probe_cfg_ctl_ = bk_pin_cfg_(0u);
  this->probe_cfg_ref_ = bk_pin_cfg_(6u);
#endif
  this->drive_scl_low_();
  this->delay_();
#if defined(USE_BK72XX)
  this->probe_cfg_scl_drv_ = scl_num != BK_PIN_UNKNOWN ? bk_pin_cfg_(scl_num) : 0u;
#endif
  const bool scl_driven_low = !this->read_scl_();
  this->release_scl_();
  this->drive_sda_low_();
  this->delay_();
#if defined(USE_BK72XX)
  this->probe_cfg_sda_drv_ = sda_num != BK_PIN_UNKNOWN ? bk_pin_cfg_(sda_num) : 0u;
#endif
  const bool sda_driven_low = !this->read_sda_();
  this->release_sda_();

  this->probe_ran_ = true;
  this->probe_idle_sda_ = (int) sda_idle;
  this->probe_idle_scl_ = (int) scl_idle;
  this->probe_drv_sda_ = (int) sda_driven_low;
  this->probe_drv_scl_ = (int) scl_driven_low;

  // Short-circuit test (cross-read). The probe above does not prove the outgoing path on BK7231: a read of
  // a pad that is an output returns the data register (latch), so "driven=0" can lie in either
  // direction. Here we drive one line low and read THE OTHER ONE, working as an input -
  // such a read cannot lie. With a SDA<->SCL jumper fitted, a correct pad MUST give 0 on the other
  // side; a CHT83xx sensor holds both lines high, so 0 cannot come from it.
  this->drive_sda_low_();
  this->delay_();
  this->probe_loop_scl_ = this->read_scl_() ? 0 : 1;
  this->release_sda_();
  this->release_scl_();
  this->delay_();
  this->drive_scl_low_();
  this->delay_();
  this->probe_loop_sda_ = this->read_sda_() ? 0 : 1;
  this->release_scl_();
  this->release_sda_();
  ESP_LOGI(TAG, "Line probe: idle SDA=%d SCL=%d, driven-low SDA=%d SCL=%d, scan %s", (int) sda_idle,
           (int) scl_idle, (int) sda_driven_low, (int) scl_driven_low, this->scan_ ? "enabled" : "disabled");

  if (!this->recover_()) {
    ESP_LOGE(TAG, "Bus is not usable: SDA/SCL will not both go high. Check pull-ups "
                  "(4.7k-10k recommended, or enable internal_pullup) and wiring.");
  }

  if (this->scan_) {
    this->i2c_scan_();
    this->log_scan_results_();
    this->scan_ran_ = true;
    this->scan_found_ = (int) this->scan_results_.size();
    if (!this->scan_results_.empty()) {
      this->scan_first_ = this->scan_results_.front();
    }
  }

  // Diagnostics AFTER the scan, not before. A build with census + probe before the scan ended with found=0,
  // while the same hardware without them gave found=1 first=0x44: diagnostics can latch
  // the bus (a measurement command -> the slave holds SCL), and the latch eats the later scan. The main
  // path gets the bus first and in the state it found it in.
  if (!this->census_pins_.empty()) {
    this->run_pad_census_();
  }
  if (this->protocol_probe_) {
    this->run_protocol_probe_();
  }
  this->setup_ran_ = true;
}

void SoftI2CBus::loop() {
  if (!this->bus_recovery_ || !this->initialized_) {
    return;
  }
  // Sampling the idle bus every second is free compared to what a stuck bus costs.
  if (millis() - this->last_check_ms_ < 1000) {
    return;
  }
  this->last_check_ms_ = millis();

  if (this->read_scl_() && !this->read_sda_()) {
    // SCL high + SDA low while idle == a slave is mid-transfer (it lost power / was reset).
    if (!this->recover_()) {
      if (!this->stuck_reported_) {
        this->stuck_reported_ = true;
        ESP_LOGE(TAG, "Bus is stuck low and cannot be released - check the supply of the slave devices");
      }
    } else {
      this->stuck_reported_ = false;
    }
  }
}

// ---------------------------------------------------------------------------
// the I2CBus interface
// ---------------------------------------------------------------------------
i2c::ErrorCode SoftI2CBus::write_readv(uint8_t address, const uint8_t *write_buffer, size_t write_count,
                                       uint8_t *read_buffer, size_t read_count) {
  if (!this->initialized_) {
    return i2c::ERROR_NOT_INITIALIZED;
  }

  // Address-only probe, used by the bus scanner
  if (write_count == 0 && read_count == 0) {
    if (!this->start_()) {
      return i2c::ERROR_TIMEOUT;
    }
    const bool ack = this->write_address_(address, false);
    this->stop_();
    return ack ? i2c::ERROR_OK : i2c::ERROR_NOT_ACKNOWLEDGED;
  }

  // PURE READ. i2c::I2CBus::read() delegates here with write_count == 0
  // (i2c_bus.h: `return this->write_readv(address, nullptr, 0, buffer, len);`). The address word must then
  // go with the READ bit and no data at all. An earlier version of this function lacked this
  // path: it sent the address as WRITE and only then loaded the bytes. A word in receive mode
  // does not ask the slave for data, so SDA was never taken over - and the transaction ended with
  // ERROR_OK, because there was neither a NACK nor a timeout. Effect: the scan sees the device and ACKs,
  // writes go through, and every ESPHome read returns garbage that does not pass CRC. Exactly
  // what sht3xd was doing: "Retrying to reconnect the sensor" with found=1 first=0x44.
  if (read_count > 0 && write_count == 0) {
    if (!this->start_()) {
      this->recover_();
      return this->fail_(i2c::ERROR_TIMEOUT, address);
    }
    if (!this->write_address_(address, true)) {
      this->stop_();
      // A NACK with latched SCL is a normal symptom, not a diagnosis: a slave mid-transmission does not
      // accept a new address. A bare STOP will not help (STOP needs a raised SCL), so
      // we try pulses before reporting the error.
      this->recover_();
      return this->fail_(i2c::ERROR_NOT_ACKNOWLEDGED, address);
    }
    for (size_t i = 0; i < read_count; i++) {
      if (!this->read_byte_(&read_buffer[i], i + 1 < read_count)) {
        this->stop_();
        return this->fail_(i2c::ERROR_TIMEOUT, address);
      }
    }
    this->stop_();
    return i2c::ERROR_OK;
  }

  if (!this->start_()) {
    this->recover_();
    return this->fail_(i2c::ERROR_TIMEOUT, address);
  }

  if (!this->write_address_(address, false)) {
    this->stop_();
      // A NACK with latched SCL is a normal symptom, not a diagnosis: a slave mid-transmission does not
      // accept a new address. A bare STOP will not help (STOP needs a raised SCL), so
      // we try pulses before reporting the error.
      this->recover_();
    return this->fail_(i2c::ERROR_NOT_ACKNOWLEDGED, address);
  }

  for (size_t i = 0; i < write_count; i++) {
    if (!this->write_byte_(write_buffer[i])) {
      this->stop_();
      // A NACK with latched SCL is a normal symptom, not a diagnosis: a slave mid-transmission does not
      // accept a new address. A bare STOP will not help (STOP needs a raised SCL), so
      // we try pulses before reporting the error.
      this->recover_();
      return this->fail_(i2c::ERROR_NOT_ACKNOWLEDGED, address);
    }
  }

  if (read_count > 0) {
    // A read right after a write needs a repeated start, not a stop+start pair.
    if (!this->restart_()) {
      this->recover_();
      return this->fail_(i2c::ERROR_TIMEOUT, address);
    }
    if (!this->write_address_(address, true)) {
      this->stop_();
      // A NACK with latched SCL is a normal symptom, not a diagnosis: a slave mid-transmission does not
      // accept a new address. A bare STOP will not help (STOP needs a raised SCL), so
      // we try pulses before reporting the error.
      this->recover_();
      return this->fail_(i2c::ERROR_NOT_ACKNOWLEDGED, address);
    }
    for (size_t i = 0; i < read_count; i++) {
      if (!this->read_byte_(&read_buffer[i], i + 1 < read_count)) {
        this->stop_();
        return this->fail_(i2c::ERROR_TIMEOUT, address);
      }
    }
  }

  this->stop_();
  return i2c::ERROR_OK;
}

}  // namespace soft_i2c
}  // namespace esphome
