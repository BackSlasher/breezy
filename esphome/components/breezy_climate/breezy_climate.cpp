#include "breezy_climate.h"
#include "esphome/core/log.h"

namespace esphome {
namespace breezy_climate {

static const char *const TAG = "breezy_climate";

// Temperature byte1 encoding (same for heat/cool/fan modes)
static const uint8_t TEMP_BYTES[15] = {
    0x0c,  // 16C
    0x18,  // 17C
    0x14,  // 18C
    0x30,  // 19C
    0x3c,  // 20C
    0x28,  // 21C
    0x24,  // 22C
    0x60,  // 23C
    0x78,  // 24C
    0x78,  // 25C (same byte as 24C, different encoding)
    0x74,  // 26C
    0x50,  // 27C
    0x5c,  // 28C
    0x48,  // 29C
    0x44,  // 30C
};

// Heat mode fan byte0 values
static const uint8_t HEAT_FAN_BYTES[4] = {0x74, 0x60, 0x5c, 0x78};  // Auto, Low, Med, High

// Cool mode fan byte0 values
static const uint8_t COOL_FAN_BYTES[4] = {0x24, 0x30, 0x3c, 0x28};

// Fan-only mode byte0 = cool_byte0 XOR 0xC0
static const uint8_t FAN_ONLY_BYTES[4] = {0xe4, 0xf0, 0xfc, 0xe8};

// Power toggle byte0 values
static const uint8_t POWER_TOGGLE_BYTES[4] = {0xf4, 0xe0, 0xdc, 0xf8};

void BreezyClimate::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Breezy Climate...");

  // The pin belongs to the remote_transmitter (RMT); we never touch it
  // directly. It idles LOW, which keeps the transistor off and leaves the IR
  // line released - driving it HIGH would clamp the line to ground and kill
  // the real remote.

