#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace breezy_spi {

// Which bus dialect this tap listens to. RAW = the controller<->panel bus
// itself (33ms poll/response, ~76-bit 60 00 replies). SPLITTER = the flat
// 112-bit re-broadcast served downstream of the AC's "splitter" accessory,
// which is an active protocol translator. Same fields either way; framing,
// power detection and the room-temp encoding differ.
enum Dialect : uint8_t {
  DIALECT_RAW = 0,
  DIALECT_SPLITTER = 1,
};

// Decoded AC status from SPI bus
struct ACStatus {
  uint8_t raw[14];
  bool valid;
  uint32_t timestamp;

  bool power;
  uint8_t mode;       // 0x20=heat, 0x80=cool, 0xC0=fan
  uint8_t set_temp;
  uint8_t room_temp;
  uint8_t fan;        // 0x80=auto, 0x84=med, 0x88=high, 0x8C=low
  bool compressor;
};

class BreezySPIComponent : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_clk_pin(InternalGPIOPin *pin) { clk_pin_ = pin; }
  void set_data_pin(InternalGPIOPin *pin) { data_pin_ = pin; }
  void set_dialect(Dialect dialect) { dialect_ = dialect; }

  // Age of the last vote-accepted frame. Consumers that ACT on the decoded
  // state (closed-loop verification) must check this: our own IR
  // transmissions poison this device's decode for a variable stretch, and
  // acting on a stale reading causes retry storms (2026-08-07).
  uint32_t state_age_ms() const {
    return status_.valid ? (millis() - status_.timestamp) : UINT32_MAX;
  }
  // Direct access to the decoded state, bypassing the sensor-publish layer
  // (which lags up to one update_interval behind and caused a false-retry
  // race when verification checked freshness on status_ but values on the
  // not-yet-republished sensors).
  const ACStatus &status() const { return status_; }
  // Age of the last confirmed power reading. Since 2026-08-07 power comes
  // from state-frame byte2 bit 0x08 (same voted frames as the rest),
  // so this tracks state_age_ms() closely; kept as a separate accessor
  // because the climate component gates toggle decisions on it and the
  // sourcing may diverge again.
  uint32_t power_age_ms() const {
    return power_updated_at_ ? (millis() - power_updated_at_) : UINT32_MAX;
  }

  // Sensor setters
  void set_set_temperature_sensor(sensor::Sensor *sensor) { set_temp_sensor_ = sensor; }
  void set_room_temperature_sensor(sensor::Sensor *sensor) { room_temp_sensor_ = sensor; }
  void set_power_sensor(binary_sensor::BinarySensor *sensor) { power_sensor_ = sensor; }
  void set_compressor_sensor(binary_sensor::BinarySensor *sensor) { compressor_sensor_ = sensor; }
  void set_mode_sensor(text_sensor::TextSensor *sensor) { mode_sensor_ = sensor; }
  void set_fan_sensor(text_sensor::TextSensor *sensor) { fan_sensor_ = sensor; }

  // Get current status (for climate component)
  const ACStatus &get_status() const { return status_; }
  bool is_valid() const { return status_.valid; }

  // ISR handler (called from static ISR)
  void handle_clock_isr();

 protected:
  // 4-bit reversal - this protocol transmits nibbles LSB-first, so it shows
  // up in the temperature fields and in both frame checksums.
  static uint8_t br4_(uint8_t x) {
    return ((x & 1) << 3) | ((x & 2) << 1) | ((x & 4) >> 1) | ((x & 8) >> 3);
  }

  // The state frame's checksum (see PROTOCOL_BUS.md 3.1): ONE 8-bit sum,
  // mod 256, of the payload with each nibble bit-reversed (the bus is
  // LSB-first): bytes 1..7 whole, plus byte8's high nibble (the room
  // field's upper half) as the high nibble of a ninth, half-filled byte.
  // The header byte0 is excluded. The sum's low nibble is stored
  // bit-reversed in byte8's low nibble, its high nibble in byte9's high
  // nibble. So the two stored nibbles are NOT independent (the low sum
  // carries into the high one) and byte8's upper half IS covered.
  //
  // History: the 2026-08-14 derivation modelled two independent mod-16
  // nibble sums with a constant -2 seed. That is this formula restricted to
  // the archive's conditions (low sum < 16, room 24-27 so byte8's high
  // nibble was a constant 2, i.e. +4 after reversal; -6 + 4 = -2). It
  // broke on 2026-11-11 (heat season): heat/25/low with the compressor
  // running sums past 16, every frame was rejected for minutes, the decode
  // froze on OFF while the unit ran. The carry-only fix of 2026-11-12 then
  // broke the moment the room read 22 (byte8 high nibble 3). Cracked
  // byte-exact on two frames and confirmed on every repeated frame recorded.
  // This is a true validity oracle - it authenticates a frame outright.
  static bool checksums_ok_(const uint8_t *d) {
    uint8_t sum = 0;
    for (int i = 1; i < 8; i++) {
      sum += (uint8_t) ((br4_(d[i] >> 4) << 4) | br4_(d[i] & 0x0F));
    }
    sum += (uint8_t) (br4_(d[8] >> 4) << 4);
    return br4_(d[8] & 0x0F) == (sum & 0x0F) &&
           br4_(d[9] >> 4) == (sum >> 4);
  }

  void decode_status_();
  void publish_sensors_();
  const char *mode_to_string_(uint8_t mode);
  const char *fan_to_string_(uint8_t fan);

  InternalGPIOPin *clk_pin_{nullptr};
  InternalGPIOPin *data_pin_{nullptr};

  // Sensors
  sensor::Sensor *set_temp_sensor_{nullptr};
  sensor::Sensor *room_temp_sensor_{nullptr};
  binary_sensor::BinarySensor *power_sensor_{nullptr};
  binary_sensor::BinarySensor *compressor_sensor_{nullptr};
  text_sensor::TextSensor *mode_sensor_{nullptr};
  text_sensor::TextSensor *fan_sensor_{nullptr};

  // ISR timing state
  volatile uint32_t spi_last_bit_time_{0};

  Dialect dialect_{DIALECT_RAW};

  // Gap-delimited burst recorder (2026-08-04): records bursts exactly as they
  // arrive - preceding gap, bit length, payload - with no opinion about frame
  // length. This IS the frame path for the raw (no-splitter) bus dialect, and
  // doubles as the capture instrument that cracked it.
  static constexpr uint8_t CAP_RING = 12;
  static constexpr uint8_t CAP_BYTES = 40;  // up to 320 bits per burst
  struct CapBurst {
    uint32_t gap_us;
    uint16_t nbits;
    uint8_t data[CAP_BYTES];
  };
  volatile CapBurst cap_ring_[CAP_RING];
  volatile uint8_t cap_head_{0};   // ISR writes
  uint8_t cap_tail_{0};            // loop() reads
  volatile uint16_t cap_bits_{0};
  volatile uint8_t cap_cur_[CAP_BYTES];
  volatile uint32_t cap_gap_{0};

  // Health counters
  uint32_t diag_slip_rejects_{0};
  uint32_t diag_last_report_{0};

  // Inter-edge interval histogram, 21 buckets of 32µs (last = overflow).
  // Counts EVERY edge including debounce-rejected ones. A healthy bus is
  // bimodal: ~95% at 128-159µs plus a few % of sub-32µs runts.
  volatile uint32_t edge_hist_[21]{};

  // Throttle for the DEBUG frame sampler (<=2 bursts/s - see loop()).
  uint32_t last_sample_ms_{0};

  uint32_t power_updated_at_{0};

  // 2-consecutive-frame vote candidate (loop-context only, not ISR)
  uint8_t vote_frame_[14]{};

  // Decoded status
  ACStatus status_{};
};

}  // namespace breezy_spi
}  // namespace esphome
