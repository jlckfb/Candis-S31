# Candis-S31 EVT1 as-fabricated hardware facts

Extracted 2026-08-14 from the running EasyEDA project 【ESP32-S31】Candis-S31, board `v0.5_260803_1544打样` / PCB_3 full pad netlist (291 components, 939 pads); cross-checked against BSP `candis_s31.h` and `pinout/README.md` (all consistent). Schematic drafting hazard noted for next revision: display-page LCD_VCI_EN_H wire self-overlaps and passes within 1e-13 of the LCD_QSPI_SIO2 stub endpoint near (275,735)/(330,720) — not a net join, confirmed separate nets in the PCB netlist; clean up in the next schematic revision.

## U4 ESP32-S31 (QFN80) pad → net → function

Functional rows only; power/ground pads summarized below the table.

| pad | Signal | PCB net | Function / link |
|---|---|---|---|
| 1 | ANT | $3N276 | C5/L2 → RF matching; R1/R2 0Ω select one of RF1 IPEX / U3 ceramic antenna |
| 2,3 | VDDA3/VDDA4 | $3N259 | Analog supply, C11(1uF)+C1(100nF) decoupling via L1/L2(2nH/600mA); Espressif strongly recommends an added 10uF — missing on EVT |
| 4 | CHIP_PU | ESP32S31_CHIP_PU | ←R28←D1←KEY_RST_N (TG28.29 PWROK); Q1.3 auto-download |
| 5 | GPIO0 | GPIO0 | TF card detect (CARD1.CD) |
| 6 | GPIO1 | FUSB303_EN_N | Type-C controller enable, active low |
| 7 | GPIO2 | GPIO2 | U2 (SN74LVC1G07) open-drain output = merged RTC/TG28 interrupt, active low |
| 8 | GPIO3 | GPIO3 | LCD_CTP_INT_N (R52 10kΩ pull-up to CTP rail) |
| 9 | GPIO4 | GPIO4 | WS2812B_DATAIN, via R102 33Ω → U25 buffer |
| 10 | GPIO5 | GPIO5 | LCD_VBAT_EN_H → U18 TPS22917 ON (R54 10kΩ pull-down) |
| 12,13 | GPIO6/7 | GPIO6/7 | LP_I2C_SCL/SDA (via R26/R27 to TG28.40/39 and U1 RTC) |
| 14 | GPIO8 | ES8389_DSDIN | I2S playback data |
| 15 | GPIO9 | GPIO9 | U14.2 = LCD_QSPI_SIO3 |
| 16 | GPIO10 | GPIO10 | U14.7 = LCD_CS_N (R50 10kΩ pull-up LCD_3V3_SW) |
| 17 | GPIO11 | GPIO11 | U14.5 = LCD_QSPI_SIO0 |
| 19 | GPIO12 | GPIO12 | U14.6 = LCD_QSPI_CLK |
| 20 | GPIO13 | GPIO13 | U14.4 = LCD_QSPI_SIO1 |
| 21 | GPIO14 | GPIO14 | U14.3 = LCD_QSPI_SIO2 |
| 22 | GPIO15 | GPIO15 | U14.8 = LCD_RST_N (R51 10kΩ pull-down) |
| 23 | GPIO16 | GPIO16 | via R55 (0Ω) → U14.9 = LCD_TE |
| 24 | GPIO17 | GPIO17 | U14.18 = LCD_CTP_RST_N |
| 25,26 | GPIO18/19 | ES8389_SCLK/LRCK | I2S BCLK/LRCK |
| 27-29,31-33 | GPIO20-25 | GPIO20-25 | TF DAT0/DAT1/DAT2/DAT3/CLK/CMD |
| 36-38,40-42 | SPI_* | QSPI_CS/DO/WP/HOLD/CLK/DI | via 0Ω array to U5 W25Q128 FLASH_* |
| 39 | VDD_SPI | ESP32S31_VDD_SPI | Internally supplied **output**: VDD3P3 → internal POWER_SWITCH → RSPI ≈ 3Ω → pad 39 (datasheet Table 5-3). U5.8 (W25Q128 VCC) shares the net; decoupling C21/C26 100nF + C22/C27 1µF matches the design guide's required 0.1µF + 1µF. `R24` (0Ω from VCC_3V3_MAIN) and `R22` (100kΩ to FLASH_CS) are both **DNP, and correctly so** — see the DNP section |
| 44,45 | USB_DP/DM | $3N245/$3N247 | via R23/R25 33Ω → ESP32S31_USB_DP/DM → USB2 A6/A7/B6/B7 (D5 ESD); physical pads, not GPIO44/45 |
| 46,47 | GPIO33/34 | FUSB303_SCL/SDA | Main I2C SYSTEM_I2C; bus pull-ups R4/R5 2.2kΩ → VCC_3V3_MAIN |
| 48 | GPIO35 | GPIO35 | via R59 (33Ω) → ES8389_MCLK |
| 49 | GPIO36 | GPIO36 | VDD_SPI voltage strap: R6 10kΩ pull-up = high = 3.3V. `R9` 10kΩ to GND is **DNP and must stay empty** (footprint is present). Also TF_PWR_EN_N active low, via R100 0Ω → Q5 gate |
| 50 | GPIO37 | GPIO37 | JTAG_SEL strap, 10kΩ pull-up, no application load |
| 51 | GPIO38 | GPIO38 | via R48 → LCD_VCI_EN_H → U14.1 (R49 pull-down). ESP-IDF GPIO docs/io_mux call this secondary `Boot Mode select 0`, although it is omitted from datasheet v0.5 Table 3-1's user-facing strap list; it is a don't-care for the documented SPI and Joint Download modes |
| 52,53 | GPIO39/40 | CAM_RST_N/CAM_PWDN | Camera reset / power-down. ESP-IDF GPIO docs/io_mux call these secondary `Boot Mode select 1/2`, although datasheet v0.5 exposes only GPIO36/37/60/61 as user-facing straps. Reconcile them as lower bits of a wider internal boot-mode field: they are don't-care when GPIO61 selects SPI Boot or GPIO61/GPIO60 select Joint Download, and may participate in undocumented SPI Download/ATE/diagnostic combinations. Exact bit-level behavior awaits an S31 TRM definition |
| 55 | GPIO42 | AUDIO_PA_EN_H | PA CTRL (100kΩ pull-down) |
| 56 | GPIO43 | FUSB303_INT_N | Type-C interrupt, active low (100kΩ pull-up) |
| 57 | GPIO44 | ES8389_ASDOUT | I2S record data; via R61 (0Ω) doubles as AD1 power-up config — must be Hi-Z/low while ALDO3 rises (BSP order: ALDO3 on first, then the I2S channel is created, so the pad is still in its reset Hi-Z state at rail rise; teardown deletes the I2S channel before dropping the rail) |
| 58 | GPIO45 | GPIO45 | U15.3 OTG boost permission logic input (100kΩ pull-down) |
| 59-62,65-68 | GPIO46-53 | GPIO46-53 | CAM_D0-D7 |
| 69-72 | GPIO54-57 | GPIO54-57 | CAM_PCLK/XCLK/VSYNC/HSYNC (JTAG mux conflict) |
| 73 | GPIO58 | $3N97 | via R17+R37 (499Ω each) → CH343P_RXD (UART0 TX, 998Ω total) |
| 74 | GPIO59 | ESP32S31_UART0_RXD | via R36 (499Ω) ← CH343P_TXD |
| 75,76 | GPIO60/61 | ESP32S31_STRAP/BOOT | 10kΩ pull-up each; GPIO61 = BOOT key / auto-download |
| 78,79 | XTAL_N/P | XTAL_N/P | X1 passive 40MHz (YSX321SL 15pF; L4 24nH in series with XTAL_P branch, 22pF on both sides) |

