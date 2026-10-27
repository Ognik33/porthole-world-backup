#!/usr/bin/env python3
"""Тесты копий на Linux-сборке хелпера: backup, list, prune, space, paths.

Архивы проверяются двумя независимыми читателями: Python zipfile (CRC каждого файла) и
Info-ZIP `unzip -t`, если он есть. Содержимое сравнивается с миром байт в байт.
Использование: backup_test.py <тестовая сборка хелпера> [--fast]  (--fast: без ZIP64-тестов)
"""
import os, queue, shutil, socket, subprocess, sys, tempfile, threading, time, zipfile

exe = sys.argv[1]
FAST = "--fast" in sys.argv
UNZIP = shutil.which("unzip")
ok = 0


def check(name, cond, info=""):
    global ok
    if not cond:
        raise SystemExit(f"FAIL {name} {info}")
    ok += 1
    print("PASS", name)


def cmd(*args):
    """Команда через CLI (--cmd ждёт конца копии). Возвращает строки вывода как (вид, поля)."""
    out = subprocess.run([exe, "--cmd", *map(str, args)], capture_output=True, text=True, encoding="utf-8", timeout=600).stdout
    res = []
    for line in out.splitlines():
        kind, *rest = line.split("\t")
        res.append((kind, dict(f.split("=", 1) for f in rest)))
    return res


def first(lines, kind):
    for k, f in lines:
        if k == kind:
            return f
    return None


def verify_zip(path, world, name):
    """Архив читается, CRC сходятся, содержимое = мир (без session.lock), каталоги на месте."""
    with zipfile.ZipFile(path) as z:
        bad = z.testzip()
        if bad:
            return f"bad crc {bad}"
        infos = {i.filename: i for i in z.infolist()}
        if any(not (i.flag_bits & 0x800) for i in infos.values()):
            return "utf-8 flag missing"
        expect = {name + "/"}
        for root, dirs, files in os.walk(world):
            rel = os.path.relpath(root, world)
            pre = name + "/" + ("" if rel == "." else rel.replace(os.sep, "/") + "/")
            for d in dirs:
                expect.add(pre + d + "/")
            for f in files:
                if rel == "." and f == "session.lock":
                    continue
                expect.add(pre + f)
                with open(os.path.join(root, f), "rb") as fh, z.open(pre + f) as zf:
                    while True:
                        a, b = fh.read(1 << 20), zf.read(1 << 20)
                        if a != b:
                            return f"content differs {pre + f}"
                        if not a:
                            break
        if set(infos) != expect:
            return f"entries differ: extra {sorted(set(infos) - expect)[:3]} missing {sorted(expect - set(infos))[:3]}"
    if UNZIP:
        r = subprocess.run([UNZIP, "-tqq", path], capture_output=True)
        if r.returncode != 0:
            return "unzip -t failed: " + r.stdout.decode(errors="replace")[-300:]
    return None


tmp = tempfile.mkdtemp(prefix="wb-backup-")
saves = os.path.join(tmp, "inst A", "saves")
world = os.path.join(saves, "Наш мир")
out = os.path.join(tmp, "Porthole Backups")


def make_world(path, rnd=3_000_000):
    os.makedirs(os.path.join(path, "region"), exist_ok=True)
    os.makedirs(os.path.join(path, "data"), exist_ok=True)
    os.makedirs(os.path.join(path, "datapacks", "пусто"), exist_ok=True)  # пустой каталог тоже нужен
    with open(os.path.join(path, "level.dat"), "wb") as f:
        f.write(b"\x1f\x8b" + os.urandom(1000))
    with open(os.path.join(path, "region", "r.0.0.mca"), "wb") as f:
        f.write(os.urandom(rnd))
    with open(os.path.join(path, "data", "карта_1.dat"), "wb") as f:
        f.write(b"\0" * 100_000)
    open(os.path.join(path, "empty.txt"), "wb").close()
    with open(os.path.join(path, "session.lock"), "wb") as f:
        f.write(b"\xe2\x98\x83")


make_world(world)
time.sleep(1.1)