  // Set initial state
  this->mode = climate::CLIMATE_MODE_OFF;
  this->target_temperature = 22;
  this->fan_mode = climate::CLIMATE_FAN_AUTO;

#ifdef USE_API
  register_service(&BreezyClimate::on_send_ir_raw, "send_ir_raw", {"timings"});
  register_service(&BreezyClimate::on_send_ir_raw_n, "send_ir_raw_n",
                   {"timings", "times", "wait_us"});
#endif
}

#ifdef USE_API
void BreezyClimate::on_send_ir_raw(std::vector<int32_t> timings) {
  ESP_LOGI(TAG, "send_ir_raw: %u timings", (unsigned) timings.size());
  // Clamp out-of-range values to 0; transmit_timings_ skips zeros while still
  // flipping mark/space parity, which exactly matches the old yaml lambda
  // (including tolerating the historical 65535 end-of-capture terminator).
  std::vector<uint16_t> buf(timings.size());
  for (size_t i = 0; i < timings.size(); i++) {
    int t = timings[i];
    buf[i] = (t > 0 && t < 65535) ? (uint16_t) t : 0;
  }
  transmit_timings_(buf.data(), buf.size());
}

void BreezyClimate::on_send_ir_raw_n(std::vector<int32_t> timings, int32_t times, int32_t wait_us) {
  ESP_LOGI(TAG, "send_ir_raw_n: %u timings x%d (wait %dus)",
           (unsigned) timings.size(), times, wait_us);
  if (this->transmitter_ == nullptr) {
    ESP_LOGE(TAG, "no transmitter configured");
    return;
  }
  auto call = this->transmitter_->transmit();
  auto *data = call.get_data();
  data->set_carrier_frequency(0);
  bool mark = true;
  for (int t : timings) {
    if (t > 0 && t < 65535) {
      if (mark) {
        data->mark(t);
      } else {
        data->space(t);
      }
    }
    mark = !mark;
  }
  call.set_send_times(times);
  call.set_send_wait(wait_us);
  call.perform();
}
#endif

void BreezyClimate::loop() {
  // Send any pending IR here, in the main task. Never transmit from control():
  // that can run in the httpd task where preemption corrupts the timing.
  if (tx_stage_ != TxStage::None && (int32_t) (millis() - tx_not_before_) >= 0) {
    if (tx_stage_ == TxStage::Decide) {
      // Wait (bounded) for a FRESH POWER reading before routing. Power has
      // its own freshness clock (master-vote) - it recovers from our TX
      // poisoning later than the reply stream, and gating on reply freshness
      // caused parity-flipping toggle retries (user-diagnosed 2026-08-07).
      bool fresh = spi_hub_ != nullptr && spi_hub_->status().valid &&
                   spi_hub_->power_age_ms() < 1500;
      if (!fresh && (int32_t) (millis() - decide_deadline_) < 0) {
        tx_not_before_ = millis() + 300;
        return;
      }
      bool on = fresh ? spi_hub_->status().power : hvac_definitely_on_();
      if (tx_target_off_) {
        if (on) {
          tx_stage_ = TxStage::PowerToggle;
          tx_command_after_toggle_ = false;
        } else {
          tx_stage_ = TxStage::None;  // already off
        }
      } else {
        if (on) {
          tx_stage_ = TxStage::Command;
        } else {
          tx_stage_ = TxStage::PowerToggle;
          tx_command_after_toggle_ = true;
        }
      }
      return;
    }
    if (tx_stage_ == TxStage::PowerToggle) {
      bool state_frame = send_power_toggle_();
      if (tx_command_after_toggle_ && state_frame) {
        // Single blast: the power frame carried the full target state, so
        // there is no chaser and no wrong-mode limbo. Just verify.
        tx_stage_ = TxStage::None;
        verify_pending_ = true;
        verify_at_ = millis() + verify_wait_ms_;
      } else if (tx_command_after_toggle_) {
        // Unmapped combo fell back to the legacy toggle: chase with the full
        // command after the receptive window (measured 2026-08-03: 500ms was
        // too early, 3000ms works).
        tx_stage_ = TxStage::Command;
        tx_not_before_ = millis() + 3000;
      } else {
        // Power-off: verify it actually went off.
        tx_stage_ = TxStage::None;
        verify_pending_ = true;
        verify_at_ = millis() + verify_wait_ms_;
      }
    } else {
      send_ir_command_(tx_mode_, tx_temp_, tx_fan_);
      if (tx_bursts_left_ == 0) tx_bursts_left_ = kTxBursts;
      if (--tx_bursts_left_ > 0) {
        tx_not_before_ = millis() + kTxBurstGapMs;
      } else {
        tx_stage_ = TxStage::None;
        // The HVAC reflects an accepted command within ~1.3s and the decode
        // is fresh within ~1s, so 2.5s is a safe verify point - tightened
        // from 4s (2026-08-06) to speed the retry loop, which is the whole
        // acceptance mechanism (see tx_retries_left_).
        verify_pending_ = true;
        verify_at_ = millis() + verify_wait_ms_;
      }
    }
    return;  // skip the sensor sync this iteration; state is in flux
  }

  // Closed-loop retry: re-send if the independent SPI reading disagrees.
  //
  // ESCALATING BACKOFF (2026-08-07): our own transmissions poison our own
  // decode pipeline for many seconds (dual-witness proof: the HVAC applied
  // the FIRST frame in 1.4s while this device's reading stayed stale through
  // eight retries 2.5s apart - 19s of self-jamming; the immune second tap
  // saw the truth immediately). Rapid retries therefore CAUSE the very
  // verification failures they respond to. Each retry now waits ~1.6x longer
  // than the last, giving the pipeline recovery room instead of re-poisoning
  // it. Most commands need zero retries - the HVAC accepts first frames.
  if (verify_pending_ && (int32_t) (millis() - verify_at_) >= 0) {
    // FRESHNESS GATE: never act on a stale decode. Our own transmissions
    // poison this device's decode pipeline for a variable stretch, so right
    // after sending, the sensors may still show the OLD state - retrying on
    // that is how retry storms are born (each retry re-poisons the decode
    // that would have exonerated it). If the decode is stale, wait for it to
    // recover; retransmit only on FRESH disagreement.
    // Every confirmation involves the power bit, so gate on BOTH clocks -
    // the reply stream AND the slower master-vote power.
    if (spi_hub_ != nullptr) {
      uint32_t age = spi_hub_->state_age_ms();
      if (spi_hub_->power_age_ms() > age) age = spi_hub_->power_age_ms();
      if (age > 1500) {
        verify_at_ = millis() + 1000;  // check again shortly, without sending
        return;
      }
    }
    verify_pending_ = false;
    if (!command_confirmed_()) {
      if (tx_retries_left_ > 0) {
        tx_retries_left_--;
        verify_wait_ms_ = verify_wait_ms_ + (verify_wait_ms_ >> 1);  // x1.5
        if (verify_wait_ms_ > 20000) verify_wait_ms_ = 20000;
        ESP_LOGW(TAG, "command not confirmed by SPI, re-sending (%u attempt(s) left, next verify in %ums)",
                 tx_retries_left_, verify_wait_ms_);
        // Re-ROUTE rather than re-command: an off-retry needs a toggle, and
        // the power state may have changed since the original decision.
        tx_stage_ = TxStage::Decide;
        decide_deadline_ = millis() + 5000;
        tx_not_before_ = millis();
      } else {
        ESP_LOGE(TAG, "command still not confirmed after retries; giving up");
      }
    }
  }

  // Optimistic UI: while a command is in flight or unverified, keep showing
  // the TARGET that control() already published. The decode is self-poisoned
  // for ~3-11s after our own TX, and republishing it made the HA card bounce
  // new -> old -> new over ~10s. Room temp and HVAC action stay live (they
  // are physical truth, independent of the pending command); mode/target/fan
  // resume from sensors on confirmation (where they equal the target) or on
  // give-up (honest revert to what the unit actually did).
  update_state_from_sensors_(tx_stage_ != TxStage::None || verify_pending_);
}

void BreezyClimate::update_state_from_sensors_(bool hold_targets) {
  // Snapshot so we only publish when something actually changed - loop() runs
  // thousands of times a second and an unconditional publish_state() floods
  // the API and logs.
  const float prev_current = this->current_temperature;
  const float prev_target = this->target_temperature;
  const climate::ClimateMode prev_mode = this->mode;
  const climate::ClimateFanMode prev_fan =
      this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO);

  // Update current temperature from room temp sensor
  if (room_temp_sensor_ != nullptr && room_temp_sensor_->has_state()) {
    this->current_temperature = room_temp_sensor_->state;
  }

  // Update target temperature from set temp sensor
  if (!hold_targets && set_temp_sensor_ != nullptr && set_temp_sensor_->has_state()) {
    this->target_temperature = set_temp_sensor_->state;
  }

  // Update mode from power and mode sensors
  if (!hold_targets && power_sensor_ != nullptr && power_sensor_->has_state()) {
    if (!power_sensor_->state) {
      this->mode = climate::CLIMATE_MODE_OFF;
    } else if (mode_sensor_ != nullptr && mode_sensor_->has_state()) {
      std::string mode_str = mode_sensor_->state;
      if (mode_str == "heat") {
        this->mode = climate::CLIMATE_MODE_HEAT;
      } else if (mode_str == "cool") {
        this->mode = climate::CLIMATE_MODE_COOL;
      } else if (mode_str == "fan_only") {
        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
      }
    }
  }

