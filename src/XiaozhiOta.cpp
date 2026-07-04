#include "XiaozhiOta.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "XiaozhiProtocol.h"
#include "Reliability.h"

namespace {

constexpr char FIRMWARE_VERSION[] = "1.0.0";

String buildRequestBody() {
  // Mirrors the fields the official firmware reports (main/ota.cc); the
  // backend keys on mac_address/uuid, the rest is descriptive.
  JsonDocument doc;
  doc["language"] = "en-US";
  doc["flash_size"] = ESP.getFlashChipSize();
  doc["minimum_free_heap_size"] = esp_get_minimum_free_heap_size();
  doc["mac_address"] = xzDeviceId();
  doc["uuid"] = xzClientId();
  doc["chip_model_name"] = "esp32s3";

  JsonObject chip = doc["chip_info"].to<JsonObject>();
  chip["model"] = (int)ESP.getChipModel()[0];  // descriptive only
  chip["cores"] = ESP.getChipCores();
  chip["revision"] = ESP.getChipRevision();
  chip["features"] = 0;

  JsonObject app = doc["application"].to<JsonObject>();
  app["name"] = "esp32-smart-assistant";
  app["version"] = FIRMWARE_VERSION;
  app["compile_time"] = __DATE__ " " __TIME__;
  app["idf_version"] = esp_get_idf_version();

  JsonObject board = doc["board"].to<JsonObject>();
  board["type"] = "esp32-s3-eye";
  board["name"] = "esp32-s3-eye";
  board["ssid"] = WiFi.SSID();
  board["rssi"] = WiFi.RSSI();
  board["channel"] = WiFi.channel();
  board["ip"] = WiFi.localIP().toString();
  board["mac"] = xzDeviceId();

  String body;
  serializeJson(doc, body);
  return body;
}

}  // namespace

bool xiaozhiOtaCheck(XiaozhiOtaResult &out) {
  out = XiaozhiOtaResult{};

  const char *url = XIAOZHI_OTA_URL;
  bool tls = strncmp(url, "https://", 8) == 0;

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  if (tls) secureClient.setInsecure();

  HTTPClient http;
  http.setTimeout(8000);
  http.setConnectTimeout(5000);
  if (!http.begin(tls ? secureClient : plainClient, url)) {
    Serial.println("[OTA] http.begin failed (bad URL?)");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Device-Id", xzDeviceId());
  http.addHeader("Client-Id", xzClientId());
  http.addHeader("Activation-Version", "1");
  http.addHeader("User-Agent", "esp32-s3-eye/1.0.0");

  String body = buildRequestBody();
  Serial.printf("[OTA] POST %s\n", url);
  reliabilityFeedWatchdog();  // TLS + HTTP can take several seconds
  int status = http.POST(body);
  reliabilityFeedWatchdog();

  if (status != 200) {
    Serial.printf("[OTA] HTTP %d: %s\n", status,
                  http.errorToString(status).c_str());
    http.end();
    return false;
  }

  String resp = http.getString();
  http.end();
  Serial.printf("[OTA] Response: %s\n", resp.c_str());

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    Serial.printf("[OTA] Bad JSON: %s\n", err.c_str());
    return false;
  }

  if (doc["websocket"].is<JsonObject>()) {
    strlcpy(out.wsUrl, doc["websocket"]["url"] | "", sizeof(out.wsUrl));
    strlcpy(out.wsToken, doc["websocket"]["token"] | "", sizeof(out.wsToken));
  }
  if (doc["activation"].is<JsonObject>()) {
    out.hasActivation = true;
    strlcpy(out.activationCode, doc["activation"]["code"] | "",
            sizeof(out.activationCode));
    Serial.printf("[OTA] Device not bound — activation code: %s\n",
                  out.activationCode);
  }

  out.ok = true;
  return true;
}
