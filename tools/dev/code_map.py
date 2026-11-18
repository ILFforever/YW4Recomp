"""The data behind the site's code map (docs/advanced/): every module and function of the C runtime, and who calls whom.

Reads the tracked .c/.cpp/.h files under runtime/src and runtime/include (git ls-files, so only what is committed
or staged counts) and writes docs/advanced/code-map.js. Run it after runtime changes, from the repo root:

    python -m tools.dev.code_map          # rewrite docs/advanced/code-map.js
    python -m tools.dev.code_map --check  # exit 1 if it is out of date (no writes)

What it finds, with plain regular expressions (no compiler):
- A module is a file name without its extension: gpu.c + gpu.h = "gpu".
- Module links: A includes B's header, or names a non-static function defined in B (headers only declare, so
  only .c/.cpp files count as using one).
- Functions: every definition that starts at column 0, static ones too, with its line, length and the comment
  right above it. A function "calls" another when it names it in its body: a direct call, or passing it as a
  function pointer. Calls through pointers at run time and calls inside macros are not seen.
- "(tables)": function names used outside any function, such as handler tables.

The site links each function to GitHub at the commit this ran on, so run it on a clean tree for exact line links.
Areas (GROUPS below) are assigned by hand; a new module without one stops the script with its name.
"""
import argparse
import bisect
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs" / "advanced" / "code-map.js"

GROUPS = {
    "core": "main loader yw4load dispatch yw4cpu yw4fp yw4rt crypto setjmp host game_ini version sha256 sysinfo log "
            "c11_keywords msvc_shim dlc_known mat4",
    "system": "kernel hle hle_diag hle_libc hle_math hle_mem hle_os hle_string heap ipc ipc_simple svc_acc svc_am "
              "svc_audio svc_fs svc_hid svc_misc svc_nifm svc_sm svc_time savefs romfs swkbd vi ec modmerge",
    "nvn": "nvn nvn_cmdbuf nvn_objects nvn_shader tegra pixels",
    "vulkan": "gpu gpu_draw gpu_internal gpu_queue gpu_pcache gpu_query vk window",
    "passes": "gbuffer shadow shadow_mask gpu_shadow bloom dof post gpu_post gpu_aa bones widescreen framerate",
    "upscale": "gpu_fsr gpu_dlss gpu_dlssg gpu_fg",
    "overlay": "gamemenu ui hud hud_style theme settings textin pause shortcutwheel mousemenu gamelogo gaiji_create "
               "bugreport diaryrows cheats battle bindings run discord discord_game discord_ipc",
    "debug": "perf events profile gpu_passes gpu_watch gpu_ab memscan trace_drawlist ai_trace",
}

KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "defined", "else", "do", "case"}
DEF = re.compile(r'^([A-Za-z_][^;{}()#=]*?)\b([A-Za-z_]\w*)\s*\(([^;{}]*?)\)\s*(?:const\s*)?\{', re.M)
IDENT = re.compile(r'\b[A-Za-z_]\w*\b')
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
STRIP = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=True).stdout


def strip(src):
    """Comments and string/char literals out, newlines kept (so line numbers still match)."""
    return STRIP.sub(lambda m: "\n" * m.group(0).count("\n") if m.group(0).startswith("/") else '""', src)


def top_comment(src):
    m = re.match(r'\s*(?:/\*(.*?)\*/|((?://[^\n]*\n\s*)+))', src, re.S)
    if not m:
        return ""
    text = m.group(1) or m.group(2)
    text = " ".join(re.sub(r'^\s*(//+|\*+)\s?', '', text, flags=re.M).split())
    s = " ".join(re.split(r'(?<=[.!?])\s+', text)[:2])
    return s[:280] + ("…" if len(s) > 280 else "")


def comment_above(lines, start_line):
    out, i = [], start_line - 2
    while i >= 0:
        s = lines[i].strip()
        if s.startswith(("//", "/*", "*")) or s.endswith("*/"):
            out.append(s)
            i -= 1
        else:
            break
    if not out:
        return ""
    text = re.sub(r'/\*+|\*+/', ' ', "\n".join(reversed(out)))
    text = " ".join(re.sub(r'^\s*(//+|\*+)\s?', '', text, flags=re.M).split())
    return text[:420] + ("…" if len(text) > 420 else "")


def match_brace(text, i):
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return j
    return -1


