#!/bin/sh
# Builds the release zip from dist/ and the docs.  Usage: sh package.sh 0.9.1.1-beta
set -e
VER=${1:?usage: sh package.sh <version>}
OUT="$PWD/XP_Optimus_Bypass_v$VER.zip"
TMP=$(mktemp -d)
D="$TMP/XP_Optimus_Bypass"
mkdir -p "$D"
cp dist/xp_optimus_bypass.dll dist/xp_optimus_bypass.json dist/xp_optimus_bypass_global.json dist/xp_optimus_bypass.ini \
   dist/install.bat dist/uninstall.bat dist/start_xplane.bat README.md README.zh-TW.md BENCHMARK.md CHANGELOG.md LICENSE "$D/"
(cd "$D" && sha256sum xp_optimus_bypass.dll xp_optimus_bypass.json xp_optimus_bypass_global.json \
   install.bat uninstall.bat start_xplane.bat | sed 's/$/\r/' > SHA256SUMS.txt)
# fixed timestamps and file order, so the zip is reproducible too
EPOCH=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct)}
find "$TMP" -exec touch -h -d "@$EPOCH" {} +
rm -f "$OUT"
(cd "$TMP" && find XP_Optimus_Bypass -type f | LC_ALL=C sort | TZ=UTC zip -X -q "$OUT" -@)
rm -rf "$TMP"
sha256sum "$OUT" dist/xp_optimus_bypass.dll
