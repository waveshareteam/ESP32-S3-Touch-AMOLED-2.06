#include "ESP_I2S.h"
#include "es8311.h"
#include "audio.h"

// Audio pins and settings follow examples/arduino/08_ES8311
#define AUDIO_I2S_MCLK 16
#define AUDIO_I2S_BCLK 41
#define AUDIO_I2S_WS 45
#define AUDIO_I2S_DOUT 42
#define AUDIO_I2S_DIN 40
#define AUDIO_PA_EN 46
#define AUDIO_SAMPLE_RATE 16000

static I2SClass i2s;
static es8311_handle_t codec = nullptr;
static bool s_audio_ok = false;

bool audio_begin(void) {
  pinMode(AUDIO_PA_EN, OUTPUT);
  digitalWrite(AUDIO_PA_EN, HIGH);

  i2s.setPins(AUDIO_I2S_BCLK, AUDIO_I2S_WS, AUDIO_I2S_DOUT, AUDIO_I2S_DIN, AUDIO_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    return false;
  }

  codec = es8311_create(0, ES8311_ADDRRES_0);
  if (codec == nullptr) {
    return false;
  }
  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = AUDIO_SAMPLE_RATE * 256,
    .sample_frequency = AUDIO_SAMPLE_RATE
  };
  if (es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    return false;
  }
  es8311_sample_frequency_config(codec, clk.mclk_frequency, clk.sample_frequency);
  s_audio_ok = true;
  return true;
}

void audio_set_volume(uint8_t volume) {
  if (s_audio_ok) {
    es8311_voice_volume_set(codec, volume, nullptr);
  }
}

void audio_beep(void) {
  if (!s_audio_ok) {
    return;
  }
  // 120 ms, 1 kHz stereo tone generated once
  static const size_t FRAMES = AUDIO_SAMPLE_RATE * 120 / 1000;
  static int16_t *tone = nullptr;
  if (tone == nullptr) {
    tone = (int16_t *)malloc(FRAMES * 2 * sizeof(int16_t));
    if (tone == nullptr) {
      return;
    }
    for (size_t i = 0; i < FRAMES; i++) {
      int16_t s = (int16_t)(8000 * sinf(2.0f * PI * 1000.0f * i / AUDIO_SAMPLE_RATE));
      tone[i * 2] = s;
      tone[i * 2 + 1] = s;
    }
  }
  i2s.write((const uint8_t *)tone, FRAMES * 2 * sizeof(int16_t));
}
