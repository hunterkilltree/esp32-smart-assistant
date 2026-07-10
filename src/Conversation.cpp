#include "Conversation.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "Display.h"
#include "AppState.h"
#include "AiEngine.h"
#include "AudioCapture.h"
#include "AudioPlayback.h"
#include "SpeakServer.h"

namespace {

// If the mic really is dead (see pins_config.h) the engine will never hear
// speech — don't let LISTENING hang forever.
constexpr unsigned long LISTENING_TIMEOUT_MS = 60000;

// True while a turn is in flight (button pressed → reply finished). The
// mic does NOT reopen automatically after a reply: the result screen holds
// until the user presses the talk button again (push-to-talk rounds).
bool s_conversationActive = false;

// Set when the button aborts a reply: audio still in flight for that turn
// is ignored instead of flipping the state back to SPEAKING.
bool s_discardTurn = false;

// Transcript of the model's spoken reply, accumulated from onTranscript
// chunks. It is NOT sent the instant the turn ends: chunks can trail in
// after turnComplete (and after the mid-reply interrupt/WS-drop holds), so
// conversationLoop() posts it once no new chunk has arrived for
// TRANSCRIPT_SETTLE_MS — one POST per reply, always the full text.
char s_transcript[TRANSCRIPT_MAX];
size_t s_transcriptLen = 0;
bool s_transcriptPending = false;   // collected text not yet sent
unsigned long s_transcriptLastChunkMs = 0;

// Authoritative full reply text: the `speech` argument of the set_emotion
// tool call, which arrives complete at the START of the reply — immune to
// the tail-of-reply socket drops that truncate the streamed transcript.
char s_speechText[TRANSCRIPT_MAX];

void transcriptClear() {
  s_transcriptLen = 0;
  s_transcript[0] = '\0';
  s_speechText[0] = '\0';
  s_transcriptPending = false;
}

// Prefer the tool call's full text; fall back to the streamed transcript
// only when the model didn't provide the speech argument.
void transcriptFlush() {
  if (s_speechText[0]) {
    speakServerSend(s_speechText);
  } else if (s_transcriptLen > 0) {
    Serial.println("[Speak] No speech arg — sending streamed transcript");
    speakServerSend(s_transcript);
  }
  transcriptClear();
}

// ---- AI engine callbacks (all run on the loop task) ----

void onReady() {
  s_conversationActive = false;
  s_discardTurn = false;
  Serial.println("it back to ready \n");
  if (appStateGet() == AssistantState::RESULT) {
    // Session auto-reconnect (e.g. Gemini goAway) while a result is held —
    // keep the response screen, just flip the WS dot back to green.
    displayShowConnectivity(WiFi.status() == WL_CONNECTED, true);
    return;
  }
  appStateSet(AssistantState::IDLE);
  appShowFace(Expression::HAPPY, "Connected!");
}

void onDisconnected() {
  s_conversationActive = false;
  s_discardTurn = false;
  audioPlaybackClear();
  AssistantState st = appStateGet();
  if (st == AssistantState::RESULT) {
    // Held response survives WS drops; the dot shows the link is down.
    displayShowConnectivity(WiFi.status() == WL_CONNECTED, false);
    return;
  }
  if ((st == AssistantState::THINKING || st == AssistantState::SPEAKING) &&
      appTurnFaceActive()) {
    // Socket died at the tail of a reply (large TTS frames can starve the
    // WS heartbeat) — the turnComplete that would move us to RESULT is
    // lost. Hold the reply we already have instead of wiping to IDLE.
    Serial.println("[State] WS drop mid-reply — holding RESULT");
    appStateSet(AssistantState::RESULT);
    displayShowConnectivity(WiFi.status() == WL_CONNECTED, false);
    return;  // collected transcript is sent by the settle check in the loop
  }
  appClearTurnFace();
  appStateSet(AssistantState::IDLE);
  // appStateSet no-ops if already IDLE — still flip the WS dot to red.
  displayShowConnectivity(WiFi.status() == WL_CONNECTED, false);
}

void onAudio(const int16_t *pcm, size_t samples) {
  if (s_discardTurn) return;  // tail of an aborted reply
  // First TTS chunk of the turn doubles as the "model started speaking"
  // signal (neither engine has a separate reliable event for it). Only a
  // live turn (LISTENING/THINKING) may flip to SPEAKING — a trailing chunk
  // arriving after turnComplete must not drag RESULT back to SPEAKING
  // (nothing would ever end that phantom turn).
  AssistantState st = appStateGet();
  if (st == AssistantState::LISTENING || st == AssistantState::THINKING) {
    appStateSet(AssistantState::SPEAKING);
  } else if (st != AssistantState::SPEAKING) {
    return;  // RESULT/IDLE — stale audio, drop it
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
  s_conversationActive = false;
  // Tail of an aborted turn (button already reset us to IDLE) — ignore.
  if (appStateGet() == AssistantState::IDLE) {
    transcriptClear();
    return;
  }
  // Hold the reply's face + caption on the RESULT screen; the next
  // talk-button press clears it and reopens the mic. The collected
  // transcript is sent by the settle check in conversationLoop().
  appStateSet(AssistantState::RESULT);
  if (!s_speechText[0] && s_transcriptLen == 0) {
    Serial.println("[Speak] No reply text this turn — nothing to send");
  }
}

void onInterrupted() {
  // The engine cancelled the in-flight reply. The mic only streams while
  // LISTENING, so an "interruption" reported during THINKING/SPEAKING can
  // only come from stale or noisy audio (this board's mic is flaky) — it
  // must NOT yank the state back to LISTENING and skip the RESULT hold.
  audioPlaybackClear();
  switch (appStateGet()) {
    case AssistantState::LISTENING:
      // Reply cancelled because the user kept talking — its face is stale.
      appClearTurnFace();
      transcriptClear();  // partial text of the cancelled reply
      break;  // stay LISTENING; the server will re-endpoint
    case AssistantState::THINKING:
    case AssistantState::SPEAKING:
      if (appTurnFaceActive()) {
        Serial.println("[State] Interrupt mid-reply — holding RESULT");
        appStateSet(AssistantState::RESULT);  // hold what was shown
        // Transcript keeps accumulating; the settle check sends it whole.
      } else {
        appStateSet(AssistantState::IDLE);  // nothing to show yet
      }
      break;
    default:
      break;  // IDLE / RESULT — nothing in flight
  }
}

// set_emotion tool call → face + on-screen caption, kept for the whole
// turn (appSetTurnFace). The model is prompted to use exactly
// happy/sad/neutral/thinking, but tolerate close synonyms. `text` is the
// model's short reply caption; fall back to a fixed word if it's missing.
void onEmotion(const char *emotion, const char *text) {
  // Stale after an abort (IDLE) or after the turn already ended (RESULT).
  if (appStateGet() == AssistantState::IDLE ||
      appStateGet() == AssistantState::RESULT) {
    return;
  }

  Expression expr;
  const char *fallback;
  if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "joy") == 0 ||
      strcmp(emotion, "excited") == 0) {
    expr = Expression::HAPPY;   fallback = "Happy";
  } else if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "sorry") == 0 ||
             strcmp(emotion, "angry") == 0) {
    expr = Expression::SAD;     fallback = "Sorry...";
  } else if (strcmp(emotion, "thinking") == 0 ||
             strcmp(emotion, "confused") == 0) {
    expr = Expression::THINKING; fallback = "Thinking...";
  } else if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "calm") == 0) {
    expr = Expression::NEUTRAL; fallback = "Ready";
  } else {
    return;  // unknown keyword — keep the current face
  }
  appSetTurnFace(expr, (text && text[0]) ? text : fallback);
}

