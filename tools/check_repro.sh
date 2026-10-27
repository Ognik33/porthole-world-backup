#!/usr/bin/env bash
# Собирает два раза с нуля и сравнивает хеши: сборка обязана быть воспроизводимой
# (Release на GitHub бит в бит совпадает с локальной сборкой тем же компилятором).
set -euo pipefail
cd "$(dirname "$0")/.."
sums() { sha256sum dist/*.paddon dist/world_backup_cli.exe | awk '{print $1}'; }
rm -rf build dist && bash tools/build.sh >/dev/null && A=$(sums)
rm -rf build dist && bash tools/build.sh >/dev/null && B=$(sums)
if [ "$A" = "$B" ]; then echo "PASS reproducible build"; else echo "FAIL build differs"; echo "$A"; echo "$B"; exit 1; fi
