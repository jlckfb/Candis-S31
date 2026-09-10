# 低功耗状态机示例（low-power）

Candis-S31 低功耗状态机示例。四档功耗状态 + LP core 管家，演示"HP 睡、LP 值守"的分工。

| 项目 | 当前值 |
|---|---|
| 目标芯片 | ESP32-S31（preview target） |
| 测试 ESP-IDF | `v6.1-rc1` |
| 主路径 | **light sleep → deep sleep → TG28 软关机**（软关机需仅电池供电） |
| 编译状态 | **已验证**（ESP-IDF v6.1-rc1） |

## 状态定义

| 状态 | 名称 | 屏幕 | 触摸 | 外设轨 | HP 核 | LP core |
|---|---|---|---|---|---|---|
| **S0** | `RUN` | 亮 | dynamic | 开 | light sleep 切片轮询 | 运行（值守共享 IRQ 线） |
| **S1** | `SCREEN_OFF` | CO5300 sleep | monitor（~10µA，可被触摸唤醒） | 保持 | light sleep | **停止**（交还 GPIO2） |
| **DEEP_SLEEP** | `DEEP_SLEEP` | 关协议并关闭显示 VBAT/VCI | CST820 重新开启 ALDO2，进入 monitor，GPIO3 作为 EXT1 | 除 TOUCH 外的可控外设/控制轨全关 | deep sleep（唤醒即重启） | 停止 |
| **S2** | `SHUTDOWN` | 断电 | 断电 | 全关 | 断电（主轨切断后） | 断电 |

S0 的关键点是**委派**：HP 不自己轮询 RTC/PMIC，而是进 light sleep，由 LP core 通过 mailbox 中断把事件推上来。
DEEP_SLEEP 是 S1 与 S2 之间的深睡档：S1 超时后停止面板和触摸协议所有者，再执行 `bsp_power_safe_state()`，把可控外设/控制轨和防倒灌 GPIO 收到安全态后进入深睡。为保留 GPIO3 触摸唤醒，当前实现随后仅重新开启 CST820 的 ALDO2 触摸供电，并让控制器进入 monitor 模式；显示 VBAT/VCI、相机、音频、SD 和其他可控轨保持关闭。唤醒会重跑 `app_main`，`bsp_board_init()` 再次执行安全态，随后 S0 重新初始化面板/触摸。唤醒源是 SoC RTC 定时（20s）+ EXT1 任意低电平；EXT1 mask 同时武装 GPIO2（共享 IRQ：RX8130CE 闹钟 /IRQ、TG28 IRQ 都能经它唤醒）与 GPIO3（CST820 触摸 INT）。触摸唤醒需要 CST820 在深睡期间保持供电，S2 关机需要仅电池供电。

## 迁移表

| 源 | 目标 | 触发条件 | 迁移动作 |
|---|---|---|---|
| 冷启动 | S0 | 上电/复位（cause=UNDEFINED） | `bsp_board_init` → LP I2C → PMIC → RTC → 排空共享 IRQ 线（契约 D3）→ 报告启动原因 → **深睡计数归零** |
| S0 | S1 | 停留满 `S0_DWELL_SECONDS`(15s) | 屏 sleep → 触摸 monitor → 停 LP core → 交还 GPIO2 → 武装 light sleep 唤醒源 |
| S1 | S0 | **触摸** INT 拉低或 PMIC 电源键事件 | 撤唤醒源 → 触摸退 monitor → 屏 sleep-out → 重启 LP core |
| S1 | S1 | 共享 IRQ 拉低（非电源键的 PMIC/RTC 事件） | 服务共享线（清标志至线释放），**不离开 S1**，也不缩短空闲预算 |
| S1 | DEEP_SLEEP | 无用户唤醒达 `S1_SLICES_BEFORE_S2 × S1_SLEEP_SLICE_US`(30s) | 撤 light sleep 唤醒源 → 关闭面板/触摸协议所有者 → `bsp_power_safe_state()` 关闭可控轨并置高阻 → 重新开启 TOUCH ALDO2、重建 CST820 并进入 monitor → GPIO2/GPIO3 配 RTC 输入 + 内部上拉 → 武装深睡唤醒源（RTC 定时 + EXT1）→ 计数 +1 → `esp_deep_sleep_start()` |
| DEEP_SLEEP | S0 | 定时唤醒且计数 < `DEEP_SLEEP_MAX_CYCLES`(2)；或任意 EXT1 唤醒 | 定时唤醒保留计数；EXT1 外部事件优先并清零计数，回 S0 |
| DEEP_SLEEP | S2 | 仅定时唤醒且计数 ≥ `DEEP_SLEEP_MAX_CYCLES`(2) | 重启式唤醒 → 直接走下方 S2 顺序 |
| S2 | 冷启动 | TG28 重新上电（RTC 闹钟到点 / 插 USB / 按电源键） | 走"冷启动"行（cause=UNDEFINED，计数归零） |

