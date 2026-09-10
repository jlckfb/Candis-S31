# Candis-S31 内测指南

> 面向内测用户：安装官方 ESP-IDF 后，克隆本仓库即可编译全部工程。板级运行时位于 `components/candis_s31/`，四个可复用驱动和 Board Manager 资料分别位于 `vendor/idf-extra-components/`、`vendor/esp-board-manager/`；不依赖外部 BSP 检出，也不需要额外环境变量。

## 1. 这块板子是什么

Candis-S31 是围绕 ESP32-S31 与 2.0 英寸 460×460 方形 AMOLED 设计的低功耗开发板。板上资源：CO5300 AMOLED（QSPI）+ CST820 触摸、TG28 电源管理/充电/电量计、RX8130CE RTC、双 USB Type-C（C1 供电+调试、C2 OTG）、ES8389 音频（扬声器 + 双麦克风）、DVP 摄像头接口、microSD、按键、单颗 WS2812 RGB 灯。

**当前硬件状态（EVT1）**：显示、触摸、microSD、USB Host/Device、Wi-Fi、BLE、RTC、PMIC/充电域均已在实物上验证；音频数字链路已验证，听感和录音验收仍按当前硬件记录维护。相机传感器识别、DVP 取流和内建彩条已验证；真实场景图像质量与 Demo JPEG 落卡仍待确认；长时老化与完整低功耗功耗拆分仍待补充。

## 2. 环境准备（唯一必需前置条件）

安装 **官方 ESP-IDF `v6.1-rc1`** 或更新版本，并启用 preview target：

```bash
git clone -b v6.1-rc1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s31
. ./export.sh
```

也可以使用 Espressif Installation Manager（EIM）或官方 VS Code 扩展选择同一版本。ESP32-S31 目前是 **preview target**，本文命令均带 `--preview`。

## 3. 快速开始

克隆仓库后，在仓库根目录执行完整构建矩阵：

```bash
git clone https://github.com/jlckfb/Candis-S31.git
cd Candis-S31
./tools/build-all.sh
```

也可以单独编译任一工程：

```bash
cd examples/esp-idf/camera-test   # 相机预览示例
idf.py --preview -D IDF_TARGET=esp32s31 build

cd ../display-hello               # Board Manager 屏幕示例
idf.py --preview -D IDF_TARGET=esp32s31 build
```

可用工程清单：

- `examples/esp-idf/getting-started`：只验证 SoC、Flash 和串口，不初始化板级外设；
- `examples/esp-idf/display-hello`：使用仓内 Board Manager 生成配置和官方 LVGL adapter 的最小屏幕示例；
- `examples/esp-idf/low-power`：S0/S1/深睡/S2 + LP core 状态机原型；
- `examples/esp-idf/player`：TF 卡音视频播放器；
- `firmware/factory`：产测专用固件；
- `examples/esp-idf/camera-test`、`examples/esp-idf/power-cycle`、`examples/esp-idf/usb-cdc-device`：相机、功耗和 USB 专用独立工程。
- `examples/esp-idf/buttons`：BOOT/PWR/RTC 闹钟按键事件串口打印；
- `examples/esp-idf/led`：WS2812B RGB 常亮/呼吸/闪烁循环；
- `examples/esp-idf/rtc`：RX8130CE 读写与分钟闹钟（共享中断线上报）；
- `examples/esp-idf/pmic`：TG28 只读转储 + AUDIO_PA 一次 off→on→恢复；
- `examples/esp-idf/storage`：TF 卡挂载/容量/目录/文件写读删探测；
- `examples/esp-idf/display-touch`：AMOLED 十字标记跟随手指并打印坐标；
- `examples/esp-idf/display-benchmark`：全屏/局部刷新帧率（验收线：全屏 ≥30、局部 ≥60）；
- `examples/esp-idf/audio-recorder`：麦克风录 5 秒到 `/sdcard/example_record.wav`；
- `examples/esp-idf/audio-player`：播放 1 kHz 程序生成正弦音，再播放录音文件；
- `examples/esp-idf/wifi`：扫描打印 AP；Kconfig 配置 SSID 后连接打印 IP；
- `examples/esp-idf/ble`：NimBLE 以 `candis-example` 广播 10 秒后扫描 10 秒；
- `examples/esp-idf/usb-host-msc`：Type-C2 Host 挂载 U 盘列根目录（含 500 mA 守卫验证）。

`display-hello` 的 Board Manager 配置文件已经提交在工程的 `components/gen_bmgr_codes/`，普通用户不必先安装额外 Python 工具。修改 `vendor/esp-board-manager/esp_friends_boards/candis_s31/` 后，维护者才需要加载本地 `idf_ext.py` 并重新运行 `idf.py bmgr -b candis_s31`。

首次编译会从 ESP Component Registry 下载公开依赖（LVGL、esp_lvgl_adapter、esp_video、esp_codec_dev 等）；各工程的 `dependencies.lock` 固定已解析的版本和哈希。仓内板级代码和四个未发布驱动通过相对 `override_path` 固定到本仓路径。