Power/ground pads: 11/43/54/64/77/80 = VCC_3V3_MAIN; 30/34/35 = ESP_LDO_1V8 (in-package PSRAM); 18 = VREF_TOUCH (470nF); 63 = VREF_ADC (100nF); 81 (EP) = GND.

## TG28 (U6, QFN-40) rail → load

> Inductor designators corrected 2026-08-19 from the PCB_3 pad netlist: `$2N82` = {U6.22, U7.1}, `$2N83` = {U6.25, U9.1}, `$2N33` = {U6.35, U8.2}. The previous revision of this table swapped U7/U8 and listed pins 35/22 against the wrong converters.

| pin | Net | Load / note |
|---|---|---|
| 21/22 | VCC_3V3_MAIN (DCDC1 3.3V/2A): pin 22 = LX1 switch node → **U7** 1µH → rail; pin 21 = output/sense | Main rail, always on (49 pads). **Probe the rail at U7 pad 2**, not at U8 |
| 25/26 | CAM_DVDD_1V5_SW (DCDC2 1.5V): pin 25 = LX2 switch node → **U9** 1µH → rail; pin 26 = output/sense | FPC1.10. **Probe at U9 pad 2, or C112/C113 (100nF/1µF) / C38 (22µF)** — probing U7 returns 3.3 V and looks like a DCDC2 fault |
| 18 | LCD_3V3_SW (ALDO1 3.3V) | U14.13+14 (panel maker confirmed pin13 = NC, no load branch) |
| 19 | LCD_CTP_3V3_SW (ALDO2 3.3V) | U14.22/23, touch pull-ups, Q2 isolation gate |
| 16 | AUDIO_3V3_SW (ALDO3 3.3V) | ES8389/MIC/audio pull-ups; also U20.3 self-enable PA switch |
| 15 | TG28_ALDO4 via L6 -> CAM_AVDD_2V8_SW | FPC1.4; R95 (0Ω) -> CAM_AF_VCC (FPC1.23) - **R95 refitted 2026-08-20; see "Post-fabrication reworks"** |
| 12 | CAM_DOVDD_2V8_SW (BLDO1 2.8V) | FPC1.11, camera I2C pull-ups |
| 14 | 3V3_EXT_SW (BLDO2 3.3V/300mA) | U26.2, R103/R104, Q6 |
| 20 | TG28_DC1SW | U24.1 (WS2812B-1313-V6 VDD), U25.5 (buffer VCC); OTP default OFF pending first-board measurement |
| 28 | TG28_VRTC (3.0V always on) | U1.10 (RTC VBAT), R31/R32 pull-ups |
| 27 | TG28_VBACKUP | Backup input |
| 33 | TG28_VBAT+ | CN1.1 two-wire battery; TS = R29 10kΩ fixed to GND (no real NTC) |
| 37 | TG28_VBUS | ←U12 TPS22917←U11 resettable fuse←USB1_VBUS_RAW |
| 6/7/13/17/23/24/34 (+35) | TG28_VSYS | System bus → U18 (LCD_VBAT) / U20 (PA) / U16 (ISL9113). Pin 35 is the charger-side switch node: pin 35 → **U8** 1µH → TG28_VSYS. All three 1µH parts share the same `IND-SMD_L2.5-W2.0_FTC252010S` footprint, so identify them by net, not by size |
| 4/5/8/9/10/11/32 | NC | DCDC3, DCDC4 (LX4/FB4 floating), CPUSLDO unused |
| 29 | KEY_RST_N (PWROK) | →D1.1; R101 (510Ω) → SW3 RESET; C119 |
| 30 | KEY_PWRON_N (PWRON) | SW1 PWRON key; R32 10kΩ pull-up VRTC |
| 38 | TG28_IRQ_N | U2.2 input, wired-AND with U1.6 (RTC /IRQ), R31 10kΩ pull-up VRTC |
| 39/40 | TG28_SCL/SDA | via R26/R27 → GPIO6/7 (LP I2C, 7-bit 0x34) |
| 1 | TG28_CHGL_LED | Charge indicator LED |