  // Update fan mode from sensor
  if (!hold_targets && fan_sensor_ != nullptr && fan_sensor_->has_state()) {
    std::string fan_str = fan_sensor_->state;
    if (fan_str == "auto") {
      this->fan_mode = climate::CLIMATE_FAN_AUTO;
    } else if (fan_str == "low") {
      this->fan_mode = climate::CLIMATE_FAN_LOW;
    } else if (fan_str == "medium") {
      this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
    } else if (fan_str == "high") {
      this->fan_mode = climate::CLIMATE_FAN_HIGH;
    }
  }

  // HVAC action: what the unit is actually DOING right now, from the
  // compressor sensor - "cooling" vs "idle at setpoint" is the difference
  // between the compressor running and waiting. Heat is reverse-cycle on
  // this unit, so the same compressor bit drives the heating action.
  const climate::ClimateAction prev_action = this->action;
  if (this->mode == climate::CLIMATE_MODE_OFF) {
    this->action = climate::CLIMATE_ACTION_OFF;
  } else if (this->mode == climate::CLIMATE_MODE_FAN_ONLY) {
    this->action = climate::CLIMATE_ACTION_FAN;
  } else if (compressor_sensor_ != nullptr && compressor_sensor_->has_state()) {
    if (compressor_sensor_->state) {
      this->action = (this->mode == climate::CLIMATE_MODE_HEAT)
                         ? climate::CLIMATE_ACTION_HEATING
                         : climate::CLIMATE_ACTION_COOLING;
    } else {
      this->action = climate::CLIMATE_ACTION_IDLE;
    }
  }

  if (this->current_temperature != prev_current || this->target_temperature != prev_target ||
      this->mode != prev_mode || this->action != prev_action ||
      this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO) != prev_fan) {
    this->publish_state();
  }
}

climate::ClimateTraits BreezyClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_ACTION);
  traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
  traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  traits.add_supported_mode(climate::CLIMATE_MODE_COOL);
  traits.add_supported_mode(climate::CLIMATE_MODE_FAN_ONLY);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_AUTO);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_LOW);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_MEDIUM);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_HIGH);
  traits.set_visual_min_temperature(16);
  traits.set_visual_max_temperature(30);
  traits.set_visual_temperature_step(1);
  return traits;
}

void BreezyClimate::control(const climate::ClimateCall &call) {
  // Get current state
  climate::ClimateMode target_mode = this->mode;
  float target_temp = this->target_temperature;
  climate::ClimateFanMode target_fan = this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO);

  // Apply changes from call
  if (call.get_mode().has_value()) {
    target_mode = *call.get_mode();
  }
  if (call.get_target_temperature().has_value()) {
    target_temp = *call.get_target_temperature();
  }
  if (call.get_fan_mode().has_value()) {
    target_fan = *call.get_fan_mode();
  }

  // NOTE: everything below only *queues* the transmission - loop() performs it.
  // Transmitting here would run in whatever task called control() (the httpd
  // task for web/UI requests), where preemption corrupts the IR timing.

  // Handle OFF.
  //
  // Power toggles are decided from the LIVE mode sensor, never from this
  // entity's own belief. On 2026-08-04 the entity - believing power was off
  // because its sensors were stale - "helpfully" prefixed a command with a
  // toggle and turned the running HVAC off. The raw bus cannot yet distinguish
  // an OFF unit from heat-idle (both report mode 0x20), so the rule is
  // asymmetric: toggling OFF requires positive proof the unit is on
  // (sensor reads cool/fan_only); anything less skips the toggle.
  if (target_mode == climate::CLIMATE_MODE_OFF) {
    tx_target_off_ = true;
    tx_stage_ = TxStage::Decide;
    tx_not_before_ = millis();
    decide_deadline_ = millis() + 5000;
    tx_retries_left_ = 6;
    verify_wait_ms_ = 2500;
    verify_pending_ = false;
    this->mode = climate::CLIMATE_MODE_OFF;
    // Remember temp/fan changes made while off - HA sends each parameter as
    // its own command, and dropping these here meant "set 23, then cool"
    // powered on with the STALE temperature (observed 2026-08-07).
    this->target_temperature = target_temp;
    this->fan_mode = target_fan;
    this->publish_state();
    return;
  }

  IRMode ir_mode = climate_mode_to_ir_mode_(target_mode);
  IRFan ir_fan = climate_fan_to_ir_fan_(target_fan);
  uint8_t temp = static_cast<uint8_t>(target_temp);
  if (temp < 16) temp = 16;
  if (temp > 30) temp = 30;

  tx_mode_ = ir_mode;
  tx_temp_ = temp;
  tx_fan_ = ir_fan;
  tx_not_before_ = millis();
  // THE ACCEPTANCE MODEL, FINAL (2026-08-07, dual-witness proof): the HVAC
  // accepts first frames essentially always - it beeps (ack) and applies
  // within ~1s. Every historical "miss" was an artifact: our own IR
  // transmission poisons OUR OWN decode pipeline for a variable ~3-11s, so
  // impatient verification read stale state and retried, and each retry
  // re-poisoned the decode that would have exonerated it (13-send storms for
  // one-degree changes). The vendor remote's ~15% re-press rate is optical
  // reception loss at the panel receiver, nothing more. Fixes: verification
  // is freshness-gated (never act on a stale decode) and compares the hub's
  // decoded status directly (sensor objects lag one polling tick). Retries
  // now exist only for genuine rarities. Open diagnostic: WHY one 200ms
  // transmission poisons decode for seconds (see TODO.md).
  tx_retries_left_ = 6;
  verify_wait_ms_ = 2500;  // backoff resets per command
  verify_pending_ = false;
  if (tx_stage_ == TxStage::PowerToggle && tx_command_after_toggle_) {
    // A power-ON sequence is already pending (e.g. HA sent "mode: cool" and
    // is now sending "temp: 23" two seconds later). Keep the stage; the
    // updated tx_mode_/tx_temp_/tx_fan_ flow into the power-state frame.
  } else {
    // Route in loop() once the power reading is provably fresh - deciding
    // here on possibly-stale data mis-routed (2026-08-07).
    tx_target_off_ = false;
    tx_stage_ = TxStage::Decide;
    decide_deadline_ = millis() + 5000;
  }

  // Update local state
  this->mode = target_mode;
  this->target_temperature = target_temp;
  this->fan_mode = target_fan;
  this->publish_state();
}

