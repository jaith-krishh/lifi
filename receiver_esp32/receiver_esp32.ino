/*
 * LiFi Optical Receiver — ESP32
 * -----------------------------
 * Decodes the pulse-width OOK light signal sent by transmitter_nano.ino and
 * shows it on a live web dashboard.
 *
 * Wiring
 *   3.3V -> LDR -> node A -> 10k -> GND ;  node A -> GPIO36 (VP)   light sensor
 *   10k pot: outer pins 3.3V and GND, wiper -> GPIO34              threshold
 *   Buzzer + -> GPIO25, Buzzer - -> GND                            chirps
 *
 * Calibration pot
 *   Fully counter-clockwise (first ~4% of travel) = AUTO threshold: the ESP32
 *   tracks ambient and LED levels and sets the threshold itself.
 *   Anywhere else = MANUAL: the pot position is the threshold (0..4095).
 *
 * Wi-Fi
 *   Always starts an access point "LiFi-Receiver" (password "lifi1234").
 *   Connect to it and open http://192.168.4.1 (most phones pop it up
 *   automatically). Optionally also joins your Wi-Fi: set STA_SSID below.
 *
 * No extra libraries needed: uses WiFi, WebServer, DNSServer and ESPmDNS from
 * the ESP32 Arduino core (2.x or 3.x).
 */

// Uncomment on BOTH boards if your LDR is too slow for the default timing
// (see README). Must be defined before LiFiDecoder.h is included.
// #define LIFI_PROFILE LIFI_PROFILE_LDR_SAFE

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

#include "LiFiDecoder.h"
#include "dashboard.h"

// ---------- Pins ----------
const int PIN_LDR    = 36;  // ADC1_CH0 (VP)
const int PIN_POT    = 34;  // ADC1_CH6
const int PIN_BUZZER = 25;

// ---------- Wi-Fi ----------
const char* AP_SSID  = "LiFi-Receiver";
const char* AP_PASS  = "lifi1234";  // at least 8 characters
const char* STA_SSID = "";          // optional: your router's SSID
const char* STA_PASS = "";
const char* MDNS_NAME = "lifi";     // http://lifi.local when on the same Wi-Fi

// ---------- Behaviour ----------
const int  POT_AUTO_ZONE = 150;   // pot reading below this = auto threshold
const bool BEEP_ON_ERROR = true;  // low beep when a damaged frame arrives

// ---------- Shared state (decoder task -> web server) ----------
const int TRACE_LEN      = 300;   // points in the scope view
const int TRACE_STEP_MS  = 10;    // => 3 s of signal on screen
const int HISTORY_LEN    = 12;

struct HistoryEntry {
  lifi::FrameStatus status;
  char text[lifi::MAX_PAYLOAD + 1];
  uint32_t endMs;
  uint32_t durationMs;
};

struct Shared {
  int adc, threshold, hyst, floorLevel, peakLevel, pot;
  bool autoMode, hasSignal, lightOn, inFrame;
  char partial[lifi::MAX_PAYLOAD + 1];
  uint8_t bytesReceived, bytesExpected;
  int32_t stretch;

  uint32_t ok, badChecksum, badLength, truncated, glitches, seq;
  float bitRate;
  uint32_t recent;      // last 32 frame results, bit = 1 if ok
  uint8_t recentCount;

  HistoryEntry history[HISTORY_LEN];
  uint8_t historyHead, historyCount;

  uint16_t trace[TRACE_LEN];
  uint16_t traceHead;
};

static Shared sh;
static SemaphoreHandle_t shLock;
static volatile uint8_t beepRequest = 0;  // 1 = valid frame, 2 = error

WebServer server(80);
DNSServer dns;

// ---------------------------------------------------------------------------
// Decoder task: samples the LDR every 1 ms on its own high-priority task so
// web traffic can never disturb pulse timing.
// ---------------------------------------------------------------------------
static int readLight() {
  // Average 4 conversions to knock down ADC noise (~40 us total).
  int sum = 0;
  for (int i = 0; i < 4; i++) sum += analogRead(PIN_LDR);
  return sum / 4;
}

