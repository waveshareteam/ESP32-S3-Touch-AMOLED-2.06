# `bsp_extra` 开发板适配

[English](README.md)

`bsp_extra` 是位于 ESP32-S3-Touch-AMOLED-2.06 BSP 之上的固件本地适配层。
它为 MusicPlayer、VideoPlayer、Recorder、SpecAnalyzer 和 Xiaozhi 提供共同使用的
ES8311/ES7210 编解码器会话，并提供 MusicPlayer 所需的文件播放器适配接口。

可供多个产品复用的开发板支持应在 Waveshare 托管 BSP 中维护。只有负责协调本
Brookesia 产品应用或生命周期的代码才应保留在这里。

## 音频拓扑

当前托管 BSP 提供一套共享的 I2S 控制器和编解码器数据接口：

- ES8311 播放使用标准立体声 I2S。
- 选择语音/采集模式时，ES7210 使用四时隙 TDM 进行采集。
- ES7210 串行输出的时隙顺序为 MIC1、MIC3/参考、MIC2、MIC4。
- TDM 时隙掩码用于选择读取操作返回的串行时隙；物理麦克风掩码用于选择执行
  增益控制的 ES7210 输入。两者属于不同的命名空间，不能混用。

`bsp_extra_codec_init()` 初始化由 BSP 管理的标准 I2S TX 与 TDM RX 混合通路，
取得由 BSP 管理的扬声器和麦克风编解码器句柄，并打开默认的 16 kHz、16 位、
立体声播放格式。随后可通过两个格式接口选择工作模式：

- `bsp_extra_codec_set_fs()` 先关闭当前编解码器流，再按指定采样率、位宽和
  单/立体声模式重新打开 ES8311 播放。
- `bsp_extra_codec_set_voice_fs()` 先关闭当前流，然后打开 ES8311 立体声播放，
  按指定通道数与 TDM 时隙打开 ES7210 采集，并对选定的物理麦克风输入设置增益。

读写适配接口分别调用 `esp_codec_dev_read()` 和 `esp_codec_dev_write()`。调用成功
时，会报告完整的请求字节数。当前编解码器 API 为同步接口，因此适配函数虽然为了
回调兼容而接收 `timeout_ms`，但不会在本层另外执行超时控制。

## 当前 BSP 引脚映射

下表数据来自当前解析的托管 BSP，`bsp_extra` 通过 BSP 宏使用这些值；应用中不要
再次把它们写成独立的数字常量。

| 信号 | GPIO |
| --- | ---: |
| I2S MCLK | 42 |
| I2S BCLK/SCLK | 9 |
| I2S LRCK/WS | 45 |
| ESP32-S3 输出到 ES8311 的数据 | 8 |
| ES7210 输入到 ESP32-S3 的数据 | 10 |
| 功率放大器使能 | 46 |

PA 使能为高电平有效，并由编解码器 GPIO 接口配置。
`bsp_extra_codec_pa_is_enabled()` 不会改变输出状态；它仅启用 GPIO 输入缓冲并读取
物理引脚电平，使日志和诊断能够报告 PA 是否真正处于使能状态。

## 独占音频所有权

ES8311、ES7210、I2S 控制器、时钟和 PA 共同组成一套共享会话。因此
`bsp_extra_audio_session_acquire()` 使用一个跨核安全的轻量所有者令牌，供以下
客户端使用：

- MusicPlayer
- VideoPlayer
- Recorder
- SpecAnalyzer
- Xiaozhi

获取操作为非阻塞且不可递归。只有当前所有者为 `NONE` 时才返回 `ESP_OK`；否则
返回 `ESP_ERR_INVALID_STATE`，调用者必须提示资源忙或稍后重试。只有当前所有者
能够释放会话，而且每次成功获取都必须恰好对应一次释放。无效、重复或跨所有者的
释放操作都会被拒绝。

所有者令牌保护的是完整编解码器生命周期，而不是一次读、写或播放请求。客户端
必须遵循以下顺序：

1. 获取自己的所有者令牌。
2. 初始化或重新配置编解码器，然后启动工作任务或文件操作。
3. 停止回调和工作任务，再关闭所有文件或依赖 SD 卡的资源。
4. 删除播放器或停止编解码器流，并确认清理成功。
5. 释放音频所有者令牌。

只要解码器、录音任务、网络回调或队列中的文件仍可能访问编解码器，就不能释放
所有权。如果清理返回错误，应继续持有所有权并完成或重试清理，而不能让第二个
应用开始使用硬件。

## 文件播放器生命周期

`bsp_extra_player_init()` 会初始化编解码器、恢复默认播放格式、在 core 0 上以
优先级 5 创建音频播放器工作任务，并注册转发回调。文件遍历只接受不区分大小写的
`.mp3` 和 `.wav` 文件名。

成功交给 `audio_player_play()` 后，`FILE` 的所有权转移给音频播放器，并由播放器
关闭。如果交接失败，调用者仍负责关闭文件。`bsp_extra_player_del()` 会先等待安全
覆盖版音频播放器完成删除，再清除本地状态，并按照先采集、后播放的顺序关闭流。
只有该函数成功后，调用者才能释放 SD 挂载租约或音频所有者。

## 验证

修改音频代码后，必须执行
[`../../HARDWARE_VALIDATION_ZH.md`](../../HARDWARE_VALIDATION_ZH.md) 中
MusicPlayer、Recorder、SpecAnalyzer、VideoPlayer 和 AIChats 之间的切换测试。
确认 PA 日志反映预期状态、ES7210 的两个麦克风通道均有数据，而且快速退出应用
不会遗留音频所有者或 SD 文件。