### S2 的关闭顺序（顺序不可交换）

1. 停 LP core、撤 light-sleep 唤醒源；
2. 释放面板/触摸协议所有者，调用 `bsp_power_safe_state()` 关闭可控外设轨及负载开关并停放 GPIO；
3. 读取 PMIC VBUS 状态；读取失败或 USB 仍供电时拒绝软关机，保持安全态；
4. 仅电池供电时，读有效 RTC 日历，用 `rx8130ce_alarm_from_time()` 设绝对时刻闹钟 → `bsp_rtc_alarm_irq_enable(true)` → 清残留标志 → 确认 GPIO2 已释放为高；
5. **最后**调用 `bsp_pmic_power_off()`（TG28 REG10 bit0 软关机，写后整机掉电）。若调用意外返回，打印错误并等待复位。

### S2 的"唤醒"语义（重要）

主轨断后 **ESP 完全断电**，没有 RAM 保持、没有唤醒残留。所以：

- S2 的"唤醒"**实为 TG28 重新上电后的冷启动**，不是 sleep 返回；
- RTC 闹钟的作用是**定时让 TG28 重新上电**，而不是"唤醒 ESP"；
- 因此 `esp_sleep_get_wakeup_causes()` 在这条路径上必然报 `UNDEFINED`。判定"是否闹钟叫醒"要靠 **PMIC 上电源寄存器**（`bsp_pmic_get_power_on_source`）+ **RX8130CE 的 AF 标志**，`report_boot_reason()` 就是这么做的；
- 跨 S2 需要保留的状态必须落 NVS 或 RTC 寄存器域之外的存储，本示例没有演示这一点。

**与 DEEP_SLEEP 唤醒的区分（启动分流依据）**：深睡唤醒同样是“app_main 重跑”，但 RTC 域全程未断电，`esp_sleep_get_wakeup_causes()` 的位图包含真实唤醒源（TIMER/EXT1…）；S2 再上电是真正掉电后的冷启动，位图包含 `BIT(ESP_SLEEP_WAKEUP_UNDEFINED)`。`app_main` 据此分流：包含 `UNDEFINED` → 深睡计数归零、回 S0；否则按计数决定回 S0 还是进 S2。两者不会混淆。

## 无交互验收窗口

- 保持触摸和按键不动，采集 **180 s**：每轮 S0 停留 15 s、S1 空闲 30 s、深睡定时 20 s，共两轮名义 130 s，另加初始化/轨道切换时间。S0/S1 使用经过 light-sleep 补偿的 `esp_timer` 单调时间计时，LP mailbox 或共享 IRQ 提前唤醒不再把一次唤醒误算作完整切片。
- 必须看到 `deep sleep: cycle 1/2`、定时唤醒的 `cycle 1/2, returning to S0`、`deep sleep: cycle 2/2`、`deep-sleep budget exhausted (2 cycles), going to S2`。仅看到 LP 心跳或进入 S1 不算状态机完成；LP 心跳只证明 mailbox 通路正常。
- **USB 供电时 S2 停在 VBUS guard**：`VBUS present: deferring PMIC power-off` → `still powered after S2; idling`。此分支不设闹钟，之后拔 USB 不会自动重试；软关机与 RTC 冷启动需仅电池供电。
- 完整 S2→RTC 冷启动需在**仅电池供电**下进行，RTC `time_valid=1`，共享 IRQ 可释放；采集工具必须不经 VBUS 给板供电（例如独立 UART RX/GND）。预留 **8 min**：进入 S2 后闹钟比较目标分钟，等待约 241–300 s，再检查 `requesting PMIC power-off` 后的实际断电及新一轮 `PMIC power-on source` / `RTC flags ... alarm=1` 冷启动证据。电流值还需电表；串口静默本身不是断电证明。
- 触摸/电源键唤醒是另一条真实硬件交互路径，需人工动作或物理治具；不能由上述无交互定时路径代替。

