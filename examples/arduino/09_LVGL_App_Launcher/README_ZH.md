# 09_LVGL_App_Launcher

[English](README.md)

参考 ESP-Brookesia 手机系统 `AppLauncher` 的手表风格启动器，基于 LVGL 9 与 Arduino。

- 第 1 页：表盘（RTC/NTP 时间、电池、Wi-Fi、位置）
- 第 2 页：应用 —— AI 语音（启动 voice0/voice1 中的 xiaozhi-esp32 固件）与设置
- 设置：显示、声音、时区与位置、电池、Wi-Fi、软件更新（OTA）
- 空闲调暗：30 秒无触摸后屏幕降至约 10% 亮度，设置中的休眠时间从此时开始计算；触摸、PWR 或抬腕（QMI8658 加速度计）恢复亮度，抬腕也会唤醒熄屏并回到表盘

## 模块结构

每个模块位于 `src/` 下的独立文件夹，版本号定义在 `<module>_version.h`。

| 模块 | 文件夹 | 职责 | 数据分区 |
| --- | --- | --- | --- |
| Core | `src/core` | 启动、LVGL 移植、设置（NVS）、电源策略、OTA 引擎、模块注册表 | `nvs` |
| Board | `src/board` | CO5300 屏幕、FT3168 触摸、AXP2101 电源、PCF85063 RTC、QMI8658 IMU | `board` |
| Audio | `src/audio` | ES8311 编解码器与 I2S | `audio` |
| Protocol | `src/protocol` | Wi-Fi、NTP、HTTPS 传输 | `protocol` |
| Server | `src/server` | OTA 服务器地址与清单格式 | `server` |
| Apps | `src/apps` | AI 语音与设置应用 | `apps` |
| Components | `src/components` | 公共 UI 主题、时区表 | `components` |
| Launcher | `src/launcher` | 页面、表盘、应用生命周期 | `launcher` |

## 分区表

本文件夹中的 `partitions.csv` 会替代开发板菜单中的分区方案。

| 名称 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| nvs | 0x9000 | 20 KB | 设置、Wi-Fi、服务器地址、已安装数据版本（与 AI 语音共用） |
| otadata | 0xE000 | 8 KB | 当前应用槽与回滚状态 |
| app0 / app1 | 0x10000 / 0x250000 | 各 2.25 MB | 启动器 A/B 固件槽 |
| voice0 / voice1 | 0x490000 / 0x7D0000 | 各 3.25 MB | AI 语音（xiaozhi）A/B 固件槽 |
| assets | 0xB10000 | 3 MB | AI 语音字体、唤醒词与表情 |
| board、protocol、server | 0xE10000、0xE20000、0xE30000 | 各 64 KB | 模块数据 |
| audio、components | 0xE40000、0xF00000 | 各 256 KB | 模块数据 |
| apps、launcher | 0xE80000、0xF40000 | 各 512 KB | 模块数据 |
| storage | 0xFC0000 | 192 KB | 用户文件 FAT |
| coredump | 0xFF0000 | 64 KB | 崩溃转储 |

AI 语音应用会把启动槽切换到 voice0/voice1 并重启；在 AI 语音中短按 PWR 返回启动器（长按 BOOT 打开其局域网更新页面）。启动器的 OTA 与回滚只使用 app0/app1。

所有代码模块链接在同一个应用镜像中，因此固件 OTA 会一起替换它们。
清单中的模块版本号用于显示哪些模块发生了变化。模块数据分区可以单独更新，无需更新固件。

分区表无法通过 OTA 修改。首次使用此分区表时请通过 USB 烧录。

## OTA 更新

1. 编译程序并生成清单：

   ```bash
   python3 tools/make_ota_manifest.py --base-url https://example.com/fw/ --firmware build/09_LVGL_App_Launcher.ino.bin --out build/manifest.json
   ```

   如需发布模块数据镜像，可添加 `--data launcher=launcher.bin:2`（可重复）。
2. 将 `manifest.json` 及其引用的文件上传到 HTTPS 服务器，例如 GitHub Releases。
3. 在设备上打开 设置 > 软件更新 > Edit server，输入清单地址，然后点击 Check for updates 与 Install。

安全检查：

- 仅接受 `https://` 地址（包括重定向），并使用 ESP-IDF CA 证书包验证证书。
- 清单必须与本开发板 ID 及格式版本一致。
- 每个镜像在启用前都会校验 SHA-256 与大小。
- 新固件在运行 15 秒前处于待验证状态；若提前崩溃或复位，引导程序会回滚到上一个槽。
- 清单本身未签名。请保护好 HTTPS 服务器，或使用 Secure Boot v2 签名镜像以获得更强保障。

Wi-Fi 密码以未加密方式保存在 NVS 中。
