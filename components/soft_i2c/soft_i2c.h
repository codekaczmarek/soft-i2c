#pragma once

#include <vector>

// Version banner: setup() logs it as the FIRST line. Its presence in the log proves that
// this version of the component made it into the build (and not an old clone from cache), while
// its absence says that setup() was never called at all.
// The release version = the VERSION file in the main directory (single source of truth). The CI job
// "Version consistency" fails if any of them drift apart - or if the v* tag does not equal
// VERSION. Do not bump this number in isolation.
#define SOFT_I2C_VERSION "1.4.0"


#include "esphome/components/i2c/i2c_bus.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"

namespace esphome {
namespace soft_i2c {

/// Software (bit-banged) I2C master.
///
/// Deliberately uses only the portable `esphome::GPIOPin` interface, so it works on every
/// platform - it exists mainly for LibreTiny boards (bk72xx / ln882x / rtl87xx), where
/// ESPHome has no I2C support at all (esphome#5289): the `i2c:` component rejects any pin
/// because the LibreTiny GPIO layer does not implement the bidirectional pin mode I2C needs.
///
/// Lines are driven "open drain style" in software: a low is driven with a push-pull output
/// (the output latch is pre-cleared to 0 before the pin is taken over, so there is never a
/// glitch-high), a high is always *released* to an input with a pull-up. The master therefore
/// never fights a slave driving the line low. Clock stretching is honoured (SCL is sampled
/// before every clock pulse) and every wait is bounded by a timeout.
class SoftI2CBus final : public i2c::I2CBus, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  /// The single pure virtual of i2c::I2CBus: performs a combined write+read transaction.
  i2c::ErrorCode write_readv(uint8_t address, const uint8_t *write_buffer, size_t write_count,
                             uint8_t *read_buffer, size_t read_count) override;

  void set_sda_pin(GPIOPin *sda) { this->sda_ = sda; }
  void set_scl_pin(GPIOPin *scl) { this->scl_ = scl; }
  void set_frequency(uint32_t frequency);
  void set_scan(bool scan) { this->scan_ = scan; }
  void set_internal_pullup(bool internal_pullup) { this->internal_pullup_ = internal_pullup; }
  void set_timeout_us(uint32_t timeout_us) { this->timeout_us_ = timeout_us; }
  void set_bus_recovery(bool bus_recovery) { this->bus_recovery_ = bus_recovery; }

  /// Sensor power pin (e.g. P8 = "battery relay" on the Tuya CBU). Raised in setup() BEFORE the
  /// line configuration and the scan, so the order is deterministic within a single
  /// component - unlike `esphome.on_boot` + `output:`, where the actuator initialises the pad
  /// only in its own setup(), i.e. AFTER the higher-priority component has already run.
  void set_power_pin(GPIOPin *power_pin, int8_t pin_number) {
    this->power_pin_ = power_pin;
    this->power_pin_num_ = pin_number;
  }
  void set_power_on_delay_ms(uint32_t delay_ms) { this->power_on_delay_ms_ = delay_ms; }

  /// Sweep of SDA/SCL pairs over the whole pin list. Answers the question of which pads the sensor
  /// actually sits on - instead of guessing a pair from an article. The pair is reported in dump_config().
  void set_pair_sweep(bool pair_sweep) { this->pair_sweep_ = pair_sweep; }
  void set_sweep_pins(std::vector<uint8_t> sweep_pins) { this->sweep_pins_ = std::move(sweep_pins); }
  void set_sweep_budget_ms(uint32_t budget_ms) { this->sweep_budget_ms_ = budget_ms; }

  /// Frequency ladder on selected pairs. Answers "our master is too fast / the pull-ups are too
  /// weak for the slave to sample SDA in time" - after 8064 probes with zero ACKs this is just as
  /// likely as the device being absent.
  void set_slow_probe_pairs(std::vector<uint8_t> pairs) { this->slow_pairs_ = std::move(pairs); }
  void set_slow_probe_addresses(std::vector<uint8_t> addrs) { this->slow_addrs_ = std::move(addrs); }
  void set_slow_probe_frequencies(std::vector<uint32_t> freqs) { this->slow_freqs_ = std::move(freqs); }
  void set_protocol_probe(bool protocol_probe) { this->protocol_probe_ = protocol_probe; }
  void set_protocol_probe_addresses(std::vector<uint8_t> addrs) {
    this->protocol_probe_addrs_ = std::move(addrs);
  }
  void set_census_pins(std::vector<uint8_t> census_pins) { this->census_pins_ = std::move(census_pins); }
  /// How long we wait for the slave to raise SCL on its own after clock stretching. This is NOT the
  /// same as `timeout`: an SHT3x conversion in high-repeatability mode takes ~8.5 ms typically, and
  /// more at reduced supply. A limit on the order of 10 ms cuts it in half and latches the bus.
  void set_stretch_timeout_us(uint32_t stretch_timeout_us) { this->stretch_timeout_us_ = stretch_timeout_us; }

