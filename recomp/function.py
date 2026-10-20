"""Translate whole guest functions to C (decision 0007).

A guest function becomes `void f_<offset>(CPUState *ctx)`:

    straight-line code   recomp.lift, one C statement block per instruction
    B / B.cond / CBZ / TBZ inside the function   goto L_<offset>
    branch to another function's entry           tail call (TAILCALL)
    BL                   set x30 to the guest return address, call f_<target>
    BLR                  same, through yw4_resolve(target)
    BR                   jump table -> switch on the target address; otherwise an indirect tail call
    RET                  return
    ADR / ADRP / LDR (literal)   yw4_image_base + a constant offset

Function bodies are found by following control flow from each entry point, not
from Ghidra's sizes (which cut some functions short). Code reachable from two
entries is translated into both functions, which is correct, only bigger.

Offsets are from the start of the image (the same offsets as everywhere else
in the project); the image is loaded at yw4_image_base.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from . import lift
from .lift import bits, sign_extend


@dataclass
class Code:
    """A flat guest image: `data` at offset 0, instructions in [0, text_end)."""
    data: bytes
    text_end: int

    def word(self, off: int) -> int:
        return struct.unpack_from("<I", self.data, off)[0]

    def is_code(self, off: int) -> bool:
        return 0 <= off < self.text_end and off % 4 == 0


# --------------------------------------------------------------------------- control flow decode

@dataclass(frozen=True)
class Flow:
    kind: str                 # b, cond, bl, blr, br, ret, trap
    target: int | None = None
    cond: str | None = None   # C expression: branch taken (for kind == "cond")
    reg: int | None = None    # BLR/BR/RET register


def decode_flow(enc: int, pc: int) -> Flow | None:
    """Control-flow instructions; None for everything else."""
    if enc & 0x7C000000 == 0x14000000:                       # B, BL
        target = pc + sign_extend(bits(enc, 25, 0), 26) * 4
        return Flow("bl" if enc >> 31 else "b", target)
    if enc & 0xFF000010 == 0x54000000:                       # B.cond
        target = pc + sign_extend(bits(enc, 23, 5), 19) * 4
        return Flow("cond", target, f"cond_holds(ctx->nzcv, {bits(enc, 3, 0)})")
    if enc & 0x7E000000 == 0x34000000:                       # CBZ, CBNZ
        target = pc + sign_extend(bits(enc, 23, 5), 19) * 4
        op = "!=" if bits(enc, 24, 24) else "=="
        return Flow("cond", target, f"{lift.rd_reg(bits(enc, 4, 0), bits(enc, 31, 31))} {op} 0")
    if enc & 0x7E000000 == 0x36000000:                       # TBZ, TBNZ
        target = pc + sign_extend(bits(enc, 18, 5), 14) * 4
        bit = bits(enc, 31, 31) << 5 | bits(enc, 23, 19)
        op = "!=" if bits(enc, 24, 24) else "=="
        return Flow("cond", target, f"((({lift.rd_reg(bits(enc, 4, 0), 1)}) >> {bit}) & 1) {op} 0")
    if enc & 0xFFFFFC1F == 0xD63F0000:
        return Flow("blr", reg=bits(enc, 9, 5))
    if enc & 0xFFFFFC1F == 0xD61F0000:
        return Flow("br", reg=bits(enc, 9, 5))
    if enc & 0xFFFFFC1F == 0xD65F0000:
        return Flow("ret", reg=bits(enc, 9, 5))
    if enc & 0xFFFF0000 == 0 or enc & 0xFFE0001F in (0xD4200000, 0xD4400000):  # UDF, BRK, HLT
        return Flow("trap")
    if enc == 0xE7FFDEFE:  # unallocated in A64; main uses it as a trap word on assert-failure paths
        return Flow("trap")
    return None


# --------------------------------------------------------------------------- PC-relative and system

def image_addr(off: int) -> str:
    return f"(yw4_image_base + {off:#x}ull)"


def emit_pc_relative(enc: int, pc: int) -> str | None:
    """ADR, ADRP and integer LDR (literal): the PC is known at translation time."""
    if enc & 0x1F000000 == 0x10000000:                       # ADR, ADRP
        imm = sign_extend(bits(enc, 23, 5) << 2 | bits(enc, 30, 29), 21)
        value = (pc & ~0xFFF) + (imm << 12) if enc >> 31 else pc + imm
        return lift.wr_reg(bits(enc, 4, 0), 1, image_addr(value)) or ";"
    if enc & 0x3F000000 == 0x18000000:                       # LDR (literal), integer
        opc, t = bits(enc, 31, 30), bits(enc, 4, 0)
        addr = image_addr(pc + sign_extend(bits(enc, 23, 5), 19) * 4)
        if opc == 0:
            return lift.wr_reg(t, 1, f"ld32({addr})") or ";"
        if opc == 1:
            return lift.wr_reg(t, 1, f"ld64({addr})") or ";"
        if opc == 2:
            return lift.wr_reg(t, 1, f"(uint64_t)(int64_t)(int32_t)ld32({addr})") or ";"
        return ";"  # PRFM (literal)
    return None


def emit_system(enc: int) -> str | None:
    if enc & 0xFFFFF01F == 0xD503201F:                       # NOP, YIELD, WFE, ... (hints)
        return ";"
    if enc & 0xFFFFF0FF in (0xD50330BF, 0xD503309F):         # DMB, DSB
        return "__atomic_thread_fence(__ATOMIC_SEQ_CST);"
    if enc & 0xFFFFF0FF == 0xD50330DF:                       # ISB
        return ";"
    if enc & 0xFFFFF0FF == 0xD503305F:                       # CLREX: drop the exclusive reservation
        return "ctx->excl_size = 0;"
    return None


# --------------------------------------------------------------------------- jump tables

def written_regs(code: Code, off: int) -> set[int]:
    """X registers an instruction may write. Over-approximates: a wrong "yes" only makes
    the jump-table search give up."""
    enc = code.word(off)
    f = decode_flow(enc, off)
    if f:
        return set(range(19)) | {30} if f.kind in ("bl", "blr") else set()  # calls clobber x0-x18, x30
    form = lift.find_form(enc)
    if form and form.mem:
        a = form.mem(enc)
        if a:
            regs = set(a.regs) if a.load else set()
            if a.writeback:
                regs.add(a.n)
            return regs - {31}
    if enc & 0x3F000000 == 0x08000000:                       # exclusives: Rt and the status Rs
        return {bits(enc, 4, 0), bits(enc, 20, 16)}
    if enc & 0x3A000000 == 0x28000000:                       # other pairs (SIMD): writeback only
        return {bits(enc, 9, 5)}
    if enc & 0x0E000000 == 0x0E000000:                       # SIMD/FP data processing
        to_gp = enc & 0x5F20FC00 == 0x1E200000 or enc & 0x9FE0FC00 in (0x0E002C00, 0x0E003C00)
        return {bits(enc, 4, 0)} if to_gp else set()         # FMOV/FCVT* to GP, SMOV/UMOV
    return {bits(enc, 4, 0)}


def trace_constant(code: Code, off: int, reg: int, lowest: int) -> int | None:
    """Image offset held in `reg` just before `off`, if the last write to it (scanning back
    to `lowest`) is an ADRP, optionally followed by ADD #imm and register moves."""
    extra = 0
    for o in range(off - 4, lowest - 4, -4):
        if reg not in written_regs(code, o):
            continue
        e = code.word(o)
        if e & 0xFFC00000 == 0x91000000:                     # add xR, xS, #imm
            extra += bits(e, 21, 10)
            reg = bits(e, 9, 5)
        elif e & 0xFFE0FFE0 == 0xAA0003E0:                   # mov xR, xS
            reg = bits(e, 20, 16)
        elif e & 0x9F000000 == 0x90000000:                   # adrp xR, page
            imm = sign_extend(bits(e, 23, 5) << 2 | bits(e, 30, 29), 21)
            return (o & ~0xFFF) + (imm << 12) + extra
        else:
            return None
        if reg == 31:
            return None
    return None


