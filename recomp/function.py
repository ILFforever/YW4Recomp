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

import os
import re
import struct
from dataclasses import dataclass, field

from . import lift
from .lift import bits, sign_extend


@dataclass
class Code:
    """A flat guest image: `data` at offset 0, instructions in [0, text_end).

    base: the C variable holding the module's load address (decision 0014: main is
    yw4_image_base, the sdk yw4_sdk_base); prefix: what its function names start with."""
    data: bytes
    text_end: int
    base: str = "yw4_image_base"
    prefix: str = "f_"

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

def image_addr(off: int, base: str = "yw4_image_base") -> str:
    return f"({base} + {off:#x}ull)"


def emit_pc_relative(enc: int, pc: int, base: str = "yw4_image_base") -> str | None:
    """ADR, ADRP and integer LDR (literal): the PC is known at translation time."""
    if enc & 0x1F000000 == 0x10000000:                       # ADR, ADRP
        imm = sign_extend(bits(enc, 23, 5) << 2 | bits(enc, 30, 29), 21)
        value = (pc & ~0xFFF) + (imm << 12) if enc >> 31 else pc + imm
        return lift.wr_reg(bits(enc, 4, 0), 1, image_addr(value, base)) or ";"
    if enc & 0x3F000000 == 0x18000000:                       # LDR (literal), integer
        opc, t = bits(enc, 31, 30), bits(enc, 4, 0)
        addr = image_addr(pc + sign_extend(bits(enc, 23, 5), 19) * 4, base)
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
    if enc & 0xFFE0001F == 0xD4000001:                       # SVC #imm: the kernel (decision 0014)
        return f"yw4_svc(ctx, {(enc >> 5) & 0xFFFF:#x});"
    sysreg, t = (enc >> 5) & 0xFFFF, enc & 31
    if enc & 0xFFF00000 == 0xD5300000 and sysreg in MRS_REGS:  # MRS Xt, <reg>
        return f"ctx->x[{t}] = {MRS_REGS[sysreg]};" if t != 31 else ";"
    if enc & 0xFFF00000 == 0xD5100000 and sysreg in MSR_REGS:  # MSR <reg>, Xt
        return MSR_REGS[sysreg].format(x="0" if t == 31 else f"ctx->x[{t}]")
    if enc & 0xFFFFFFE0 in CACHE_OPS:                        # DC CVAU/CVAC/CIVAC, IC IVAU: no cache to maintain
        return ";"
    return None


# System registers the sdk reads and writes (op0:op1:CRn:CRm:op2 = encoding bits 20:5)
MRS_REGS = {
    0xDE83: "ctx->tpidrro_el0",          # TPIDRRO_EL0: the thread-local region, set by the kernel
    0xDE82: "ctx->tpidr_el0",            # TPIDR_EL0
    0xDF01: "yw4_cntpct()",              # CNTPCT_EL0: the 19.2 MHz system counter
    0xD801: "0x8444C004ull",             # CTR_EL0 of the Cortex-A57: 64-byte cache lines
    0xDA20: "(uint64_t)ctx->fpcr",       # FPCR
    0xDA21: "(uint64_t)ctx->fpsr",       # FPSR (not modelled: always what was last written)
}
MSR_REGS = {
    0xDE82: "ctx->tpidr_el0 = {x};",
    0xDA20: "yw4_msr_fpcr(ctx, {x});",   # decision 0008: only the default mode is supported
    0xDA21: "ctx->fpsr = (uint32_t){x};",
}
CACHE_OPS = {0xD50B7B20, 0xD50B7A20, 0xD50B7E20, 0xD50B7520}  # DC CVAU, DC CVAC, DC CIVAC, IC IVAU (Rt cleared)


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
        if e & 0xFF00001E == 0x54000008 or e & 0xFF00001F == 0x54000000:  # HI/LS, or EQ (below)
            eq = e & 0xFF00001F == 0x54000000                # `cmp w, #N; b.eq`: the index was masked to 0..N, so N entries
            o -= 4
            lo_cmp = max(br_off - 4 * 8 * window, lowest)    # the compiler can schedule much code in between
            while o >= lo_cmp:                               # the flag-setting compare before it
                e = code.word(o)
                if e & 0x7F80001F == 0x7100001F and not bits(e, 22, 22):
                    bound = bits(e, 21, 10) - eq             # cmp xN/wN, #imm (subs xzr, ..)
                    if bound < 0:
                        bound = None
                    break
                if e & 0x1F000000 in (0x11000000, 0x0B000000) and bits(e, 29, 29):
                    return None                              # some other flag setter
                o -= 4
            break
        o -= 4
    table = trace_constant(code, ldr_off, rt, lowest)
    if bound is None:
        return None
    if table is not None:
        return table_targets(code, table, bound)
    # The table's address was loaded by code the compiler placed after the branch (the
    # register is set once, in a block laid out later). Take an `adrp xRt` (+ `add xRt, xRt,
    # #imm`) anywhere nearby whose table has only code addresses near the branch in it.
    for cand in nearby_tables(code, rt, lowest, br_off):
        targets = table_targets(code, cand, bound, near=br_off)
        if targets is not None:
            return targets
    return None


