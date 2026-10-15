# 03_esp-brookesia

[English](README.md) | [简体中文](README_ZH.md)

## 功能

运行 ESP-Brookesia 手机风格界面，内置 SquareLine 演示应用。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板

## 怎么运行

```bash
idf.py set-target esp32s3 build flash monitor
```

## 预期现象

屏幕显示带状态栏时钟和应用图标的主界面，触摸可以切换页面并打开演示应用。

## 常见问题

- 除非在硬件上用所有支持的 ESP-IDF 版本重新测试过，否则不要修改 `sdkconfig.defaults` 中的内存相关选项。
- 升级 LVGL、显示、触摸或 Brookesia 依赖前，确认本示例在所有支持的 ESP-IDF 版本下仍能编译。
