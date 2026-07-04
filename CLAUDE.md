# ESP32 Smart Assistant (ESP32-S3-EYE v2.2)

## Overview
This project is a **real-time smart voice assistant** running on ESP32-S3-EYE v2.2.

The ESP32 acts as a **low-power edge streaming device**, responsible for:
- Live microphone audio streaming (NOT record-and-send)
- Camera snapshot capture (optional)
- WiFi communication with the AI engine
- Audio playback response
- LCD facial expression + status display
- Simple control logic (wake trigger, VAD, button)

All AI processing happens in the cloud: the firmware connects **directly to
the selected AI engine's realtime voice API** over one WebSocket — there is
no middle backend to host:
- **Gemini Live API** (`AI_ENGINE_GEMINI`, default) — `src/GeminiLive.cpp`
- **OpenAI Realtime API** (`AI_ENGINE_OPENAI`) — `src/OpenAiRealtime.cpp`

The engine is chosen with `AI_ENGINE` in `include/secrets.h` (API keys live
there too); both clients implement the common interface in
`include/AiEngine.h`. The provider runs VAD/endpointing + STT + LLM + TTS
server-side. Audio is raw mono 16-bit PCM both directions (base64 inside
JSON WebSocket messages) — no Opus/codec anywhere. Protocol details:
`README.md` "Wire protocol".

---

## Key Requirements (IMPORTANT)

### 1. Real-time streaming mode (PRIMARY MODE)

This project MUST support **live audio streaming over WebSocket**.

❌ NOT allowed as main flow:
- record audio → send file → wait response

✅ REQUIRED main flow:
- continuous microphone stream → AI engine → real-time AI response

### 2. Boot self-test BEFORE the main flow

On every boot the firmware MUST verify, with results shown on the LCD,
**before** entering the main flow:
- every pin/peripheral (PSRAM, LED, button, mic, camera; speaker is an
  intentional SKIP until an external amp is wired)
- configuration (secrets.h filled in — WiFi credentials AND the selected
  engine's API key — plus WiFi actually connects)

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
**The face is chosen by the model itself**: a `set_emotion(happy|sad|
neutral|thinking)` tool is registered with the engine and the system prompt
(`AI_SYSTEM_PROMPT` in `include/config.h`) tells the model to call it before
every reply; `Conversation.cpp` maps the call to a face.

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
   │ Continuous PCM audio stream (base64 in JSON, WebSocket)
   ▼
[AI engine — Gemini Live API or OpenAI Realtime API]
   ├── Streaming Speech-to-Text (server VAD + ASR)
   ├── LLM + set_emotion tool call (drives the LCD face)
   ├── TTS generation (streaming 24 kHz PCM)
   ▼
[ESP32 Response Layer]
   ├── Speaker playback (I2S, external amp)
   ├── LCD facial expression / LED feedback
   └── Optional camera snapshot
```

State machine: `IDLE → LISTENING → THINKING → SPEAKING → LISTENING …`
(continuous conversation until button press; see
`include/AssistantState.h`; wire protocol in `README.md`).

Uplink sample rate is engine-dependent (16 kHz Gemini / 24 kHz OpenAI —
`AUDIO_SAMPLE_RATE` in `config.h` follows `AI_ENGINE`); downlink TTS is
24 kHz for both.

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