// THE POWER FRAME IS TOGGLE-WITH-EMBEDDED-STATE (cracked 2026-08-07 from
// remote captures): PWR header, then [mode/fan code][temp byte][zeros][tail],
// and on power-ON the unit applies the EMBEDDED state - there is no snapshot
// restore. The old POWER_TOGGLE_BYTES "fan variants" were mode+fan codes
// misfiled (0xF4 = heat/auto, which is why our toggles "restored heat").
// All 12 mode/fan combos are mapped (2026-08-07 guided capture session);
// the legacy toggle fallback below power_state_encode_ is dead code kept as
// a safety net.
bool BreezyClimate::power_code_(IRMode mode, IRFan fan, const char **syms) {
  // Code-region SYMBOL spellings, space-separated tokens (SS/LS/LM/LL).
  // The SPELLING IS THE CODE (2026-08-07 capture session): heat/med and
  // cool/med share the bit string 1111100 and differ ONLY in symbols
  // (LL LM LS LM vs LM LS LL LM) - the receiver reads symbols, not bits.
  // Families: heat starts LL, fan_only starts SS LL, cool starts LM LS;
  // cool bit-codes = fan_only bit-codes minus the leading 0.
  // Entries marked (grammar) follow the family pattern (cool alternates
  // LM/LS on 1-bits, compressing to LL only inside runs of >= 4) and were
  // live-fire-proven 2026-08-07 (single frame, unit obeyed); the rest are
  // capture-verified.
  switch (mode) {
    case IRMode::Heat:
      switch (fan) {
        case IRFan::Auto: *syms = "LL LL SS LM SS SS"; return true;
        case IRFan::Low:  *syms = "LL LM SS SS SS SS SS"; return true;
        case IRFan::Med:  *syms = "LL LM LS LM SS SS"; return true;
        case IRFan::High: *syms = "LL LL LM SS SS SS"; return true;
        default: return false;
      }
    case IRMode::FanOnly:
      switch (fan) {
        case IRFan::Auto: *syms = "SS LL SS SS LM SS SS"; return true;  // (grammar)
        case IRFan::Low:  *syms = "SS LL LM SS SS SS SS"; return true;
        case IRFan::Med:  *syms = "SS LL LL LM SS SS"; return true;
        case IRFan::High: *syms = "SS LL SS LM SS SS SS"; return true;
        default: return false;
      }
    case IRMode::Cool:
      switch (fan) {
        case IRFan::Auto: *syms = "LM LS SS SS LM SS SS"; return true;
        case IRFan::Low:  *syms = "LM LS LM SS SS SS SS"; return true;  // (grammar)
        case IRFan::Med:  *syms = "LM LS LL LM SS SS"; return true;
        case IRFan::High: *syms = "LM LS SS LM SS SS SS"; return true;  // (grammar)
        default: return false;
      }
    default:
      return false;
  }
}

#define PSF_APPEND(m, s) \
  do { if (pos + 2 > max_len) return 0; out[pos++] = (m); out[pos++] = (s); } while (0)

size_t BreezyClimate::power_state_encode_(IRMode mode, IRFan fan, uint8_t temp,
                                          uint16_t *out, size_t max_len) {
  const char *code;
  if (!power_code_(mode, fan, &code)) return 0;
  uint8_t tb = get_temp_byte_(temp);
  if (tb == 0 && temp != 16) return 0;

  // Everything after the code region: temp byte + 8 zeros + tail, spelled by
  // the Standard state machine seeded with the state the code symbols leave
  // behind. Verified against every capture: this reproduces the remote's
  // temp/tail spellings exactly (e.g. tail "11" comes out LS LM, never LL,
  // because zeros preserve AfterLM).
  uint8_t rest[24];
  size_t rn = 0;
  for (int i = 7; i >= 0; i--) rest[rn++] = (tb >> i) & 1;
  for (int i = 0; i < 8; i++) rest[rn++] = 0;
  const char *tail = (temp == 24) ? "000011" : "0000011";
  for (const char *p = tail; *p; p++) rest[rn++] = *p - '0';

  size_t pos = 0;
  for (int f = 0; f < 3; f++) {
    if (pos + 2 > max_len) return 0;
    out[pos++] = (f == 0) ? HEADER_MARK : SEP_MARK;
    out[pos++] = (f == 0) ? HEADER_SPACE_PWR : SEP_SPACE_PWR;
    // Code region: verbatim symbol replay, tracking the machine state.
    State st = State::Fresh;
    for (const char *p = code; *p; ) {
      char a = p[0], b = p[1];
      if (a == 'S' && b == 'S') {
        PSF_APPEND(SS_MARK, SS_SPACE);
        if (st == State::Fresh) st = State::AfterLM;
        else if (st == State::AfterLL || st == State::AfterLS) st = State::After0;
      } else if (a == 'L' && b == 'S') {
        PSF_APPEND(LS_MARK, LS_SPACE);
        st = State::AfterLS;
      } else if (a == 'L' && b == 'M') {
        PSF_APPEND(LM_MARK, LM_SPACE);
        st = State::AfterLM;
      } else {  // LL
        PSF_APPEND(LL_MARK, LL_SPACE);
        st = State::AfterLL;
      }
      p += 2;
      while (*p == ' ') p++;
    }
    pos += encode_bits_(rest, rn, Encoding::Standard, out + pos, max_len - pos, st);
  }
  if (pos >= max_len) return 0;
  out[pos++] = TRAIL;
  return pos;
}