// Full reply text from the set_emotion tool call — arrives once, before
// the audio. Stale calls (aborted turn, or after the turn ended) are
// dropped, mirroring onEmotion.
void onReplyText(const char *text) {
  if (s_discardTurn || !text || !text[0]) return;
  if (appStateGet() == AssistantState::IDLE ||
      appStateGet() == AssistantState::RESULT) {
    return;
  }
  snprintf(s_speechText, sizeof(s_speechText), "%s", text);
  s_transcriptPending = true;
  s_transcriptLastChunkMs = millis();
}

// Streamed transcript of the reply being spoken — accumulate the chunks
// (also during RESULT, to catch trailing ones). Overflow beyond
// TRANSCRIPT_MAX keeps the head of the reply and drops the rest.
void onTranscript(const char *text) {
  if (s_discardTurn || !text) return;
  size_t n = strlen(text);
  size_t room = sizeof(s_transcript) - 1 - s_transcriptLen;
  if (n > room) n = room;
  memcpy(s_transcript + s_transcriptLen, text, n);
  s_transcriptLen += n;
  s_transcript[s_transcriptLen] = '\0';
  s_transcriptPending = true;
  s_transcriptLastChunkMs = millis();
}

// ---- Button: start / stop / barge-in ----

void handleButton() {
  switch (appStateGet()) {
    case AssistantState::IDLE:
      Serial.println("[Button] Starting conversation");
      s_conversationActive = true;
      s_discardTurn = false;
      transcriptClear();
      appStateSet(AssistantState::LISTENING);
      break;
    case AssistantState::RESULT:
      // Done reading the held response — back to listening. If the button
      // beat the settle timer, send the reply text now rather than lose it.
      Serial.println("[Button] Next round — listening");
      if (s_transcriptPending) transcriptFlush();
      s_conversationActive = true;
      s_discardTurn = false;
      transcriptClear();
      appClearTurnFace();
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
      appClearTurnFace();
      transcriptClear();
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
  cbs.onReplyText = onReplyText;
  cbs.onTranscript = onTranscript;
  aiEngineInit(cbs);
}

void conversationLoop(bool buttonPressed) {
  if (buttonPressed && aiEngineReady()) handleButton();

  // ---- Speak relay: one POST per reply, after the transcript settles ----
  // Runs while the RESULT screen is held; waiting for a quiet gap makes
  // sure trailing transcript chunks made it into the message.
  if (s_transcriptPending && appStateGet() == AssistantState::RESULT &&
      millis() - s_transcriptLastChunkMs > TRANSCRIPT_SETTLE_MS) {
    transcriptFlush();
  }

  // ---- Uplink: pump mic PCM chunks to the engine while listening ----
  if (appStateGet() == AssistantState::LISTENING) {
    static int16_t chunk[AUDIO_CHUNK_SAMPLES];
    while (audioCaptureDequeueChunk(reinterpret_cast<uint8_t *>(chunk))) {
      aiEngineSendAudio(chunk, AUDIO_CHUNK_SAMPLES);
    }

    if (millis() - appStateEnteredMs() > LISTENING_TIMEOUT_MS) {
      Serial.println("[State] Listening timed out — closing conversation");
      s_conversationActive = false;
      appClearTurnFace();
      appStateSet(AssistantState::IDLE);
    }
  }

  // Engine never answered — don't stay wedged on the thinking face.
  if (appStateGet() == AssistantState::THINKING &&
      millis() - appStateEnteredMs() > THINKING_TIMEOUT_MS) {
    Serial.println("[State] No engine response — returning to idle");
    s_conversationActive = false;
    appClearTurnFace();
    appStateSet(AssistantState::IDLE);
  }
}
