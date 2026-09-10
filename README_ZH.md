# Candis-S31

[English](README.md)

立创·Candis-S31 面向新一代 ESP32-S31 开放的整板参考设计。围绕 **2.0 英寸 460 × 460 方形 AMOLED**，
集成触摸、音频、DVP 摄像头接口、TF 卡、双 USB Type-C 与电源管理，为 ESP32-S31 产品开发提供可参考、
可复用的硬件与固件方案。

![立创·Candis-S31 开发板](img/board-hero.png)

**2.0" AMOLED（460 × 460）· 双核 RISC-V · 16 MB / 32 MB PSRAM**

## 硬件概览

板载完整的交互硬件：AMOLED 显示与触摸、音频输入输出、DVP 摄像头接口、双 USB Type-C、TF 卡槽、
电源管理与独立 RTC。

![Candis-S31 开发板正反面器件标注图](img/board-overview.png)

| | 模块 | 说明 |
|---|---|---|
| 01 | **ESP32-S31** | 双核 RISC-V，面向图形、多媒体与无线交互 |
| 02 | **CO5300 AMOLED** | 2.0 英寸方形屏幕，460 × 460，QSPI 接口 |
| 03 | **CST820** | 板载电容触摸控制器，构成完整屏幕交互 |
| 04 | **TG28 PMIC** | 多路电源轨、电池充电与 RTC 供电 |
| 05 | **ES8389** | 音频编解码、双模拟麦克风与扬声器功放 |
| 06 | **双 Type-C** | 独立调试接口与原生 USB OTG 接口 |

## ESP32-S31 平台

主控为乐鑫 ESP32-S31，集成多协议连接与先进 HMI 能力。

![Espressif ESP32-S31](img/esp32-s31.png)

| | |
|---|---|
| **320 MHz** | 双核高性能 32 位 RISC-V 主处理器 |
| **16 MB / 32 MB** | 封装内 Octal PSRAM，为图形资源提供空间 |
| **16 MB** | 板载 W25Q128 QSPI Flash |
| **PPA** | 搭配 JPEG Codec 与 2D DMA 图形硬件能力 |
| **Wi-Fi 6** | 2.4 GHz 无线局域网连接 |
| **多协议连接** | Bluetooth 5.4 / IEEE 802.15.4 |

## 连接、电源与扩展

| | 接口 | 说明 |
|---|---|---|
| C1 | 调试 Type-C | CH343P USB 转串口，承担下载、串口调试与供电入口 |
| C2 | USB OTG | ESP32-S31 原生 USB，配合 FUSB303B 角色控制与 ISL9113 5 V 升压 |
| PWR | TG28 PMIC | 多路 DCDC/LDO、电池充电、开关机控制与 RTC 常供电 |
| RTC | RX8130CE | 内置晶体的独立实时时钟 |
| I²C | EXT 扩展口 | GH1.25-4 引出 GND、受控 3.3 V 与独立 I²C 总线 |
| RGB | 按键与灯光 | PWRON、BOOT、RESET 三枚按键与一颗 WS2812 RGB LED |

## 硬件规格

| 项目 | 参数 |
|---|---|
| 主处理器 | ESP32-S31 双核 32 位 RISC-V，最高 320 MHz |
| 低功耗协处理器 | 单核 32 位 RISC-V，最高 40 MHz |
| 片上存储 | 512 KB SRAM |
| PSRAM | 16 MB / 32 MB 封装内 Octal PSRAM |
| Flash | 16 MB W25Q128 QSPI Flash，3.3 V |
| 无线连接 | 2.4 GHz Wi-Fi 6、Bluetooth 5.4、IEEE 802.15.4 |
| 显示 | 2.0 英寸 460 × 460 AMOLED，CO5300 QSPI 控制器 |
| 触摸 | CST820 电容触摸控制器 |
| 音频 | ES8389 Codec、双模拟麦克风、NS4150B 差分扬声器功放 |
| 摄像头 | 24 Pin FPC，8 位 DVP 数据接口 |
| 存储扩展 | TF / microSD 卡槽，SDMMC 总线，独立受控电源 |
| USB Type-C 1 | CH343P USB 转串口：下载、日志与供电 |
| USB Type-C 2 | 原生 USB OTG，FUSB303B Type-C 控制，ISL9113 5 V 升压 |
| 电源管理 | TG28 PMIC：多路 DCDC/LDO、电池充电、软开关机与 VRTC |
| 实时时钟 | RX8130CE，内置 32.768 kHz 晶体 |
| 扩展接口 | GH1.25-4：GND、受控 3.3 V、I²C SDA、I²C SCL |
| 其他资源 | PWRON / BOOT / RESET 按键、WS2812B-1313-V6 RGB LED |
| 硬件版本 | v0.5-Beta1 / EVT1 |

> **说明**
> 开发板不含电池和外壳。USB Type-C 2 作为电源输出时限 5 V / 500 mA；EXT 接口的受控 3.3 V 限 300 mA。
> 摄像头模组必须与 24 Pin FPC 引脚定义匹配。扬声器接口为差分输出，任一端均不可接地。
>
> 本板外置 W25Q128 为 3.3 V 器件，因此 VDD_SPI 取 3.3 V，R6 在复位期间将 GPIO36 保持为高；
> R6 必须保留。

## 展示

![Candis-S31 与电池](img/gallery-1.jpg)

![Candis-S31 AMOLED 显示](img/gallery-2.jpg)

![Candis-S31 板卡细节](img/gallery-3.jpg)

## 快速开始

仓库根目录不是构建工程，每个示例都是独立的 ESP-IDF 工程：

