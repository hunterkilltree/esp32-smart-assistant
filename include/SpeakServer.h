#pragma once

// Speak-server relay: POSTs the transcript of each finished AI reply as
// plain text to SPEAK_SERVER_URL (config.h), e.g. a LAN TTS/logging server:
//   curl -X POST http://192.168.1.77:8080/speak --data-binary "Hello world"
//
// Blocking call (LAN HTTP, capped by SPEAK_SERVER_TIMEOUT_MS) — invoke at
// turn boundaries only, never inside the audio streaming path.
// No-op if text is empty, WiFi is down, or SPEAK_SERVER_URL is "".
void speakServerSend(const char *text);
