# 发布脚本

[English](README.md)

本目录提供将构建输出打包成可刷写固件归档包的辅助脚本。ESP-IDF 和 Arduino 示例构建完成后，
使用 `releases/package_firmware.py` 生成包含 `manifest.json`、刷写脚本、刷写参数及
`bin/combined.bin` 的归档包。

Arduino 示例使用既有 ESP32-S3 FQBN 选项、随附库与 Arduino-ESP32 `3.3.11`。可使用
`releases/download_artifacts.py` 下载已完成 CI 运行的工件；推送 `v*` 标签会将生成的
`firmware-*` 归档包发布到对应 GitHub Release。
