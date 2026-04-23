// ======================================================================
// SoundRecorder — ES7210 Microphone WAV Recorder
// Waveshare ESP32-S3-Touch-AMOLED-2.06
// Recording a 30-second demo from the Internal microphone to the SD card
// ======================================================================

#include "FS.h"
#include "HWCDC.h"
#include "SD_MMC.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>

HWCDC USBSerial;

// Increase Arduino loop task stack
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ================= PINS =================
#define IIC_SDA 15
#define IIC_SCL 14
#define I2S_BCLK (gpio_num_t)41
#define I2S_LRCK (gpio_num_t)45
#define I2S_DOUT (gpio_num_t)40
#define I2S_DIN (gpio_num_t)42
#define I2S_MCLK (gpio_num_t)16
#define PA_CTRL 46
#define SDMMC_CLK 2
#define SDMMC_CMD 1
#define SDMMC_DATA 3

// ================= CONFIG =================
#define SAMPLE_RATE 16000
#define RECORD_SECONDS 30
#define ES7210_ADDR 0x40
#define ES8311_ADDR 0x18

// ================= GLOBALS =================
i2s_chan_handle_t rx_handle = NULL;
i2s_chan_handle_t tx_handle = NULL;

void i2c_scan() {
  USBSerial.println("[I2C] Scanning bus...");
  for (byte a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      USBSerial.printf("[I2C]   0x%02X\n", a);
    }
  }
}

// ================= SD =================
bool init_sd() {
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  if (!SD_MMC.begin("/sdcard", true)) {
    USBSerial.println("[SD] ERROR: Card Mount Failed!");
    return false;
  }
  USBSerial.printf("[SD] OK. Size: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
  return true;
}

// ================= I2S =================
void init_i2s() {
  USBSerial.println("[I2S] Init Full-Duplex (16kHz, 16-bit, STEREO)...");

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 512;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_MCLK,
          .bclk = I2S_BCLK,
          .ws = I2S_LRCK,
          .dout = I2S_DOUT,
          .din = I2S_DIN,
          .invert_flags = {false, false, false},
      },
  };
  
  // Explicitly ensure MCLK is active and standard 256 multiplier
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
  USBSerial.println("[I2S] Full-duplex Stereo enabled. MCLK on GPIO16.");
}

// ================= I2C HELPERS =================
uint8_t read_reg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)addr, (uint8_t)1);
  return Wire.read();
}

void write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void update_reg(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t val) {
  uint8_t current = read_reg(addr, reg);
  uint8_t next = (current & ~mask) | (val & mask);
  write_reg(addr, reg, next);
}

// ================= CODECS =================

void init_es8311() {
  USBSerial.println("[ES8311] Init (DAC mode, ADC SDOUT tri-stated)...");

  write_reg(ES8311_ADDR, 0x32, 0x00);
  write_reg(ES8311_ADDR, 0x17, 0x00);
  write_reg(ES8311_ADDR, 0x0E, 0xFF);
  write_reg(ES8311_ADDR, 0x12, 0x02);
  write_reg(ES8311_ADDR, 0x14, 0x00);
  write_reg(ES8311_ADDR, 0x0D, 0xFA);
  write_reg(ES8311_ADDR, 0x15, 0x00);
  write_reg(ES8311_ADDR, 0x02, 0x10);
  write_reg(ES8311_ADDR, 0x00, 0x00);
  write_reg(ES8311_ADDR, 0x00, 0x1F);
  write_reg(ES8311_ADDR, 0x01, 0x30);
  write_reg(ES8311_ADDR, 0x01, 0x00);
  write_reg(ES8311_ADDR, 0x45, 0x00);
  write_reg(ES8311_ADDR, 0x0D, 0xFC);
  write_reg(ES8311_ADDR, 0x02, 0x00);

  delay(20);

  write_reg(ES8311_ADDR, 0x00, 0x80); 
  write_reg(ES8311_ADDR, 0x01, 0x3F);

  // DAC Mode, ADC Muted/Tri-Stated
  update_reg(ES8311_ADDR, 0x09, 0x40, 0x00); 
  update_reg(ES8311_ADDR, 0x0A, 0x40, 0x40); 
  
  // 16-bit
  update_reg(ES8311_ADDR, 0x09, 0x1C, 0x0C); 
  update_reg(ES8311_ADDR, 0x0A, 0x1C, 0x0C);

  write_reg(ES8311_ADDR, 0x17, 0xBF);
  write_reg(ES8311_ADDR, 0x0E, 0x02);
  write_reg(ES8311_ADDR, 0x12, 0x00);
  write_reg(ES8311_ADDR, 0x14, 0x1A);
  
  update_reg(ES8311_ADDR, 0x14, 0x40, 0x00); // Disable PDM

  write_reg(ES8311_ADDR, 0x0D, 0x01);
  write_reg(ES8311_ADDR, 0x15, 0x40);
  write_reg(ES8311_ADDR, 0x37, 0x08);
  write_reg(ES8311_ADDR, 0x45, 0x00);
  write_reg(ES8311_ADDR, 0x32, 0x00); // 0dB attenuation
}

