# Candis-S31 Camera Preview

Continuous OV5640 preview on the 2.0-inch 460x460 AMOLED. Build and flash it,
point the lens at a subject, and the live image appears on the panel. The
centre 460x460 window of the 800x600 sensor frame is published directly with
no overlay, no touch input and no operator interaction; initialisation and
stream failures are reported on the panel and in the serial log.

The board component (`components/candis_s31/`) owns the whole sensor profile -
byte order, blanking, PLL/DVDD setup, ISP colour trim and the 180-degree panel
mounting. This example selects a V4L2 format, applies the profile and drives
the preview.

## Build and flash

Run from this directory after activating the official ESP-IDF environment:

```bash
cd examples/esp-idf/camera-test
idf.py --preview -B build -D IDF_TARGET=esp32s31 build
idf.py --preview -B build -p /dev/ttyACM0 -b 4000000 flash monitor
```

The example uses the repository-owned `components/candis_s31/` board component
and the drivers under `vendor/idf-extra-components/`, injected through
`cmake/candis_components.cmake`; it needs no external BSP checkout.

## Serial output

A healthy startup prints, in order:

```
I (1709) ov5640: Detected Camera sensor PID=0x5640
I (2043) camera_test: CAMERA_PREVIEW_READY format=RGB565X source=800x600 crop=460x460 first_frame_ms=33
I (4021) camera_test: CAMERA_PREVIEW_RATE capture=39.3fps presented=78 superseded=0
```

`CAMERA_PREVIEW_READY` is printed after the first valid frame has been handed
to LVGL; `first_frame_ms` measures from STREAMON. `CAMERA_PREVIEW_RATE` is a
single two-second startup window, not a long-run throughput measurement.
Steady-state preview is otherwise silent: invalid frames are logged and
skipped, and a failed pipeline reports `CAMERA_PREVIEW_STOPPED` with an
on-screen message.

Measured on EVT1 (2026-09-10): reset to `CAMERA_PREVIEW_READY` 2.04 s (1.14 s
bootloader/PSRAM, 1.71 s sensor detect, 2.04 s first frame), 39.3 fps capture
with `presented=78 superseded=0` in the first two seconds.

## Configuration options (`main/Kconfig.projbuild`)

| Option | Default | Meaning |
| --- | --- | --- |
| `CAMERA_TEST_FORCE_50HZ` | n | Manual 50 Hz mains-light compensation instead of the sensor's automatic detection. OV5640 DVP application note 2.13 section 4.7.8 requires the detection settings to match the input clock, and this board drives XCLK at 20 MHz rather than the 24 MHz the sensor defaults assume. Enable for 50 Hz installations when banding under mains-powered lighting is visible. |
| `CAMERA_TEST_SENSOR_STATS` | n | Print one `CAMERA_SENSOR_STATS` line per second with the sensor's exposure, AGC gain, AWB R/G/B gains and AWB limits, plus the frame's mean RGB. Off in normal use; enable it when tuning the board sensor profile for a different module or illuminant. |
| `CAMERA_TEST_SERIAL_FRAME_DUMP` | n | Export one source frame and the displayed crop over the console after three seconds of steady preview. Use `tools/capture_camera_frame.py` instead of a text monitor: the console temporarily switches to 460800 baud. |

## Board camera profile

`components/candis_s31/src/bsp_camera.c` applies and read-back-verifies the
whole profile; the example only supplies the V4L2 format.

- **Byte order.** The `esp_cam_sensor` table named RGB565_BE writes
  `0x4300=0x6F`, which OV5640 v2.33 Table 7-15 defines as RGB565 with output
  sequence `0xF` (blue first). The BSP re-applies `0x61` (sequence `0x1`,
  `{r[4:0],g[5:3]},{g[2:0],b[4:0]}`, red first) and `0x501F=0x01`. The
  application consumes V4L2 `RGB565X` and renders
  `LV_COLOR_FORMAT_RGB565_SWAPPED` without any esp_video byte swapping
  (`CONFIG_ESP_VIDEO_ENABLE_SWAP_BYTE=n`).
- **Clock and timing.** 20 MHz XCLK from XTAL-backed LEDC, 800 MHz PLL1,
  80 MHz DVP clock, HTS=2060, VTS=984, AEC limit 980 lines. PLL2 is retuned
  for the 20 MHz input so the ADC runs at 200 MHz.
- **Mounting.** `CONFIG_BSP_DISPLAY_ROTATE_180` (default y) flips the CO5300
  scan direction and mirrors the CST820 raw coordinates, because the EVT1
  AMOLED is mounted upside down.