def table_targets(code: Code, table: int, bound: int, near: int | None = None) -> list[int] | None:
    """The bound+1 case targets of the int32 offset table at `table`, or None if any isn't code
    (or, with `near`, isn't within 64 KiB of it)."""
    targets = []
    for k in range(bound + 1):
        if table + 4 * k + 4 > len(code.data):
            return None
        t = table + struct.unpack_from("<i", code.data, table + 4 * k)[0]
        if not code.is_code(t) or (near is not None and abs(t - near) > 0x10000):
            return None
        targets.append(t)
    return targets


def nearby_tables(code: Code, reg: int, lowest: int, br_off: int, reach: int = 0x8000) -> list[int]:
    """Addresses `adrp xreg` (+ `add xreg, xreg, #imm` right after) builds within `reach` of the branch."""
    found = []
    for o in range(max(lowest, br_off - reach), min(code.text_end, br_off + reach), 4):
        e = code.word(o)
        if e & 0x9F000000 != 0x90000000 or bits(e, 4, 0) != reg:
            continue
        imm = sign_extend(bits(e, 23, 5) << 2 | bits(e, 30, 29), 21)
        addr = (o & ~0xFFF) + (imm << 12)
        n = code.word(o + 4) if o + 4 < code.text_end else 0
        if n & 0xFFC00000 == 0x91000000 and bits(n, 9, 5) == reg and bits(n, 4, 0) == reg:
            addr += bits(n, 21, 10)
        found.append(addr)
    return found


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

def fname(off: int, prefix: str = "f_") -> str:
    return f"{prefix}{off:x}"


def label(off: int) -> str:
    return f"L_{off:x}"


@dataclass
class EmitStats:
    unimplemented: dict[str, int] = field(default_factory=dict)  # coverage subgroup -> count
    cached: int = 0      # functions whose registers live in locals (cache_registers)
    not_cached: int = 0  # ... and those that keep them in the CPUState


