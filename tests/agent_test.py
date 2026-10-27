#!/usr/bin/env python3
"""Тест протокола агента world_backup на Linux-сборке (без Windows и без Porthole).

Проверяет оба способа, которыми Luau шлёт команды:
  * tcp_send - одно соединение на команду, строка, закрытие;
  * tcp_open - одно постоянное соединение, команды приходят с паузами.
Агент лабы проходил только первый способ: соединение он читал до закрытия.
Использование: agent_test.py <путь к тестовой сборке хелпера>
"""
import queue, socket, subprocess, sys, threading, time

exe = sys.argv[1]
TOKEN = "t" * 24


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


port = free_port()
proc = subprocess.Popen([exe, "--agent", str(port), TOKEN], stdout=subprocess.PIPE, text=True, encoding="utf-8")
lines = queue.Queue()
threading.Thread(target=lambda: [lines.put(l.rstrip("\n")) for l in proc.stdout], daemon=True).start()


def one_shot(*parts, token=TOKEN, newline=True):
    """Как porthole.tcp_send: подключиться, отправить строку, закрыть."""
    with socket.create_connection(("127.0.0.1", port), timeout=2) as c:
        c.sendall(("\t".join((token,) + parts) + ("\n" if newline else "")).encode())


def persistent():
    """Как porthole.tcp_open: соединение остаётся открытым."""
    return socket.create_connection(("127.0.0.1", port), timeout=2)


def cmd(*parts, token=TOKEN):
    return ("\t".join((token,) + parts) + "\n").encode()


def expect(prefix, timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        try:
            l = lines.get(timeout=0.2)
        except queue.Empty:
            continue
        if l.startswith(prefix):
            return l
    raise SystemExit(f"FAIL: no line starting with {prefix!r}")


def expect_nothing(seconds):
    try:
        l = lines.get(timeout=seconds)
    except queue.Empty:
        return True
    raise SystemExit(f"FAIL: unexpected line {l!r}")


def count(prefix, n, timeout=5):
    got, end = 0, time.time() + timeout
    while time.time() < end and got < n:
        try:
            if lines.get(timeout=0.2).startswith(prefix):
                got += 1
        except queue.Empty:
            pass
    return got


ok = 0
def check(name, cond):
    global ok
    if not cond:
        raise SystemExit("FAIL " + name)
    ok += 1
    print("PASS", name)


hello = expect("hello")
check("hello proto", "\tproto=1\t" in hello + "\t")
check("hello caps persist", "caps=persist" in hello)
check("ready port", expect("ready") == f"ready\tport={port}")

# ---- tcp_send ----
one_shot("ping"); check("one-shot ping", expect("pong") == "pong")
one_shot("ping", newline=False); check("one-shot without LF", expect("pong") == "pong")
one_shot("ping", token="wrong" * 5); check("bad token", "reason=bad_token" in expect("error"))
one_shot("nope"); check("bad command", "reason=bad_command" in expect("error"))

# ---- tcp_open: команды с паузой дольше 2 с (на этом агент лабы закрывал соединение) ----
c = persistent()
c.sendall(cmd("ping")); check("persistent ping 1", expect("pong") == "pong")
time.sleep(2.6)
c.sendall(cmd("ping")); check("persistent ping after idle", expect("pong") == "pong")

# строка, разрезанная на два пакета, выполняется один раз и целиком
c.sendall(cmd("ping")[:7]); expect_nothing(0.4)
c.sendall(cmd("ping")[7:]); check("split line", expect("pong") == "pong")

# CRLF и пустые строки не мешают
c.sendall(b"\r\n\n" + cmd("ping").replace(b"\n", b"\r\n")); check("crlf and empty lines", expect("pong") == "pong")

# несколько команд одним пакетом
c.sendall(cmd("ping") * 3); check("three in one packet", count("pong", 3) == 3)

# два постоянных соединения одновременно плюс tcp_send между ними
d = persistent()
d.sendall(cmd("ping")); check("second persistent", expect("pong") == "pong")
one_shot("ping"); check("one-shot while persistent open", expect("pong") == "pong")
c.sendall(cmd("ping")); check("first persistent still works", expect("pong") == "pong")

# чужой токен закрывает только своё соединение
e = persistent()
e.sendall(cmd("ping", token="x" * 24)); expect("error")
time.sleep(0.2)
check("bad-token conn closed", e.recv(16) == b"")
d.sendall(cmd("ping")); check("others unaffected", expect("pong") == "pong")

# слишком длинная строка: ошибка, соединение закрыто, агент жив
f = persistent()
try:
    f.sendall(b"a" * 70000)
except OSError:
    pass
check("line too long", "reason=line_too_long" in expect("error"))
one_shot("ping"); check("alive after long line", expect("pong") == "pong")

# больше соединений, чем мест: новые всё равно обслуживаются
extra = [persistent() for _ in range(20)]
time.sleep(0.3)
g = persistent(); g.sendall(cmd("ping")); check("new conn when full", expect("pong") == "pong")
for s in extra:
    s.close()

c.close(); d.close(); g.close()
one_shot("quit")
proc.wait(timeout=5)
check("quit exits", proc.returncode == 0)
# сторожевой таймер: без команд с верным токеном агент завершается сам
port2 = free_port()
p2 = subprocess.Popen([exe, "--agent", str(port2), TOKEN, "2"], stdout=subprocess.PIPE, text=True, encoding="utf-8")
p2.stdout.readline(); p2.stdout.readline()  # hello, ready
for _ in range(3):  # ping каждую секунду держит агент живым дольше 2 с
    time.sleep(1)
    with socket.create_connection(("127.0.0.1", port2), timeout=2) as c2:
        c2.sendall(cmd("ping"))
check("ping keeps agent alive", p2.poll() is None)
with socket.create_connection(("127.0.0.1", port2), timeout=2) as c2:
    c2.sendall(cmd("ping", token="z" * 24))  # чужой токен таймер не продлевает
rest = p2.communicate(timeout=6)[0]
check("idle timeout exits", p2.returncode == 0 and "error\treason=idle_timeout" in rest)
print(f"OK {ok} checks")
