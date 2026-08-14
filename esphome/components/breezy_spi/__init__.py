import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL

CODEOWNERS = ["@nitz"]
MULTI_CONF = False

CONF_CLK_PIN = "clk_pin"
CONF_DATA_PIN = "data_pin"
CONF_DIALECT = "dialect"

breezy_spi_ns = cg.esphome_ns.namespace("breezy_spi")
BreezySPIComponent = breezy_spi_ns.class_("BreezySPIComponent", cg.PollingComponent)
Dialect = breezy_spi_ns.enum("Dialect")
DIALECTS = {
    # The controller<->panel bus as it really is: 33ms poll/response cycle,
    # ~76-bit replies headed 60 00. Use when breezy taps the bus directly.
    "raw": Dialect.DIALECT_RAW,
    # The flattened 112-bit re-broadcast served by the AC's "splitter"
    # accessory (an active protocol translator). Use when breezy sits
    # downstream of the splitter.
    "splitter": Dialect.DIALECT_SPLITTER,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BreezySPIComponent),
        cv.Required(CONF_CLK_PIN): pins.internal_gpio_input_pin_schema,
        cv.Required(CONF_DATA_PIN): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_DIALECT, default="raw"): cv.enum(DIALECTS, lower=True),
        cv.Optional(CONF_UPDATE_INTERVAL, default="1s"): cv.update_interval,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    clk_pin = await cg.gpio_pin_expression(config[CONF_CLK_PIN])
    cg.add(var.set_clk_pin(clk_pin))

    data_pin = await cg.gpio_pin_expression(config[CONF_DATA_PIN])
    cg.add(var.set_data_pin(data_pin))

    cg.add(var.set_dialect(config[CONF_DIALECT]))
