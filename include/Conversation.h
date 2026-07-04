#pragma once

// Conversation logic: wires the AI engine callbacks (audio, emotion tool
// calls, turn events) to the state machine, handles the talk button
// (start / stop / barge-in), pumps mic PCM chunks to the engine while
// LISTENING, and enforces the listening/thinking safety timeouts.

// Registers the engine callbacks (calls aiEngineInit). Call once from
// setup(), before the first backendSessionLoop().
void conversationInit();

// Call every loop() iteration while the backend session is RUNNING.
void conversationLoop(bool buttonPressed);
