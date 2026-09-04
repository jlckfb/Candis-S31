#!/usr/bin/env bash
# pack_factory_release.sh — stage a Candis-S31 Factory release package locally.
#
# This is a packaging dry-run helper: it never publishes anything. It fills
# tools/release/manifest.template.yaml and the Recovery Launchpad template
# with build facts, copies the merged image, its SHA-256, and the generated
# dependency lock into a staging directory, and zips the result. GitHub
# Release publication stays a manual step performed only after hardware
# validation.
#
# Usage:
#   tools/release/pack_factory_release.sh [FACTORY_DIR] [OUT_DIR]
#
#   FACTORY_DIR  factory project (default: <repo>/firmware/factory); must
#                already contain build/candis_s31_factory_merged.bin (see the
#                factory README `idf.py merge-bin` step).
#   OUT_DIR      staging directory (default: <repo>/firmware/factory/release,
#                the git-ignored local staging area). The zip is written to
#                <OUT_DIR>.zip.
#
# Required env: RELEASE_NAME (the exact GitHub release tag, or a concrete CI
# dry-run identifier). Optional: BOARD_REV, IDF_PATH.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)

FACTORY_DIR=${1:-$REPO_ROOT/firmware/factory}
OUT_DIR=${2:-$REPO_ROOT/firmware/factory/release}
TEMPLATE=$SCRIPT_DIR/manifest.template.yaml
MERGED=candis_s31_factory_merged.bin
LAUNCHPAD_TEMPLATE=$REPO_ROOT/firmware/recovery/launchpad.template.toml

RELEASE_NAME=${RELEASE_NAME:-}
BOARD_REV=${BOARD_REV:-EVT1 (schematic v0.5, fab v0.5_260803_1544)}
IDF_PATH=${IDF_PATH:-}

die() { echo "pack_factory_release: ERROR: $*" >&2; exit 2; }

[ -f "$FACTORY_DIR/build/$MERGED" ] || die "missing $FACTORY_DIR/build/$MERGED — run 'idf.py --preview merge-bin --output $MERGED --format raw' first"
[ -f "$TEMPLATE" ] || die "missing template $TEMPLATE"
[ -n "$RELEASE_NAME" ] || die "RELEASE_NAME must be the exact release tag (or a concrete CI dry-run identifier)"
[[ "$RELEASE_NAME" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] ||
    die "RELEASE_NAME must contain only letters, digits, dot, underscore, or hyphen"
[ -f "$LAUNCHPAD_TEMPLATE" ] || die "missing template $LAUNCHPAD_TEMPLATE"
command -v git >/dev/null || die "git not found"

# Canonicalize OUT_DIR: the zip step runs inside it, so a relative path would
# otherwise resolve the package name against the wrong directory.
mkdir -p "$OUT_DIR"
OUT_DIR=$(cd -- "$OUT_DIR" && pwd)
case "$OUT_DIR" in
    /|"$HOME") die "refusing to use $OUT_DIR as the staging directory" ;;
esac
# The staging directory belongs to this script: drop leftovers from previous
# runs so the package contains exactly this run's payload.
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
cp "$REPO_ROOT/firmware/recovery/flash_factory.sh" \
   "$OUT_DIR/flash_factory.sh"
cp "$REPO_ROOT/firmware/recovery/README.md" "$OUT_DIR/RECOVERY.md"
cp "$REPO_ROOT/firmware/recovery/enter_download_mode.md" \
   "$OUT_DIR/enter_download_mode.md"
chmod +x "$OUT_DIR/flash_factory.sh"
cp "$FACTORY_DIR/build/$MERGED" "$OUT_DIR/$MERGED"

git_field() { # git_field <dir> <rev-parse-arg...>
    git -C "$1" rev-parse "$2" 2>/dev/null || echo "<unknown>"
}
git_dirty() { # git_dirty <dir> -> true|false
    if [ -n "$(git -C "$1" status --porcelain 2>/dev/null)" ]; then echo true; else echo false; fi
}

APP_COMMIT=$(git_field "$REPO_ROOT" HEAD)
APP_DIRTY=$(git_dirty "$REPO_ROOT")
if [ -n "$IDF_PATH" ] && [ -d "$IDF_PATH" ]; then
    IDF_VERSION=$(git -C "$IDF_PATH" describe --tags --always 2>/dev/null || echo "<unknown>")
    IDF_COMMIT=$(git_field "$IDF_PATH" HEAD)
else
    IDF_VERSION="<unknown: set IDF_PATH>"
    IDF_COMMIT="<unknown: set IDF_PATH>"
fi

BIN_SHA256=$(sha256sum "$OUT_DIR/$MERGED" | cut -d' ' -f1)
printf '%s  %s\n' "$BIN_SHA256" "$MERGED" > "$OUT_DIR/$MERGED.sha256"

if [ -f "$FACTORY_DIR/dependencies.lock" ]; then
    cp "$FACTORY_DIR/dependencies.lock" "$OUT_DIR/dependencies.lock"
    LOCK_SHA256=$(sha256sum "$OUT_DIR/dependencies.lock" | cut -d' ' -f1)
else
    LOCK_SHA256="<no dependencies.lock — regenerate with idf.py reconfigure>"
fi

DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)

sed -e "s|@RELEASE_NAME@|$RELEASE_NAME|g" \
    -e "s|@BOARD_REV@|$BOARD_REV|g" \
    -e "s|@DATE@|$DATE|g" \
    -e "s|@BIN_SHA256@|$BIN_SHA256|g" \
    -e "s|@APP_COMMIT@|$APP_COMMIT|g" \
    -e "s|@APP_DIRTY@|$APP_DIRTY|g" \
    -e "s|@IDF_VERSION@|$IDF_VERSION|g" \
    -e "s|@IDF_COMMIT@|$IDF_COMMIT|g" \
    -e "s|@LOCK_SHA256@|$LOCK_SHA256|g" \
    "$TEMPLATE" > "$OUT_DIR/manifest.yaml"
sed -e "s|@RELEASE_NAME@|$RELEASE_NAME|g" \
    -e "s|@BIN_SHA256@|$BIN_SHA256|g" \
    "$LAUNCHPAD_TEMPLATE" > "$OUT_DIR/launchpad.toml"

if grep -Eq '@(RELEASE_NAME|BIN_SHA256)@|<tag>|<sha256>' \
        "$OUT_DIR/launchpad.toml"; then
    die "generated launchpad.toml still contains a release placeholder"
fi

if command -v zip >/dev/null; then
    PKG=${OUT_DIR%/}.zip
    rm -f "$PKG"
    ( cd "$OUT_DIR" && zip -r "$PKG" . >/dev/null )
else
    # Minimal environments may lack zip; tar.gz carries the same payload.
    PKG=${OUT_DIR%/}.tar.gz
    rm -f "$PKG"
    tar -czf "$PKG" -C "$OUT_DIR" .
fi

echo "pack_factory_release: staged in $OUT_DIR"
echo "pack_factory_release: package $PKG"
echo "pack_factory_release: sha256($MERGED) = $BIN_SHA256"
