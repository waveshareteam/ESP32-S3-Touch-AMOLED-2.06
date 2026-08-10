# 固件工件

[English](firmware.md)

本仓库有两类固件工件：`FirmWare/` 下已提交的出厂或恢复二进制文件，以及由
`Build Examples` GitHub Actions 工作流生成的源码构建归档包。

出厂二进制文件用于产品恢复和支持流程，不由 CI 构建、重新打包或重新上传。CI 仅构建维护中
的第一方 ESP-IDF 项目和 Arduino 草图，并生成包含 `manifest.json`、`flash.sh`、`flash.bat`、
`flash_args.txt` 与 `bin/combined.bin` 的 `firmware-*` 工件。

请使用 `releases/download_artifacts.py` 下载已完成工作流的工件。生成和下载的归档包由 Git 忽略。
