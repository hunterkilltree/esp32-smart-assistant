# Build Progress

Tracks implementation status by phase. Update a checkbox to `[x]` (with date) as soon as that phase is implemented and verified — this file is the source of truth for "where did we leave off."

Goal: real-time streaming voice assistant on ESP32-S3-EYE v2.2 (continuous mic → WebSocket → backend AI → streamed audio response). Record-then-send is explicitly disallowed as the main flow — see `CLAUDE.md`.

## Phases

- [x] **Phase 0 — Environment** (2026-07-03): `platformio.ini` env renamed to `esp32-s3-eye`, build_flags (PSRAM/camera/USB-CDC) added, lib_deps added. Builds clean.
- [x] **Phase 1 — Connectivity & control core** (2026-07-03): WiFi connect, `WebSocketsClient` to backend, `AssistantState` (idle/listening/speaking) state machine, `Bounce2` wake button, `Adafruit_NeoPixel` status LED. See `src/main.cpp`.
- [x] **Phase 2 — Audio capture** (2026-07-03): I2S mic RX on `I2S_NUM_0` + FreeRTOS task in `src/AudioCapture.cpp`, energy-based (RMS) VAD for local endpointing only, continuous raw PCM chunks sent as WS binary frames while `LISTENING`.
- [x] **Phase 3 — Audio playback** (2026-07-03): I2S TX on `I2S_NUM_1` to external amp, byte ring buffer (`freertos/ringbuf.h`) decouples incoming WS binary frames from I2S write cadence. See `src/AudioPlayback.cpp`.
- [x] **Phase 4 — Camera snapshot** (2026-07-03): `esp32-camera` init with `CAMERA_MODEL_ESP32S3_EYE` pins, triggered by backend `{"type":"request_snapshot"}`, JPEG sent as WS binary preceded by a `{"type":"snapshot","bytes":N}` JSON header (see protocol note below). See `src/CameraCapture.cpp`.
- [x] **Phase 5 — Wake word upgrade** (2026-07-03): **Deferred, not implemented.** ESP-SR (WakeNet+AFE) requires ESP-IDF components that don't integrate cleanly with a PlatformIO `framework = arduino` build — would likely need `framework = espidf` with Arduino-as-component, a separate project restructure. Button-press wake trigger (Phase 1) remains the wake mechanism. Revisit if on-device wake word becomes a hard requirement.
- [x] **Phase 6 — Reliability** (2026-07-03): WiFi reconnect with exponential backoff (2s → 30s cap), `ArduinoOTA` (starts after first successful WiFi connect), task watchdog (10s timeout, panics/reboots on stall, fed once per `loop()`). See `src/Reliability.cpp`.
- [x] **LCD display (added post-roadmap, 2026-07-03)**: onboard ST7789 LCD shows `"init"` as the very first thing in `setup()` — a quick visual check that a freshly flashed board is running the new firmware. Also mirrors assistant state text (`idle`/`listening`/`speaking`) alongside the RGB LED in `setState()`. See `src/Display.cpp`. **Superseded by Phase 7's face UI (plain-text `displayShowStatus` kept only for the pin-check firmware).**
- [x] **Phase 7 — Boot self-test + colorful face UI (2026-07-03)**: brings the firmware in line with the project spec poster (`project_image.png`). (a) `src/SelfTest.cpp`: on-boot checklist shown on the LCD **before the main flow** — PSRAM, LED cycle, button-not-stuck, secrets.h-not-placeholder, WiFi connect (15s timeout, shows IP), mic I2S data liveness, speaker (intentional SKIP — no amp), camera init + real JPEG capture — with colored PASS/WARN/FAIL/SKIP rows and a verdict summary screen (counts, IP, WS endpoint; holds 8s or until BOOT press). Self-test owns the audio/camera driver inits; watchdog is armed only after it so the blocking checks can't trip it. (b) `src/Display.cpp` rewritten: rainbow splash, checklist UI, and expressive faces drawn with GFX primitives — neutral/cyan (idle), listening/azure (big eyes), thinking/yellow (flat mouth + thought dots), speaking/orange (open mouth), happy/green (wide smile), sad/red (frown + tear) — plus a status bar and WiFi/WS connectivity dots. (c) New `THINKING` state in `AssistantState` (VAD timeout → THINKING; first TTS frame → SPEAKING; 15s no-backend timeout → IDLE) and backend `{"type":"emotion","value":"smile"|"sad"|"thinking"}` support in `main.cpp`. (d) Docs: professional `README.md` added; `CLAUDE.md` rewritten (was truncated) with the self-test/expression requirements and corrected hardware facts (LCD is 1.3" 240×240, not 2.4"/320×240). Both envs build clean. **Flashed to hardware 2026-07-03**: board boots through the full self-test into the main loop with no crash/panic — `[WiFi] Connected, IP: 192.168.1.57` observed on serial (self-test's own serial lines were lost to USB-CDC re-enumeration; results are on the LCD). LCD visuals confirmed showing by the user, with one round of feedback applied: the boot screens advanced too fast to read, so pacing is now user-friendly and centralized in `config.h` (splash 3s, 700ms pause after each checklist row, finished checklist holds 10s, summary holds 15s — every hold skippable early with the BOOT button via `holdScreen()` in `SelfTest.cpp`). Reflashed 2026-07-03.