# ---------- backup ----------
r = cmd("backup", 1, world, out, "clean", 1)
done = first(r, "backup_done")
check("clean backup done", done is not None, r)
check("backup_start precedes", r[0][0] == "backup_start" and r[0][1]["file"] == done["file"])
folder = os.path.join(out, "Наш мир")
check("archive in world folder, named by start time", os.path.dirname(done["file"]) == folder and done["file"].endswith("_clean.zip"))
check("archive valid and equal to world", verify_zip(done["file"], world, "Наш мир") is None, verify_zip(done["file"], world, "Наш мир"))
check("no tmp left", not any(n.endswith(".tmp") for n in os.listdir(folder)))
check("marker holds world path", open(os.path.join(folder, ".world-backup"), encoding="utf-8").read().strip() == world)
first_copy = done["file"]

r = cmd("backup", 2, world, out, "clean", 1)
check("unchanged world skipped", first(r, "backup_skip") and first(r, "backup_skip")["reason"] == "unchanged", r)
r = cmd("backup", 3, world, out, "hot", 1)
check("hot also skipped when nothing changed", first(r, "backup_skip") is not None, r)
r = cmd("backup", 4, world, out, "clean", 0)
check("if_changed=0 copies anyway (button)", first(r, "backup_done") is not None, r)
time.sleep(1.1)
with open(os.path.join(world, "level.dat"), "ab") as f:
    f.write(b"x")
r = cmd("backup", 5, world, out, "clean", 1)
check("changed world copied", first(r, "backup_done") is not None, r)
second = first(r, "backup_done")["file"]
check("second copy valid", verify_zip(second, world, "Наш мир") is None)

r = cmd("backup", 6, os.path.join(saves, "нет такого"), out, "clean", 1)
check("missing world", first(r, "backup_error")["reason"] == "no_world")
r = cmd("backup", 7, world, "relative/out", "clean", 1)
check("relative root rejected", first(r, "backup_error")["reason"] == "bad_root")
r = cmd("backup", 8, world, out, "warm", 1)
check("bad kind rejected", first(r, "backup_error")["reason"] == "bad_args")

r = subprocess.run([exe, "--cmd", "backup", "12", world, out, "clean", "0"], capture_output=True, text=True,
                   env={**os.environ, "WB_TEST_FREE": str(200 * 1024 * 1024)}).stdout
check("not enough space: refused before writing", "reason=no_space" in r and "need=" in r, r)

# ---------- одинаковые имена миров из разных инстансов, чужая папка ----------
world_b = os.path.join(tmp, "inst B", "saves", "Наш мир")
make_world(world_b, rnd=10_000)
r = cmd("backup", 9, world_b, out, "clean", 0)
fb = first(r, "backup_done")["file"]
check("same name from another instance gets hashed folder", os.path.basename(os.path.dirname(fb)).startswith("Наш мир (") and os.path.dirname(fb) != folder, fb)
r = cmd("backup", 10, world_b, out, "clean", 0)
check("hashed folder reused", os.path.dirname(first(r, "backup_done")["file"]) == os.path.dirname(fb))
foreign = os.path.join(tmp, "x", "saves", "Чужое")
make_world(foreign, rnd=10)
os.makedirs(os.path.join(out, "Чужое"))
with open(os.path.join(out, "Чужое", "мои заметки.txt"), "w") as f:
    f.write("не трогать")
r = cmd("backup", 11, foreign, out, "clean", 0)
check("foreign folder without marker not used", os.path.basename(os.path.dirname(first(r, "backup_done")["file"])).startswith("Чужое ("))

# ---------- list ----------
r = cmd("list", 20, out, world)
copies = [f for k, f in r if k == "copy"]
ld = first(r, "list_done")
check("list count", ld and int(ld["count"]) == 3 and len(copies) == 3, r)
check("list newest first", copies[0]["file"] == second and copies[-1]["file"] == first_copy)
check("list fields", copies[0]["kind"] == "clean" and int(copies[0]["bytes"]) == os.path.getsize(second) and len(copies[0]["local"]) == 19)
check("archive time = start of copy", int(copies[0]["time"]) == int(os.path.getmtime(second)))
r = cmd("list", 21, out, os.path.join(saves, "Новый"))
check("list of world without copies", first(r, "list_done")["count"] == "0")

