# Vendored ESP Board Manager snapshot

This directory is a **vendored mirror** of the ESP Board Manager pieces that
carry the Candis-S31 board definition, staged for submission to
[`espressif/esp-board-manager`](https://github.com/espressif/esp-board-manager):

- `esp_board_manager/` — the Board Manager component, including the YAML
  code generator (`test_apps/`, `docs/`, and `examples/` stay upstream)
- `esp_friends_boards/` — the friends-boards package with
  `esp_friends_boards/candis_s31`

Source of truth during upstream preparation:
`LeenixP/esp-board-manager`, branch `feat/candis-s31`.
There is no open submission pull request; the earlier
[`esp-board-manager#7`](https://github.com/espressif/esp-board-manager/pull/7)
was closed without maintainer acceptance. Refresh this snapshot after
board-definition changes land there:

```bash
tools/sync_upstream.sh            # or: EBM_ROOT=/path/to/esp-board-manager tools/sync_upstream.sh
```

`esp_boards` and `m5stack_boards` are official registry packages unrelated to
Candis-S31 and are not mirrored. The snapshot metadata lives in
`vendor/esp-board-manager/SOURCE_COMMIT`.

Until the four driver releases and the board definition are published on the
ESP Component Registry, Board Manager based projects consume this snapshot by
adding `vendor/esp-board-manager/esp_board_manager` and
`vendor/esp-board-manager/esp_friends_boards` as local component overrides.
