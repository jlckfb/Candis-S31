# Candis-S31 Watch Demo (firmware/demo)

手表形态综合演示固件:覆盖 EVT1 板载全部外设,主打 2.0 英寸 460×460 AMOLED
显示体验(48 MHz QSPI + TE 同步 + LVGL 9.5 双缓冲局部 60 fps)。

## 功能

- 表盘:RTC 时间/日期/电量,上滑(或点击)进应用网格
- 录音机:仅左 MIC / 仅右 MIC / 双 MIC / 双 MIC+基础降噪,增益 0–36 dB,
  电平表,录到 TF WAV(最长 60 s),录音列表回放
- 播放器:TF 卡 WAV 浏览播放,音量/进度
- WiFi:扫描 → 屏上键盘输密码 → 连接,显示 IP
- 蓝牙(NimBLE):扫描 → 连接 → GATT 服务列表
- 文件管理:TF 目录浏览/删除/容量,热插拔提示
- USB OTG:角色实时显示;U 盘自动识别挂载并浏览;Device 模式状态
- 游戏:2048、贪吃蛇(触摸)
- 彩灯:WS2812B 颜色/呼吸/闪烁
- 设置:亮度/音量/麦克风增益/息屏超时/RTC 时间/关于
- 电源:电量详情、立即息屏、深度睡眠(RTC 闹钟唤醒)、关机
- 相机:占位(等 FPC 线序转接板,EVT1 暂不启用)
- 系统信息:芯片/Flash/PSRAM/堆水位/任务/运行时长

## 操作

- 触摸屏:点按/滑动;任意操作重置息屏计时
- BOOT 键(侧键):短按 = 返回 / 表盘上进菜单;长按 = 立即息屏
- PWR 键:短按 = 回表盘(长按为 TG28 硬件关机,固件不拦截)
- 息屏后触摸/BOOT/PWR 唤醒

## 构建与烧录

与 factory 相同的契约,需要 `CANDIS_S31_BSP_PATH` 指向本地 BSP:

```sh
cd firmware/demo
export CANDIS_S31_BSP_PATH=<esp-bsp>/bsp/candis_s31
idf.py build
idf.py --preview flash   # 板子经 /dev/ttyACM0 连接时
```

## 架构速览

`demo_main.c` 启动 → `demo_board.c`(板级引导 + NVS 设置)→
`ui/ui_manager.c`(导航栈 + 状态栏 + 主题)→ `services/`(input/power/
storage/audio/net 五个后台任务)→ `apps/` + `games/`(纯 UI,经服务队列
操作硬件)。服务回调经 `ui_async()` 落到 LVGL 线程;应用屏按需创建、
退出即销毁(仅表盘/菜单常驻)。

设计文档与评审记录:`.work/evt1/demo/DESIGN.md`(不入库)。
