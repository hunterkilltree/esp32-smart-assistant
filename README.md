# ESP32 Smart Assistant

A **real-time streaming voice assistant** for the **ESP32-S3-EYE v2.2** with an
expressive, colorful face on the onboard LCD.

The firmware speaks the **[xiaozhi](https://github.com/78/xiaozhi-esp32)
WebSocket protocol**: it streams live Opus-encoded microphone audio to a
xiaozhi backend — the free official **xiaozhi.me** cloud or a self-hosted
[xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) —
which runs VAD + ASR + LLM + TTS and streams Opus TTS audio back. All heavy
AI work happens on the backend, never on the device.

```
[ESP32-S3-EYE v2.2]
   │
   │  Opus audio frames (WebSocket, binary) + JSON control messages
   ▼
[xiaozhi backend]  (xiaozhi.me cloud or self-hosted)
   ├── Server-side VAD + streaming ASR
   ├── LLM (Qwen / DeepSeek / configurable)
   ├── Streaming TTS
   ▼
[ESP32 Response Layer]
   ├── Speaker playback (Opus decode → I2S, external amp)
   ├── LCD facial expression + status (driven by server `llm` emotions)
   └── RGB LED feedback
```

## Features

- **Live audio streaming** — continuous 16 kHz mono PCM, Opus-encoded in
  60 ms frames, over WebSocket while listening. Record-then-send is
  explicitly *not* the main flow.
- **Real backend, zero backend work** — first boot shows a 6-digit
  activation code on the LCD; enter it at [xiaozhi.me](https://xiaozhi.me)
  (Console → Add Device) and the assistant is live against the free
  official cloud. Self-hosted servers work by changing two URLs in
  `secrets.h`.
- **Boot self-test** — every pin, peripheral, and configuration value is
  verified **before the main flow starts**, with results shown on the LCD as
  a colored PASS / WARN / FAIL / SKIP checklist plus a summary screen.
- **Expressive LCD face** — the assistant state and server-sent `llm`
  emotions (`happy`, `sad`, `thinking`, …) are drawn as colorful faces,
  with a status bar and live WiFi/WebSocket connectivity dots.
- **Continuous conversation** — one button press opens a session; after
  each TTS reply the mic reopens automatically until you press the button
  again (or the server says goodbye). A press during a reply barges in.
- **Reliability** — WiFi reconnect with exponential backoff, WS
  auto-reconnect + hello re-handshake, ArduinoOTA updates, task watchdog.

## Buttons

The board has 6 buttons. RST is hardwired reset; the other five all do
something:

| Button | Wiring | Function |
|---|---|---|
| **BOOT** | GPIO0 | Talk: start / stop / barge-in a conversation |
| **PLAY** | ADC ladder, GPIO1 | Same as BOOT (talk), without the strapping-pin quirks |
| **UP+** | ADC ladder, GPIO1 | Volume +10% (software gain, saved to flash, on-screen bar) |
| **DN−** | ADC ladder, GPIO1 | Volume −10% |
| **MENU** | ADC ladder, GPIO1 | Toggle info screen: state, WiFi/IP/RSSI, WS status, Device-Id, volume, heap |

MENU/PLAY/UP/DN share one pin — a resistor ladder on GPIO1 where each
button produces a distinct voltage. The mV windows in `config.h`
(`ADC_BTN_*`) are `[UNCONFIRMED]` guesses: run the pin-check firmware,
press each button, read its real mV off the LCD, and tighten the windows.
Presses that match no window are logged with their measured mV.

## LCD facial expressions

| Expression | Color | Shown when |
|---|---|---|
| Neutral (soft smile) | Cyan | `IDLE` — ready | 
| Listening (big eyes) | Azure | `LISTENING` — mic open, streaming |
| Thinking (flat mouth + dots) | Yellow | `THINKING` — server transcribed, LLM working |
| Speaking (open mouth) | Orange | `SPEAKING` — TTS audio playing |
| Happy (wide smile) | Green | server emotion `happy`/`laughing`/`loving`/… |
| Sad (frown + tear) | Red | server emotion `sad`/`crying`/`angry`/… |

## State machine

`IDLE → LISTENING → THINKING → SPEAKING → LISTENING (continuous) → … → IDLE`

- **BOOT button** in `IDLE` starts a conversation (`listen start`, mode
  `auto` — the server does the endpointing; no on-device wake word, see
  `PROGRESS.md`).
- `LISTENING → THINKING` when the server sends the `stt` transcription.
- `THINKING → SPEAKING` on `tts start`; TTS Opus frames stream in.
- `SPEAKING → LISTENING` on `tts stop` while the conversation is active.
- BOOT button during `LISTENING` stops; during `THINKING`/`SPEAKING` aborts.
- `THINKING → IDLE` automatically after 15 s if the server never answers.

## First-boot activation (official cloud)

1. Flash, connect WiFi (via `secrets.h`).
2. After the self-test, the device POSTs its identity to the xiaozhi OTA
   endpoint. An unbound device gets back a **6-digit code**, shown big on
   the LCD.
3. Register at [xiaozhi.me](https://xiaozhi.me), open the console, choose
   *Add Device*, and enter the code.
4. The device re-polls every 10 s (BOOT button forces an immediate
   re-check), sees it's bound, connects the WebSocket, and shows the green
   "Connected!" face. Done — press BOOT and talk.

## Boot self-test

On every boot, after the splash screen, the firmware runs a self-test and
paints each result on the LCD **before entering the main flow**:

| Check | Verifies | On this board |
|---|---|---|
| `PSRAM` | Octal PSRAM detected (camera needs it) | PASS, 8 MB |
| `LED` | RGB status LED cycles red/green/blue | PASS (visual) |
| `BUTTON` | Wake button reads released, not stuck | PASS |
| `CONFIG` | `secrets.h` filled in, WS URL is ws(s):// | PASS once filled in |
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

## Wire protocol (xiaozhi, WebSocket transport, protocol version 1)

Spec: [xiaozhi-esp32/docs/websocket.md](https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md).
Implemented in `src/XiaozhiProtocol.cpp`.

- **Connect headers**: `Authorization: Bearer <token>`,
  `Protocol-Version: 1`, `Device-Id: <mac>`, `Client-Id: <persistent uuid>`.
- **Handshake**: client sends
  `{"type":"hello","version":1,"transport":"websocket","audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}}`;
  server answers with its own `hello` carrying `session_id` and the
  downlink `sample_rate` (24 kHz on the official cloud). 10 s timeout.
- **Binary frames**: one raw Opus frame each — device→server 16 kHz mono
  60 ms, server→device at the hello-announced rate.
- **Device → server JSON**: `listen` (`start`/`stop`, mode `auto`), `abort`.
- **Server → device JSON**: `stt` (transcription), `llm` (`emotion` → face),
  `tts` (`start`/`stop`/`sentence_start`), `goodbye`, `system` (reboot).

The device identity (`Device-Id` = WiFi MAC, `Client-Id` = UUID persisted in
NVS) is also sent as headers on the OTA/activation check
(`src/XiaozhiOta.cpp`), which returns the WebSocket URL/token and, for an
unbound device, the activation code.

## Getting started

Requires [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

```bash
# 1. Create your secrets file (auto-created from the template on first build)
cp include/secrets.h.example include/secrets.h
#    ...then edit include/secrets.h: WiFi credentials. The xiaozhi URLs
#    default to the official cloud; point them at your own server if you
#    self-host.

# 2. Build + flash (auto-detects the port; -p COM3 to force one)
./upload_project.sh -p COM3

# 3. Optional: open the serial monitor after upload
./upload_project.sh -p COM3 -m
```

The board shows the rainbow splash, runs the self-test checklist, then either
the activation-code screen (first boot) or the cyan "Ready" face.

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
  config.h           # timing, audio/Opus, self-test tunables
  secrets.h.example  # template for WiFi + xiaozhi endpoints (secrets.h is gitignored)
  AssistantState.h   # IDLE / LISTENING / THINKING / SPEAKING
src/
  main.cpp           # thin boot sequence + loop pipeline (each stage is a module)
  AppState.cpp       # state machine + face/LED feedback
  AdcButtons.cpp     # 4-button ADC ladder driver (GPIO1): debounce + mV log
  Controls.cpp       # PLAY=talk, UP/DN=volume, MENU=info screen
  WifiLink.cpp       # WiFi reconnect/backoff + ArduinoOTA servicing
  BackendSession.cpp # OTA check -> activation code screen -> WS connect
  Conversation.cpp   # protocol callbacks, button, mic->server pump, timeouts
  XiaozhiProtocol.cpp# xiaozhi WS protocol: hello, listen/abort, tts/llm/stt, opus frames
  XiaozhiOta.cpp     # OTA/config check + first-boot activation code
  SelfTest.cpp       # boot self-test (pins, peripherals, config) on the LCD
  Display.cpp        # ST7789 UI: splash, checklist, faces, activation screen
  AudioCapture.cpp   # I2S mic RX task + Opus encode task (60ms frames)
  AudioPlayback.cpp  # Opus decode task + I2S TX (skipped while no speaker pins)
  CameraCapture.cpp  # OV2640 init + JPEG snapshot (self-test/pin-check only)
  Reliability.cpp    # watchdog, ArduinoOTA
  PinCheckMain.cpp   # standalone pin-check firmware (separate env)
```

The `loop()` is a fixed four-stage pipeline — each stage returns early if its
layer isn't ready, so a bug is isolated to whichever module's log prefix
stops appearing: `wifiLinkLoop()` → `backendSessionLoop()` → `xzLoop()` →
`conversationLoop()`.

## Known issues

- **Onboard mic is a suspected hardware fault on this specific board** —
  pins are triple-verified against the official BSP, esp-skainet, and the
  v2.2 schematic; the I2S link is electrically alive but never responds to
  sound. Full investigation in `include/pins_config.h` and `PROGRESS.md`.
  Until that's resolved (continuity/oscilloscope check, or an external I2S
  mic), the assistant can't actually hear you — the whole pipeline runs,
  but the server only ever receives silence.
- **Speaker pins are unverified guesses** — the stock board has no speaker
  interface at all. Wire an external I2S amp and confirm pins via pin-check
  first (GPIO46 is a strapping pin — risky at boot). Until then TTS replies
  are received and decoded but inaudible.
- **TLS is unpinned** — the WS/HTTPS clients use `setInsecure()` (no CA
  validation), like most hobby firmware. Pin the ISRG root via
  `beginSslWithCA` if you care.
- **Camera / vision is idle** — snapshots still work in the self-test, but
  the xiaozhi vision path needs the MCP feature, which this firmware
  doesn't implement yet.

## Roadmap

- Fix or replace the mic (hardware) → first real end-to-end conversation
- External I2S amp for audible TTS
- MCP device tools (vision/camera, LED control, volume)
- On-device wake word (ESP-SR) — needs an ESP-IDF-based build restructure

## Libraries

| Purpose | Library |
|---|---|
| WebSocket streaming | `links2004/WebSockets` |
| Opus encode/decode | `pschatzmann/arduino-libopus` |
| Control JSON | `bblanchon/ArduinoJson` |
| RGB LED | `adafruit/Adafruit NeoPixel` |
| Button debounce | `thomasfredericks/Bounce2` |
| LCD UI | `adafruit/Adafruit GFX` + `Adafruit ST7735 and ST7789` |

WiFi, I2S, camera, ring buffers, OTA, and the watchdog come from the
`arduino-esp32` core.