static void recordFrame(const lifi::Frame& f) {
  bool ok = f.status == lifi::FRAME_OK;
  switch (f.status) {
    case lifi::FRAME_OK:           sh.ok++; break;
    case lifi::FRAME_BAD_CHECKSUM: sh.badChecksum++; break;
    case lifi::FRAME_BAD_LENGTH:   sh.badLength++; break;
    default:                       sh.truncated++; break;
  }
  sh.seq++;
  sh.recent = (sh.recent << 1) | (ok ? 1 : 0);
  if (sh.recentCount < 32) sh.recentCount++;
  if (ok && f.durationMs > 0) sh.bitRate = (f.len + 2) * 8 * 1000.0f / f.durationMs;

  HistoryEntry& h = sh.history[sh.historyHead];
  h.status = f.status;
  memcpy(h.text, f.text, sizeof(h.text));
  h.endMs = f.endMs;
  h.durationMs = f.durationMs;
  sh.historyHead = (sh.historyHead + 1) % HISTORY_LEN;
  if (sh.historyCount < HISTORY_LEN) sh.historyCount++;
}

static void decoderTask(void*) {
  lifi::Slicer slicer;
  lifi::Decoder decoder;
  int manualThreshold = -1, potRaw = 0;
  uint32_t lastPotMs = 0, lastTraceMs = 0;
  int tracePeak = 0;

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1));
    uint32_t now = millis();

    if (now - lastPotMs >= 50) {
      lastPotMs = now;
      potRaw = analogRead(PIN_POT);
      manualThreshold = potRaw < POT_AUTO_ZONE ? -1 : potRaw;
    }

    int v = readLight();
    bool on = slicer.update(v, manualThreshold);
    lifi::Frame frame;
    bool done = decoder.update(on, now, frame);
    if (v > tracePeak) tracePeak = v;  // peak-hold so short pulses show up

    xSemaphoreTake(shLock, portMAX_DELAY);
    sh.adc = v;
    sh.lightOn = on;
    if (done) recordFrame(frame);
    if (now - lastTraceMs >= TRACE_STEP_MS) {
      lastTraceMs = now;
      sh.trace[sh.traceHead] = (uint16_t)tracePeak;
      sh.traceHead = (sh.traceHead + 1) % TRACE_LEN;
      tracePeak = 0;

      sh.pot = potRaw;
      sh.threshold = slicer.threshold;
      sh.hyst = slicer.hyst;
      sh.floorLevel = slicer.floorLevel();
      sh.peakLevel = slicer.peakLevel();
      sh.autoMode = slicer.autoMode;
      sh.hasSignal = slicer.hasSignal;
      sh.inFrame = decoder.inFrame();
      decoder.partialText(sh.partial);
      sh.bytesReceived = decoder.bytesReceived();
      sh.bytesExpected = decoder.bytesReceived() > 0 ? decoder.expectedLen() + 2 : 0;
      sh.stretch = decoder.stretchMs();
      sh.glitches = decoder.glitches;
    }
    xSemaphoreGive(shLock);

    if (done) {
      beepRequest = frame.status == lifi::FRAME_OK ? 1 : 2;
      Serial.printf("[RX] %-9s \"%s\" len=%u %lu ms\n", lifi::statusName(frame.status),
                    frame.text, frame.len, (unsigned long)frame.durationMs);
    }
  }
}

// ---------------------------------------------------------------------------
// Buzzer: non-blocking chirps, driven from loop()
// ---------------------------------------------------------------------------
static uint32_t secondChirpAt = 0;

static void serviceBuzzer() {
  uint8_t req = beepRequest;
  if (req) {
    beepRequest = 0;
    if (req == 1) {
      tone(PIN_BUZZER, 2800, 60);
      secondChirpAt = millis() + 110;
    } else if (BEEP_ON_ERROR) {
      tone(PIN_BUZZER, 600, 200);
    }
  }
  if (secondChirpAt && (int32_t)(millis() - secondChirpAt) >= 0) {
    secondChirpAt = 0;
    tone(PIN_BUZZER, 3600, 60);
  }
}

// ---------------------------------------------------------------------------
// Web server
// ---------------------------------------------------------------------------
static void jsonString(String& out, const char* s) {
  out += '"';
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
    else if (c < 0x20 || c > 0x7E) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else out += (char)c;
  }
  out += '"';
}

