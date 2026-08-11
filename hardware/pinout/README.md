# Preliminary pinout

This table is derived from schematic revision 0.5 and cross-checked against the BSP header `candis_s31.h`. It has not been checked against an assembled EVT1 board.

## Board functions

| Function | Signal | GPIO |
|---|---|---:|
| TF card | Card detect | 0 |
| USB Type-C control | Enable, active low | 1 |
| PMIC / RTC | Shared interrupt, active low | 2 |
| Touch | Interrupt, active low | 3 |
| RGB LED | Data | 4 |
| Display power | VBAT enable | 5 |
| Low-power I2C | SCL | 6 |
| Low-power I2C | SDA | 7 |
| Audio | Data out | 8 |
| AMOLED QSPI | Chip select | 10 |
| AMOLED | Reset, active low | 15 |
| AMOLED | Tearing effect | 16 |
| AMOLED QSPI | Clock | 12 |
| AMOLED QSPI | Data 0 | 11 |
| AMOLED QSPI | Data 1 | 13 |
| AMOLED QSPI | Data 2 | 14 |
| AMOLED QSPI | Data 3 | 9 |
| Touch | Reset, active low | 17 |
| Audio | Bit clock | 18 |
| Audio | Left/right clock | 19 |
| TF card | Data 0 | 20 |
| TF card | Data 1 | 21 |
| TF card | Data 2 | 22 |
| TF card | Data 3 | 23 |
| TF card | Clock | 24 |
| TF card | Command | 25 |
| Main I2C | SCL | 33 |
| Main I2C | SDA | 34 |
| Audio | Master clock | 35 |
| TF card power | Enable, active low | 36 |
| Display power | VCI enable | 38 |
| Camera | Reset, active low | 39 |
| Camera | Power-down | 40 |
| Audio | Speaker amplifier enable | 42 |
| USB Type-C control | Interrupt, active low | 43 |
| Audio | Data in | 44 |
| USB OTG | Enable | 45 |
| Camera | Data 0-7 | 46-53 |
| Camera | Pixel clock | 54 |
| Camera | Master clock | 55 |
| Camera | VSYNC | 56 |
| Camera | HSYNC | 57 |
| UART0 | TX | 58 |
| UART0 | RX | 59 |

## Reserved and special pins

- GPIO26, GPIO27, GPIO28, GPIO30, GPIO31, and GPIO32 are reserved for flash
  and VDD_SPI; ESP32-S31 has no GPIO29.
- GPIO36, GPIO37, GPIO60, and GPIO61 are strapping pins. In particular,
  GPIO36 drives the active-low TF-card power enable, so its external pull-up
  and level during the sampling window must be checked against the VDD_SPI
  strap requirements before EVT1 firmware drives it.
- GPIO41 is not available for normal application use.
- ESP32-S31 physical package pins 44 and 45 are the native USB D+ (USB_DP)
  and D- (USB_DM) pads; they are not GPIO44 and GPIO45. Each passes through a
  33 ohm series resistor (R25/R23) on the way to Type-C2. The separate USB
  Serial/JTAG interface is fixed to GPIO33/GPIO34. Those GPIOs are already
  the Candis-S31 main-I2C SCL/SDA pair, so enabling USB Serial/JTAG would
  collide with the board bus; it is not wired to a connector. Console and
  flashing therefore use UART0 through CH343P. The GPIO-numbered signals
  remain audio data-in on GPIO44 and USB-OTG enable on GPIO45 as listed above.
- GPIO54-57 are the JTAG signals MTDO, MTCK, MTDI, and MTMS. On this board
  they drive the camera PCLK, XCLK, VSYNC, and HSYNC lines, so external JTAG
  use conflicts with camera capture.
- There is no 32.768 kHz crystal on the board. The XTAL_32K pads are the
  GPIO0/GPIO1 pins, already assigned to TF-card detect and the FUSB303B
  enable, so the SoC RTC slow clock uses the internal RC oscillator. The
  RX8130CE has its own built-in crystal; its FOUT output only reaches the
  RTC_FOUT1 test net and is not connected to the SoC.
- GPIO2 is shared by the PMIC and RTC interrupt outputs. Firmware must identify and clear both sources.

## Expansion connector (EXT, GH1.25-4)

| Pin | Signal |
|---:|---|
| 1 | GND |
| 2 | 3V3_EXT_SW (TG28 BLDO2 output, 3.3 V, 300 mA max) |
| 3 | EXT_I2C_SDA |
| 4 | EXT_I2C_SCL |

Pins 5 and 6 are mechanical shield pads tied to GND. EXT_I2C is isolated
from the main I2C bus (SYSTEM_I2C on GPIO33/34) through Q6 (2N7002DW); the
connector side is pulled up to 3V3_EXT_SW by R103/R104 (4.7 kohm), so the
port is active only while BLDO2 is enabled.

## Physical keys

The board has reset, power-on, and boot keys. They operate the reset path,
TG28_SW power control, and the ESP32-S31 boot strap respectively; they are not
general application buttons. The BSP therefore does not expose them through
the ESP-BSP button API.

## Before using this table in firmware

1. Compare the signal with the latest schematic revision.
2. Check its active level and reset state.
3. Confirm that the pin is not used by flash, PSRAM, or a boot strap.
4. Validate the associated power rail before driving an enable signal.
5. Record any board revision dependency in the driver.

Do not turn this document into a framework-specific header. The same hardware facts should feed `esp_friends_boards/candis_s31`, Arduino, PlatformIO, factory tests, and documentation.
