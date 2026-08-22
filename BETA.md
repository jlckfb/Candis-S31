# Candis-S31 内测指南

> 面向内测用户：拿到本仓库后，只需要装好 ESP-IDF 环境，即可直接编译全部固件工程。所有板级支持代码（BSP 与器件驱动）已完整内置于 `vendor/esp-bsp/`，**不依赖任何未发布的外部代码、不需要额外环境变量**。

## 1. 这块板子是什么

Candis-S31 是围绕 ESP32-S31 与 2.0 英寸 460×460 方形 AMOLED 设计的低功耗核心板。板上资源：CO5300 AMOLED（QSPI）+ CST820 触摸、TG28 电源管理/充电/电量计、RX8130CE RTC、双 USB Type-C（C1 供电+调试、C2 OTG）、ES8389 音频（扬声器 + 双麦克风）、DVP 摄像头接口、microSD、按键、单颗 WS2812 RGB 灯。

**当前硬件状态（EVT1）**：显示、触摸、音频（扬声器与双麦）、microSD、USB Host/Device、Wi-Fi、BLE、RTC、PMIC/充电域均已在实物上验证；相机因排线线序问题**等待 FPC 镜像转接板，当前不可用**；长时老化与低功耗功耗数据尚未表征。

## 2. 环境准备（唯一前置条件）

安装 **ESP-IDF `v6.1-rc1`** 或更新版本。任选一种官方方式：

- **EIM（ESP-IDF Installation Manager）**：按官方指引选择 v6.1-rc1 安装；
- **命令行**：
  ```bash
  git clone -b v6.1-rc1 --recursive https://github.com/espressif/esp-idf.git
  cd esp-idf && ./install.sh esp32s31 && . ./export.sh
  ```

ESP32-S31 目前是 **preview target**，`idf.py` 命令需要带 `--preview`（本文命令均已带上）。

## 3. 快速开始：三个固件工程

克隆仓库后直接编译，无需任何额外设置：

```bash
git clone https://github.com/jlckfb/Candis-S31.git
cd Candis-S31

# 手表综合演示（表盘 + 17 个应用：录音/播放/WiFi/BLE/文件/USB/游戏/设置/电源等）
# 未完善，有问题！！！！！
cd firmware/demo
idf.py --preview set-target esp32s31
idf.py --preview build

# Factory 产测/诊断固件（60+ 条硬件诊断命令，见 firmware/factory/README.md）
cd ../../firmware/factory
idf.py --preview set-target esp32s31
idf.py --preview build

# USB Device CDC 固件（C2 口枚举为串口设备）
cd ../usb_cdc_device
idf.py --preview set-target esp32s31
idf.py --preview build
```

`examples/esp-idf/getting-started`（最小示例）与 `examples/esp-idf/low-power`（低功耗状态机）同样开箱即编。

**首次编译会从 ESP Component Registry 联网下载公开依赖**（LVGL、esp_lvgl_adapter、esp_video、esp_codec_dev 等官方组件），属正常现象；板级相关代码全部来自仓内 `vendor/esp-bsp/` 快照。

## 4. 烧录与串口

- **只用 C1 口**（CH343P USB 转串口）：`idf.py --preview -p /dev/ttyACM0 flash monitor`（2M 波特已验证，约 44 秒/次）。
- **C2 口不能烧录**：ESP32-S31 ROM 无 USB-OTG 下载模式；C2 是 USB Host/Device 数据口。
- Windows 下串口名为对应 COM 口；Linux 通常为 `/dev/ttyACM0`。

## 5. 硬件使用注意（务必读）

1. **整机异常掉电（电流跌到约 4mA、串口无响应）（将取电限制放宽后未复现）**：长按 PWRON 恢复；**先不要拔线**——恢复后先用 `pmic regs`、`pmic power_off_source`、`pmic power_on_source`、`pmic irq_snapshot` 取证，再动硬件（TG28 只有拔 VBUS 才复位，拔线会丢现场）。
2. **相机模组暂停使用**：主板 FPC1 座与部分模组的 24P 排线线序存在镜像问题，**请等 FPC 转接板到位后再插相机**；板上 R95 保持拆除状态，不要自行补焊。
3. **自动烧录电路异常（仅出现一次，后面再未复现）**（串口完全静默、连 ROM 启动行都没有）：先量 BOOT 脚电平，若为 0V，拔插 C1 冷启动 20 秒即可恢复；不要反复烧录。
4. **GPIO16（TE）中断所有权归显示栈**，自定义诊断代码不要再对该脚注册 ISR。

## 6. 已知限制（内测期）

- 相机：不可用（等 FPC 转接板）。
- 电池电量计 SOC 为参考精度（内置 4.2V 通用模型；针对具体电芯的标定进行中）。
- RTC 存在快慢交替现象（约 ±1-2 秒/30 秒量级，VRTC 域，待定位）。
- 低功耗各模式功耗数据尚未表征；深睡唤醒可用，数值请以内测群共享数据为准。
- 音量映射曲线未校准（出厂默认音量偏大）。

## 7. 开发者向（可跳过）

- 固件默认使用仓内 `vendor/esp-bsp/` 的 BSP 快照。若你在同步开发 BSP：设置 `CANDIS_S31_BSP_PATH=<你的 esp-bsp 工作区>/bsp/candis_s31` 即可切换回活代码，构建系统优先使用该环境变量。
- 维护者同步快照：`tools/sync_bsp.sh`（来源提交记录在 `vendor/esp-bsp/SOURCE_COMMIT`）。
- 与上游的关系：器件驱动与 BSP 正按 espressif/esp-bsp 上游流程推进（见 `UPSTREAM.md`）；examples 对官方发布版的依赖待 PR 合入后切换。

## 8. 反馈

内测问题请带：串口完整日志、复现步骤、当前固件 git 版本（启动日志可见）。硬件异常先按第 5 节取证再反馈。
