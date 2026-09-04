# Candis-S31 Camera Test

Diagnostic firmware for the camera / DVP link with live AMOLED preview. It
initializes the display, runs bounded start/capture/stop cycles, then leaves
the live camera image on the screen; the final PASS result is only emitted
after the operator taps PASS on the display — serial-only frame checks are
reported separately as automatic evidence (`main/camera_test_main.c` header).

The `esp_cam_sensor` format table named RGB565_BE writes `0x4300=0x6F`, which
OV5640/Linux semantics define as RGB565 LE. The repository board component re-applies
`0x61` (RGB565 BE) after that table; this diagnostic verifies the resulting
RGB565X byte stream without hiding it behind esp_video byte swapping
(`sdkconfig.defaults` keeps `CONFIG_ESP_VIDEO_ENABLE_SWAP_BYTE=n`).

Status: the camera link is partially verified on EVT1 hardware — the corrected
FPC adapter, sensor detection, DVP streaming, and built-in color-bar path pass.
The real-scene image remains anomalous and the Demo JPEG file path is not yet
accepted; keep the visual operator verdict separate from serial PASS.


## Build and flash

```bash
idf.py --preview -C firmware/camera_test -D IDF_TARGET=esp32s31 build
idf.py --preview -C firmware/camera_test -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C firmware/camera_test -p /dev/ttyACM0 monitor
```

该工程通过 `cmake/candis_components.cmake` 使用仓库自有
`components/candis_s31/`，四个通用驱动来自 `vendor/idf-extra-components/`。
5 s 启动延迟（`CONFIG_CAMERA_TEST_BOOT_DELAY_MS`）为串口监控连接预留窗口。

## Variants

`main/CMakeLists.txt` builds `camera_test_main.c` + `camera_visual.c`. For
the headless RAW8 variant, replace the first SRCS entry with
`camera_raw_test_main.c`: that variant initializes the sensor through the
same esp_video path, then releases the esp_video DVP controller and
reconfigures the public `esp_driver_cam` controller for true 8-bit RAW
capture at 320-wide (no display, serial statistics only).

## Configuration options (`main/Kconfig.projbuild`)

| Option | Default | Meaning |
| --- | --- | --- |
| `CAMERA_TEST_USE_UYVY` | n | Use the sensor UYVY output and convert to RGB565 in the app, bypassing a module-specific RGB565 packing fault. |
| `CAMERA_TEST_DISPLAY_PATTERN` | n | Replace camera frames with a fixed four-quadrant pattern to isolate the display / LVGL path from sensor data. |
| `CAMERA_TEST_SENSOR_COLOR_BAR` | n | Enable the OV5640 internal color bar before streaming to isolate the DVP receiver from the analog image path. |
| `CAMERA_TEST_SENSOR_PCLK_INVERT` | n | Drive OV5640 register `0x4740` bit 5 low (sensor-side sampling-edge experiment). |
| `CAMERA_TEST_RECEIVER_PCLK_INVERT` | n | Set the S31 LCD_CAM `CAM_CLK_INV` bit after STREAMON (receiver-side sampling-edge experiment). |
| `CAMERA_TEST_UYVY_CTRL_3F` | n | Use the Linux-compatible UYVY format control value `0x3F` for a module-level output-format comparison. |
| `CAMERA_TEST_CYCLES` | 3 | Complete start/capture/stop cycles run at boot (checks teardown and re-initialization). |
| `CAMERA_TEST_FRAMES` | 6 | Frames captured and checked per cycle. |
| `CAMERA_TEST_BOOT_DELAY_MS` | 5000 | Delay before board/camera init so a serial monitor can attach. |

`sdkconfig.defaults` additionally pins the runtime baseline: explicit display
configuration (48 MHz pixel clock, TE sync, double draw buffer) because visual
confirmation is required for PASS; OV5640 800x600 with both the RGB565_BE and
YUV422 tables registered and YUV422 as boot default (the DVP controller
cannot change color families after open); XCLK from the board-local LEDC
workaround (`CONFIG_BSP_CAMERA_XCLK_USE_LEDC`); and driver-owned MMAP camera
buffers in PSRAM.
