# Candis-S31

[English](README.md)

Candis-S31 是一块围绕 ESP32-S31 和 2.0 英寸 460 × 460 方形 AMOLED 设计的开发板。板上还包含触摸、充电与电源管理、RTC、双 USB Type-C、音频、DVP 摄像头接口、TF 卡、按键和一颗 RGB LED。

> **硬件状态：** EVT1 仍在 Layout，尚未投板。ESP-IDF 入门工程和 Factory Bring-up 工程已经通过编译，但开发板功能还没有经过实物验证。

## 开始使用

仓库根目录不是编译工程。请打开独立的 ESP-IDF 示例：

```bash
cd examples/esp-idf/getting-started
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p PORT flash monitor
```

当前基线是 ESP-IDF `v6.1-beta1`。使用 EIM、官方 VS Code 插件或命令行都打开同一个工程。构建前请阅读[示例说明](examples/esp-idf/getting-started/README.md)。

## 支持状态

| 开发环境 | 当前状态 |
|---|---|
| ESP-IDF | 入门工程、Factory、本地完整 BSP 和本地 Board Manager 定义均使用 `v6.1-beta1` 编译通过；仍需硬件验证和上游发布 |
| Arduino | 等待 ESP32-S31 Core，再提交 Candis-S31 board 与 variant |
| PlatformIO | 等待 ESP32-S31 平台、工具和框架支持，再增加 board manifest |

Arduino 和 PlatformIO 只有在公开的标准工具能够正常构建后才会加入。单独的 variant 或 board JSON 不能带来一个新 SoC 的支持。本仓库不提供私有 ESP-IDF 分叉、修改版框架或复制的第三方库。

## 仓库目录

```text
.
├── hardware/                  # 原理图、引脚表和 EVT 注意事项
├── examples/
│   └── esp-idf/
│       └── getting-started/   # 独立 ESP-IDF 工程
├── firmware/
│   ├── factory/               # Factory Bring-up 源码和发布约定
│   └── recovery/              # 恢复流程
└── .github/workflows/         # 构建验证
```

仓库根目录有意不提供 `CMakeLists.txt`、`components/` 或框架安装文件。

## 硬件资料

- [硬件资料说明](hardware/README.md)
- [开发板原理图](hardware/schematic/Easy-S31_SCH_v0.5_2026-07-28_0924.pdf)
- [初版引脚表](hardware/pinout/README.md)
- [EVT1 上电注意事项](hardware/bring-up.md)
- [出厂固件](firmware/factory/README.md)
- [Recovery](firmware/recovery/README.md)
- [上游归属和贡献路径](UPSTREAM.md)

完成 EVT1 电源检查前，不要打开屏幕偏置、USB OTG 或其他受控电源轨。

## 板级适配

Candis-S31 按照代码职责分别提交到对应上游：

- 在独立的 `esp-bsp` 工作区开发 Candis-S31 BSP，先由 Factory 工程验证，再决定上游提交；
- 完整 ESP-IDF 板级定义进入 ESP Board Manager 的 [`espressif/esp_friends_boards`](https://components.espressif.com/components/espressif/esp_friends_boards)；
- 可复用器件驱动进入各自源码仓库并发布到 [ESP Component Registry](https://components.espressif.com/)；
- Arduino-ESP32 具备 ESP32-S31 Core 后，再向 [Arduino-ESP32](https://github.com/espressif/arduino-esp32) 提交 board 与 variant；
- PlatformIO Espressif32 具备 ESP32-S31 平台支持后，再向 [platform-espressif32](https://github.com/platformio/platform-espressif32) 提交 board manifest。

ESP32-S31 的通用问题才需要修改 ESP-IDF 本身。Candis-S31 的引脚分配和器件选择通常不需要进入 ESP-IDF 核心仓库。本地 BSP 遵循 ESP-BSP 公共 API，让 Factory 固件和上游示例验证同一份实现。完整 BSP 只有在 EVT 实测并与维护方确认接收范围后才提交；Board Manager 定义仍然单独维护。

BSP 源码不会复制回本仓库。开发阶段 Factory 通过 `CANDIS_S31_BSP_PATH` 加载独立工作区，公开示例只依赖正式发布的能力。逐仓库的提交范围见[上游归属和贡献路径](UPSTREAM.md)。

本地 BSP 已覆盖显示与触摸、TG28_SW 电源管理、RX8130CE RTC、FUSB303B 与 USB Host、SDMMC、ES8389 音频、DVP 摄像头链路和 RGB LED。对应的 Board Manager 定义也已使用这些开发组件完成生成和编译。这只是软件实现完成，不代表硬件验证完成；在实板测量前，EVT 结果仍统一记录为 `NOT_RUN`。

## 许可

本仓库源码默认采用 Apache-2.0，单独标注的文件除外。硬件文档继续受文件内已有声明约束。
