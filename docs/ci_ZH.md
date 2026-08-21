# 持续集成

[English](ci.md)

本仓库使用 `Build Examples` GitHub Actions 工作流验证构建并打包源码构建的固件工件。

## ESP-IDF

工作流发现 `examples/esp-idf/` 下的第一方项目，并在 ESP32-S3 上使用 ESP-IDF
`v5.5.5` 和 `v6.0.2` 构建。

## 文档检查

完整、自包含的 Markdown 审核使用 `config/markdown-audit.json` 检查所有权、双语配对和互链、
本地及 HTML 链接/片段、主页结构和公开文本隐私。PR 和 push 的变更范围使用完整的重命名感知输入：

```text
python3 scripts/audit_markdown.py . --changed-files-from changed-files.txt --config config/markdown-audit.json
```

当路由分类器确认每个旧路径和新路径均为 Markdown 或精确列入允许列表的文档资源文件时，CI 还会添加
`--expect-docs-only`。缺失、为空、格式错误或不可用的变更文件数据会使范围作业失败，不会静默通过或选择全部构建。

旧版轻量检查器仍保留，用于兼容其针对主页、双语链接和本地链接的约定：

```text
python3 scripts/check_docs.py . --config config/markdown-audit.json
```

该检查器有意保持为范围有限的仓库检查器，不能替代完整审核。

## Arduino

工作流发现 `examples/arduino/` 下的第一方草图，使用 Arduino-ESP32 `3.3.11`、既有
FQBN 选项和 `examples/arduino/libraries/` 中的随附库。库内上游示例不属于产品 CI。

## 手动运行

可用 `target=all`、示例目录名或仓库相对示例路径手动启动工作流。

## 工件和路由

成功构建由 `releases/package_firmware.py` 打包为 `firmware-*` 工件。轻量质量和范围
作业始终可见；完整的仅文档变更会按设计跳过昂贵的示例构建，缺少或不可用的差异会失败。
`FirmWare/` 中的文件单独报告，不进入示例发现或构建。