def build():
    files = [f for f in git("ls-files", "runtime/src", "runtime/include").split() if f.endswith((".c", ".h", ".cpp"))]
    mod_of = lambda f: os.path.splitext(os.path.basename(f))[0]
    raw, clean = {}, {}
    for f in files:
        raw[f] = (ROOT / f).read_text(encoding="utf-8", errors="replace")
        clean[f] = strip(raw[f])

    group_of = {m: g for g, names in GROUPS.items() for m in names.split()}
    mods = {}
    for f in files:
        m = mods.setdefault(mod_of(f), {"id": mod_of(f), "files": [], "loc": 0, "blurb": ""})
        m["files"].append(f)
        m["loc"] += sum(1 for line in raw[f].splitlines() if line.strip())
    missing = sorted(m for m in mods if m not in group_of)
    if missing:
        sys.exit("code_map: no area for " + ", ".join(missing) + " (add it to GROUPS in tools/dev/code_map.py)")
    for m in mods.values():
        m["group"] = group_of[m["id"]]
        m["files"].sort(key=lambda x: (x.endswith(".h"), x))
        m["blurb"] = next((b for b in (top_comment(raw[f]) for f in m["files"]) if b), "")

    # functions: definitions, bodies, file-scope text with the bodies blanked out
    funcs, bodies, scope_text = {}, [], {}
    for f in files:
        mod, text = mod_of(f), clean[f]
        nl = [i for i, c in enumerate(text) if c == "\n"]
        line_of = lambda pos: bisect.bisect_right(nl, pos - 1) + 1
        raw_lines = raw[f].splitlines()
        spans, last_end = [], -1
        for d in DEF.finditer(text):
            if d.start() <= last_end:
                continue
            prefix, name = d.group(1), d.group(2)
            if name in KEYWORDS or prefix.strip() in ("", "else", "return"):
                continue
            end = match_brace(text, d.end() - 1)
            if end < 0:
                continue
            last_end = end
            static = bool(re.search(r'\bstatic\b', prefix))
            key = f"{mod}:{name}" if static else name
            if key in funcs:
                continue
            start_line = line_of(d.start())
            funcs[key] = {"k": key, "n": name, "m": mod, "f": f, "l": line_of(d.start(2)),
                          "len": line_of(end) - start_line + 1, "s": int(static),
                          "d": comment_above(raw_lines, start_line), "c": {}}
            bodies.append((key, mod, text[d.end() - 1:end + 1]))
            spans.append((d.start(), end + 1))
        if not f.endswith(".h"):
            t = list(text)
            for a, b in spans:
                for i in range(a, b):
                    if t[i] != "\n":
                        t[i] = " "
            scope_text[f] = "".join(t)

    def resolve(tok, mod):
        if f"{mod}:{tok}" in funcs:
            return f"{mod}:{tok}"
        return tok if tok in funcs else None

    for key, mod, body in bodies:
        for tok in IDENT.findall(body):
            k = resolve(tok, mod)
            if k and k != key:
                funcs[key]["c"][k] = 1
    for f, text in scope_text.items():
        mod, depth, used = mod_of(f), 0, {}
        for m in re.finditer(r'[{}]|\b[A-Za-z_]\w*\b', text):
            tok = m.group(0)
            if tok == "{":
                depth += 1
            elif tok == "}":
                depth = max(0, depth - 1)
            elif depth > 0 and (k := resolve(tok, mod)):
                used[k] = 1
        if used:
            key = f"{mod}:(tables)"
            funcs[key] = {"k": key, "n": "(tables)", "m": mod, "f": f, "l": 1, "len": 0, "s": 1,
                          "d": "Function names used outside any function: tables of function pointers, handlers and the like.",
                          "c": used}

    # module links: includes, and uses of another module's public functions
    owner = {fn["n"]: fn["m"] for fn in funcs.values() if not fn["s"]}
    edges = defaultdict(lambda: {"include": False, "fns": set()})
    for f in files:
        src = mod_of(f)
        for h in INCLUDE.findall(raw[f]):
            tgt = mod_of(h)
            if tgt in mods and tgt != src:
                edges[(src, tgt)]["include"] = True
        if f.endswith(".h"):
            continue
        for tok in IDENT.findall(clean[f]):
            o = owner.get(tok)
            if o and o != src:
                edges[(src, o)]["fns"].add(tok)

    nodes = [mods[k] for k in sorted(mods)]
    links = [{"source": s, "target": t, "include": e["include"], "fnCount": len(e["fns"]), "fns": sorted(e["fns"])[:60]}
             for (s, t), e in sorted(edges.items())]
    func_list = [{**fn, "c": sorted(fn["c"])} for fn in sorted(funcs.values(), key=lambda x: (x["m"], x["l"], x["k"]))]
    return {"nodes": nodes, "links": links, "funcs": func_list}


def render(data, commit, date):
    payload = json.dumps({"commit": commit, "date": date, **data}, ensure_ascii=False, separators=(",", ":"))
    return ("// Generated by python -m tools.dev.code_map from runtime/ — do not edit. Read by docs/advanced/index.html.\n"
            f"window.CODE_MAP = {payload};\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if docs/advanced/code-map.js is out of date")
    args = ap.parse_args()
    data = build()
    if args.check:
        try:
            text = OUT.read_text(encoding="utf-8")
            old = json.loads(text[text.index("{"):text.rindex("}") + 1])
        except (OSError, ValueError):
            sys.exit(f"code_map: {OUT.relative_to(ROOT)} is missing or unreadable; run python -m tools.dev.code_map")
        old.pop("commit", None), old.pop("date", None)
        if old != json.loads(json.dumps(data)):
            sys.exit("code_map: docs/advanced/code-map.js is out of date; run python -m tools.dev.code_map")
        print("code_map: up to date")
        return
    commit = git("rev-parse", "HEAD").strip()
    date = git("log", "-1", "--format=%cs").strip()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(render(data, commit, date), encoding="utf-8", newline="\n")
    n_funcs = sum(1 for f in data["funcs"] if f["n"] != "(tables)")
    print(f"code_map: {len(data['nodes'])} modules, {n_funcs} functions, {len(data['links'])} module links "
          f"-> {OUT.relative_to(ROOT)} ({OUT.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
