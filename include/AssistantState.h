#pragma once

// Assistant state machine (mirrors the spec's IDLE / STREAMING /
// PROCESSING / RESPONDING flow, plus a result-hold step):
//   IDLE      — waiting for a wake trigger
//   LISTENING — mic open, PCM streaming to the backend
//   THINKING  — utterance ended, waiting on the backend (STT + LLM + TTS)
//   SPEAKING  — TTS audio arriving / playing
//   RESULT    — reply finished; its face + caption held on screen until
//               the talk button sends us back to LISTENING
enum class AssistantState {
    IDLE,
    LISTENING,
    THINKING,
    SPEAKING,
    RESULT
};
