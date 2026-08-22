# Candis-S31

[English](README.md)

Candis-S31 是一块围绕 ESP32-S31 和 2.0 英寸 460 × 460 方形 AMOLED 设计的开发板。板上还包含触摸、充电与电源管理、RTC、双 USB Type-C、音频、DVP 摄像头接口、TF 卡、按键和一颗 RGB LED。

> **硬件状态：** EVT1（原理图 v0.5，2026-08-03 投板 `v0.5_260803_1544`）已回板，点亮工作已大部分完成。实物已验证：AMOLED 显示（460×460 QSPI，TE 同步 LVGL 管线）、电容触摸、音频输出（扬声器）与输入（双麦克风）、microSD、USB Type-C2 Host 与 Device 角色、Wi-Fi、BLE、RTC、PMIC/电池充电域（含 17 个应用的手表形态 LVGL 综合演示）。相机等待修正版 FPC 转接板；长时老化与低功耗功耗表征未开始。
>
> **内测用户：** 见 [BETA.md](BETA.md)——装好 ESP-IDF 环境后克隆本仓库即可直接编译全部固件工程。

> **VDD_SPI 绑带：** GPIO36 是 VDD_SPI 电压绑带（数据手册 Table 3-4），兼作 TF 卡低有效电源使能。本板 Flash 为外置 W25Q128（3.3V），故 VDD_SPI=3.3V；R6（10kΩ 上拉至 3.3V）正确地在复位时拉高 GPIO36，符合 ESP32-S31 v0.0 勘误 SPI-855（1.8V VDD_SPI 无法启动）。无需处理 R6，切勿拆除。

## 开始使用

仓库根目录不是编译工程。请打开独立的 ESP-IDF 示例：

```bash
cd examples/esp-idf/getting-started
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p PORT flash monitor
```

当前基线是 ESP-IDF `v6.1-rc1`。使用 EIM、官方 VS Code 插件或命令行都打开同一个工程。构建前请阅读[示例说明](examples/esp-idf/getting-started/README.md)。

## 支持状态

| 开发环境 | 当前状态 |
|---|---|
| ESP-IDF | 入门工程、Factory、低功耗示例以及 BSP 与 Board Manager 定义均使用 `v6.1-rc1` 编译通过；核心外设已在 EVT1 实物上验证（见上方硬件状态） |
| Arduino | 等待 ESP32-S31 Core，再提交 Candis-S31 board 与 variant |
| PlatformIO | 等待 ESP32-S31 平台、工具和框架支持，再增加 board manifest |

Arduino 和 PlatformIO 只有在公开的标准工具能够正常构建后才会加入。单独的 variant 或 board JSON 不能带来一个新 SoC 的支持。本仓库不提供私有 ESP-IDF 分叉、修改版框架或复制的第三方库。

### 构建验证 — 2026-08-22

基线：ESP-IDF `v6.1-rc1`，目标芯片 `esp32s31`（preview）。`tools/build-all.sh` 串行编译下列 14 个目标；2026-08-22 完整回归 14/14 通过。`display_usb_hid` 示例不在矩阵内：它通过 `esp_lvgl_port` 助手驱动 HID 输入，而本板 BSP 使用 `esp_lvgl_adapter`。

| 目标（`tools/build-all.sh --list`） | 源码位置 | 状态（2026-08-22） |
|---|---|---|
| `example:display` | esp-bsp `examples/display` | 编译通过 |
| `example:display_camera_video` | esp-bsp `examples/display_camera_video` | 编译通过 |
| `example:display_lvgl_demos` | esp-bsp `examples/display_lvgl_demos` | 编译通过 |
| `example:display_lvgl_benchmark` | esp-bsp `examples/display_lvgl_benchmark` | 编译通过 |
| `example:display_sdcard` | esp-bsp `examples/display_sdcard` | 编译通过 |
| `example:audio` | esp-bsp `examples/audio` | 编译通过 |
| `example:display_audio_photo` | esp-bsp `examples/display_audio_photo` | 编译通过 |
| `testapp:tg28_sw` | esp-bsp `components/tg28_sw/test_apps` | 编译通过 |
| `testapp:rx8130ce` | esp-bsp `components/rx8130ce/test_apps` | 编译通过 |
| `testapp:fusb303b` | esp-bsp `components/fusb303b/test_apps` | 编译通过 |
| `testapp:cst820` | esp-bsp `components/lcd_touch/esp_lcd_touch_cst820/test_apps` | 编译通过 |
| `factory` | `firmware/factory` | 编译通过 |
| `getting-started` | `examples/esp-idf/getting-started` | 编译通过 |
| `low-power` | `examples/esp-idf/low-power` | 编译通过 |