## Connectors as-fabricated

- U14 display 24P BTB: 1=LCD_VCI_EN_H 2=SIO3(GPIO9) 3=SIO2(GPIO14) 4=SIO1(GPIO13) 5=SIO0(GPIO11) 6=CLK(GPIO12) 7=CS(GPIO10) 8=RST(GPIO15) 9=TE(GPIO16) 10=GND 11/12=NC 13/14=LCD_3V3_SW 15=LCD_VBAT 16=GND 17=CTP_INT(GPIO3) 18=CTP_RST(GPIO17) 19=CTP_SCL 20=CTP_SDA 21=GND 22/23=LCD_CTP_3V3_SW 24=NC 25-28=shell GND
- FPC1 camera 24P (FH12-24S): 1=NC 2=GND 3=CAM_I2C_SDA 4=CAM_AVDD_2V8 5=CAM_I2C_SCL 6=CAM_RST_N 7=CAM_VSYNC 8=CAM_PWDN 9=CAM_HSYNC 10=CAM_DVDD_1V5 11=CAM_DOVDD_2V8 12=CAM_D7 13=CAM_MCLK 14=CAM_D6 15=GND 16=CAM_D5 17=CAM_PCLK 18=CAM_D4 19=CAM_D0 20=CAM_D3 21=CAM_D1 22=CAM_D2 23=CAM_AF_VCC 24=GND (AF_VCC fed from CAM_AVDD through R95 0Ω as fabricated; **R95 refitted by hand 2026-08-20**, so FPC1.23 is currently powered with the camera rail. No VCM driver IC on board: AF_VCC is a raw 2.8 V feed, not a focus control signal. Pin 23/24 are the family-incompatible positions - OV5640-AF modules expect 23=AF-VCC/24=AF-GND, while the Korvo OV3660 module uses 23=LED+/24=IR_CUT)
- CARD1 TF: 1=DAT2 2=DAT3 3=CMD 4=TF_VDD_3V3_SW 5=CLK 6=GND 7=DAT0 8=DAT1 CD=GPIO0 (GPIO20-25 mapping in U4 table)
- USB1 (debug port): VBUS→U11 fuse→U12 TPS22917→TG28_VBUS; CC1/CC2 via R33/R34 5.1kΩ pull-down (D2/D4 ESD); D+/D-→CH343P UD+/UD-
- USB2 (OTG): CC1/2→U17 FUSB303B; D+/D-→R23/R25→U4.44/45; VBUS=OTG_VBUS_5V (U16 ISL9113 boost; EN=OTG_BOOST_EN_SAFE from U15 SN74LVC1G58 wired as 2-input AND with one inverted input — U15.1 tied to VCC_3V3_MAIN selects the config, U15.3=GPIO45 (R40 100kΩ pull-down), U15.6=FUSB303_SOURCE_OK_N (R44 100kΩ pull-up), Y=U15.4 → Y = GPIO45 AND NOT(FUSB303_SOURCE_OK_N), R45 100kΩ pull-down on Y)
- USB2 role strap as fabricated: U17.3 (`PORT/DEBUG_N`) is **floating**, because R41 and R42 are both DNP. FUSB303B Table 1: "Float = FUSB303B as a Dual Role Port (DRP)". So Type-C2 comes up as DRP, not Sink — reconcile this against the Source-before-boost and 500 mA-only policy during `typec_test`. R46 909kΩ → GND sets the address strap and matches the datasheet's ~900kΩ standby-current recommendation. `EN_N` (U17.11) rests at 3.3 V through R43 100kΩ in parallel with the device's internal ~6MΩ pull-up to VDD; onsemi FUSB303B **Rev.3** Table 4 rates these pins −0.5…6.0 V, superseding the erroneous 2.0 V `EN_N` row printed in Rev.2 (the copy kept in `其他IC资料/FUSB303B.pdf`)
- U26 EXT GH1.25-4: 1=GND 2=3V3_EXT_SW 3=EXT_I2C_SDA 4=EXT_I2C_SCL (U27 = M2 mounting hole)
- CN1 battery: 1=TG28_VBAT+ 2=GND; CN2 speaker: 1/2=U22.8/U22.5 (NS4150B differential output, neither side may be grounded)
- Keys: SW1=KEY_PWRON_N (TG28.30); SW2=ESP32S31_BOOT (GPIO61); SW3 RESET→R101 (510Ω)→KEY_RST_N (TG28.29 PWROK)

