# 持续集成

[English](ci.md)

本仓库使用 `Build Examples` GitHub Actions 工作流验证构建并打包源码构建的固件工件。

## ESP-IDF

工作流发现 `examples/esp-idf/` 下的第一方项目，并在 ESP32-S3 上使用 ESP-IDF
`v5.5.5` 和 `v6.0.2` 构建。

## Arduino

工作流发现 `examples/arduino/` 下的第一方草图，使用 Arduino-ESP32 `3.3.11`、既有
FQBN 选项和 `examples/arduino/libraries/` 中的随附库。库内上游示例不属于产品 CI。

## 手动运行

可用 `target=all`、示例目录名或仓库相对示例路径手动启动工作流。

## 工件和路由

成功构建由 `releases/package_firmware.py` 打包为 `firmware-*` 工件。轻量质量和范围
作业始终可见；完整的仅文档变更会按设计跳过昂贵的示例构建，缺少或不可用的差异会失败。
`FirmWare/` 中的文件单独报告，不进入示例发现或构建。
