# ESP32-S3 AI Voice Assistant

A push-to-talk AI voice assistant built on an ESP32-S3 with a 2.4" color display. Hold the talk button, ask a question in Hebrew or English, release, and the device answers aloud while an animated chat interface shows the conversation, with Hebrew rendered right to left. It supports live web search, real-time clock awareness, conversation memory, and a rotary knob for volume.

The ESP32-S3 handles audio, display and controls. Speech recognition, the language model and speech synthesis run in the cloud (OpenAI API).

## Features

- **Push-to-talk** with K0 (on the display board) or BOOT
- **Cloud AI pipeline:** speech-to-text → LLM with web search and the current Israel time → text-to-speech
- **Conversation memory** between questions; **press the knob** to start a new chat
- **Volume knob** (EC11 rotary encoder, 10 steps, works while the assistant is speaking)
- **"Midnight Gradient" UI** on a 320×240 ST7789 display: status card, chat bubbles, clock, volume bar
- **Right-to-left Hebrew** rendering, with numbers and English kept in the correct order
- **Animations at 25 fps** (mic level meter, thinking dots, equalizer) in a FreeRTOS task on core 0, so they keep running during network requests
- **No idle hiss:** the amplifier is powered down through its SD pin whenever nothing is playing

## Architecture

```
           ┌──────────── core 1 (loop) ─────────────┐
[K0/BOOT] → Mic I2S 16 kHz (stereo, slot A) → PSRAM
          → STT → LLM (+web_search, time, memory) → TTS (PCM 24 kHz) → PSRAM
          → volume (knob) → Amp I2S 24 kHz STEREO → Speaker
          → screen: status, question bubble, answer bubble, volume bar

           ┌──────────── core 0 (animTask) ─────────┐
           every 40 ms: mic meter / thinking dots / equalizer
           (shares the SPI screen with core 1 via a mutex)

[Knob turn] → ISR (quadrature) → volume     [Knob press] → new chat
[AMP_SD = GPIO 13] HIGH only while playing → no idle hiss
```

## Hardware

| Part | Model |
|---|---|
| Microcontroller | ESP32-S3-DevKitC-1 (N16R8: 16MB Flash, 8MB OPI PSRAM) |
| Breakout | ESP32-S3 GPIO Extension Board V2775 |
| Display | ST7789 2.4" IPS, 320×240, 4-wire SPI, with EC11 encoder and K0 key |
| Microphone | INMP441 MEMS, I2S (3.3V only) |
| Amplifier | MAX98357A Class-D, I2S, 3W (5V) |
| Speaker | 4–8 Ω |

## Wiring

**INMP441 microphone**

| INMP441 | ESP32-S3 |
|---|---|
| SCK / WS / SD | GPIO 17 / 16 / 18 |
| L/R | GPIO 15 (driven LOW = left channel) |
| VDD / GND | 3.3V / GND |

**MAX98357A amplifier**

| MAX98357A | ESP32-S3 |
|---|---|
| LRC / BCLK / DIN | GPIO 12 / 11 / 10 |
| GAIN | GND (12 dB) |
| SD | GPIO 13 (on/off from code) |
| VIN / GND | 5V / GND |
| SPEAK+ / SPEAK− | Speaker only (BTL output, never to GND) |

**Display and controls**

| Display pin | ESP32-S3 | Function |
|---|---|---|
| SCL / SDA | GPIO 4 / 5 | SPI clock / data |
| RES / DC / CS | GPIO 6 / 7 / 1 | Reset / data-command / chip select |
| BLK | GPIO 2 | Backlight (HIGH = on) |
| TRA / TRB | GPIO 42 / 41 | Encoder A / B |
| PSH | GPIO 40 | Knob press (new chat) |
| K0 | GPIO 39 | Talk button |
| VDD / GND | 3.3V / GND | |

## Arduino IDE settings

| Setting | Value |
|---|---|
| Board package | esp32 by Espressif Systems (3.x) |
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | Disabled (when using the COM/UART USB port) |
| Flash Size | 16MB |
| PSRAM | **OPI PSRAM** (required) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| Libraries | ArduinoJson, GFX Library for Arduino (Arduino_GFX), U8g2 (Hebrew font only) |

## Setup

1. Open `assistant/assistant.ino` in the Arduino IDE. The `fonts.cpp` tab opens with it and is required for the Hebrew font.
2. Fill in `WIFI_SSID`, `WIFI_PASS` and `OPENAI_KEY` **locally**.
3. Upload, open the Serial Monitor at 115200 baud, and wait for `Ready` on the screen.
4. Hold K0 (or BOOT), ask a question, release. Turn the knob for volume; press it for a new chat.

**Display colors look inverted?** The 4th argument of `Arduino_ST7789(...)` controls IPS color inversion. This module needs `false`; some other ST7789 modules need `true`.

> **Security:** never commit your real API key or Wi-Fi password, and hide those lines in any screenshot. The values in this repository are placeholders. `client.setInsecure()` skips TLS certificate validation, which is acceptable for a prototype but should be replaced with a pinned root CA in production.

## Measured performance

| Metric | Value |
|---|---|
| Speech-to-text | 1.7–3.0 s |
| LLM (incl. web search) | 3.9–5.5 s |
| Button release → speech | ~10 s |
| Recording SNR | 30–39 dB |
| UI animation | 25 fps (40 ms per frame) |

## Roadmap

- Stream TTS audio to cut ~4 s of latency
- Certificate validation and a separate secrets file
- Enclosure