## DNP positions as fabricated (footprint present, part not fitted)

The PCB_3 pad netlist above contains **every footprint, including unfitted
ones**, so on its own it expresses design intent rather than the fitted
circuit. Assembly state comes from the schematic BOM flag (`addIntoBom`); the
calibration pair is `R63`/`R68` fitted against `R62`/`R65` unfitted. Eleven
positions are unfitted on `v0.5_260803_1544`:

| Ref | Page | Footprint | Value | Net path | Consequence when unfitted |
|---|---|---|---|---|---|
| R9 | esp32-s31 | R0201 | 10kΩ | GPIO36 → GND | **Required empty.** Leaves R6 as the only strap resistor, so GPIO36 = 3.3 V. Fitted, the strap would divide to ~1.65 V and be indeterminate |
| R24 | esp32-s31 | R0201 | 0Ω | VCC_3V3_MAIN → ESP32S31_VDD_SPI | **Correct by design.** VDD_SPI is an output fed internally from VDD3P3 through RSPI ≈ 3Ω; an external link bypasses the internal power switch |
| R22 | esp32-s31 | R0201 | 100kΩ | ESP32S31_VDD_SPI → FLASH_CS | **Correct.** Pad 36 SPICS is WPU at reset and WPU,IE after reset; R22 pulls to VDD_SPI itself, so it adds nothing |
| R1 | esp32-s31 | R0201 | 0Ω | IPEX RF1 branch | Antenna select: the ceramic U3 branch (R2) is fitted, the IPEX branch is isolated |
| L1 | esp32-s31 | L0201-RD | 2nH | RF shunt → GND | Shunt leg open. The series path still conducts, so the antenna radiates but the match is **untuned** — verify by VNA against S11 ≈ 40+j0, S21 < −35 dB @ 4.8/7.2 GHz |
| C74 | display | C0603 | 22µF | LCD_VBAT → GND | LCD_VBAT bulk falls from 32.1µF to 10.1µF (−69%). Less inrush margin on first panel light-up; keep the `display_brightness 30` first step |
| R62 | audio | R0201 | 100kΩ | ES8389_AD0 → AUDIO_3V3_SW | Intended: R63 pull-down decides AD0 = 0 |
| R65 | audio | R0201 | 100kΩ | ES8389_AD1 → AUDIO_3V3_SW | Intended: R68 pull-down decides AD1 = 0 |
| R41 | usb-c2-otg | R0201 | 10kΩ | U17.3 → VCC_3V3_MAIN | See R42 |
| R42 | usb-c2-otg | R0201 | 10kΩ | U17.3 → GND | Together with R41 unfitted, U17.3 floats → FUSB303B resolves as **DRP** (Table 1). Confirm during `typec_test` |
| C36 | power | C0603 | 22µF | — | No footprint in PCB_3 (`addIntoPcb=false`): schematic-only residue, no electrical effect |

