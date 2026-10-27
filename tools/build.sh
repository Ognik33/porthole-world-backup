#!/usr/bin/env bash
# Собирает dist/world-backup-<версия>.paddon и dist/world_backup_cli.exe (нужны mingw-w64, python3, zip).
# Одна DLL из всех native/*.c (новые файлы подхватываются сами) и miniz из native/third_party.
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(grep '^version' addon/world-backup/addon.toml | cut -d'"' -f2)
OUTDIR=build/world-backup
rm -rf "$OUTDIR" && mkdir -p "$OUTDIR/native/windows-x86_64" dist
mapfile -t SRC < <(ls native/*.c | LC_ALL=C sort)
# miniz (сжатие deflate): сторонний код, собирается отдельно и без наших предупреждений
MZDEFS=(-DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES)
x86_64-w64-mingw32-gcc -c -O2 -w "${MZDEFS[@]}" -o build/miniz_win.o native/third_party/miniz/miniz.c
CFLAGS=(-O2 -s -static-libgcc -Wall -Wextra "${MZDEFS[@]}" "-DWB_VERSION=\"$VERSION\"" -Wl,--no-insert-timestamp)
LIBS=(-lws2_32 -liphlpapi -lshell32)
x86_64-w64-mingw32-gcc -shared "${CFLAGS[@]}" -o "$OUTDIR/native/windows-x86_64/world_backup.dll" "${SRC[@]}" build/miniz_win.o "${LIBS[@]}"
x86_64-w64-mingw32-gcc -DCLI_MAIN "${CFLAGS[@]}" -o dist/world_backup_cli.exe "${SRC[@]}" build/miniz_win.o "${LIBS[@]}"
python3 tools/bundle.py "$OUTDIR/main.luau"
cp -r addon/world-backup/addon.toml addon/world-backup/lang "$OUTDIR/"
OUT="dist/world-backup-$VERSION.paddon"
rm -f "$OUT"
# воспроизводимый архив: одинаковое время файлов, фиксированный порядок, без доп. атрибутов
find "$OUTDIR" -exec touch -h -d '2026-01-01 00:00:00 UTC' {} +
(cd build && find world-backup | LC_ALL=C sort | zip -qX "../$OUT" -@)
echo "built $OUT"
sha256sum "$OUT" "$OUTDIR/native/windows-x86_64/world_backup.dll" dist/world_backup_cli.exe
