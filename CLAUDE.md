# ESP32 Smart Assistant (ESP32-S3-EYE v2.2)

## Overview
This project is a **real-time smart voice assistant** running on ESP32-S3-EYE v2.2.

The ESP32 acts as a **low-power edge streaming device**, responsible for:
- Live microphone audio streaming (NOT record-and-send)
- Camera snapshot capture (optional)
- WiFi communication with backend
- Audio playback response
- LCD facial expression + status display
- Simple control logic (wake trigger, VAD, button)

All AI processing is handled externally by a **xiaozhi backend**
(https://github.com/78/xiaozhi-esp32 — official xiaozhi.me cloud or a
self-hosted xiaozhi-esp32-server):
- Speech-to-Text (STT, server-side VAD/endpointing)
- Large Language Model (LLM)
- Text-to-Speech (TTS)

The firmware implements the xiaozhi WebSocket protocol v1 (Opus audio both
directions, hello handshake, listen/abort/tts/llm/stt JSON messages) in
`src/XiaozhiProtocol.cpp`, plus the first-boot OTA/activation flow
(`src/XiaozhiOta.cpp` — activation code shown on the LCD, entered at
xiaozhi.me). Protocol details: `README.md` + xiaozhi's `docs/websocket.md`.

---

## Key Requirements (IMPORTANT)

### 1. Real-time streaming mode (PRIMARY MODE)

This project MUST support **live audio streaming over WebSocket**.

❌ NOT allowed as main flow:
- record audio → send file → wait response

✅ REQUIRED main flow:
- continuous microphone stream → backend → real-time AI response

### 2. Boot self-test BEFORE the main flow

On every boot the firmware MUST verify, with results shown on the LCD,
**before** entering the main flow:
- every pin/peripheral (PSRAM, LED, button, mic, camera; speaker is an
  intentional SKIP until an external amp is wired)
- configuration (secrets.h filled in, WiFi actually connects, xiaozhi WS URL sane)

Implemented in `src/SelfTest.cpp`. A FAIL continues in degraded mode but must
be clearly visible (red row + red summary).

### 3. LCD facial expression display

All user-facing information goes to the onboard LCD, and it must be
**colorful and enjoyable** — expressive faces, not plain text:
- Smile (green) → positive / happy response
- Sad (red) → error / not understood
- Thinking (yellow) → processing / waiting
- Plus state faces: neutral/cyan (idle), listening/azure, speaking/orange

Implemented in `src/Display.cpp` (`displayShowFace`, `Expression` enum).
The backend can push emotions via `{"type":"emotion","value":...}`.

### 4. Pin changes require verification

Pins in `include/pins_config.h` tagged `[CONFIRMED — DO NOT CHANGE]` were
verified on real hardware via the pin-check firmware + an explicit human
"yes". Never change them without asking and re-verifying via
`./upload_project.sh --pincheck`.

---

## System Architecture

```
[ESP32-S3-EYE v2.2]
   │
   │ Continuous Opus Audio Stream (WebSocket, xiaozhi protocol v1)
   ▼
[xiaozhi backend — xiaozhi.me cloud or self-hosted]
   ├── Streaming Speech-to-Text (server VAD + ASR)
   ├── LLM processing (Qwen / DeepSeek / configurable)
   ├── TTS generation (streaming Opus)
   ▼
[ESP32 Response Layer]
   ├── Speaker playback (I2S, external amp)
   ├── LCD facial expression / LED feedback
   └── Optional camera snapshot
```

State machine: `IDLE → LISTENING → THINKING → SPEAKING → LISTENING …`
(continuous conversation until button press / server goodbye; see
`include/AssistantState.h`; wire protocol in `README.md`).

---

## Hardware facts (corrected — do not "fix" these back)

- Display: **1.3" 240×240 ST7789** (not 2.4"/320×240 as older docs claimed)
- PSRAM: 8 MB **octal** — needs `board_build.arduino.memory_type = qio_opi`
- No onboard speaker; mic on this specific board is a suspected hardware
  fault (see `PROGRESS.md` / `include/pins_config.h`)

---

## Development Environment

### PlatformIO (Required)

Built with **PlatformIO (VS Code extension)**. See `platformio.ini` for the
authoritative config (env `esp32-s3-eye`, plus `esp32-s3-eye-pincheck` for
the pin diagnostic firmware).

Build/flash via `./upload_project.sh` (`-b` build-only, `-p <port>`,
`-m` monitor, `--pincheck`).

### Progress tracking

`PROGRESS.md` is the source of truth for what is implemented and verified on
hardware — read it first, update it when a phase lands.