Incoming inspection must confirm the ten PCB-present positions are genuinely
empty. A wrongly fitted `R9` is the one that blocks boot: GPIO36 would sit near
1.65 V, and erratum SPI-855 forbids booting a v0.0 die with 1.8 V VDD_SPI.

## Post-fabrication reworks

These positions were **fitted at fabrication** and later changed by hand, so they
are not part of the DNP table above. Board-level results recorded after the date
shown reflect the reworked state.

| Ref | Date | Change | Reason | Evidence | DVT intent |
|---|---|---|---|---|---|
| `R95` (0Ω 0402, camera page) | 2026-08-19 | **Removed** — cuts `CAM_AVDD_2V8_SW` (ALDO4 2.8 V) → `CAM_AF_VCC` → FPC1.23 | FPC1 is wired to the OV5640-AF family convention (23=AF-VCC, 24=AF-GND), but the module in use was an ESP32-S31-Korvo-1 OV3660 whose pin 23=LED+ and pin 24=IR_CUT. Mating the two energised the module's IR-CUT coil continuously (tens of ohms), overloading ALDO4; combined with a 100 % display load and camera-init transients this dropped DCDC1 below its 85 % UVP threshold and TG28 powered the whole board off | Same stress case that failed 4 times in a row passed **20/20** after removal (`.work/evt1/s7_tg28_r95_removed.log`, `s7_tg28_r95_removed_2.log`); PoR baseline REG20=0x04 / REG21=0x00. Korvo pin mapping from `esp32s31原厂官方资料/esp32-s31-korvo-1-schematics.pdf` p3 (J4 FPC_24P_CAM) | Keep **DNP** while an OV3660-class module is used. If IR-CUT switching or a real AF module must be supported, pins 23/24 need a configurable driver (0Ω option + GPIO/load switch) — decide at DVT review. A genuine OV5640-AF module needs R95 refitted, and that configuration must re-run the "display 100 % + camera_test ×10" stress before being trusted |
| `R95` | 2026-08-20 | **Refitted by hand**; current board state supplies `CAM_AF_VCC` from ALDO4 when camera power is enabled | The planned first OV5640-AF validation stage required R95 to remain removed; the operator confirmed it had been refitted only after the collapse | `camera_test` hung with no sensor log, the board fell to about 4 mA, PWRON recovered it, and the recovered TG28 state was REG20=0x01 / REG21=0x20 (`.work/evt1/s8_ov5640_camera_collapse_pmic.log`) | Do not repeat camera tests with R95 fitted until the AF load is quantified and a controlled power plan is approved. Remove R95 again for the sensor-only bring-up stage |
| FPC1 mating | 2026-08-20 | **Camera FPC tail confirmed mirrored (pin 1 ↔ pin 24) against FPC1** — user physical inspection; module removed, camera tests paused pending a pin-order adapter FPC cable/board | Mirrored mating maps board pin 10 (`CAM_DVDD_1V5_SW`) to module DGND and board pin 23 (`CAM_AF_VCC`, R95 fitted) to module AGND — two hard rail shorts; sensor AVDD/DVDD/DOVDD all land on GND or SoC GPIOs and SCCB SDA/SCL land on module D2/D3, so detection is physically silent | Explains the 2026-08-20 03:39 collapse exactly (zero sensor logs + ~4 mA + REG21=0x20); full derivation and adapter verification in `.work/evt1/20260820_fpc_mirror_adapter_verify.md`; in-project `OV5640_FPC转换板` schematic verified as an exact 24-position mirror (FPC1.pinN ↔ FPC2.pin(25−N), 24/24) via EasyEDA live read | DVT: fix the camera connector pin order (footprint/silkscreen) so no adapter is needed in production; the interconnect FPC between mainboard and adapter must be same-face/straight (a reverse-face cable re-mirrors the chain); R95 rule reverts to the pinout-check plan once orientation is corrected |