def find_jump_table(code: Code, br_off: int, reg: int, lowest: int, window: int = 32) -> list[int] | None:
    """Targets of a Clang switch table ending in `br reg`, or None if the pattern isn't there.

        cmp   wI, #N             ; bound: N+1 entries
        b.hi  default
        ...                      ; xT = table, set by adrp/add (often hoisted out of a loop)
        ldrsw xA, [xT, xIdx, lsl #2]
        add   reg, xA, xT        ; entries are int32 offsets from the table
        br    reg

    Unrelated instructions may be interleaved. The result only decides which labels the
    BR's switch can jump to and which code gets discovered; any other target still goes
    through yw4_resolve, so a wrong guess fails loudly rather than silently.
    """
    lo = max(br_off - 4 * window, lowest)
    o = br_off - 4
    while o >= lo:                                           # add reg, xA, xT (no shift)
        e = code.word(o)
        if e & 0xFFE0FC00 == 0x8B000000 and bits(e, 4, 0) == reg:
            break
        if reg in written_regs(code, o):
            return None
        o -= 4
    else:
        return None
    pair = {bits(e, 9, 5), bits(e, 20, 16)}
    o -= 4
    while o >= lo:                                           # ldrsw xA, [xT, xIdx, lsl #2]
        e = code.word(o)
        if e & 0xFFE0FC00 == 0xB8A07800 and bits(e, 4, 0) in pair and bits(e, 9, 5) in pair:
            break
        o -= 4
    else:
        return None
    ldr_off, rt = o, bits(e, 9, 5)
    bound = None
    o -= 4
    while o >= lo:                                           # b.hi / b.ls guarding the table
        e = code.word(o)
        if e & 0xFF00001E == 0x54000008:                     # cond 8 (HI) or 9 (LS)
            o -= 4
            while o >= lo:                                   # the flag-setting compare before it
                e = code.word(o)
                if e & 0x7F80001F == 0x7100001F and not bits(e, 22, 22):
                    bound = bits(e, 21, 10)                  # cmp xN/wN, #imm (subs xzr, ..)
                    break
                if e & 0x1F000000 in (0x11000000, 0x0B000000) and bits(e, 29, 29):
                    return None                              # some other flag setter
                o -= 4
            break
        o -= 4
    table = trace_constant(code, ldr_off, rt, lowest)
    if bound is None or table is None:
        return None
    targets = []
    for k in range(bound + 1):
        if table + 4 * k + 4 > len(code.data):
            return None
        t = table + struct.unpack_from("<i", code.data, table + 4 * k)[0]
        if not code.is_code(t):
            return None
        targets.append(t)
    return targets


