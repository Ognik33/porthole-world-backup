#!/usr/bin/env python3
"""Готовит окружение для Luau-тестов: строки перевода как таблица STRINGS.

Использование: gen_env.py <out.luau> [язык]   (по умолчанию russian)
"""
import pathlib, sys, tomllib

root = pathlib.Path(__file__).resolve().parent.parent
out = pathlib.Path(sys.argv[1])
lang = sys.argv[2] if len(sys.argv) > 2 else "russian"
data = tomllib.loads((root / "addon" / "world-backup" / "lang" / f"{lang}.toml").read_text(encoding="utf-8"))


def long_str(s):
    eq = "="
    while f"]{eq}]" in s or s.endswith("]" + eq):
        eq += "="
    return f"[{eq}[\n{s}]{eq}]"


lines = ["STRINGS = {"]
for k in sorted(data):
    lines.append(f"\t{k} = {long_str(data[k])},")
lines.append("}")
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text("\n".join(lines) + "\n", encoding="utf-8")
