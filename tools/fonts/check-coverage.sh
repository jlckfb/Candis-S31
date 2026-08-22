#!/usr/bin/env bash
# Font coverage gate for the Candis-S31 demo UI.
#
# Fails (exit 1) when any non-ASCII character used in a user-visible string
# literal under firmware/demo/main is missing from BOTH generated Candis
# subsets (candis_ui_20/24) AND the linked LVGL built-in fallback
# (lv_font_source_han_sans_sc_16_cjk). Characters covered only by the 16 px
# fallback are reported as size-mixing warnings but do not fail the gate;
# add them to chars-ui-20.txt / chars-ui-24.txt and re-run generate.sh.
#
# Runs automatically at the end of tools/fonts/generate.sh. Uses only bash
# and python3 (stdlib). No network, no npm.
set -euo pipefail

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(cd -- "$TOOLS_DIR/../.." && pwd)
DEMO_MAIN="$REPO_ROOT/firmware/demo/main"

python3 - "$DEMO_MAIN" <<'PY'
import glob, os, re, sys

demo_main = sys.argv[1]
font_dir = os.path.join(demo_main, "ui", "fonts")
candis = [os.path.join(font_dir, "candis_ui_20.c"),
          os.path.join(font_dir, "candis_ui_24.c")]
fallback = os.path.join(
    demo_main, "..", "managed_components", "lvgl__lvgl", "src", "font",
    "lv_font_source_han_sans_sc_16_cjk.c")

def die(msg):
    print("check-coverage: %s" % msg, file=sys.stderr)
    sys.exit(1)

def parse_font(path, sym):
    try:
        t = open(path, encoding="utf-8", errors="replace").read()
    except OSError as e:
        die("cannot read %s: %s" % (path, e))
    cov = set()
    for m in re.finditer(
            r"\.range_start = (\d+), \.range_length = (\d+), "
            r"\.glyph_id_start = (\d+),\s*"
            r"\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+), "
            r"\.list_length = (\d+), \.type = (\w+)", t):
        rs, rl, gis, ul, gio, ll, typ = m.groups()
        rs, rl, ll = int(rs), int(rl), int(ll)
        if ul == "NULL" and gio == "NULL" and ll == 0:
            cov.update(range(rs, rs + rl))          # FORMAT0_TINY
        elif ul != "NULL":                          # SPARSE_TINY
            mm = re.search(r"\b" + ul + r"\[\] = \{(.*?)\};", t, re.S)
            if not mm:
                die("cannot locate array %s in %s" % (ul, path))
            cov.update(rs + int(x, 16)
                       for x in re.findall(r"0x[0-9a-fA-F]+", mm.group(1)))
        elif gio != "NULL":                         # FORMAT0_FULL, ofs 0 = hole
            mm = re.search(r"\b" + gio + r"\[\] = \{(.*?)\};", t, re.S)
            if not mm:
                die("cannot locate array %s in %s" % (gio, path))
            for i, x in enumerate(re.findall(r"0x[0-9a-fA-F]+", mm.group(1))):
                if int(x, 16) != 0:
                    cov.add(rs + i)
    if not cov:
        die("parsed zero codepoints from %s (format drift?)" % path)
    print("  %-28s %5d codepoints" % (sym, len(cov)))
    return cov

def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text

print("check-coverage: parsing fonts")
c20 = parse_font(candis[0], "candis_ui_20")
c24 = parse_font(candis[1], "candis_ui_24")
fb = parse_font(fallback, "sc16_cjk (fallback)")

used = {}   # cp -> {"files": set()}
pat = re.compile(r'"((?:[^"\\]|\\.)*)"')
for path in sorted(glob.glob(demo_main + "/**/*.c", recursive=True)):
    if os.path.dirname(path).startswith(font_dir):
        continue  # generated font sources contain every glyph as a comment
    text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
    for m in pat.finditer(text):
        for ch in m.group(1):
            if ord(ch) >= 0x80:
                used.setdefault(ord(ch), set()).add(
                    os.path.relpath(path, demo_main))

# Each font is used through its own fallback chain: a character that only
# exists in the 24 px subset still tofus in body text rendered with
# candis_ui_20, and vice versa. Check both chains independently.
miss20 = sorted(cp for cp in used if cp not in c20 and cp not in fb)
miss24 = sorted(cp for cp in used if cp not in c24 and cp not in fb)

def warn_mix(subset, label):
    for cp in sorted(cp for cp in used
                     if cp in used and cp not in subset and cp in fb
                     and not (0xF000 <= cp <= 0xF8FF)):
        print("  WARN %s renders 16px-fallback U+%04X %s <- %s" %
              (label, cp, chr(cp), ", ".join(sorted(used[cp]))))

print("check-coverage: %d distinct non-ASCII chars in demo strings" % len(used))
warn_mix(c20, "body/candis_ui_20")
warn_mix(c24, "title/candis_ui_24")
for label, missing in (("candis_ui_20", miss20), ("candis_ui_24", miss24)):
    for cp in missing:
        print("  FAIL %s chain tofu U+%04X %s <- %s" %
              (label, cp, chr(cp), ", ".join(sorted(used[cp]))))
if miss20 or miss24:
    die("missing from candis_ui_20 chain: %d, from candis_ui_24 chain: %d; "
        "add the characters to tools/fonts/chars-ui-20.txt and "
        "chars-ui-24.txt, then re-run tools/fonts/generate.sh"
        % (len(miss20), len(miss24)))
print("check-coverage: PASS (both font chains complete)")
PY
