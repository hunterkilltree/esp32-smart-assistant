# Build Progress

Tracks implementation status by phase. Update a checkbox to `[x]` (with date) as soon as that phase is implemented and verified — this file is the source of truth for "where did we leave off."

Goal: real-time streaming voice assistant on ESP32-S3-EYE v2.2 (continuous mic → WebSocket → AI engine → streamed audio response). Record-then-send is explicitly disallowed as the main flow — see `CLAUDE.md`.

## Phases

- [x] **Phase 0 — Environment** (2026-07-03): `platformio.ini` env renamed to `esp32-s3-eye`, build_flags (PSRAM/camera/USB-CDC) added, lib_deps added. Builds clean.
- [x] **Phase 1 — Connectivity & control core** (2026-07-03): WiFi connect, `WebSocketsClient`, `AssistantState` (idle/listening/speaking) state machine, `Bounce2` wake button, `Adafruit_NeoPixel` status LED. See `src/main.cpp`.
- [x] **Phase 2 — Audio capture** (2026-07-03): I2S mic RX + FreeRTOS task in `src/AudioCapture.cpp`, energy-based (RMS) VAD for local diagnostics only, continuous PCM chunks streamed while `LISTENING`.
- [x] **Phase 3 — Audio playback** (2026-07-03): I2S TX to external amp, byte ring buffer (`freertos/ringbuf.h`) decouples incoming audio from I2S write cadence. See `src/AudioPlayback.cpp`.
- [x] **Phase 4 — Camera snapshot** (2026-07-03): `esp32-camera` init with `CAMERA_MODEL_ESP32S3_EYE` pins, JPEG capture proven. Now used by self-test/pin-check only (see `src/CameraCapture.cpp`); sending snapshots to the AI engine is future work.
- [x] **Phase 5 — Wake word upgrade** (2026-07-03): **Deferred, not implemented.** ESP-SR (WakeNet+AFE) requires ESP-IDF components that don't integrate cleanly with a PlatformIO `framework = arduino` build — would likely need `framework = espidf` with Arduino-as-component, a separate project restructure. Button-press wake trigger (Phase 1) remains the wake mechanism. Revisit if on-device wake word becomes a hard requirement.
- [x] **Phase 6 — Reliability** (2026-07-03): WiFi reconnect with exponential backoff (2s → 30s cap), `ArduinoOTA` (starts after first successful WiFi connect), task watchdog (10s timeout, panics/reboots on stall, fed once per `loop()`). See `src/Reliability.cpp`.
- [x] **LCD display (added post-roadmap, 2026-07-03)**: onboard ST7789 LCD shows `"init"` as the very first thing in `setup()` — a quick visual check that a freshly flashed board is running the new firmware. **Superseded by Phase 7's face UI (plain-text `displayShowStatus` kept only for the pin-check firmware).**
- [x] **Phase 7 — Boot self-test + colorful face UI (2026-07-03)**: (a) `src/SelfTest.cpp`: on-boot checklist shown on the LCD **before the main flow** — PSRAM, LED cycle, button-not-stuck, secrets.h-not-placeholder, WiFi connect (15s timeout, shows IP), mic I2S data liveness, speaker (intentional SKIP — no amp), camera init + real JPEG capture — with colored PASS/WARN/FAIL/SKIP rows and a verdict summary screen (counts, IP; holds until BOOT press or timeout). Self-test owns the audio/camera driver inits; watchdog is armed only after it so the blocking checks can't trip it. (b) `src/Display.cpp`: rainbow splash, checklist UI, and expressive faces drawn with GFX primitives — neutral/cyan (idle), listening/azure (big eyes), thinking/yellow (flat mouth + thought dots), speaking/orange (open mouth), happy/green (wide smile), sad/red (frown + tear) — plus a status bar and WiFi/WS connectivity dots. (c) `THINKING` state in `AssistantState`. (d) Screen pacing centralized in `config.h` (splash 3s, 700ms pause per checklist row, checklist holds 10s, summary 15s — every hold skippable with BOOT via `holdScreen()`). **Flashed to hardware 2026-07-03**, LCD visuals confirmed by the user.
- [x] **Phase 8 — Realtime protocol layer + module split (2026-07-03)**: the hand-rolled WS protocol was replaced with a real streaming session layer (TLS WebSocket, JSON setup handshake with a 10s timeout → auto-reconnect, server-side VAD/endpointing — local VAD is diagnostic only), and `main.cpp` was split into single-purpose modules for easier debugging — `AppState` (state machine + face/LED), `WifiLink` (reconnect/backoff + ArduinoOTA), `BackendSession` (connect phases), `Conversation` (engine callbacks, button, uplink pump, timeouts); `loop()` is a 4-stage early-return pipeline. Continuous-conversation state machine (button starts session → server VAD → THINKING → SPEAKING → back to LISTENING; button = stop/barge-in), emotion→face mapping, 60s listening safety timeout, `ensureBacklight()` on every full-screen draw (GPIO48 backlight re-asserted so nothing can leave the panel dark). Speaker pins commented out in `pins_config.h` (no amp wired); `AudioPlayback.cpp` compiles without them (`HAS_SPEAKER` guard) and discards TTS audio with a boot log line instead of installing the I2S TX driver. Flashed and serial-verified on hardware 2026-07-03: full self-test **7 pass / 0 warn / 0 fail**, WiFi connects, WS + session-setup flow runs as designed. *(This phase originally targeted a third-party backend; Phase 10 replaced it with direct AI-provider APIs — same session/state architecture.)*
- [x] **Phase 9 — 4-button controls (2026-07-03)**: the four front buttons (MENU/PLAY/UP/DN) share an ADC resistor ladder on **GPIO1** (ADC1_CH0, per the v2.2 schematic — also the reason the old speaker BCLK=1 guess is retired for good). New `src/AdcButtons.cpp` (self-throttled poll, 2-sample debounce, edge events, logs the measured mV of every press — unclassified presses print their mV so the `[UNCONFIRMED]` `ADC_BTN_*` windows in `config.h` can be calibrated) + `src/Controls.cpp`: **PLAY** = second talk button (merged with BOOT), **UP/DN** = volume ±10% (software gain on TTS playback, persisted in NVS, on-screen bar overlay for 1.5s), **MENU** = toggle info/debug screen (state, WiFi SSID/RSSI/IP, AI link status, engine name, volume, free heap — `displayShowInfo`). Pincheck got a `PRESS 4BTN` calibration test (8s window, live mV on the LCD, button name when a window matches). Flashed + serial-verified 2026-07-03 (self-test 7/7, volume restored from NVS); **the mV windows still need the pincheck calibration pass with a human pressing each button**.
- [x] **Pin audit vs. board pin map (`pin_io.png`, 2026-07-03)**: cross-checked every pin in `pins_config.h` against the board's pin-group map. Confirmed matches: mic (41/42/2), LCD data (48/47/44/21/43), camera (all), buttons (ADC ladder on GPIO1 + BOOT on GPIO0). Corrections: **`PIN_LCD_RST` reverted 45 → -1** — the LCD has no reset wire (only five LCD signals exist) and GPIO45 is a *strapping pin* (VDD_SPI flash-voltage select; driving it during a reset can brick the boot until power-cycle). New facts documented: **BAT_DET on GPIO14** (`PIN_BAT_DET`, unused so far), USB on 19/20, SD on 38/39/40 — which are the *only* free GPIOs on the board, so the future external-amp pins should be **38/39/40** (all three old speaker guesses were taken: 1=buttons, 14=battery, 46=strapping).