void init_es7210() {
  USBSerial.println("[ES7210] Init with STRICT Read-Modify-Write...");

  write_reg(ES7210_ADDR, 0x00, 0xFF);
  write_reg(ES7210_ADDR, 0x00, 0x41);
  write_reg(ES7210_ADDR, 0x01, 0x3F);
  write_reg(ES7210_ADDR, 0x09, 0x30);
  write_reg(ES7210_ADDR, 0x0A, 0x30);
  write_reg(ES7210_ADDR, 0x23, 0x2A);
  write_reg(ES7210_ADDR, 0x22, 0x0A);
  write_reg(ES7210_ADDR, 0x20, 0x0A);
  write_reg(ES7210_ADDR, 0x21, 0x2A);
  
  update_reg(ES7210_ADDR, 0x08, 0x01, 0x00); // master_mode = false
  
  write_reg(ES7210_ADDR, 0x40, 0x43);
  write_reg(ES7210_ADDR, 0x41, 0x70);
  write_reg(ES7210_ADDR, 0x42, 0x70);
  write_reg(ES7210_ADDR, 0x07, 0x20); // OSR
  write_reg(ES7210_ADDR, 0x02, 0xC1); // MAINCLK

  update_reg(ES7210_ADDR, 0x11, 0xE0, 0x60); // 16-bit
  update_reg(ES7210_ADDR, 0x11, 0x03, 0x00); // ES_I2S_NORMAL
  
  write_reg(ES7210_ADDR, 0x01, 0x3F); 
  write_reg(ES7210_ADDR, 0x06, 0x00);
  write_reg(ES7210_ADDR, 0x40, 0x43);
  write_reg(ES7210_ADDR, 0x47, 0x08); 
  write_reg(ES7210_ADDR, 0x48, 0x08); 
  write_reg(ES7210_ADDR, 0x49, 0x08); 
  write_reg(ES7210_ADDR, 0x4A, 0x08); 

  update_reg(ES7210_ADDR, 0x43, 0x10, 0x00); 
  update_reg(ES7210_ADDR, 0x44, 0x10, 0x00);
  update_reg(ES7210_ADDR, 0x45, 0x10, 0x00);
  update_reg(ES7210_ADDR, 0x46, 0x10, 0x00);
  
  write_reg(ES7210_ADDR, 0x4B, 0xFF);
  write_reg(ES7210_ADDR, 0x4C, 0xFF);
  
  update_reg(ES7210_ADDR, 0x01, 0x0B, 0x00); // Enable clocks for MIC1, MIC2
  write_reg(ES7210_ADDR, 0x4B, 0x00); // MIC12 ON
  
  // --- MIC1 Gain ---
  // 37.5dB (0x0E) - Absolute maximum hardware analog gain
  update_reg(ES7210_ADDR, 0x43, 0x10, 0x10); // Enable PGA
  update_reg(ES7210_ADDR, 0x43, 0x0F, 0x0E); 
  
  // --- MIC2 Gain ---
  // 37.5dB (0x0E) - Absolute maximum hardware analog gain
  update_reg(ES7210_ADDR, 0x44, 0x10, 0x10); // Enable PGA
  update_reg(ES7210_ADDR, 0x44, 0x0F, 0x0E);

  write_reg(ES7210_ADDR, 0x12, 0x00); // No TDM

  write_reg(ES7210_ADDR, 0x40, 0x43);
  write_reg(ES7210_ADDR, 0x00, 0x71);
  write_reg(ES7210_ADDR, 0x00, 0x41);
  
  USBSerial.println("[ES7210] Done.");
}

// ================= WAV =================
#pragma pack(push, 1)
struct WavHeader {
  char riff[4] = {'R', 'I', 'F', 'F'};
  uint32_t overall_size = 0;
  char wave[4] = {'W', 'A', 'V', 'E'};
  char fmt[4] = {'f', 'm', 't', ' '};
  uint32_t fmt_len = 16;
  uint16_t format = 1;
  uint16_t channels = 2; // STEREO
  uint32_t sample_rate = SAMPLE_RATE;
  uint32_t byterate = SAMPLE_RATE * 2 * 2; 
  uint16_t block_align = 4;                
  uint16_t bits = 16;
  char data[4] = {'d', 'a', 't', 'a'};
  uint32_t data_size = 0;
};
#pragma pack(pop)

