/*
 * Host-side end-to-end simulation of the LiFi link.
 *
 *   Transmitter encoding (from the protocol spec)
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

using namespace lifi;

// ---- Transmitter (mirrors transmitter_nano.ino) ----
static Timing T;  // timing profile under test

static void pulse(std::vector<uint8_t>& s, int on, int off) {
  s.insert(s.end(), on, 1);
  s.insert(s.end(), off, 0);
}
static void sendByte(std::vector<uint8_t>& s, uint8_t v) {
  for (int i = 7; i >= 0; i--) pulse(s, (v >> i) & 1 ? T.oneOn : T.zeroOn, T.bitOff);
}
static void sendFrame(std::vector<uint8_t>& s, const std::string& msg, bool corrupt = false) {
  uint8_t len = (uint8_t)msg.size(), chk = len;
  pulse(s, T.preambleOn, T.preambleOff);
  sendByte(s, len);
  for (char c : msg) { sendByte(s, (uint8_t)c); chk ^= (uint8_t)c; }
  sendByte(s, corrupt ? (uint8_t)(chk ^ 0x01) : chk);
  s.insert(s.end(), T.endOff, 0);  // end of frame
  s.insert(s.end(), 1000, 0);  // gap between frames
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

static void setProfile(int profile) {
  Timing fast, safe;
  if (profile == 1) {
    safe.preambleOn = 240; safe.preambleOff = 120; safe.oneOn = 100;
    safe.zeroOn = 30; safe.bitOff = 50; safe.endOff = 300;
    T = safe;
  } else {
    T = fast;
  }
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
  const std::vector<std::string> msgs = {"HELLO LIFI", "TEMP:27C", "STATUS:OK", "ALERT!", "NODE-TX-01",
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
  const std::vector<std::string> short_msgs = {"HELLO LIFI", "TEMP:27C", "~!@#$%^&*()_+{}|:<>?"};

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
