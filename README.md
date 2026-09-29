# ESP32-S3 AI Voice Assistant

A push-to-talk voice assistant built on an ESP32-S3. Hold the BOOT button, ask a question in Hebrew or English, release, and the device answers aloud through a speaker. It supports live web search, real-time clock awareness, and conversation memory.

The ESP32 handles audio in and out. Speech recognition, the language model, and speech synthesis run in the cloud (OpenAI API).

## Pipeline

```
[BOOT held] -> INMP441 mic (I2S, 16 kHz) -> PSRAM buffer -> 80 Hz high-pass + normalization
     -> HTTPS -> Speech-to-text  (gpt-4o-mini-transcribe)
     -> HTTPS -> LLM             (gpt-6-luna + web_search + current Israel time via NTP)
     -> HTTPS -> Text-to-speech  (gpt-4o-mini-tts, 24 kHz PCM)
     -> PSRAM buffer -> MAX98357A amp (I2S, 24 kHz) -> speaker
```

**RGB LED status (GPIO 48):** blue = listening, yellow = thinking, green = speaking, red = error.

## Hardware

| Part | Model |
|---|---|
| Microcontroller | ESP32-S3-DevKitC-1 (N16R8: 16MB Flash, 8MB OPI PSRAM) |
| Breakout | ESP32-S3 GPIO Extension Board V2775 |
| Microphone | INMP441 MEMS, I2S (3.3V only) |
| Amplifier | MAX98357A Class-D, I2S, 3W (5V) |
| Speaker | 4–8 Ω |

## Wiring

| INMP441 | ESP32-S3 |
|---|---|
| SCK | GPIO 17 |
| WS | GPIO 16 |
| SD | GPIO 18 |
| L/R | GPIO 15 (driven LOW = left channel) |
| VDD / GND | 3.3V / GND |

| MAX98357A | ESP32-S3 |
|---|---|
| BCLK | GPIO 11 |
| LRC | GPIO 12 |
| DIN | GPIO 10 |
| VIN / GND | 5V / GND |
| SPEAK+ / SPEAK− | Speaker only (BTL output, never to GND) |

## Arduino IDE settings

| Setting | Value |
|---|---|
| Board package | esp32 by Espressif Systems (3.x) |
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | Disabled (when using the COM/UART USB port) |
| Flash Size | 16MB |
| PSRAM | **OPI PSRAM** (required) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| Library | ArduinoJson (Benoit Blanchon) |

## Setup

1. Open `assistant/assistant.ino` in the Arduino IDE.
2. Fill in `WIFI_SSID`, `WIFI_PASS` and `OPENAI_KEY` **locally**.
3. Upload, open the Serial Monitor at 115200 baud, and wait for `Ready`.
4. Hold BOOT, ask a question, release.

> **Security:** never commit your real API key or Wi-Fi password. The values in this repository are placeholders. `client.setInsecure()` skips TLS certificate validation, which is acceptable for a prototype but should be replaced with a pinned root CA in production.

## Measured performance

| Metric | Value |
|---|---|
| Speech-to-text | 1.7–3.0 s |
| LLM (incl. web search) | 3.9–5.5 s |
| Button release → speech | ~10 s |
| Recording SNR | 30–39 dB |

## Roadmap

- Stream TTS audio to cut ~4 s of latency
- Wake word instead of a button
- TFT display for question / answer / status
- Certificate validation and a separate secrets file
- Enclosure