static void handleStatus() {
  static Shared snap;  // static: too big for the web server's stack
  xSemaphoreTake(shLock, portMAX_DELAY);
  memcpy(&snap, &sh, sizeof(snap));
  xSemaphoreGive(shLock);

  String j;
  j.reserve(3000);
  j += F("{\"up\":");      j += millis();
  j += F(",\"profile\":\""); j += (LIFI_PROFILE == LIFI_PROFILE_LDR_SAFE) ? "LDR_SAFE" : "FAST";
  j += F("\",\"adc\":");   j += snap.adc;
  j += F(",\"thr\":");     j += snap.threshold;
  j += F(",\"hyst\":");    j += snap.hyst;
  j += F(",\"floor\":");   j += snap.floorLevel;
  j += F(",\"peak\":");    j += snap.peakLevel;
  j += F(",\"pot\":");     j += snap.pot;
  j += F(",\"auto\":");    j += snap.autoMode ? "true" : "false";
  j += F(",\"sig\":");     j += snap.hasSignal ? "true" : "false";
  j += F(",\"on\":");      j += snap.lightOn ? "true" : "false";
  j += F(",\"rx\":");      j += snap.inFrame ? "true" : "false";
  j += F(",\"partial\":"); jsonString(j, snap.partial);
  j += F(",\"got\":");     j += snap.bytesReceived;
  j += F(",\"exp\":");     j += snap.bytesExpected;
  j += F(",\"stretch\":"); j += snap.stretch;
  j += F(",\"ok\":");      j += snap.ok;
  j += F(",\"chk\":");     j += snap.badChecksum;
  j += F(",\"len\":");     j += snap.badLength;
  j += F(",\"trunc\":");   j += snap.truncated;
  j += F(",\"glitch\":");  j += snap.glitches;
  j += F(",\"seq\":");     j += snap.seq;
  j += F(",\"bps\":");     j += String(snap.bitRate, 1);

  int okRecent = 0;
  for (int i = 0; i < snap.recentCount; i++) okRecent += (snap.recent >> i) & 1;
  j += F(",\"q\":");
  j += snap.recentCount ? (okRecent * 100 / snap.recentCount) : -1;

  j += F(",\"hist\":[");
  for (int i = 0; i < snap.historyCount; i++) {
    int idx = (snap.historyHead - 1 - i + HISTORY_LEN) % HISTORY_LEN;  // newest first
    const HistoryEntry& h = snap.history[idx];
    if (i) j += ',';
    j += F("{\"s\":\""); j += lifi::statusName(h.status);
    j += F("\",\"t\":");  j += h.endMs;
    j += F(",\"d\":");    j += h.durationMs;
    j += F(",\"m\":");    jsonString(j, h.text);
    j += '}';
  }

  j += F("],\"traceMs\":"); j += TRACE_STEP_MS;
  j += F(",\"trace\":[");
  for (int i = 0; i < TRACE_LEN; i++) {  // oldest first
    if (i) j += ',';
    j += snap.trace[(snap.traceHead + i) % TRACE_LEN];
  }
  j += F("]}");

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static void handleReset() {
  xSemaphoreTake(shLock, portMAX_DELAY);
  sh.ok = sh.badChecksum = sh.badLength = sh.truncated = sh.seq = 0;
  sh.recent = 0;
  sh.recentCount = 0;
  sh.historyCount = sh.historyHead = 0;
  sh.bitRate = 0;
  xSemaphoreGive(shLock);
  server.send(200, "application/json", "{\"reset\":true}");
}

static void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("Access point \"%s\" (password \"%s\") -> http://%s\n", AP_SSID, AP_PASS,
                WiFi.softAPIP().toString().c_str());

  // Captive portal: every hostname resolves to us, so phones open the dashboard.
  dns.start(53, "*", WiFi.softAPIP());

  if (STA_SSID[0]) {
    Serial.printf("Joining Wi-Fi \"%s\"", STA_SSID);
    WiFi.begin(STA_SSID, STA_PASS);
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
      delay(500);
      Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnected -> http://%s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println(F("\nCould not join; continuing with the access point only."));
    }
  }

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Also reachable at http://%s.local (same network, mDNS)\n", MDNS_NAME);
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nLiFi Receiver starting"));

  pinMode(PIN_BUZZER, OUTPUT);
  analogReadResolution(12);  // 0..4095 (GPIO36/34 default to 11 dB, ~0-3.3 V)

  memset(&sh, 0, sizeof(sh));
  shLock = xSemaphoreCreateMutex();

  setupWiFi();

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", INDEX_HTML); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.onNotFound([]() {  // captive-portal probes and stray URLs -> dashboard
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();

  // Core 1, above loop() (priority 1), so sampling preempts web handling.
  xTaskCreatePinnedToCore(decoderTask, "lifi-rx", 4096, nullptr, 3, nullptr, 1);

  tone(PIN_BUZZER, 2000, 80);  // power-on chirp
  Serial.printf("Profile: %s. Pot fully CCW = auto threshold.\n",
                LIFI_PROFILE == LIFI_PROFILE_LDR_SAFE ? "LDR_SAFE" : "FAST");
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  serviceBuzzer();
  delay(2);
}
