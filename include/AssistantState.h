#pragma once

// Assistant state machine (mirrors the spec's IDLE / STREAMING /
// PROCESSING / RESPONDING flow):
//   IDLE      — waiting for a wake trigger
//   LISTENING — mic open, PCM streaming to the backend
//   THINKING  — utterance ended, waiting on the backend (STT + LLM + TTS)
//   SPEAKING  — TTS audio arriving / playing
enum class AssistantState {
    IDLE,
    LISTENING,
    THINKING,
    SPEAKING
};
