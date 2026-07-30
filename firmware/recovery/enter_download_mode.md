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
   loader before writing flash.

Record whether automatic entry worked. DTR/RTS polarity and reset timing must
still be confirmed on EVT1.

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

After flashing, let `--after hard-reset` reset the board. If that fails,
release BOOT and press RESET once. A normal boot must not hold GPIO61 at the
download-strap level.