```bash
cd examples/esp-idf/getting-started
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p PORT flash monitor
```

当前基线为 ESP-IDF `v6.1-rc1`。Espressif Installation Manager（EIM）、官方 VS Code 扩展与命令行
使用同一个工程；构建前请先阅读[示例说明](examples/esp-idf/getting-started/README.md)。

`tools/build-all.sh` 串行编译完整矩阵，是克隆后推荐的第一次检查：

```bash
tools/build-all.sh --list      # 列出目标及其源码目录
tools/build-all.sh             # 构建全部目标
```

## 示例

| 示例 | 用途 |
|---|---|
| [`getting-started`](examples/esp-idf/getting-started) | 工具链与目标芯片验证 |
| [`display-hello`](examples/esp-idf/display-hello) | 通过声明式 Board Manager 路径点亮 AMOLED |
| [`display-touch`](examples/esp-idf/display-touch) | AMOLED 十字标记与实时触摸坐标 |
| [`display-benchmark`](examples/esp-idf/display-benchmark) | 全屏与局部刷新帧率 |
| [`camera-test`](examples/esp-idf/camera-test) | OV5640 DVP 采集与 460 × 460 AMOLED 实时预览 |
| [`player`](examples/esp-idf/player) | 基于 ESP-GMF 的 TF 卡音视频播放器 |
| [`audio-recorder`](examples/esp-idf/audio-recorder) | 麦克风采集并写入 TF 卡 WAV 文件 |
| [`audio-player`](examples/esp-idf/audio-player) | 正弦音与 WAV 播放 |
| [`storage`](examples/esp-idf/storage) | TF 卡挂载与文件读写校验 |
| [`usb-host-msc`](examples/esp-idf/usb-host-msc) | Type-C2 主机模式挂载 USB 存储设备 |
| [`usb-cdc-device`](examples/esp-idf/usb-cdc-device) | Type-C2 原生 USB CDC 设备控制台 |
| [`led`](examples/esp-idf/led) | WS2812B RGB LED 灯效 |
| [`buttons`](examples/esp-idf/buttons) | BOOT 与 PWR 按键事件 |
| [`rtc`](examples/esp-idf/rtc) | RX8130CE 走时与闹钟 |
| [`pmic`](examples/esp-idf/pmic) | TG28 轨道打印与功放控制 |
| [`power-cycle`](examples/esp-idf/power-cycle) | 电源状态与电池域操作 |
| [`low-power`](examples/esp-idf/low-power) | 浅睡眠、深睡眠与息屏状态机 |
| [`wifi`](examples/esp-idf/wifi) | Wi-Fi STA 扫描与连接 |
| [`ble`](examples/esp-idf/ble) | NimBLE 广播与扫描 |
| [`factory`](firmware/factory) | 产测与整机功能验证控制台 |

## 仓库结构

```text
.
├── hardware/                  # 原理图、引脚与硬件说明
├── docs/                      # 系统概述
├── cmake/                     # 独立工程的公共接线
├── components/
│   ├── candis_s31/            # 板级运行时组件（引脚、电源、显示、音频、摄像头）
│   └── esp_lvgl_port/         # 板级 LVGL 适配
├── examples/esp-idf/          # 独立示例工程
├── firmware/
│   ├── factory/               # 产测控制台与发布约定
│   └── recovery/              # 恢复流程
├── tools/                     # 构建矩阵与发布脚本
├── vendor/
│   ├── esp-board-manager/     # Board Manager 与 friends-board 快照
│   └── idf-extra-components/  # 可复用驱动快照
└── .github/workflows/         # 构建验证
```

每个独立工程都在自己的目录中构建；仓库根目录刻意不放应用级 `CMakeLists.txt`。

## 文档

- [系统概述](docs/system-overview.md)
- [硬件概览](hardware/README.md)
- [原理图](hardware/schematic/SCH_Schematic_3_2026-08-10.pdf)
- [引脚定义](hardware/pinout/README.md)
- [板级运行时组件](components/candis_s31/README.md) 及其 [API 参考](components/candis_s31/API.md)
- [产测固件](firmware/factory/README.md)
- [恢复流程](firmware/recovery/README.md)
- [上游归属与贡献映射](UPSTREAM.md)

## 工具链

本仓库使用 ESP-IDF `v6.1-rc1` 的 preview target `esp32s31`。板级运行时组件、声明式 Board Manager 定义
与全部示例都可以在克隆后直接构建，无需额外环境变量。环境安装与烧录步骤见[快速开始](QUICKSTART.md)。

## 板级支持

- 板级运行时组件位于 [`components/candis_s31/`](components/candis_s31)，负责引脚定义、上电时序、
  Type-C 策略、摄像头时钟与 AMOLED/LVGL 通路，无需检出 ESP-BSP。
- 可复用的 TG28_SW、RX8130CE、FUSB303B、CST820 驱动镜像在
  [`vendor/idf-extra-components/`](vendor/idf-extra-components)，声明式开发板描述位于
  [`vendor/esp-board-manager/esp_friends_boards/`](vendor/esp-board-manager/esp_friends_boards)；
  [`display-hello`](examples/esp-idf/display-hello) 直接使用该 Board Manager 定义。
- Board Manager 描述器件与外围接线；本板的共享中断、充电、Type-C、摄像头时钟与显示切换策略由运行时
  组件承载。因此完整固件使用该组件，display 示例直接使用 Board Manager 通路。
- 驱动归属与上游贡献映射记录在 [UPSTREAM.md](UPSTREAM.md)。

## 许可

除文件内另有声明外，源码采用 Apache-2.0 许可。硬件文档仍受随附文件中的声明约束。
