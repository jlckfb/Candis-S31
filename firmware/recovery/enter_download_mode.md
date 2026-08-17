# Enter ESP32-S31 ROM download mode

Use the **Type-C1 programming connector**. Type-C1 is connected through the
CH343P USB-to-UART bridge to UART0 TX/RX on GPIO58/GPIO59. Type-C2 is the USB
OTG connector and is not the programming port.

## Automatic entry through CH343P

1. Disconnect any serial monitor that already owns the port.
2. Connect Type-C1 with a known data cable and identify the new serial device.
3. Run `esptool` with `--before default-reset`. The CH343P DTR/RTS download
   circuit should assert reset and the GPIO61 boot strap automatically.
4. Confirm `esptool --chip esp32s31 --port PORT chip-id` identifies the ROM
   loader. A successful `chip-id` is the evidence that download mode was
   entered; do **not** record which strap levels produced it, because the
   exact strap bit order is not yet settled. ESP-IDF `v6.1-beta1` labels
   GPIO60/GPIO61 as "Boot Mode select 3/4" in
   `soc/esp32s31/register/soc/io_mux_reg.h`, while `soc/boot_mode.h` `IS_00XX`
   tests bits 2-3 for download and the `GPIO_STRAP_REG` field comment in
   `soc/esp32s31/register/soc/gpio_reg.h` still carries another chip's pin
   names plus a "need update the description" note. Capture the levels with a
   scope if the mapping matters for a report.
5. Pass explicit `0 1` GPIO61 assertions to `flash_factory.sh`: `0` is the
   observed download-reset level, while `1` is the required post-write run
   level. The script refuses to write without both assertions.

Record whether automatic entry worked. DTR/RTS polarity and reset timing must
still be confirmed on EVT1.

## Boot-mode straps shared with board functions

GPIO38/GPIO39/GPIO40 are labelled "Boot Mode select 0/1/2" by the same IDF
header, and this board also uses them as `LCD_VCI_EN_H`, `CAM_RST_N`, and
`CAM_PWDN`. From a cold start all three sit low (R49 and R94 to GND; R93 pulls
to `CAM_DOVDD_2V8_SW`, which is off), so a power-on entry is unaffected.

The hazard is a **warm** entry: if the firmware has already brought the display
or camera rails up, it drives those pins high, and a reset taken in that state
may sample a different boot mode. If automatic entry fails on a board that was
running display or camera tests, remove power so every switched rail collapses
and the pins return to their resistor-defined levels, then retry from a cold
start before concluding the ROM path is broken.

## Manual BOOT-key sequence

If automatic entry fails:

1. Hold the **BOOT** key to keep GPIO61 at its download-strap level.
2. While holding BOOT, press and release **RESET**. If the reset key cannot
   produce a clean reset on the board revision, remove and reapply power while
   BOOT remains held.
3. Keep BOOT held through reset release, then release BOOT.
4. Immediately run the `chip-id` preflight again.

Do not substitute the power-on key for BOOT. The reset and power-on keys pass
through the TG28_SW/reset chain and are not application GPIOs. If neither
automatic nor manual entry works, stop and capture GPIO61, `CHIP_PU`, DTR, RTS,
UART0 TX, and UART0 RX on an oscilloscope; do not erase flash while the ROM
connection is intermittent.

## Exit

`flash_factory.sh` keeps the ROM session alive after writing, then performs a
hard reset only after the caller has explicitly asserted GPIO61 run level `1`.
Release BOOT before that reset. Save UART0 output and accept recovery only if
the ROM log reports SPI boot and the Factory application starts. If the reset
fails, release BOOT and press RESET once; a board that remains in download
mode has failed recovery.
