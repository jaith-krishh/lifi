/*
 * LiFiEncoder.h — frame builder and symbol timing for the LiFi transmitter.
 *
 * Plain C++ with no Arduino dependencies, so the exact same code runs on the
 * Nano and in the host-side simulation in ../tests.
 *
 * Frame:  PREAMBLE | LEN | PAYLOAD[LEN] | CHECKSUM | END
 *   - bytes MSB first, CHECKSUM = LEN ^ payload[0] ^ ... ^ payload[LEN-1]
 *   - every symbol is "LED on for X ms, then off for Y ms"
 */
#pragma once

#include <stdint.h>

namespace lifi_tx {

const uint8_t MAX_PAYLOAD     = 32;
const uint8_t MAX_FRAME_BYTES = MAX_PAYLOAD + 2;  // LEN + payload + CHECKSUM

// Symbol timing in ms. Must match receiver_esp32/LiFiDecoder.h.
struct Timing {
  uint16_t preambleOn, preambleOff;
  uint16_t oneOn, zeroOn, bitOff;
  uint16_t endOff;
};

const Timing TIMING_FAST     = {80, 40, 30, 10, 15, 100};    // spec timing, ~29 bit/s
const Timing TIMING_LDR_SAFE = {240, 120, 100, 30, 50, 300}; // slow CdS LDRs, ~9 bit/s

// Define LIFI_TX_LDR_SAFE before including this file to pick the slow profile.
#ifdef LIFI_TX_LDR_SAFE
const Timing TIMING = TIMING_LDR_SAFE;
#else
const Timing TIMING = TIMING_FAST;
#endif

// Writes LEN | PAYLOAD | CHECKSUM into `out` (room for MAX_FRAME_BYTES) and
// returns the number of bytes written. Text past MAX_PAYLOAD is cut off.
// An empty message gives LEN = 0, which the receiver rejects, so callers
// should not send one.
inline uint8_t buildFrame(const char* msg, uint8_t* out) {
  uint8_t len = 0;
  while (len < MAX_PAYLOAD && msg[len]) len++;

  uint8_t checksum = len;
  out[0] = len;
  for (uint8_t i = 0; i < len; i++) {
    out[1 + i] = (uint8_t)msg[i];
    checksum ^= (uint8_t)msg[i];
  }
  out[1 + len] = checksum;
  return len + 2;
}

// Turns a built frame into light symbols: calls emit(onMs, offMs) once for
// the preamble, once per bit (MSB first) and once for the end gap (onMs = 0).
template <typename Emit>
void emitFrame(const uint8_t* frame, uint8_t n, const Timing& t, Emit emit) {
  emit(t.preambleOn, t.preambleOff);
  for (uint8_t i = 0; i < n; i++) {
    for (int8_t b = 7; b >= 0; b--) {
      bool one = (frame[i] >> b) & 0x01;
      emit(one ? t.oneOn : t.zeroOn, t.bitOff);
    }
  }
  emit(0, t.endOff);
}

}  // namespace lifi_tx
