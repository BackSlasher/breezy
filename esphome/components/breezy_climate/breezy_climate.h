#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/remote_base/remote_base.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/breezy_spi/breezy_spi.h"

namespace esphome {
namespace breezy_climate {

// IR timing constants (microseconds).
// Retuned 2026-08-06 to match the vendor remote AT SOURCE: same-tap diff of
// remote frames vs ours showed our marks ~26us short, our spaces ~26us long,
// and header/sep spaces ~33us long (n=2838 short symbols per side). The old
// constants came from early captures with a different measurement bias. The
// controller accepted remote frames 10/10 while ours landed at a few percent
// in the same hour - this skew is the prime suspect.
constexpr uint16_t HEADER_MARK = 2994;
constexpr uint16_t HEADER_SPACE_HEAT = 2813;
constexpr uint16_t HEADER_SPACE_PWR = 3773;
constexpr uint16_t SEP_MARK = 2953;
constexpr uint16_t SEP_SPACE_HEAT = 2823;
constexpr uint16_t SEP_SPACE_PWR = 3777;
constexpr uint16_t TRAIL = 3980;

// Symbol timings
constexpr uint16_t SS_MARK = 1024;
constexpr uint16_t SS_SPACE = 894;
constexpr uint16_t LS_MARK = 1024;
constexpr uint16_t LS_SPACE = 1854;
constexpr uint16_t LM_MARK = 1992;
constexpr uint16_t LM_SPACE = 894;
constexpr uint16_t LL_MARK = 1992;
constexpr uint16_t LL_SPACE = 1849;

// Mode types for IR encoding
enum class IRMode : uint8_t {
  Heat,
  Cool,
  FanOnly,
  PowerToggle
};

// Fan speeds for IR encoding
enum class IRFan : uint8_t {
  Auto,
  Low,
  Med,
  High
};

// Encoding styles
enum class Encoding : uint8_t {
  Standard,   // Uses LL lookahead from fresh state (heat mode)
  LsStart,    // No LL from fresh, but LL allowed after (cool/fan modes)
  NoLl        // Pure LS/LM alternation, no LL ever
};

// Symbol-encoder state machine states (shared by the command encoder and the
// power-frame encoder, which seeds it with the state its verbatim code
// symbols leave behind).
enum class State : uint8_t { Fresh, After0, AfterLL, AfterLM, AfterLS };

class BreezyClimate : public climate::Climate,
                      public Component,
                      public remote_base::RemoteTransmittable {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Sensor references for state feedback
  void set_power_sensor(binary_sensor::BinarySensor *sensor) { power_sensor_ = sensor; }
  void set_mode_sensor(text_sensor::TextSensor *sensor) { mode_sensor_ = sensor; }
  void set_set_temp_sensor(sensor::Sensor *sensor) { set_temp_sensor_ = sensor; }
  void set_room_temp_sensor(sensor::Sensor *sensor) { room_temp_sensor_ = sensor; }
  void set_fan_sensor(text_sensor::TextSensor *sensor) { fan_sensor_ = sensor; }
  void set_compressor_sensor(binary_sensor::BinarySensor *sensor) { compressor_sensor_ = sensor; }
  void set_spi_hub(breezy_spi::BreezySPIComponent *hub) { spi_hub_ = hub; }

  // Climate trait definition
  climate::ClimateTraits traits() override;

  // Handle control commands from HA
  void control(const climate::ClimateCall &call) override;

 protected:
  // IR transmission
  void send_ir_command_(IRMode mode, uint8_t temp, IRFan fan);
  // Returns true if a state-carrying power frame was sent (no chaser needed).
  bool send_power_toggle_();
  bool power_code_(IRMode mode, IRFan fan, const char **bits);
  size_t power_state_encode_(IRMode mode, IRFan fan, uint8_t temp,
                             uint16_t *out, size_t max_len);
  void transmit_timings_(const uint16_t *timings, size_t count);

  // IR encoding
  size_t ir_encode_(IRMode mode, uint8_t temp, IRFan fan, uint16_t *out, size_t max_len);
  uint8_t get_byte0_(IRMode mode, IRFan fan);
  uint8_t get_temp_byte_(uint8_t temp);
  void byte_to_bits_msb_(uint8_t byte, uint8_t *bits);
  size_t encode_bits_(const uint8_t *bits, size_t num_bits, Encoding encoding,
                      uint16_t *out, size_t max_len, State initial = State::Fresh);
  size_t generate_frame_(uint8_t byte0, uint8_t byte1, uint8_t byte2, uint8_t byte3,
                         uint8_t extra_bits, uint16_t header_space, Encoding encoding,
                         uint16_t *out, size_t max_len);
  size_t generate_24c_frame_(uint8_t byte0, uint16_t header_space, Encoding byte0_encoding,
                             uint16_t *out, size_t max_len);

  // State update from sensors
  void update_state_from_sensors_(bool hold_targets);

  // True if the SPI-derived sensors agree with what we last transmitted
  bool command_confirmed_();
  // True only when the mode sensor positively proves the unit is running
  // (cool/fan_only). 0x20 is ambiguous between heat and OFF on the raw bus.
  bool hvac_definitely_on_();
  const char *ir_fan_to_string_(IRFan fan);
  const char *ir_mode_to_string_(IRMode mode);

  // Convert HA types to IR types
  IRFan climate_fan_to_ir_fan_(climate::ClimateFanMode fan);
  IRMode climate_mode_to_ir_mode_(climate::ClimateMode mode);

  // Sensor references
  binary_sensor::BinarySensor *power_sensor_{nullptr};
  text_sensor::TextSensor *mode_sensor_{nullptr};
  sensor::Sensor *set_temp_sensor_{nullptr};
  sensor::Sensor *room_temp_sensor_{nullptr};
  text_sensor::TextSensor *fan_sensor_{nullptr};
  binary_sensor::BinarySensor *compressor_sensor_{nullptr};
  breezy_spi::BreezySPIComponent *spi_hub_{nullptr};

  // Deferred transmission. control() may be called from the web server's httpd
  // task, where FreeRTOS preempts the busy-wait in transmit_timings_() and
  // corrupts the IR timing (verified 2026-08-02: web-path transmissions decode
  // as garbage while API-path ones are clean). So control() only records what
  // to send, and loop() - which always runs in the main task - sends it.
  // Decide: power-vs-command routing deferred into loop() so it can wait for
  // a FRESH power reading - deciding in control() used stale post-TX data
  // and mis-routed (2026-08-07: skipped the wake toggle entirely).
  enum class TxStage : uint8_t { None, Decide, PowerToggle, Command };
  TxStage tx_stage_{TxStage::None};
  bool tx_target_off_{false};
  uint32_t decide_deadline_{0};
  // Burst repetition is DISABLED (kTxBursts = 1): every repetition scheme
  // tried on 2026-08-06 made things worse or no better, and each "rule"
  // fitted to one dataset died on the next (see v4_bringup_notes.md - deaf
  // windows, duty skew, second-sighting all disproven). Empirically the
  // sparse pattern wins: single bursts with the closed-loop retry at ~5s
  // intervals eventually land everything, while rapid repeats appear
  // actively counterproductive (best surviving hypothesis: a controller-side
  // refractory period that RESETS on receipt). The vendor remote's ~7 tight
  // bursts per press work, so a fast-acceptance recipe exists - finding it
  // needs a systematic burst-count x spacing sweep, not more single-shot
  // guesses. Until then: 1 burst, retries do the repetition.
  static constexpr uint8_t kTxBursts = 1;
  static constexpr uint32_t kTxBurstGapMs = 2000;
  uint8_t tx_bursts_left_{0};
  bool tx_command_after_toggle_{false};
  IRMode tx_mode_{IRMode::Cool};
  uint8_t tx_temp_{22};
  IRFan tx_fan_{IRFan::Auto};
  uint32_t tx_not_before_{0};

  // Closed-loop verification. A ~190-symbol transmission outlives the RMT
  // hardware buffer and is refilled by interrupt, so a late refill can still
  // glitch the output (measured 2026-08-02: 1 of 8 transmissions came out with
  // a malformed third frame, and that was exactly the one the HVAC ignored).
  // The SPI decode is an independent witness of the HVAC's real state, so we
  // check whether the command actually landed and re-send if it did not.
  bool verify_pending_{false};
  uint32_t verify_at_{0};
  uint32_t verify_wait_ms_{2500};  // escalates x1.5 per retry (self-jam recovery)
  uint8_t tx_retries_left_{0};

  // Timing buffer
  uint16_t ir_buffer_[400];
};

}  // namespace breezy_climate
}  // namespace esphome