全部固件目标基于仓内 [`vendor/esp-bsp/`](vendor/esp-bsp/README.md) 的内置 BSP 快照构建，普通克隆即可编译，无需额外出库或环境变量。

## 仓库目录

```text
.
├── hardware/                  # 原理图、引脚表和 EVT 注意事项
├── examples/
│   └── esp-idf/
│       ├── getting-started/   # 独立 ESP-IDF 工程
│       └── low-power/        # S0/S1/深睡/S2 状态机原型
├── firmware/
│   ├── factory/               # Factory Bring-up 源码和发布约定
│   └── recovery/              # 恢复流程
└── .github/workflows/         # 构建验证
```

仓库根目录有意不提供 `CMakeLists.txt`、`components/` 或框架安装文件。

## 硬件资料

- [硬件资料说明](hardware/README.md)
- [开发板原理图](hardware/schematic/SCH_Schematic_3_2026-08-10.pdf)
- [初版引脚表](hardware/pinout/README.md)
- [EVT1 上电注意事项](hardware/bring-up.md)
- [出厂固件](firmware/factory/README.md)
- [Recovery](firmware/recovery/README.md)
- [上游归属和贡献路径](UPSTREAM.md)

完成 EVT1 电源检查前，不要打开屏幕偏置、USB OTG 或其他受控电源轨。

## 板级适配

Candis-S31 按照代码职责分别提交到对应上游：

- Candis-S31 BSP 与可复用的 TG28_SW / RX8130CE / FUSB303B / CST820 驱动托管在公开的 [`LeenixP/esp-bsp`](https://github.com/LeenixP/esp-bsp) fork（`feat/candis-s31` 分支；各驱动组件的 `pr/*` 分支已按上游提交备好）——本仓库在 [`vendor/esp-bsp/`](vendor/esp-bsp/README.md) 内置了可直接编译的 BSP 快照，普通克隆即可编译全部固件；
- 完整 ESP-IDF 板级定义进入 ESP Board Manager 的 [`espressif/esp_friends_boards`](https://components.espressif.com/components/espressif/esp_friends_boards)；
- 可复用器件驱动进入各自源码仓库并发布到 [ESP Component Registry](https://components.espressif.com/)；
- Arduino-ESP32 具备 ESP32-S31 Core 后，再向 [Arduino-ESP32](https://github.com/espressif/arduino-esp32) 提交 board 与 variant；
- PlatformIO Espressif32 具备 ESP32-S31 平台支持后，再向 [platform-espressif32](https://github.com/platformio/platform-espressif32) 提交 board manifest。

ESP32-S31 的通用问题才需要修改 ESP-IDF 本身。Candis-S31 的引脚分配和器件选择通常不需要进入 ESP-IDF 核心仓库。BSP 遵循 ESP-BSP 公共 API，让 Factory 固件和上游示例验证同一份实现。驱动组件经维护方讨论后提交 ESP-BSP 上游；Board Manager 定义单独维护。

BSP 开发时把 `CANDIS_S31_BSP_PATH` 指向活的 esp-bsp 工作区即可覆盖内置快照；维护者用 `tools/sync_bsp.sh` 刷新快照。逐仓库的提交范围见[上游归属和贡献路径](UPSTREAM.md)。

BSP 已覆盖显示与触摸、TG28_SW 电源管理、RX8130CE RTC、FUSB303B 与 USB Host、SDMMC、ES8389 音频、DVP 摄像头链路和 RGB LED。Board Manager 定义已使用开发组件完成生成和编译，但刻意不暴露 Type-C 控制器和 OTG GPIO：其当前模型无法原子保证"先 Source、后升压"和仅 500 mA 的板级约束；Type-C2 USB Host 必须使用 BSP API。

## 许可

本仓库源码默认采用 Apache-2.0，单独标注的文件除外。硬件文档继续受文件内已有声明约束。
