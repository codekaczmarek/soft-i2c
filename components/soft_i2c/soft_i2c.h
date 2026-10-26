#pragma once

#include <vector>

// Banner wersji: setup() loguje go JAKO PIERWSZA linia. Jego obecnosc w logu dowodzi, ze
// ta wersja komponentu trafila do builda (a nie stary clone z cache), a jego brak mowi,
// ze setup() w ogole nie zostal wywolany.
#define SOFT_I2C_VERSION "1.2.6"


#include "esphome/components/i2c/i2c_bus.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"

namespace esphome {
namespace soft_i2c {

/// Software (bit-banged) I2C master.
///
/// Deliberately uses only the portable `esphome::GPIOPin` interface, so it works on every
/// platform — it exists mainly for LibreTiny boards (bk72xx / ln882x / rtl87xx), where
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

  // Diagnostyka jest zgłaszana z dump_config(): to jedyne wywołanie logu, którego output na
  // pewno dociera do podglądu logów. Linii z setup() nie sposób było zobaczyć w czterech
  // kolejnych buildach, więc żadnemu logowi z setup() nie ufażemy - stan jest zapamiętywany
  // tutaj i drukowany razem z numerem wersji w tej samej linii.
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
  bool wait_scl_high_();
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

  GPIOPin *sda_{nullptr};
  GPIOPin *scl_{nullptr};
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
  /// Ustawione, jesli setup() zdjal filtr JTAG SDK-u (patrz komentarz przy gpio_ops_disable_filter
  /// w .cpp). Widoczne w linii dump_config(), zeby dalo sie odrozyc "filtr zdjety" od "pin nie
  /// reaguje z innego powodu".
  bool jtag_filter_off_{false};
  /// Test zwory (cross-read): ile z linii "drugiej" spadly, gdy "bliska" byla sterowana w dol.
  /// W przeciwienstwie do probe_drv_* odczyt dotyczy padu pracujacego jako WEJSCIE, wiec nie jest
  /// obciazony problemem latcha (BK7231 na wyjsciu zwraca rejestr danych, nie stan padu).
  /// Wartosc 1/1 ma sens tylko przy zalozonej zworze miedzy SDA i SCL.
  int probe_loop_sda_{0};
  int probe_loop_scl_{0};
  std::vector<uint8_t> scan_results_;
};

}  // namespace soft_i2c
}  // namespace esphome
