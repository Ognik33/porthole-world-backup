#!/usr/bin/env bash
# Дымовой тест определения мира под Wine (необязательный, в CI не запускается: Wine тяжёлый).
# Поддельная «игра» tests/win/fake_game.exe держит порт и session.lock, CLI хелпера определяет мир
# по её PID. Порт → PID под Wine в контейнере не работает (Wine отдаёт PID 0), поэтому --resolve-pid.
# Проверяет: --gameDir и рабочую папку, кириллицу в путях, методы lock / mtime / ask, что токен
# из командной строки не попадает в вывод.
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
command -v wine >/dev/null || { echo "wine not found, skipping"; exit 0; }
export LANG=C.UTF-8 LC_ALL=C.UTF-8 WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-/tmp/wb-wine}
mkdir -p build
mapfile -t SRC < <(ls native/*.c | LC_ALL=C sort)
MZDEFS=(-DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES)
x86_64-w64-mingw32-gcc -c -O2 -w "${MZDEFS[@]}" -o build/miniz_win.o native/third_party/miniz/miniz.c
x86_64-w64-mingw32-gcc -DCLI_MAIN -O2 "${MZDEFS[@]}" -o build/smoke_cli.exe "${SRC[@]}" build/miniz_win.o -lws2_32 -liphlpapi -lshell32
x86_64-w64-mingw32-gcc -O2 -o build/fake_game.exe tests/win/fake_game.c -lws2_32 -lshell32
timeout 120 wineboot -i >/dev/null 2>&1
timeout 60 wine cmd /c exit >/dev/null 2>&1 # первый запуск в новом префиксе медленный

G="$WINEPREFIX/drive_c/Игры/мс"
rm -rf "$G" && mkdir -p "$G/saves/Наш мир" "$G/saves/Старый" "$G/saves/Не мир"
touch -d '2020-01-01' "$G/saves/Старый/session.lock" "$G/saves/Наш мир/session.lock"
W='C:\Игры\мс'
pass=0; fail=0
check() { if eval "$2"; then pass=$((pass+1)); echo "PASS $1"; else fail=$((fail+1)); echo "FAIL $1"; echo "$OUT"; fi; }

# запустить игру ($@) в папке $RUN_CWD (по умолчанию корень репозитория), дождаться ready,
# выполнить $AFTER, определить мир
run() {
  rm -f "$ROOT/build/fg.out"
  (cd "${RUN_CWD:-$ROOT}" && wine "$ROOT/build/fake_game.exe" "$@" > "$ROOT/build/fg.out" 2>&1) &
  for _ in $(seq 1 180); do grep -q ready "$ROOT/build/fg.out" 2>/dev/null && break; sleep 0.5; done
  local pid; pid=$(grep -o 'pid=[0-9]*' "$ROOT/build/fg.out" | cut -d= -f2)
  eval "${AFTER:-true}"
  OUT=$(timeout 60 wine build/smoke_cli.exe --resolve-pid "$pid" | tr -d '\r')
  wineserver -k; sleep 1
}

AFTER= run 51658 "$W\\saves\\Наш мир\\session.lock" --username Ognik33 --gameDir "$W" --accessToken SECRET
check "lock: world found" '[[ "$OUT" == *"method=lock"*"world=$W\\saves\\Наш мир"* ]]'
check "lock: launcher vanilla, gameDir source" '[[ "$OUT" == *"launcher=vanilla"*"source=gameDir"* ]]'
check "token never printed" '[[ "$OUT" != *SECRET* ]]'
check "folders without session.lock are not worlds" '[[ "$OUT" == *"worlds=2"* ]]'

AFTER='sleep 3; touch "$G/saves/Наш мир/session.lock"' run 51658 - --gameDir "$W"
check "mtime: lock written after start" '[[ "$OUT" == *"method=mtime"*"name=Наш мир"* ]]'

AFTER='sleep 3; touch "$G/saves/Наш мир/session.lock" "$G/saves/Старый/session.lock"' run 51658 - --gameDir "$W"
check "ask: two candidates" '[[ "$OUT" == *"method=ask"*"candidates=2"* ]] && [ "$(grep -c ^candidate <<<"$OUT")" = 2 ]'

RUN_CWD=$G AFTER= run 51658 -
check "no gameDir: working folder" '[[ "$OUT" == *"source=cwd"*"saves="*"мс\\saves"* ]]'

AFTER= run 51658 - --gameDir 'нет\такой'
check "relative gameDir, no saves" '[[ "$OUT" == *"reason=no_saves"*"source=gameDir+cwd"* ]]'

# ---------- копии: пути Windows, кириллица, длинные пути, время архива, переименование ----------
rm -rf "$WINEPREFIX/drive_c/Копии миров"
mkdir -p "$G/saves/Наш мир/region" && head -c 2000000 /dev/urandom > "$G/saves/Наш мир/region/r.0.0.mca"
echo data > "$G/saves/Наш мир/level.dat"
sleep 1.1 # «мир не менялся» сравнивается строго по секундам
cli() { timeout 120 wine build/smoke_cli.exe --cmd "$@" | tr -d '\r'; }
OUT=$(cli backup 1 "$W\\saves\\Наш мир" 'C:\Копии миров' clean 0)
check "backup on Windows paths" '[[ "$OUT" == *backup_done*"file=C:\\Копии миров\\Наш мир\\"*"_clean.zip"* ]]'
Z=$(ls "$WINEPREFIX/drive_c/Копии миров/Наш мир/"*.zip 2>/dev/null | head -1)
check "archive readable by Python zipfile" 'python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None; assert \"Наш мир/region/r.0.0.mca\" in z.namelist(); assert not any(n.endswith(\"session.lock\") for n in z.namelist())" "$Z"'
OUT=$(cli list 2 'C:\Копии миров' "$W\\saves\\Наш мир")
check "list on Windows" '[[ "$OUT" == *"kind=clean"*"list_done"*"count=1"* ]]'
OUT=$(cli backup 3 "$W\\saves\\Наш мир" 'C:\Копии миров' clean 1)
check "unchanged skipped (archive time set via SetFileTime)" '[[ "$OUT" == *"backup_skip"*"unchanged"* ]]'
OUT=$(cli space 4 'C:\Копии миров\нет такой')
check "space on Windows" '[[ "$OUT" == space*"free="* ]]'
OUT=$(cli paths)
check "paths: Documents" '[[ "$OUT" == *"backups="*"Porthole Backups"* ]]'
OUT=$(cli prune 5 'C:\Копии миров' "$W\\saves\\Наш мир" 1 0)
check "prune on Windows" '[[ "$OUT" == *"pruned"*"count=0"* ]]'

echo "OK $pass checks, $fail failed"
[ "$fail" = 0 ]
