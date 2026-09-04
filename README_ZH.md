# Candis-S31

[English](README.md)

Candis-S31 是一块围绕 ESP32-S31 和 2.0 英寸 460 × 460 方形 AMOLED 设计的开发板。板上还包含触摸、充电与电源管理、RTC、双 USB Type-C、音频、DVP 摄像头接口、TF 卡、按键和一颗 RGB LED。

> **硬件状态：** EVT1（原理图 v0.5，2026-08-03 投板 `v0.5_260803_1544`）已回板，点亮工作已大部分完成。实物已验证：AMOLED 显示（460×460 QSPI，TE 同步 LVGL 管线）、电容触摸、microSD、USB Type-C2 Host 与 Device 角色、Wi-Fi、BLE、RTC、PMIC/电池充电域（含 16 个应用的手表形态 LVGL 综合演示）。ES8389 编解码器可初始化，已有数字音频链路检查通过；当前 EVT1 的扬声器听感与麦克风录音验收仍待复核。相机 FPC 转接板已到位，传感器识别、DVP 取流和内建彩条路径已在 EVT1 实板验证；真实场景成像质量与 JPEG 拍照落卡仍需硬件确认。长时老化与低功耗功耗表征未开始。
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
| ESP-IDF | 入门工程、Board Manager 屏幕示例、Factory、低功耗示例、播放器和仓内板级组件均使用 `v6.1-rc1` 编译通过；核心外设已在 EVT1 实物上验证（见上方硬件状态） |
| Arduino | 等待 ESP32-S31 Core，再提交 Candis-S31 board 与 variant |
| PlatformIO | 等待 ESP32-S31 平台、工具和框架支持，再增加 board manifest |

Arduino 和 PlatformIO 只有在公开的标准工具能够正常构建后才会加入。单独的 variant 或 board JSON 不能带来一个新 SoC 的支持。本仓库不提供私有 ESP-IDF 分叉或修改版框架；仓内组件镜像均在 `components/`、`vendor/` 和 [UPSTREAM.md](UPSTREAM.md) 中明确标注来源与归属。

### 构建验证 — 2026-09-04

基线：ESP-IDF `v6.1-rc1`，目标芯片 `esp32s31`（preview）。`tools/build-all.sh` 串行编译下列 14 个默认目标；矩阵设计为普通克隆即可运行。构建日志属于本地产物，按设计不随仓库发布；可直接运行 `tools/build-all.sh` 重现并查看末尾汇总。`display-hello` 目标额外验证仓内 Board Manager 生成文件和相对路径覆盖。

| 目标（`tools/build-all.sh --list`） | 源码位置 | 状态 |
|---|---|---|
| `factory` | `firmware/factory` | 编译通过 |
| `getting-started` | `examples/esp-idf/getting-started` | 编译通过 |
| `low-power` | `examples/esp-idf/low-power` | 编译通过 |
| `player` | `examples/esp-idf/player` | 编译通过 |
| `display-hello` | `examples/esp-idf/display-hello` | 编译通过；Board Manager 本地路径 |
| `demo` | `firmware/demo` | 编译通过 |
| `camera-test` | `firmware/camera_test` | 编译通过 |
| `powercycle` | `firmware/powercycle` | 编译通过 |
| `usb-cdc-device` | `firmware/usb_cdc_device` | 编译通过 |
| `testapp:candis_s31` | `components/candis_s31/test_apps` | 编译通过 |
| `testapp:tg28_sw` | `vendor/idf-extra-components/tg28_sw/test_apps` | 编译通过 |
| `testapp:rx8130ce` | `vendor/idf-extra-components/rx8130ce/test_apps` | 编译通过 |
| `testapp:fusb303b` | `vendor/idf-extra-components/fusb303b/test_apps` | 编译通过 |
| `testapp:cst820` | `vendor/idf-extra-components/esp_lcd_touch_cst820/test_apps` | 编译通过 |

板级运行时维护在 [`components/candis_s31/`](components/candis_s31)，可复用驱动映射到 [`vendor/idf-extra-components/`](vendor/idf-extra-components)，声明式 Board Manager 定义位于 [`vendor/esp-board-manager/`](vendor/esp-board-manager)。不需要外部 BSP 检出。

## 仓库目录

```text
.
├── hardware/                  # 原理图、引脚表和 EVT 注意事项
├── docs/                      # 系统总览和板卡使用说明
├── cmake/                     # 独立工程共用接线
├── components/
│   ├── candis_s31/            # 仓库自有板级运行时组件
│   └── esp_lvgl_port/         # 板级 LVGL 兼容端口
├── examples/
│   └── esp-idf/
│       ├── getting-started/   # 工具链最小验证
│       ├── display-hello/     # Board Manager + LVGL 屏幕示例
│       ├── low-power/         # S0/S1/深睡/S2 状态机原型
│       └── player/            # TF 卡音视频播放器（基于 ESP-GMF）
├── firmware/
│   ├── camera_test/           # 相机/DVP 自动诊断与带屏实时预览
│   ├── demo/                  # 手表形态综合 LVGL 演示
│   ├── factory/               # Factory Bring-up 源码和发布约定
│   ├── powercycle/            # 功耗测量辅助固件
│   ├── recovery/              # 恢复流程
│   └── usb_cdc_device/        # Type-C2 USB CDC 设备诊断
├── tools/                     # 构建验证和发布脚本
├── vendor/
│   ├── esp-board-manager/     # Board Manager 与 friends-board 快照
│   └── idf-extra-components/  # 可复用驱动快照
└── .github/workflows/         # 构建验证
```

仓库根目录没有顶层应用 `CMakeLists.txt`；每个 ESP-IDF 工程都从自身目录构建。

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

本仓库按乐鑫维护方在 [esp-bsp#823](https://github.com/espressif/esp-bsp/pull/823) 给出的归属建议组织：

- Candis-S31 的完整运行时组件维护在 [`components/candis_s31/`](components/candis_s31)，包括引脚、电源时序、Type-C 策略、相机时钟规避和 AMOLED/LVGL 管线；不依赖 esp-bsp 检出。
- TG28_SW、RX8130CE、FUSB303B、CST820 四个可复用驱动暂存于 [`vendor/idf-extra-components/`](vendor/idf-extra-components)，待后续手动提交 `espressif/idf-extra-components`。
- 声明式板卡定义暂存于 [`vendor/esp-board-manager/esp_friends_boards/`](vendor/esp-board-manager/esp_friends_boards)，待后续手动提交 `esp_friends_boards`；[`display-hello`](examples/esp-idf/display-hello) 展示其本地 Board Manager 接入方式。

Board Manager 能描述设备和外设接线，但不承载本板完整的共享中断、充电、Type-C、相机时钟和显示事务策略；高级固件使用仓库自有运行时组件，小型屏幕示例直接使用 Board Manager。

## 许可

本仓库源码默认采用 Apache-2.0，单独标注的文件除外。硬件文档继续受文件内已有声明约束。
