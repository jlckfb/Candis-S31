# Candis-S31 UI font generation

The firmware ships generated 20 px and 24 px LVGL bitmap subsets for readable
Chinese UI on the 2-inch 460×460 panel. The existing LVGL Source Han Sans SC
16 font remains each generated font's fallback for dynamic text.

The converter is pinned to upstream commit
`c968ed42ebbba96b5bbfbc4ebbc60d1c51a79c45`, which includes the LVGL 9.3+
`static_bitmap` fix. The source OTF is the copy shipped by LVGL 9.5 and is
verified before generation with SHA-256
`1ee89e1669362dee13851129c0a8a791a87521eb4148e5efbf5d26596738e25b`.

Regenerate from this directory:

```sh
npm ci
bash generate.sh
```

Normal ESP-IDF builds consume the committed C outputs and do not require
Node.js. When static UI copy changes, update the character lists, regenerate,
build against the locked LVGL version, and inspect both image-size delta and
missing-glyph behavior.

`generate.sh` ends by running `check-coverage.sh`, which fails when any
non-ASCII character in a `firmware/demo/main` string literal is missing from
either fallback chain — candis_ui_20 + built-in 16 px fallback, or
candis_ui_24 + the same fallback — because each subset renders its own size.
Run it standalone after editing UI copy even when the fonts did not change.

The derived font names intentionally avoid Adobe's reserved `Source` name.
See `LICENSE-OFL-1.1.txt` for the font license and required notice.
