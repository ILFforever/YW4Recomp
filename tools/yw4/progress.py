"""Progress lines for the launcher (decision 0050): with YW4_PROGRESS=1 the build steps print

    @progress <step> <done> <total> <what>

one per piece of work done, so the launcher can show a bar, the step and time left without parsing their normal
output (which stays the same). Steps: unpack, extract, icon, ingest, translate, shaders, compile, link. <done> and
<total> are counts of that step's own units (bytes, functions, shaders, files); a step with no count reports 0 1
at its start and 1 1 at its end. <what> is free text, possibly empty, on the rest of the line."""

from __future__ import annotations

import os
import sys
import threading
import time

_on = os.environ.get("YW4_PROGRESS") == "1"
_last: dict[str, float] = {}
# YW4_PROGRESS_FILES=1 (a launcher that shows the files compiling, 1.6 and later): recomp/build.py also prints
#     @file count <n>                        the files it will compile
#     @file start <key> <seconds> <name>     one starts: <key> its object (unique), <seconds> its usual time, <name> the source
#     @file done <key>
#     @file wait <n>                         how many wait for free memory now (when that changes)
# A separate switch, not YW4_PROGRESS=2: a launcher before it would show these lines in its log, and game code before
# it would read "2" as no progress at all.
_files_on = _on and os.environ.get("YW4_PROGRESS_FILES") == "1"
_write = threading.Lock()  # one line at a time: the compile threads report together


def file_event(*parts: object) -> None:
    """One @file line (only with YW4_PROGRESS_FILES=1), written whole."""
    if not _files_on:
        return
    line = "@file " + " ".join(str(p) for p in parts) + "\n"
    with _write:
        sys.stdout.write(line)
        sys.stdout.flush()


def report(step: str, done: int, total: int, what: str = "", every: float = 0.0) -> None:
    """One progress line (only with YW4_PROGRESS=1). `every`: at most one line per that many seconds for this
    step, except the last (done == total), for steps with very many small units."""
    if not _on:
        return
    now = time.monotonic()
    if every and done < total and now - _last.get(step, 0.0) < every:
        return
    _last[step] = now
    with _write:
        sys.stdout.write(f"@progress {step} {done} {total} {what}".rstrip() + "\n")
        sys.stdout.flush()
