# 出厂固件源码

[English](README.md)

本 ESP-IDF 工程构建开发板的出厂固件：ESP-Brookesia 应用启动器及其内置应用。

CI 使用 `.github/product.json` 中指定的 ESP-IDF 版本构建它。实测过、可直接烧录的固件在上一级
[`firmware`](../) 目录。

用同一 ESP-IDF 版本在本地构建，在仓库根目录运行：

```bash
idf.py -C firmware/sourcecode set-target esp32s3 build
```
