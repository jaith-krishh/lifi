# LiFi Transmitter — Arduino Nano

This sketch sends text over a beam of light. It uses pulse-width On-Off Keying (OOK), and the ESP32 + LDR receiver decodes it.

## 1. Parts

| Qty | Part | Purpose |
|-----|------|---------|
| 1 | Arduino Nano | Transmitter MCU |
| 1 | Bright LED (white or red, 5 mm) | Optical Tx |
| 1 | 220 Ω resistor | Current limit for the Tx LED (~10–15 mA) |
| 1 | LED (any colour) + 1 kΩ resistor | Status: lights up while a frame is being sent |
| 1 | 10 kΩ potentiometer | Picks which message to send |
| — | Breadboard, jumpers, USB cable | |

## 2. Wiring

```
          Arduino Nano
        ┌──────────────┐
        │           D3 ├──[220Ω]──►|── GND     Tx LED (long leg toward resistor)
        │           D2 ├──[ 1kΩ]──►|── GND     Status LED
        │           A0 ├──────────┐
        │           5V ├───┐      │
        │          GND ├─┐ │      │
        └──────────────┘ │ │   ┌──┴──┐
                         │ └───┤ POT ├───┐
                         │     └─────┘   │
                         └───────────────┘
           (pot: outer pins to 5V and GND, middle wiper to A0)
```

Checklist:
1. D3 → 220 Ω → LED anode (long leg). LED cathode (short leg, flat side) → GND.
2. D2 → 1 kΩ → status LED anode. Its cathode → GND.
3. Pot: one outer pin → 5V, the other outer pin → GND, middle pin → A0.
4. Connect all GNDs together.

## 3. Protocol

```
PREAMBLE | LEN (1 byte) | PAYLOAD (LEN bytes) | CHECKSUM (1 byte) | END
```

| Symbol | LED ON | LED OFF |
|--------|--------|---------|
| Preamble | 80 ms | 40 ms |
| Bit `1` | 30 ms | 15 ms |
| Bit `0` | 10 ms | 15 ms |
| End of frame | — | 100 ms |

- Bytes go out **MSB first**, 8 bits each.
- `CHECKSUM = LEN XOR payload[0] XOR payload[1] …`. The receiver rejects any frame whose checksum doesn't match.
- The receiver only has to measure how long each **ON** pulse lasts:
  `ON ≥ 55 ms → preamble`, `ON ≥ 20 ms → 1`, otherwise `0`.
  Each threshold sits halfway between two pulse widths. The receiver also subtracts the LDR's lag, which it measures on the preamble.

Example: `"HI"` → `LEN=0x02`, `'H'=0x48`, `'I'=0x49`, `CHK=0x02^0x48^0x49=0x03`.

Speed (default timing): about 35 ms per bit, so roughly 28 bit/s. A 10-character message is 12 bytes and takes about 3.5 s per frame. That is slow, but an LDR physically can't follow pulses much faster. See the tips below.

## 4. Upload

1. Open `transmitter_nano.ino` in the Arduino IDE.
2. **Tools → Board → Arduino Nano**.
3. **Tools → Processor → ATmega328P**. If upload fails, try **ATmega328P (Old Bootloader)**. Most clone Nanos need this one.
4. Select the COM port and click Upload.
5. Open **Serial Monitor at 9600 baud**, with line ending set to *Newline*.

## 5. Using it

- On power-up, both LEDs blink 3 times (self test).
- The pot range is split into 6 slots:

  | Pot position | Message |
  |---|---|
  | Slot 0 (fully CCW) | `HELLO LIFI` |
  | Slot 1 | `TEMP:27C` |
  | Slot 2 | `STATUS:OK` |
  | Slot 3 | `ALERT!` |
  | Slot 4 | `NODE-TX-01` |
  | Slot 5 (fully CW) | Custom message from Serial |

- Type any text (up to 32 characters) in Serial Monitor and press Enter to set the custom message.
- The frame repeats every ~1 s. The status LED stays on while a frame is being sent.
- Each frame is logged to Serial, for example
  `[TX #4] "HELLO LIFI" len=10 chk=0x62`.
  Compare this with what the receiver prints to check the link.

To change the presets, edit `MSG_0` … `MSG_4` at the top of the sketch. To add more, add `MSG_5` and include it in `MESSAGES[]`. The slot count adjusts automatically.

## 6. Testing the transmitter alone (before the ESP32 is ready)

1. **Visual check:** you should see a long flash (preamble), then a flicker of short and medium flashes, then a pause.
2. **Phone camera slow-mo:** record at 240 fps to see each pulse clearly.
3. **Logic check:** temporarily set every timing constant 10× larger. You can then count the bits by eye and compare them with the binary of `LEN`.

## 7. Tips for a reliable link

- **Alignment:** put the LED and LDR 2–10 cm apart and pointing straight at each other. A straw or black paper tube around the LDR blocks room light a lot.
- **LED choice:** a white or red high-brightness LED works best with a CdS LDR. LDRs respond weakly to IR and blue.
- **LDR speed:** a CdS LDR is slow to go dark again, so every pulse looks longer at the receiver. The receiver corrects for this automatically, using the 80 ms preamble as a timing reference. Simulation (`tests/`) shows the default timing works up to a sensor fall time of ~15 ms. Many LDRs are slower than that, about 30 ms for a GL5528. If the dashboard shows lots of `checksum`/`length` errors:
  - set `LDR_SAFE_PROFILE = true` in this sketch **and** uncomment `#define LIFI_PROFILE LIFI_PROFILE_LDR_SAFE` in the receiver (~9 bit/s; works up to ~40 ms fall time); or
  - swap the LDR for a **phototransistor (e.g. TEPT5700, L14G1) or photodiode**. They respond in microseconds.
- **Never block in `delay()` elsewhere:** the timing is software-driven, so keep `loop()` simple. Add sensors only between frames.
- **Power:** 220 Ω on D3 draws only ~10–15 mA, well below the Nano's 40 mA pin limit. For more range, drive a brighter LED through an NPN transistor (2N2222/BC547: D3 → 1 kΩ → base, LED + resistor on the collector).

## 8. Receiver

The matching ESP32 receiver and web dashboard are in [`../receiver_esp32`](../receiver_esp32). Both boards must use the same timing profile.
