#pragma once

#include <Arduino.h>
#include "audio_version.h"

// Audio module: ES8311 codec + I2S speaker/mic path. Requires Wire to be started first.
bool audio_begin(void);
void audio_set_volume(uint8_t volume);  // 0 - 100
void audio_beep(void);
