# 贡献指南

[English](CONTRIBUTING.md)

感谢您帮助改进本开发板仓库。

## 流程

1. Fork 仓库并创建主题分支。
2. 将改动集中在一个缺陷、示例或文档更新上。
3. 路径、设置或行为变化时更新受影响的文档。
4. 提交包含简明说明和验证记录的 Pull Request。

## 验证

Pull Request 应使用 GitHub Actions 进行构建验证。硬件行为发生变化时，请说明板卡版本、示例
路径和已执行的手工检查。ESP-IDF 示例位于 `examples/esp-idf/`，Arduino 草图和随附库位于
`examples/arduino/`；随附库中的示例默认不属于产品 CI。
