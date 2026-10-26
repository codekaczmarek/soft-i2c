#include "soft_i2c.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

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
                "driven=%d/%d | scan=%d found=%d first=0x%02X | errors=%u last_err=%d@0x%02X jtagfilter=%d",
                SOFT_I2C_VERSION, sda_sum, scl_sum, (unsigned) this->frequency_,
                this->internal_pullup_ ? "internal" : "external", (unsigned) this->timeout_us_,
                (int) this->setup_ran_, this->probe_idle_sda_, this->probe_idle_scl_, this->probe_drv_sda_,
                this->probe_drv_scl_, (int) this->scan_ran_, this->scan_found_, (unsigned) this->scan_first_,
                (unsigned) this->error_count_, this->last_err_, (unsigned) this->last_err_addr_,
                (int) this->jtag_filter_off_);
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
  // Clear the output latch *before* claiming the pin, otherwise the transition to output
  // mode would momentarily drive the line high while a slave is ACKing.
  this->sda_->digital_write(false);
  this->sda_->pin_mode(gpio::FLAG_OUTPUT);
}

void SoftI2CBus::release_scl_() { this->scl_->pin_mode(this->input_flags_()); }

void SoftI2CBus::drive_scl_low_() {
  this->scl_->digital_write(false);
  this->scl_->pin_mode(gpio::FLAG_OUTPUT);
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
// low level protocol
// ---------------------------------------------------------------------------

/// Wait until SCL is actually high. A clock-stretching slave holds it low, a missing pull-up
/// keeps it low forever — hence the timeout.
bool SoftI2CBus::wait_scl_high_() {
  const uint32_t start = micros();
  this->scl_->pin_mode(this->input_flags_());
  while (!this->scl_->digital_read()) {
    if (micros() - start > this->timeout_us_) {
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
/// set SDA high, raise SCL, then drop SDA — that falling edge while SCL is high is the Sr
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
  if (this->read_sda_()) {
    return true;
  }

  uint8_t pulses = 0;
  for (; pulses < 9; pulses++) {
    this->drive_scl_low_();
    this->delay_();
    this->release_scl_();
    if (!this->wait_scl_high_()) {
      return false;  // SCL itself is stuck: wiring or pull-up problem
    }
    this->delay_();
    if (this->read_sda_()) {
      break;
    }
    this->drive_scl_low_();
    this->delay_();
  }

  this->stop_();
  const bool released = this->read_sda_();
  if (released) {
    this->recoveries_++;
    ESP_LOGW(TAG, "Bus was stuck low, recovered after %d SCL pulses (total: %u)", pulses + 1,
             (unsigned) this->recoveries_);
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
  this->sda_->setup();
  this->scl_->setup();
  // Idle state: both lines released, so the bus is high as soon as we are up.
  this->release_sda_();
  this->release_scl_();
  delayMicroseconds(10);
  this->initialized_ = true;

  // One-shot line probe, logged unconditionally because it is the only evidence that separates
  // "no slave answered" from "the pad never reaches the line". Released lines reading high prove
  // the pull-up and the input path; driving a line low and reading it back proves the output path.
  // A line that stays high while driven is a pad that is not usable as GPIO (its mux is still
  // owned by another function), not a missing sensor.
  const bool sda_idle = this->read_sda_();
  const bool scl_idle = this->read_scl_();
  this->drive_scl_low_();
  this->delay_();
  const bool scl_driven_low = !this->read_scl_();
  this->release_scl_();
  this->drive_sda_low_();
  this->delay_();
  const bool sda_driven_low = !this->read_sda_();
  this->release_sda_();

  this->probe_ran_ = true;
  this->probe_idle_sda_ = (int) sda_idle;
  this->probe_idle_scl_ = (int) scl_idle;
  this->probe_drv_sda_ = (int) sda_driven_low;
  this->probe_drv_scl_ = (int) scl_driven_low;
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

  if (!this->start_()) {
    this->recover_();
    return this->fail_(i2c::ERROR_TIMEOUT, address);
  }

  if (!this->write_address_(address, false)) {
    this->stop_();
    return this->fail_(i2c::ERROR_NOT_ACKNOWLEDGED, address);
  }

  for (size_t i = 0; i < write_count; i++) {
    if (!this->write_byte_(write_buffer[i])) {
      this->stop_();
      return this->fail_(i2c::ERROR_NOT_ACKNOWLEDGED, address);
    }
  }

  if (read_count > 0) {
    // A read right after a write needs a repeated start, not a stop+start pair.
    if (write_count > 0) {
      if (!this->restart_()) {
        this->recover_();
        return this->fail_(i2c::ERROR_TIMEOUT, address);
      }
      if (!this->write_address_(address, true)) {
        this->stop_();
        return this->fail_(i2c::ERROR_NOT_ACKNOWLEDGED, address);
      }
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
