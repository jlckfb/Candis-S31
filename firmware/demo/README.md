# Candis-S31 Watch Demo (firmware/demo)

手表形态综合演示固件：覆盖 EVT1 板载可用外设，主打 2.0 英寸 460×460 AMOLED
显示体验。显示管线：LVGL 9.5 PARTIAL 渲染 + 双 460×48 内部 DMA 缓冲 +
GPIO16 TE 门控（QSPI 48 MHz）。历史实板基线为局部更新约 60 fps、全屏更新约
30 fps（静态/轻量 workload；不是所有 UI 负载的保证）；运行时可在 Screen Test
页查看 TE/FPS 统计。速度档：Flash QIO 80 MHz + PSRAM Octal DDR 250 MHz
（S31 官方 Kconfig 档位，证据见 `firmware/factory/sdkconfig.qio80_psram250.defaults`）。

相机页面用于验证 OV5640 DVP 链路。传感器识别、DVP 取流和内建彩条已有实板
证据；30 fps 时序、单帧曝光上限、单次自动对焦和图像细节参数已按原厂资料
完成离线实现，仍须下次 EVT 实板确认真实场景画质与 JPEG 落卡。

## 功能

- 表盘（导航栈根）：RTC 时间 HH:MM（分钟级刷新，不做秒针）、日期+星期、
  电量细弧（充电时绿色）、板名；上滑或点击进入应用菜单。RTC 不可用时
  时间与状态栏显示 "--:--"，电量与界面不受影响。表盘本身即常驻活证据：
  时间/电量环均为 last-value 守卫写入，息屏后自动停摆，零成本健康指示器。
- 菜单：三列卡片网格（132×112）+ 分组节头（TESTS/HARDWARE/CONNECTIVITY/
  SYSTEM/GAMES），注册数 16 个应用，按压反馈仅颜色交换
- 测试中心（一级核心应用）：41 项主动测试分布于 9 个域（显示触控/相机/
  音频/存储/网络/系统/存储介质/加速硬件/电源），其中 27 项非交互 AUTO
  项可一键 "RUN ALL" 顺序执行（约 3-4 分钟）；结果入库为会话级 RAM 结果库，
  PASS/FAIL/WARN/SKIP/NOT RUN 五档语义与 factory 固件对齐；聚合优先级
  FAIL > WARN > NOT_RUN > SKIP > PASS；交互项（四色块/触摸四角/画线/取景/
  听音）有专用运行视图（进度条/阶段文本/操作员确认/交互画布），相机/音频/
  USB 等资源与功能应用互斥仲裁（忙则 SKIP）。前置条件不满足（无卡/息屏/
  音频占用）自动 SKIP 并留证据串，不伪判为通过
- 屏幕测试（显示触控）：纯色/黑白/灰阶全屏、触摸坐标全屏、局部动效 +
  TE/FPS 实时统计行（TE 经 esp_lvgl_port observer，不进 ISR）
- 相机：OV5640 800×600 RGB565 大端取景，中心裁切后零缩放贴帧；目标帧率
  30.003 fps，50/60 Hz 最大曝光分别为 30.06/25.03 ms，启动时执行内嵌单次
  自动对焦，点击取景画面可重新对焦。拍照经 S31 硬件 JPEG 编码器存 TF
  （`/photos/IMG_nnnn.jpg` 顺序命名，长按拍照键存 RGB565 裸帧）；真实成像与
  JPEG 落卡仍待实板确认。
  零拷贝 V4L2 MMAP 缓冲仅在 `LV_EVENT_REFR_READY` 后归还驱动，避免 AMOLED
  尚在取源时被下一帧覆盖。
  取景期间每 2 s 刷新闲置计时以避免中途息屏；离页后停止刷新，继续按用户设置计时。
  另有 5 帧捕获 CRC 自检测试项（camera.frames）。

- 录音机：仅左 MIC / 仅右 MIC / 双 MIC / 双 MIC+基础降噪，增益 0–36 dB，
  电平表，录到 TF WAV（最长 60 s），录音列表回放
