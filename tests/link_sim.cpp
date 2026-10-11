/*
 * Host-side end-to-end simulation of the LiFi link.
 *
 *   lifi_tx::buildFrame + emitFrame (the exact code the Nano runs)
 *     -> LED light level per millisecond
 *     -> LDR modelled as a first-order lag (separate rise / fall time constants)
 *     -> ambient light + ADC noise, 12-bit clamp
 *     -> lifi::Slicer + lifi::Decoder (the exact code the ESP32 runs)
 *
 * Build & run:  make -C tests      (or: g++ -std=c++11 -O2 tests/link_sim.cpp -o sim && ./sim)
 */
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../receiver_esp32/LiFiDecoder.h"
#include "../transmitter_nano/LiFiEncoder.h"

using namespace lifi;

// ---- Transmitter: the Nano's own encoder ----
static lifi_tx::Timing TX = lifi_tx::TIMING_FAST;  // transmitter profile under test
static Timing T;                                   // receiver profile under test

static void sendFrame(std::vector<uint8_t>& s, const std::string& msg, bool corrupt = false) {
  uint8_t frame[lifi_tx::MAX_FRAME_BYTES];
  uint8_t n = lifi_tx::buildFrame(msg.c_str(), frame);
  if (corrupt) frame[n - 1] ^= 0x01;
  lifi_tx::emitFrame(frame, n, TX, [&s](uint16_t on, uint16_t off) {
    s.insert(s.end(), on, 1);
    s.insert(s.end(), off, 0);
  });
  s.insert(s.end(), 1000, 0);  // gap between frames (T_FRAME_GAP)
}

static float gFraction = Slicer().autoFraction;

struct Result { int ok = 0, bad = 0, trunc = 0, badlen = 0, wrongText = 0; std::vector<std::string> got; };

// tauRise/tauFall in ms; manualThr < 0 = auto threshold.
static Result run(const std::vector<std::string>& msgs, double tauRise, double tauFall,
                  double noise, int manualThr, bool corruptLast = false, bool quiet = true) {
  std::vector<uint8_t> light;
  light.insert(light.end(), 500, 0);
  for (size_t i = 0; i < msgs.size(); i++)
    sendFrame(light, msgs[i], corruptLast && i + 1 == msgs.size());

  std::mt19937 rng(42);
  std::normal_distribution<double> gauss(0.0, noise);
  const double ambient = 700, ledSwing = 2200;

  Slicer slicer;
  slicer.autoFraction = gFraction;
  Decoder dec;
  dec.timing = T;
  Result r;
  double ldr = 0;  // 0..1 response
  size_t expectIdx = 0;

  for (uint32_t t = 0; t < light.size(); t++) {
    double target = light[t];
    double tau = target > ldr ? tauRise : tauFall;
    ldr += (target - ldr) * (1.0 - std::exp(-1.0 / tau));
    int adc = (int)std::lround(ambient + ledSwing * ldr + gauss(rng));
    if (adc < 0) adc = 0;
    if (adc > 4095) adc = 4095;

    bool on = slicer.update(adc, manualThr);
    Frame f;
    if (dec.update(on, t, f)) {
      switch (f.status) {
        case FRAME_OK:
          r.ok++;
          r.got.push_back(f.text);
          if (expectIdx < msgs.size() && msgs[expectIdx] != f.text) r.wrongText++;
          break;
        case FRAME_BAD_CHECKSUM: r.bad++; break;
        case FRAME_BAD_LENGTH:   r.badlen++; break;
        default:                 r.trunc++; break;
      }
      expectIdx++;
      if (!quiet) printf("  t=%6u  %-9s \"%s\" (%u ms)\n", t, statusName(f.status), f.text, f.durationMs);
    }
  }
  return r;
}

static int failures = 0;
static void check(bool cond, const char* what) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) failures++;
}

// Receiver timing is set from the transmitter's table; profile_check.cpp
// verifies the two tables are identical in both profiles.
static void setProfile(int profile) {
  TX = profile == 1 ? lifi_tx::TIMING_LDR_SAFE : lifi_tx::TIMING_FAST;
  T.preambleOn = TX.preambleOn; T.preambleOff = TX.preambleOff;
  T.oneOn = TX.oneOn; T.zeroOn = TX.zeroOn; T.bitOff = TX.bitOff; T.endOff = TX.endOff;
}