- **Lens shading.** `0x5000` keeps LENC enabled (`0xa7`). Disabling it (bit 7
  clear) costs about 38 % of corner brightness and drops the corner red/green
  ratio from 0.84 to 0.50 on a full-frame grey target, so the shipped profile
  leaves the sensor's correction curve in place.
- **Colour.** Automatic white balance remains enabled; the trim does not write
  manual AWB gains. `BSP_CAMERA_CMX_COLOR_TRIM` (default y, red 112 % / blue
  109 %) scales the red/blue input-column magnitudes in `0x5381..0x5389`,
  preserving the signs in `0x538A/0x538B`. The base matrix is restored before
  each application, so repeated profile calls do not compound the trim.
  On 2026-09-10 a full-frame grey card under indoor lighting measured, over the
  displayed 460x460 window, R/G = 0.990 and B/G = 1.025 (centre 300x300:
  1.034 / 1.026). That is a single-module, single-illuminant check, **not** an
  absolute or multi-illuminant colour-accuracy claim: automatic white balance
  moves its operating point by roughly ±10 % between scenes, and the same
  profile measured R/G = 0.93 on a second, dimmer scene at 6.25x AGC.
  Setting both percentages to 100 restores the unscaled board matrix;
  `BSP_CAMERA_SKIP_ISP_PROFILE` bypasses both the board ISP matrix and its trim
  for explicit A/B diagnosis.
- **Known module limits (measured, not fixed in software).** The bare lens
  shows a residual radial red deficit: with LENC on, R/G falls from 1.08 at
  the frame centre to 0.84 at the corner of the 800x600 frame. A global matrix
  trim cannot flatten a radial error, so the periphery keeps a slight cyan
  cast. In dim scenes the sensor pins exposure at 969/984 lines with AGC at
  6.25x and the colour error grows (R/G 0.62 on an unlit whiteboard). The AWB
  red limit (`0x5193`, 0x70 or 0xF0) was measured to have no effect on the
  converged gains in either regime.
- **DC clock.** `BSP_CAMERA_CORE_120MHZ` keeps the LCDCAM core at 120 MHz
  (divide-by-one) for DVP; the IDF default divides it to 60 MHz and truncates
  frames.

`CONFIG_BSP_CAMERA_SENSOR_DATA_REMAP` swaps D0/D1 and D3/D4 for a non-standard
adapter. It must stay disabled: an unnecessary bit swap makes pixel values
non-monotonic and produces severe colour banding. A stale project `sdkconfig`
can retain an old value, so delete `sdkconfig` after changing
`sdkconfig.defaults`, or build with `tools/build-all.sh --isolated camera-test`.

The application applies the profile **after open and S_FMT, before REQBUFS and
STREAMON**, because the lazy video initialisation overwrites pre-open
registers. MMAP frames remain capture-owned until the crop copy completes; the
four display buffers remain LVGL-owned until refresh completion.

## Autofocus

The demo does not implement autofocus. The EVT1 module's voice-coil motor rail
(`CAM_AF_VCC`) has no populated path on this board, so the lens has no focus
actuator and no `cam_motor` device is registered; `bsp_camera_start()` sets up
DVP capture only. The fitted lens is fixed and rests at its spring position,
which is focused for subjects at a normal working distance.

## Verifying the frame without the panel

From the repository root, with `CAMERA_TEST_SERIAL_FRAME_DUMP=y` flashed
and any text monitor closed:

```bash
python tools/capture_camera_frame.py --reset --baud 460800 \
  --output-dir /tmp/frame
```

The tool writes `source.ppm`, `source-center-crop.ppm` and
`display-rgb565.ppm`, plus raw buffers and CRC-checked frame metadata. The
exported display data is the submitted LVGL crop, **not panel readback**; it
should be byte-identical to the source centre crop, which proves the copy did
not alter colour. Capture temporarily blocks frame publication while
transmitting about 1.38 MB at 460800 baud; do not use that interval as a
preview-fps measurement. Export occurs after three seconds of preview so the
sensor's AEC/AWB have settled.

## Validation status

EVT1 validation with ESP-IDF v6.1-rc1 (2026-09-10) established: reset to
`CAMERA_PREVIEW_READY` in 2.04 s, 39.3 fps capture with `dropped`/`rejected`
at zero, byte-identical source/display crops, grey-card neutrality over the
displayed window, and the LENC A/B above. Re-run the default preview, the
`CAMERA_TEST_SENSOR_STATS` build and the ISP-profile-bypass build after any
change to the profile, then repeat the grey-card capture before claiming a
colour result.
