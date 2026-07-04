#pragma once
#include <cstddef>
#include <cstdint>

// xiaozhi OTA/config check: POSTs the device's identity + firmware info to
// XIAOZHI_OTA_URL. The response tells the device where to connect
// (websocket url/token) and, for a device not yet bound to an account, a
// 6-digit activation code the user enters at xiaozhi.me. Poll until the
// activation field disappears from the response, then connect.
struct XiaozhiOtaResult {
  bool ok = false;               // request + parse succeeded
  bool hasActivation = false;    // device not bound yet — show the code
  char activationCode[16] = "";
  char wsUrl[160] = "";          // empty -> fall back to XIAOZHI_WS_URL
  char wsToken[96] = "";         // empty -> fall back to XIAOZHI_WS_TOKEN
};

// Blocking (a few seconds). Requires WiFi up and xzInit() done (uses the
// same Device-Id/Client-Id identity as the WebSocket connection).
bool xiaozhiOtaCheck(XiaozhiOtaResult &out);
