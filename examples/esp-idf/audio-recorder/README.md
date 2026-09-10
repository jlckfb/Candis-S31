# ESP-IDF audio recorder

Records five seconds from the board's ES8389-connected analog microphones to
`/sdcard/example_record.wav` (16 kHz, stereo, PCM16) and prints the captured
byte count and the peak sample level.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, QIO 80 MHz, 32 MB PSRAM octal 250 MHz |
| Partition table | `partitions.csv` (single 8 MB app) |
| Managed components | `espressif/esp_codec_dev` 1.6.2 |
| Compile status | Verified with ESP-IDF v6.1-rc1 from fresh defaults (2026-09-10) |
| Hardware status | **Partial** — EVT1 (2026-09-10): 320000 bytes (5.0 s) captured, saved and replayed after clean audio/TF teardown; listening acceptance remains separate |

## Behavior

- With no TF card, report the missing card and return. Insert a compatible
  card and reset to run again; the example does not poll for insertion.
- Opens the codec through `bsp_audio_codec_open` in the validated order:
  speaker first (shared I2S clock), speaker PA domain off during capture,
  microphone second, input gain 12 dB.
- Discards the first two pipeline-fill blocks, captures five seconds into a
  PSRAM buffer, then computes the peak absolute sample.
- Writes a PCM16 WAV header plus the capture to the card. `saved …` and
  `recording done` require successful writes, file close, audio teardown and
  TF unmount. Failure reports `recording failed` and returns without an idle heartbeat.
- Closes both codecs, releases I2S, and leaves the PA and audio rail off.
- The final-source EVT1 run saved the five-second recording and returned at
  7.423 s without I2S disable or GPIO ownership errors; `audio-player`
  subsequently consumed its full 320000-byte PCM payload.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/audio-recorder -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/audio-recorder -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/audio-recorder -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
I (…) audio_recorder: captured 320000 bytes (5.0 s), peak level 8123/32767 (24.8%FS)
I (…) audio_recorder: saved /sdcard/example_record.wav
I (…) audio_recorder: recording done
```

The WAV file is playable on a host (`aplay`, Windows Media Player, …).

## Constraints

- Requires a TF card; the recording path is always `/sdcard/example_record.wav`.
- The capture stores both analog microphone channels (no beamforming or
  denoise).
- A quiet room still yields a small nonzero peak; a silent sensor with peak 0
  suggests an I2S routing problem, not a firmware hang.