- [x] **Phase 10 — Direct AI engine integration: Gemini Live / OpenAI Realtime (2026-07-04)**: removed the third-party backend protocol entirely (protocol + OTA/activation modules deleted) and replaced it with **direct WebSocket clients for the AI providers' realtime voice APIs**, selectable at compile time via `AI_ENGINE` in `secrets.h`:
  - **Common interface** `include/AiEngine.h` (init/connect/loop/sendAudio/abort + callbacks: ready, disconnected, audio, user-speech-end, turn-complete, interrupted, emotion). Both clients compile against it; the unselected one compiles to an empty translation unit via `#if AI_ENGINE == ...` guards.
  - **`src/GeminiLive.cpp`** — WSS to `generativelanguage.googleapis.com` `BidiGenerateContent` (API key in the URL): `setup` message (model `gemini-2.0-flash-live-001` by default, AUDIO response modality, system prompt, `set_emotion` function declaration) → `setupComplete`; uplink `realtimeInput.audio` (base64 16 kHz PCM); downlink `serverContent` (`inlineData` 24 kHz PCM decoded in slices, `turnComplete`, `interrupted`), `toolCall` → `toolResponse`, `goAway` logged.
  - **`src/OpenAiRealtime.cpp`** — WSS to `api.openai.com/v1/realtime?model=gpt-realtime` (Bearer header): `session.created` → `session.update` (audio/pcm 24 kHz both ways, `server_vad`, voice, prompt, `set_emotion` tool) → `session.updated`; uplink `input_audio_buffer.append`; downlink `response.output_audio.delta` (legacy `response.audio.delta` also handled), `speech_started` → barge-in, `speech_stopped` → THINKING, `response.function_call_arguments.done` → emotion + `function_call_output` + follow-up `response.create` (its extra `response.done` is suppressed so end-of-turn fires once).
  - **Emotion via tool call**: `AI_SYSTEM_PROMPT` (config.h) instructs the model to call `set_emotion(happy|sad|neutral|thinking)` before every reply; `Conversation.cpp` maps it to the LCD faces. Stale emotions after a button-abort are ignored (`s_discardTurn` also swallows in-flight TTS audio of the aborted turn).
  - **Opus removed end-to-end**: both APIs speak raw PCM, so the codec, its two 32 KB-stack tasks, and the `arduino-libopus` dependency are gone. `AudioCapture` now queues plain PCM chunks (16 kHz Gemini / 24 kHz OpenAI — `AUDIO_SAMPLE_RATE` follows `AI_ENGINE`); `AudioPlayback` is a plain PCM ring (64 KB, fixed 24 kHz, volume gain applied at write-out). Base64 via mbedTLS. `-DWEBSOCKETS_MAX_DATA_SIZE=65536` because TTS JSON messages can exceed the lib's 15 KB default.
  - `secrets.h` now carries `AI_ENGINE` + `GEMINI_API_KEY`/`OPENAI_API_KEY` (+ optional model overrides). Self-test CONFIG check validates the selected engine's key; boot summary + MENU info screen show the engine name. NVS volume moved to namespace `assistant` (one-time reset to default). `BackendSession` reduced to connect→running (no activation flow; activation screen removed from `Display`).
  - **Build-verified 2026-07-04**: main env with `AI_ENGINE_GEMINI`, main env with `AI_ENGINE_OPENAI`, and the pincheck env all compile clean (RAM 22.7%, flash 30.5%). **Not yet exercised against the live APIs on hardware** — needs a real API key in `secrets.h`, then flash and watch serial for the setup handshake. End-to-end voice remains blocked by the mic hardware fault + missing amp (see Known gaps); the session layer, tool-call emotions, and faces can be verified from serial/LCD regardless.

