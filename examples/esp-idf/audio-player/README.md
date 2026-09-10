# ESP-IDF audio player

Plays one second of a program-generated 1 kHz sine tone through the speaker
(no asset files needed), then tries to play `/sdcard/example_record.wav`
from the TF card. A missing file prints a skip line before teardown. Only
PCM16 WAV (1-2 channels, 8/16/44.1/48 kHz) is accepted, matching the ES8389
BCLK policy.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, QIO 80 MHz, 32 MB PSRAM octal 250 MHz |
| Partition table | `partitions.csv` (single 8 MB app) |
| Managed components | `espressif/esp_codec_dev` 1.6.2 |
| Compile status | Verified with ESP-IDF v6.1-rc1 from fresh defaults |

## Behavior

- Generates a 1 kHz sine for one second at 16 kHz stereo and writes it to
  the codec at volume 60.
- Then opens `/sdcard/example_record.wav` if present, re-opens the speaker
  with the file's sample rate/channels through `bsp_audio_codec_open`, and
  streams the entire data chunk.
- Prints `playback done` only after successful playback and teardown. Missing
  WAV files or unavailable TF storage are explicit skips; short reads, codec
  errors and teardown errors report `playback failed`. No idle heartbeat follows.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/audio-player -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/audio-player -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/audio-player -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
I (…) audio_player: sine: played 1 s of 1 kHz at 16000 Hz
I (…) audio_player: wav: played 320000/320000 bytes of /sdcard/example_record.wav (16000 Hz, 2 ch)
I (…) audio_player: playback done
```

Without the recording on the card:

```text
I (…) audio_player: skip: /sdcard/example_record.wav not found (record one with audio-recorder)
```

## Constraints

- The speaker is driven at the board's default route; no equalizer or
  DSP processing is applied here.
- 8/16/44.1/48 kHz sample rates follow the ES8389 BCLK policy; other rates
  are rejected with a `skip:` line.
