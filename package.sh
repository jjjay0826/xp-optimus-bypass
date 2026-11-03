#!/bin/sh
# Reproducible GUI installer release, built from repository sources.
set -eu
VER=${1:?usage: sh package.sh <version>}
case "$VER" in ''|*[!A-Za-z0-9._-]*) echo 'Invalid version' >&2; exit 1;; esac
OUT="$PWD/XP_Optimus_Bypass_v${VER}_installer-r1.zip"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
D="$TMP/XP_Optimus_Bypass"
mkdir -p "$D/app" "$D/docs"
for name in XP_Optimus_Bypass.bat README.txt LICENSE CHANGELOG.md; do
    cp "installer/$name" "$D/$name"
done
for name in bootstrap.ps1 core.ps1 manager.ps1 discovery.ps1 launch.ps1 i18n.ps1 i18n.json log-viewer.ps1; do
    cp "installer/app/$name" "$D/app/$name"
done
cp installer/docs/*.md "$D/docs/"
for name in xp_optimus_bypass.dll xp_optimus_bypass.ini xp_optimus_bypass.json xp_optimus_bypass_global.json; do
    cp "dist/$name" "$D/app/$name"
done
# Do not ship a placeholder log: the DLL creates the actual installed-root log.
(cd "$D" && find . -type f | LC_ALL=C sort | while IFS= read -r file; do sha256sum "$file"; done) > "$TMP/checksums"
mv "$TMP/checksums" "$D/SHA256SUMS.txt"
EPOCH=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct)}
find "$TMP" -exec touch -h -d "@$EPOCH" {} +
rm -f "$OUT"
(cd "$TMP" && find XP_Optimus_Bypass -type f | LC_ALL=C sort | TZ=UTC zip -X -q "$OUT" -@)
sha256sum "$OUT" dist/xp_optimus_bypass.dll
