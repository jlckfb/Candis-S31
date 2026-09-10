# 快速开始

本页面向拿到 Candis-S31 开发板的使用者：安装官方 ESP-IDF 后，克隆本仓库即可编译全部工程。
板级运行时位于 `components/candis_s31/`，可复用驱动与 Board Manager 资料位于
`vendor/idf-extra-components/`、`vendor/esp-board-manager/`，不依赖外部 BSP 检出，也不需要额外环境变量。

## 1. 这块板子是什么

Candis-S31 围绕 ESP32-S31 与 2.0 英寸 460 × 460 方形 AMOLED 设计。板上资源：
CO5300 AMOLED（QSPI）+ CST820 触摸、TG28 电源管理/充电/电量计、RX8130CE RTC、
双 USB Type-C（C1 供电+调试、C2 OTG）、ES8389 音频（扬声器 + 双麦克风）、DVP 摄像头接口、
microSD、三枚按键与一颗 WS2812 RGB 灯。完整规格见仓库根目录 [README.md](README.md)。

## 2. 环境准备

安装 **官方 ESP-IDF `v6.1-rc1`** 或更新版本：

```bash
git clone -b v6.1-rc1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s31
. ./export.sh
```

也可以使用 Espressif Installation Manager（EIM）或官方 VS Code 扩展选择同一版本。
ESP32-S31 目前是 **preview target**，因此下文的 `idf.py` 命令都带 `--preview`。

## 3. 构建

克隆仓库后，先在仓库根目录跑一次完整构建矩阵：

```bash
git clone https://github.com/jlckfb/Candis-S31.git
cd Candis-S31
./tools/build-all.sh
```

也可以只编译其中任一工程：

```bash
cd examples/esp-idf/camera-test
idf.py --preview -D IDF_TARGET=esp32s31 build
```

首次编译会从 ESP Component Registry 下载公开依赖（LVGL、esp_lvgl_adapter、esp_video、
esp_codec_dev 等），各工程的 `dependencies.lock` 固定已解析的版本与哈希。仓内板级代码和四个
未发布驱动通过相对 `override_path` 固定到本仓路径，因此无需额外配置。

已有本地实验配置时，用 `./tools/build-all.sh --isolated` 按仓库默认值验证：

```bash
./tools/build-all.sh --isolated                              # 全部目标
./tools/build-all.sh --isolated camera-test display-benchmark # 指定目标
```

隔离运行会在日志目录下为每个目标生成独立的 `sdkconfig` 与 `build/`，不复制、不删除、不覆盖
工程原有配置；它只隔离配置，不等同于干净克隆或重新下载全部依赖。普通模式仍是增量构建，
若发现其他芯片的旧配置会提示改用隔离模式。

`display-hello` 的 Board Manager 生成文件已提交在工程的 `components/gen_bmgr_codes/`，
普通使用者无需安装额外 Python 工具；只有修改 `vendor/esp-board-manager/esp_friends_boards/candis_s31/`
的维护者才需要加载本地 `idf_ext.py` 并重新运行 `idf.py bmgr -b candis_s31`。

## 4. 烧录与串口

- **只用 C1 口**（CH343P USB 转串口）：

  ```bash
  idf.py --preview -p /dev/ttyACM0 -b 4000000 flash
  idf.py --preview -p /dev/ttyACM0 monitor
  ```

  4,000,000 baud 是当前实测最高可靠下载速率；线材或主机不稳定时降速即可。
- **C2 口不能烧录**：ESP32-S31 的 ROM 下载模式走 UART，C2 是原生 USB 数据口（Host/Device）。
- Windows 下串口名为对应的 COM 口，Linux 通常为 `/dev/ttyACM0`。

## 5. 示例工程