## Expected I2C addresses

All 7-bit. Final adjudication at EVT is the `i2c_scan` measurement.

| Bus | Address | Device | Strap / wiring evidence |
|---|---|---|---|
| Main (FUSB303_SDA/SCL, pull-ups R4/R5 2.2kΩ → VCC_3V3_MAIN) | 0x21 | FUSB303B (U17) | ADDR pin low via R46 909kΩ → GND (datasheet: LOW = address 42h 8-bit = 0x21; ~900kΩ recommended to cut standby current). A 0x31 answer means an assembly/strap anomaly |
| Main, via Q3 (2N7002DW, gates → AUDIO_3V3_SW) | 0x10 | ES8389 (U19) | AD0: R63 100kΩ pull-down fitted, R62 100kΩ pull-up DNP; AD1: R68 100kΩ pull-down fitted, R65 100kΩ pull-up DNP (R61 0Ω ties AD1 to GPIO44, sampled while ALDO3 rises). 0x20 is the 8-bit write address that esp_codec_dev/BSP use — on the wire it is 0x10. Segment pull-ups R73/R74 4.7kΩ → AUDIO_3V3_SW |
| Main, via Q2 (gates → LCD_CTP_3V3_SW) | 0x15 | CST820 touch (U14) | Segment pull-ups R56/R57 4.7kΩ → LCD_CTP_3V3_SW; visible on main bus only while ALDO2 is on |
| Main, via Q4 (gates → CAM_DOVDD_2V8_SW) | 0x3C | OV5640 SCCB | Segment pull-ups R96/R97 4.7kΩ → CAM_DOVDD_2V8_SW |
| LP (TG28_SDA/SCL via R26/R27 0Ω → GPIO6/7) | 0x32 | RX8130CE RTC (U1) | Fixed device address |
| LP | 0x34 | TG28 PMIC (U6) | Fixed device address |

EXT segment (U26) is isolated by Q6 (gates → 3V3_EXT_SW) with pull-ups R103/R104 4.7kΩ → 3V3_EXT_SW; address depends on the attached peripheral.
