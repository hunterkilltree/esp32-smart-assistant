#include "Conversation.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "Display.h"
#include "AppState.h"
#include "AiEngine.h"
#include "AudioCapture.h"
#include "AudioPlayback.h"

namespace {

// If the mic really is dead (see pins_config.h) the engine will never hear
// speech — don't let LISTENING hang forever.
constexpr unsigned long LISTENING_TIMEOUT_MS = 60000;

// Continuous conversation: after the model finishes speaking, reopen the
// mic instead of going idle, until the button ends it.
bool s_conversationActive = false;

// Set when the button aborts a reply: audio still in flight for that turn
// is ignored instead of flipping the state back to SPEAKING.
bool s_discardTurn = false;

// ---- AI engine callbacks (all run on the loop task) ----

void onReady() {
  s_conversationActive = false;
  s_discardTurn = false;
  appStateSet(AssistantState::IDLE);
  appShowFace(Expression::HAPPY, "Connected!");
}

void onDisconnected() {
  s_conversationActive = false;
  s_discardTurn = false;
  audioPlaybackClear();
  appStateSet(AssistantState::IDLE);
  // appStateSet no-ops if already IDLE — still flip the WS dot to red.
  displayShowConnectivity(WiFi.status() == WL_CONNECTED, false);
}

void onAudio(const int16_t *pcm, size_t samples) {
  if (s_discardTurn) return;  // tail of an aborted reply
  // First TTS chunk of the turn doubles as the "model started speaking"
  // signal (neither engine has a separate reliable event for it).
  if (appStateGet() != AssistantState::SPEAKING) {
    appStateSet(AssistantState::SPEAKING);
  }
  audioPlaybackWrite(reinterpret_cast<const uint8_t *>(pcm),
                     samples * sizeof(int16_t));
}

void onUserSpeechEnd() {
  if (appStateGet() == AssistantState::LISTENING) {
    appStateSet(AssistantState::THINKING);
  }
}

void onTurnComplete() {
  s_discardTurn = false;
  if (s_conversationActive) {
    // Continuous conversation: reopen the mic for the next turn.
    appStateSet(AssistantState::LISTENING);
  } else {
    appStateSet(AssistantState::IDLE);
  }
}

void onInterrupted() {
  // User spoke over the reply — the engine already stopped generating.
  audioPlaybackClear();
  if (s_conversationActive) appStateSet(AssistantState::LISTENING);
}

// set_emotion tool call → face. The model is prompted to use exactly
// happy/sad/neutral/thinking, but tolerate close synonyms.
void onEmotion(const char *emotion) {
  if (appStateGet() == AssistantState::IDLE) return;  // stale after abort

  Expression expr;
  const char *caption;
  if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "joy") == 0 ||
      strcmp(emotion, "excited") == 0) {
    expr = Expression::HAPPY;   caption = "Happy";
  } else if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "sorry") == 0 ||
             strcmp(emotion, "angry") == 0) {
    expr = Expression::SAD;     caption = "Sorry...";
  } else if (strcmp(emotion, "thinking") == 0 ||
             strcmp(emotion, "confused") == 0) {
    expr = Expression::THINKING; caption = "Thinking...";
  } else if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "calm") == 0) {
    expr = Expression::NEUTRAL; caption = "Ready";
  } else {
    return;  // unknown keyword — keep the current face
  }
  appShowFace(expr, caption);
}

// ---- Button: start / stop / barge-in ----

void handleButton() {
  switch (appStateGet()) {
    case AssistantState::IDLE:
      Serial.println("[Button] Starting conversation");
      s_conversationActive = true;
      s_discardTurn = false;
      appStateSet(AssistantState::LISTENING);
      break;
    case AssistantState::LISTENING:
      Serial.println("[Button] Stopping conversation");
      s_conversationActive = false;
      appStateSet(AssistantState::IDLE);
      break;
    case AssistantState::THINKING:
    case AssistantState::SPEAKING:
      Serial.println("[Button] Aborting response");
      s_conversationActive = false;
      s_discardTurn = true;
      aiEngineAbort();
      audioPlaybackClear();
      appStateSet(AssistantState::IDLE);
      break;
  }
}

}  // namespace

void conversationInit() {
  AiEngineCallbacks cbs = {};
  cbs.onReady = onReady;
  cbs.onDisconnected = onDisconnected;
  cbs.onAudio = onAudio;
  cbs.onUserSpeechEnd = onUserSpeechEnd;
  cbs.onTurnComplete = onTurnComplete;
  cbs.onInterrupted = onInterrupted;
  cbs.onEmotion = onEmotion;
  aiEngineInit(cbs);
}

void conversationLoop(bool buttonPressed) {
  if (buttonPressed && aiEngineReady()) handleButton();

  // ---- Uplink: pump mic PCM chunks to the engine while listening ----
  if (appStateGet() == AssistantState::LISTENING) {
    static int16_t chunk[AUDIO_CHUNK_SAMPLES];
    while (audioCaptureDequeueChunk(reinterpret_cast<uint8_t *>(chunk))) {
      aiEngineSendAudio(chunk, AUDIO_CHUNK_SAMPLES);
    }

    if (millis() - appStateEnteredMs() > LISTENING_TIMEOUT_MS) {
      Serial.println("[State] Listening timed out — closing conversation");
      s_conversationActive = false;
      appStateSet(AssistantState::IDLE);
    }
  }

  // Engine never answered — don't stay wedged on the thinking face.
  if (appStateGet() == AssistantState::THINKING &&
      millis() - appStateEnteredMs() > THINKING_TIMEOUT_MS) {
    Serial.println("[State] No engine response — returning to idle");
    s_conversationActive = false;
    appStateSet(AssistantState::IDLE);
  }
}
