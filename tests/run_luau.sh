#!/usr/bin/env bash
# Luau-тесты: юнит-тесты модулей и сценарии против поддельного porthole.
set -euo pipefail
cd "$(dirname "$0")/.."
LUAU=${LUAU:-luau}
mkdir -p build
python3 tests/gen_env.py build/env.luau russian
python3 tools/bundle.py --lib build/lib.luau >/dev/null
python3 tools/bundle.py build/main.luau >/dev/null
echo "== unit"
{ cat build/env.luau tests/mock_porthole.luau; echo "__req = (function()"; cat build/lib.luau; echo "end)()"; cat tests/unit.luau; } > build/t_unit.luau
"$LUAU" build/t_unit.luau
for s in tests/scenario_*.luau; do
  echo "== $s"
  { cat build/env.luau tests/mock_porthole.luau build/main.luau "$s"; } > build/t_scn.luau
  "$LUAU" build/t_scn.luau
done
