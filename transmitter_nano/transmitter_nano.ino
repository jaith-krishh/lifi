/*
 * LiFi Optical Transmitter — Arduino Nano
 * ---------------------------------------
 * Sends text messages over a free-space light beam using pulse-width
 * On-Off Keying (OOK). Designed to be decoded by an ESP32 + LDR receiver.
 *
 * Wiring
 *   D3  -> 220R -> LED (anode)  ; LED cathode -> GND      (optical Tx)
 *   D2  -> 1k   -> LED (anode)  ; LED cathode -> GND      (Tx status)
 *   A0  <- 10k pot wiper ; pot outer pins to 5V and GND   (message select)
 *
 * Frame on the wire (built by LiFiEncoder.h)
 *   PREAMBLE | LEN | PAYLOAD[LEN] | CHECKSUM | END
 *   - every byte is sent MSB first, 8 bits
 *   - CHECKSUM = XOR of LEN and every payload byte
 *
 * Symbol timing (ms)            default   LDR_SAFE
 *   Preamble : ON / OFF          80 / 40   240 / 120
 *   Bit '1'  : ON / OFF          30 / 15   100 / 50
 *   Bit '0'  : ON / OFF          10 / 15    30 / 50
 *   End      : OFF                   100         300
 *
 * The receiver tells symbols apart by ON-pulse width only (default timing:
 * ON >= 55 ms -> preamble, ON >= 20 ms -> '1', otherwise '0'), after
 * subtracting the sensor lag it measures on the preamble.
 *
 * Pot slots (counter-clockwise -> clockwise)
 *   preset messages | LIVE telemetry | custom message from Serial
 *   LIVE reads the Nano's own temperature sensor and supply voltage, so it
 *   sends real, changing data with no extra parts.
 *
 * Serial Monitor (9600 baud): type a line and press Enter to send a custom
 * message. Turn the pot back to another slot to return to the presets.
 */

// Uncomment on BOTH boards if the receiver's LDR is too slow for the default
// timing (receiver: uncomment LIFI_PROFILE_LDR_SAFE). ~9 bit/s instead of ~29.
// Must come before the #include below.
// #define LIFI_TX_LDR_SAFE

#include "LiFiEncoder.h"

// ---------- Pins ----------
const uint8_t PIN_TX_LED     = 3;
const uint8_t PIN_STATUS_LED = 2;
const uint8_t PIN_SELECT_POT = A0;

const uint16_t T_FRAME_GAP = 1000;  // idle time between repeated frames (ms)

// ---------- Messages ----------
const uint8_t MAX_PAYLOAD = lifi_tx::MAX_PAYLOAD;

const char MSG_0[] PROGMEM = "HELLO LIFI";
const char MSG_1[] PROGMEM = "STATUS:OK";
const char MSG_2[] PROGMEM = "ALERT!";
const char MSG_3[] PROGMEM = "NODE-TX-01";
const char* const MESSAGES[] PROGMEM = { MSG_0, MSG_1, MSG_2, MSG_3 };
const uint8_t NUM_PRESETS = sizeof(MESSAGES) / sizeof(MESSAGES[0]);
// The pot range is split into slots: one per preset, then live telemetry,
// then (fully clockwise) the custom message received over Serial.
const uint8_t SLOT_LIVE   = NUM_PRESETS;
const uint8_t SLOT_CUSTOM = NUM_PRESETS + 1;
const uint8_t NUM_SLOTS   = NUM_PRESETS + 2;

// The internal temperature sensor is only accurate to about +-10 C out of the
// box. Compare with a thermometer and put the difference here.
const int8_t TEMP_OFFSET_C = 0;

char customMsg[MAX_PAYLOAD + 1] = "CUSTOM";
char serialBuf[MAX_PAYLOAD + 1];
uint8_t serialLen = 0;

int8_t currentSlot = -1;
uint32_t frameCount = 0;

// ---------- Low-level symbol output ----------
void pulse(uint16_t onMs, uint16_t offMs) {
  if (onMs > 0) {
    digitalWrite(PIN_TX_LED, HIGH);
    delay(onMs);
  }
  digitalWrite(PIN_TX_LED, LOW);
  delay(offMs);
}

// ---------- Frame ----------
void sendFrame(const char* msg) {
  uint8_t frame[lifi_tx::MAX_FRAME_BYTES];
  uint8_t n = lifi_tx::buildFrame(msg, frame);
  if (frame[0] == 0) return;  // empty message: nothing to send

  digitalWrite(PIN_STATUS_LED, HIGH);
  lifi_tx::emitFrame(frame, n, lifi_tx::TIMING, pulse);
  digitalWrite(PIN_STATUS_LED, LOW);

  frameCount++;
  uint8_t checksum = frame[n - 1];
  Serial.print(F("[TX #"));
  Serial.print(frameCount);
  Serial.print(F("] \""));
  Serial.print(msg);
  Serial.print(F("\" len="));
  Serial.print(frame[0]);
  Serial.print(F(" chk=0x"));
  if (checksum < 0x10) Serial.print('0');
  Serial.println(checksum, HEX);
}

