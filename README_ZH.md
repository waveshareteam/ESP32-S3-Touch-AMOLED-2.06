<div align="center">
  <h1>ESP32-S3-Touch-AMOLED-2.06</h1>
  <p><strong>ESP32-S3 2.06 英寸 410 × 502 QSPI AMOLED 触控开发板</strong></p>
  <p><a href="README.md">English</a> | <strong>简体中文</strong></p>
  <p>
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/actions/workflows/build.yml"><img alt="构建" src="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/actions/workflows/build.yml/badge.svg"></a>
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/releases/latest"><img alt="最新版本" src="https://img.shields.io/github/v/release/waveshareteam/ESP32-S3-Touch-AMOLED-2.06"></a>
    <a href="LICENSE"><img alt="许可证" src="https://img.shields.io/github/license/waveshareteam/ESP32-S3-Touch-AMOLED-2.06"></a>
  </p>
  <p>
    <a href="https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm">🌐 产品</a> &middot;
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/releases/latest">📦 固件</a> &middot;
    <a href="examples/esp-idf/">🧩 ESP-IDF</a> &middot;
    <a href="examples/arduino/examples/">🔧 Arduino</a> &middot;
    <a href="hardware/">📚 文档</a>
  </p>
  <p><img src="docs/images/esp32-s3-touch-amoled-2.06.jpg" alt="ESP32-S3-Touch-AMOLED-2.06 开发板" width="500"></p>
</div>

---

## ✨ 概述

ESP32-S3-Touch-AMOLED-2.06 是一块手表形态的开发板，在 ESP32-S3 上集成了 2.06 英寸 AMOLED
触摸屏、六轴运动传感器、电源管理和音频输入输出。本仓库提供它的 ESP-IDF 和 Arduino 示例、
可直接烧录的示例固件、出厂固件源码，以及原理图。

| 功能 | 器件或接口 |
| --- | --- |
| MCU | ESP32-S3 |
| 存储器 | 32 MB Flash，8 MB PSRAM |
| 显示 | 2.06 英寸 410 × 502 QSPI AMOLED，CO5300 控制器 |
| 触摸 | FT3168 电容触摸控制器，I2C 接口 |
| 电源管理 | AXP2101 |
| 运动传感器 | QMI8658 六轴 IMU |
| 音频 | ES7210 ADC 双数字麦克风；ES8311 音频编解码器 |
| 板级支持 | 托管组件 `waveshare/esp32_s3_touch_amoled_2_06` |

## 📦 快速开始

体验示例最快的方法是使用[最新版本](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/releases/latest)中打包好的可烧录固件。

1. 下载所需示例和框架版本对应的 `*-combined.zip`。
2. 解压后通过 `python -m pip install esptool` 安装 esptool。
3. 通过 USB 连接开发板。
4. Windows 运行 `flash_combined.bat COMx`，Linux 运行 `./flash_combined.sh /dev/ttyACM0`。
5. 如果开发板未自动重启，请手动复位。

出厂固件（恢复开箱演示）：在 [`firmware`](firmware/) 目录下载文件名带 `FactoryOnly` 的文件，源码位于 [`examples/factory`](examples/factory/)。

## 🧪 示例

### ESP-IDF

| 示例 | 主要功能 |
| --- | --- |
| [01_AXP2101](examples/esp-idf/01_AXP2101/README_ZH.md) | 电源管理和电池遥测 |
| [02_lvgl_demo_v9](examples/esp-idf/02_lvgl_demo_v9/README_ZH.md) | LVGL 9 显示与触摸演示 |
| [03_esp-brookesia](examples/esp-idf/03_esp-brookesia/README_ZH.md) | ESP-Brookesia 应用界面 |
| [04_Immersive_block](examples/esp-idf/04_Immersive_block/README_ZH.md) | 由运动传感器驱动的 LVGL 方块演示 |
| [05_Spec_Analyzer](examples/esp-idf/05_Spec_Analyzer/README_ZH.md) | 麦克风频谱分析器 |
| [06_videoplayer](examples/esp-idf/06_videoplayer/README_ZH.md) | 带音频的 SD 卡视频播放 |

### Arduino

| 示例 | 主要功能 |
| --- | --- |
| [01_HelloWorld](examples/arduino/examples/01_HelloWorld/README_ZH.md) | 显示初始化 |
| [02_GFX_AsciiTable](examples/arduino/examples/02_GFX_AsciiTable/README_ZH.md) | GFX 文本和字符渲染 |
| [03_LVGL_PCF85063_simpleTime](examples/arduino/examples/03_LVGL_PCF85063_simpleTime/README_ZH.md) | 使用 PCF85063 RTC 的 LVGL 时钟界面 |
| [04_LVGL_QMI8658_ui](examples/arduino/examples/04_LVGL_QMI8658_ui/README_ZH.md) | LVGL IMU 数据界面 |
| [05_LVGL_AXP2101_ADC_Data](examples/arduino/examples/05_LVGL_AXP2101_ADC_Data/README_ZH.md) | LVGL 电源和电池遥测界面 |
| [06_LVGL_Arduino_v9](examples/arduino/examples/06_LVGL_Arduino_v9/README_ZH.md) | LVGL 9 界面演示 |
| [07_LVGL_SD_Test](examples/arduino/examples/07_LVGL_SD_Test/README_ZH.md) | SD 卡测试 |
| [08_ES8311](examples/arduino/examples/08_ES8311/README_ZH.md) | ES8311 音频输出 |

草图使用的库随仓库提供，位于 [`examples/arduino/libraries`](examples/arduino/libraries/)。

## 🛠️ 开发环境

| 开发框架 | 版本 | 示例数 |
| --- | --- | ---: |
| ESP-IDF | `v5.5.5` | 6 |
| ESP-IDF | `v6.0.2` | 6 |
| Arduino-ESP32 | `3.3.11` | 8 |

## 📚 资料

- [原理图](hardware/schematic/)
- [产品页面](https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm)
- [Wiki](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.06/)

## 🤝 支持

- 示例或仓库问题：[提交 issue](https://gitee.com/waveshare/ESP32-S3-Touch-AMOLED-2.06/issues)。
- 产品使用、硬件或订单问题：[微雪技术支持](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.06/Technical-Support/)。

## 📄 许可证

Apache License 2.0，详见 [LICENSE](LICENSE)。
