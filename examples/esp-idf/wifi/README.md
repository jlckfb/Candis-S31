# ESP-IDF WiFi

Brings the STA interface up, scans and prints the strongest APs, and — when
an SSID is configured — connects and prints the IP address.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, QIO 80 MHz, 32 MB PSRAM octal 250 MHz |
| Partition table | `partitions.csv` (single 8 MB app) |
| Managed components | None direct |
| Compile status | Verified |

## Behavior

- Scans once (blocking) and prints up to the 10 strongest APs with RSSI and
  channel.
- `CONFIG_EXAMPLE_WIFI_SSID` empty (default): scan-only mode; prints
  `no SSID configured … scan only` and ends. Capture **15 s** for the scan.
- SSID configured: connects, waits up to 20 s for `IP_EVENT_STA_GOT_IP`, and
  prints `connected, ip: <address>`; a timeout prints the disconnect reason.
- For the connection path, allow **35 s** from reset and expect
  `connected, ip: ...`. An available 2.4 GHz AP, correct credentials and DHCP
  are required. Zero visible APs is reported as `0 APs visible, 0 printed`.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/wifi -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/wifi -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/wifi -p /dev/ttyACM0 monitor
```

Set SSID/password in `idf.py --preview menuconfig` under "WiFi example"
before rebuilding. CMake `-D EXAMPLE_WIFI_SSID=...` does not set a Kconfig value.

## Expected serial output

```text
I (…) wifi: WiFi STA up
I (…) wifi: ap  1: MyNetwork                        rssi -41 ch  6
I (…) wifi: scan: 12 APs visible, 10 printed
I (…) wifi: no SSID configured (CONFIG_EXAMPLE_WIFI_SSID empty); scan only
```

## Constraints

- 2.4 GHz station mode.
- The radio shares the board antenna with BLE; run one radio example at a
  time.
