# Candis-S31 TF-Card AVI Player

Full-screen MJPEG/AVI player demo (`main/player_main.c`), built on the
Espressif player stack: `espressif/esp_player`, the GMF framework
(`gmf_core`, `gmf_audio`, `gmf_video`, `media_lib_sal`), and the
`esp_audio_render` / `esp_video_codec` / `esp_video_render` /
`esp_lvgl_port` components (see `main/idf_component.yml`; all resolve from
the ESP Component Registry on first build).

Status: compile-tested with ESP-IDF v6.1-rc1 (`--preview`, vendored BSP).
No on-board playback session has been recorded yet — treat the runtime
path as unverified until the EVT1 board confirms it.

## Behavior

- On boot the board and display come up, the playback surface attaches to
  the active LVGL screen, and the file list follows the TF card.
- When the TF card mounts, the reference media from `main/assets`
  (460x460 @ 30 fps AVI clips, plus an MP4 and the Noto Sans SC subtitle
  font under OFL-1.1) is installed onto the card automatically
  (`storage_install_reference_media()`), so a blank card becomes playable
  without a host copy step.
- Short-press the power key to toggle the on-screen overlay; long-press
  BOOT to stop playback (`on_input` in `main/player_main.c`).

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/player -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/player -D IDF_TARGET=esp32s31 flash monitor
```

The BSP resolves to the vendored snapshot (`vendor/esp-bsp`) by default; set
`CANDIS_S31_BSP_PATH` to develop against a live esp-bsp checkout. First
builds download the player-stack dependencies from the ESP Component
Registry; no lock file is committed.