# --------------------------------------------------------------------------- discovery

@dataclass
class Function:
    entry: int
    body: set[int] = field(default_factory=set)
    jump_tables: dict[int, list[int]] = field(default_factory=dict)  # BR offset -> targets
    problems: list[str] = field(default_factory=list)


def discover(code: Code, entry: int, entries: set[int]) -> Function:
    """Every instruction reachable from `entry` without entering another function."""
    fn = Function(entry)
    work = [entry]

    def follow(t: int) -> None:
        if t == entry or t not in entries:
            work.append(t)

    while work:
        off = work.pop()
        while off not in fn.body:
            if not code.is_code(off):
                fn.problems.append(f"flow leaves .text at {off:#x}")
                break
            fn.body.add(off)
            enc = code.word(off)
            f = decode_flow(enc, off)
            nxt = off + 4
            if f is None or f.kind in ("bl", "blr"):
                pass
            elif f.kind == "cond":
                follow(f.target)
            elif f.kind == "b":
                follow(f.target)
                break
            elif f.kind == "br":
                targets = find_jump_table(code, off, f.reg, entry)
                if targets:
                    fn.jump_tables[off] = targets
                    for t in targets:
                        follow(t)
                break
            else:  # ret, trap
                break
            if nxt in entries and nxt != entry:
                break  # falls into the next function: a tail call, emitted at the end of the block
            off = nxt
    return fn


