# Candis-S31 TF-Card AVI Player

Full-screen MJPEG/AVI player demo (`main/player_main.c`), built on the
Espressif player stack: `espressif/esp_player`, the GMF framework
(`gmf_core`, `gmf_audio`, `gmf_video`, `media_lib_sal`), and the
`esp_audio_render` / `esp_video_codec` / `esp_video_render` /
`esp_lvgl_port` components (see `main/idf_component.yml`; all resolve from
the ESP Component Registry on first build).

Status: compile-tested with ESP-IDF v6.1-rc1 (`--preview`, repository board
component) and started on EVT1 on 2026-09-10. The display, the audio render
device, the explicit MJPEG decode pipeline and the TF mount all come up, and
the media scan reports the files found on the card. Selecting a file and
watching it play is a manual step and has not been recorded yet.

## Behavior

- On boot the board and display come up, the playback surface attaches to
  the active LVGL screen, and the file list follows the TF card.
- When the TF card mounts, the 460x460 @ 30 fps MJPEG/PCM AVI from
  `main/assets` is installed onto the card automatically
  (`storage_install_reference_media()`), so a blank card becomes playable
  without a host copy step.
- The browser lists `.avi` files only. The previously bundled H.264/AAC MP4
  is deliberately removed during migration: the current ESP32-S31 extractor
  returned no video codec for that file on hardware, so exposing it produced
  a deterministic playback failure.
- Short-press the power key to show the on-screen overlay; long-press
  BOOT to stop playback (`on_input` in `main/player_main.c`).

## Real playback acceptance

On EVT1 (2026-09-10), a temporary build-only probe used the existing
`video_player_play()` API to play this actual TF-card file at 1.0× without
looping. Hardware decoding/playback reached `reason=eof` and a 100% finish
callback in 5.603 s, with no playback/audio/decode errors. The probe is not
part of the normal project; this verifies the media path, not touch selection
or visual/listening quality.

Use the existing touch browser: after `scanned ... media file(s)`, tap
`Candis_demo_460x460_30fps.avi`. This bundled file is **5.033333 s**, 460×460
MJPEG at 30 fps with 16 kHz stereo PCM (metadata checked with `ffprobe`).
Leave loop disabled. Capture at least **10 s after selection**
and require `playback started: /sdcard/Candis_demo_460x460_30fps.avi` followed
by `playback ended: ... reason=eof`, with no playback/audio/decode errors.
`reason=stopped` verifies a stop request, not whole-file playback.

The real control path is `event_file_clicked()` → `start_playback_locked()` →
`video_player_play()`; pause, stop, seek, loop and volume remain the existing
overlay controls (the decode pipeline runs at the file's own rate). There is **no serial play command or auto-play on
mount**. A mounted card and a ready pipeline are not playback acceptance.
Selection needs a working touch panel and an operator or physical touch
fixture; video smoothness/orientation and speaker output still need visual
and listening evidence. BOOT long-press only stops; it cannot select a file.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/player -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/player -D IDF_TARGET=esp32s31 flash monitor
```

The board runtime is injected from the repository-owned
`components/candis_s31/` component; its manifest maps the four reusable
drivers to `vendor/idf-extra-components/`. The committed `dependencies.lock`
pins the player-stack versions and hashes; do not edit it by hand. A fresh
build still needs network access to download the pinned public Registry
packages.
