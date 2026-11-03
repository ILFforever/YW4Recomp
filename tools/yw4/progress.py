"""Progress lines for the launcher (decision 0050): with YW4_PROGRESS=1 the build steps print

    @progress <step> <done> <total> <what>

one per piece of work done, so the launcher can show a bar, the step and time left without parsing their normal
output (which stays the same). Steps: unpack, extract, icon, ingest, translate, shaders, compile, link. <done> and
<total> are counts of that step's own units (bytes, functions, shaders, files); a step with no count reports 0 1
at its start and 1 1 at its end. <what> is free text, possibly empty, on the rest of the line."""

from __future__ import annotations

import os
import sys
import time

_on = os.environ.get("YW4_PROGRESS") == "1"
_last: dict[str, float] = {}


def report(step: str, done: int, total: int, what: str = "", every: float = 0.0) -> None:
    """One progress line (only with YW4_PROGRESS=1). `every`: at most one line per that many seconds for this
    step, except the last (done == total), for steps with very many small units."""
    if not _on:
        return
    now = time.monotonic()
    if every and done < total and now - _last.get(step, 0.0) < every:
        return
    _last[step] = now
    print(f"@progress {step} {done} {total} {what}".rstrip(), flush=True, file=sys.stdout)