All phases build successfully (`pio run`) as of 2026-07-04. **Flashed and confirmed running on real ESP32-S3-EYE v2.2 hardware (2026-07-03)** — see "Hardware verification" below (pre-Phase-10 firmware; Phase 10 flash status tracked in the Phase 10 entry).

## Hardware verification (2026-07-03)

Flashed to a real board via `./upload_project.sh -p COM3`. Confirmed on-device:

- **WiFi connects** with real credentials in `include/secrets.h`.
- **LCD works end-to-end**: boot sequence observed as `"init"` → `"listening"` → `"idle"`, matching `displayInit()` running first, followed by a phantom wake-button press (see below) driving the state machine through `LISTENING` back to `IDLE`. This confirms the corrected LCD pins (SCLK/MOSI/CS/DC/backlight) and the mic pins are all wired correctly.
- **Known quirk, not a bug**: opening the serial monitor toggles DTR/RTS on the board, which touches GPIO0 — the same pin as the wake button (`PIN_BUTTON = 0`, confirmed correct via the official BSP as `BSP_BUTTON_5_IO`). This reliably looks like a phantom button press right after connecting the monitor (`[Button] Wake trigger pressed` → local VAD silence timeout → back to idle). Expected behavior of a boot-strap pin doing double duty as a button; only worth changing if it becomes annoying enough to move the wake button to a dedicated GPIO.
- **No crashes/panics** observed in the serial log through display init, WiFi connect, and WS connect attempts.
- **Not yet tested**: actual mic streaming against a live engine, speaker playback, or OTA.