  /// The way of "pulling the line down". LibreTiny on Beken does not drive LOW through gpio_output():
  /// digitalWrite() before the pad is taken over into OUTPUT mode emulates the state with resistors
  /// (wiring_private.h: pinSetOutputPull -> pinMode(INPUT_PULLDOWN ^ !!status)) and returns SKIPPING
  /// gpio_output().
  ///   output   - canonical Arduino order: OUTPUT first, then digitalWrite(false)
  ///   pulldown - giving up OUTPUT mode entirely: LOW = INPUT_PULLDOWN, HIGH = INPUT_PULLUP
  /// The wrong order (digitalWrite before pin_mode) ends with REG_WRITE(0x00) from gpio_config(),
  /// which erases the pull-down - the line floats instead of going down, and the read still shows
  /// zero because GMODE_OUTPUT disables the input path. That is exactly what we had been doing
  /// from the start.
  void set_drive_mode(uint8_t drive_mode) { this->drive_mode_ = drive_mode; }

  /// Active-low power switch (PMOS). The actuator does HIGH->LOW anyway, so the 0x0002 -> 0x0000
  /// transition in the register proves the pad is OURS - the LOW state alone would prove nothing.
  void set_power_active_low(bool active_low) { this->power_active_low_ = active_low; }

  /// List of pads whose cfg register is read in dump_config(). Serves to prove that the actuator
  /// really moved the pad: on BK7231 gpio_output() sets bit 1 in the very word that gpio_config()
  /// reads, so driven HIGH = 0x0002, driven LOW = 0x0000, nothing touched = some other value.
  /// Without this read there is no way to tell "the power-on does not work" from "the power-on was
  /// never performed" - and those are two quite different conclusions.
  void set_diag_pins(std::vector<uint8_t> diag_pins) { this->diag_pins_ = std::move(diag_pins); }

  // Diagnostics is reported from dump_config(): it is the only log call whose output reliably
  // reaches the log view. Lines from setup() were impossible to see in four consecutive builds,
  // so no log from setup() is trusted - the state is remembered here and printed together with
  // the version number on the same line.
  bool setup_ran_{false};
  bool probe_ran_{false};
  int probe_idle_sda_{-1}, probe_idle_scl_{-1}, probe_drv_sda_{-1}, probe_drv_scl_{-1};
  bool scan_ran_{false};
  int scan_found_{-1};
  uint8_t scan_first_{0};

 protected:
  // --- pin mode helpers -------------------------------------------------
  gpio::Flags input_flags_() const;
  void delay_() const;
  void release_sda_();
  void drive_sda_low_();
  void release_scl_();
  void drive_scl_low_();
  bool read_sda_();
  bool read_scl_();

  // --- protocol helpers -------------------------------------------------
  /// Wait for SCL to come up. `limit_us` is deliberately a parameter, not a field: an ordinary bit
  /// clock needs a dozen-ish microseconds of patience, while unblocking a latched slave needs
  /// hundreds of milliseconds. Using a single limit for both cases gave either latches (10 ms per
  /// conversion) or a boot lasting half a minute (120 ms on every clock edge).
  bool wait_scl_high_(uint32_t limit_us);
  bool wait_scl_high_() { return this->wait_scl_high_(this->timeout_us_); }
  bool start_();
  bool restart_();
  void stop_();
  bool write_byte_(uint8_t data);
  bool read_byte_(uint8_t *out, bool ack);
  bool write_address_(uint8_t address, bool read);
  bool recover_();

  /** Log a failing transaction (at most once per second) and return the error code */
  i2c::ErrorCode fail_(i2c::ErrorCode err, uint8_t address);

  std::vector<uint8_t> i2c_scan_();
  void log_scan_results_();
  /// Pair sweep: for every ordered pair of candidates, probes addresses 0x08..0x77.
  void run_pair_sweep_();

