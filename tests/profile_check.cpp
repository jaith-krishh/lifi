/*
 * Checks that the transmitter (LiFiEncoder.h) and receiver (LiFiDecoder.h)
 * use identical symbol timing. Built once per profile by the Makefile:
 *   default            -> FAST on both sides
 *   -DPROFILE_LDR_SAFE -> LDR_SAFE on both sides
 */
#include <cstdio>

#ifdef PROFILE_LDR_SAFE
#define LIFI_PROFILE LIFI_PROFILE_LDR_SAFE
#define LIFI_TX_LDR_SAFE
#define NAME "LDR_SAFE"
#else
#define NAME "FAST"
#endif

#include "../receiver_esp32/LiFiDecoder.h"
#include "../transmitter_nano/LiFiEncoder.h"

int main() {
  lifi::Timing rx;
  const lifi_tx::Timing& tx = lifi_tx::TIMING;
  bool same = rx.preambleOn == tx.preambleOn && rx.preambleOff == tx.preambleOff &&
              rx.oneOn == tx.oneOn && rx.zeroOn == tx.zeroOn && rx.bitOff == tx.bitOff &&
              rx.endOff == tx.endOff;
  printf("  [%s] %s profile: transmitter and receiver timing match\n", same ? "PASS" : "FAIL", NAME);
  return same ? 0 : 1;
}
