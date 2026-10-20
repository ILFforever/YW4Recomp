"""Compare Ghidra's function list with our two independent sources.

    python -m tools.yw4.compare_functions

Sources:
    ghidra   <work_dir>/ghidra/main.functions.json (python -m tools.yw4.ghidra)
    unwind   .eh_frame_hdr function starts
    taken    address-taken functions (pointer relocations into .text)

Prints the overlap and writes the misses to <work_dir>/ghidra/function_gaps.json
so they can be inspected in Ghidra.
"""

from __future__ import annotations

import json
import sys

from . import config, recon


def main() -> int:
    try:
        cfg = config.load()
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    funcs_path = cfg.work_dir / "ghidra" / "main.functions.json"
    if not funcs_path.is_file():
        print(f"{funcs_path} not found; run python -m tools.yw4.ghidra first", file=sys.stderr)
        return 1

    image, mods = recon.load_main(cfg)
    text_size = mods["main"]["segments"]["text"]["size"]
    funcs = json.loads(funcs_path.read_text())["functions"]
    ghidra = {f["entry"] for f in funcs if f["entry"] < text_size}
    unwind = set(recon.eh_frame_function_starts(image))
    taken = recon.address_taken_functions(image, text_size)

    def report(name: str, ref: set[int]) -> list[int]:
        missing = sorted(ref - ghidra)
        print(f"{name:7} {len(ref):7,}  found by Ghidra {len(ref) - len(missing):7,}  "
              f"missing {len(missing):6,} ({100 * len(missing) / max(len(ref), 1):.2f}%)")
        return missing

    print(f"ghidra  {len(ghidra):7,} functions in .text "
          f"({sum(f['thunk'] for f in funcs):,} thunks)")
    miss_unwind = report("unwind", unwind)
    miss_taken = report("taken", taken)
    extra = sorted(ghidra - unwind - taken)
    print(f"Ghidra-only (no unwind info, not address-taken): {len(extra):,}")

    out = cfg.work_dir / "ghidra" / "function_gaps.json"
    out.write_text(json.dumps({
        "unwind_not_in_ghidra": [hex(a) for a in miss_unwind],
        "taken_not_in_ghidra": [hex(a) for a in miss_taken],
        "ghidra_only": [hex(a) for a in extra],
    }, indent=1))
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