### Pin corrections from hardware bring-up

The original guessed pins for mic and LCD were wrong and have been corrected in `include/pins_config.h`, sourced from Espressif's official `esp-bsp` (`esp32_s3_eye.h`) and cross-checked against `esp-skainet`'s `esp32_s3_eye_board.h` — both agree:

| Signal | Old guess | Corrected (verified) |
|---|---|---|
| Mic WS/LRCLK | 47 | **42** |
| Mic SCK/BCLK | 21 | **41** |
| Mic SD (data in) | 14 | **2** |
| LCD SCLK | 3 (collided with LED) | **21** |
| LCD RST | 48 | **-1 (NC, no hardware reset pin)** |
| LCD Backlight | -1 (assumed always-on) | **48 (must be driven HIGH — this was the actual root cause of the blank screen)** |
| LCD MOSI / CS / DC | 47 / 44 / 43 | unchanged, were already correct |
| RGB LED | 3 | unchanged, was already correct |
| Button | 0 | unchanged, was already correct |

Camera pins were already verified correct (matched the official BSP exactly on the first pass).

### Pin-check session (2026-07-03) — see `include/pins_config.h` for the confirmed-pin legend

Added a dedicated diagnostic firmware, `src/PinCheckMain.cpp` (PlatformIO env `esp32-s3-eye-pincheck`, flash with `./upload_project.sh --pincheck`), that tests each pin in isolation with an obvious expected result, so pins only get tagged `[CONFIRMED — DO NOT CHANGE]` after an explicit human "yes" — not incidental evidence from normal firmware boot. Results so far:

