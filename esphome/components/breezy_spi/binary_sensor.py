import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID, DEVICE_CLASS_POWER, DEVICE_CLASS_RUNNING
from . import breezy_spi_ns, BreezySPIComponent

CONF_BREEZY_SPI_ID = "breezy_spi_id"
CONF_POWER = "power"
CONF_COMPRESSOR = "compressor"

DEPENDENCIES = ["breezy_spi"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BREEZY_SPI_ID): cv.use_id(BreezySPIComponent),
        cv.Optional(CONF_POWER): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_POWER,
        ),
        cv.Optional(CONF_COMPRESSOR): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_RUNNING,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_BREEZY_SPI_ID])

    if power_config := config.get(CONF_POWER):
        sens = await binary_sensor.new_binary_sensor(power_config)
        cg.add(parent.set_power_sensor(sens))

    if compressor_config := config.get(CONF_COMPRESSOR):
        sens = await binary_sensor.new_binary_sensor(compressor_config)
        cg.add(parent.set_compressor_sensor(sens))
