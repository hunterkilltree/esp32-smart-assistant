#pragma once

// Conversation logic: wires the xiaozhi protocol callbacks (tts/stt/llm
// emotion/goodbye) to the state machine, handles the wake button
// (start / stop / barge-in), pumps encoded mic frames to the server while
// LISTENING, and enforces the listening/thinking safety timeouts.

// Registers the protocol callbacks (calls xzInit). Call once from setup(),
// before the first backendSessionLoop()/xiaozhiOtaCheck().
void conversationInit();

// Call every loop() iteration while the backend session is RUNNING.
void conversationLoop(bool buttonPressed);