# --------------------------------------------------------------------------- emission

def fname(off: int) -> str:
    return f"f_{off:x}"


def label(off: int) -> str:
    return f"L_{off:x}"


@dataclass
class EmitStats:
    unimplemented: dict[str, int] = field(default_factory=dict)  # coverage subgroup -> count


def emit_function(code: Code, fn: Function, entries: set[int], subgroup=None,
                  stats: EmitStats | None = None) -> str:
    order = sorted(fn.body)
    body = fn.body
    labels: set[int] = set()
    lines: list[str] = []

    def jump(t: int) -> str:
        """goto inside the function, tail call into another one."""
        if t in body and (t == fn.entry or t not in entries):
            labels.add(t)
            return f"goto {label(t)};"
        if t in entries:
            return f"TAILCALL({fname(t)});"
        return f"yw4_unimplemented(ctx, {image_addr(t)}, 0); /* branch to {t:#x}: not discovered */"

    for i, off in enumerate(order):
        enc = code.word(off)
        f = decode_flow(enc, off)
        pc = image_addr(off)
        falls_through = True
        if f is None:
            stmt = emit_pc_relative(enc, off) or emit_system(enc)
            if stmt is None:
                try:
                    stmt = lift.lift(enc)
                except lift.LiftError:
                    stmt = f"yw4_unimplemented(ctx, {pc}, {enc:#010x}u);"
                    falls_through = False
                    if stats is not None and subgroup:
                        g = subgroup(enc)
                        stats.unimplemented[g] = stats.unimplemented.get(g, 0) + 1
        elif f.kind == "b":
            stmt, falls_through = jump(f.target), False
        elif f.kind == "cond":
            stmt = f"if ({f.cond}) {{ {jump(f.target)} }}"
        elif f.kind == "bl":
            if f.target not in entries:
                stmt = f"yw4_unimplemented(ctx, {pc}, {enc:#010x}u); /* call to non-entry {f.target:#x} */"
            else:
                stmt = f"ctx->x[30] = {image_addr(off + 4)}; {fname(f.target)}(ctx);"
        elif f.kind == "blr":
            stmt = (f"{{ uint64_t t = {lift.rd_reg(f.reg, 1)}; ctx->x[30] = {image_addr(off + 4)}; "
                    f"yw4_resolve(t)(ctx); }}")
        elif f.kind == "br":
            target = lift.rd_reg(f.reg, 1)
            if off in fn.jump_tables:
                cases = " ".join(f"case {t:#x}: {jump(t)}" for t in sorted(set(fn.jump_tables[off])))
                stmt = (f"switch ({target} - yw4_image_base) {{ {cases} "
                        f"default: TAILCALL_INDIRECT({target}); }}")
            else:
                stmt = f"TAILCALL_INDIRECT({target});"
            falls_through = False
        elif f.kind == "ret":
            stmt, falls_through = "return;", False
        else:
            stmt, falls_through = f"yw4_guest_trap(ctx, {pc}, {enc:#010x}u);", False
        lines.append((off, stmt))
        if falls_through:
            nxt = off + 4
            if i + 1 >= len(order) or order[i + 1] != nxt:
                lines.append((None, jump(nxt)))

    out = [f"void {fname(fn.entry)}(CPUState *ctx) {{"]
    for off, stmt in lines:
        if off is not None and off in labels:
            out.append(f"{label(off)}:")
        out.append(f"    {stmt}")
    out.append("}")
    return "\n".join(out)