bool BreezyClimate::send_power_toggle_() {
  // Prefer the state-carrying power frame with the TARGET state, so a
  // power-on lands directly in the wanted mode - single blast, no limbo.
  IRMode m = tx_command_after_toggle_ ? tx_mode_ : climate_mode_to_ir_mode_(
      this->mode == climate::CLIMATE_MODE_OFF ? climate::CLIMATE_MODE_COOL : this->mode);
  IRFan f = tx_command_after_toggle_ ? tx_fan_
      : climate_fan_to_ir_fan_(this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO));
  uint8_t temp = tx_command_after_toggle_ ? tx_temp_
      : static_cast<uint8_t>(this->target_temperature);
  if (temp < 16) temp = 16;
  if (temp > 30) temp = 30;

  size_t len = power_state_encode_(m, f, temp, ir_buffer_, 400);
  if (len > 0) {
    transmit_timings_(ir_buffer_, len);
    ESP_LOGD(TAG, "Sent power-state frame: mode=%d temp=%d fan=%d (%zu timings)",
             static_cast<int>(m), temp, static_cast<int>(f), len);
    return true;
  }
  // Unmapped combo: legacy auto-variant toggle (= heat/auto state frame, but
  // proven to toggle) - the chaser corrects the state afterwards.
  len = ir_encode_(IRMode::PowerToggle, temp, IRFan::Auto, ir_buffer_, 400);
  if (len > 0) {
    transmit_timings_(ir_buffer_, len);
    ESP_LOGD(TAG, "Sent legacy power toggle (%zu timings)", len);
  }
  return false;
}

void BreezyClimate::send_ir_command_(IRMode mode, uint8_t temp, IRFan fan) {
  size_t len = ir_encode_(mode, temp, fan, ir_buffer_, 400);
  if (len > 0) {
    transmit_timings_(ir_buffer_, len);
    ESP_LOGD(TAG, "Sent IR command: mode=%d temp=%d fan=%d (%zu timings)",
             static_cast<int>(mode), temp, static_cast<int>(fan), len);
  } else {
    ESP_LOGW(TAG, "IR encode failed");
  }
}

void BreezyClimate::transmit_timings_(const uint16_t *timings, size_t count) {
  // Hand the waveform to the RMT peripheral, which clocks it out in hardware.
  // Software bit-banging (digital_write + delayMicroseconds) was tried and does
  // not survive a multitasking OS: any task switch stretches a space enough to
  // flip a symbol, so transmissions issued from the web server's httpd task
  // decoded as garbage (verified 2026-08-02, and their three repeat-frames
  // differed from each other). Deferring to loop() and suspending the scheduler
  // each helped but neither was reliable. RMT removes the problem entirely -
  // nothing the CPU does can disturb the output timing.
  //
  // Carrier is disabled: this protocol is baseband (the panel's receiver has
  // already demodulated). A "mark" therefore drives the pin HIGH, which turns
  // the transistor on and pulls the IR line LOW - the correct active-low mark.
  if (this->transmitter_ == nullptr) {
    ESP_LOGE(TAG, "no transmitter configured");
    return;
  }
  // Sent as ONE buffer containing all three frames. Loading a single frame and
  // using RMT's repeat (set_send_times(3)) was tried 2026-08-02 to avoid
  // mid-flight FIFO refills - it transmits, but the HVAC rejects it (1/6, and
  // that one was a no-op). Presumably the repeat makes every burst start with
  // the header pair instead of header-then-separator, and drops the trailing
  // gap. Truncating to 1 or 2 frames is rejected too, so the full three-frame
  // sequence really is required; see v4_bringup_notes.md.
  auto call = this->transmitter_->transmit();
  auto *data = call.get_data();
  data->set_carrier_frequency(0);
  data->reserve(count);

  bool mark = true;
  for (size_t i = 0; i < count; i++) {
    uint16_t us = timings[i];
    if (us > 0 && us < 65535) {
      if (mark) {
        data->mark(us);
      } else {
        data->space(us);
      }
    }
    mark = !mark;
  }
  call.perform();
}

const char *BreezyClimate::ir_fan_to_string_(IRFan fan) {
  switch (fan) {
    case IRFan::Low: return "low";
    case IRFan::Med: return "medium";
    case IRFan::High: return "high";
    default: return "auto";
  }
}

bool BreezyClimate::hvac_definitely_on_() {
  // The power sensor is real as of 2026-08-06: on the raw bus it is decoded
  // from the master frames' byte[10] discriminator (0x01/0x02 only while
  // OFF), which distinguishes off from heat. Prefer it; fall back to the
  // mode sensor (cool/fan_only are positive proof of ON regardless).
  if (power_sensor_ != nullptr && power_sensor_->has_state()) {
    return power_sensor_->state;
  }
  return mode_sensor_ != nullptr && mode_sensor_->has_state() &&
         (mode_sensor_->state == "cool" || mode_sensor_->state == "fan_only");
}

