# 01_AXP2101

[English](README.md) | [简体中文](README_ZH.md)

## 功能

通过 XPowersLib 经 I2C 读取 AXP2101 电源管理芯片，打印各路电源、电池和充电数据。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板
- 锂电池（可选）

## 怎么运行

```bash
idf.py set-target esp32s3 build flash monitor
```

## 预期现象

串口先打印一次各路电源的开关状态和电压，之后每秒打印 PMU 温度、充电状态，以及电池、VBUS 和系统电压。屏幕不点亮。

## 常见问题

- `Init PMU FAILED!`：在 `idf.py menuconfig` 的 **XPowersLib Configuration** 中检查 I2C 引脚（SDA GPIO15、SCL GPIO14，由 `sdkconfig.defaults` 设置）。
