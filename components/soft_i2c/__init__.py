"""Software (bit-banged) I2C bus.

Drop-in replacement for ``i2c:`` on platforms where ESPHome has no I2C at all — most notably
LibreTiny boards (bk72xx / ln882x / rtl87xx, e.g. the Tuya CBU / BK7231 modules), where the
``i2c:`` component rejects every pin with *"This pin mode is not supported"* because the
LibreTiny GPIO layer does not implement the bidirectional pin mode that I2C requires
(esphome#5289).

The bus is a real ``esphome::i2c::I2CBus`` subclass, so **stock components keep working**:
anything that takes ``i2c_id:`` (sht3xd, bme280, aht10, bh1750, ...) can be pointed at it.

    external_components:
      - type: local
        path: ./esphome-components

    soft_i2c:
      - id: bus_sht
        sda: P21
        scl: P20
        scan: true

    sensor:
      - platform: sht3xd
        i2c_id: bus_sht

Bit-banging costs CPU: keep the frequency at 100 kHz or below and prefer a hardware I2C
master where one exists.
"""

import logging

from esphome import pins
from esphome.components import i2c
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_FREQUENCY,
    CONF_ID,
    CONF_INVERTED,
    CONF_SCAN,
    CONF_SCL,
    CONF_SDA,
    CONF_TIMEOUT,
)
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)

CONF_INTERNAL_PULLUP = "internal_pullup"
CONF_BUS_RECOVERY = "bus_recovery"

DEPENDENCIES = ["i2c"]
MULTI_CONF = True

soft_i2c_ns = cg.esphome_ns.namespace("soft_i2c")
SoftI2CBus = soft_i2c_ns.class_("SoftI2CBus", i2c.I2CBus, cg.Component)


def _validate_pin(value, key):
    """Reject pin configs the bit-bang driver cannot honour."""
    if isinstance(value, dict) and value.get(CONF_INVERTED):
        raise cv.Invalid(
            "soft_i2c cannot use an inverted pin: the master never drives the line high, "
            "so inverting it would invert the bus, not the signal",
            key,
        )
    return value


def _validate_bus(config: ConfigType) -> ConfigType:
    _validate_pin(config[CONF_SDA], [CONF_SDA])
    _validate_pin(config[CONF_SCL], [CONF_SCL])
    if config[CONF_SDA] == config[CONF_SCL]:
        raise cv.Invalid("SDA and SCL must be two different pins")
    if not config[CONF_INTERNAL_PULLUP]:
        _LOGGER.info(
            "soft_i2c: internal_pullup disabled - external pull-up resistors (4.7k-10k) on SDA and SCL are required"
        )
    return config


def _validate_config(config: ConfigType) -> ConfigType:
    # MULTI_CONF: the loader hands us the whole list, otherwise the single config
    for bus in config if isinstance(config, list) else [config]:
        _validate_bus(bus)
    return config


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SoftI2CBus),
        # Both lines default to input so the bus idles high before setup() takes over
        cv.Required(CONF_SDA): pins.internal_gpio_input_pin_schema,
        cv.Required(CONF_SCL): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_FREQUENCY, default="100kHz"): cv.All(
            cv.frequency, cv.float_range(min=1000, min_included=True, max=400000, max_included=True)
        ),
        cv.Optional(CONF_SCAN, default=False): cv.boolean,
        cv.Optional(CONF_INTERNAL_PULLUP, default=True): cv.boolean,
        cv.Optional(CONF_TIMEOUT, default="10ms"): cv.positive_time_period,
        cv.Optional(CONF_BUS_RECOVERY, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)

# The loader looks up this exact attribute name; the function checks the *validated* config.
FINAL_VALIDATE_SCHEMA = _validate_config


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    sda = await cg.gpio_pin_expression(config[CONF_SDA])
    cg.add(var.set_sda_pin(sda))
    scl = await cg.gpio_pin_expression(config[CONF_SCL])
    cg.add(var.set_scl_pin(scl))

    cg.add(var.set_frequency(int(config[CONF_FREQUENCY])))
    cg.add(var.set_scan(config[CONF_SCAN]))
    cg.add(var.set_internal_pullup(config[CONF_INTERNAL_PULLUP]))
    cg.add(var.set_timeout_us(int(config[CONF_TIMEOUT].total_microseconds)))
    cg.add(var.set_bus_recovery(config[CONF_BUS_RECOVERY]))
