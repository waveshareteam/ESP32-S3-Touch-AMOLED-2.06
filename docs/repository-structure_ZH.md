# 仓库结构

[English](repository-structure.md)

本仓库的维护目录如下：

```text
examples/esp-idf/      第一方 ESP-IDF 项目
examples/arduino/      第一方 Arduino 草图和随附库
FirmWare/              已提交的出厂或恢复二进制文件
Material/              产品媒体和资料
Schematic/             原理图 PDF
releases/              CI 固件打包和下载工具
```

CI 仅将第一方示例根目录识别为产品示例，并排除随附库内的示例。请使用以上维护路径。

## 组件归属

随附的 Arduino `XPowersLib` 会保留，因为尚未确认适用于本仓库的等价托管替代品。产品本地的
`bsp_extra` 保留为其示例提供音频/视频适配器和板级胶合代码。Brookesia 树归 Brookesia 示例
所有，并不是通用板级支持的替代品。

`FirmWare/` 是不可变交付边界。当前其中只有一个已提交的二进制文件，没有源码项目或发布归档；
二进制文件和工件会被单独清点，绝不会进入示例构建矩阵。
