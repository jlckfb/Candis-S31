# QIO 80 MHz + PSRAM 200 MHz 提速验证操作卡

面向 2026-08-25 早实板验证。本配置为 `sdkconfig.defaults`（DIO 40 MHz +
PSRAM 100 MHz 已验证基线）之上的增量提速档，全部依据见
`sdkconfig.qio80_psram200.defaults` 头注释；不超频：80 MHz 是 ESP32-S31 当前
IDF 正式菜单最高 Flash 档（120 MHz 因 IDF-14653 未放行，禁用），200 MHz 是
S31 PSRAM 的 Kconfig 默认档（250 MHz 待器件额定悬案闭环后单列验证）。

## 构建（已验证，rc=0）

```sh
cd firmware/factory
idf.py -B build-qio80-psram200 \
  -DSDKCONFIG=$PWD/sdkconfig.qio80_psram200 \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qio80_psram200.defaults" \
  build
```

独立 `build-qio80-psram200/` 与独立 `sdkconfig.qio80_psram200`，不触碰默认
`build/` 与 `sdkconfig`。

## 烧录 + 串口（板在位后执行）

```sh
cd firmware/factory
idf.py -B build-qio80-psram200 -p /dev/ttyACM0 -b 2000000 flash monitor
```

（端口以实板枚举为准；历史 2 Mbaud 烧录已在该板验证过。）

## 回退姿势

任何异常（不启动、启动报 Flash/PSRAM 错、MEMTEST FAIL）：

```sh
idf.py -B build -p /dev/ttyACM0 -b 2000000 flash monitor
```

默认 `build/` 目录对应原 DIO 40 MHz + PSRAM 100 MHz 基线镜像，烧回即恢复。
两个镜像互不覆盖，可反复切换。

## 判读要点

1. **启动日志**：bootloader/2nd stage 应报 `QIO` 与 `80MHz`（注意：镜像头
   mode 字节按 IDF 设计仍写 dio，由 bootloader 启动时升级到 quad，见
   esptool_py/Kconfig.projbuild:76-81；以启动日志与
   `sdkconfig.qio80_psram200` 中 `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` 为准）。
2. **PSRAM**：启动应报 Octal PSRAM 200 MHz；`SPIRAM_MEMTEST` 启动自检须
   `OK`。若 200 MHz 下 MEMTEST FAIL 或反复冷启动偶发失败 → 直接回退基线并
   记录，本轮不强求。
3. **功能回归**：
   - `mem_psram_verify`（PSRAM 容量/alias/读写校验，由 PsramProbe 提供）；
   - 显示回归：局部刷新约 60 fps / 全屏约 30 fps 无残影、无撕裂恶化；
   - 摄像头 + 显示并发压力（HW-Debug.md §2.4 建议项）。
4. **稳定性**：反复冷启动 ≥5 次，无灰白闪之外的新增异常；记录启动日志留档。

## 风险

- **QIO 对烧录无影响**：esptool 始终以 dio 烧 bootloader（IDF 设计），
  烧录器/串口流程与基线完全一致；风险只在固件运行期。
- **QIO 运行期风险**：W25Q128 IO2/IO3 与 WP#/HOLD# 复用，EVT1 已把四线接到
  SPIQ/SPIHD（HW-Debug.md §2.4 网表证据），理论无障碍；若启动日志报
  quad mode 切换失败，回退基线即可。
- **PSRAM 200 MHz 非超频**（Kconfig 默认档），但器件容量/额定悬案未闭环；
  失败判据见上文第 2 条，回退路径始终可用。