已有本地实验配置时，用 `./tools/build-all.sh --isolated` 验证仓库默认值。
每次运行在打印的日志目录下生成 `iso/<target>/build` 和独立 `sdkconfig`，
不复制、删除或覆盖工程原有配置；也可只选 `--isolated camera-test display-benchmark`。
这属于配置隔离，不等同于干净克隆或重新下载全部依赖。普通模式仍是增量构建；
遇到其他芯片的旧配置会失败并提示隔离，不再自动删除配置。
Kconfig 或 `sdkconfig.defaults` 改了，不代表已有 `sdkconfig` 会同步改变。

## 4. 烧录与串口

- **只用 C1 口**（CH343P USB 转串口）：`idf.py --preview -p /dev/ttyACM0 -b 4000000 flash`，再用 `idf.py --preview -p /dev/ttyACM0 monitor` 监控；4,000,000 baud 是当前实测最高可靠下载速率，线材或主机不稳定时降速。
- **C2 口不能烧录**：ESP32-S31 ROM 无 USB-OTG 下载模式；C2 是 USB Host/Device 数据口。
- Windows 下串口名为对应 COM 口；Linux 通常为 `/dev/ttyACM0`。

## 5. 硬件使用注意（务必读）

1. **整机异常掉电（电流跌到约 4mA、串口无响应）**：长按 PWRON 恢复；先不要拔线，恢复后先用 `pmic regs`、`pmic power_off_source`、`pmic power_on_source`、`pmic irq_snapshot` 取证，再动硬件。
2. **相机转接与 AF 供电**：部分模组需要线序匹配的 FPC 转接板；上电前核对接线，不要把历史诊断用的 DVP 位交换套用到标准模组。R95 是 AF-VCC 与传感器 AVDD 的连接电阻，不应因油画感盲目拆除；本轮画质故障由关闭错误的数据位重映射修复，R95 保持原状。
3. **自动烧录电路异常**：若串口完全静默，先量 BOOT；若为 0V，拔插 C1 冷启动约 20 秒后再量，不要反复烧录。
4. **GPIO16（TE）中断所有权归显示栈**：自定义诊断代码不要对该脚注册 ISR；Factory 使用已有 TE observer/显示栈接口。
5. **GPIO36 是 VDD_SPI strap 且兼作 TF 电源控制**：不要拆除 R6，也不要在复位采样期间强行拉低该脚。

## 6. 已知限制（内测期）

- 相机：传感器识别、DVP 取流和内建彩条已验证；真实场景颜色/图像质量以及 Demo JPEG `/sdcard/IMG_nnnn.jpg` 落卡尚未完成硬件验收。
- 已修复的相机油画/伪色：旧配置交换 D0/D1 和 D3/D4，造成亮度和色度值非单调。`BSP_CAMERA_SENSOR_DATA_REMAP` 现默认关闭；旧构建目录中的 `sdkconfig` 若仍为 `y`，需在 menuconfig 中关闭或使用新配置重新构建。
- 相机细彩色横带尚未完全解决：校验通过的源图在显示前已含横带，不能仅凭全帧传输时间或TE开关判为显示撕裂。2026-09-08同一时钟/格式下，仅将OV5640 `0x4005[1]`从持续黑电平更新改为正常冻结，用户确认横纹明显减少、屏闪不再出现；仍有残余横纹；随后用户反馈画面偏暗偏绿，不确定是否在BLC切换后出现，随后仅提高自动增益上限至`0x00f8`，用户确认亮度和颜色改善且稳态横纹/屏闪未加重。启动阶段短暂屏闪/横纹加重另以暖机期间暂缓上屏处理，首次上屏目视仍待验收；不宣称全部横纹消失。该对照由camera-test的默认关闭选项`CAMERA_TEST_BLC_NORMAL_FREEZE`提供，不能当作全部产品路径的画质PASS。源帧发布on/off/on统计只用于定位；详见camera-test说明。
- 电池电量计：默认使用 TG28 芯片内置 ROM 模型；SOC 仅作参考，具体电芯尚未标定。
- RTC：板上没有独立的 RTC 纽扣电池或超级电容，电池与 C1 同时移除会丢失时间；长期漂移数据仍需按记录补齐。
- 低功耗：通用深睡和触摸唤醒已有实测，完整示例状态机与功耗分项仍需持续回归。
- 音频：数字链路已验证，音量映射和不同声源下的最终产品听感仍需按板卡复核。
- 显示：默认物理方向为180°。严格验收门槛仍为全屏不低于30 FPS、局部不低于60 FPS；本轮TE开启实测全屏29.973–29.987、局部59.959–59.971，尚未达标。统计是LVGL刷新吞吐，不是物理扫描帧率或无撕裂验收，不将59.97向上取整为PASS。

## 7. 开发者向（可跳过）

- 仓库自有板级组件：`components/candis_s31/`；统一注入入口：`cmake/candis_components.cmake`。
- 可复用驱动快照：`vendor/idf-extra-components/`；Board Manager 与 `candis_s31` 定义：`vendor/esp-board-manager/`。
- `tools/sync_upstream.sh` 从维护者的本地准备工作树刷新两个 vendor 快照；来源提交记录在各自的 `SOURCE_COMMIT`。
- 上游归属和暂不提交 PR 的说明见 `UPSTREAM.md`。官方 `esp-bsp` 仅维护 Espressif/M5Stack 板，本仓库不依赖其检出。

## 8. 反馈

内测问题请带：串口完整日志、复现步骤、当前固件 git 版本（启动日志可见）。硬件异常先按第 5 节取证再反馈。
