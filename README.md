# LiFi: Optical Data Transmission System

A free-space optical data link built from hobby parts. An **Arduino Nano** encodes text as LED light pulses, an **ESP32** decodes them with an LDR, and the received messages appear on a live **web dashboard** over Wi-Fi.

```
[ TRANSMITTER: Arduino Nano ]                     [ RECEIVER: ESP32 ]
  Pot (A0) → pick message                           LDR divider → GPIO36
  Serial  → custom message        ~~~ light ~~~>    Pot (GPIO34) → threshold
  LED (D3) → data                                   Buzzer (GPIO25) → chirp
  LED (D2) → "sending"                              Wi-Fi → web dashboard
```

| Folder | What's inside |
|---|---|
| [`transmitter_nano/`](transmitter_nano) | Nano sketch, frame encoder, breadboard build + upload guide |
| [`receiver_esp32/`](receiver_esp32) | ESP32 sketch, decoder, dashboard + guide |
| [`tests/`](tests) | PC simulation of the full link (encoder → LDR model → decoder) |
| [`docs/`](docs) | Images |

## Quick start

1. **Wire and flash the transmitter.** Follow [transmitter_nano/README.md](transmitter_nano/README.md). The Tx LED should flicker in bursts about once a second.
2. **Wire and flash the receiver.** Follow [receiver_esp32/README.md](receiver_esp32/README.md).
3. Join Wi-Fi **LiFi-Receiver** (password `lifi1234`) and open **http://192.168.4.1**.
4. Turn the receiver pot fully counter-clockwise (auto threshold). Point the LED at the LDR from 2–10 cm away.
5. Messages appear on the dashboard, and the buzzer chirps for each good frame.

If you see mostly errors, your LDR is probably too slow for the default timing. Switch both boards to the `LDR_SAFE` profile (see the receiver README, section 7).

## Protocol

Pulse-width modulated On-Off Keying. A bit's value is set by **how long the LED stays on**, so the receiver needs no shared clock.

```
PREAMBLE | LEN | PAYLOAD (LEN bytes) | CHECKSUM | END
```

| Symbol | ON | OFF |
|---|---|---|
| Preamble | 80 ms | 40 ms |
| `1` | 30 ms | 15 ms |
| `0` | 10 ms | 15 ms |
| End | — | 100 ms |

- Bytes are sent MSB first.
- `CHECKSUM = LEN ⊕ payload[0] ⊕ … ⊕ payload[n-1]`.
- The preamble doubles as a **timing reference**. The receiver measures how much the LDR stretched it and removes that stretch from every bit in the frame.

## Testing without hardware

```bash
make -C tests
```

This runs the Nano's real encoder (`transmitter_nano/LiFiEncoder.h`) and the ESP32's real decoder (`receiver_esp32/LiFiDecoder.h`) through a simulated LDR with lag and ADC noise. It checks:
- that the encoder produces the right bytes and pulses
- that both boards use identical timing, in both profiles
- correct decoding in auto and manual threshold modes
- that corrupted frames are rejected
- that no false frames appear when the threshold is wrong

It also prints a table of which LDR speeds each timing profile can handle.

## Demo script (≈3 minutes)

1. **Show the hardware.** "The Nano is the transmitter, the ESP32 the receiver. There are no wires between them, only light."
2. **Open the dashboard** on a phone. Point at the scope: "This is the light the LDR sees: a long preamble, then short and long pulses for 0s and 1s."
3. **Send presets.** Turn the transmitter pot. Each new message appears on the dashboard, and the buzzer chirps.
4. **Live telemetry.** Turn to the LIVE slot. The Nano sends its own chip temperature and supply voltage, and the frame number matches the Nano's Serial log. Hold a finger on the Nano's chip and the temperature creeps up.
5. **Send a custom message.** Type it into the Nano's Serial Monitor with the pot fully clockwise. It appears on the phone.
6. **Break the beam.** Put a hand in the way mid-frame. The log shows a `truncated` frame, and link quality drops. "The checksum means we never display corrupted data."
7. **Calibration.** Shine a phone torch nearby or switch on a lamp. Show the threshold line adapting (auto), or turn the pot to compensate (manual).

## Concepts to explain

- **On-Off Keying (OOK):** the simplest digital modulation. The carrier (light) is either on or off.
- **Pulse-width encoding:** bits are told apart by duration, so the clock is recovered from the signal itself.
- **Framing:** a preamble for synchronisation, a length field and an end marker.
- **Error detection:** an XOR checksum catches any single-bit error.
- **Signal conditioning:** a voltage divider turns resistance into voltage. A threshold with hysteresis gives noise immunity, and adaptive thresholding handles changing room light.
- **Channel limits:** the LDR's response time limits bandwidth to about 29 bit/s. Real LiFi uses photodiodes and fast LEDs to reach Mbit/s to Gbit/s, which is why a phototransistor upgrade helps so much.
- **Real-time design on the ESP32:** sampling runs on its own high-priority FreeRTOS task, so serving web pages never disturbs pulse timing.

## Ideas to extend it

- Replace the LDR with a phototransistor and shorten every timing about 10×.
- Add an external sensor (DHT11, LM35, …) to the LIVE telemetry slot.
- Use Manchester encoding or a CRC-8 for better error detection.
- Add a reverse channel (IR LED on the ESP32, sensor on the Nano) for acknowledgements.