// ---------- Live telemetry (ATmega328P internal sensors) ----------
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__)
// One conversion on an internal ADC channel. ADMUX selects the reference and
// channel; analogRead() rewrites ADMUX on every call, so A0 is unaffected.
uint16_t readInternalAdc(uint8_t admux) {
  ADMUX = admux;
  delay(10);                       // let the reference and mux settle
  ADCSRA |= _BV(ADSC);             // first conversion after a switch is
  while (bit_is_set(ADCSRA, ADSC)) // inaccurate: discard it
    ;
  ADCSRA |= _BV(ADSC);
  while (bit_is_set(ADCSRA, ADSC))
    ;
  return ADCW;
}

// Channel 8 = temperature sensor, measured against the 1.1 V reference.
int16_t readChipTempC() {
  uint16_t raw = readInternalAdc(_BV(REFS1) | _BV(REFS0) | _BV(MUX3));
  return (int16_t)(((int32_t)raw * 100 - 32431) / 122) + TEMP_OFFSET_C;
}

// Channel 14 = 1.1 V bandgap, measured against AVCC; Vcc = 1.1 * 1023 / raw.
// Done last so the reference is back on AVCC for the next analogRead().
uint16_t readVccMillivolts() {
  uint16_t raw = readInternalAdc(_BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1));
  return raw ? (uint16_t)(1125300UL / raw) : 0;
}

// e.g. "T:31C V:4.97 #12" (frame number matches the [TX #12] Serial log)
void buildTelemetry(char* out) {
  int16_t tempC = readChipTempC();
  uint16_t mv = readVccMillivolts();
  snprintf(out, MAX_PAYLOAD + 1, "T:%dC V:%u.%02u #%lu",
           tempC, mv / 1000, (mv % 1000) / 10, (unsigned long)(frameCount + 1));
}
#else
// Other boards (e.g. Nano Every) have a different ADC: send uptime instead.
void buildTelemetry(char* out) {
  snprintf(out, MAX_PAYLOAD + 1, "UP:%lus #%lu",
           (unsigned long)(millis() / 1000), (unsigned long)(frameCount + 1));
}
#endif

// ---------- Input handling ----------
// Map the pot to a slot, with hysteresis so the selection doesn't flicker
// when the wiper sits on a boundary.
int8_t readSlot() {
  int raw = analogRead(PIN_SELECT_POT);  // 0..1023
  int slotWidth = 1024 / NUM_SLOTS;
  int8_t slot = min(raw / slotWidth, NUM_SLOTS - 1);

  if (currentSlot >= 0 && slot != currentSlot) {
    int center = currentSlot * slotWidth + slotWidth / 2;
    if (abs(raw - center) < slotWidth / 2 + 20) return currentSlot;
  }
  return slot;
}

void getMessageForSlot(int8_t slot, char* out) {
  if (slot < NUM_PRESETS) {
    strncpy_P(out, (const char*)pgm_read_ptr(&MESSAGES[slot]), MAX_PAYLOAD);
    out[MAX_PAYLOAD] = '\0';
  } else if (slot == SLOT_LIVE) {
    buildTelemetry(out);
  } else {
    strncpy(out, customMsg, MAX_PAYLOAD + 1);
  }
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLen > 0) {
        serialBuf[serialLen] = '\0';
        strncpy(customMsg, serialBuf, MAX_PAYLOAD + 1);
        serialLen = 0;
        Serial.print(F("Custom message set: \""));
        Serial.print(customMsg);
        Serial.println(F("\" (turn pot fully clockwise to send)"));
      }
    } else if (serialLen < MAX_PAYLOAD && c >= 0x20 && c <= 0x7E) {
      serialBuf[serialLen++] = c;
    }
  }
}

// ---------- Arduino ----------
void setup() {
  pinMode(PIN_TX_LED, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_TX_LED, LOW);
  digitalWrite(PIN_STATUS_LED, LOW);

  Serial.begin(9600);
  Serial.println(F("LiFi Transmitter ready."));
  Serial.println(F("Pot: presets, then LIVE telemetry, fully CW = custom message."));
  Serial.println(F("Type text + Enter to set the custom message."));

  // Power-on self test: blink both LEDs 3 times.
  for (uint8_t i = 0; i < 3; i++) {
    digitalWrite(PIN_TX_LED, HIGH);
    digitalWrite(PIN_STATUS_LED, HIGH);
    delay(150);
    digitalWrite(PIN_TX_LED, LOW);
    digitalWrite(PIN_STATUS_LED, LOW);
    delay(150);
  }
}

void loop() {
  pollSerial();

  int8_t slot = readSlot();
  if (slot != currentSlot) {
    currentSlot = slot;
    Serial.print(F("Selected slot "));
    Serial.print(slot);
    if (slot == SLOT_LIVE) Serial.println(F(" (LIVE telemetry)"));
    else if (slot == SLOT_CUSTOM) Serial.println(F(" (custom message)"));
    else Serial.println();
  }

  char msg[MAX_PAYLOAD + 1];
  getMessageForSlot(currentSlot, msg);
  sendFrame(msg);

  // Idle gap; keep reading Serial so typed input isn't lost.
  uint32_t start = millis();
  while (millis() - start < T_FRAME_GAP) {
    pollSerial();
  }
}