- [x] **Phase 8 — xiaozhi protocol rewrite (2026-07-03)**: replaced the hand-rolled WS protocol (which had no backend to talk to) with the **xiaozhi protocol v1** (https://github.com/78/xiaozhi-esp32), so the device can talk to the free official xiaozhi.me cloud or a self-hosted `xiaozhi-esp32-server` — a real STT+LLM+TTS backend with zero backend work. Changes: (a) **Opus audio both directions** — `pschatzmann/arduino-libopus` added; uplink 16 kHz mono 60 ms frames encoded in a dedicated 32 KB-stack task (`AudioCapture.cpp`), downlink Opus decoded in the playback task at the server-hello-announced rate, default 24 kHz (`AudioPlayback.cpp`); raw-PCM path kept for pin-check. (b) **`src/XiaozhiProtocol.cpp`** — TLS WS with `Authorization`/`Protocol-Version`/`Device-Id` (MAC)/`Client-Id` (NVS-persisted UUID) headers, hello handshake (10 s timeout → reconnect), `listen start/stop` (mode `auto`, server-side endpointing — local VAD is now diagnostic only), `abort`, and server `stt`/`llm` emotion/`tts start·stop·sentence_start`/`goodbye`/`system reboot` handling. (c) **`src/XiaozhiOta.cpp` + activation flow** — POSTs device info to the xiaozhi OTA endpoint; unbound devices show the 6-digit code big on the LCD (`displayShowActivation`), re-poll every 10 s (BOOT = immediate re-check), then auto-connect once bound at xiaozhi.me; falls back to `secrets.h` WS defaults after 3 failed OTA checks. (d) **main.cpp** rewritten: continuous-conversation state machine (button starts session → server VAD → `stt`→THINKING → `tts start`→SPEAKING → `tts stop`→back to LISTENING; button = stop/barge-in), xiaozhi emotion→face mapping, 60 s listening safety timeout. (e) Camera snapshots dropped from the main flow (needs xiaozhi's MCP feature — future work); still in self-test/pin-check. (f) `secrets.h` now carries `XIAOZHI_OTA_URL`/`XIAOZHI_WS_URL`/`XIAOZHI_WS_TOKEN` instead of `WS_HOST/PORT/PATH`. Both envs build clean. **Flashed and serial-verified on hardware 2026-07-03**: boots with no crash/panic, WiFi connects, the OTA check reaches the real xiaozhi cloud and returns `websocket.url`/`token` plus activation code (observed `116462` for Device-Id `34:85:18:8c:4f:1c`), the 10s re-poll loop runs as designed, and the code screen shows on the LCD. **Awaiting**: user binds the device at xiaozhi.me (Console → Add Device → enter the code shown on the LCD), after which the device auto-connects the WebSocket. End-to-end voice still blocked by the mic hardware fault + missing amp (see Known gaps). Also added `ensureBacklight()` to every full-screen draw in `Display.cpp` — the GPIO48 backlight is re-asserted on each redraw so nothing can leave the panel dark while the firmware runs. **Refactor (same day, user request):** `main.cpp` split into single-purpose modules for easier debugging — `AppState` (state machine + face/LED), `WifiLink` (reconnect/backoff + ArduinoOTA), `BackendSession` (OTA→activation→connect phases), `Conversation` (protocol callbacks, button, uplink pump, timeouts); `loop()` is now a 4-stage early-return pipeline. Speaker pins were commented out in `pins_config.h` (user edit, no amp wired); `AudioPlayback.cpp` now compiles without them (`HAS_SPEAKER` guard) and discards TTS audio with a boot log line instead of installing the I2S TX driver. Reflashed and re-verified on hardware 2026-07-03: full self-test **7 pass / 0 warn / 0 fail** captured on serial, OTA/activation flow unchanged.

- [x] **Phase 9 — 4-button controls (2026-07-03)**: the four front buttons (MENU/PLAY/UP/DN) share an ADC resistor ladder on **GPIO1** (ADC1_CH0, per the v2.2 schematic — also the reason the old speaker BCLK=1 guess is retired for good). New `src/AdcButtons.cpp` (self-throttled poll, 2-sample debounce, edge events, logs the measured mV of every press — unclassified presses print their mV so the `[UNCONFIRMED]` `ADC_BTN_*` windows in `config.h` can be calibrated) + `src/Controls.cpp`: **PLAY** = second talk button (merged with BOOT), **UP/DN** = volume ±10% (software gain on decoded TTS in `AudioPlayback`, persisted in NVS key `xiaozhi/volume`, on-screen bar overlay for 1.5s), **MENU** = toggle info/debug screen (state, WiFi SSID/RSSI/IP, WS status, Device-Id, volume, free heap — `displayShowInfo`). Pincheck got a `PRESS 4BTN` calibration test (8s window, live mV on the LCD, button name when a window matches). Flashed + serial-verified 2026-07-03 (self-test 7/7, volume restored from NVS); **the mV windows still need the pincheck calibration pass with a human pressing each button**.

- [x] **Pin audit vs. board pin map (`pin_io.png`, 2026-07-03)**: cross-checked every pin in `pins_config.h` against the board's pin-group map. Confirmed matches: mic (41/42/2), LCD data (48/47/44/21/43), camera (all), buttons (ADC ladder on GPIO1 + BOOT on GPIO0). Corrections: **`PIN_LCD_RST` reverted 45 → -1** — the LCD has no reset wire (only five LCD signals exist) and GPIO45 is a *strapping pin* (VDD_SPI flash-voltage select; driving it during a reset can brick the boot until power-cycle). New facts documented: **BAT_DET on GPIO14** (`PIN_BAT_DET`, unused so far), USB on 19/20, SD on 38/39/40 — which are the *only* free GPIOs on the board, so the future external-amp pins should be **38/39/40** (all three old speaker guesses were taken: 1=buttons, 14=battery, 46=strapping). Reflashed + verified: self-test 7/7.

All phases build successfully (`pio run`) as of 2026-07-03. **Flashed and confirmed running on real ESP32-S3-EYE v2.2 hardware (2026-07-03)** — see "Hardware verification" below (pre-Phase-8 firmware; Phase 8 flash status tracked in the Phase 8 entry).

## Hardware verification (2026-07-03)

Flashed to a real board via `./upload_project.sh -p COM3`. Confirmed on-device:

- **WiFi connects** with real credentials in `include/secrets.h`.
- **LCD works end-to-end**: boot sequence observed as `"init"` → `"listening"` → `"idle"`, matching `displayInit()` running first, followed by a phantom wake-button press (see below) driving the state machine through `LISTENING` back to `IDLE`. This confirms the corrected LCD pins (SCLK/MOSI/CS/DC/backlight) and the mic pins are all wired correctly.
- **Known quirk, not a bug**: opening the serial monitor toggles DTR/RTS on the board, which touches GPIO0 — the same pin as the wake button (`PIN_BUTTON = 0`, confirmed correct via the official BSP as `BSP_BUTTON_5_IO`). This reliably looks like a phantom button press right after connecting the monitor (`[Button] Wake trigger pressed` → local VAD silence timeout → back to idle). Expected behavior of a boot-strap pin doing double duty as a button; only worth changing if it becomes annoying enough to move the wake button to a dedicated GPIO.
- **No crashes/panics** observed in the serial log through display init, WiFi connect, and WS connect attempts.
- **Not yet tested**: actual mic streaming, speaker playback, camera snapshot, or OTA — none of these have been exercised on hardware yet, and there's still no backend to talk to.

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

**As of Phase 8 this is the xiaozhi protocol v1** — spec at
https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md, summarized in
`README.md`, implemented in `src/XiaozhiProtocol.cpp`. The old hand-rolled
protocol (raw PCM binary frames + `wake`/`utterance_end`/`state`/`emotion`/
`snapshot` JSON) is gone; nothing ever spoke it on the backend side.

## Known gaps

- **Speaker pins still unverified** (`include/pins_config.h`, `PIN_SPK_I2S_*`): the ESP32-S3-EYE has no official speaker interface at all (confirmed via the BSP — I2S TX/MCLK are NC on the mic bus), so these three pins are arbitrary picks that avoid every GPIO the BSP lists as in use. Not tested on hardware. If you wire an external I2S amp, confirm against your board's actual free pins first — GPIO46 in particular is a strapping pin, risky to drive at boot.
- **Mic — likely dead/faulty hardware on this board**, not a pin or config issue. See "Pin-check session" above for the full investigation. `AudioCapture.cpp` currently uses 32-bit read + `>>14` shift, I2S_NUM_1, STAND_MSB — the last config tested, kept as-is since none of the alternatives tried made any difference. Don't touch this again without new physical evidence (continuity/oscilloscope check).
- **`include/secrets.h`** (gitignored) has real WiFi credentials filled in; the xiaozhi URLs default to the official cloud (`api.tenclass.net`). The device must be bound once at xiaozhi.me via the on-LCD activation code before conversations work.
- **End-to-end voice is blocked by hardware, not firmware**: the mic fault means the server only ever hears silence (it will never produce an `stt` result), and with no amp wired the TTS reply is decoded but inaudible. The protocol/session layer (activation, hello, listen, tts messages, emotion faces) can still be fully exercised and observed on the LCD/serial.
- **Untested on hardware**: speaker playback (no amp wired), ArduinoOTA. Camera snapshot capture is confirmed via pin-check. Mic capture pipeline runs without crashing but the mic itself appears non-functional (see above).
- **TLS is unpinned** (`setInsecure()`) for both the OTA HTTPS call and the WSS connection.

## Chosen libraries (lib_deps)

| Purpose | Library | lib_deps entry |
|---|---|---|
| WebSocket streaming | Links2004/arduinoWebSockets | `links2004/WebSockets @ ^2.4.1` |
| Control/metadata JSON messages | ArduinoJson | `bblanchon/ArduinoJson @ ^7.2.0` |
| Onboard RGB LED feedback | Adafruit NeoPixel | `adafruit/Adafruit NeoPixel @ ^1.12.0` |
| Button debounce | Bounce2 | `thomasfredericks/Bounce2 @ ^2.71` |
| Onboard LCD text display | Adafruit GFX + ST7735/ST7789 | `adafruit/Adafruit GFX Library @ ^1.11.9`, `adafruit/Adafruit ST7735 and ST7789 Library @ ^1.10.3` |
| Opus encode/decode (xiaozhi audio) | arduino-libopus | `https://github.com/pschatzmann/arduino-libopus.git` |

WiFi, I2S (mic + speaker), camera, ring buffers, OTA, and the watchdog are all provided by the `arduino-esp32` core / ESP-IDF — no extra lib_deps needed for those.

## Tooling

- `./upload_project.sh` wraps `pio run`/`pio device monitor` — builds + uploads by default, `-b` build-only, `-m` open monitor after upload, `-p <port>` to force a port, `-c` clean build first, `-h` for full usage. Auto-locates `pio` even when it's not on PATH (checks the default PlatformIO IDE install location). Also auto-creates `include/secrets.h` from the example template if missing.
- `./upload_project.sh --pincheck -p <port>` flashes the `esp32-s3-eye-pincheck` environment (`src/PinCheckMain.cpp`) and opens the monitor. This is a standalone diagnostic firmware, separate from `main.cpp` (excluded from each other via `build_src_filter` in `platformio.ini`), that tests each pin/peripheral in `pins_config.h` in isolation with an obvious expected result: LED cycles red/green/blue/off, LCD shows "LCD OK", button press is detected within a 5s window, mic RMS level is printed for 3s, a speaker tone burst is written, and one camera JPEG is captured. Loops every 5s. See the pin verification legend at the top of `include/pins_config.h` — pins move from `[UNCONFIRMED]` to `[CONFIRMED — DO NOT CHANGE]` only after a human explicitly confirms the expected output on real hardware.

## Next steps (not started)

- Bind the device at xiaozhi.me with the on-LCD activation code (first boot after Phase 8 flash) and confirm the WS hello/session on serial.
- Fix or replace the mic (continuity/oscilloscope check, or wire an external I2S mic) — until then the assistant cannot hear.
- Wire and verify an external I2S amp so TTS replies are audible.
- MCP device tools (camera/vision, LED, volume) once the basics are proven.