void record_wav(const char *path, uint32_t secs) {
  uint32_t targetBytes = secs * SAMPLE_RATE * 2 * 2; 

  USBSerial.printf("[REC] Allocating %u bytes PSRAM...\n", targetBytes);
  uint8_t *psram = (uint8_t *)heap_caps_malloc(targetBytes, MALLOC_CAP_SPIRAM);
  if (!psram) {
    USBSerial.println("[REC] PSRAM alloc failed!");
    return;
  }

  USBSerial.println("[REC] Discarding settling samples...");
  uint8_t discard[4096];
  for (int i = 0; i < 8; i++) {
    size_t br = 0;
    i2s_channel_read(rx_handle, discard, sizeof(discard), &br, portMAX_DELAY);
  }

  USBSerial.printf("[REC] === RECORDING %us ===\n", secs);
  uint32_t written = 0;
  int16_t buf[2048]; 
  size_t br = 0;
  uint32_t last_print = 0, start = millis();

  while (written < targetBytes) {
    br = 0;
    i2s_channel_read(rx_handle, buf, sizeof(buf), &br, portMAX_DELAY);
    if (br > 0) {
      uint32_t rem = targetBytes - written;
      uint32_t cp = (br < rem) ? br : rem;
      
      // Ensure we only copy complete stereo frames (multiples of 4 bytes)
      cp = (cp / 4) * 4;
      
      if (cp > 0) {
        // --- Software Digital Gain ---
        // Increase volume by x4 (+12dB) to catch distant sounds.
        // Hard clipping is applied to prevent integer overflow (which causes horrible noise).
        int16_t *samples = (int16_t *)buf;
        int num_samples = cp / 2;
        for (int i = 0; i < num_samples; i++) {
          int32_t val = (int32_t)samples[i] * 4; 
          if (val > 32767) val = 32767;
          else if (val < -32768) val = -32768;
          samples[i] = (int16_t)val;
        }

        memcpy(psram + written, buf, cp);
        written += cp;
      }
    }

    if (millis() - last_print >= 3000) {
      last_print = millis();
      int stereo_frames = br / 4;
      int16_t peakL = 0, peakR = 0;
      for (int i = 0; i < stereo_frames; i++) {
        if (abs(buf[i * 2]) > abs(peakL)) peakL = buf[i * 2];
        if (abs(buf[i * 2 + 1]) > abs(peakR)) peakR = buf[i * 2 + 1];
      }
      USBSerial.printf("[REC] %us/%us | %u/%u | L:%d R:%d\n",
                       (millis() - start) / 1000, secs, written, targetBytes,
                       (int)peakL, (int)peakR);
    }
  }

  USBSerial.println("[REC] Flushing to SD...");
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    USBSerial.println("[REC] SD open failed!");
    heap_caps_free(psram);
    return;
  }

  WavHeader h;
  h.data_size = written;
  h.overall_size = written + 36;
  f.write((uint8_t *)&h, sizeof(h));

  uint32_t sw = 0;
  while (sw < written) {
    uint32_t chunk = ((written - sw) < 32768) ? (written - sw) : 32768;
    size_t w = f.write(psram + sw, chunk);
    if (w == 0) {
      USBSerial.println("[REC] SD write stall!");
      break;
    }
    sw += w;
  }

  f.close();
  heap_caps_free(psram);
  USBSerial.printf("[REC] DONE! %u bytes -> %s\n", written + 44, path);
}

// ================= SETUP =================
void setup() {
  USBSerial.begin(115200);
  delay(1500);
  USBSerial.println("\n=== 06_RecTest: ES7210 Mic Recorder ===\n");

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, HIGH);
  USBSerial.println("[SYS] PA_CTRL=HIGH");
  delay(50);

  i2c_scan();

  bool sd_ok = init_sd();
  if (!sd_ok) {
    USBSerial.println("[SYS] No SD card. Insert and RESET.");
  }

  init_i2s();
  delay(100);

  init_es8311();
  delay(50);
  
  init_es7210();
  delay(50);

  if (sd_ok) {
    USBSerial.println("\n--- Recording in 1s ---\n");
    delay(1000);
    record_wav("/record.wav", RECORD_SECONDS);
  }

  i2s_channel_disable(rx_handle);
  i2s_channel_disable(tx_handle);
  USBSerial.println("\n[SYS] Done. Check SD card.");
}

void loop() { delay(10000); }
