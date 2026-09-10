# ESP-IDF PMIC

Read-only dump of the TG28_SW power-management IC: chip status, battery and
charger readbacks, regulator rails, OTP load switches, and the SAR ADC
channels. Finishes with a single off→on→restore toggle of the
`BSP_POWER_AUDIO_PA` power domain.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, DIO 40 MHz |
| Managed components | None direct; public registry dependencies via the repository board component |
| Compile status | Verified |

## Behavior

- Prints chip ID, battery voltage/presence, SOC (only meaningful when the
  fuel-gauge model is valid), VBUS/charging flags, power-on source, charge
  current, input current limit, and charge voltage.
- Prints every adjustable rail with its enabled state and millivolt
  readback, plus the two OTP switches (DC1SW feeds the RGB LED, DC4SW is a
  spare). DLDO1/DLDO2 are OTP-strapped load switches on this board and are
  skipped as rails.
- Prints the five SAR ADC channels (VBAT, TS, VBUS, VSYS, TDIE).
- Toggles `BSP_POWER_AUDIO_PA` off → on → back to its original state,
  logging each step.
- Capture **10 s** from reset. Expect status/charger, all rail/switch and ADC
  readbacks, then `power domain ...: off` → `on` → `restored=<original>`, with
  a live battery and VBUS connected for their values. The application then
  ends without a heartbeat.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/pmic -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/pmic -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/pmic -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
I (…) pmic: status: id=0x47 vbat=4102mV soc=87%(ref) bat=yes vbus=yes charging=yes done=no
I (…) pmic: charger: src=0x08 current=500mA input_limit=500mA voltage=4200mV
I (…) pmic: rail ALDO1: on  3300mV
…
I (…) pmic: rails: 6/9 enabled
I (…) pmic: adc VBAT: 4101mV
…
I (…) pmic: power domain AUDIO_PA: original=off
I (…) pmic: power domain AUDIO_PA: off
I (…) pmic: power domain AUDIO_PA: on
I (…) pmic: power domain AUDIO_PA: restored=off
```

## Constraints

- Safety contract: this example never writes a charge register (no
  `bsp_pmic_set_charge_current`, no `bsp_pmic_set_input_current_limit`, no
  `bsp_pmic_set_charge_voltage`) and never calls `bsp_pmic_power_off`. The
  charge policy stays inside the BSP.
- The example uses the IDF default partition table; no TF card, display, or
  PSRAM is required.
- SOC percent is only meaningful when the fuel-gauge model is valid; the
  example never derives a percentage from the battery voltage.
