#!/usr/bin/env bash
# Все тесты без Windows и без Porthole:
#   1) тексты и метаданные (tools/lint.py)
#   2) логика определения мира без Windows (tests/resolve_test.c), копии и zip (tests/backup_test.py)
#      и протокол агента на Linux-сборке хелпера (tests/agent_test.py)
#   3) Luau: юнит-тесты модулей и сценарии против поддельного porthole (нужен luau, путь в $LUAU)
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
echo "== lint"
python3 tools/lint.py

mapfile -t SRC < <(ls native/*.c | LC_ALL=C sort)
MZDEFS=(-DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES)
gcc -c -O2 -w "${MZDEFS[@]}" -o build/miniz.o native/third_party/miniz/miniz.c
gcc -DTEST_MAIN -O2 -Wall -Wextra -Werror "${MZDEFS[@]}" -o build/wb_test "${SRC[@]}" build/miniz.o -lpthread

echo "== resolve logic"
gcc -Inative -O2 -Wall -Wextra -Werror "${MZDEFS[@]}" -o build/resolve_test tests/resolve_test.c native/resolve.c native/main.c native/backup.c build/miniz.o -lpthread
./build/resolve_test

echo "== backup (FAST=1 skips the slow ZIP64 part)"
timeout -k 2 600 python3 -u tests/backup_test.py build/wb_test ${FAST:+--fast} | grep -v '^PASS'

echo "== agent protocol"
timeout -k 2 60 python3 -u tests/agent_test.py build/wb_test | grep -v '^PASS'

echo "== luau"
LUAU=${LUAU:-luau}
if ! command -v "$LUAU" >/dev/null; then
  echo "luau not found: install it or set LUAU=/path/to/luau" >&2
  exit 1
fi
LUAU="$LUAU" bash tests/run_luau.sh | grep -v '^PASS'
echo "ALL TESTS PASSED"
