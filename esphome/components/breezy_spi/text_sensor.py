import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID
from . import breezy_spi_ns, BreezySPIComponent

CONF_BREEZY_SPI_ID = "breezy_spi_id"
CONF_MODE = "mode"
CONF_FAN = "fan"

DEPENDENCIES = ["breezy_spi"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BREEZY_SPI_ID): cv.use_id(BreezySPIComponent),
        cv.Optional(CONF_MODE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_FAN): text_sensor.text_sensor_schema(),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_BREEZY_SPI_ID])

    if mode_config := config.get(CONF_MODE):
        sens = await text_sensor.new_text_sensor(mode_config)
        cg.add(parent.set_mode_sensor(sens))

    if fan_config := config.get(CONF_FAN):
        sens = await text_sensor.new_text_sensor(fan_config)
        cg.add(parent.set_fan_sensor(sens))
