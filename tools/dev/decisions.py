"""The decision records' index, and checks that the docs still point at things that exist.

    python -m tools.dev.decisions            write docs/decisions/README.md from the records' headers
    python -m tools.dev.decisions --check    check, write nothing; exit 1 on a problem

Each record (docs/decisions/NNNN-*.md) carries a header right under its title, hidden when Markdown is shown:

    <!-- decision
    state: accepted | plan | proposed | rejected
    date: YYYY-MM-DD
    replaces: none | 0005 (part), 0042
    code: none | runtime/src/gamemenu.c, runtime/src/ui.c
    -->

"Replaced by" is never written by hand: the index works it out from the other records' `replaces`.

--check finds: records without a header or with a bad one; `code` paths that no longer exist; "decision NNNN"
mentions (in the docs, the README, CLAUDE.md and the code) of a record that doesn't exist; relative Markdown links
to files that don't exist; and an index that is out of date. Only the repo's own text files are read (never
work/), so it is quick and needs no game files.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DECISIONS = REPO / "docs" / "decisions"
INDEX = DECISIONS / "README.md"
STATES = ("accepted", "plan", "proposed", "rejected")

HEADER = re.compile(r"<!-- decision\n(.*?)\n-->", re.S)
TITLE = re.compile(r"^# (\d{4}) — (.+)$", re.M)
# "decision 0053", "decisions 0047, 0050 and 0054", "Decision 0052"
MENTION = re.compile(r"\b[Dd]ecisions? ((?:\d{4})(?:(?:, | and | or |/)\d{4})*)")
MDLINK = re.compile(r"\]\(([^)\s#]+)(?:#[^)]*)?\)")
# where mentions are looked for: the repo's own text, never work/ or third-party code
SCAN = ["README.md", "CLAUDE.md", "docs", "runtime/src", "runtime/include", "recomp", "tools", "launcher/ui",
        "launcher/src-tauri/src"]
SCAN_EXT = {".md", ".c", ".h", ".cpp", ".py", ".rs", ".js", ".html", ".toml", ".glsl", ".comp", ".frag", ".vert"}


@dataclass
class Record:
    number: str
    title: str
    path: Path
    state: str = ""
    date: str = ""
    replaces: list[tuple[str, bool]] = field(default_factory=list)  # (number, only part)
    code: list[str] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)


def read_records() -> list[Record]:
    out = []
    for p in sorted(DECISIONS.glob("[0-9][0-9][0-9][0-9]-*.md")):
        text = p.read_text(encoding="utf-8")
        t = TITLE.search(text)
        r = Record(p.name[:4], t.group(2).strip() if t else p.stem, p)
        if not t or t.group(1) != r.number:
            r.problems.append("its title isn't '# NNNN — Title' with its own number")
        h = HEADER.search(text)
        if not h:
            r.problems.append("no '<!-- decision' header")
            out.append(r)
            continue
        fields = {}
        for line in h.group(1).splitlines():
            k, _, v = line.partition(":")
            fields[k.strip()] = v.strip()
        r.state, r.date = fields.get("state", ""), fields.get("date", "")
        if r.state not in STATES:
            r.problems.append(f"state '{r.state}' isn't one of {', '.join(STATES)}")
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", r.date):
            r.problems.append(f"date '{r.date}' isn't YYYY-MM-DD")
        rep = fields.get("replaces", "none")
        if rep and rep != "none":
            for part in rep.split(","):
                m = re.fullmatch(r"\s*(\d{4})\s*(\(part\))?\s*", part)
                if m:
                    r.replaces.append((m.group(1), bool(m.group(2))))
                else:
                    r.problems.append(f"replaces '{part.strip()}' isn't 'NNNN' or 'NNNN (part)'")
        code = fields.get("code", "none")
        if code and code != "none":
            r.code = [c.strip() for c in code.split(",") if c.strip()]
        out.append(r)
    return out


def index_text(records: list[Record]) -> str:
    by = {r.number: r for r in records}
    replaced_by: dict[str, list[str]] = {}
    for r in records:
        for n, part in r.replaces:
            replaced_by.setdefault(n, []).append(f"{r.number}{' (part)' if part else ''}")
    lines = [
        "# Decisions",
        "",
        "Why the project is built the way it is: one record per choice, numbered in the order they were made. This",
        "index is generated from the records' headers by `python -m tools.dev.decisions` (do not edit it by hand);",
        "`--check` also checks the docs' links and the code's `decision NNNN` mentions.",
        "",
        "States: **accepted** (in use), **plan** (agreed, not built yet), **proposed**, **rejected**. *Replaced by*",
        "lists later records that replace a record, or part of it.",
        "",
        "| # | Decision | State | Date | Replaced by | Code |",
        "|---|---|---|---|---|---|",
    ]
    for r in records:
        rb = ", ".join(f"[{x}]({by[x[:4]].path.name})" if x[:4] in by else x for x in replaced_by.get(r.number, []))
        code = ", ".join(f"`{c}`" for c in r.code)
        lines.append(f"| {r.number} | [{r.title}]({r.path.name}) | {r.state} | {r.date} | {rb} | {code} |")
    return "\n".join(lines) + "\n"


def scan_files() -> list[Path]:
    out = []
    for s in SCAN:
        p = REPO / s
        if p.is_file():
            out.append(p)
        elif p.is_dir():
            out += [f for f in p.rglob("*") if f.is_file() and f.suffix in SCAN_EXT and "node_modules" not in f.parts]
    return out


def check(records: list[Record]) -> list[str]:
    problems = []
    numbers = {r.number for r in records}
    for r in records:
        problems += [f"{r.path.relative_to(REPO).as_posix()}: {p}" for p in r.problems]
        for c in r.code:
            if not (REPO / c).exists():
                problems.append(f"{r.path.relative_to(REPO).as_posix()}: code path {c} doesn't exist")
        for n, _ in r.replaces:
            if n not in numbers:
                problems.append(f"{r.path.relative_to(REPO).as_posix()}: replaces {n}, which doesn't exist")
    for f in scan_files():
        rel = f.relative_to(REPO).as_posix()
        text = f.read_text(encoding="utf-8", errors="replace")
        for m in MENTION.finditer(text):
            for n in re.findall(r"\d{4}", m.group(1)):
                if n not in numbers:
                    line = text.count("\n", 0, m.start()) + 1
                    problems.append(f"{rel}:{line}: mentions decision {n}, which doesn't exist")
        if f.suffix != ".md":
            continue
        for m in MDLINK.finditer(text):
            target = m.group(1)
            if re.match(r"[a-z]+:", target):  # http:, https:, mailto:
                continue
            if not (f.parent / target).exists():
                line = text.count("\n", 0, m.start()) + 1
                problems.append(f"{rel}:{line}: links to {target}, which doesn't exist")
    if not INDEX.is_file() or INDEX.read_text(encoding="utf-8") != index_text(records):
        problems.append("docs/decisions/README.md is out of date: run python -m tools.dev.decisions")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="check, write nothing; exit 1 on a problem")
    args = ap.parse_args(argv)
    records = read_records()
    if args.check:
        problems = check(records)
        for p in problems:
            print(p)
        print(f"{len(records)} decisions, {len(problems)} problem(s)")
        return 1 if problems else 0
    bad = [f"{r.path.name}: {p}" for r in records for p in r.problems]
    if bad:
        print("\n".join(bad), file=sys.stderr)
        print("fix these headers first (python -m tools.dev.decisions --check lists everything)", file=sys.stderr)
        return 1
    INDEX.write_text(index_text(records), encoding="utf-8", newline="\n")
    print(f"wrote {INDEX.relative_to(REPO).as_posix()} ({len(records)} decisions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
