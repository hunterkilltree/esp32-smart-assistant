# ESP32 Smart Assistant

A **real-time streaming voice assistant** for the **ESP32-S3-EYE v2.2** with an
expressive, colorful face on the onboard LCD.

The firmware connects **directly to an AI provider's realtime voice API** over
one WebSocket — no middle backend to run. Pick the engine in `secrets.h`:

- **Gemini Live API** (`AI_ENGINE_GEMINI`) — free-tier API key from
  [Google AI Studio](https://aistudio.google.com/apikey)
- **OpenAI Realtime API** (`AI_ENGINE_OPENAI`) — paid API key from
  [platform.openai.com](https://platform.openai.com/api-keys)

Live microphone PCM streams up; the provider runs VAD + ASR + LLM + TTS
server-side and streams spoken PCM audio back. The model is given a
`set_emotion(emotion, text)` tool and calls it before every reply — that tool
call picks the face (smile / sad / neutral) shown on the LCD **and** puts a
short caption of the reply (max ~6 words, in the reply's language) under the
face for the whole spoken turn. All heavy AI work happens in the cloud, never
on the device.

```
[ESP32-S3-EYE v2.2]
   │
   │  16/24 kHz PCM (base64 in JSON, WebSocket) — both directions
   ▼
[AI engine]  (Gemini Live API  or  OpenAI Realtime API)
   ├── Server-side VAD + streaming ASR
   ├── LLM + set_emotion tool call  ──►  LCD face + short caption text
   ├── Streaming TTS (24 kHz PCM)
   ▼
[ESP32 Response Layer]
   ├── Speaker playback (I2S, external amp)
   ├── LCD facial expression + status
   └── RGB LED feedback
```

## Features

- **Live audio streaming** — continuous mono 16-bit PCM (16 kHz for Gemini,
  24 kHz for OpenAI) in 60 ms chunks over WebSocket while listening.
  Record-then-send is explicitly *not* the main flow.
- **No backend to host** — the device talks straight to the provider; the
  only setup is pasting an API key into `secrets.h`.
- **Model-driven emotions + on-screen text** — a `set_emotion(emotion,
  text)` function (emotion: happy|sad|neutral|thinking) is registered with
  the engine; the system prompt tells the model to call it before every
  reply. The firmware maps the call to the face and word-wraps the short
  `text` caption under it, keeping both on screen while the reply plays
  (e.g. a sad message shows the red sad face plus a short sorry text).
- **Boot self-test** — every pin, peripheral, and configuration value
  (WiFi, API key) is verified **before the main flow starts**, with results
  shown on the LCD as a colored PASS / WARN / FAIL / SKIP checklist plus a
  summary screen.
- **Expressive LCD face** — assistant states and model emotions are drawn
  as colorful faces, with a status bar and live WiFi/WebSocket dots.
- **Push-to-talk rounds with a held result** — one button press opens the
  mic for one exchange; when the reply finishes, its face + caption stay
  on screen so you can read them at your own pace. Press the button again
  to talk again (a press during a reply aborts it).
- **Reliability** — WiFi reconnect with exponential backoff, WS
  auto-reconnect + session re-setup, ArduinoOTA updates, task watchdog.

## Buttons

The board has 6 buttons. RST is hardwired reset; the other five all do
something:

| Button | Wiring | Function |
|---|---|---|
| **BOOT** | GPIO0 | Talk: start / stop / barge-in a conversation |
| **PLAY** | ADC ladder, GPIO1 | Same as BOOT (talk), without the strapping-pin quirks |
| **UP+** | ADC ladder, GPIO1 | Volume +10% (software gain, saved to flash, on-screen bar) |
| **DN−** | ADC ladder, GPIO1 | Volume −10% |
| **MENU** | ADC ladder, GPIO1 | Toggle info screen: state, WiFi/IP/RSSI, AI link status, engine, volume, heap |

MENU/PLAY/UP/DN share one pin — a resistor ladder on GPIO1 where each
button produces a distinct voltage. The mV windows in `config.h`
(`ADC_BTN_*`) are `[UNCONFIRMED]` guesses: run the pin-check firmware,
press each button, read its real mV off the LCD, and tighten the windows.
Presses that match no window are logged with their measured mV.

## LCD facial expressions

| Expression | Color | Shown when |
|---|---|---|
| Neutral (soft smile) | Cyan | `IDLE` — ready, or model emotion `neutral` |
| Listening (big eyes) | Azure | `LISTENING` — mic open, streaming |
| Thinking (flat mouth + dots) | Yellow | `THINKING` — model working on a reply |
| Speaking (open mouth) | Orange | `SPEAKING` — TTS audio playing |
| Happy (wide smile) | Green | model called `set_emotion("happy")` |
| Sad (frown + tear) | Red | model called `set_emotion("sad")` |

## State machine

`IDLE → LISTENING → THINKING → SPEAKING → RESULT (response held) → LISTENING → …`

- **BOOT button** in `IDLE` starts a round — the mic streams continuously
  and the engine does the endpointing server-side (no on-device wake word,
  see `PROGRESS.md`).
- `LISTENING → THINKING` when the engine reports the utterance ended
  (OpenAI `speech_stopped`; Gemini has no explicit event, so the face jumps
  straight to the reply).
- `THINKING → SPEAKING` on the first TTS audio chunk; the `set_emotion`
  tool call lands just before it and sets the face + caption, which stay
  on screen until the turn ends (or is aborted/interrupted).
- `SPEAKING → RESULT` when the turn completes — the reply's face + caption
  stay on screen ("BOOT/PLAY = talk" footer), surviving WS reconnects,
  until the next button press returns to `LISTENING`.
- BOOT button during `LISTENING` stops; during `THINKING`/`SPEAKING` aborts.
- `THINKING → IDLE` automatically after 15 s if the engine never answers.

## Boot self-test

On every boot, after the splash screen, the firmware runs a self-test and
paints each result on the LCD **before entering the main flow**:

| Check | Verifies | On this board |
|---|---|---|
| `PSRAM` | Octal PSRAM detected (camera needs it) | PASS, 8 MB |
| `LED` | RGB status LED cycles red/green/blue | PASS (visual) |
| `BUTTON` | Wake button reads released, not stuck | PASS |
| `CONFIG` | `secrets.h`: WiFi + selected engine's API key filled in | PASS once filled in |
| `WIFI` | Connects to the AP within 15 s, shows IP | PASS with real credentials |
| `MIC` | I2S capture delivers live, non-flat PCM | PASS (see known issues) |
| `SPKR` | — | SKIP by design (no amp wired) |
| `CAMERA` | Sensor init + one real JPEG capture | PASS with byte count |

A failed check does not brick the device — it continues in degraded mode,
but the red FAIL row and summary tell you exactly what to fix.

## Hardware

| Component | Details |
|---|---|
| MCU | ESP32-S3 (ESP32-S3-WROOM-1, 8 MB flash, 8 MB **octal** PSRAM) |
| Camera | OV2640 |
| Display | **1.3" 240×240 ST7789** LCD (SPI) |
| Microphone | MSM261S4030H0R digital I2S mic |
| Speaker | **None onboard** — requires an external I2S amp (pins unverified) |
| Wake button | BOOT button (GPIO0, doubles as strapping pin) |
| USB | Native USB (CDC) |
| WiFi | 2.4 GHz |

Pin assignments live in `include/pins_config.h` with a verification legend:
pins are only tagged `[CONFIRMED — DO NOT CHANGE]` after the dedicated
pin-check firmware proved them on real hardware with an explicit human
confirmation. **Do not change confirmed pins without re-running pin-check.**

## Wire protocol

Both engines follow the same shape — one TLS WebSocket, a JSON setup
message, then base64 PCM chunks both ways — implemented behind the common
interface in `include/AiEngine.h`:

- **Gemini Live** (`src/GeminiLive.cpp`) — WSS to
  `generativelanguage.googleapis.com` (`BidiGenerateContent`, key in the
  URL). A `setup` message configures the model, system prompt, and the
  `set_emotion` tool; mic audio goes up as `realtimeInput` (16 kHz PCM),
  replies come back in `serverContent` messages (24 kHz PCM +
  `turnComplete`/`interrupted` flags), tool calls arrive as `toolCall`.
  [Docs](https://ai.google.dev/api/live)
- **OpenAI Realtime** (`src/OpenAiRealtime.cpp`) — WSS to
  `api.openai.com/v1/realtime` (`Authorization: Bearer` header). A
  `session.update` event configures voice, server VAD, prompt, and the
  tool; mic audio goes up as `input_audio_buffer.append`, replies come
  back as `response.output_audio.delta` events, tool calls as
  `response.function_call_arguments.done`.
  [Docs](https://platform.openai.com/docs/guides/realtime)

Session setup must complete within 10 s or the socket is recycled; a
dropped socket auto-reconnects and re-runs setup (conversation context is
lost on reconnect).

## Getting started

Requires [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

```bash
# 1. Create your secrets file (auto-created from the template on first build)
cp include/secrets.h.example include/secrets.h
#    ...then edit include/secrets.h:
#      - WiFi credentials
#      - AI_ENGINE (AI_ENGINE_GEMINI or AI_ENGINE_OPENAI)
#      - the matching API key (GEMINI_API_KEY / OPENAI_API_KEY)

# 2. Build + flash (auto-detects the port; -p COM3 to force one)
./upload_project.sh -p COM3

# 3. Optional: open the serial monitor after upload
./upload_project.sh -p COM3 -m
```

The board shows the rainbow splash, runs the self-test checklist, connects
to the engine, and shows the green "Connected!" face. Press BOOT and talk.

### Pin-check diagnostic firmware

A standalone diagnostic build tests each pin in isolation with an obvious
expected result (LED colors, LCD text, button window, mic clap test, speaker
tone, camera capture):

```bash
./upload_project.sh --pincheck -p COM3
```

## Project structure

```
include/
  pins_config.h      # all pin assignments + verification status legend
  config.h           # engine/model selection, prompt, timing, audio, self-test tunables
  secrets.h.example  # template for WiFi + AI engine + API keys (secrets.h is gitignored)
  AssistantState.h   # IDLE / LISTENING / THINKING / SPEAKING
  AiEngine.h         # common engine interface (callbacks, connect, audio in/out)
src/
  main.cpp           # thin boot sequence + loop pipeline (each stage is a module)
  AppState.cpp       # state machine + face/LED feedback
  AdcButtons.cpp     # 4-button ADC ladder driver (GPIO1): debounce + mV log
  Controls.cpp       # PLAY=talk, UP/DN=volume, MENU=info screen
  WifiLink.cpp       # WiFi reconnect/backoff + ArduinoOTA servicing
  BackendSession.cpp # engine WebSocket bring-up
  Conversation.cpp   # engine callbacks, button, mic->engine pump, timeouts
  GeminiLive.cpp     # Gemini Live API client (AI_ENGINE_GEMINI)
  OpenAiRealtime.cpp # OpenAI Realtime API client (AI_ENGINE_OPENAI)
  SelfTest.cpp       # boot self-test (pins, peripherals, config) on the LCD
  Display.cpp        # ST7789 UI: splash, checklist, faces, info screen
  AudioCapture.cpp   # I2S mic RX task -> 60 ms PCM chunks
  AudioPlayback.cpp  # PCM ring buffer -> I2S TX (skipped while no speaker pins)
  CameraCapture.cpp  # OV2640 init + JPEG snapshot (self-test/pin-check only)
  Reliability.cpp    # watchdog, ArduinoOTA
  PinCheckMain.cpp   # standalone pin-check firmware (separate env)
```

The `loop()` is a fixed four-stage pipeline — each stage returns early if its
layer isn't ready, so a bug is isolated to whichever module's log prefix
stops appearing: `wifiLinkLoop()` → `backendSessionLoop()` → `aiEngineLoop()`
→ `conversationLoop()`.

## Known issues

- **Onboard mic is a suspected hardware fault on this specific board** —
  pins are triple-verified against the official BSP, esp-skainet, and the
  v2.2 schematic; the I2S link is electrically alive but never responds to
  sound. Full investigation in `include/pins_config.h` and `PROGRESS.md`.
  Until that's resolved (continuity/oscilloscope check, or an external I2S
  mic), the assistant can't actually hear you — the whole pipeline runs,
  but the engine only ever receives silence.
- **Speaker pins are unverified guesses** — the stock board has no speaker
  interface at all. Wire an external I2S amp and confirm pins via pin-check
  first (GPIO46 is a strapping pin — risky at boot). Until then TTS replies
  are received but inaudible.
- **TLS is unpinned** — the WS client uses `setInsecure()` (no CA
  validation), like most hobby firmware. Pin the provider's root CA via
  `beginSslWithCA` if you care.
- **Voice barge-in needs an open mic** — the mic only streams while
  `LISTENING`, so interrupting a reply currently requires the button, not
  your voice. Trivial to change once the mic hardware actually works.
- **Camera / vision is idle** — snapshots still work in the self-test, but
  no image is sent to the engine yet.

## Roadmap

- Fix or replace the mic (hardware) → first real end-to-end conversation
- External I2S amp for audible TTS
- Camera snapshots to the engine (both APIs accept images) — "what am I
  looking at?"
- On-device wake word (ESP-SR) — needs an ESP-IDF-based build restructure

## Libraries

| Purpose | Library |
|---|---|
| WebSocket streaming | `links2004/WebSockets` |
| JSON (setup, events, base64 audio envelopes) | `bblanchon/ArduinoJson` |
| RGB LED | `adafruit/Adafruit NeoPixel` |
| Button debounce | `thomasfredericks/Bounce2` |
| LCD UI | `adafruit/Adafruit GFX` + `Adafruit ST7735 and ST7789` |

WiFi, I2S, base64 (mbedTLS), camera, ring buffers, OTA, and the watchdog
come from the `arduino-esp32` core — no audio codec library is needed since
both engines speak raw PCM.