# Register caching (decision 0035). Every guest register a function uses becomes a local
# variable, loaded from the CPUState once at the entry; the CPUState is brought up to date
# before anything else can look at it (a call, a tail call, a return, the kernel) and the
# locals are reloaded after a call returns. Within the function the host compiler can then
# keep guest registers in host registers instead of loading and storing them for every
# instruction (550 ARM instructions of main+0xa77390 were ~12,800 x86 ones before).
# Off (decision 0035): measured slower (main thread 9.3-9.8 ms against 8.0 ms per frame: the write-back
# and reload around every call cost more than the locals save) and it hung the game at start-up in 2 of
# 6 runs (threads waiting on each other, two running: most likely a wait loop whose memory read the
# compiler moved out of the loop once nothing else in the loop touched memory). YW4_CACHE_REGISTERS=1
# turns it on for experiments; it would need a compiler barrier at every loop's back edge first.
CACHE_REGISTERS = os.environ.get("YW4_CACHE_REGISTERS", "0") == "1"
REG_REF = re.compile(r"ctx->(x\[(\d+)\]|v\[(\d+)\]|sp\b|nzcv\b)")
CALL = re.compile(r"\b([fs]_[0-9a-f]+)\(ctx\);")
# runtime functions that take the CPUState and may read or write registers; their statement is
# wrapped whole (they are alone in it, and never assign a register after the call)
KERNEL = re.compile(r"^yw4_(svc|msr_fpcr|guest_trap|unimplemented)\(ctx, [^;]*\);$")
# what may still mention ctx once the rest is handled: fields that stay in the CPUState
OTHER_CTX = re.compile(r"ctx->(excl_size|excl_val|excl_addr|tpidrro_el0|tpidr_el0|fpcr|fpsr)\b")


def local_name(ref: re.Match) -> str:
    kind = ref.group(1)
    if ref.group(2) is not None:
        return f"rx{ref.group(2)}"
    if ref.group(3) is not None:
        return f"rv{ref.group(3)}"
    return f"r{kind}"


def cache_registers(stmts: list[str]) -> tuple[list[str], list[str]] | None:
    """The statements with registers in locals, and the declarations to put at the entry;
    None if a statement passes the CPUState somewhere this doesn't know (left uncached)."""
    used: dict[str, str] = {}  # local -> its CPUState field
    for s in stmts:
        for ref in REG_REF.finditer(s):
            used[local_name(ref)] = ref.group(0)
    if not used:
        return [], stmts
    # Written back: registers the function assigns (whole, a lane, compound) or takes the address
    # of. A register it only reads still has its value in the CPUState.
    renamed = "\n".join(REG_REF.sub(local_name, s) for s in stmts)
    written = {n for n in used
               if re.search(rf"\b{n}(?:\.\w+\[[^\]]*\])?\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|[(,=]\s*&\s*{n}\b|\b{n}\s*(?:\+\+|--)", renamed)}
    order = sorted(used, key=lambda n: (n[1], int(n[2:]) if n[2:].isdigit() else -1, n))
    flush = " ".join(f"{used[n]} = {n};" for n in order if n in written)
    reload = " ".join(f"{n} = {used[n]};" for n in order)
    out = []
    for s in stmts:
        t = REG_REF.sub(local_name, s)
        if KERNEL.match(t):
            out.append(f"{{ {flush} {t} {reload} }}")
            continue
        t = CALL.sub(lambda m: f"{{ {flush} {m.group(1)}(ctx); {reload} }}", t)
        t = t.replace("c_->fn(ctx);", f"{flush} c_->fn(ctx); {reload}")
        t = re.sub(r"\b(TAILCALL|TAILCALL_INDIRECT)\(", lambda m: f"{{ {flush} }} {m.group(1)}(", t)
        if t == "return;":
            t = f"{{ {flush} return; }}"
        # anything else that still hands over the CPUState: leave this function as it was
        rest = t.replace(flush, "").replace(reload, "")
        rest = OTHER_CTX.sub("", rest)
        for done in ("(ctx);", "c_->fn(ctx)"):
            rest = rest.replace(done, "")
        if re.search(r"\bctx\b", rest) or re.search(r"\breturn\b", rest.replace("{  return; }", "")):
            return None
        out.append(t)
    decls = [f"uint64_t {n} = {used[n]};" if n[1] != "v" else f"v128 {n} = {used[n]};" for n in order]
    return decls, out


