"""Software (bit-banged) I2C bus.

Drop-in replacement for ``i2c:`` on platforms where ESPHome has no I2C at all - most notably
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
    CONF_NUMBER,
    CONF_SCAN,
    CONF_SCL,
    CONF_SDA,
    CONF_TIMEOUT,
)
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)

CONF_INTERNAL_PULLUP = "internal_pullup"
CONF_BUS_RECOVERY = "bus_recovery"
CONF_POWER_PIN = "power_pin"
CONF_POWER_ON_DELAY = "power_on_delay"
CONF_DIAG_PINS = "diag_pins"
CONF_PAIR_SWEEP = "pair_sweep"
CONF_SWEEP_PINS = "sweep_pins"
CONF_SWEEP_BUDGET = "sweep_budget"
CONF_DRIVE_MODE = "drive_mode"
CONF_POWER_ACTIVE_LOW = "power_active_low"
CONF_SLOW_PROBE_PAIRS = "slow_probe_pairs"
CONF_SLOW_PROBE_ADDRESSES = "slow_probe_addresses"
CONF_SLOW_PROBE_FREQUENCIES = "slow_probe_frequencies"
CONF_PROTOCOL_PROBE = "protocol_probe"
CONF_PROTOCOL_PROBE_ADDRESSES = "protocol_probe_addresses"
CONF_PAD_CENSUS = "pad_census"
CONF_STRETCH_TIMEOUT = "stretch_timeout"

DEPENDENCIES = ["i2c"]
MULTI_CONF = True

soft_i2c_ns = cg.esphome_ns.namespace("soft_i2c")
SoftI2CBus = soft_i2c_ns.class_("SoftI2CBus", i2c.I2CBus, cg.Component)


# [sda, scl] pair for the frequency ladder. cv.length_exactly does not exist in this version of
# config_validation, so the length is checked by a custom validator.
_pair_schema = cv.All(
    cv.ensure_list(cv.int_range(min=0, max=31)),
    lambda v: v if len(v) == 2 else (_ for _ in ()).throw(
        cv.Invalid("para musi miec dokladnie dwa pady: [sda, scl]")
    ),
)


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
    if len(config[CONF_DIAG_PINS]) > 8:
        raise cv.Invalid("diag_pins is diagnostics - at most 8 pads, otherwise the log dump buries everything else")
    if config[CONF_PAIR_SWEEP]:
        # GPIO6..11 are NOT blocked here. The earlier block ("QSPI/XIP, bricking risk") was
        # WRONG, and it is what hid the sensor: BK7231N has 2 MB of flash inside the package
        # (CBU datasheet, chapter 1), and the LibreTiny board definition (boards/variants/cbu.c)
        # describes GPIO6, 7, 8, 9, 10 and 11 as ordinary PIN_GPIO | PIN_IRQ | PIN_PWM. The CBU
        # datasheet itself brings them out to module pins: P6 (pin 10), P7 (pin 9), P8 (pin 8),
        # P9 (pin 19), all "common GPIO / hardware PWM". OpenBeken drives an SHT30 on this very
        # board on P7/P8, so driving them as GPIO is proven practice, not an experiment.
        # The warning stays deliberately: pads 0/1 are the logger's UART2, and 6..11 are the PWM
        # bank - if the project uses any of them for something else, the sweep will take them over.
        risky = sorted(set(config[CONF_SWEEP_PINS]) & (set(range(6, 12)) | {0, 1}))
        if risky:
            _LOGGER.warning(
                "soft_i2c: sweep_pins zawiera GPIO%s - bank PWM (6..11) i piny logera UART2 (0,1). "
                "Na CBU sa to normalne GPIO, ale jesli inny komponent uzyla tych padow, przesiew je "
                "przejmie na czas skanowania.",
                risky,
            )
        if len(config[CONF_SWEEP_PINS]) < 2:
            raise cv.Invalid(
                "pair_sweep requires sweep_pins with at least two pads - the GPIO-capable pad list from the "
                "board definition (for cbu: 0,1,6..11,14..17,20..24,26,28) minus the flash and "
                "logger lines",
                [CONF_SWEEP_PINS],
            )
    if config[CONF_PAD_CENSUS]:
        seen = set()
        for p in config[CONF_PAD_CENSUS]:
            p = int(p)
            if p in (int(config[CONF_SDA][CONF_NUMBER]), int(config[CONF_SCL][CONF_NUMBER])):
                raise cv.Invalid(
                    f"pad_census nie moze zawierac GPIO{p} - to pad magistrali, zarzadza nim bus, "
                    "a spis przelaczylby go w tryb wejscia w srodku transmisji",
                    [CONF_PAD_CENSUS],
                )
            if p in seen:
                raise cv.Invalid(f"pad_census zawiera duplikat GPIO{p}", [CONF_PAD_CENSUS])
            seen.add(p)
        if len(seen) > 16:
            raise cv.Invalid(
                "pad_census is capped at 16 pads - beyond that the log stops being readable, and exhausting "
                "the list adds nothing anyway",
                [CONF_PAD_CENSUS],
            )
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
        # Sensor power pin (e.g. 8 = P8 "battery relay" on the Tuya CBU). Raised in setup() before
        # the line configuration and the scan.
        cv.Optional(CONF_POWER_PIN): pins.internal_gpio_output_pin_schema,
        cv.Optional(CONF_POWER_ON_DELAY, default="0ms"): cv.positive_time_period_milliseconds,
        # Read of the pad's cfg register in dump_config() - proof that the actuator really moved the pad.
        cv.Optional(CONF_DIAG_PINS, default=[]): cv.ensure_list(cv.int_range(min=0, max=31)),
        # SDA/SCL pair sweep over the given list of pads. The list is mandatory because "all pins"
        # is not safe on BK7231 - see the validation below.
        cv.Optional(CONF_PAIR_SWEEP, default=False): cv.boolean,
        cv.Optional(CONF_SWEEP_PINS, default=[]): cv.ensure_list(cv.int_range(min=0, max=31)),
        cv.Optional(CONF_SWEEP_BUDGET, default="5s"): cv.positive_time_period_milliseconds,
        # How to "pull the line down". On Beken, digitalWrite(LOW) before pinMode(OUTPUT) drives
        # nothing - see the component docstring. "output" is the canonical Arduino order,
        # "pulldown" gives up OUTPUT mode and uses the pull-down as the low state.
        cv.Optional(CONF_DRIVE_MODE, default="output"): cv.one_of("output", "pulldown", lower=True),
        # Active-low power switch (PMOS between the battery and the sensor).
        cv.Optional(CONF_POWER_ACTIVE_LOW, default=False): cv.boolean,
        # Frequency ladder on selected pairs: a pair as [sda, scl], addresses and Hz given explicitly.
        cv.Optional(CONF_SLOW_PROBE_PAIRS, default=[]): cv.ensure_list(_pair_schema),
        cv.Optional(CONF_SLOW_PROBE_ADDRESSES, default=[0x40]): cv.ensure_list(
            cv.i2c_address  # pylint: disable=no-member
        ),
        cv.Optional(CONF_SLOW_PROBE_FREQUENCIES, default=[100000, 20000, 5000, 1000, 200]): (
            cv.ensure_list(cv.positive_int)
        ),
        # Raw read of the T&H protocols under a single address: SHT3x (command 0x2C06 + CRC in data),
        # SHT4x (command 0xFD without CRC in the command) and CHT8305 (registers 0x00/0x01 without
        # CRC). The same address, three protocols, bytes and the CRC verdict in the log - this
        # settles what really sits on the bus, instead of guessing which sensor platform to pick.
        cv.Optional(CONF_PROTOCOL_PROBE, default=False): cv.boolean,
        cv.Optional(CONF_PROTOCOL_PROBE_ADDRESSES, default=[0x44]): cv.ensure_list(
            cv.i2c_address  # pylint: disable=no-member
        ),
        # Electrical census of the pads behind the button and the LED. The pad is read twice: with
        # the internal pull-up and with the internal pull-down (~tens of kOhm). 0/0 = permanently
        # tied to ground (button to GND, or the GND net). 1/1 = permanently tied to VDD through an
        # impedance smaller than the pull-down (LED through a resistor, external pull-up).
        # 1/0 = floating, nothing holds it.
        cv.Optional(CONF_PAD_CENSUS, default=[]): cv.ensure_list(cv.int_range(min=0, max=31)),
        # How long we wait for the slave to raise SCL on its own after clock stretching. Default 120
        # ms: an SHT3x high-repeatability conversion is ~8.5 ms typically, but it grows at weak
        # supply, and cutting it in half latches the bus (the slave stays in the middle of a
        # transmission and holds SCL). `timeout` remains the operational limit; this is the
        # PATIENCE limit for the clock.
        cv.Optional(CONF_STRETCH_TIMEOUT, default="120ms"): cv.positive_time_period_milliseconds,
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

    if CONF_POWER_PIN in config:
        power_pin = await cg.gpio_pin_expression(config[CONF_POWER_PIN])
        # The pin number comes straight from the validated config - GPIOPin does not expose get_pin()
        # in all versions, and the register diagnostics need exactly this number.
        cg.add(var.set_power_pin(power_pin, int(config[CONF_POWER_PIN][CONF_NUMBER])))
        cg.add(var.set_power_on_delay_ms(int(config[CONF_POWER_ON_DELAY].total_milliseconds)))

    if config[CONF_DIAG_PINS]:
        init = ", ".join(str(int(p)) for p in config[CONF_DIAG_PINS])
        cg.add(var.set_diag_pins(cg.RawExpression(f"std::vector<uint8_t>{{{init}}}")))

    cg.add(var.set_pair_sweep(config[CONF_PAIR_SWEEP]))
    if config[CONF_SWEEP_PINS]:
        init = ", ".join(str(int(p)) for p in config[CONF_SWEEP_PINS])
        cg.add(var.set_sweep_pins(cg.RawExpression(f"std::vector<uint8_t>{{{init}}}")))
        cg.add(var.set_sweep_budget_ms(int(config[CONF_SWEEP_BUDGET].total_milliseconds)))

    cg.add(var.set_drive_mode(1 if config[CONF_DRIVE_MODE] == "pulldown" else 0))
    cg.add(var.set_power_active_low(config[CONF_POWER_ACTIVE_LOW]))

    if config[CONF_SLOW_PROBE_PAIRS]:
        flat = ", ".join(str(int(p)) for pair in config[CONF_SLOW_PROBE_PAIRS] for p in pair)
        cg.add(var.set_slow_probe_pairs(cg.RawExpression(f"std::vector<uint8_t>{{{flat}}}")))
        addrs = ", ".join(f"0x{int(a):02X}" for a in config[CONF_SLOW_PROBE_ADDRESSES])
        cg.add(var.set_slow_probe_addresses(cg.RawExpression(f"std::vector<uint8_t>{{{addrs}}}")))
        freqs = ", ".join(str(int(f)) for f in config[CONF_SLOW_PROBE_FREQUENCIES])
        cg.add(var.set_slow_probe_frequencies(cg.RawExpression(f"std::vector<uint32_t>{{{freqs}}}")))

    cg.add(var.set_stretch_timeout_us(int(config[CONF_STRETCH_TIMEOUT].total_microseconds)))
    cg.add(var.set_protocol_probe(config[CONF_PROTOCOL_PROBE]))
    if config[CONF_PROTOCOL_PROBE_ADDRESSES]:
        addrs = ", ".join(f"0x{int(a):02X}" for a in config[CONF_PROTOCOL_PROBE_ADDRESSES])
        cg.add(var.set_protocol_probe_addresses(cg.RawExpression(f"std::vector<uint8_t>{{{addrs}}}")))

    if config[CONF_PAD_CENSUS]:
        init = ", ".join(str(int(p)) for p in config[CONF_PAD_CENSUS])
        cg.add(var.set_census_pins(cg.RawExpression(f"std::vector<uint8_t>{{{init}}}")))
