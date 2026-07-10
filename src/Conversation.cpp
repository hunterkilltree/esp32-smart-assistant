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
  s_discardTurn = false;
  Serial.println("[State] Engine session ready");
  if (appStateGet() == AssistantState::RESULT) {
    // Session auto-reconnect (e.g. Gemini goAway) while a result is held —
    // keep the response screen, just flip the WS dot back to green.
    displayShowConnectivity(WiFi.status() == WL_CONNECTED, true);
    return;
  }
  if (appStateGet() == AssistantState::LISTENING) {
    // Reconnected mid-round (held through the drop in onDisconnected) —
    // resume the call; session resumption restores the conversation.
    Serial.println("[State] Reconnected — still listening");
    displayShowConnectivity(WiFi.status() == WL_CONNECTED, true);
    return;
  }
  s_conversationActive = false;
  appStateSet(AssistantState::IDLE);
  appShowFace(Expression::HAPPY, "Connected!");
}

void onDisconnected() {
  s_discardTurn = false;
  audioPlaybackClear();
  AssistantState st = appStateGet();
  if (st == AssistantState::LISTENING) {
    // Mid-round drop must not yank the user out of their call: stay
    // LISTENING (red dot shows the gap; uplink no-ops until the socket is
    // back and the session resumes). Audio spoken during the gap is lost.
    // The LISTENING timeout still ends the round if reconnect never lands.
    Serial.println("[State] WS drop while listening — holding LISTENING");
    displayShowConnectivity(WiFi.status() == WL_CONNECTED, false);
    return;
  }
  s_conversationActive = false;
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
  // Turn died before anything could be held on screen (e.g. the socket
  // dropped before set_emotion arrived). The settle check only runs in
  // RESULT and the next button press wipes the buffer — relay whatever
  // reply text was already collected now, or it is lost.
  if (s_transcriptPending) {
    Serial.println("[Speak] WS drop — flushing collected reply text");
    transcriptFlush();
  }
  appClearTurnFace();
  appStateSet(AssistantState::IDLE);
  // appStateSet no-ops if already IDLE — still flip the WS dot to red.
  displayShowConnectivity(WiFi.status() == WL_CONNECTED, false);
}

// No onAudio callback is registered: the board has no speaker, so the
// engine discards TTS frames without decoding them. The reply is spoken by
// the LAN speak server (SpeakServer.h) and SPEAKING is never entered —
// re-register an onAudio handler here if an amp is ever wired.

void onUserSpeechEnd() {
  if (appStateGet() == AssistantState::LISTENING) {
    appStateSet(AssistantState::THINKING);
  }
}

void onTurnComplete() {
  s_discardTurn = false;
  // Tail of an aborted turn (button already reset us to IDLE) — ignore.
  if (appStateGet() == AssistantState::IDLE) {
    s_conversationActive = false;
    transcriptClear();
    return;
  }
  // Still LISTENING means no reply content ever arrived — this turnComplete
  // closes a reply that was cancelled by user speech. The engine will
  // answer the utterance again on its own: keep the round open instead of
  // flashing an empty RESULT screen (which would also drop that follow-up
  // reply as stale).
  if (appStateGet() == AssistantState::LISTENING) {
    Serial.println("[State] Turn ended with no reply — still listening");
    return;
  }
  s_conversationActive = false;
  // Hold the reply's face + caption on the RESULT screen; the next
  // talk-button press clears it and reopens the mic.
  appStateSet(AssistantState::RESULT);
  // Text-only replies: everything has arrived by turnComplete — relay to
  // the speak server right away (queued to the background sender).
  if (!s_speechText[0] && s_transcriptLen == 0) {
    Serial.println("[Speak] No reply text this turn — nothing to send");
  } else {
    transcriptFlush();
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
  // First reply content: text-only turns have no audio to signal "model is
  // replying" — the tool call is that signal. Leaving LISTENING also stops
  // the mic pump so stray noise can't cancel the reply being generated.
  if (appStateGet() == AssistantState::LISTENING) {
    appStateSet(AssistantState::THINKING);
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
  if (appStateGet() == AssistantState::LISTENING) {
    appStateSet(AssistantState::THINKING);  // reply started (see onEmotion)
  }
  snprintf(s_speechText, sizeof(s_speechText), "%s", text);
  s_transcriptPending = true;
  s_transcriptLastChunkMs = millis();
}

// Streamed reply-text chunks (modelTurn text parts) — accumulated as the
// fallback when the model omits the speech argument. Overflow beyond
// TRANSCRIPT_MAX keeps the head of the reply and drops the rest.
void onTranscript(const char *text) {
  if (s_discardTurn || !text) return;
  AssistantState st = appStateGet();
  if (st == AssistantState::IDLE || st == AssistantState::RESULT) {
    return;  // stale chunk of an aborted or already-flushed turn
  }
  if (st == AssistantState::LISTENING) {
    appStateSet(AssistantState::THINKING);  // reply started (see onEmotion)
  }
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

  // ---- Speak relay fallback ----
  // A clean turnComplete flushes immediately (onTurnComplete). This covers
  // the RESULT holds that never get one — WS drop / interrupt mid-reply —
  // sending the collected text once no more chunks are arriving.
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

    // End the round on prolonged user silence (voice-based, so a WS drop
    // never ends it), with an absolute cap as backstop for a mic whose
    // noise floor never dips below the VAD threshold (or one that's dead —
    // a dead mic reads silence and trips the silence timeout).
    bool silentTooLong =
        audioCaptureMsSinceVoice() > LISTENING_SILENCE_TIMEOUT_MS;
    if (silentTooLong ||
        millis() - appStateEnteredMs() > LISTENING_MAX_MS) {
      Serial.printf("[State] %s — closing conversation\n",
                    silentTooLong ? "No voice from user"
                                  : "Listening round hit max length");
      s_conversationActive = false;
      if (s_transcriptPending) transcriptFlush();  // don't lose reply text
      appClearTurnFace();
      appStateSet(AssistantState::IDLE);
    }
  }

  // Engine never answered — don't stay wedged on the thinking face.
  if (appStateGet() == AssistantState::THINKING &&
      millis() - appStateEnteredMs() > THINKING_TIMEOUT_MS) {
    Serial.println("[State] No engine response — returning to idle");
    s_conversationActive = false;
    if (s_transcriptPending) transcriptFlush();  // don't lose reply text
    appClearTurnFace();
    appStateSet(AssistantState::IDLE);
  }
}