- **LED — confirmed.** User watched the red/green/blue/off cycle.
- **LCD — confirmed.** User saw "LCD OK" text and backlight.
- **Camera — confirmed, but needed a real fix, not just a pin change.** Camera init was failing with `cam_dma_config: frame buffer malloc failed` — not a wiring problem, but PSRAM being unusable. The generic `esp32-s3-devkitm-1` PlatformIO board definition doesn't configure this module's octal PSRAM correctly. Fixed by adding to `platformio.ini`: `board_build.flash_mode = qio` and `board_build.arduino.memory_type = qio_opi`. After that, camera captured real JPEGs (19663 and 23642 bytes across two cycles).
- **Button — confirmed.** Initial report ("when I press button reset the led blink green...") turned out to describe the normal boot sequence, not a real button-detect result — likely a separate hardware RST button rather than the BOOT button on GPIO0. Added an LCD-visible result (`PRESS BTN` → `BTN: YES`/`BTN: NO`) so this could be confirmed without reading the serial log. User then pressed the physical BOOT button during the window and confirmed `BTN: YES` appeared.
- **Mic — suspected hardware fault, pins are NOT the problem.** Pins triple-confirmed correct from espressif/esp-bsp, espressif/esp-skainet, and the actual official v2.2 schematic PDF (all three agree exactly: WS=42, SCK=41, SD=2; schematic names the exact chip, MSM261S4030H0R). User confirmed 3.3V at the mic's VDD pin with a multimeter. Despite this, extensively tested and the mic never once responded to sound: both I2S ports, both 16-bit and 32-bit+shift (the latter matching esp-skainet's proven wake-word driver exactly), both STAND_I2S and STAND_MSB timing, both LEFT/RIGHT channel select (RIGHT is pure zero — confirms slot logic is sane; LEFT has a real, config-sensitive, non-zero signal under every combination), fixed and adaptive RMS thresholds, RMS and peak-based detection, a loud clap, and a close air-puff ("blow test"). The digital I2S link is clearly electrically alive (signal changes with every config change) but has zero correlation with real sound — this points to a hardware-level fault (bad solder joint on SCK/WS/SD, or a dead mic component) on this specific board, not a firmware/config problem. See the full write-up in `include/pins_config.h`'s mic section. Next step if this needs to be revisited: check SCK/WS/SD continuity with a multimeter, or probe with an oscilloscope for real clocking activity — further firmware guessing is unlikely to help.

### Operational note: serial monitor DTR/RTS puts the board into download mode (2026-07-03)

On this native-USB (USB-Serial-JTAG) board, a serial monitor that asserts DTR/RTS on open makes the ROM reset the chip **into bootloader download mode** — the app never runs, the LCD backlight is dark, and the board seems dead until RST is pressed (and if the port stays open with DTR asserted, every RST release lands right back in download mode — observed as "screen only on while holding RST"). Fixed by `monitor_dtr = 0` / `monitor_rts = 0` in `platformio.ini`; if invoking a monitor manually, pass `--dtr 0 --rts 0`. If a board ever looks bricked with a dark screen, close all monitors and press RST once before assuming a firmware problem.

### Operational note: `pio device monitor` and COM port locking

Running `pio device monitor` as a backgrounded/detached process (rather than a foreground call with a bounded timeout) has left orphaned `pio.exe`/`python.exe` processes that keep COM3 open, causing subsequent uploads to fail with "port is busy." If an upload suddenly fails with a port-busy error, check for and kill lingering `device monitor` processes before retrying — foreground calls with an explicit timeout clean up correctly on their own.

## WS wire protocol

**As of Phase 10 the device talks directly to the AI provider** — Gemini
Live API or OpenAI Realtime API, selected via `AI_ENGINE` in `secrets.h`.
Both are one TLS WebSocket with a JSON setup handshake and base64 PCM audio
both directions; summarized in `README.md` ("Wire protocol"), implemented in
`src/GeminiLive.cpp` / `src/OpenAiRealtime.cpp` behind `include/AiEngine.h`.

## Known gaps

