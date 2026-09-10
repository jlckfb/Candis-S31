# ESP-IDF BLE (NimBLE)

Advertises as a connectable peripheral named `candis-example` for 10 s, then
actively scans for 10 s and prints up to 32 distinct peers with address,
RSSI and advertised name. A full result list does not restart logging.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, QIO 80 MHz, 32 MB PSRAM octal 250 MHz |
| Partition table | `partitions.csv` (single 8 MB app) |
| Managed components | None direct (NimBLE via ESP-IDF `bt`) |
| Compile status | Verified |
| Hardware status | **Verified for advertise/scan** — EVT1 (2026-09-10): an independent host received 100 advertisements from `30:ed:a0:f4:66:eb`; active scan completed with 32 distinct peers and no duplicate lines; connections/bonding were not tested |

## Behavior

- NimBLE host syncs, then general-connectable advertising starts with the
  complete name `candis-example`; a connecting central is logged.
- After 10 s advertising stops and an active 10 s scan begins. The first
  32 distinct peers print `peer <addr> rssi <rssi> name "<name>"`; each is
  listed once, and further reports are not printed after the list is full.
- After the real scan-complete event, the example prints
  `advertise + scan cycle done` once and ends. Capture **30 s** from reset.
  A failed advertising/scan start or missing completion event is an error,
  not a completed cycle. A nearby advertising peer is needed for receive
  evidence; an external scanner is needed to independently observe our advertising.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/ble -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/ble -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/ble -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
I (…) ble: BLE host synced
I (…) ble: advertising as "candis-example" for 10 s
I (…) ble: scanning for 10 s
I (…) ble: peer 5f:3a:11:82:09:c2 rssi -62 name "Mi Band 8"
I (…) ble: scan complete: listed 32 peers (limit 32)
I (…) ble: advertise + scan cycle done
```

Verify the advertising window with a phone scanner app (nRF Connect,
LightBlue): `candis-example` must appear for 10 s.

## Constraints

- The radio shares the board antenna with WiFi; run one radio example at a
  time.
- The example uses the public address policy (`ble_hs_id_infer_auto`); no
  bonding or persistence is configured.