## 唤醒源表

| 状态 | 唤醒源 | 引脚/机制 | 触发方式 | IDF API |
|---|---|---|---|---|
| S0 | LP mailbox 中断 | PMU 软中断 | LP core 上报 | `esp_sleep_enable_ulp_wakeup()` |
| S0 | 定时切片 | LP timer | 2s | `esp_sleep_enable_timer_wakeup()` |
| S1 | 触摸 | `BSP_TOUCH_INT`(GPIO3) | 低电平 | `gpio_wakeup_enable()` + `esp_sleep_enable_gpio_wakeup()` |
| S1 | 共享 IRQ | `BSP_PMIC_RTC_INT`(GPIO2) | **低电平**（不可用边沿，见下） | 同上 |
| S1 | RTC 定时 | LP timer | 10s | `esp_sleep_enable_timer_wakeup()` |
| DEEP_SLEEP | SoC RTC 定时 | LP timer | `DEEP_SLEEP_WAKE_US`(20s) | `esp_sleep_enable_timer_wakeup()` |
| DEEP_SLEEP | 共享 IRQ | `BSP_PMIC_RTC_INT`(GPIO2) | **EXT1 任意低电平**（RTCIO 输入 + 内部上拉保险；仅 RX8130CE /IRQ、TG28 IRQ 可拉低） | `esp_sleep_enable_ext1_wakeup_io()` + `ESP_EXT1_WAKEUP_ANY_LOW` |
| DEEP_SLEEP | 触摸 | `BSP_TOUCH_INT`(GPIO3) | **EXT1 任意低电平**；安全态后重新开启 ALDO2、重建 CST820 并进入 monitor，再将 GPIO3 切到 RTCIO 输入 + 内部上拉；完整触摸唤醒结果仍待实板确认 | 同上 |
| S2 | RTC 闹钟 | RX8130CE /IRQ → GPIO2 | 让 TG28 重新上电 | 冷启动，无 sleep API |


**为什么共享 IRQ 必须电平触发**：TG28 IRQ 与 RX8130CE /IRQ 是开漏线与。源 B 在源 A 仍拉低时动作**不产生新边沿**，边沿触发必然丢中断。BSP 的 `bsp_shared_irq_register_callback()` 用 `GPIO_INTR_LOW_LEVEL` 正是这个原因。

**为什么 light sleep 用 `gpio_wakeup_enable()` 而不是 `esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown()`**：后者需要 `SOC_GPIO_SUPPORT_HP_PERIPH_PD_SLEEP_WAKEUP`，esp32s31 的 `soc_caps.h` **未定义**该宏。相应地本工程不开 `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP`——开了会关掉 GPIO 模块，使这条唤醒路径失效。

## LP core 分工

LP 固件（`main/lp_core/main.c`）低速轮询共享 IRQ 线电平，边沿变化时唤醒 HP 并经 mailbox 上报：

| 事件 | 载荷 | 含义 |
|---|---|---|
| `BOOT` | 协议版本 | LP 初始化完成（**第一帧，sync send**） |
| `HEARTBEAT` | 轮询计数 | 1s 心跳 |
| `IRQ_ASSERTED` | 轮询计数 | 共享线由高转低 → HP 侧服务 I2C 清标志 |
| `IRQ_RELEASED` | 轮询计数 | 共享线回高 |
| `I2C_SAMPLE` / `I2C_ERROR` | 寄存器值 / 错误码 | 预留给 LP I2C 直读 RX8130/TG28（本示例未启用，见"设计边界"） |