- **Phase 10 has not run against the live APIs yet** — the selected engine needs a real API key in `secrets.h` (`GEMINI_API_KEY` free at aistudio.google.com/apikey, or `OPENAI_API_KEY` paid). Flash, watch serial for `[Gemini] Setup complete` / `[OpenAI] Session configured`, then press BOOT.
- **Speaker pins still unverified** (`include/pins_config.h`, `PIN_SPK_I2S_*`): the ESP32-S3-EYE has no official speaker interface at all (confirmed via the BSP — I2S TX/MCLK are NC on the mic bus), so any speaker pins are picks that must avoid every GPIO the BSP lists as in use — free candidates are the SD-card pins **38/39/40** (see pin audit above). Not tested on hardware. GPIO46 in particular is a strapping pin, risky to drive at boot.
- **Mic — likely dead/faulty hardware on this board**, not a pin or config issue. See "Pin-check session" above for the full investigation. `AudioCapture.cpp` currently uses 32-bit read + `>>14` shift, I2S_NUM_1, STAND_MSB — the last config tested, kept as-is since none of the alternatives tried made any difference. Don't touch this again without new physical evidence (continuity/oscilloscope check).
- **End-to-end voice is blocked by hardware, not firmware**: the mic fault means the engine only ever hears silence (server VAD will never trigger), and with no amp wired the TTS reply is received but inaudible. The session layer (setup handshake, tool-call emotions, faces) can still be fully exercised and observed on the LCD/serial.
- **Voice barge-in requires the button**: the mic only streams while `LISTENING`, so the engines' interrupt-on-speech feature can't trigger during `SPEAKING`. Extend capture to run during playback once the mic hardware works.
- **Untested on hardware**: speaker playback (no amp wired), ArduinoOTA. Camera snapshot capture is confirmed via pin-check. Mic capture pipeline runs without crashing but the mic itself appears non-functional (see above).
- **TLS is unpinned** (`setInsecure()`) for the WSS connection.

## Chosen libraries (lib_deps)

| Purpose | Library | lib_deps entry |
|---|---|---|
| WebSocket streaming | Links2004/arduinoWebSockets | `links2004/WebSockets @ ^2.4.1` |
| JSON (setup/events/audio envelopes) | ArduinoJson | `bblanchon/ArduinoJson @ ^7.2.0` |
| Onboard RGB LED feedback | Adafruit NeoPixel | `adafruit/Adafruit NeoPixel @ ^1.12.0` |
| Button debounce | Bounce2 | `thomasfredericks/Bounce2 @ ^2.71` |
| Onboard LCD text display | Adafruit GFX + ST7735/ST7789 | `adafruit/Adafruit GFX Library @ ^1.11.9`, `adafruit/Adafruit ST7735 and ST7789 Library @ ^1.10.3` |

WiFi, I2S (mic + speaker), base64 (mbedTLS), camera, ring buffers, OTA, and
the watchdog are all provided by the `arduino-esp32` core / ESP-IDF — no
extra lib_deps needed for those. No audio codec library: both engines speak
raw PCM.

## Tooling

- `./upload_project.sh` wraps `pio run`/`pio device monitor` — builds + uploads by default, `-b` build-only, `-m` open monitor after upload, `-p <port>` to force a port, `-c` clean build first, `-h` for full usage. Auto-locates `pio` even when it's not on PATH (checks the default PlatformIO IDE install location). Also auto-creates `include/secrets.h` from the example template if missing.
- `./upload_project.sh --pincheck -p <port>` flashes the `esp32-s3-eye-pincheck` environment (`src/PinCheckMain.cpp`) and opens the monitor. This is a standalone diagnostic firmware, separate from `main.cpp` (excluded from each other via `build_src_filter` in `platformio.ini`), that tests each pin/peripheral in `pins_config.h` in isolation with an obvious expected result: LED cycles red/green/blue/off, LCD shows "LCD OK", button press is detected within a 5s window, mic clap test, a speaker tone burst is written, and one camera JPEG is captured. Loops every 5s. See the pin verification legend at the top of `include/pins_config.h` — pins move from `[UNCONFIRMED]` to `[CONFIRMED — DO NOT CHANGE]` only after a human explicitly confirms the expected output on real hardware.

## Next steps (not started)

- Put a real API key in `include/secrets.h` (Gemini free tier is the default engine), flash, and verify the session setup + a full talk turn on serial/LCD.
- Fix or replace the mic (continuity/oscilloscope check, or wire an external I2S mic) — until then the assistant cannot hear.
- Wire and verify an external I2S amp (candidate pins 38/39/40) so TTS replies are audible.
- Stream mic audio during SPEAKING too, enabling voice barge-in (engines already support it server-side).
- Camera snapshots to the engine ("what am I looking at?") — both APIs accept images.