static void encoderTests() {
  printf("=== Transmitter encoder ===\n");
  uint8_t f[lifi_tx::MAX_FRAME_BYTES];

  uint8_t n = lifi_tx::buildFrame("HI", f);
  check(n == 4 && f[0] == 0x02 && f[1] == 0x48 && f[2] == 0x49 && f[3] == 0x03,
        "\"HI\" -> 02 48 49 03 (LEN, 'H', 'I', CHK)");

  std::string longMsg(40, 'x');
  n = lifi_tx::buildFrame(longMsg.c_str(), f);
  check(n == lifi_tx::MAX_FRAME_BYTES && f[0] == lifi_tx::MAX_PAYLOAD,
        "40-char message is cut to 32 payload bytes");

  int symbols = 0, ones = 0;
  uint16_t lastOn = 1, lastOff = 0;
  n = lifi_tx::buildFrame("HI", f);
  lifi_tx::emitFrame(f, n, lifi_tx::TIMING_FAST, [&](uint16_t on, uint16_t off) {
    symbols++;
    if (on == lifi_tx::TIMING_FAST.oneOn) ones++;
    lastOn = on; lastOff = off;
  });
  // 02 48 49 03 has 1 + 2 + 3 + 2 = 8 one-bits.
  check(symbols == 1 + 8 * 4 + 1 && ones == 8 && lastOn == 0 && lastOff == 100,
        "emits preamble, 32 bits MSB-first (8 ones), then a dark end gap");
  printf("\n");
}

static void envelope(const std::vector<std::string>& msgs, const double* falls, int nFalls,
                     const double* rises, int nRises) {
  printf("  frames decoded / %zu  (rows: LDR rise time, cols: LDR fall time)\n", msgs.size());
  printf("  rise\\fall ");
  for (int j = 0; j < nFalls; j++) printf("%5.0fms", falls[j]);
  printf("\n");
  for (int i = 0; i < nRises; i++) {
    printf("  %5.0fms   ", rises[i]);
    for (int j = 0; j < nFalls; j++) {
      Result e = run(msgs, rises[i], falls[j], 25, -1);
      printf("%7d", e.ok - e.wrongText);
    }
    printf("\n");
  }
}

int main() {
  encoderTests();

  const std::vector<std::string> msgs = {"HELLO LIFI", "T:31C V:4.97 #12", "STATUS:OK", "ALERT!", "NODE-TX-01",
                                         "~!@#$%^&*()_+{}|:<>?", "abcdefghijklmnopqrstuvwxyz012345"};

  printf("=== FAST profile (spec timing) ===\n");
  setProfile(0);

  printf("1) Ideal link (fast sensor, light noise, auto threshold)\n");
  Result r = run(msgs, 0.5, 0.5, 10, -1, false, false);
  check(r.ok == (int)msgs.size() && r.wrongText == 0, "all messages decoded exactly");

  printf("2) Quick LDR (rise 4 ms, fall 8 ms), noise 25, auto threshold\n");
  r = run(msgs, 4, 8, 25, -1);
  check(r.ok == (int)msgs.size() && r.wrongText == 0, "all messages decoded exactly");

  printf("3) Same LDR, manual threshold from pot (ADC 1300)\n");
  r = run(msgs, 4, 8, 25, 1300);
  check(r.ok == (int)msgs.size() && r.wrongText == 0, "all messages decoded exactly");

  printf("4) Corrupted checksum is rejected\n");
  r = run({"HELLO", "WORLD"}, 4, 8, 25, -1, true, false);
  check(r.ok == 1 && r.bad == 1, "first frame ok, second flagged as checksum error");

  printf("5) Manual threshold above the LED level -> nothing decoded, no false frames\n");
  r = run(msgs, 4, 8, 25, 3500);
  check(r.ok == 0 && r.bad == 0, "no frames accepted");

  const double fastFalls[] = {2, 4, 6, 8, 10, 12, 15, 20};
  const double fastRises[] = {2, 4, 6, 8, 10};
  envelope(msgs, fastFalls, 8, fastRises, 5);

  printf("\n=== LDR_SAFE profile ===\n");
  setProfile(1);
  const std::vector<std::string> short_msgs = {"HELLO LIFI", "T:31C V:4.97 #12", "~!@#$%^&*()_+{}|:<>?"};

  printf("6) Datasheet-slow LDR (GL5528: rise 20 ms, fall 30 ms), auto threshold\n");
  r = run(short_msgs, 20, 30, 25, -1, false, false);
  check(r.ok == (int)short_msgs.size() && r.wrongText == 0, "all messages decoded exactly");

  printf("7) Very slow LDR (rise 20 ms, fall 40 ms), auto threshold\n");
  r = run(short_msgs, 20, 40, 25, -1);
  check(r.ok == (int)short_msgs.size() && r.wrongText == 0, "all messages decoded exactly");

  const double safeFalls[] = {10, 20, 30, 40, 50};
  const double safeRises[] = {10, 20, 30};
  envelope(short_msgs, safeFalls, 5, safeRises, 3);

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL CHECKS PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
