#pragma once
#include <Adafruit_NeoPixel.h>

#include "AssistantState.h"
#include "Display.h"

// Assistant state machine + user feedback (LCD face, RGB LED).
// Owns the current AssistantState; every transition updates the face and
// the LED in one place, so "what state am I in" is always visible.

// Call once from setup(); `led` must outlive the program (main owns it).
void appStateInit(Adafruit_NeoPixel *led);

// Transition (no-op if already there). Starts/stops mic capture when
// entering/leaving LISTENING, and repaints the face + LED.
void appStateSet(AssistantState s);

AssistantState appStateGet();

// "IDLE" / "LISTENING" / ... — for serial logs and the info screen.
const char *appStateName(AssistantState s);

// millis() timestamp of the last transition — used for state timeouts.
unsigned long appStateEnteredMs();

// Repaints the face screen and restores the WiFi/WS status dots that a
// full-screen redraw wipes out (e.g. for emotion overlays). `caption` is
// optional model text word-wrapped under the face (see displayShowFace).
void appShowFace(Expression expr, const char *statusText,
                 const char *caption = nullptr);

// Redraws the face matching the CURRENT state — used to restore the screen
// after a temporary overlay (volume bar, info screen).
void appRepaintFace();

// The model's set_emotion(emotion, text) for the current turn: shows the
// face + caption now and keeps them through the THINKING/SPEAKING repaints
// (otherwise the first TTS chunk would wipe them with the generic
// "Speaking..." face). When the turn completes the state machine moves to
// RESULT, which keeps drawing this face + caption until the talk button
// starts the next round (Conversation.cpp clears it then, and on
// abort/interrupt/timeouts).
void appSetTurnFace(Expression expr, const char *caption);
void appClearTurnFace();
// True if the model has set a face/caption for the current turn — i.e.
// there is reply content worth holding on the RESULT screen.
bool appTurnFaceActive();
