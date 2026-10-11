/*
 * LiFiDecoder.h — light-level slicer and frame decoder for the LiFi receiver.
 *
 * Plain C++ with no Arduino dependencies, so the same code runs on the ESP32
 * and in the host-side simulation in ../tests.
 *
 * Protocol (must match transmitter_nano.ino):
 *   PREAMBLE | LEN | PAYLOAD[LEN] | CHECKSUM | END
 *   Preamble ON 80 ms, bit '1' ON 30 ms, bit '0' ON 10 ms, gaps OFF 15 ms,
 *   end-of-frame OFF 100 ms. Bytes MSB first. CHECKSUM = XOR(LEN, payload...).
 */
#pragma once

#include <stdint.h>
#include <string.h>

namespace lifi {

const uint8_t MAX_PAYLOAD = 32;
const uint8_t MAX_BYTES   = MAX_PAYLOAD + 2;  // LEN + payload + CHECKSUM

// ---------- Transmitter timing (ms) — must match transmitter_nano.ino ----------
// Two profiles; select the same one on both boards with LIFI_PROFILE.
#define LIFI_PROFILE_FAST     0  // spec timing, ~29 bit/s: needs a fast sensor/bright LED
#define LIFI_PROFILE_LDR_SAFE 1  // ~9 bit/s: works with slow CdS LDRs (fall time <= 40 ms)

#ifndef LIFI_PROFILE
#define LIFI_PROFILE LIFI_PROFILE_FAST
#endif

struct Timing {
#if LIFI_PROFILE == LIFI_PROFILE_LDR_SAFE
  uint16_t preambleOn  = 240;
  uint16_t preambleOff = 120;
  uint16_t oneOn       = 100;
  uint16_t zeroOn      = 30;
  uint16_t bitOff      = 50;
  uint16_t endOff      = 300;
#else
  uint16_t preambleOn  = 80;
  uint16_t preambleOff = 40;
  uint16_t oneOn       = 30;
  uint16_t zeroOn      = 10;
  uint16_t bitOff      = 15;
  uint16_t endOff      = 100;
#endif

  // Decision points derived from the above (midpoints between symbols).
  uint32_t oneMin()      const { return (oneOn + zeroOn) / 2; }           // '1' vs '0'
  uint32_t preambleMin() const { return (preambleOn + oneOn) / 2; }       // preamble vs '1'
  uint32_t preambleMax() const { return (uint32_t)preambleOn * 4; }       // steady light
  uint32_t endGap() const {                                               // frame over
    uint16_t longestGap = preambleOff > bitOff ? preambleOff : bitOff;
    return longestGap + (endOff - longestGap) / 2;
  }
};

const uint32_t GLITCH_MAX_MS = 3;  // ON pulses shorter than this are noise

enum FrameStatus : uint8_t {
  FRAME_OK = 0,
  FRAME_BAD_CHECKSUM,
  FRAME_BAD_LENGTH,
  FRAME_TRUNCATED,
};

inline const char* statusName(FrameStatus s) {
  switch (s) {
    case FRAME_OK:           return "ok";
    case FRAME_BAD_CHECKSUM: return "checksum";
    case FRAME_BAD_LENGTH:   return "length";
    default:                 return "truncated";
  }
}

struct Frame {
  FrameStatus status;
  uint8_t len;                    // payload bytes actually received
  char text[MAX_PAYLOAD + 1];
  uint32_t durationMs;            // preamble start -> last bit
  uint32_t endMs;
};

// ---------------------------------------------------------------------------
// Slicer: turns a raw ADC value into ON/OFF using a threshold with hysteresis.
// Manual mode: threshold comes from the calibration pot.
// Auto mode:   threshold sits between a tracked ambient floor and LED peak.
// ---------------------------------------------------------------------------
class Slicer {
 public:
  // Fraction of the floor->peak span used as the auto threshold. Halfway
  // balances a slow rise (short pulses look shorter) against a slow fall
  // (pulses look longer); see tests/link_sim.cpp.
  float autoFraction = 0.5f;
  int   minContrast  = 250;    // auto mode: smaller span = "no signal" (ADC counts)
  int   manualHyst   = 30;
  float trackRate    = 0.0002f;  // per sample; ~5 s time constant at 1 kHz

  int  threshold = 0;
  int  hyst      = 0;
  bool autoMode  = true;
  bool on        = false;
  bool hasSignal = false;

  int floorLevel() const { return (int)floor_; }
  int peakLevel()  const { return (int)peak_; }

  // threshold < 0 selects auto mode.
  bool update(int v, int manualThreshold) {
    // Instant attack, slow release, so the levels follow the real extremes.
    if (!init_) { floor_ = peak_ = (float)v; init_ = true; }
    if (v < floor_) floor_ = (float)v; else floor_ += (v - floor_) * trackRate;
    if (v > peak_)  peak_  = (float)v; else peak_  += (v - peak_)  * trackRate;

    autoMode = manualThreshold < 0;
    if (autoMode) {
      int span = (int)(peak_ - floor_);
      hasSignal = span >= minContrast;
      threshold = (int)(floor_ + span * autoFraction);
      hyst = span / 10 > 15 ? span / 10 : 15;
      if (!hasSignal) { on = false; return on; }
    } else {
      hasSignal = true;
      threshold = manualThreshold;
      hyst = manualHyst;
    }

    on = on ? (v > threshold - hyst) : (v > threshold + hyst);
    return on;
  }