# ---------- prune (поддельные архивы: prune смотрит только имена и даты) ----------
pf = os.path.join(out, "Ротация")
pw = os.path.join(tmp, "inst A", "saves", "Ротация")
os.makedirs(pw)
cmd("backup", 30, pw, out, "clean", 0)  # создаёт папку с меткой
for n in os.listdir(pf):
    if n.endswith(".zip"):
        os.remove(os.path.join(pf, n))
base = int(time.time()) - 100_000
names = []
for i in range(6):
    for kind in ("clean", "hot"):
        t = time.localtime(base + i * 3600 + (0 if kind == "clean" else 1800))
        n = time.strftime("%Y-%m-%d_%H-%M-%S", t) + f"_{kind}.zip"
        p = os.path.join(pf, n)
        with open(p, "wb") as f:
            f.truncate(1024 * 1024)  # 1 МБ, разреженный
        os.utime(p, (base + i * 3600, base + i * 3600 + (0 if kind == "clean" else 1800)))
        names.append(n)
for n in ("заметки.txt", "2020-01-01_00-00-00_clean.zip.bak", "2020-01-01_00-00-00_cold.zip"):
    open(os.path.join(pf, n), "w").close()
r = cmd("prune", 31, out, pw, 2, 1)
left = sorted(n for n in os.listdir(pf) if n.endswith(".zip") and "cold" not in n)
check("prune keeps 2 clean + 1 hot", first(r, "pruned")["count"] == "9" and len(left) == 3, (r, left))
check("prune keeps the newest", set(left) == {names[10], names[8], names[11]}, left)
check("foreign files untouched", all(os.path.exists(os.path.join(pf, n)) for n in ("заметки.txt", "2020-01-01_00-00-00_clean.zip.bak", "2020-01-01_00-00-00_cold.zip")))
r = cmd("prune", 32, out, pw, 0, 0)
left = [n for n in os.listdir(pf) if n.endswith("_clean.zip") or n.endswith("_hot.zip")]
check("N=0 still keeps the last clean copy", left == [names[10]], left)
# лимит места: 5 чистых по 1 МБ, лимит 2 МБ
for i in range(5):
    t = time.localtime(base + 50_000 + i * 60)
    p = os.path.join(pf, time.strftime("%Y-%m-%d_%H-%M-%S", t) + "_clean.zip")
    with open(p, "wb") as f:
        f.truncate(1024 * 1024)
    os.utime(p, (base + 50_000 + i * 60,) * 2)
r = cmd("prune", 33, out, pw, 10, 10, 2)
left = sorted(n for n in os.listdir(pf) if n.endswith("_clean.zip"))
check("space limit removes oldest", first(r, "pruned")["count"] == "4" and len(left) == 2, (r, left))
r = cmd("prune", 34, out, pw, 10, 10, 1)
left = [n for n in os.listdir(pf) if n.endswith("_clean.zip")]
check("space limit never removes the newest clean", len(left) == 1, left)

# ---------- брошенный .tmp ----------
stale = os.path.join(folder, "2020-01-01_00-00-00_clean.zip.tmp")
mine_not = os.path.join(folder, "что-то.tmp")
open(stale, "w").close()
open(mine_not, "w").close()
cmd("backup", 40, world, out, "clean", 0)
check("stale tmp of ours removed", not os.path.exists(stale))
check("foreign tmp kept", os.path.exists(mine_not))

# ---------- space, paths ----------
r = cmd("space", 50, os.path.join(out, "нет", "такой", "папки"))
sp = first(r, "space")
check("space of missing path uses parent", sp and int(sp["free"]) > 0 and int(sp["total"]) >= int(sp["free"]), r)
r = cmd("space", 51, "relative")
check("space rejects relative path", first(r, "space_error") is not None)
r = subprocess.run([exe, "--cmd", "paths"], capture_output=True, text=True, env={**os.environ, "HOME": "/home/тест"}).stdout
check("paths: Documents/Porthole Backups", "backups=/home/тест/Documents/Porthole Backups" in r, r)

