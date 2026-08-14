import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)
from . import breezy_spi_ns, BreezySPIComponent

CONF_BREEZY_SPI_ID = "breezy_spi_id"
CONF_SET_TEMPERATURE = "set_temperature"
CONF_ROOM_TEMPERATURE = "room_temperature"

DEPENDENCIES = ["breezy_spi"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BREEZY_SPI_ID): cv.use_id(BreezySPIComponent),
        cv.Optional(CONF_SET_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_ROOM_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_BREEZY_SPI_ID])

    if set_temp_config := config.get(CONF_SET_TEMPERATURE):
        sens = await sensor.new_sensor(set_temp_config)
        cg.add(parent.set_set_temperature_sensor(sens))

    if room_temp_config := config.get(CONF_ROOM_TEMPERATURE):
        sens = await sensor.new_sensor(room_temp_config)
        cg.add(parent.set_room_temperature_sensor(sens))
