#pragma once

// Speak-server relay: POSTs the text of each finished AI reply as plain
// text to SPEAK_SERVER_URL (config.h), e.g. a LAN TTS/logging server:
//   curl -X POST http://192.168.1.77:8080/speak --data-binary "Hello world"
//
// Non-blocking: the text is copied onto a queue and a dedicated FreeRTOS
// task performs the HTTP POST (connect SPEAK_SERVER_TIMEOUT_MS, response
// wait SPEAK_SERVER_READ_TIMEOUT_MS), so the caller — the Arduino loop
// task — is never stalled by a slow or unreachable server.
// No-op if text is empty, WiFi is down, or SPEAK_SERVER_URL is "".
void speakServerSend(const char *text);