消息格式：`lp_message_t` 高 8 位事件码 + 低 24 位载荷（`main/lp_shared.h`，HP 与 LP 共用，故不含任何 IDF 依赖）。

### GPIO2 归属

GPIO2 同一时刻只能属于一方：LP core（RTC 功能，LP IO matrix）或 HP core（数字功能）。

- LP core 值守时（S0），pad 归 LP；
- 但**服务共享线是 HP 的活**（I2C 读写 + `gpio_get_level()` 判线释放），而 `gpio_get_level()` 读的是数字输入寄存器，pad 挂在 RTC 功能上时其值不保证跟随真实线电平；
- 所以 `service_shared_irq()` 的做法是：先 `rtc_gpio_deinit()` 把 pad 拿回 HP → 调 `bsp_shared_irq_service()` → 再 `claim_shared_irq_pad_for_lp()` 交还。

补充一条 BSP 行为观察：`bsp_shared_irq_service()` 内部的 `shared_irq_gpio_init()` 有 `s_gpio_ready` 一次性标志，**只有首次调用**会 `gpio_config()` 抢 pad，后续调用只做 I2C + 读电平。本示例仍每次显式交接，一是覆盖首次调用，二是让归属意图可读。

### mailbox 使用约束

**LP 侧只用 `lp_core_mailbox_send()`，不要 async 与 sync 混用。**

先 `lp_core_mailbox_send_async` 再 sync `lp_core_mailbox_send` 会因 tx_idx 奇偶槽错位，把消息写进 HP 不扫描的 ACK 槽，HP 侧 receive 与 LP 侧会互相等死。

`main/lp_core/main.c` 因此**全程只用 `lp_core_mailbox_send()`**，初始化后第一帧就是 sync send。改这个文件时请勿引入 async 发送。

另有两条来自 IDF 头文件的约束：
- 软件 mailbox 要求 **LP core 先** `lp_core_mailbox_init()`，HP 才能 init。HP 侧 `start_lp_housekeeper()` 因此 `vTaskDelay(100ms)` 后才 init；
- LP 侧 `lp_core_mailbox_send()` 的 timeout 单位是 **CPU 周期**（HP 侧是 tick）。本示例给有界值而非 `-1`，避免 HP 停止接收时把管家卡死。

## 电流参考值

估算依据为数据手册量级与组件标称值。

| 状态 | 分项 | 数值 | 依据 |
|---|---|---|---|
| **S1** | ESP32-S31 light sleep | ~50 µA 级（估算） | S31 datasheet light-sleep 量级 |
| | CST820 monitor 模式 | ~10 µA | `esp_lcd_touch_cst820.h` 中 monitor 档说明 |
| **DEEP_SLEEP** | ESP32-S31 deep sleep | ~10 µA 级（估算） | S31 datasheet deep-sleep 量级 |
| | 触摸保持供电时的整机输入基线 | 约 9.4-9.7 mA @ 5.1 V | `DEEP_SLEEP_KEEP_TOUCH_RAIL=1`；数据含 CH343P/TG28/FUSB303 等常供负载 |
| **S2** | ESP32-S31 | 0 | 主轨已断，芯片无供电 |
| | RX8130CE（备份域） | 亚 µA 级（估算） | 备份电池供电，`INIEN=1` 自动切换；具体值查 ETM50E-05 |

**注**：S31 的 CPU 档位为 40/240/320 MHz。

## 实现要点

### 1. deep sleep 与 EXT1 唤醒脚

S1 与 S2 之间插入 DEEP_SLEEP 档，用 RTC 定时 + GPIO2 EXT1 唤醒。

**EXT1 唤醒脚必须有确定电平偏置（上拉或下拉），不能浮空**，否则芯片深睡后唤不醒。本板 GPIO2 经 R31（10kΩ）上拉到 TG28_VRTC（3.0V 常供轨），电平确定，可安全用作 EXT1 源；代码里仍额外使能内部上拉作保险。

其他注意点：

