#!/usr/bin/env python3
"""Проверки текстов и метаданных, которые легко нарушить случайно:

* в текстах для пользователей (README, CHANGELOG, docs, переводы) нет длинного и среднего тире;
* в переводах одинаковый набор ключей и одинаковые подстановки {name} во всех языках;
* автор в addon.toml и в LICENSE только Ognik33.
"""
import pathlib, re, sys, tomllib

root = pathlib.Path(__file__).resolve().parent.parent
addon = root / "addon" / "world-backup"
errors = []

user_texts = [*root.glob("README*.md"), root / "CHANGELOG.md", *(root / "docs").glob("*.md"), *(addon / "lang").glob("*.toml")]
for p in user_texts:
    for n, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
        for ch, name in (("\u2014", "em dash"), ("\u2013", "en dash")):
            if ch in line:
                errors.append(f"{p.relative_to(root)}:{n}: {name} in user-facing text")

langs = {p.stem: tomllib.loads(p.read_text(encoding="utf-8")) for p in sorted((addon / "lang").glob("*.toml"))}
base = langs.get("english")
if base is None:
    errors.append("lang/english.toml is missing")
else:
    for name, data in langs.items():
        for k in sorted(set(base) ^ set(data)):
            errors.append(f"lang/{name}.toml: key {k!r} {'missing' if k in base else 'not in english'}")
        for k in set(base) & set(data):
            a, b = set(re.findall(r"{(\w+)}", base[k])), set(re.findall(r"{(\w+)}", data[k]))
            if a != b:
                errors.append(f"lang/{name}.toml: {k!r} placeholders {sorted(b)} != english {sorted(a)}")

meta = tomllib.loads((addon / "addon.toml").read_text(encoding="utf-8"))
if meta.get("author") != "Ognik33":
    errors.append("addon.toml: author must be Ognik33")
if not re.search(r"^Copyright \(c\) \d{4} Ognik33$", (root / "LICENSE").read_text(encoding="utf-8"), re.M):
    errors.append("LICENSE: copyright line must name Ognik33 only")

for e in errors:
    print("FAIL", e)
if errors:
    sys.exit(1)
print(f"PASS lint ({len(user_texts)} texts, {len(langs)} languages)")
