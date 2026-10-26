"""Driver for the Sensylink CHT8305 / CHT8310 temperature+humidity sensors.

These sensors look like SHT3x parts on the pin header but they are *not* protocol compatible:
they use a plain 8-bit register pointer with **no CRC**, while ``sht3xd`` speaks Sensirion
command words with CRC-8. Pointing ``sht3xd`` at a CHT83xx is why ``setup()`` dies on
``HEATER_DISABLE`` - the command word does not exist on this chip.

The variant is auto-detected from the version register: 0x8215 / 0x8315 are the CHT831x family,
which reports 13-bit temperature and needs humidity from its own register (the shared read window
carries a parity bit there).

    sensor:
      - platform: cht83xx
        i2c_id: bus_a
        address: 0x40          # AD0 to GND
        update_interval: 60s
        temperature:
          name: "Temperature"
        humidity:
          name: "Humidity"

Works on any ``i2c_id``, including the ``soft_i2c`` bit-bang bus, which is what BK7231/LibreTiny
boards need (esphome#5289).

Address is set by the AD0 pin: GND 0x40, VCC 0x41, SDA 0x42, SCL 0x43.
"""
