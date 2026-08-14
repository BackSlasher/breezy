import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, remote_base
from esphome.components.breezy_spi import BreezySPIComponent
from esphome.const import CONF_ID

from . import breezy_climate_ns

DEPENDENCIES = ["breezy_spi"]

CONF_POWER_SENSOR = "power_sensor"
CONF_MODE_SENSOR = "mode_sensor"
CONF_SET_TEMP_SENSOR = "set_temp_sensor"
CONF_ROOM_TEMP_SENSOR = "room_temp_sensor"
CONF_FAN_SENSOR = "fan_sensor"
CONF_COMPRESSOR_SENSOR = "compressor_sensor"
CONF_SPI_HUB = "spi_hub"

BreezyClimate = breezy_climate_ns.class_("BreezyClimate", climate.Climate, cg.Component)

# climate_schema() replaced the removed CLIMATE_SCHEMA and already declares the
# component ID, so no GenerateID here.
# IR goes out through a remote_transmitter (RMT hardware timing) - see the
# comment in transmit_timings_(). Software bit-banging a GPIO was not reliable.
CONFIG_SCHEMA = climate.climate_schema(BreezyClimate).extend(
    {
        cv.Optional(CONF_POWER_SENSOR): cv.use_id(cg.EntityBase),
        cv.Optional(CONF_MODE_SENSOR): cv.use_id(cg.EntityBase),
        cv.Optional(CONF_SET_TEMP_SENSOR): cv.use_id(cg.EntityBase),
        cv.Optional(CONF_ROOM_TEMP_SENSOR): cv.use_id(cg.EntityBase),
        cv.Optional(CONF_FAN_SENSOR): cv.use_id(cg.EntityBase),
        cv.Optional(CONF_COMPRESSOR_SENSOR): cv.use_id(cg.EntityBase),
        cv.Optional(CONF_SPI_HUB): cv.use_id(BreezySPIComponent),
    }
).extend(cv.COMPONENT_SCHEMA).extend(remote_base.REMOTE_TRANSMITTABLE_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    await remote_base.register_transmittable(var, config)

    for key, setter in [
        (CONF_POWER_SENSOR, var.set_power_sensor),
        (CONF_MODE_SENSOR, var.set_mode_sensor),
        (CONF_SET_TEMP_SENSOR, var.set_set_temp_sensor),
        (CONF_ROOM_TEMP_SENSOR, var.set_room_temp_sensor),
        (CONF_FAN_SENSOR, var.set_fan_sensor),
        (CONF_COMPRESSOR_SENSOR, var.set_compressor_sensor),
        (CONF_SPI_HUB, var.set_spi_hub),
    ]:
        if key in config:
            sens = await cg.get_variable(config[key])
            cg.add(setter(sens))