| 示例 | 用途 |
|---|---|
| `examples/esp-idf/getting-started` | 只验证 SoC、Flash 与串口，不初始化板级外设 |
| `examples/esp-idf/display-hello` | 使用仓内 Board Manager 生成配置与官方 LVGL adapter 的最小屏幕示例 |
| `examples/esp-idf/display-touch` | AMOLED 十字标记跟随手指并打印坐标 |
| `examples/esp-idf/display-benchmark` | 全屏/局部刷新帧率（验收线：全屏 ≥ 30、局部 ≥ 60） |
| `examples/esp-idf/camera-test` | OV5640 DVP 采集与 460 × 460 AMOLED 实时预览 |
| `examples/esp-idf/player` | TF 卡音视频播放器 |
| `examples/esp-idf/audio-recorder` | 麦克风录 5 秒到 `/sdcard/example_record.wav` |
| `examples/esp-idf/audio-player` | 播放 1 kHz 程序生成正弦音，再播放录音文件 |
| `examples/esp-idf/storage` | TF 卡挂载、容量、目录与文件写读删校验 |
| `examples/esp-idf/usb-host-msc` | Type-C2 Host 挂载 U 盘并列出根目录 |
| `examples/esp-idf/usb-cdc-device` | Type-C2 原生 USB CDC 设备控制台 |
| `examples/esp-idf/led` | WS2812B RGB 常亮/呼吸/闪烁循环 |
| `examples/esp-idf/buttons` | BOOT / PWR / RTC 闹钟按键事件串口打印 |
| `examples/esp-idf/rtc` | RX8130CE 读写与分钟闹钟（共享中断线上报） |
| `examples/esp-idf/pmic` | TG28 轨道转储与 AUDIO_PA 一次 off→on→恢复 |
| `examples/esp-idf/power-cycle` | 电源状态循环与电池域操作 |
| `examples/esp-idf/low-power` | 浅睡眠、深睡眠与息屏状态机 |
| `examples/esp-idf/wifi` | 扫描打印 AP；在 Kconfig 配置 SSID 后连接并打印 IP |
| `examples/esp-idf/ble` | NimBLE 以 `candis-example` 广播后再扫描 |
| `firmware/factory` | 产测控制台固件 |

每个工程都在自己的目录中独立构建，仓库根目录没有应用级 `CMakeLists.txt`。

## 6. 硬件说明

- **显示方向**：出厂物理方向为 180°，BSP 已按此设置 MADCTL 与触摸坐标变换。
- **显示刷新**：面板 TE 周期约 59.9 Hz，因此 TE 同步下的刷新吞吐为全屏约 30 fps、局部约 60 fps；
  该数值由面板时序决定，统计口径是 LVGL 刷新吞吐。
- **电源输出**：USB Type-C 2 作为电源输出时限 5 V / 500 mA；EXT 接口的受控 3.3 V 限 300 mA。
- **扬声器**：接口为差分输出，任一端均不可接地。
- **摄像头**：模组必须与 24 Pin FPC 引脚定义匹配。色彩由传感器内部自动白平衡处理，不同照明下白点
  随光源变化；镜头边缘存在自然的色度衰减。
- **电量计**：默认使用 TG28 内置 ROM 电池模型，SOC 为估算值；需要精确 SOC 时按实际电芯写入模型。
- **RTC**：板上没有独立的纽扣电池或超级电容，时间由主板电池域维持，电池与 C1 同时移除会丢失时间。

## 7. 开发者向

- 仓库自有板级组件：`components/candis_s31/`；统一注入入口：`cmake/candis_components.cmake`。
- 可复用驱动：`vendor/idf-extra-components/`；Board Manager 与 `candis_s31` 定义：
  `vendor/esp-board-manager/`。
- `tools/sync_upstream.sh` 用于刷新两个 vendor 快照，来源提交记录在各自的 `SOURCE_COMMIT`。
- 上游归属与提交策略见 [UPSTREAM.md](UPSTREAM.md)。官方 `esp-bsp` 只维护 Espressif/M5Stack 开发板，
  本仓库不依赖其检出。

## 8. 反馈

提交问题时请附带：完整串口日志、复现步骤、当前固件版本（启动日志可见）。硬件异常请先按第 6 节
取证，再提供测量结果。
