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
 * Frame on the wire
 *   PREAMBLE | LEN | PAYLOAD[LEN] | CHECKSUM | END
 *   - every byte is sent MSB first, 8 bits
 *   - CHECKSUM = XOR of LEN and every payload byte
 *
 * Symbol timing (ms)
 *   Preamble : ON 80, OFF 40
 *   Bit '1'  : ON 30, OFF 15
 *   Bit '0'  : ON 10, OFF 15
 *   End      : OFF 100
 *
 * The receiver tells symbols apart by ON-pulse width only:
 *   ON >= 55 ms -> preamble, ON >= 20 ms -> '1', otherwise '0'.
 *
 * Serial Monitor (9600 baud): type a line and press Enter to send a custom
 * message. Turn the pot back to another slot to return to the presets.
 */

// ---------- Pins ----------
const uint8_t PIN_TX_LED     = 3;
const uint8_t PIN_STATUS_LED = 2;
const uint8_t PIN_SELECT_POT = A0;

// ---------- Timing (ms) — must match the receiver ----------
const uint16_t T_PREAMBLE_ON  = 80;
const uint16_t T_PREAMBLE_OFF = 40;
const uint16_t T_ONE_ON       = 30;
const uint16_t T_ZERO_ON      = 10;
const uint16_t T_BIT_OFF      = 15;
const uint16_t T_END_OFF      = 100;
const uint16_t T_FRAME_GAP    = 1000;  // idle time between repeated frames

// ---------- Messages ----------
const uint8_t MAX_PAYLOAD = 32;

const char MSG_0[] PROGMEM = "HELLO LIFI";
const char MSG_1[] PROGMEM = "TEMP:27C";
const char MSG_2[] PROGMEM = "STATUS:OK";
const char MSG_3[] PROGMEM = "ALERT!";
const char MSG_4[] PROGMEM = "NODE-TX-01";
const char* const MESSAGES[] PROGMEM = { MSG_0, MSG_1, MSG_2, MSG_3, MSG_4 };
const uint8_t NUM_PRESETS = sizeof(MESSAGES) / sizeof(MESSAGES[0]);
// The pot range is split into NUM_PRESETS + 1 slots; the last slot sends
// the custom message received over Serial.
const uint8_t NUM_SLOTS = NUM_PRESETS + 1;

char customMsg[MAX_PAYLOAD + 1] = "CUSTOM";
char serialBuf[MAX_PAYLOAD + 1];
uint8_t serialLen = 0;

int8_t currentSlot = -1;
uint32_t frameCount = 0;

// ---------- Low-level symbol output ----------
void pulse(uint16_t onMs, uint16_t offMs) {
  digitalWrite(PIN_TX_LED, HIGH);
  delay(onMs);
  digitalWrite(PIN_TX_LED, LOW);
  delay(offMs);
}

void sendPreamble() { pulse(T_PREAMBLE_ON, T_PREAMBLE_OFF); }

void sendBit(bool b) { pulse(b ? T_ONE_ON : T_ZERO_ON, T_BIT_OFF); }

void sendByte(uint8_t value) {
  for (int8_t i = 7; i >= 0; i--) {
    sendBit((value >> i) & 0x01);
  }
}

void sendEnd() {
  digitalWrite(PIN_TX_LED, LOW);
  delay(T_END_OFF);
}

// ---------- Frame ----------
void sendFrame(const char* msg) {
  uint8_t len = strnlen(msg, MAX_PAYLOAD);
  uint8_t checksum = len;

  digitalWrite(PIN_STATUS_LED, HIGH);

  sendPreamble();
  sendByte(len);
  for (uint8_t i = 0; i < len; i++) {
    sendByte((uint8_t)msg[i]);
    checksum ^= (uint8_t)msg[i];
  }
  sendByte(checksum);
  sendEnd();

  digitalWrite(PIN_STATUS_LED, LOW);

  frameCount++;
  Serial.print(F("[TX #"));
  Serial.print(frameCount);
  Serial.print(F("] \""));
  Serial.print(msg);
  Serial.print(F("\" len="));
  Serial.print(len);
  Serial.print(F(" chk=0x"));
  if (checksum < 0x10) Serial.print('0');
  Serial.println(checksum, HEX);
}

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
  Serial.println(F("Pot selects a preset; last position = custom message."));
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
    Serial.println(slot);
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