const char *BreezyClimate::ir_mode_to_string_(IRMode mode) {
  switch (mode) {
    case IRMode::Heat: return "heat";
    case IRMode::Cool: return "cool";
    case IRMode::FanOnly: return "fan_only";
    default: return "";
  }
}

bool BreezyClimate::command_confirmed_() {
  // Compare against the SPI-derived reading of the HVAC's own display bus -
  // independent of whether our IR was received.
  //
  // Mode MUST be checked here: without it, a mode change that the HVAC ignored
  // still "confirms" whenever temp and fan happen to already match, so the
  // retry never fires (measured 2026-08-03 - this is how a failed fan_only and
  // a too-early post-power-on command both passed silently).
  //
  // Prefer the hub's DECODED STATUS over the sensor objects: sensors republish
  // only on the polling tick, so right after the decode recovers from
  // TX-poisoning they can lag one interval behind - the freshness gate would
  // approve a comparison against a value that is one tick stale, firing a
  // false retry (observed 2026-08-07).
  if (spi_hub_ != nullptr && spi_hub_->status().valid) {
    const auto &st = spi_hub_->status();
    // POWER must be checked: the off-state replies MIRROR the live store
    // (mode/set/fan keep tracking even while off - discovered 2026-08-07,
    // which also debunked the "0x20 off-signature"), so field equality alone
    // can confirm a command the unit received but never woke up for.
    if (tx_target_off_) return !st.power;
    if (!st.power) return false;
    if (st.set_temp != tx_temp_) return false;
    uint8_t want_fan;
    switch (tx_fan_) {
      case IRFan::Auto: want_fan = 0x80; break;
      case IRFan::Low: want_fan = 0x8C; break;
      case IRFan::Med: want_fan = 0x84; break;
      case IRFan::High: want_fan = 0x88; break;
      default: want_fan = 0x80; break;
    }
    if (st.fan != want_fan) return false;
    uint8_t want_mode;
    switch (tx_mode_) {
      case IRMode::Heat: want_mode = 0x20; break;
      case IRMode::Cool: want_mode = 0x80; break;
      case IRMode::FanOnly: want_mode = 0xC0; break;
      default: return true;  // toggle etc. - nothing to verify field-wise
    }
    return st.mode == want_mode;
  }
  // Fallback: sensor objects (no hub wired in config).
  if (set_temp_sensor_ != nullptr && set_temp_sensor_->has_state()) {
    if ((uint8_t) set_temp_sensor_->state != tx_temp_) return false;
  }
  if (fan_sensor_ != nullptr && fan_sensor_->has_state()) {
    if (fan_sensor_->state != std::string(ir_fan_to_string_(tx_fan_))) return false;
  }
  const char *want_mode = ir_mode_to_string_(tx_mode_);
  if (want_mode[0] != '\0' && mode_sensor_ != nullptr && mode_sensor_->has_state()) {
    if (mode_sensor_->state != std::string(want_mode)) return false;
  }
  return true;
}

IRFan BreezyClimate::climate_fan_to_ir_fan_(climate::ClimateFanMode fan) {
  switch (fan) {
    case climate::CLIMATE_FAN_LOW: return IRFan::Low;
    case climate::CLIMATE_FAN_MEDIUM: return IRFan::Med;
    case climate::CLIMATE_FAN_HIGH: return IRFan::High;
    default: return IRFan::Auto;
  }
}

IRMode BreezyClimate::climate_mode_to_ir_mode_(climate::ClimateMode mode) {
  switch (mode) {
    case climate::CLIMATE_MODE_HEAT: return IRMode::Heat;
    case climate::CLIMATE_MODE_COOL: return IRMode::Cool;
    case climate::CLIMATE_MODE_FAN_ONLY: return IRMode::FanOnly;
    default: return IRMode::Heat;
  }
}

// IR Encoding implementation (ported from ir_encoder.cpp)

uint8_t BreezyClimate::get_byte0_(IRMode mode, IRFan fan) {
  uint8_t fan_idx = static_cast<uint8_t>(fan);
  switch (mode) {
    case IRMode::Heat: return HEAT_FAN_BYTES[fan_idx];
    case IRMode::Cool: return COOL_FAN_BYTES[fan_idx];
    case IRMode::FanOnly: return FAN_ONLY_BYTES[fan_idx];
    case IRMode::PowerToggle: return POWER_TOGGLE_BYTES[fan_idx];
    default: return 0;
  }
}

uint8_t BreezyClimate::get_temp_byte_(uint8_t temp) {
  if (temp < 16 || temp > 30) return 0;
  return TEMP_BYTES[temp - 16];
}

void BreezyClimate::byte_to_bits_msb_(uint8_t byte, uint8_t *bits) {
  for (int i = 0; i < 8; i++) {
    bits[i] = (byte >> (7 - i)) & 1;
  }
}

// Helper macro for appending symbols
#define APPEND_SYMBOL(m, s, pos) do { \
  if ((pos) + 2 > max_len) return pos; \
  out[(pos)++] = (m); \
  out[(pos)++] = (s); \
} while(0)

#define APPEND_SS(pos) APPEND_SYMBOL(SS_MARK, SS_SPACE, pos)
#define APPEND_LS(pos) APPEND_SYMBOL(LS_MARK, LS_SPACE, pos)
#define APPEND_LM(pos) APPEND_SYMBOL(LM_MARK, LM_SPACE, pos)
#define APPEND_LL(pos) APPEND_SYMBOL(LL_MARK, LL_SPACE, pos)