- 播放器：后台扫描 TF 卡 WAV，播放、暂停、音量和进度控制
- WiFi：扫描 → 屏上键盘输密码 → 连接，显示 IP；连接成功后凭据持久化
  到 NVS，重启后一键重连，支持在 WiFi 页忘记凭据
- 蓝牙（NimBLE）：扫描 → 连接 → GATT 服务列表
- 文件管理：TF 目录浏览、详情、容量和热插拔提示；长按并二次确认后删除
- USB OTG：角色实时显示；U 盘自动识别、挂载和浏览；Device 模式实现中
- 游戏：2048、贪吃蛇、打砖块（触摸跟手 + 局部刷新流畅度的活体演示）
- 彩灯：WS2812B 颜色、呼吸和闪烁
- 设置：亮度、音量、麦克风增益、息屏超时、RTC 时间和关于
- 电源：电量详情、立即息屏、深度睡眠（RTC 闹钟唤醒）和关机
- 系统信息：芯片、Flash、PSRAM、堆水位、任务和运行时长

## 运行取舍（音频）

`svc_audio_start()` 在网络服务启动前一次性创建 speaker/microphone codec
句柄并保持到关机：I2S 双通道 DMA（约 12.8 KB 内部 DMA 内存）在内部池仍
完整时预占，录音/播放不再懒初始化（2026-08-21 实板日志：懒初始化在池
耗尽后 10/10 失败）。代价是 AUDIO_3V3_SW（TG28 ALDO3）与 I2S 通道在
demo 全程供电；深睡是芯片复位路径（唤醒后重新走启动链），睡眠失败返
回运行态时句柄仍有效，因此无需睡前释放。每流仍按需
`esp_codec_dev_open/close`。

## 充电与电量口径

`bsp_pmic_init` 强制 REG62=50 mA（带精确回读，ESP 单独复位后收回上次会话
抬升的档位）并验证 TG28 芯片内置 ROM 电量模型可读；不在仓库中嵌入或分发模型
字节。`svc_power` 的充电控制在 2 s 轮询里以 50→100→200→300→400→500 mA
逐档抬升（插线且电池首次在位后完整观察 2 s，门全绿即写第一档 100 mA；其后
每次成功抬升间隔精确 4 s；证据门：VBUS/电池在位、确实在充、VBAT 处于
[3000, min(Vchg+50, 4450)] mV、VBUS ADC ≥ VINDPM+320 mV，全部实读配置值，
限值未装入前禁止抬升；电池移除会重启 2 s 观察）。
UI 显示的是**已验证的目标档位**（REG62 上限），实际电流可被 VINDPM 自动
回退压低，不作为测量值呈现。HOLD 期间任一门失效即向 50 mA 降档（仅压降计入
收敛预算）；重复压降(2 次)或连续控制器轮询失败(3 次)收敛回 50 mA 并锁定到
拔线重插。500 mA 上限待电芯/连接器热签核；固定 TS 分压、无 NTC，不存在电芯
温度闭环。ROM 模型下的 SOC 为参考精度，不同电芯 SKU/化学体系需要授权的新模型；
状态栏和表盘以 `~` 标明未标定估值，电源页明确显示 `uncalibrated ref`。模型不可读时
百分比显示“电量模型未装入”，电压仅作诊断量。

## 测试报告导出（TF 卡 JSONL）

测试中心 "Export" 按钮把当前会话全部结果写入 `/sdcard/demo_report_
YYYYMMDD_HHMMSS.jsonl`（RTC 无效时退化为 uptime 命名）。每条测试一行：

```
FACTORY_RESULT {"id":"audio.speaker_tone","status":"PASS","evidence":"880Hz 2s audible","duration_ms":2410}
```

末尾一行 `FACTORY_SUMMARY {"total":41,"pass":..,"fail":..,"warn":..,
"skip":..,"not_run":..}`。行格式与 factory 固件主机工具（run_evt.py）
解析器完全兼容，可直接复用。结果不落 NVS（会话级诊断，factory 自身已有
持久化，职责不重复）；"Reset" 按钮清空结果库。