 private:
  float floor_ = 0, peak_ = 0;
  bool init_ = false;
};

// ---------------------------------------------------------------------------
// Decoder: measures ON-pulse widths and assembles frames.
// Call update() once per sample with the slicer output.
// ---------------------------------------------------------------------------
class Decoder {
 public:
  Timing timing;
  uint32_t glitches = 0;

  // How much longer the sensor makes each ON pulse look, measured on the
  // last preamble (a slow LDR stays "lit" a while after the LED turns off).
  // Subtracted from every bit width before classifying it.
  int32_t stretchMs() const { return stretch_; }

  bool inFrame() const { return inFrame_; }
  uint8_t bytesReceived() const { return nBytes_; }
  uint8_t bitsInByte() const { return nBits_; }
  uint8_t expectedLen() const { return nBytes_ > 0 ? bytes_[0] : 0; }

  // Payload bytes received so far in the current frame (for live display).
  void partialText(char* out) const {
    uint8_t n = 0;
    if (inFrame_) {
      for (uint8_t i = 1; i < nBytes_ && n < MAX_PAYLOAD; i++) out[n++] = (char)bytes_[i];
    }
    out[n] = '\0';
  }

  // Returns true when a frame finished (successfully or not); fills `out`.
  bool update(bool lightOn, uint32_t nowMs, Frame& out) {
    bool done = false;

    if (lightOn && !wasOn_) {
      onStart_ = nowMs;
    } else if (!lightOn && wasOn_) {
      offStart_ = nowMs;
      done = handlePulse(nowMs - onStart_, nowMs, out);
    } else if (!lightOn && inFrame_ && nowMs - offStart_ > timing.endGap()) {
      done = finish(FRAME_TRUNCATED, nowMs, out);
    }

    wasOn_ = lightOn;
    return done;
  }

 private:
  bool handlePulse(uint32_t width, uint32_t nowMs, Frame& out) {
    if (width < GLITCH_MAX_MS) { glitches++; return false; }

    if (width > timing.preambleMax()) {
      // Steady light, not data: drop whatever we had.
      if (inFrame_ && nBytes_ > 0) return finish(FRAME_TRUNCATED, nowMs, out);
      inFrame_ = false;
      return false;
    }

    if (width >= timing.preambleMin()) {
      bool reported = inFrame_ && nBytes_ > 0 && finish(FRAME_TRUNCATED, nowMs, out);
      inFrame_ = true;
      frameStart_ = nowMs - width;
      // The preamble's true width is known, so the excess is sensor lag.
      int32_t s = (int32_t)width - (int32_t)timing.preambleOn;
      int32_t lo = -(int32_t)timing.zeroOn / 2, hi = (int32_t)timing.preambleOn / 2;
      stretch_ = s < lo ? lo : (s > hi ? hi : s);
      nBytes_ = nBits_ = cur_ = 0;
      return reported;
    }

    if (!inFrame_) return false;

    int32_t corrected = (int32_t)width - stretch_;
    cur_ = (uint8_t)((cur_ << 1) | (corrected >= (int32_t)timing.oneMin() ? 1 : 0));
    if (++nBits_ < 8) return false;

    bytes_[nBytes_++] = cur_;
    nBits_ = cur_ = 0;

    if (nBytes_ == 1) {
      if (bytes_[0] == 0 || bytes_[0] > MAX_PAYLOAD) return finish(FRAME_BAD_LENGTH, nowMs, out);
      return false;
    }
    if (nBytes_ == bytes_[0] + 2) {
      uint8_t chk = 0;
      for (uint8_t i = 0; i < nBytes_ - 1; i++) chk ^= bytes_[i];
      return finish(chk == bytes_[nBytes_ - 1] ? FRAME_OK : FRAME_BAD_CHECKSUM, nowMs, out);
    }
    return false;
  }

  bool finish(FrameStatus st, uint32_t nowMs, Frame& out) {
    out.status = st;
    out.len = 0;
    // Payload = bytes after LEN, excluding the checksum once it has arrived.
    uint8_t payloadEnd = nBytes_;
    if (nBytes_ >= 2 && nBytes_ == bytes_[0] + 2) payloadEnd = nBytes_ - 1;
    for (uint8_t i = 1; i < payloadEnd && out.len < MAX_PAYLOAD; i++) {
      out.text[out.len++] = (char)bytes_[i];
    }
    out.text[out.len] = '\0';
    out.durationMs = nowMs - frameStart_;
    out.endMs = nowMs;

    inFrame_ = false;
    nBytes_ = nBits_ = cur_ = 0;
    return true;
  }

  bool wasOn_ = false;
  bool inFrame_ = false;
  uint32_t onStart_ = 0, offStart_ = 0, frameStart_ = 0;
  int32_t stretch_ = 0;
  uint8_t bytes_[MAX_BYTES];
  uint8_t nBytes_ = 0, nBits_ = 0, cur_ = 0;
};

}  // namespace lifi