size_t BreezyClimate::encode_bits_(const uint8_t *bits, size_t num_bits, Encoding encoding,
                                   uint16_t *out, size_t max_len, State initial) {
  size_t pos = 0;
  size_t i = 0;
  State state = initial;

  // NoLl encoding: pure LS/LM alternation
  if (encoding == Encoding::NoLl) {
    bool use_ls = true;
    for (i = 0; i < num_bits; i++) {
      if (bits[i] == 0) {
        APPEND_SS(pos);
        use_ls = true;
      } else {
        if (use_ls) {
          APPEND_LS(pos);
          use_ls = false;
        } else {
          APPEND_LM(pos);
          use_ls = true;
        }
      }
    }
    return pos;
  }

  // Standard and LsStart encodings
  bool allow_ll_from_fresh = (encoding == Encoding::Standard);

  while (i < num_bits) {
    if (bits[i] == 0) {
      APPEND_SS(pos);
      if (state == State::Fresh) {
        state = State::AfterLM;
      } else if (state == State::AfterLL || state == State::AfterLS) {
        state = State::After0;
      }
      i++;
    } else {
      bool next_is_1 = (i + 1 < num_bits) && (bits[i + 1] == 1);

      switch (state) {
        case State::Fresh:
          if (allow_ll_from_fresh && next_is_1) {
            APPEND_LL(pos);
            state = State::AfterLL;
            i += 2;
          } else {
            APPEND_LS(pos);
            state = State::AfterLS;
            i++;
          }
          break;

        case State::After0:
        case State::AfterLS:
          if (next_is_1) {
            APPEND_LL(pos);
            state = State::AfterLL;
            i += 2;
          } else {
            APPEND_LM(pos);
            state = State::AfterLM;
            i++;
          }
          break;

        case State::AfterLL:
          if (next_is_1) {
            APPEND_LL(pos);
            state = State::AfterLL;
            i += 2;
          } else {
            APPEND_LM(pos);
            state = State::AfterLM;
            i++;
          }
          break;

        case State::AfterLM:
          APPEND_LS(pos);
          state = State::AfterLS;
          i++;
          break;
      }
    }
  }
  return pos;
}

size_t BreezyClimate::generate_frame_(uint8_t byte0, uint8_t byte1, uint8_t byte2, uint8_t byte3,
                                       uint8_t extra_bits, uint16_t header_space, Encoding encoding,
                                       uint16_t *out, size_t max_len) {
  size_t pos = 0;

  // Header
  if (pos + 2 > max_len) return 0;
  out[pos++] = HEADER_MARK;
  out[pos++] = header_space;

  // Build bits array
  uint8_t bits[32];
  size_t bit_count = 0;

  uint8_t bytes[4] = {byte0, byte1, byte2, byte3};
  for (int b = 0; b < 3; b++) {
    byte_to_bits_msb_(bytes[b], &bits[bit_count]);
    bit_count += 8;
  }
  // Add extra bits from byte3 (MSB first)
  for (int j = extra_bits - 1; j >= 0; j--) {
    bits[bit_count++] = (byte3 >> j) & 1;
  }

  // Encode bits
  pos += encode_bits_(bits, bit_count, encoding, out + pos, max_len - pos);
  return pos;
}

size_t BreezyClimate::generate_24c_frame_(uint8_t byte0, uint16_t header_space,
                                           Encoding byte0_encoding,
                                           uint16_t *out, size_t max_len) {
  size_t pos = 0;

  // Header
  if (pos + 2 > max_len) return 0;
  out[pos++] = HEADER_MARK;
  out[pos++] = header_space;

  // byte0 with the MODE's normal encoding. This used to hardcode Standard,
  // which happens to match LsStart for byte0s without adjacent 1s (cool 0x24)
  // but spells fan-only 0xFC as LL LL LL where the remote sends LS LL LL LM -
  // which is why this frame was a no-op in fan-only mode until 2026-08-04.
  uint8_t bits0[8];
  byte_to_bits_msb_(byte0, bits0);
  pos += encode_bits_(bits0, 8, byte0_encoding, out + pos, max_len - pos);

  // byte1 = 0x78 = 01111000 with no-LL for the 1111 sequence
  APPEND_SS(pos);   // 0
  APPEND_LS(pos);   // 1
  APPEND_LM(pos);   // 1
  APPEND_LS(pos);   // 1
  APPEND_LM(pos);   // 1
  APPEND_SS(pos);   // 0
  APPEND_SS(pos);   // 0
  APPEND_SS(pos);   // 0

  // byte2 = 0x00 (8 zeros)
  for (int j = 0; j < 8; j++) {
    APPEND_SS(pos);
  }

  // byte3 lower 6 bits = 000011 (30 bits total)
  APPEND_SS(pos);
  APPEND_SS(pos);
  APPEND_SS(pos);
  APPEND_SS(pos);
  APPEND_LS(pos);
  APPEND_LM(pos);

  return pos;
}

