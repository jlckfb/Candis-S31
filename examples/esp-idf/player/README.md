# Candis-S31 TF-Card AVI Player

Full-screen MJPEG/AVI player demo (`main/player_main.c`), built on the
Espressif player stack: `espressif/esp_player`, the GMF framework
(`gmf_core`, `gmf_audio`, `gmf_video`, `media_lib_sal`), and the
`esp_audio_render` / `esp_video_codec` / `esp_video_render` /
`esp_lvgl_port` components (see `main/idf_component.yml`; all resolve from
the ESP Component Registry on first build).

Builds with ESP-IDF v6.1-rc1 (`--preview`, repository board component) and
runs on EVT1: the display, the audio render device, the explicit MJPEG decode
pipeline and the TF mount come up, and the media scan lists the files found on
the card.

## Behavior

- On boot the board and display come up, the playback surface attaches to
  the active LVGL screen, and the file list follows the TF card.
- When the TF card mounts, the 460x460 @ 30 fps MJPEG/PCM AVI from
  `main/assets` is installed onto the card automatically
  (`storage_install_reference_media()`), so a blank card becomes playable
  without a host copy step.
- The browser lists `.avi` files only.
- Short-press the power key to show the on-screen overlay; long-press
  BOOT to stop playback (`on_input` in `main/player_main.c`).

## Playback acceptance

Use the touch browser: after `scanned ... media file(s)`, tap
`Candis_demo_460x460_30fps.avi`. This bundled file is **5.033333 s**, 460×460
MJPEG at 30 fps with 16 kHz stereo PCM (metadata checked with `ffprobe`).
Leave loop disabled. Capture at least **10 s after selection**
and require `playback started: /sdcard/Candis_demo_460x460_30fps.avi` followed
by `playback ended: ... reason=eof`.

The control path is `event_file_clicked()` → `start_playback_locked()` →
`video_player_play()`; pause, stop, seek, loop and volume are the overlay
controls (the decode pipeline runs at the file's own rate). Playback starts
from the on-screen browser: there is no serial play command and no auto-play
on mount. BOOT long-press stops playback; selecting a file needs the touch
panel.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/player -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/player -D IDF_TARGET=esp32s31 flash monitor
```

The board runtime is injected from the repository-owned
`components/candis_s31/` component; its manifest maps the four reusable
drivers to `vendor/idf-extra-components/`. The committed `dependencies.lock`
pins the player-stack versions and hashes; do not edit it by hand. A fresh
build downloads the pinned public Registry packages.
