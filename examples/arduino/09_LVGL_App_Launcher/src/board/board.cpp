#include <Wire.h>
#include <sys/time.h>
#include "pin_config.h"
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "SensorPCF85063.hpp"
#include "SensorQMI8658.hpp"
#include "XPowersLib.h"
#include "../board/board.h"

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

static Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT,
                                                22 /* col_offset1 */, 0 /* row_offset1 */,
                                                0 /* col_offset2 */, 0 /* row_offset2 */);

static std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
  std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

static void touch_interrupt(void);

static std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(IIC_Bus, FT3168_DEVICE_ADDRESS,
                                                              DRIVEBUS_DEFAULT_VALUE, TP_INT, touch_interrupt));

static SensorPCF85063 rtc;
static XPowersPMU pmu;
static SensorQMI8658 imu;

static bool s_sleeping = false;
static uint8_t s_brightness = 180;
static bool s_rtc_ok = false;
static bool s_pmu_ok = false;
static bool s_imu_ok = false;

static void touch_interrupt(void) {
  FT3168->IIC_Interrupt_Flag = true;
}

/* ---------------- Display ---------------- */

bool board_display_begin(void) {
  if (!gfx->begin()) {
    return false;
  }
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(s_brightness);
  return true;
}

uint16_t board_display_width(void) {
  return gfx->width();
}

uint16_t board_display_height(void) {
  return gfx->height();
}

void board_display_push(const uint16_t *frame) {
  if (s_sleeping) {
    return;
  }
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)frame, gfx->width(), gfx->height());
}

void board_display_set_brightness(uint8_t level) {
  s_brightness = level;
  if (!s_sleeping) {
    gfx->setBrightness(level);
  }
}

void board_display_sleep(void) {
  if (s_sleeping) {
    return;
  }
  gfx->setBrightness(0);
  gfx->displayOff();
  s_sleeping = true;
}

void board_display_wake(void) {
  if (!s_sleeping) {
    return;
  }
  gfx->displayOn();
  s_sleeping = false;
  gfx->setBrightness(s_brightness);
}

bool board_display_is_sleeping(void) {
  return s_sleeping;
}

/* ---------------- Touch ---------------- */

bool board_touch_begin(void) {
  for (int retry = 0; retry < 5; retry++) {
    if (FT3168->begin()) {
      FT3168->IIC_Write_Device_State(FT3168->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
                                     FT3168->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
      return true;
    }
    delay(500);
  }
  return false;
}

bool board_touch_read(int32_t *x, int32_t *y) {
  if (!FT3168->IIC_Interrupt_Flag) {
    return false;
  }
  FT3168->IIC_Interrupt_Flag = false;
  *x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  *y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  return true;
}

/* ---------------- PMU ---------------- */

bool board_pmu_begin(void) {
  s_pmu_ok = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!s_pmu_ok) {
    return false;
  }
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.clearIrqStatus();
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  return true;
}

BatteryInfo board_battery(void) {
  BatteryInfo info = { false, -1, 0, false, false };
  if (!s_pmu_ok) {
    return info;
  }
  info.present = pmu.isBatteryConnect();
  info.percent = info.present ? pmu.getBatteryPercent() : -1;
  info.voltage_mv = info.present ? pmu.getBattVoltage() : 0;
  info.charging = pmu.isCharging();
  info.vbus_in = pmu.isVbusIn();
  return info;
}

bool board_power_key_pressed(void) {
  if (!s_pmu_ok) {
    return false;
  }
  pmu.getIrqStatus();
  bool pressed = pmu.isPekeyShortPressIrq();
  pmu.clearIrqStatus();
  return pressed;
}

/* ---------------- IMU ---------------- */

bool board_imu_begin(void) {
  s_imu_ok = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!s_imu_ok) {
    return false;
  }
  // Low-power accelerometer mode needs the gyroscope disabled
  imu.disableGyroscope();
  imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
                          SensorQMI8658::LPF_MODE_0);
  imu.enableAccelerometer();
  return true;
}

bool board_imu_read_accel(float *x, float *y, float *z) {
  if (!s_imu_ok || !imu.getDataReady()) {
    return false;
  }
  return imu.getAccelerometer(*x, *y, *z);
}

/* ---------------- RTC / time ---------------- */

bool board_rtc_begin(void) {
  s_rtc_ok = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  return s_rtc_ok;
}

bool board_time_load_from_rtc(void) {
  if (!s_rtc_ok) {
    return false;
  }
  RTC_DateTime dt = rtc.getDateTime();
  if (dt.getYear() < 2024) {
    return false;
  }
  struct tm utc = dt.toUnixTime();
  utc.tm_isdst = 0;

  // mktime() interprets tm as local time, so convert under UTC and restore TZ afterwards
  String saved_tz = getenv("TZ") ? getenv("TZ") : "";
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t epoch = mktime(&utc);
  if (saved_tz.length() > 0) {
    setenv("TZ", saved_tz.c_str(), 1);
  } else {
    unsetenv("TZ");
  }
  tzset();

  struct timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  return true;
}

void board_time_save_to_rtc(void) {
  if (!s_rtc_ok) {
    return;
  }
  time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  rtc.setDateTime(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
}

void board_time_apply_tz(const char *posix_tz) {
  setenv("TZ", posix_tz, 1);
  tzset();
}
