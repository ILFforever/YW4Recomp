"""Name the functions behind runtime addresses like "sdk+0x32e0e8" or "main+0x30".

    python -m tools.yw4.where sdk+0x32e0e8 main+0x5c2d40 ...
    yw4.exe ... 2>&1 | python -m tools.yw4.where -     (annotates every such address in the text)

Uses each module's dynamic symbol table (work/exefs/, from tools.yw4.ingest): the nearest
defined function symbol at or below the offset. The sdk exports almost everything, so sdk
addresses mostly get a name; main exports little, so its addresses often stay unnamed.
"""

from __future__ import annotations

import bisect
import json
import re
import sys

from . import config, symbols

ADDR = re.compile(r"\b(main|sdk)\+0x([0-9a-fA-F]+)")


def load_tables(cfg: config.Config) -> dict[str, tuple[list[int], list[symbols.Symbol]]]:
    exefs = cfg.work_dir / "exefs"
    infos = {m["name"]: m for m in json.loads((exefs / "modules.json").read_text())}
    tables = {}
    for name in ("main", "sdk"):
        image = (exefs / f"{name}.bin").read_bytes()
        syms = sorted((s for s in symbols.read_dynsym(image, infos[name]) if s.defined and s.type == "func"),
                      key=lambda s: s.value)
        tables[name] = ([s.value for s in syms], syms)
    return tables


def name_of(tables, module: str, off: int, names: dict[str, str]) -> str | None:
    values, syms = tables[module]
    i = bisect.bisect_right(values, off) - 1
    if i < 0:
        return None
    s = syms[i]
    if s.size and off >= s.value + s.size:
        return None  # past the end of the nearest symbol: an unexported function
    return f"{names.get(s.name, s.name)}+{off - s.value:#x}"


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if not args:
        print(__doc__)
        return 1
    tables = load_tables(config.load())
    text = sys.stdin.read() if args == ["-"] else "\n".join(args)
    found = {(m.group(1), int(m.group(2), 16)) for m in ADDR.finditer(text)}
    raw = {}
    for mod, off in found:
        values, syms = tables[mod]
        i = bisect.bisect_right(values, off) - 1
        if i >= 0:
            raw[syms[i].name] = None
    names = symbols.demangle(list(raw))

    def annotate(m: re.Match) -> str:
        n = name_of(tables, m.group(1), int(m.group(2), 16), names)
        return f"{m.group(0)} ({n})" if n else m.group(0)

    print(ADDR.sub(annotate, text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
