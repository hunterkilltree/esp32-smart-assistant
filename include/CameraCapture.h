#pragma once
#include <cstddef>
#include <cstdint>

// Initializes the camera using CAMERA_MODEL_ESP32S3_EYE pins.
// Returns true on success. Call once from setup(), after Serial is up.
bool cameraCaptureInit();

// Captures a single JPEG snapshot and invokes onFrame with the encoded
// buffer; the frame is released automatically after the callback returns.
// Returns false if capture failed (camera not initialized, no frame, etc).
bool cameraCaptureSnapshot(void (*onFrame)(const uint8_t *jpegData, size_t len));
