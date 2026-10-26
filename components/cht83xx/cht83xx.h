#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome::cht83xx {

/// Support for the Sensylink CHT8305 / CHT8310 temperature+humidity sensors.
///
/// Register-pointer protocol (8-bit register, 16-bit big-endian values, no CRC), which is why the
/// ``sht3xd`` driver cannot be reused: that one sends Sensirion command words with CRC-8 and dies
/// on a heater command this chip does not implement.
class CHT83XXComponent final : public PollingComponent, public i2c::I2CDevice {
 public:
  void set_temperature_sensor(sensor::Sensor *temperature_sensor) { temperature_sensor_ = temperature_sensor; }
  void set_humidity_sensor(sensor::Sensor *humidity_sensor) { humidity_sensor_ = humidity_sensor; }

  void setup() override;
  void dump_config() override;
  void update() override;

 protected:
  sensor::Sensor *temperature_sensor_{nullptr};
  sensor::Sensor *humidity_sensor_{nullptr};
  uint16_t manufacturer_id_{0};
  uint16_t version_id_{0};
  bool is_831x_{false};
};

}  // namespace esphome::cht83xx
