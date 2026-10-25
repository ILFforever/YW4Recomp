"""Give a SPIR-V file a random OpString so the driver's shader cache has never seen it (a cold
build for testing). Keeps <file>.orig the first time.
    python -m tools.dev.spv_salt work/shaders/glsl/<key>.frag.spv
The startup prebuild (gpu_draw.c) was checked this way: a salted mask shader, then a run that must not
log "pipeline took" from the game's own draw. Rerun python -m recomp.shader (or delete the salted file
and restore <file>.orig) to undo."""
import random
import shutil
import struct
import sys
from pathlib import Path

p = Path(sys.argv[1])
orig = p.with_name(p.name + ".orig")
if not orig.exists():
    shutil.copy(p, orig)
w = list(struct.unpack(f"<{orig.stat().st_size // 4}I", orig.read_bytes()))
at = 5
while at < len(w) and (w[at] & 0xFFFF) not in (3, 4, 5, 6, 7, 71, 72):  # first debug or annotation
    at += w[at] >> 16
s = f"salt{random.getrandbits(48)}".encode() + b"\0"
s += b"\0" * (-len(s) % 4)
words = list(struct.unpack(f"<{len(s) // 4}I", s))
new_id = w[3]
w[3] += 1
w[at:at] = [((2 + len(words)) << 16) | 7, new_id] + words
p.write_bytes(struct.pack(f"<{len(w)}I", *w))
print(p, "salted")
