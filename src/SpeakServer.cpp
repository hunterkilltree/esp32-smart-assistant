#include "SpeakServer.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "config.h"

void speakServerSend(const char *text) {
  static const char url[] = SPEAK_SERVER_URL;
  if (url[0] == '\0' || !text || !text[0]) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Speak] WiFi down — reply text not sent");
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(SPEAK_SERVER_TIMEOUT_MS);
  http.setTimeout(SPEAK_SERVER_TIMEOUT_MS);
  if (!http.begin(url)) {
    Serial.printf("[Speak] Bad server URL: %s\n", url);
    return;
  }
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  Serial.printf("[Speak] Sending (%u bytes): \"%s\"\n",
                (unsigned)strlen(text), text);
  int code = http.POST(reinterpret_cast<uint8_t *>(const_cast<char *>(text)),
                       strlen(text));
  if (code > 0) {
    Serial.printf("[Speak] POST %s -> HTTP %d (%u bytes)\n", url, code,
                  (unsigned)strlen(text));
  } else {
    // Server unreachable is expected when the PC app isn't running — log
    // and move on; the conversation itself is unaffected.
    Serial.printf("[Speak] POST %s failed: %s\n", url,
                  HTTPClient::errorToString(code).c_str());
  }
  http.end();
}
