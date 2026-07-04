#pragma once
#include <cstddef>
#include <cstdint>

// xiaozhi WebSocket protocol client (protocol version 1, raw Opus binary
// frames) — see https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md
//
// Flow: xzConnect() opens the socket with the identifying headers, sends the
// client hello, and waits for the server hello (10s timeout). Once ready,
// listen/abort JSON messages and binary Opus frames flow both ways. The
// WebSockets library auto-reconnects a dropped socket; the hello handshake
// re-runs on every (re)connect.
//
// All calls, including the callbacks, happen on the Arduino loop task.

struct XiaozhiCallbacks {
  // Server hello received — session is usable. serverSampleRate is the
  // downlink Opus sample rate (24000 on the official backend).
  void (*onReady)(uint32_t serverSampleRate);
  void (*onDisconnected)();
  // One encoded downlink Opus frame (TTS audio).
  void (*onAudio)(const uint8_t *data, size_t len);
  // state: "start" | "stop" | "sentence_start" (text = subtitle, may be "").
  void (*onTtsState)(const char *state, const char *text);
  void (*onStt)(const char *text);
  // xiaozhi emotion keyword: "happy", "sad", "thinking", "neutral", ...
  void (*onEmotion)(const char *emotion);
  void (*onGoodbye)();
};

// Computes the persistent device identity (MAC + NVS-stored UUID) and
// registers callbacks. Call once, before xzConnect()/xiaozhiOtaCheck().
void xzInit(const XiaozhiCallbacks &cbs);

// "aa:bb:cc:dd:ee:ff" — also used as the Device-Id OTA header.
const char *xzDeviceId();
// Persistent UUID — also used as the Client-Id OTA header.
const char *xzClientId();

// Parses ws:// or wss:// URL and starts connecting (non-blocking; the
// socket is serviced by xzLoop()).
void xzConnect(const char *wsUrl, const char *token);

// Pump. Call every loop() iteration after xzConnect().
void xzLoop();

bool xzSocketConnected();  // TCP/WS link up
bool xzReady();            // hello handshake completed

void xzSendListenStart(const char *mode);  // "auto" | "manual" | "realtime"
void xzSendListenStop();
void xzSendAbort(const char *reason);      // e.g. "wake_word_detected", or ""
void xzSendAudio(const uint8_t *opusData, size_t len);
