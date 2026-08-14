#include "breezy_spi.h"
#include "esphome/core/log.h"
#include <Arduino.h>

namespace esphome {
namespace breezy_spi {

static const char *const TAG = "breezy_spi";

// Global instance for ISR
BreezySPIComponent *global_breezy_spi = nullptr;

// Static ISR handler
static void IRAM_ATTR spi_clock_isr() {
  if (global_breezy_spi != nullptr) {
    global_breezy_spi->handle_clock_isr();
  }
}

void BreezySPIComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Breezy SPI...");

  global_breezy_spi = this;

  clk_pin_->setup();
  data_pin_->setup();

  // Attach interrupt on CLK rising edge using Arduino API
  attachInterrupt(digitalPinToInterrupt(clk_pin_->get_pin()), spi_clock_isr, RISING);

  ESP_LOGCONFIG(TAG, "  CLK pin: GPIO%d", clk_pin_->get_pin());
  ESP_LOGCONFIG(TAG, "  DATA pin: GPIO%d", data_pin_->get_pin());
}

// RAW BUS DIALECT (2026-08-04, reverse-engineered after the permanent install
// removed the AC's "splitter" - actually an active protocol translator that
// re-served this bus flattened into 112-bit frames; everything before that
// date was decoded downstream of it and never saw the real bus).
//
// The raw bus runs a 33ms poll/response cycle:
//   - 113-bit master frames headed 95 5F (controller -> panel)
//   - short ~36-bit polls answered after a ~611us turnaround by ~76-bit
//     replies headed 60 00 (panel -> controller)
// The replies carry the OLD dialect's fields at the OLD byte offsets with the
// OLD encodings - the splitter was re-broadcasting them nearly verbatim. So
// this ISR just records gap-delimited bursts as they are; loop() picks out
// the replies. Frame grammar and field map validated against 13 labeled
// captures + live LCD ground truth, v4_bringup_notes.md 2026-08-04.
void IRAM_ATTR BreezySPIComponent::handle_clock_isr() {
  uint32_t now = micros();
  uint32_t elapsed = now - spi_last_bit_time_;

  // DIAGNOSTIC (2026-08-14): bucket EVERY edge interval before the guard, to
  // find out whether the guard is eating real clock edges - 40% of replies
  // arrive one bit short. Decode path below is untouched.
  if (elapsed < 640) {
    edge_hist_[elapsed >> 5]++;  // 20 buckets x 32µs
  } else {
    edge_hist_[20]++;
  }

  // Debounce: real clock edges arrive every ~140µs; anything sooner is a
  // glitch. 50µs let 60-130µs runts through, causing 1-bit frame slips
  // (verified against 25k captured frames, 2026-08-02).
  if (elapsed < 90) return;
  spi_last_bit_time_ = now;

  // Gap-delimited burst recorder - no opinion about frame length; the bus has
  // 36/76/113-bit bursts and mis-guessing here is how the last decoder died.
  if (elapsed > 500) {
    if (cap_bits_ > 0) {
      uint8_t h = cap_head_;
      cap_ring_[h].gap_us = cap_gap_;
      cap_ring_[h].nbits = cap_bits_;
      uint16_t nbytes = (cap_bits_ + 7) / 8;
      for (uint16_t i = 0; i < nbytes && i < CAP_BYTES; i++)
        cap_ring_[h].data[i] = cap_cur_[i];
      cap_head_ = (h + 1) % CAP_RING;
    }
    cap_gap_ = elapsed;
    cap_bits_ = 0;
  }
  if (cap_bits_ < CAP_BYTES * 8) {
    uint16_t idx = cap_bits_ >> 3;
    if ((cap_bits_ & 7) == 0) cap_cur_[idx] = 0;
    cap_cur_[idx] |= data_pin_->digital_read() << (7 - (cap_bits_ & 7));
  }
  cap_bits_++;
}

void BreezySPIComponent::loop() {
  // Health heartbeat at DEBUG (1 line / 5s): how stale is the decoded state,
  // and how much is the slip filter rejecting. last_frame_age is the number
  // that catches a starved pipeline - the stale-witness failure mode of
  // 2026-08-04 would have been visible here in seconds instead of hours.
  uint32_t now_ms = millis();
  if (now_ms - diag_last_report_ >= 5000) {
    diag_last_report_ = now_ms;
    ESP_LOGD(TAG, "health: slip_rejects=%u last_frame_age=%ums",
             diag_slip_rejects_, status_.valid ? now_ms - status_.timestamp : 0);
    // Inter-edge interval histogram, 32µs buckets. Cheap (one line / 5s) and
    // it is the instrument that identified log-flood frame corruption on
    // 2026-08-14: a healthy bus is bimodal - ~95% of edges in the 128-159µs
    // bucket (the real clock) and a few % of sub-32µs runts. Any population
    // appearing between those, or a collapse of the 128-159 peak, means
    // edges are being missed and frames are losing bits.
    char hb[256];
    size_t hp = 0;
    for (int i = 0; i < 21 && hp < sizeof(hb) - 16; i++) {
      if (edge_hist_[i])
        hp += snprintf(hb + hp, sizeof(hb) - hp, "%d:%u ", i * 32, edge_hist_[i]);
    }
    ESP_LOGD(TAG, "edges: %s", hb);
  }

  // Drain the burst ring: pick out panel replies, filter slips, vote, decode.
  while (cap_tail_ != cap_head_) {
    CapBurst b;
    b.gap_us = cap_ring_[cap_tail_].gap_us;
    b.nbits = cap_ring_[cap_tail_].nbits;
    uint16_t nbytes = (b.nbits + 7) / 8;
    if (nbytes > CAP_BYTES) nbytes = CAP_BYTES;
    for (uint16_t i = 0; i < nbytes; i++) b.data[i] = cap_ring_[cap_tail_].data[i];
    cap_tail_ = (cap_tail_ + 1) % CAP_RING;

    // TWO capture instruments, and the choice matters enormously:
    //
    // 1. THROTTLED SAMPLER (DEBUG, <=2 bursts/s) - use this one. Low enough
    //    volume that it does not disturb the ISR.
    // 2. FULL BURST STREAM (VERBOSE, every burst) - THE OBSERVER EFFECT
    //    TRAP. Logging ~100 bursts/s blocks the capture ISR long enough to
    //    miss clock edges, so frames silently LOSE BITS: everything after
    //    the drop shifts left, and a drop inside a run of zeros still looks
    //    structurally legal. Measured 2026-08-14: 40% of replies corrupted
    //    under VERBOSE vs 0.1% at DEBUG. Every "mystery frame" in this
    //    project's history came from VERBOSE-era data (the 75-bit
    //    "subtype-B", the 112-bit master variant, the bogus DRY=0x40 mode
    //    code - all shifted frames). If you must use it, treat the output
    //    as noisy and majority-vote everything.
    uint32_t sample_now = millis();
    bool sample_it = (sample_now - last_sample_ms_) >= 500;
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
    const bool build_hex = true;
#else
    const bool build_hex = sample_it;
#endif
    if (build_hex) {
      char hex[CAP_BYTES * 3 + 1];
      for (uint16_t i = 0; i < nbytes; i++) sprintf(hex + i * 3, "%02X ", b.data[i]);
      hex[nbytes * 3] = 0;
      ESP_LOGV(TAG, "burst: gap=%uus nbits=%u %s", b.gap_us, b.nbits, hex);
      if (sample_it) {
        last_sample_ms_ = sample_now;
        ESP_LOGD(TAG, "sample: nbits=%u %s", b.nbits, hex);
      }
    }

    // Dialect-dependent frame acceptance.
    if (dialect_ == DIALECT_SPLITTER) {
      // Splitter dialect: flat 112-bit frames, header 0x60 or 0xE0, byte1 00.
      // This is the bench-proven grammar (pre-2026-08-04); the vote covers
      // bytes 2..13 as it always did there.
      if (b.nbits != 112) continue;
      if (!((b.data[0] == 0x60 || b.data[0] == 0xE0) && b.data[1] == 0x00)) continue;
      bool match = true;
      for (int i = 2; i < 14; i++) {
        if (b.data[i] != vote_frame_[i]) match = false;
      }
      for (int i = 0; i < 14; i++) vote_frame_[i] = b.data[i];
      if (!match) continue;
      for (int i = 0; i < 14; i++) status_.raw[i] = b.data[i];
      status_.timestamp = millis();
      status_.valid = true;
      decode_status_();
      continue;
    }

    // THERE IS NO "MASTER FRAME" (settled 2026-08-14). The bus carries just
    // two things: the 36-bit LCD STATUS POLL (controller opens it, the panel
    // writes into it - the line is wired-AND; what the panel reports there
    // is not decoded) and the 76-bit CONTROLLER STATUS below. What we
    // called a 111-114 bit master is
    // simply a poll and a controller status arriving less than 500µs apart,
    // so the burst recorder glues them into one record - the controller
    // status begins at bit 37 (36-bit poll + 1). It decodes and passes both
    // checksums every time (6/6 on clean capture). Un-glue it here rather
    // than discard it: glued pairs get MORE common exactly when the bus is
    // busy, which is when frames matter most. (Graveyard: the "master
    // byte10 = power" rule of 2026-08-06 was this frame's own byte2 power
    // bit seen at a shifted offset; the 112-bit "variant" was a glued pair
    // with a log-flood bit drop.)
    uint8_t unglued[10];
    if (b.nbits >= 108 && b.nbits <= 118) {
      for (int start = 36; start <= 38; start++) {
        if (start + 76 > b.nbits) break;
        for (int i = 0; i < 10; i++) {
          uint8_t v = 0;
          for (int k = 0; k < 8; k++) {
            int bit = start + i * 8 + k;
            if (bit < b.nbits)
              v |= ((b.data[bit >> 3] >> (7 - (bit & 7))) & 1) << (7 - k);
          }
          unglued[i] = v;
        }
        if (unglued[0] == 0x60 && unglued[1] == 0x00 &&
            checksums_ok_(unglued)) {
          for (int i = 0; i < 10; i++) b.data[i] = unglued[i];
          b.nbits = 76;
          break;
        }
      }
    }

    // Raw dialect - THE STATE FRAME: exactly 76 bits, header 60 00,
    // controller-authored, carrying the complete unit state. The only other
    // thing on this bus is the 36-bit LCD status poll, ignored here.
    if (b.nbits != 76) { if (b.data[0] == 0x60 && b.data[1] == 0x00) diag_slip_rejects_++; continue; }
    if (b.data[0] != 0x60 || b.data[1] != 0x00) continue;

    // THE FRAME CARRIES TWO REAL CHECKSUMS (derived 2026-08-14 from 1.2M
    // archived frames; verified 91/91 on clean live capture). Read the 76
    // bits LSB-first as nineteen 4-bit groups: the last two groups are
    // sums of the preceding groups mod 16. In byte terms, with br4() a
    // 4-bit reversal:
    //     br4(byte8 & 0x0F) == sum(br4(byte[i] & 0x0F), i=0..7) % 16
    //     br4(byte9 >> 4)   == (sum(br4(byte[i] >> 4), i=0..7) - 2) % 16
    // This REPLACES the old structural slip filter (byte3/mode/temp-range
    // guesses), which both rejected legal frames and passed corrupt ones -
    // the phantom "DRY = 0x40" mode code came through it. Blind spot to
    // respect: the room field's upper bits (byte8 bits 4-7) sit in no
    // checksum, so the room still needs its own repeat vote below.
    if (!checksums_ok_(b.data)) { diag_slip_rejects_++; continue; }

    // 2-consecutive vote, split in two. Bytes 2..6 (compressor/set/fan/mode)
    // vote together: a frame must repeat before it can update state, so
    // single-frame glitches never reach the sensors. Byte 7 (room temp) votes
    // SEPARATELY: it legitimately dithers between adjacent codes when the room
    // sits on a quantization boundary, and including it in the main vote
    // starved the whole pipeline for ~19s at a time (measured 2026-08-05) -
    // which broke command verification while the room was between degrees.
    bool match = true;
    for (int i = 2; i < 7; i++) {
      if (i == 3) continue;  // byte3 bit7 flaps while ON; excluded from the vote
      if (b.data[i] != vote_frame_[i]) match = false;
    }
    // Room votes on BOTH its halves: byte8's high nibble is the field's
    // upper bits and is covered by neither checksum, so a bit dropped there
    // is invisible to the filter and would decode as a wild temperature.
    bool room_match = (b.data[7] == vote_frame_[7]) &&
                      ((b.data[8] & 0xF0) == (vote_frame_[8] & 0xF0));
    for (int i = 0; i < 10; i++) vote_frame_[i] = b.data[i];
    if (!match) continue;  // candidate stored; wait for a confirming repeat

    for (int i = 2; i < 7; i++) status_.raw[i] = b.data[i];
    status_.raw[0] = b.data[0];
    status_.raw[1] = b.data[1];
    if (room_match) status_.raw[7] = b.data[7];  // room updates on its own vote
    status_.raw[8] = b.data[8];
    status_.raw[9] = b.data[9];
    for (int i = 10; i < 14; i++) status_.raw[i] = 0;
    status_.timestamp = millis();
    power_updated_at_ = millis();  // power rides in byte2 of this same frame
    status_.valid = true;
    decode_status_();
  }
}

void BreezySPIComponent::update() {
  // Publish sensor values on polling interval
  if (status_.valid) {
    publish_sensors_();
  }
}

void BreezySPIComponent::decode_status_() {
  uint8_t *raw = status_.raw;

  // Byte 2 is a BITFIELD (OFF/ON full-bus diff, 2026-08-07, unanimous over
  // ~550 replies): bit 0x08 = power, bit 0x02 = compressor running. The old
  // "0x0A = compressor" reading was both bits set - the only combination we
  // had ever correlated. Mode/set/fan (bytes 4-6) mirror the live store even
  // while off, but byte2 does NOT - it is the real power flag.
  status_.compressor = (raw[2] & 0x02) != 0;

  if (dialect_ == DIALECT_SPLITTER) {
    // Splitter dialect: byte2 != 0 meant power on - bench-proven.
    status_.power = (raw[2] != 0x00);
  } else {
    status_.power = (raw[2] & 0x08) != 0;
  }

  // Byte 4: set temp (bit-reversed nibble + 16) - identical to the splitter
  // dialect; validated live at 16/22/23/24/27.
  uint8_t b4 = raw[4];
  uint8_t rev = 0;
  for (int i = 0; i < 4; i++) {
    rev |= ((b4 >> i) & 1) << (3 - i);
  }
  status_.set_temp = rev + 16;

  // Byte 5: fan - identical to the splitter dialect (80 auto / 84 med /
  // 88 high / 8C low); auto, low, high validated live.
  status_.fan = raw[5];

  // Byte 6: mode - identical to the splitter dialect (20 heat / 80 cool /
  // C0 fan_only), with the caveat that 0x20 is also what an OFF unit reports.
  status_.mode = raw[6];

  // Room temp on the raw bus is a FULL 8-BIT FIELD spanning byte7's low
  // nibble and byte8's high nibble, transmitted LSB-first, biased by 40:
  //     room = br4(byte7 & 0x0F) | (br4(byte8 >> 4) << 4)   ... minus 40
  // Derived 2026-08-14 from 1.2M archived frames; matches the independent
  // oracle exactly on 97.8% of denoised frames and within 1C on 99.99%.
  // The old 5-bit form (8 + br4 + 16*(1-byte8.bit7)) is the algebraic
  // special case for 16-31C and silently misreads anything above 31C, so
  // it is retired. Byte8's high nibble carries NO checksum protection
  // (see the frame filter), hence the separate repeat vote for the room.
  uint8_t b7 = raw[7];
  if (dialect_ == DIALECT_RAW) {
    int room = (int) (br4_(b7 & 0x0F) | (br4_(raw[8] >> 4) << 4)) - 40;
    if (room >= 0 && room <= 60) {
      status_.room_temp = (uint8_t) room;
    } else {
      status_.room_temp = 0;  // outside sane range - publish nothing
    }
  } else if (dialect_ == DIALECT_SPLITTER) {
    // Splitter dialect: 2+2-bit encoding of the TRANSLATED byte7,
    // bench-proven and still tracking the LCD.
    uint8_t low_bits = b7 & 0x03;
    uint8_t high_bits = (b7 >> 2) & 0x03;
    uint8_t rev_high = ((high_bits & 1) << 1) | ((high_bits >> 1) & 1);
    if (low_bits == 0x01) {
      status_.room_temp = 16 + rev_high;
    } else if (low_bits == 0x03) {
      status_.room_temp = 20 + rev_high;
    } else if (low_bits == 0x00) {
      status_.room_temp = 24 + rev_high;
    } else if (low_bits == 0x02) {
      status_.room_temp = 28 + rev_high;
    } else {
      status_.room_temp = 0;  // Unknown
    }
  } else {
    status_.room_temp = 0;  // Unknown - see above
  }
}

void BreezySPIComponent::publish_sensors_() {
  if (set_temp_sensor_ != nullptr) {
    set_temp_sensor_->publish_state(status_.set_temp);
  }
  if (room_temp_sensor_ != nullptr && status_.room_temp != 0) {
    // room_temp 0 = not decodable (raw dialect until the real field is found)
    room_temp_sensor_->publish_state(status_.room_temp);
  }
  if (power_sensor_ != nullptr) {
    power_sensor_->publish_state(status_.power);
  }
  if (compressor_sensor_ != nullptr) {
    compressor_sensor_->publish_state(status_.compressor);
  }
  if (mode_sensor_ != nullptr) {
    mode_sensor_->publish_state(mode_to_string_(status_.mode));
  }
  if (fan_sensor_ != nullptr) {
    fan_sensor_->publish_state(fan_to_string_(status_.fan));
  }
}

const char *BreezySPIComponent::mode_to_string_(uint8_t mode) {
  switch (mode) {
    case 0x20: return "heat";
    case 0x80: return "cool";
    case 0xC0: return "fan_only";
    default: return "unknown";
  }
}

const char *BreezySPIComponent::fan_to_string_(uint8_t fan) {
  switch (fan) {
    case 0x80: return "auto";
    case 0x84: return "medium";
    case 0x88: return "high";
    case 0x8C: return "low";
    default: return "unknown";
  }
}

void BreezySPIComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Breezy SPI:");
  ESP_LOGCONFIG(TAG, "  CLK Pin: GPIO%d", clk_pin_->get_pin());
  ESP_LOGCONFIG(TAG, "  DATA Pin: GPIO%d", data_pin_->get_pin());
  ESP_LOGCONFIG(TAG, "  Dialect: %s", dialect_ == DIALECT_SPLITTER ? "splitter" : "raw");
  LOG_SENSOR("  ", "Set Temperature", set_temp_sensor_);
  LOG_SENSOR("  ", "Room Temperature", room_temp_sensor_);
  LOG_BINARY_SENSOR("  ", "Power", power_sensor_);
  LOG_BINARY_SENSOR("  ", "Compressor", compressor_sensor_);
  LOG_TEXT_SENSOR("  ", "Mode", mode_sensor_);
  LOG_TEXT_SENSOR("  ", "Fan", fan_sensor_);
}

}  // namespace breezy_spi
}  // namespace esphome
