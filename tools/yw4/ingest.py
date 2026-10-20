"""Input pipeline: ExeFS modules -> verified flat images in work_dir.

    python -m tools.yw4.ingest          verify main against target.toml, write outputs
    python -m tools.yw4.ingest --pin    record main's hash in target.toml (first run only)

Refuses to write anything if main does not match the pinned build.
Outputs (gitignored): <work_dir>/exefs/<module>.bin and modules.json.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tomllib
from pathlib import Path

from . import config, nso

TARGET_PATH = config.REPO_ROOT / "target.toml"
MODULE_NAMES = ("rtld", "main", "sdk") + tuple(f"subsdk{i}" for i in range(10))


def load_target() -> dict[str, str]:
    with TARGET_PATH.open("rb") as f:
        return tomllib.load(f).get("main", {})


def pin_target(image_sha256: str, build_id: str) -> None:
    text = TARGET_PATH.read_text(encoding="utf-8")
    text = re.sub(r'(?m)^image_sha256 = ".*"$', f'image_sha256 = "{image_sha256}"', text)
    text = re.sub(r'(?m)^build_id = ".*"$', f'build_id = "{build_id}"', text)
    TARGET_PATH.write_text(text, encoding="utf-8", newline="\n")


def process_module(path: Path) -> tuple[nso.Nso, bytes]:
    blob = path.read_bytes()
    header = nso.parse_header(blob)
    return header, nso.flat_image(blob, header)


def describe(name: str, header: nso.Nso, image: bytes) -> dict:
    return {
        "name": name,
        "build_id": nso.build_id_hex(header.build_id),
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "image_size": len(image),
        "bss_size": header.bss_size,
        "segments": {
            s.name: {
                "mem_offset": s.mem_offset,
                "size": s.size,
                "file_size": s.file_size,
                "compressed": s.compressed,
            }
            for s in header.segments
        },
        "dynstr": list(header.dynstr),
        "dynsym": list(header.dynsym),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pin", action="store_true",
                    help="record main's hash in target.toml (only when nothing is pinned yet)")
    args = ap.parse_args(argv)

    try:
        cfg = config.load()
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1

    # main first: nothing else runs until the build is verified.
    try:
        main_header, main_image = process_module(cfg.exefs_dir / "main")
    except nso.NsoError as e:
        print(f"main: {e}", file=sys.stderr)
        return 1
    got_hash = hashlib.sha256(main_image).hexdigest()
    got_id = nso.build_id_hex(main_header.build_id)

    target = load_target()
    want_hash = target.get("image_sha256", "")
    if not want_hash:
        if args.pin:
            pin_target(got_hash, got_id)
            print(f"pinned main image_sha256={got_hash} build_id={got_id} in {TARGET_PATH.name}")
            print("Commit target.toml, then run again without --pin.")
            return 0
        print(f"No build pinned in {TARGET_PATH.name}.\n"
              f"  main image_sha256 = {got_hash}\n"
              f"  main build_id     = {got_id}\n"
              "If this is the final 4++ update, run again with --pin.", file=sys.stderr)
        return 2
    if args.pin:
        print(f"{TARGET_PATH.name} already pins a build; edit it by hand to change targets.",
              file=sys.stderr)
        return 1
    if got_hash != want_hash:
        print("Wrong game build: main does not match target.toml.\n"
              f"  expected image_sha256 {want_hash} (build_id {target.get('build_id', '?')})\n"
              f"  got      image_sha256 {got_hash} (build_id {got_id})\n"
              "This project supports exactly one version: the final 4++ update.",
              file=sys.stderr)
        return 1
    print(f"main OK  build_id={got_id}")

    out_dir = cfg.work_dir / "exefs"
    out_dir.mkdir(parents=True, exist_ok=True)
    modules = []
    for name in MODULE_NAMES:
        path = cfg.exefs_dir / name
        if not path.is_file():
            continue
        if name == "main":
            header, image = main_header, main_image
        else:
            try:
                header, image = process_module(path)
            except nso.NsoError as e:
                print(f"{name}: {e}", file=sys.stderr)
                return 1
        (out_dir / f"{name}.bin").write_bytes(image)
        info = describe(name, header, image)
        modules.append(info)
        text = info["segments"]["text"]["size"]
        print(f"{name:9} image {len(image):>11,} bytes  .text {text:>11,}  -> {name}.bin")

    (out_dir / "modules.json").write_text(json.dumps(modules, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