  GPIOPin *sda_{nullptr};
  GPIOPin *scl_{nullptr};
  GPIOPin *power_pin_{nullptr};
  int8_t power_pin_num_{-1};
  uint32_t power_on_delay_ms_{0};
  bool power_applied_{false};
  std::vector<uint8_t> diag_pins_;
  bool pair_sweep_{false};
  std::vector<uint8_t> sweep_pins_;
  uint32_t sweep_budget_ms_{5000};
  int sweep_tried_{0};
  int sweep_hits_{0};
  int sweep_budget_hit_{0};
  int sweep_filtered_out_{0};
  uint8_t sweep_sda_{0};
  uint8_t sweep_scl_{0};
  uint8_t sweep_addr_{0};
  uint32_t sweep_ms_{0};
  uint8_t drive_mode_{0};
  bool protocol_probe_{false};
  std::vector<uint8_t> protocol_probe_addrs_;
  std::vector<uint8_t> census_pins_;
  int census_tied_low_{0};
  int census_tied_high_{0};
  int census_floating_{0};
  uint32_t stretch_timeout_us_{120000};
  int scl_stuck_{0};
  bool power_active_low_{false};
  uint32_t power_cfg_high_{0xFFFFFFFFu};
  uint32_t power_cfg_low_{0xFFFFFFFFu};
  std::vector<uint8_t> slow_pairs_;
  std::vector<uint8_t> slow_addrs_;
  std::vector<uint32_t> slow_freqs_;
  int slow_tried_{0};
  int slow_hits_{0};
  uint8_t slow_hit_sda_{0};
  uint8_t slow_hit_scl_{0};
  uint8_t slow_hit_addr_{0};
  uint32_t slow_hit_freq_{0};
  uint32_t slow_min_freq_{0};
  void run_slow_probe_();
  /// Raw reads of three T&H protocols under the same addresses, with bytes and CRC in the log.
  void run_protocol_probe_();
  /// Electrical census of the pads: read with pull-up and with pull-down -> ground / VDD / floating.
  void run_pad_census_();
  uint32_t frequency_{100000};
  uint32_t half_period_us_{5};
  uint32_t timeout_us_{10000};
  bool internal_pullup_{true};
  bool bus_recovery_{true};
  bool scan_{false};
  bool initialized_{false};
  bool stuck_reported_{false};
  uint32_t last_check_ms_{0};
  uint32_t last_error_log_ms_{0};
  uint32_t recoveries_{0};
  uint32_t error_count_{0};
  /// Last failure, recorded so dump_config() can show *what* failed. Needed because ESP_LOGW from
  /// fail_() during setup() does not always reach the log view on this platform, while the
  /// dump_config() line always does.
  int last_err_{0};
  uint8_t last_err_addr_{0};
  /// Set if setup() removed the SDK's JTAG pad filter (see the comment at gpio_ops_disable_filter
  /// in the .cpp). Visible in the dump_config() line so that "filter removed" can be told apart
  /// from "pin not reacting for some other reason".
  bool jtag_filter_off_{false};
  /// Short-circuit test (cross-read): how many reads of the "other" line went low while the
  /// "near" one was driven low. Unlike probe_drv_*, the read concerns a pad working as an INPUT,
  /// so it is not burdened by the latch problem (BK7231 on output returns the data register, not
  /// the pad state). A value of 1/1 only makes sense with a short assumed between SDA and SCL.
  int probe_loop_sda_{0};
  int probe_loop_scl_{0};
  /// Contents of the pad configuration register (REG_GPIO_n_CONFIG) measured while the pin was
  /// driven low. It decides: OUTPUT/OUTPUT_ENABLE bits set and the line still high => the SDK
  /// write landed, but the pad belongs to another block (JTAG/flash). Bits 0 => the write never
  /// landed at all.
  uint32_t probe_cfg_sda_drv_{0};
  uint32_t probe_cfg_scl_drv_{0};
  /// The same read in the idle state (both lines as input with pull-up). Distinguishes "pad never
  /// configured" (both zero) from "outgoing write is blocked" (idle nonzero, drv zero).
  uint32_t probe_cfg_sda_idle_{0};
  uint32_t probe_cfg_scl_idle_{0};
  /// INSTRUMENTATION CONTROL. cfg[GPIO0] = the UART2_TX pad configured by the logger itself, so it
  /// MUST be nonzero if the read works. cfg[GPIO6] = an unused pad, it should be zero. Zero on
  /// GPIO0 means the register read itself is faulty, and then cfgdrv means nothing.
  uint32_t probe_cfg_ctl_{0};
  uint32_t probe_cfg_ref_{0};
  /// GPIO number actually used for the register read (-1 = parsing dump_summary() did not yield a
  /// number; then cfgdrv is zero by definition and nothing may be inferred from it).
  int32_t probe_pin_sda_{-1};
  int32_t probe_pin_scl_{-1};
  std::vector<uint8_t> scan_results_;
};

}  // namespace soft_i2c
}  // namespace esphome
