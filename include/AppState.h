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

// millis() timestamp of the last transition — used for state timeouts.
unsigned long appStateEnteredMs();

// Repaints the face screen and restores the WiFi/WS status dots that a
// full-screen redraw wipes out (e.g. for emotion overlays).
void appShowFace(Expression expr, const char *statusText);

// Redraws the face matching the CURRENT state — used to restore the screen
// after a temporary overlay (volume bar, info screen).
void appRepaintFace();