- 深睡 RTCIO 仅 GPIO0~7 —— GPIO2（共享 IRQ）和 GPIO3（触摸 INT）都在范围内；本示例的 EXT1 mask 同时武装两脚（ANY_LOW + 内部上拉保险）。进入深睡前会在安全态后重新开启 CST820 的 ALDO2 供电、重建控制器并进入 monitor，再把 GPIO3 切到 RTCIO 输入；
- **无 EXT0，只有 EXT1**，须用 `esp_sleep_enable_ext1_wakeup_io()` + `ESP_EXT1_WAKEUP_ANY_LOW`；
- 深睡唤醒是重启（app_main 重跑），跨深睡状态靠 RTC 慢速内存的 `RTC_NOINIT_ATTR` 计数器 + magic 校验（掉电后内容失效→magic 不匹配→归零）：计数器为 0 时回到 S0，达到上限才升级 S2。

### 2. TG28 关主轨：`bsp_pmic_power_off()`

S2 的最后一步调用 `bsp_pmic_power_off()`：它封装 TG28 的软件关机（REG10 bit0 Soft PWROFF），写后整机掉电，是本板认可的整板下电路径。

调用序列：打印日志 → `vTaskDelay(50ms)` 让日志冲出 → `bsp_pmic_power_off()`。若调用意外返回（PMIC 拒绝、仍带电），打印错误并落入空转等复位。

### 3. BSP RTC 边界

- **选择性清 AF**：BSP 提供 `bsp_rtc_get_and_clear_alarm_flag()`，只清 RX8130CE 的 AF，保留 UF/TF；本示例在武装 S2 闹钟后使用该接口。随后若 UF/TF 本身仍把共享 GPIO2 拉低，`service_shared_irq()` 会报告并清除对应标志，以保证关机前线路释放；
- **闹钟武装顺序**：本示例在 `arm_rtc_wakeup_alarm()` 里实现整套顺序（读时→换算→设闹钟→使能→清 AF→排空→自检），含 VLF 兜底（`time_valid` 为假时**拒绝关机**）。

### 4. 其他

- **音频链路**：S2 只把 AUDIO 轨关掉，不做 codec 操作；
- **不启动 LVGL**：用 `bsp_display_new()` 直接拿面板句柄，省掉 draw buffer；BSP 的 raw panel API 会自动应用 EVT1 的 180° 物理镜像。
- **触摸 monitor 档的进入方式**：`bsp_display_enter_sleep()` 通过公共 `esp_lcd_touch` 睡眠钩子进入 CST820 monitor 处理，本示例随后用 `esp_lcd_touch_cst820_exit_monitor_mode()` 复位回 dynamic，再调用 `enter_monitor_mode()` 让它落入可被触摸唤醒的 standby。CST820 无触摸约 2 s 后自动进入 standby，因此 monitor 档需最多约 2 s 才真正生效；
- **不使用未公开的 CST820 睡眠寄存器命令**：`0xA5` 等 CST816 家族命令不属于本示例的自动流程。

## 文件结构

```
low-power/
├── CMakeLists.txt                  # 工程；注入仓库自有板级组件
├── sdkconfig.defaults              # 通用配置（含 ULP/LP core 开关）
├── sdkconfig.defaults.esp32s31     # S31 专属（flash 容量、分区）
├── README.md
└── main/
    ├── CMakeLists.txt              # 组件注册 + ulp_embed_binary
    ├── lp_shared.h                 # HP/LP 共用消息契约（无 IDF 依赖）
    ├── low_power_main.c            # 状态机、唤醒源、pad 归属、RTC 闹钟
    └── lp_core/
        └── main.c                  # LP 管家固件（rv32imac）
```

## 构建

工程默认通过 `cmake/candis_components.cmake` 使用
`components/candis_s31/`；四个可复用驱动映射到
`vendor/idf-extra-components/`，无需任何外部检出。

本工程当前位于 `examples/esp-idf/low-power/`，下列命令均在该目录内执行；
烧录与监视走 Type-C1（CH343P 桥）的串口；按主机修改 `/dev/ttyACM0`，Windows 使用对应 COM 口。

```bash
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -p /dev/ttyACM0 monitor
```

Board Manager 的声明式定义位于 `vendor/esp-board-manager/`，由
`examples/esp-idf/display-hello` 单独演示。