# ---------- агент: занято, прогресс, отмена при выходе ----------
big = os.path.join(tmp, "big", "saves", "Большой")
os.makedirs(os.path.join(big, "region"))
for i in range(8):
    with open(os.path.join(big, "region", f"r.{i}.mca"), "wb") as f:
        f.write(os.urandom(40_000_000))
TOKEN = "t" * 24
s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
p = subprocess.Popen([exe, "--agent", str(port), TOKEN], stdout=subprocess.PIPE, text=True, encoding="utf-8")
lines = queue.Queue()
threading.Thread(target=lambda: [lines.put(l.rstrip("\n")) for l in p.stdout], daemon=True).start()


def send(*parts):
    with socket.create_connection(("127.0.0.1", port)) as c:
        c.sendall(("\t".join((TOKEN,) + tuple(map(str, parts))) + "\n").encode())


def wait_for(prefix, timeout=60):
    end = time.time() + timeout
    seen = []
    while time.time() < end:
        try:
            l = lines.get(timeout=0.2)
        except queue.Empty:
            continue
        seen.append(l)
        if l.startswith(prefix):
            return l, seen
    raise SystemExit(f"FAIL no {prefix}: {seen[-5:]}")


wait_for("ready")
send("backup", 60, big, out, "clean", 0)
wait_for("backup_start")
send("backup", 61, world, out, "clean", 0)
l, _ = wait_for("backup_error\tseq=61")
check("second backup while busy refused", "reason=busy" in l)
send("ping")
wait_for("pong", timeout=5)
check("agent responsive during backup", True)
l, seen = wait_for("backup_done\tseq=60", timeout=300)
check("progress reported", any(x.startswith("progress\tseq=60") for x in seen))
check("big copy valid", verify_zip(l.split("file=")[1].split("\t")[0], big, "Большой") is None)

send("backup", 62, big, out, "clean", 0)
wait_for("backup_start\tseq=62")
send("quit")
p.wait(timeout=20)
bf = os.path.join(out, "Большой")
check("quit during backup: no tmp, no half archive", not any(n.endswith(".tmp") for n in os.listdir(bf)) and len([n for n in os.listdir(bf) if n.endswith(".zip")]) == 1, os.listdir(bf))

# ---------- ZIP64 ----------
if not FAST:
    w64 = os.path.join(tmp, "z64", "saves", "Огромный")
    os.makedirs(os.path.join(w64, "region"))
    with open(os.path.join(w64, "region", "huge.mca"), "wb") as f:
        f.truncate(4_400_000_000)  # 4,4 ГБ нулей, разреженный: на диске почти ничего
        f.seek(4_399_999_000)
        f.write("конец файла".encode().ljust(1000, b"!"))
    with open(os.path.join(w64, "level.dat"), "wb") as f:
        f.write(b"small")
    t0 = time.time()
    r = cmd("backup", 70, w64, out, "clean", 0)
    d = first(r, "backup_done")
    check("file > 4 GB archived", d is not None, r)
    with zipfile.ZipFile(d["file"]) as z:
        i = z.getinfo("Огромный/region/huge.mca")
        check("ZIP64 size recorded", i.file_size == 4_400_000_000)
        with z.open(i) as zf:
            zf.seek(4_399_999_000)
            check("tail of the 4.4 GB file intact", zf.read(1000) == "конец файла".encode().ljust(1000, b"!"))
    print(f"  (4.4 GB file: {time.time() - t0:.1f} s)")

    many = os.path.join(tmp, "many", "saves", "Много")
    os.makedirs(many)
    for i in range(70_000):
        open(os.path.join(many, f"f{i}"), "wb").close()
    r = cmd("backup", 71, many, out, "clean", 0)
    d = first(r, "backup_done")
    check("70000 entries (ZIP64 end record)", d is not None and len(zipfile.ZipFile(d["file"]).namelist()) == 70_001, r)
    if UNZIP:
        check("unzip accepts ZIP64 archives", subprocess.run([UNZIP, "-tqq", d["file"]], capture_output=True).returncode == 0)

shutil.rmtree(tmp)
print(f"OK {ok} checks")