def emit_function(code: Code, fn: Function, entries: set[int], subgroup=None,
                  stats: EmitStats | None = None, setjmps: frozenset[int] = frozenset(),
                  hook: str | None = None) -> str:
    """setjmps: entries that are setjmp (or a stub that jumps to it). A call to one is a
    host setjmp in this function's frame, registered so that the guest longjmp's jump back
    to the return address can return here (decision 0018).
    hook: a native `int hook(CPUState *)` that runs first; nonzero means it did the
    function's work and set its results, so the body is skipped (decision 0024)."""
    order = sorted(fn.body)
    body = fn.body
    labels: set[int] = set()
    lines: list[str] = []
    njb = 0

    def addr(off: int) -> str:
        return image_addr(off, code.base)

    def name(off: int) -> str:
        return fname(off, code.prefix)

    def jump(t: int) -> str:
        """goto inside the function, tail call into another one."""
        if t in body and (t == fn.entry or t not in entries):
            labels.add(t)
            return f"goto {label(t)};"
        if t in entries:
            return f"TAILCALL({name(t)});"
        return f"yw4_unimplemented(ctx, {addr(t)}, 0); /* branch to {t:#x}: not discovered */"

    for i, off in enumerate(order):
        enc = code.word(off)
        f = decode_flow(enc, off)
        pc = addr(off)
        falls_through = True
        if f is None:
            stmt = emit_pc_relative(enc, off, code.base) or emit_system(enc)
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
            elif f.target in setjmps:
                stmt = (f"ctx->x[30] = {addr(off + 4)}; if (!setjmp(yw4_jb[{njb}])) "
                        f"{{ yw4_setjmp_register(ctx, {addr(off + 4)}, yw4_jb[{njb}]); {name(f.target)}(ctx); }}")
                njb += 1
            else:
                stmt = f"ctx->x[30] = {addr(off + 4)}; {name(f.target)}(ctx);"
        elif f.kind == "blr":
            stmt = (f"{{ uint64_t t = {lift.rd_reg(f.reg, 1)}; ctx->x[30] = {addr(off + 4)}; "
                    f"YW4_TARGET(t, c_) c_->fn(ctx); }}")
        elif f.kind == "br":
            target = lift.rd_reg(f.reg, 1)
            if off in fn.jump_tables:
                cases = " ".join(f"case {t:#x}: {jump(t)}" for t in sorted(set(fn.jump_tables[off])))
                stmt = (f"switch ({target} - {code.base}) {{ {cases} "
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

    # restrict: guest memory never overlaps the CPUState, so the compiler may keep registers
    # in host registers across guest loads and stores. Not with setjmp: after a longjmp back,
    # cached values would be stale.
    out = [f"void {name(fn.entry)}(CPUState *{'' if njb else 'restrict '}ctx) {{"]
    if njb:  # host jump buffers live as long as this frame does
        out.append(f"    jmp_buf yw4_jb[{njb}];")
    # Probes (runtime/src/dispatch.c): log calls to chosen functions without rebuilding.
    # Before the entry label, so a loop back to the entry doesn't count as a call.
    out.append(f"    if (__builtin_expect(yw4_probe_any, 0)) yw4_probe(ctx, {addr(fn.entry)});")
    if hook:
        out.insert(0, f"int {hook}(CPUState *ctx);  /* hook (recomp/overrides.toml) */")
        out.append(f"    if ({hook}(ctx)) return;")
    # registers in locals (decision 0035); not with setjmp (a longjmp back would find stale locals)
    cached = None if njb or not CACHE_REGISTERS else cache_registers([s for _, s in lines])
    if cached is not None:
        decls, stmts = cached
        out += [f"    {d}" for d in decls]
        lines = [(off, s) for (off, _), s in zip(lines, stmts)]
    if stats is not None:
        if cached is not None:
            stats.cached += 1
        else:
            stats.not_cached += 1
    # Blocks are emitted in address order, and a function may own code below its entry
    # (a block it only jumps back to, e.g. main+0xa7d8b4 in main+0xa7d8bc): start at the entry.
    if order and order[0] != fn.entry:
        labels.add(fn.entry)
        out.append(f"    goto {label(fn.entry)};")
    for off, stmt in lines:
        if off is not None and off in labels:
            out.append(f"{label(off)}:")
        out.append(f"    {stmt}")
    out.append("}")
    return "\n".join(out)