size_t BreezyClimate::ir_encode_(IRMode mode, uint8_t temp, IRFan fan, uint16_t *out, size_t max_len) {
  if (temp < 16 || temp > 30) return 0;
  if (max_len < 200) return 0;

  // heat/med is NOT a byte code (2026-08-14 remote capture, 3 frames
  // agree): it is the 7-BIT code 0111100 in a 30-bit frame - heat/high
  // (0x78) truncated one bit, length-disambiguated like 24C-vs-25C. The
  // old 0x5C table entry is not what the remote sends (per the code
  // composition, 5C is likely AUTO/med from a mislabeled archive capture).
  // Emit the captured spelling verbatim: SS LS LM LS LM, then the temp/
  // zeros/tail region on the Standard machine seeded AfterLM.
  if (mode == IRMode::Heat && fan == IRFan::Med) {
    uint8_t tb = get_temp_byte_(temp);
    uint8_t rest[26];
    size_t rn = 0;
    rest[rn++] = 0;  // code bits 5-6 (0111100 = 5 long-ish symbols + two
    rest[rn++] = 0;  // trailing zeros, spelled by the machine like the rest)
    for (int i = 7; i >= 0; i--) rest[rn++] = (tb >> i) & 1;
    for (int i = 0; i < 8; i++) rest[rn++] = 0;
    const char *tail = (temp == 24) ? "000011" : "0000011";
    for (const char *p = tail; *p; p++) rest[rn++] = *p - '0';
    size_t hm_pos = 0;
    for (int f = 0; f < 3; f++) {
      if (hm_pos + 12 > max_len) return 0;
      out[hm_pos++] = HEADER_MARK;
      out[hm_pos++] = (f == 0) ? HEADER_SPACE_HEAT : SEP_SPACE_HEAT;
      out[hm_pos++] = SS_MARK; out[hm_pos++] = SS_SPACE;
      out[hm_pos++] = LS_MARK; out[hm_pos++] = LS_SPACE;
      out[hm_pos++] = LM_MARK; out[hm_pos++] = LM_SPACE;
      out[hm_pos++] = LS_MARK; out[hm_pos++] = LS_SPACE;
      out[hm_pos++] = LM_MARK; out[hm_pos++] = LM_SPACE;
      hm_pos += encode_bits_(rest, rn, Encoding::Standard, out + hm_pos,
                             max_len - hm_pos, State::AfterLM);
    }
    if (hm_pos >= max_len) return 0;
    out[hm_pos++] = TRAIL;
    return hm_pos;
  }

  size_t pos = 0;
  uint8_t byte0 = get_byte0_(mode, fan);
  uint8_t byte1 = get_temp_byte_(temp);
  uint8_t byte2 = 0x00;
  uint8_t byte3 = 0x03;

  uint16_t header_space = (mode == IRMode::PowerToggle) ? HEADER_SPACE_PWR : HEADER_SPACE_HEAT;
  uint16_t sep_space = (mode == IRMode::PowerToggle) ? SEP_SPACE_PWR : SEP_SPACE_HEAT;
  uint8_t extra_bits = 7;

  // Determine encoding style
  Encoding encoding;
  switch (mode) {
    case IRMode::Heat:
    case IRMode::PowerToggle:
      encoding = Encoding::Standard;
      break;
    case IRMode::Cool:
    case IRMode::FanOnly:
      encoding = Encoding::LsStart;
      break;
    default:
      return 0;
  }

  // 24C is special: same byte1 as 25C (0x78), but the remote distinguishes it
  // by SPELLING - the 1111 run is rendered LS LM LS LM (never LL) and the
  // frame is one bit shorter (30 bits, 6-bit tail). Confirmed by remote
  // captures in cool AND fan-only (ir_commands.md), and validated on hardware
  // 2026-08-04 by synthesizing the frame over send_ir_raw before porting here.
  // The HVAC pattern-matches the waveform, not the decoded bits: sending the
  // same 30 bits spelled with LL (tried 2026-08-03) is rejected outright.
  // byte0 still uses the mode's normal encoding - see generate_24c_frame_.
  bool is_24c = (temp == 24);

  if (is_24c) {
    size_t frame_len = generate_24c_frame_(byte0, header_space, encoding, out, max_len);
    if (frame_len == 0) return 0;
    pos = frame_len;

    // Frame 2
    if (pos + 2 > max_len) return 0;
    out[pos++] = SEP_MARK;
    out[pos++] = sep_space;
    for (size_t j = 2; j < frame_len; j++) {
      if (pos >= max_len) return 0;
      out[pos++] = out[j];
    }

    // Frame 3
    if (pos + 2 > max_len) return 0;
    out[pos++] = SEP_MARK;
    out[pos++] = sep_space;
    for (size_t j = 2; j < frame_len; j++) {
      if (pos >= max_len) return 0;
      out[pos++] = out[j];
    }

    // Trail
    if (pos >= max_len) return 0;
    out[pos++] = TRAIL;
    return pos;
  }

  // Generate first frame
  size_t frame_len = generate_frame_(byte0, byte1, byte2, byte3,
                                     extra_bits, header_space, encoding,
                                     out, max_len);
  if (frame_len == 0) return 0;
  pos = frame_len;

  // Frame 2
  if (pos + 2 > max_len) return 0;
  out[pos++] = SEP_MARK;
  out[pos++] = sep_space;
  for (size_t j = 2; j < frame_len; j++) {
    if (pos >= max_len) return 0;
    out[pos++] = out[j];
  }

  // Frame 3
  if (pos + 2 > max_len) return 0;
  out[pos++] = SEP_MARK;
  out[pos++] = sep_space;
  for (size_t j = 2; j < frame_len; j++) {
    if (pos >= max_len) return 0;
    out[pos++] = out[j];
  }

  // Trail
  if (pos >= max_len) return 0;
  out[pos++] = TRAIL;

  return pos;
}

void BreezyClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Breezy Climate:");
  ESP_LOGCONFIG(TAG, "  IR: via remote_transmitter (RMT)");
}

}  // namespace breezy_climate
}  // namespace esphome
