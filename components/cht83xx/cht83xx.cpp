#include "cht83xx.h"
#include "esphome/core/log.h"

namespace esphome::cht83xx {

static const char *const TAG = "cht83xx";

// Register map of the Sensylink CHT83xx family (8-bit register pointer, 16-bit big-endian value).
static const uint8_t REG_TEMPERATURE = 0x00;
static const uint8_t REG_HUMIDITY = 0x01;
static const uint8_t REG_ONESHOT = 0x0F;   ///< CHT831x: trigger a single conversion
static const uint8_t REG_ID = 0xFE;        ///< manufacturer ID, expect 0x5959
static const uint8_t REG_VERSION = 0xFF;   ///< version ID; 0x8215 / 0x8315 mean CHT831x

// Conversions, taken from the working OpenBeken driver (drv_cht8305.c) and the datasheet.
static const float CHT8305_TEMP_SCALE = 165.0f / 65535.0f;
static const float CHT8305_TEMP_OFFSET = -40.0f;
static const float CHT8305_HUM_SCALE = 100.0f / 65535.0f;
static const float CHT831X_TEMP_STEP = 0.03125f;   ///< 13-bit result, 8 LSBs are padding
static const uint32_t CONVERSION_MS = 20;

void CHT83XXComponent::setup() {
  uint8_t id[2];
  if (!this->read_bytes(REG_ID, id, 2)) {
    // LOG_STR keeps the reason in flash and surfaces it as the "[E][component] ... is marked
    // FAILED" line, which is the one setup-phase message that does reach the log view.
    this->mark_failed(LOG_STR("no CHT83xx answering at this address - check pins, pull-ups and AD0"));
    return;
  }
  this->manufacturer_id_ = (uint16_t(id[0]) << 8) | id[1];

  uint8_t ver[2];
  if (this->read_bytes(REG_VERSION, ver, 2)) {
    this->version_id_ = (uint16_t(ver[0]) << 8) | ver[1];
  }
  this->is_831x_ = (this->version_id_ == 0x8215 || this->version_id_ == 0x8315);
}

void CHT83XXComponent::dump_config() {
  // Everything that matters is on this one line: it is the only log output that reliably reaches
  // the log view on this platform, so it carries the detected model rather than just the address.
  ESP_LOGCONFIG(TAG,
                "CHT83xx: manufacturer=0x%04X version=0x%04X model=%s", this->manufacturer_id_,
                this->version_id_, this->is_831x_ ? "CHT831x" : "CHT8305");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
  LOG_SENSOR("  ", "Humidity", this->humidity_sensor_);
}

void CHT83XXComponent::update() {
  if (this->is_831x_) {
    // The CHT831x needs to be told to convert; the temperature also has to come from its own
    // register, because reading past 0x00 lands on the ALERT parity bit instead of humidity.
    const uint8_t trigger[2] = {0x00, 0x00};
    if (!this->write_bytes(REG_ONESHOT, trigger, 2)) {
      this->status_set_warning();
      return;
    }
  }

  this->set_timeout(CONVERSION_MS, [this]() {
    uint8_t raw[2];

    if (!this->read_bytes(REG_TEMPERATURE, raw, 2)) {
      ESP_LOGW(TAG, "Temperature read failed");
      this->status_set_warning();
      return;
    }
    const uint16_t raw_t = (uint16_t(raw[0]) << 8) | raw[1];
    const float temperature = this->is_831x_ ? float(raw_t >> 3) * CHT831X_TEMP_STEP
                                             : float(raw_t) * CHT8305_TEMP_SCALE + CHT8305_TEMP_OFFSET;

    if (!this->read_bytes(REG_HUMIDITY, raw, 2)) {
      ESP_LOGW(TAG, "Humidity read failed");
      this->status_set_warning();
      return;
    }
    const uint16_t raw_h = (uint16_t(raw[0]) << 8) | raw[1];
    const float humidity =
        this->is_831x_ ? float(raw_h & 0x7FFF) / 32768.0f * 100.0f : float(raw_h) * CHT8305_HUM_SCALE;

    ESP_LOGD(TAG, "Got temperature=%.2f°C humidity=%.2f%% (raw T=0x%04X H=0x%04X)", temperature, humidity, raw_t,
             raw_h);
    if (this->temperature_sensor_ != nullptr)
      this->temperature_sensor_->publish_state(temperature);
    if (this->humidity_sensor_ != nullptr)
      this->humidity_sensor_->publish_state(humidity);
    this->status_clear_warning();
  });
}

}  // namespace esphome::cht83xx
