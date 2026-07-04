#include "Conversation.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "Display.h"
#include "AppState.h"
#include "AudioCapture.h"
#include "AudioPlayback.h"
#include "XiaozhiProtocol.h"

namespace {

// If the mic really is dead (see pins_config.h) the server will never hear
// speech — don't let LISTENING hang forever.
constexpr unsigned long LISTENING_TIMEOUT_MS = 60000;

// Continuous conversation: after the server finishes speaking, reopen the
// mic instead of going idle, until the button or a server goodbye ends it.
bool s_conversationActive = false;

// ---- xiaozhi protocol callbacks (all run on the loop task) ----

void onReady(uint32_t serverSampleRate) {
  audioPlaybackConfigure(serverSampleRate);
  s_conversationActive = false;
  appStateSet(AssistantState::IDLE);
  appShowFace(Expression::HAPPY, "Connected!");
}

void onDisconnected() {
  s_conversationActive = false;
  audioPlaybackClear();
  appStateSet(AssistantState::IDLE);
  // appStateSet no-ops if already IDLE — still flip the WS dot to red.
  displayShowConnectivity(WiFi.status() == WL_CONNECTED, false);
}

void onAudio(const uint8_t *data, size_t len) {
  audioPlaybackWriteOpus(data, len);
}

void onTtsState(const char *state, const char *text) {
  (void)text;  // sentence subtitles are logged by the protocol layer
  if (strcmp(state, "start") == 0) {
    appStateSet(AssistantState::SPEAKING);
  } else if (strcmp(state, "stop") == 0) {
    if (s_conversationActive) {
      // Continuous conversation: reopen the mic for the next turn.
      xzSendListenStart("auto");
      appStateSet(AssistantState::LISTENING);
    } else {
      appStateSet(AssistantState::IDLE);
    }
  }
}

void onStt(const char *text) {
  (void)text;
  // The server heard and transcribed an utterance — it's thinking now.
  appStateSet(AssistantState::THINKING);
}

// Map xiaozhi emotion keywords onto the face set. Unknown keywords keep
// the current face.
void onEmotion(const char *emotion) {
  static const char *happyWords[] = {"happy", "laughing", "funny",  "loving",
                                     "confident", "winking", "cool",
                                     "delicious", "kissy", "silly"};
  static const char *sadWords[] = {"sad", "crying", "embarrassed", "angry"};

  Expression expr;
  const char *caption;
  bool matched = false;
  for (const char *w : happyWords) {
    if (strcmp(emotion, w) == 0) { expr = Expression::HAPPY; caption = "Happy"; matched = true; break; }
  }
  if (!matched) {
    for (const char *w : sadWords) {
      if (strcmp(emotion, w) == 0) { expr = Expression::SAD; caption = "Sorry..."; matched = true; break; }
    }
  }
  if (!matched) {
    if (strcmp(emotion, "thinking") == 0 || strcmp(emotion, "confused") == 0) {
      expr = Expression::THINKING; caption = "Thinking...";
    } else if (strcmp(emotion, "surprised") == 0 || strcmp(emotion, "shocked") == 0) {
      expr = Expression::LISTENING; caption = "Oh!";
    } else if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "relaxed") == 0 ||
               strcmp(emotion, "sleepy") == 0) {
      expr = Expression::NEUTRAL; caption = "Ready";
    } else {
      return;
    }
  }
  appShowFace(expr, caption);
}

void onGoodbye() {
  s_conversationActive = false;
  audioPlaybackClear();
  appStateSet(AssistantState::IDLE);
}

// ---- Button: start / stop / barge-in ----

void handleButton() {
  switch (appStateGet()) {
    case AssistantState::IDLE:
      Serial.println("[Button] Starting conversation");
      s_conversationActive = true;
      xzSendListenStart("auto");
      appStateSet(AssistantState::LISTENING);
      break;
    case AssistantState::LISTENING:
      Serial.println("[Button] Stopping conversation");
      s_conversationActive = false;
      xzSendListenStop();
      appStateSet(AssistantState::IDLE);
      break;
    case AssistantState::THINKING:
    case AssistantState::SPEAKING:
      Serial.println("[Button] Aborting response");
      s_conversationActive = false;
      xzSendAbort("");
      audioPlaybackClear();
      appStateSet(AssistantState::IDLE);
      break;
  }
}

}  // namespace

void conversationInit() {
  XiaozhiCallbacks cbs = {};
  cbs.onReady = onReady;
  cbs.onDisconnected = onDisconnected;
  cbs.onAudio = onAudio;
  cbs.onTtsState = onTtsState;
  cbs.onStt = onStt;
  cbs.onEmotion = onEmotion;
  cbs.onGoodbye = onGoodbye;
  xzInit(cbs);
}

void conversationLoop(bool buttonPressed) {
  if (buttonPressed && xzReady()) handleButton();

  // ---- Uplink: pump encoded mic frames to the server while listening ----
  if (appStateGet() == AssistantState::LISTENING) {
    static uint8_t opusFrame[OPUS_MAX_FRAME_BYTES];
    size_t n;
    while ((n = audioCaptureDequeueOpus(opusFrame, sizeof(opusFrame))) > 0) {
      xzSendAudio(opusFrame, n);
    }

    if (millis() - appStateEnteredMs() > LISTENING_TIMEOUT_MS) {
      Serial.println("[State] Listening timed out — closing conversation");
      s_conversationActive = false;
      xzSendListenStop();
      appStateSet(AssistantState::IDLE);
    }
  }

  // Backend never answered — don't stay wedged on the thinking face.
  if (appStateGet() == AssistantState::THINKING &&
      millis() - appStateEnteredMs() > THINKING_TIMEOUT_MS) {
    Serial.println("[State] No backend response — returning to idle");
    s_conversationActive = false;
    appStateSet(AssistantState::IDLE);
  }
}
