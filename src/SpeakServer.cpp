#include "SpeakServer.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "config.h"

namespace {

// Sends run on their own low-priority task so a slow /speak server (one
// that only answers after it finishes speaking the text out loud) never
// blocks the main loop, which must keep pumping the engine WebSocket.
// The queue carries heap-owned strings; the task frees them after sending.
QueueHandle_t s_queue = nullptr;

void speakTask(void *) {
  for (;;) {
    char *text = nullptr;
    if (xQueueReceive(s_queue, &text, portMAX_DELAY) != pdTRUE || !text) {
      continue;
    }

    HTTPClient http;
    http.setConnectTimeout(SPEAK_SERVER_TIMEOUT_MS);
    http.setTimeout(SPEAK_SERVER_READ_TIMEOUT_MS);
    if (http.begin(SPEAK_SERVER_URL)) {
      http.addHeader("Content-Type", "text/plain; charset=utf-8");
      Serial.printf("[Speak] Sending (%u bytes): \"%s\"\n",
                    (unsigned)strlen(text), text);
      int code = http.POST(reinterpret_cast<uint8_t *>(text), strlen(text));
      if (code > 0) {
        Serial.printf("[Speak] POST %s -> HTTP %d\n", SPEAK_SERVER_URL, code);
      } else if (code == HTTPC_ERROR_READ_TIMEOUT) {
        // Connect + upload succeeded — the server has the text but took
        // longer than SPEAK_SERVER_READ_TIMEOUT_MS to answer. Delivered.
        Serial.println("[Speak] Delivered, but the server never answered "
                       "(still speaking?) — treating as OK");
      } else {
        Serial.printf("[Speak] POST %s failed: %s\n", SPEAK_SERVER_URL,
                      HTTPClient::errorToString(code).c_str());
      }
      http.end();
    } else {
      Serial.printf("[Speak] Bad server URL: %s\n", SPEAK_SERVER_URL);
    }
    free(text);
  }
}

}  // namespace

void speakServerSend(const char *text) {
  static const char url[] = SPEAK_SERVER_URL;
  if (url[0] == '\0' || !text || !text[0]) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Speak] WiFi down — reply text not sent");
    return;
  }

  if (!s_queue) {
    s_queue = xQueueCreate(4, sizeof(char *));
    xTaskCreate(speakTask, "speak_relay", 8192, nullptr, 1, nullptr);
  }

  char *copy = strdup(text);
  if (!copy) return;
  if (xQueueSend(s_queue, &copy, 0) != pdTRUE) {
    Serial.println("[Speak] Send queue full — reply text dropped");
    free(copy);
  }
}