无人值守回归可在配置阶段传入
`-D CANDIS_DEMO_AUTOTEST=ON`。固件启动后会直接排队同一组 27 项 AUTO
测试，并逐项输出 `AUTOTEST_RESULT`，结束时输出 `AUTOTEST_SUMMARY`。
交互项仍保持 `NOT_RUN`，不会用默认答案伪造人工确认。此选项默认关闭。
`-D CANDIS_DEMO_PAGE_WALK=ON` 会在启动后依次打开 13 个非破坏性应用页，
逐页保留 `ui_perf` 创建/打开耗时并输出 `ui_page_walk: complete pages=13`；
测试中心、相机和 USB 因资源或链路仲裁不在此遍历中，分别由 AUTO 批次和独立
固件验证。

## 操作

- 触摸屏：点按、滑动；任意操作重置息屏计时
- BOOT 键（侧键）：短按 = 返回 / 表盘上进菜单；长按 = 立即息屏
- PWR 键：短按 = 回表盘（长按为 TG28 硬件关机，固件不拦截）
- 息屏后触摸/BOOT/PWR 唤醒

## 构建与烧录

克隆后开箱即编。工程通过仓库共用的
`cmake/candis_components.cmake` 注入 `components/candis_s31/`，四个可复用
驱动由组件清单映射到 `vendor/idf-extra-components/`：

```sh
cd firmware/demo
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview flash   # 板子经 /dev/ttyACM0 连接时
```

不要设置外部 BSP 路径；如需修改板级运行时，直接修改仓库内组件并重新构建。

## Host/Web LVGL 模拟器（设计评审用）

`sim/` 是同源码 Host 模拟器：真实编译 `main/ui/`、`main/apps/`、
`main/tests/` 的 UI 与框架代码（LVGL 树锁定 firmware 的 9.5.0 受管组件），
服务层（power/net/audio/storage）用 mock 任务按真实回调契约喂假数据。
相机/USB OTG 两页无对应总线模型，保留 `BOARD ONLY` 占位。

```sh
./sim/build_web.sh    # WASM 构建（Emscripten + pthreads）
./sim/serve_web.sh    # 127.0.0.1:8080，带 COOP/COEP 头
./sim/build_native.sh # 本机 SDL 窗口版（调试器友好）
```

VSCode 会自动转发 8080 端口；浏览器打开
`http://127.0.0.1:8080/candis_s31_sim.html`。URL 参数直达页面：
`?screen=<app-id>`（tests/display/recorder/player/wifi/ble/files/led/
settings/power/sysinfo/game2048/snake/breakout/menu/watchface），
`?state=charging|low-battery|no-battery` 注入电源状态。

协同迭代循环：改 `main/` 或 `sim/` 代码 → `./sim/build_web.sh` →
浏览器刷新即可（服务带 no-store，无需强制刷新）。模拟结果只用于
UI/交互评审，不构成任何硬件 PASS 证据；`RUN ALL` 跑的是桩测试
（`sim/main/sim_tests.c`，41 项元数据与固件一致，结果全是模拟值）。

画面回传（协同评审）：页面每 500 ms 把画布 PNG 和点击事件 POST 回
`serve.py`；AI 侧直接读 `http://127.0.0.1:8080/share/latest.png`
（用户当前画面）与 `/share/events`（最近 64 条点击/URL 事件），
无需任何浏览器扩展。地址栏随页面内导航自动同步 `?screen=<id>`，
把 URL 贴给 AI 即可精确复现页面。

## 架构速览

`demo_main.c` 启动 → `demo_board.c`(板级引导 + NVS 设置)→
`ui/ui_manager.c`(导航栈 + 状态栏 + 主题)→ `services/`(input/power/
storage/audio/net 五个后台任务)→ `tests/svc_test.c`(测试执行器任务)→
`apps/` + `games/`(纯 UI,经服务队列操作硬件)。服务回调经 `ui_async()`
落到 LVGL 线程;应用屏按需创建、退出即销毁(仅表盘/菜单常驻)。

界面以 20/24 px 思源黑体中文子集、至少 56 px 常用触控目标和 64 px 列表
行为基线；系统级设计约束见 [`docs/system-overview.md`](../../docs/
system-overview.md)。
