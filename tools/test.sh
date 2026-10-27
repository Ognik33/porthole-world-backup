#!/usr/bin/env bash
# Все тесты без Windows и без Porthole:
#   1) тексты и метаданные (tools/lint.py)
#   2) логика определения мира без Windows (tests/resolve_test.c)
#      и протокол агента на Linux-сборке хелпера (tests/agent_test.py)
#   3) Luau: юнит-тесты модулей и сценарии против поддельного porthole (нужен luau, путь в $LUAU)
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
echo "== lint"
python3 tools/lint.py

mapfile -t SRC < <(ls native/*.c | LC_ALL=C sort)
echo "== resolve logic"
gcc -Inative -O2 -Wall -Wextra -Werror -o build/resolve_test tests/resolve_test.c native/resolve.c native/main.c -lpthread
./build/resolve_test

echo "== agent protocol"
gcc -DTEST_MAIN -O2 -Wall -Wextra -Werror -o build/wb_test "${SRC[@]}" -lpthread
timeout -k 2 60 python3 -u tests/agent_test.py build/wb_test | grep -v '^PASS'

echo "== luau"
LUAU=${LUAU:-luau}
if ! command -v "$LUAU" >/dev/null; then
  echo "luau not found: install it or set LUAU=/path/to/luau" >&2
  exit 1
fi
LUAU="$LUAU" bash tests/run_luau.sh | grep -v '^PASS'
echo "ALL TESTS PASSED"
