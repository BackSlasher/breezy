import esphome.codegen as cg

CODEOWNERS = ["@nitz"]

# The actual platform schema lives in climate.py (ESPHome looks for
# <component>/<domain>.py for `climate: - platform: breezy_climate`).
breezy_climate_ns = cg.esphome_ns.namespace("breezy_climate")
