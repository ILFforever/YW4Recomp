"""Translate single ARM64 instructions to C statements.

Each instruction *form* (a group of encodings sharing one bit layout, e.g.
"add/sub immediate") is a `Form` with:

    mask, value   enc & mask == value identifies the form
    valid(enc)    False for unallocated/reserved encodings within the form
    emit(enc)     C statements operating on `ctx` (a CPUState *)
    mem(enc)      loads/stores only: the decoded MemAccess (the test generator
                  uses it to point the base register into its scratch buffer)
    vregs(enc)    SIMD/FP forms: the V registers it uses (tests must avoid v31)

Semantics follow the Arm ARM pseudocode (DDI 0487), ARMv8.0 only.
Register field 31 means XZR/WZR unless the form says it means SP.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable


class LiftError(Exception):
    pass


def bits(enc: int, hi: int, lo: int) -> int:
    return (enc >> lo) & ((1 << (hi - lo + 1)) - 1)


def ones(n: int) -> int:
    return (1 << n) - 1


# --------------------------------------------------------------------------- register access

def utype(sf: int) -> str:
    return "uint64_t" if sf else "uint32_t"


def rd_reg(n: int, sf: int, sp: bool = False) -> str:
    """C expression reading register n at the operation width."""
    if n == 31:
        base = "ctx->sp" if sp else "0"
    else:
        base = f"ctx->x[{n}]"
    return f"(({utype(sf)}){base})"


def wr_reg(d: int, sf: int, expr: str, sp: bool = False) -> str:
    """C statement writing a result. 32-bit results are zero-extended to 64 bits."""
    if d == 31 and not sp:
        return ""  # write to XZR is discarded
    target = "ctx->sp" if d == 31 else f"ctx->x[{d}]"
    return f"{target} = (uint64_t)({utype(sf)})({expr});"


def shift_expr(v: str, kind: int, amount: int, sf: int) -> str:
    """ShiftReg with a constant amount (LSL, LSR, ASR, ROR)."""
    w = 64 if sf else 32
    if amount == 0:
        return v
    if kind == 0:
        return f"({utype(sf)})({v} << {amount})"
    if kind == 1:
        return f"({v} >> {amount})"
    if kind == 2:
        return f"asr{w}({v}, {amount})"
    return f"ror{w}({v}, {amount})"


def extend_expr(m: int, option: int, shift: int, sf: int) -> str:
    """ExtendReg: UXTB/UXTH/UXTW/UXTX/SXTB/SXTH/SXTW/SXTX, then LSL by 0..4."""
    src = "0" if m == 31 else f"ctx->x[{m}]"
    ctype = ["uint8_t", "uint16_t", "uint32_t", "uint64_t",
             "int8_t", "int16_t", "int32_t", "int64_t"][option]
    signed = option >= 4
    v = f"(uint64_t)(int64_t)({ctype}){src}" if signed else f"(uint64_t)({ctype}){src}"
    v = f"({v} << {shift})" if shift else f"({v})"
    return f"(({utype(sf)}){v})"


# --------------------------------------------------------------------------- bit masks

def highest_set_bit(x: int) -> int:
    return x.bit_length() - 1


def ror(v: int, s: int, width: int) -> int:
    s %= width
    v &= ones(width)
    return ((v >> s) | (v << (width - s))) & ones(width) if s else v


def replicate(v: int, esize: int, width: int) -> int:
    out = 0
    for i in range(0, width, esize):
        out |= v << i
    return out


def decode_bit_masks(n: int, imms: int, immr: int, immediate: bool, width: int) -> tuple[int, int] | None:
    """Arm ARM DecodeBitMasks. Returns (wmask, tmask) or None if reserved."""
    length = highest_set_bit((n << 6) | (~imms & 0x3F))
    if length < 1:
        return None
    levels = ones(length)
    if immediate and (imms & levels) == levels:
        return None
    s = imms & levels
    r = immr & levels
    esize = 1 << length
    if esize > width:
        return None
    d = (s - r) & levels
    welem = ones(s + 1)
    telem = ones(d + 1)
    return replicate(ror(welem, r, esize), esize, width), replicate(telem, esize, width)


# --------------------------------------------------------------------------- forms

@dataclass(frozen=True)
class Form:
    name: str
    mask: int
    value: int
    valid: Callable[[int], bool]
    emit: Callable[[int], str]
    mem: Callable[[int], "MemAccess | None"] | None = None
    vregs: Callable[[int], tuple[int, ...]] | None = None


FORMS: list[Form] = []


def form(name: str, mask: int, value: int, valid=lambda e: True, vregs=None):
    def register(fn: Callable[[int], str]) -> Callable[[int], str]:
        FORMS.append(Form(name, mask, value, valid, fn, vregs=vregs))
        return fn
    return register


def addsub(sf: int, d: int, n_expr: str, m_expr: str, sub: int, setflags: int, d_sp: bool) -> str:
    w = 64 if sf else 32
    if setflags:
        m = f"~{m_expr}" if sub else m_expr
        return (f"{{ uint64_t f; {utype(sf)} r = awc{w}({n_expr}, ({utype(sf)}){m}, {sub}, &f); "
                f"{wr_reg(d, sf, 'r') or '(void)r;'} ctx->nzcv = f; }}")
    op = "-" if sub else "+"
    return wr_reg(d, sf, f"{n_expr} {op} {m_expr}", sp=d_sp)


# Add/subtract (immediate): sf op S 100010 sh imm12 Rn Rd
@form("addsub_imm", 0x1F800000, 0x11000000)
def _addsub_imm(e: int) -> str:
    sf, sub, s, sh, imm = bits(e, 31, 31), bits(e, 30, 30), bits(e, 29, 29), bits(e, 22, 22), bits(e, 21, 10)
    n, d = bits(e, 9, 5), bits(e, 4, 0)
    imm <<= 12 * sh
    return addsub(sf, d, rd_reg(n, sf, sp=True), f"({utype(sf)}){imm:#x}", sub, s, d_sp=not s)


# Add/subtract (shifted register): sf op S 01011 shift 0 Rm imm6 Rn Rd
@form("addsub_shift", 0x1F200000, 0x0B000000,
      valid=lambda e: bits(e, 23, 22) != 3 and (bits(e, 31, 31) or not bits(e, 15, 15)))
def _addsub_shift(e: int) -> str:
    sf, sub, s = bits(e, 31, 31), bits(e, 30, 30), bits(e, 29, 29)
    shift, m, imm6, n, d = bits(e, 23, 22), bits(e, 20, 16), bits(e, 15, 10), bits(e, 9, 5), bits(e, 4, 0)
    return addsub(sf, d, rd_reg(n, sf), shift_expr(rd_reg(m, sf), shift, imm6, sf), sub, s, d_sp=False)


# Add/subtract (extended register): sf op S 01011 00 1 Rm option imm3 Rn Rd
@form("addsub_ext", 0x1FE00000, 0x0B200000,
      valid=lambda e: bits(e, 12, 10) <= 4)
def _addsub_ext(e: int) -> str:
    sf, sub, s = bits(e, 31, 31), bits(e, 30, 30), bits(e, 29, 29)
    m, option, imm3, n, d = bits(e, 20, 16), bits(e, 15, 13), bits(e, 12, 10), bits(e, 9, 5), bits(e, 4, 0)
    return addsub(sf, d, rd_reg(n, sf, sp=True), extend_expr(m, option, imm3, sf), sub, s, d_sp=not s)


# Add/subtract with carry: sf op S 11010000 Rm 000000 Rn Rd
@form("adc_sbc", 0x1FE0FC00, 0x1A000000)
def _adc_sbc(e: int) -> str:
    sf, sub, s = bits(e, 31, 31), bits(e, 30, 30), bits(e, 29, 29)
    m, n, d = bits(e, 20, 16), bits(e, 9, 5), bits(e, 4, 0)
    w = 64 if sf else 32
    mv = f"~{rd_reg(m, sf)}" if sub else rd_reg(m, sf)
    flags = "ctx->nzcv = f; " if s else ""
    return (f"{{ uint64_t f; {utype(sf)} r = awc{w}({rd_reg(n, sf)}, ({utype(sf)}){mv}, "
            f"(unsigned)((ctx->nzcv >> 29) & 1), &f); {wr_reg(d, sf, 'r') or '(void)r;'} {flags}}}")


LOGIC_OPS = {0: "&", 1: "|", 2: "^", 3: "&"}  # AND, ORR, EOR, ANDS


def ands(sf: int, d: int, expr: str) -> str:
    """ANDS/BICS: the result, then NZ from it and C, V cleared."""
    return f"{{ {utype(sf)} r = {expr}; {wr_reg(d, sf, 'r')} ctx->nzcv = nz{64 if sf else 32}(r); }}"


def _logical_imm_valid(e: int) -> bool:
    sf, nbit = bits(e, 31, 31), bits(e, 22, 22)
    if not sf and nbit:
        return False
    return decode_bit_masks(nbit, bits(e, 15, 10), bits(e, 21, 16), True, 64 if sf else 32) is not None


# Logical (immediate): sf opc 100100 N immr imms Rn Rd
@form("logical_imm", 0x1F800000, 0x12000000, valid=_logical_imm_valid)
def _logical_imm(e: int) -> str:
    sf, opc, nbit = bits(e, 31, 31), bits(e, 30, 29), bits(e, 22, 22)
    immr, imms, n, d = bits(e, 21, 16), bits(e, 15, 10), bits(e, 9, 5), bits(e, 4, 0)
    imm, _ = decode_bit_masks(nbit, imms, immr, True, 64 if sf else 32)
    expr = f"{rd_reg(n, sf)} {LOGIC_OPS[opc]} ({utype(sf)}){imm:#x}"
    if opc == 3:
        return ands(sf, d, expr)
    return wr_reg(d, sf, expr, sp=True)


# Logical (shifted register): sf opc 01010 shift N Rm imm6 Rn Rd
@form("logical_shift", 0x1F000000, 0x0A000000, valid=lambda e: bits(e, 31, 31) or not bits(e, 15, 15))
def _logical_shift(e: int) -> str:
    sf, opc, shift, inv = bits(e, 31, 31), bits(e, 30, 29), bits(e, 23, 22), bits(e, 21, 21)
    m, imm6, n, d = bits(e, 20, 16), bits(e, 15, 10), bits(e, 9, 5), bits(e, 4, 0)
    mv = shift_expr(rd_reg(m, sf), shift, imm6, sf)
    if inv:
        mv = f"({utype(sf)})~{mv}"   # BIC, ORN, EON, BICS
    expr = f"{rd_reg(n, sf)} {LOGIC_OPS[opc]} {mv}"
    if opc == 3:
        return ands(sf, d, expr)
    return wr_reg(d, sf, expr)


# Move wide (immediate): sf opc 100101 hw imm16 Rd
@form("movewide", 0x1F800000, 0x12800000,
      valid=lambda e: bits(e, 30, 29) != 1 and (bits(e, 31, 31) or bits(e, 22, 21) < 2))
def _movewide(e: int) -> str:
    sf, opc, hw, imm16, d = bits(e, 31, 31), bits(e, 30, 29), bits(e, 22, 21), bits(e, 20, 5), bits(e, 4, 0)
    pos = hw * 16
    if opc == 0:   # MOVN
        return wr_reg(d, sf, f"~({utype(sf)}){imm16 << pos:#x}")
    if opc == 2:   # MOVZ
        return wr_reg(d, sf, f"({utype(sf)}){imm16 << pos:#x}")
    keep = ~(0xFFFF << pos) & ones(64 if sf else 32)  # MOVK
    return wr_reg(d, sf, f"({rd_reg(d, sf)} & ({utype(sf)}){keep:#x}) | ({utype(sf)}){imm16 << pos:#x}")


def _bitfield_valid(e: int) -> bool:
    sf, opc, nbit = bits(e, 31, 31), bits(e, 30, 29), bits(e, 22, 22)
    if opc == 3 or nbit != sf:
        return False
    if not sf and (bits(e, 21, 21) or bits(e, 15, 15)):
        return False
    return decode_bit_masks(nbit, bits(e, 15, 10), bits(e, 21, 16), False, 64 if sf else 32) is not None


# Bitfield: sf opc 100110 N immr imms Rn Rd  (SBFM, BFM, UBFM; includes LSL/LSR/ASR/SXT*/UXT*/BFI/...)
@form("bitfield", 0x1F800000, 0x13000000, valid=_bitfield_valid)
def _bitfield(e: int) -> str:
    sf, opc, nbit = bits(e, 31, 31), bits(e, 30, 29), bits(e, 22, 22)
    immr, imms, n, d = bits(e, 21, 16), bits(e, 15, 10), bits(e, 9, 5), bits(e, 4, 0)
    w = 64 if sf else 32
    t = utype(sf)
    wmask, tmask = decode_bit_masks(nbit, imms, immr, False, w)
    bot = f"(ror{w}({rd_reg(n, sf)}, {immr}) & ({t}){wmask:#x})"
    if opc == 0:   # SBFM: bits above the field are copies of src bit <imms>
        top = f"(({t})0 - (({rd_reg(n, sf)} >> {imms}) & 1))"
        expr = f"({top} & ({t}){~tmask & ones(w):#x}) | ({bot} & ({t}){tmask:#x})"
    elif opc == 1:  # BFM: merge into the destination
        dst = rd_reg(d, sf)
        merged = f"(({dst} & ({t}){~wmask & ones(w):#x}) | {bot})"
        expr = f"({dst} & ({t}){~tmask & ones(w):#x}) | ({merged} & ({t}){tmask:#x})"
    else:           # UBFM
        expr = f"{bot} & ({t}){tmask:#x}"
    return wr_reg(d, sf, expr)


# Extract: sf 00 100111 N 0 Rm imms Rn Rd
@form("extr", 0x7FA00000, 0x13800000,
      valid=lambda e: bits(e, 22, 22) == bits(e, 31, 31) and (bits(e, 31, 31) or not bits(e, 15, 15)))
def _extr(e: int) -> str:
    sf, m, lsb, n, d = bits(e, 31, 31), bits(e, 20, 16), bits(e, 15, 10), bits(e, 9, 5), bits(e, 4, 0)
    w = 64 if sf else 32
    if lsb == 0:
        return wr_reg(d, sf, rd_reg(m, sf))
    return wr_reg(d, sf, f"({rd_reg(m, sf)} >> {lsb}) | ({utype(sf)})({rd_reg(n, sf)} << {w - lsb})")


DP2 = {0b000010: "udiv", 0b000011: "sdiv", 0b001000: "lsl", 0b001001: "lsr", 0b001010: "asr", 0b001011: "ror"}


# Data-processing (2 source): sf 0 0 11010110 Rm opcode Rn Rd
@form("dp2", 0x7FE00000, 0x1AC00000, valid=lambda e: bits(e, 15, 10) in DP2)
def _dp2(e: int) -> str:
    sf, m, op, n, d = bits(e, 31, 31), bits(e, 20, 16), bits(e, 15, 10), bits(e, 9, 5), bits(e, 4, 0)
    w = 64 if sf else 32
    a, b = rd_reg(n, sf), rd_reg(m, sf)
    kind = DP2[op]
    if kind in ("udiv", "sdiv"):
        return wr_reg(d, sf, f"{kind}{w}({a}, {b})")
    amount = f"(unsigned)({b} & {w - 1})"
    if kind == "lsl":
        return wr_reg(d, sf, f"({utype(sf)})({a} << {amount})")
    if kind == "lsr":
        return wr_reg(d, sf, f"{a} >> {amount}")
    return wr_reg(d, sf, f"{kind}{w}({a}, {amount})")


DP1 = {0b000000: "rbit", 0b000001: "rev16", 0b000010: "rev32", 0b000011: "rev64", 0b000100: "clz", 0b000101: "cls"}


# Data-processing (1 source): sf 1 0 11010110 00000 opcode Rn Rd
@form("dp1", 0x7FFF0000, 0x5AC00000,
      valid=lambda e: bits(e, 15, 10) in DP1 and not (bits(e, 15, 10) == 3 and not bits(e, 31, 31)))
def _dp1(e: int) -> str:
    sf, op, n, d = bits(e, 31, 31), bits(e, 15, 10), bits(e, 9, 5), bits(e, 4, 0)
    w = 64 if sf else 32
    a = rd_reg(n, sf)
    kind = DP1[op]
    if kind == "rev16":
        return wr_reg(d, sf, f"rev16_{w}({a})")
    if kind == "rev32":   # REV32 on 64-bit, plain REV on 32-bit
        return wr_reg(d, sf, f"rev32_64({a})" if sf else f"__builtin_bswap32({a})")
    if kind == "rev64":
        return wr_reg(d, sf, f"__builtin_bswap64({a})")
    return wr_reg(d, sf, f"{kind}{w}({a})")


DP3_VALID = {(0b000, 0), (0b000, 1), (0b001, 0), (0b001, 1), (0b010, 0), (0b101, 0), (0b101, 1), (0b110, 0)}


def _dp3_valid(e: int) -> bool:
    sf, op54, op31, o0 = bits(e, 31, 31), bits(e, 30, 29), bits(e, 23, 21), bits(e, 15, 15)
    if op54 or (op31, o0) not in DP3_VALID:
        return False
    if op31 in (0b010, 0b110) and bits(e, 14, 10) != 31:
        return False  # SMULH/UMULH: Ra must be 11111 (other values are UNPREDICTABLE)
    return op31 == 0 or sf == 1


# Data-processing (3 source): sf op54 11011 op31 Rm o0 Ra Rn Rd
@form("dp3", 0x1F000000, 0x1B000000, valid=_dp3_valid)
def _dp3(e: int) -> str:
    sf, op31, m, o0 = bits(e, 31, 31), bits(e, 23, 21), bits(e, 20, 16), bits(e, 15, 15)
    a, n, d = bits(e, 14, 10), bits(e, 9, 5), bits(e, 4, 0)
    t = utype(sf)
    sign = "-" if o0 else "+"
    if op31 == 0:      # MADD / MSUB
        return wr_reg(d, sf, f"{rd_reg(a, sf)} {sign} ({t})({rd_reg(n, sf)} * {rd_reg(m, sf)})")
    if op31 == 0b010:  # SMULH
        return wr_reg(d, 1, f"smulh64({rd_reg(n, 1)}, {rd_reg(m, 1)})")
    if op31 == 0b110:  # UMULH
        return wr_reg(d, 1, f"umulh64({rd_reg(n, 1)}, {rd_reg(m, 1)})")
    if op31 == 0b001:  # SMADDL / SMSUBL
        prod = f"(uint64_t)((int64_t)(int32_t){rd_reg(n, 0)} * (int64_t)(int32_t){rd_reg(m, 0)})"
    else:              # UMADDL / UMSUBL
        prod = f"((uint64_t){rd_reg(n, 0)} * (uint64_t){rd_reg(m, 0)})"
    return wr_reg(d, 1, f"{rd_reg(a, 1)} {sign} {prod}")


# Conditional select: sf op 0 11010100 Rm cond 0 o2 Rn Rd
@form("csel", 0x3FE00800, 0x1A800000)
def _csel(e: int) -> str:
    sf, op, m, cond, o2 = bits(e, 31, 31), bits(e, 30, 30), bits(e, 20, 16), bits(e, 15, 12), bits(e, 10, 10)
    n, d = bits(e, 9, 5), bits(e, 4, 0)
    t = utype(sf)
    mv = rd_reg(m, sf)
    other = {(0, 0): mv, (0, 1): f"({t})({mv} + 1)", (1, 0): f"({t})~{mv}", (1, 1): f"({t})(0 - {mv})"}[(op, o2)]
    return wr_reg(d, sf, f"cond_holds(ctx->nzcv, {cond}) ? {rd_reg(n, sf)} : {other}")


# Conditional compare (register / immediate): sf op 1 11010010 Rm|imm5 cond r 0 Rn 0 nzcv
@form("ccmp", 0x3FE00410, 0x3A400000)
def _ccmp(e: int) -> str:
    sf, sub, cond, is_imm = bits(e, 31, 31), bits(e, 30, 30), bits(e, 15, 12), bits(e, 11, 11)
    n, nzcv = bits(e, 9, 5), bits(e, 3, 0)
    w = 64 if sf else 32
    t = utype(sf)
    mv = f"({t}){bits(e, 20, 16)}" if is_imm else rd_reg(bits(e, 20, 16), sf)
    if sub:
        mv = f"({t})~{mv}"
    return (f"if (cond_holds(ctx->nzcv, {cond})) {{ uint64_t f; (void)awc{w}({rd_reg(n, sf)}, {mv}, {sub}, &f); "
            f"ctx->nzcv = f; }} else {{ ctx->nzcv = {nzcv << 28:#x}; }}")


# --------------------------------------------------------------------------- loads and stores
#
# Integer loads/stores only (V=0); SIMD/FP ones come with the SIMD work. Guest addresses are
# host addresses (decision 0005), so an access is a plain host access at the computed address
# (ld*/st* in yw4cpu.h). Base register 31 is SP; data register 31 is XZR.

@dataclass(frozen=True)
class MemAccess:
    """Decoded operands of one load/store, shared by the emitter and the test generator."""
    n: int                            # base register, 31 = SP
    imm: int                          # byte offset, already scaled and sign-extended
    index: str                        # "offset", "pre" or "post"
    rm: tuple[int, int, int] | None   # register offset: (m, extend option, shift)
    load: bool
    regs: tuple[int, ...]             # Rt, or Rt and Rt2 for pairs; empty for prefetch
    esize: int                        # bytes per register
    sext: int = 0                     # loads: 0 = zero-extend, else sign-extend to 32 or 64 bits
    prefetch: bool = False
    vec: bool = False                 # regs are SIMD/FP registers (B/H/S/D/Q)
    aligned: bool = False             # faults unless naturally aligned (exclusives, acquire/release)

    @property
    def writeback(self) -> bool:
        return self.index != "offset"


def sign_extend(v: int, width: int) -> int:
    return v - (1 << width) if v >> (width - 1) & 1 else v


def _checked(a: MemAccess) -> MemAccess | None:
    # Both cases are CONSTRAINED UNPREDICTABLE in the Arm ARM; compilers never emit them.
    if a.writeback and a.n != 31 and a.n in a.regs and not a.vec:
        return None
    if a.load and len(a.regs) == 2 and a.regs[0] == a.regs[1]:
        return None
    return a


def _single_kind(size: int, opc: int, allow_prefetch: bool) -> tuple[bool, int, bool] | None:
    """(load, sext, prefetch) for the size/opc fields of single-register loads/stores."""
    if opc == 0:
        return False, 0, False       # STR/STRB/STRH
    if opc == 1:
        return True, 0, False        # LDR/LDRB/LDRH, zero-extended
    if size == 3:
        return (True, 0, True) if opc == 2 and allow_prefetch else None  # PRFM
    if opc == 2:
        return True, 64, False       # LDRSB/LDRSH/LDRSW to 64 bits
    return (True, 32, False) if size < 2 else None  # LDRSB/LDRSH to 32 bits


def _single(n: int, t: int, size: int, opc: int, imm: int, index: str,
            rm: tuple[int, int, int] | None = None) -> MemAccess | None:
    kind = _single_kind(size, opc, allow_prefetch=index == "offset")
    if kind is None:
        return None
    load, sext, prefetch = kind
    return _checked(MemAccess(n, imm, index, rm, load, () if prefetch else (t,), 1 << size, sext, prefetch))


def _decode_uimm(e: int) -> MemAccess | None:
    size = bits(e, 31, 30)
    return _single(bits(e, 9, 5), bits(e, 4, 0), size, bits(e, 23, 22), bits(e, 21, 10) << size, "offset")


IMM9_INDEX = {0: "offset", 1: "post", 2: "offset", 3: "pre"}  # 0 = LDUR/STUR, 2 = LDTR/STTR


def _decode_imm9(e: int) -> MemAccess | None:
    idx = bits(e, 11, 10)
    if idx == 2 and bits(e, 31, 30) == 3 and bits(e, 23, 22) == 2:
        return None  # no unprivileged prefetch
    return _single(bits(e, 9, 5), bits(e, 4, 0), bits(e, 31, 30), bits(e, 23, 22),
                   sign_extend(bits(e, 20, 12), 9), IMM9_INDEX[idx])


def _decode_regoff(e: int) -> MemAccess | None:
    size, option = bits(e, 31, 30), bits(e, 15, 13)
    if not option & 2:
        return None  # only UXTW, LSL (UXTX), SXTW, SXTX
    shift = size if bits(e, 12, 12) else 0
    return _single(bits(e, 9, 5), bits(e, 4, 0), size, bits(e, 23, 22), 0, "offset",
                   rm=(bits(e, 20, 16), option, shift))


PAIR_INDEX = {0: "offset", 1: "post", 2: "offset", 3: "pre"}  # 0 = LDNP/STNP (a cache hint only)


def _decode_pair(e: int) -> MemAccess | None:
    opc, load, idx = bits(e, 31, 30), bits(e, 22, 22), bits(e, 24, 23)
    if opc == 3 or (opc == 1 and (not load or idx == 0)):
        return None  # opc=01: only LDPSW (STGP is ARMv8.5)
    esize = 8 if opc == 2 else 4
    imm = sign_extend(bits(e, 21, 15), 7) * esize
    return _checked(MemAccess(bits(e, 9, 5), imm, PAIR_INDEX[idx], None, bool(load),
                              (bits(e, 4, 0), bits(e, 14, 10)), esize, 64 if opc == 1 else 0))


def _vsingle(n: int, t: int, size: int, opc: int, imm_units: int, index: str,
             rm: tuple[int, int] | None = None) -> MemAccess | None:
    """SIMD/FP single register: size:opc picks B/H/S/D/Q (opc bit 1 set with size 0 = Q)."""
    if opc & 2:
        if size:
            return None
        log = 4
    else:
        log = size
    return MemAccess(n, imm_units << log, index, (rm[0], rm[1], log if rm[1] else 0) if rm else None,
                     bool(opc & 1), (t,), 1 << log, vec=True)


def _decode_vuimm(e: int) -> MemAccess | None:
    return _vsingle(bits(e, 9, 5), bits(e, 4, 0), bits(e, 31, 30), bits(e, 23, 22), bits(e, 21, 10), "offset")


def _decode_vimm9(e: int) -> MemAccess | None:
    if bits(e, 11, 10) == 2:
        return None  # no unprivileged SIMD/FP loads
    a = _vsingle(bits(e, 9, 5), bits(e, 4, 0), bits(e, 31, 30), bits(e, 23, 22), 0, IMM9_INDEX[bits(e, 11, 10)])
    return a and MemAccess(a.n, sign_extend(bits(e, 20, 12), 9), a.index, None, a.load, a.regs, a.esize, vec=True)


def _decode_vregoff(e: int) -> MemAccess | None:
    option = bits(e, 15, 13)
    if not option & 2:
        return None
    a = _vsingle(bits(e, 9, 5), bits(e, 4, 0), bits(e, 31, 30), bits(e, 23, 22), 0, "offset",
                 rm=(bits(e, 20, 16), bits(e, 12, 12)))
    return a and MemAccess(a.n, 0, "offset", (a.rm[0], option, a.rm[2]), a.load, a.regs, a.esize, vec=True)


def _decode_vpair(e: int) -> MemAccess | None:
    opc, load, idx = bits(e, 31, 30), bits(e, 22, 22), bits(e, 24, 23)
    if opc == 3:
        return None
    esize = 4 << opc
    return _checked(MemAccess(bits(e, 9, 5), sign_extend(bits(e, 21, 15), 7) * esize, PAIR_INDEX[idx], None,
                              bool(load), (bits(e, 4, 0), bits(e, 14, 10)), esize, vec=True))


def emit_vmem(a: MemAccess, addr: str) -> list[str]:
    out = []
    w = a.esize * 8
    lane = {1: "b", 2: "h", 4: "s", 8: "d"}
    for k, t in enumerate(a.regs):
        at = f"a + {k * a.esize}" if k else "a"
        if a.load and a.esize == 16:
            out.append(f"ctx->v[{t}].d[0] = ld64({at}); ctx->v[{t}].d[1] = ld64({at} + 8);")
        elif a.load:  # scalar load: the rest of the register is zeroed
            out.append(f"ctx->v[{t}].d[0] = ld{w}({at}); ctx->v[{t}].d[1] = 0;")
        elif a.esize == 16:
            out.append(f"st64({at}, ctx->v[{t}].d[0]); st64({at} + 8, ctx->v[{t}].d[1]);")
        else:
            out.append(f"st{w}({at}, ctx->v[{t}].{lane[a.esize]}[0]);")
    return out


def emit_mem(a: MemAccess) -> str:
    if a.prefetch:
        return ";"  # PRFM is a hint with no architectural effect
    base = rd_reg(a.n, 1, sp=True)
    if a.rm:
        addr = f"{base} + {extend_expr(a.rm[0], a.rm[1], a.rm[2], 1)}"
    elif a.index == "post" or a.imm == 0:
        addr = base
    else:
        addr = f"{base} {'-' if a.imm < 0 else '+'} {abs(a.imm):#x}"
    out = [f"uint64_t a = {addr};"]
    w = a.esize * 8
    if a.vec:
        out += emit_vmem(a, addr)
    for k, t in enumerate(a.regs if not a.vec else ()):
        at = f"a + {k * a.esize}" if k else "a"
        if a.load:
            v = f"ld{w}({at})"
            if a.sext:
                v = f"(uint{a.sext}_t)(int{a.sext}_t)(int{w}_t){v}"
            out.append(wr_reg(t, 1 if a.sext != 32 else 0, v) or f"(void){v};")  # XZR: access, discard
        else:
            out.append(f"st{w}({at}, (uint{w}_t){rd_reg(t, 1)});")
    if a.writeback:
        new = "a" if a.index == "pre" else f"a {'-' if a.imm < 0 else '+'} {abs(a.imm):#x}"
        out.append(wr_reg(a.n, 1, new, sp=True))
    return "{ " + " ".join(s for s in out if s) + " }"


def mem_form(name: str, mask: int, value: int, decode: Callable[[int], MemAccess | None]) -> None:
    FORMS.append(Form(name, mask, value, valid=lambda e: decode(e) is not None,
                      emit=lambda e: emit_mem(decode(e)), mem=decode))


# Load/store register (unsigned immediate): size 111 0 01 opc imm12 Rn Rt
mem_form("ldst_uimm", 0x3F000000, 0x39000000, _decode_uimm)
# Load/store register (unscaled / post-index / unprivileged / pre-index): size 111 0 00 opc 0 imm9 idx Rn Rt
mem_form("ldst_imm9", 0x3F200000, 0x38000000, _decode_imm9)
# Load/store register (register offset): size 111 0 00 opc 1 Rm option S 10 Rn Rt
mem_form("ldst_regoff", 0x3F200C00, 0x38200800, _decode_regoff)
# Load/store pair (no-allocate / post-index / offset / pre-index): opc 101 0 idx L imm7 Rt2 Rn Rt
mem_form("ldst_pair", 0x3E000000, 0x28000000, _decode_pair)
# The same four layouts with V=1: SIMD/FP registers B/H/S/D/Q
mem_form("vldst_uimm", 0x3F000000, 0x3D000000, _decode_vuimm)
mem_form("vldst_imm9", 0x3F200000, 0x3C000000, _decode_vimm9)
mem_form("vldst_regoff", 0x3F200C00, 0x3C200800, _decode_vregoff)
mem_form("vldst_pair", 0x3E000000, 0x2C000000, _decode_vpair)


# --------------------------------------------------------------------------- atomics
#
# Guest threads run as host threads on shared memory, so these must be real host atomics
# (decision 0009). Exclusive pairs become a compare-and-swap: LDXR records the address and
# the value it saw, STXR succeeds only if the memory still holds that value.

def _atomic_ptr(size: int) -> str:
    return f"({UINT[size]} *)(uintptr_t)a"


def _excl_decode(e: int) -> MemAccess | None:
    size, load, rs, rt2, n, t = bits(e, 31, 30), bits(e, 22, 22), bits(e, 20, 16), bits(e, 14, 10), bits(e, 9, 5), bits(e, 4, 0)
    if rt2 != 31 or (load and rs != 31):
        return None
    if not load and (rs == t or (rs == n and n != 31)):
        return None  # CONSTRAINED UNPREDICTABLE
    return MemAccess(n, 0, "offset", None, bool(load), (t,), 1 << size, aligned=True)


def _excl_emit(e: int) -> str:
    size, load, acqrel, rs, n, t = bits(e, 31, 30), bits(e, 22, 22), bits(e, 15, 15), bits(e, 20, 16), bits(e, 9, 5), bits(e, 4, 0)
    ut, nbytes = UINT[size], 1 << size
    base = rd_reg(n, 1, sp=True)
    if load:  # LDXR, LDAXR
        order = "__ATOMIC_ACQUIRE" if acqrel else "__ATOMIC_RELAXED"
        return (f"{{ uint64_t a = {base}; {ut} v_ = __atomic_load_n({_atomic_ptr(size)}, {order}); "
                f"ctx->excl_addr = a; ctx->excl_val = v_; ctx->excl_size = {nbytes}; {wr_reg(t, 1, 'v_')} }}")
    # STXR, STLXR: status 0 = stored, 1 = failed; the reservation is always cleared
    return (f"{{ uint64_t a = {base}; {ut} new_ = ({ut}){rd_reg(t, 1)}; int ok_ = 0; "
            f"if (ctx->excl_size == {nbytes} && ctx->excl_addr == a) {{ {ut} old_ = ({ut})ctx->excl_val; "
            f"ok_ = __atomic_compare_exchange_n({_atomic_ptr(size)}, &old_, new_, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }} "
            f"ctx->excl_size = 0; {wr_reg(rs, 0, '!ok_') or '(void)ok_;'} }}")


# Load/store exclusive register: size 001000 0 L 0 Rs o0 Rt2 Rn Rt   (LDXR/LDAXR/STXR/STLXR; no pairs)
FORMS.append(Form("excl", 0x3FA00000, 0x08000000, valid=lambda e: _excl_decode(e) is not None,
                  emit=_excl_emit, mem=_excl_decode))


def _acqrel_decode(e: int) -> MemAccess | None:
    if not bits(e, 15, 15) or bits(e, 20, 16) != 31 or bits(e, 14, 10) != 31:
        return None  # o0 = 0 is LDLAR/STLLR (ARMv8.1)
    return MemAccess(bits(e, 9, 5), 0, "offset", None, bool(bits(e, 22, 22)), (bits(e, 4, 0),),
                     1 << bits(e, 31, 30), aligned=True)


def _acqrel_emit(e: int) -> str:
    size, load, n, t = bits(e, 31, 30), bits(e, 22, 22), bits(e, 9, 5), bits(e, 4, 0)
    ut = UINT[size]
    base = rd_reg(n, 1, sp=True)
    if load:  # LDAR
        return f"{{ uint64_t a = {base}; {ut} v_ = __atomic_load_n({_atomic_ptr(size)}, __ATOMIC_ACQUIRE); {wr_reg(t, 1, 'v_') or '(void)v_;'} }}"
    # STLR is sequentially consistent with LDAR on ARM (RCsc); a plain release store isn't on x86
    return f"{{ uint64_t a = {base}; __atomic_store_n({_atomic_ptr(size)}, ({ut}){rd_reg(t, 1)}, __ATOMIC_SEQ_CST); }}"


# Load-acquire / store-release register: size 001000 1 L 0 11111 1 11111 Rn Rt   (LDAR, STLR)
FORMS.append(Form("acqrel", 0x3FA00000, 0x08800000, valid=lambda e: _acqrel_decode(e) is not None,
                  emit=_acqrel_emit, mem=_acqrel_decode))


# --------------------------------------------------------------------------- scalar floating point
#
# ftype 0 = single (S registers), 1 = double (D). Half precision arithmetic is ARMv8.2, which
# the Switch's A57 lacks. Arithmetic goes through yw4fp.h so NaNs behave as on ARM.
# Writing an S or D register zeroes the rest of the V register.

FP = {0: ("f32", "uint32_t", "s"), 1: ("f64", "uint64_t", "d")}


def vread(n: int, ftype: int) -> str:
    return f"ctx->v[{n}].{FP[ftype][2]}[0]"


def vwrite(d: int, ftype: int, expr: str) -> str:
    """Scalar write; the value is computed first, since expr may read v[d]."""
    return f"{{ {FP[ftype][1]} r_ = {expr}; ctx->v[{d}].d[0] = r_; ctx->v[{d}].d[1] = 0; }}"


def sign_bit(ftype: int) -> str:
    return "0x80000000u" if ftype == 0 else "0x8000000000000000ull"


def _ftype_ok(e: int) -> bool:
    return bits(e, 23, 22) in FP


def _rd_rn_rm(e: int) -> tuple[int, ...]:
    return bits(e, 4, 0), bits(e, 9, 5), bits(e, 20, 16)


FP_DP2 = ["mul", "div", "add", "sub", "max", "min", "maxnm", "minnm", "nmul"]


# Floating-point data-processing (2 source): 0 0 0 11110 ftype 1 Rm opcode 10 Rn Rd
@form("fp_dp2", 0xFF200C00, 0x1E200800, valid=lambda e: _ftype_ok(e) and bits(e, 15, 12) < 9, vregs=_rd_rn_rm)
def _fp_dp2(e: int) -> str:
    ftype, d, n, m = bits(e, 23, 22), bits(e, 4, 0), bits(e, 9, 5), bits(e, 20, 16)
    p = FP[ftype][0]
    op = FP_DP2[bits(e, 15, 12)]
    if op == "nmul":  # FNMUL negates the product, NaN or not
        return vwrite(d, ftype, f"{p}_mul({vread(n, ftype)}, {vread(m, ftype)}) ^ {sign_bit(ftype)}")
    return vwrite(d, ftype, f"{p}_{op}({vread(n, ftype)}, {vread(m, ftype)})")


FRINT = {0b001000: "n", 0b001001: "p", 0b001010: "m", 0b001011: "z", 0b001100: "a", 0b001110: "n", 0b001111: "n"}


def _fp_dp1_valid(e: int) -> bool:
    op = bits(e, 20, 15)
    if not _ftype_ok(e):
        return False
    if op in (0b000100, 0b000101):  # FCVT to single / double: not to the same type
        return op - 0b000100 != bits(e, 23, 22)
    return op <= 3 or op in FRINT


# Floating-point data-processing (1 source): 0 0 0 11110 ftype 1 opcode 10000 Rn Rd
@form("fp_dp1", 0xFF207C00, 0x1E204000, valid=_fp_dp1_valid, vregs=lambda e: (bits(e, 4, 0), bits(e, 9, 5)))
def _fp_dp1(e: int) -> str:
    ftype, op, n, d = bits(e, 23, 22), bits(e, 20, 15), bits(e, 9, 5), bits(e, 4, 0)
    p, a = FP[ftype][0], vread(n, ftype)
    if op == 0:
        return vwrite(d, ftype, a)                              # FMOV (register)
    if op == 1:
        return vwrite(d, ftype, f"{a} & ~{sign_bit(ftype)}")     # FABS: NaNs too, no NaN processing
    if op == 2:
        return vwrite(d, ftype, f"{a} ^ {sign_bit(ftype)}")      # FNEG
    if op == 3:
        return vwrite(d, ftype, f"{p}_sqrt({a})")
    if op == 0b000101:
        return vwrite(d, 1, f"f32_to_f64({a})")                  # FCVT Dd, Sn
    if op == 0b000100:
        return vwrite(d, 0, f"f64_to_f32({a})")                  # FCVT Sd, Dn
    return vwrite(d, ftype, f"{p}_rint({a}, '{FRINT[op]}')")     # FRINTN/P/M/Z/A/X/I


# Floating-point compare: 0 0 0 11110 ftype 1 Rm 00 1000 Rn opc 000   (FCMP, FCMPE; opc bit 3 = with #0.0)
@form("fp_cmp", 0xFF20FC07, 0x1E202000, valid=_ftype_ok,
      vregs=lambda e: (bits(e, 9, 5),) if bits(e, 3, 3) else (bits(e, 9, 5), bits(e, 20, 16)))
def _fp_cmp(e: int) -> str:
    ftype, m, n = bits(e, 23, 22), bits(e, 20, 16), bits(e, 9, 5)
    b = "0" if bits(e, 3, 3) else vread(m, ftype)
    return f"ctx->nzcv = {FP[ftype][0]}_cmp({vread(n, ftype)}, {b});"


# Floating-point conditional compare: 0 0 0 11110 ftype 1 Rm cond 01 Rn op nzcv   (FCCMP, FCCMPE)
@form("fp_ccmp", 0xFF200C00, 0x1E200400, valid=_ftype_ok, vregs=lambda e: (bits(e, 9, 5), bits(e, 20, 16)))
def _fp_ccmp(e: int) -> str:
    ftype, m, cond, n, nzcv = bits(e, 23, 22), bits(e, 20, 16), bits(e, 15, 12), bits(e, 9, 5), bits(e, 3, 0)
    return (f"ctx->nzcv = cond_holds(ctx->nzcv, {cond}) ? "
            f"{FP[ftype][0]}_cmp({vread(n, ftype)}, {vread(m, ftype)}) : {nzcv << 28:#x};")


# Floating-point conditional select: 0 0 0 11110 ftype 1 Rm cond 11 Rn Rd
@form("fp_csel", 0xFF200C00, 0x1E200C00, valid=_ftype_ok, vregs=_rd_rn_rm)
def _fp_csel(e: int) -> str:
    ftype, m, cond, n, d = bits(e, 23, 22), bits(e, 20, 16), bits(e, 15, 12), bits(e, 9, 5), bits(e, 4, 0)
    return vwrite(d, ftype, f"cond_holds(ctx->nzcv, {cond}) ? {vread(n, ftype)} : {vread(m, ftype)}")


def vfp_expand_imm(imm8: int, ftype: int) -> int:
    """VFPExpandImm: sign, a short exponent, 4 fraction bits."""
    sign, b6, rest = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0x3F
    if ftype == 0:
        exp = (b6 ^ 1) << 7 | (0x1F if b6 else 0) << 2 | (rest >> 4)
        return sign << 31 | exp << 23 | (rest & 0xF) << 19
    exp = (b6 ^ 1) << 10 | (0xFF if b6 else 0) << 2 | (rest >> 4)
    return sign << 63 | exp << 52 | (rest & 0xF) << 48


# Floating-point move (immediate): 0 0 0 11110 ftype 1 imm8 100 00000 Rd
@form("fp_imm", 0xFF201FE0, 0x1E201000, valid=_ftype_ok, vregs=lambda e: (bits(e, 4, 0),))
def _fp_imm(e: int) -> str:
    ftype = bits(e, 23, 22)
    return vwrite(bits(e, 4, 0), ftype, f"{vfp_expand_imm(bits(e, 20, 13), ftype):#x}u")


FCVT_MODE = {0: "n", 1: "p", 2: "m", 3: "z"}


def _fp_int_kind(e: int) -> str | None:
    sf, ftype, rmode, op = bits(e, 31, 31), bits(e, 23, 22), bits(e, 20, 19), bits(e, 18, 16)
    if op in (0, 1) and ftype in FP:
        return "fcvt"                      # FCVT{N,P,M,Z}{S,U}
    if op in (4, 5) and rmode == 0 and ftype in FP:
        return "fcvta"                     # FCVTAS/AU
    if op in (2, 3) and rmode == 0 and ftype in FP:
        return "cvtf"                      # SCVTF/UCVTF
    if op in (6, 7) and (sf, ftype, rmode) in ((0, 0, 0), (1, 1, 0)):
        return "fmov"                      # FMOV Wd<->Sn, Xd<->Dn
    if op in (6, 7) and (sf, ftype, rmode) == (1, 2, 1):
        return "fmov_top"                  # FMOV Xd<->Vn.D[1]
    return None


def _fp_int_vregs(e: int) -> tuple[int, ...]:
    to_fp = bits(e, 18, 16) in (2, 3, 7)
    return (bits(e, 4, 0),) if to_fp else (bits(e, 9, 5),)


# Conversion between floating-point and integer: sf 0 0 11110 ftype 1 rmode opcode 000000 Rn Rd
@form("fp_int", 0x7F20FC00, 0x1E200000, valid=lambda e: _fp_int_kind(e) is not None, vregs=_fp_int_vregs)
def _fp_int(e: int) -> str:
    sf, ftype, rmode, op = bits(e, 31, 31), bits(e, 23, 22), bits(e, 20, 19), bits(e, 18, 16)
    n, d = bits(e, 9, 5), bits(e, 4, 0)
    kind = _fp_int_kind(e)
    w = 64 if sf else 32
    if kind in ("fcvt", "fcvta"):
        mode = "a" if kind == "fcvta" else FCVT_MODE[rmode]
        src = f"{'(double)f32_of' if ftype == 0 else 'f64_of'}({vread(n, ftype)})"
        conv = f"yw4_to_{'u' if op & 1 else 's'}{w}({src}, '{mode}')"
        return wr_reg(d, sf, f"({utype(sf)}){conv}") or ";"
    if kind == "cvtf":
        ctype = ("uint" if op & 1 else "int") + f"{w}_t"
        host = "float" if ftype == 0 else "double"
        return vwrite(d, ftype, f"bits_{FP[ftype][0]}(({host})({ctype}){rd_reg(n, sf)})")
    if kind == "fmov":
        if op == 6:
            return wr_reg(d, sf, vread(n, ftype)) or ";"
        return vwrite(d, ftype, rd_reg(n, sf))
    if op == 6:
        return wr_reg(d, 1, f"ctx->v[{n}].d[1]") or ";"
    return f"ctx->v[{d}].d[1] = {rd_reg(n, 1)};"          # the low half is kept


# --------------------------------------------------------------------------- Advanced SIMD (vector)
#
# Every vector instruction is emitted the same way: copy the source registers into locals
# (a destination may also be a source), build the result in a zeroed local, store it. With
# Q=0 only the low 64 bits are produced and the upper half of the destination is zeroed.

LANE = {0: "b", 1: "h", 2: "s", 3: "d"}
UINT = {0: "uint8_t", 1: "uint16_t", 2: "uint32_t", 3: "uint64_t"}
SINT = {0: "int8_t", 1: "int16_t", 2: "int32_t", 3: "int64_t"}


def lanes(q: int, size: int) -> int:
    return (128 if q else 64) >> (3 + size)


def vec(d: int, srcs: dict[str, int], body: str, keep_d: bool = False) -> str:
    """`body` assigns r_ from the locals named in srcs (e.g. {"n_": 5}); keep_d starts r_
    from the old destination instead of zero (for ops that only change some lanes)."""
    loads = " ".join(f"v128 {k} = ctx->v[{v}];" for k, v in srcs.items())
    init = f"v128 r_ = ctx->v[{d}];" if keep_d else "v128 r_ = {{0}};"
    return f"{{ {loads} {init} {body} ctx->v[{d}] = r_; }}"


def per_lane(count: int, stmt: str) -> str:
    return f"for (int i = 0; i < {count}; i++) {{ {stmt} }}"


def _q_size_ok(e: int) -> bool:
    return bool(bits(e, 30, 30)) or bits(e, 23, 22) != 3


# Float ops in "three same": (U, size<1>, opcode) -> (kind, helper)
FP3 = {
    (0, 0, 0x18): ("lane", "maxnm"), (0, 0, 0x1A): ("lane", "add"), (0, 0, 0x1C): ("cmp", "=="),
    (0, 0, 0x1E): ("lane", "max"), (0, 0, 0x1F): ("lane", "recps"),
    (0, 1, 0x18): ("lane", "minnm"), (0, 1, 0x1A): ("lane", "sub"), (0, 1, 0x1E): ("lane", "min"),
    (0, 1, 0x1F): ("lane", "rsqrts"),
    (1, 0, 0x18): ("pair", "maxnm"), (1, 0, 0x1A): ("pair", "add"), (1, 0, 0x1B): ("lane", "mul"),
    (1, 0, 0x1C): ("cmp", ">="), (1, 0, 0x1D): ("acmp", ">="), (1, 0, 0x1E): ("pair", "max"),
    (1, 0, 0x1F): ("lane", "div"),
    (1, 1, 0x18): ("pair", "minnm"), (1, 1, 0x1A): ("abd", "sub"), (1, 1, 0x1C): ("cmp", ">"),
    (1, 1, 0x1D): ("acmp", ">"), (1, 1, 0x1E): ("pair", "min"),
}
# Integer ops in "three same": (U, opcode) -> kind
INT3 = {(0, 0x10): "+", (1, 0x10): "-", (0, 0x11): "tst", (1, 0x11): "==", (0, 0x06): "s>", (1, 0x06): "u>",
        (0, 0x07): "s>=", (1, 0x07): "u>=", (0, 0x17): "addp"}
LOGIC3 = {(0, 0): "&", (0, 1): "&~", (0, 2): "|", (0, 3): "|~", (1, 0): "^", (1, 1): "bsl", (1, 2): "bit", (1, 3): "bif"}


def _three_same_valid(e: int) -> bool:
    u, size, op, q = bits(e, 29, 29), bits(e, 23, 22), bits(e, 15, 11), bits(e, 30, 30)
    if op >= 0x18:
        return (u, size >> 1, op) in FP3 and (q or not size & 1)  # .1d is not a float arrangement
    if op == 0x03:
        return True
    return (u, op) in INT3 and _q_size_ok(e)


def float_cmp(p: str, a: str, b: str, op: str, absolute: bool = False) -> str:
    host = "f32_of" if p == "f32" else "f64_of"
    x, y = f"{host}({a})", f"{host}({b})"
    if absolute:
        x, y = f"fabs({x})", f"fabs({y})"
    return f"({x} {op} {y})"  # false for NaNs, as on ARM


# Advanced SIMD three same: 0 Q U 01110 size 1 Rm opcode 1 Rn Rd
@form("simd_three_same", 0x9F200400, 0x0E200400, valid=_three_same_valid, vregs=_rd_rn_rm)
def _simd_three_same(e: int) -> str:
    q, u, size, m, op, n, d = (bits(e, 30, 30), bits(e, 29, 29), bits(e, 23, 22), bits(e, 20, 16),
                               bits(e, 15, 11), bits(e, 9, 5), bits(e, 4, 0))
    srcs = {"n_": n, "m_": m}
    if op == 0x03:  # logical, on 64-bit halves
        kind = LOGIC3[(u, size)]
        count = 2 if q else 1
        if kind in ("bsl", "bit", "bif"):
            srcs["d_"] = d
            expr = {"bsl": "(d_.d[i] & n_.d[i]) | (~d_.d[i] & m_.d[i])",
                    "bit": "(m_.d[i] & n_.d[i]) | (~m_.d[i] & d_.d[i])",
                    "bif": "(~m_.d[i] & n_.d[i]) | (m_.d[i] & d_.d[i])"}[kind]
        elif kind.endswith("~"):
            expr = f"n_.d[i] {kind[0]} ~m_.d[i]"
        else:
            expr = f"n_.d[i] {kind} m_.d[i]"
        return vec(d, srcs, per_lane(count, f"r_.d[i] = {expr};"))
    if op >= 0x18:
        sz = size & 1
        p, lane, count = FP[sz][0], LANE[2 + sz], lanes(q, 2 + sz)
        kind, fn = FP3[(u, size >> 1, op)]
        a, b = f"n_.{lane}[i]", f"m_.{lane}[i]"
        ones = "~(uint32_t)0" if sz == 0 else "~(uint64_t)0"
        if kind == "lane":
            stmt = f"r_.{lane}[i] = {p}_{fn}({a}, {b});"
        elif kind == "abd":
            stmt = f"r_.{lane}[i] = {p}_sub({a}, {b}) & ~{sign_bit(sz)};"
        elif kind in ("cmp", "acmp"):
            stmt = f"r_.{lane}[i] = {float_cmp(p, a, b, fn, kind == 'acmp')} ? {ones} : 0;"
        else:  # pairwise over the concatenation m:n
            h = count // 2
            stmt = (f"v128 *s_ = i < {h} ? &n_ : &m_; int j = 2 * (i % {h}); "
                    f"r_.{lane}[i] = {p}_{fn}(s_->{lane}[j], s_->{lane}[j + 1]);")
        return vec(d, srcs, per_lane(count, stmt))
    lane, count, ut, st = LANE[size], lanes(q, size), UINT[size], SINT[size]
    kind = INT3[(u, op)]
    a, b = f"n_.{lane}[i]", f"m_.{lane}[i]"
    if kind in ("+", "-"):
        stmt = f"r_.{lane}[i] = ({ut})({a} {kind} {b});"
    elif kind == "addp":
        h = count // 2
        stmt = (f"v128 *s_ = i < {h} ? &n_ : &m_; int j = 2 * (i % {h}); "
                f"r_.{lane}[i] = ({ut})(s_->{lane}[j] + s_->{lane}[j + 1]);")
    else:
        cond = {"tst": f"({a} & {b}) != 0", "==": f"{a} == {b}", "s>": f"({st}){a} > ({st}){b}",
                "u>": f"{a} > {b}", "s>=": f"({st}){a} >= ({st}){b}", "u>=": f"{a} >= {b}"}[kind]
        stmt = f"r_.{lane}[i] = ({cond}) ? ({ut})~({ut})0 : 0;"
    return vec(d, srcs, per_lane(count, stmt))


# Two-register misc, float ops with size<1> = 1: (U, opcode) -> kind
FP2 = {(0, 0x0F): "abs", (1, 0x0F): "neg", (1, 0x1F): "sqrt", (0, 0x1D): "recpe", (1, 0x1D): "rsqrte",
       (0, 0x1B): "cvtzs", (1, 0x1B): "cvtzu", (0, 0x0C): ">", (0, 0x0D): "==", (0, 0x0E): "<",
       (1, 0x0C): ">=", (1, 0x0D): "<="}
# size<1> = 0 float ops (sz = size<0>)
FP2_LO = {(0, 0x1D): "scvtf", (1, 0x1D): "ucvtf"}
# Integer ops: (U, opcode) -> kind
INT2 = {(0, 0x12): "xtn", (1, 0x05): "not", (0, 0x00): "rev64", (0, 0x09): "==0", (1, 0x08): ">=0",
        (0, 0x08): ">0", (1, 0x09): "<=0", (0, 0x0A): "<0"}


def _two_misc_valid(e: int) -> bool:
    q, u, size, op = bits(e, 30, 30), bits(e, 29, 29), bits(e, 23, 22), bits(e, 16, 12)
    if size >> 1 and (u, op) in FP2:
        return q or not size & 1
    if not size >> 1 and (u, op) in FP2_LO:
        return q or not size & 1
    kind = INT2.get((u, op))
    if kind == "xtn":
        return size < 3
    if kind == "not":
        return size == 0
    if kind == "rev64":
        return size < 3
    return kind is not None and _q_size_ok(e)


# Advanced SIMD two-register misc: 0 Q U 01110 size 10000 opcode 10 Rn Rd
@form("simd_two_misc", 0x9F3E0C00, 0x0E200800, valid=_two_misc_valid,
      vregs=lambda e: (bits(e, 4, 0), bits(e, 9, 5)))
def _simd_two_misc(e: int) -> str:
    q, u, size, op, n, d = bits(e, 30, 30), bits(e, 29, 29), bits(e, 23, 22), bits(e, 16, 12), bits(e, 9, 5), bits(e, 4, 0)
    srcs = {"n_": n}
    if (size >> 1 and (u, op) in FP2) or (not size >> 1 and (u, op) in FP2_LO):
        sz = size & 1
        p, lane, count = FP[sz][0], LANE[2 + sz], lanes(q, 2 + sz)
        kind = FP2[(u, op)] if size >> 1 else FP2_LO[(u, op)]
        a, ut = f"n_.{lane}[i]", UINT[2 + sz]
        w = 32 << sz
        if kind == "abs":
            v = f"{a} & ~{sign_bit(sz)}"
        elif kind == "neg":
            v = f"{a} ^ {sign_bit(sz)}"
        elif kind in ("sqrt", "recpe", "rsqrte"):
            v = f"{p}_{kind}({a})"
        elif kind in ("cvtzs", "cvtzu"):
            host = "(double)f32_of" if sz == 0 else "f64_of"
            v = f"({ut})yw4_to_{kind[-1]}{w}({host}({a}), 'z')"
        elif kind in ("scvtf", "ucvtf"):
            it = ("int" if kind == "scvtf" else "uint") + f"{w}_t"
            v = f"bits_{p}(({'float' if sz == 0 else 'double'})({it}){a})"
        else:  # compare with zero
            v = f"{float_cmp(p, a, '0', kind)} ? ~({ut})0 : 0"
        return vec(d, srcs, per_lane(count, f"r_.{lane}[i] = {v};"))
    kind = INT2[(u, op)]
    lane, count, ut, st = LANE[size], lanes(q, size), UINT[size], SINT[size]
    if kind == "xtn":  # narrow each 2*esize lane; XTN2 (Q=1) writes the upper half, keeping the lower
        half = 8 >> size
        src = LANE[size + 1]
        base = half if q else 0
        return vec(d, srcs, per_lane(half, f"r_.{lane}[{base} + i] = ({ut})n_.{src}[i];"), keep_d=bool(q)) \
            if q else vec(d, srcs, per_lane(half, f"r_.{lane}[i] = ({ut})n_.{src}[i];"))
    if kind == "not":
        return vec(d, srcs, per_lane(2 if q else 1, "r_.d[i] = ~n_.d[i];"))
    if kind == "rev64":  # reverse the lanes inside each 64-bit doubleword
        per = 8 >> size
        return vec(d, srcs, per_lane(count, f"r_.{lane}[i] = n_.{lane}[(i / {per}) * {per} + {per - 1} - i % {per}];"))
    cond = {"==0": f"n_.{lane}[i] == 0", ">=0": f"({st})n_.{lane}[i] >= 0", ">0": f"({st})n_.{lane}[i] > 0",
            "<=0": f"({st})n_.{lane}[i] <= 0", "<0": f"({st})n_.{lane}[i] < 0"}[kind]
    return vec(d, srcs, per_lane(count, f"r_.{lane}[i] = ({cond}) ? ({ut})~({ut})0 : 0;"))


def _imm5_size(imm5: int) -> int | None:
    for s in range(4):
        if imm5 >> s & 1:
            return s
    return None


def _copy_kind(e: int) -> str | None:
    q, op, imm5, imm4 = bits(e, 30, 30), bits(e, 29, 29), bits(e, 20, 16), bits(e, 14, 11)
    size = _imm5_size(imm5)
    if size is None:
        return None
    if op:
        return "ins_elem" if q else None
    if imm4 == 0:
        return "dup_elem" if q or size < 3 else None
    if imm4 == 1:
        return "dup_gen" if q or size < 3 else None
    if imm4 == 3:
        return "ins_gen" if q else None
    if imm4 == 5:
        return "smov" if size < (3 if q else 2) else None
    if imm4 == 7:
        return "umov" if (size == 3) == bool(q) else None
    return None


def _copy_vregs(e: int) -> tuple[int, ...]:
    kind = _copy_kind(e)
    if kind in ("smov", "umov"):
        return (bits(e, 9, 5),)
    if kind in ("dup_gen", "ins_gen"):
        return (bits(e, 4, 0),)
    return bits(e, 4, 0), bits(e, 9, 5)


# Advanced SIMD copy: 0 Q op 01110000 imm5 0 imm4 1 Rn Rd   (DUP, INS, SMOV, UMOV; MOV aliases)
@form("simd_copy", 0x9FE08400, 0x0E000400, valid=lambda e: _copy_kind(e) is not None, vregs=_copy_vregs)
def _simd_copy(e: int) -> str:
    q, imm5, imm4, n, d = bits(e, 30, 30), bits(e, 20, 16), bits(e, 14, 11), bits(e, 9, 5), bits(e, 4, 0)
    kind = _copy_kind(e)
    size = _imm5_size(imm5)
    lane, ut, idx = LANE[size], UINT[size], imm5 >> (size + 1)
    if kind == "dup_elem":
        return vec(d, {"n_": n}, per_lane(lanes(q, size), f"r_.{lane}[i] = n_.{lane}[{idx}];"))
    if kind == "dup_gen":
        return vec(d, {}, per_lane(lanes(q, size), f"r_.{lane}[i] = ({ut}){rd_reg(n, 1)};"))
    if kind == "ins_gen":
        return vec(d, {}, f"r_.{lane}[{idx}] = ({ut}){rd_reg(n, 1)};", keep_d=True)
    if kind == "ins_elem":
        return vec(d, {"n_": n}, f"r_.{lane}[{idx}] = n_.{lane}[{imm4 >> size}];", keep_d=True)
    if kind == "umov":
        return wr_reg(d, q, f"ctx->v[{n}].{lane}[{idx}]") or ";"
    return wr_reg(d, q, f"({utype(q)})({'int64_t' if q else 'int32_t'})({SINT[size]})ctx->v[{n}].{lane}[{idx}]") or ";"


# Advanced SIMD scalar copy: 01 0 11110000 imm5 0 0000 1 Rn Rd   (DUP/MOV scalar from a lane)
@form("simd_scalar_dup", 0xFFE0FC00, 0x5E000400, valid=lambda e: _imm5_size(bits(e, 20, 16)) is not None,
      vregs=lambda e: (bits(e, 4, 0), bits(e, 9, 5)))
def _simd_scalar_dup(e: int) -> str:
    imm5, n, d = bits(e, 20, 16), bits(e, 9, 5), bits(e, 4, 0)
    size = _imm5_size(imm5)
    lane = LANE[size]
    return vec(d, {"n_": n}, f"r_.{lane}[0] = n_.{lane}[{imm5 >> (size + 1)}];")


# Advanced SIMD extract: 0 Q 101110 00 0 Rm 0 imm4 0 Rn Rd
@form("simd_ext", 0xBFE08400, 0x2E000000, valid=lambda e: bits(e, 30, 30) or not bits(e, 14, 14), vregs=_rd_rn_rm)
def _simd_ext(e: int) -> str:
    q, m, pos, n, d = bits(e, 30, 30), bits(e, 20, 16), bits(e, 14, 11), bits(e, 9, 5), bits(e, 4, 0)
    count = 16 if q else 8
    return vec(d, {"n_": n, "m_": m},
               per_lane(count, f"int j = i + {pos}; r_.b[i] = j < {count} ? n_.b[j] : m_.b[j - {count}];"))


def adv_simd_expand_imm(op: int, cmode: int, imm8: int) -> int:
    """AdvSIMDExpandImm: the 64-bit pattern (replicated to 128 bits by the caller)."""
    def rep(v: int, width: int) -> int:
        return replicate(v, width, 64)
    c = cmode >> 1
    if c == 0:
        return rep(imm8, 32)
    if c == 1:
        return rep(imm8 << 8, 32)
    if c == 2:
        return rep(imm8 << 16, 32)
    if c == 3:
        return rep(imm8 << 24, 32)
    if c == 4:
        return rep(imm8, 16)
    if c == 5:
        return rep(imm8 << 8, 16)
    if c == 6:
        return rep(imm8 << 8 | 0xFF, 32) if not cmode & 1 else rep(imm8 << 16 | 0xFFFF, 32)
    if not cmode & 1 and not op:
        return rep(imm8, 8)
    if not cmode & 1:
        return sum(0xFF << (8 * i) for i in range(8) if imm8 >> i & 1)
    a, b, rest = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0x3F
    if not op:  # single-precision FMOV
        f = a << 31 | (b ^ 1) << 30 | (0x1F if b else 0) << 25 | rest << 19
        return rep(f, 32)
    return a << 63 | (b ^ 1) << 62 | (0xFF if b else 0) << 54 | rest << 48


def _modimm_kind(e: int) -> str | None:
    q, op, cmode, o2 = bits(e, 30, 30), bits(e, 29, 29), bits(e, 15, 12), bits(e, 11, 11)
    if o2:
        return None  # FMOV (half precision), ARMv8.2
    if cmode == 0xF:
        return "movi" if not op or q else None
    if cmode == 0xE:
        return "movi"
    if cmode < 0xC and cmode & 1:  # 32-bit or 16-bit ORR/BIC
        return "bic" if op else "orr"
    return "mvni" if op else "movi"


# Advanced SIMD modified immediate: 0 Q op 0111100000 abc cmode o2 1 defgh Rd   (MOVI, MVNI, ORR, BIC, FMOV)
@form("simd_modimm", 0x9FF80400, 0x0F000400, valid=lambda e: _modimm_kind(e) is not None,
      vregs=lambda e: (bits(e, 4, 0),))
def _simd_modimm(e: int) -> str:
    q, op, cmode, d = bits(e, 30, 30), bits(e, 29, 29), bits(e, 15, 12), bits(e, 4, 0)
    imm8 = bits(e, 18, 16) << 5 | bits(e, 9, 5)
    imm = adv_simd_expand_imm(op, cmode, imm8)
    kind = _modimm_kind(e)
    count = 2 if q else 1
    if kind == "movi":
        return vec(d, {}, per_lane(count, f"r_.d[i] = {imm:#x}ull;"))
    if kind == "mvni":
        return vec(d, {}, per_lane(count, f"r_.d[i] = {~imm & ones(64):#x}ull;"))
    opx = "|" if kind == "orr" else "& ~"
    return vec(d, {"d_": d}, per_lane(count, f"r_.d[i] = d_.d[i] {opx}{imm:#x}ull;"))


def _elem_index(e: int) -> tuple[int, int] | None:
    """(Rm, index) for FP by-element forms; None if reserved."""
    size, l, mbit, h = bits(e, 23, 22), bits(e, 21, 21), bits(e, 20, 20), bits(e, 11, 11)
    m = mbit << 4 | bits(e, 19, 16)
    if size == 2:
        return m, h << 1 | l
    if size == 3 and not l:
        return m, h
    return None


def _by_elem_valid(e: int) -> bool:
    if bits(e, 29, 29) or bits(e, 15, 12) != 0b1001:
        return False  # only FMUL (by element); FMLA/FMLS/FMULX/integer forms are not used by main
    if _elem_index(e) is None:
        return False
    return bool(bits(e, 30, 30)) or bits(e, 23, 22) == 2


# Advanced SIMD vector x indexed element: 0 Q U 01111 size L M Rm opcode H 0 Rn Rd
@form("simd_by_elem", 0x9F000400, 0x0F000000, valid=_by_elem_valid,
      vregs=lambda e: (bits(e, 4, 0), bits(e, 9, 5), (_elem_index(e) or (0, 0))[0]))
def _simd_by_elem(e: int) -> str:
    q, size, n, d = bits(e, 30, 30), bits(e, 23, 22), bits(e, 9, 5), bits(e, 4, 0)
    m, idx = _elem_index(e)
    sz = size & 1
    p, lane = FP[sz][0], LANE[2 + sz]
    return vec(d, {"n_": n, "m_": m},
               f"{FP[sz][1]} k_ = m_.{lane}[{idx}]; " + per_lane(lanes(q, 2 + sz), f"r_.{lane}[i] = {p}_mul(n_.{lane}[i], k_);"))


# Advanced SIMD scalar x indexed element: 01 U 11111 size L M Rm opcode H 0 Rn Rd   (FMUL only)
@form("simd_scalar_by_elem", 0xDF000400, 0x5F000000,
      valid=lambda e: not bits(e, 29, 29) and bits(e, 15, 12) == 0b1001 and _elem_index(e) is not None,
      vregs=lambda e: (bits(e, 4, 0), bits(e, 9, 5), (_elem_index(e) or (0, 0))[0]))
def _simd_scalar_by_elem(e: int) -> str:
    size, n, d = bits(e, 23, 22), bits(e, 9, 5), bits(e, 4, 0)
    m, idx = _elem_index(e)
    sz = size & 1
    lane = LANE[2 + sz]
    return vec(d, {"n_": n, "m_": m}, f"r_.{lane}[0] = {FP[sz][0]}_mul(n_.{lane}[0], m_.{lane}[{idx}]);")


# Advanced SIMD scalar three same, FABD only: 01 1 11110 1 sz 1 Rm 11010 1 Rn Rd
@form("simd_scalar_fabd", 0xFFA0FC00, 0x7EA0D400, vregs=_rd_rn_rm)
def _simd_scalar_fabd(e: int) -> str:
    sz, m, n, d = bits(e, 22, 22), bits(e, 20, 16), bits(e, 9, 5), bits(e, 4, 0)
    lane = LANE[2 + sz]
    return vec(d, {"n_": n, "m_": m}, f"r_.{lane}[0] = {FP[sz][0]}_sub(n_.{lane}[0], m_.{lane}[0]) & ~{sign_bit(sz)};")


# Advanced SIMD scalar pairwise, FADDP only: 01 1 11110 0 sz 11000 01101 10 Rn Rd
@form("simd_scalar_faddp", 0xFFBFFC00, 0x7E30D800, vregs=lambda e: (bits(e, 4, 0), bits(e, 9, 5)))
def _simd_scalar_faddp(e: int) -> str:
    sz, n, d = bits(e, 22, 22), bits(e, 9, 5), bits(e, 4, 0)
    lane = LANE[2 + sz]
    return vec(d, {"n_": n}, f"r_.{lane}[0] = {FP[sz][0]}_add(n_.{lane}[0], n_.{lane}[1]);")


# Advanced SIMD scalar two-register misc, SCVTF/UCVTF only: 01 U 11110 0 sz 10000 11101 10 Rn Rd
@form("simd_scalar_cvtf", 0xDFBFFC00, 0x5E21D800, vregs=lambda e: (bits(e, 4, 0), bits(e, 9, 5)))
def _simd_scalar_cvtf(e: int) -> str:
    u, sz, n, d = bits(e, 29, 29), bits(e, 22, 22), bits(e, 9, 5), bits(e, 4, 0)
    w = 32 << sz
    it = ("uint" if u else "int") + f"{w}_t"
    host = "float" if sz == 0 else "double"
    return vwrite(d, sz, f"bits_{FP[sz][0]}(({host})({it})ctx->v[{n}].{LANE[2 + sz]}[0])")


PERMUTE = {1: "uzp1", 2: "trn1", 3: "zip1", 5: "uzp2", 6: "trn2", 7: "zip2"}


# Advanced SIMD permute: 0 Q 001110 size 0 Rm 0 opcode 10 Rn Rd   (UZP/TRN/ZIP)
@form("simd_permute", 0xBF208C00, 0x0E000800, valid=lambda e: bits(e, 14, 12) in PERMUTE and _q_size_ok(e),
      vregs=_rd_rn_rm)
def _simd_permute(e: int) -> str:
    q, size, m, op, n, d = bits(e, 30, 30), bits(e, 23, 22), bits(e, 20, 16), bits(e, 14, 12), bits(e, 9, 5), bits(e, 4, 0)
    lane, count = LANE[size], lanes(q, size)
    h = count // 2
    kind = PERMUTE[op]
    part = 1 if kind.endswith("2") else 0
    if kind.startswith("zip"):
        stmt = f"int j = {part * h} + i / 2; r_.{lane}[i] = (i & 1) ? m_.{lane}[j] : n_.{lane}[j];"
    elif kind.startswith("uzp"):
        stmt = f"int j = 2 * (i % {h}) + {part}; r_.{lane}[i] = i < {h} ? n_.{lane}[j] : m_.{lane}[j];"
    else:
        stmt = f"int j = (i & ~1) + {part}; r_.{lane}[i] = (i & 1) ? m_.{lane}[j] : n_.{lane}[j];"
    return vec(d, {"n_": n, "m_": m}, per_lane(count, stmt))


def _tbl_vregs(e: int) -> tuple[int, ...]:
    n, length = bits(e, 9, 5), bits(e, 14, 13)
    return (bits(e, 4, 0), bits(e, 20, 16)) + tuple((n + k) % 32 for k in range(length + 1))


# Advanced SIMD table lookup: 0 Q 001110 000 Rm 0 len op 00 Rn Rd   (TBL, TBX)
@form("simd_tbl", 0xBFE09C00, 0x0E000000, vregs=_tbl_vregs)
def _simd_tbl(e: int) -> str:
    q, m, length, tbx, n, d = bits(e, 30, 30), bits(e, 20, 16), bits(e, 14, 13), bits(e, 12, 12), bits(e, 9, 5), bits(e, 4, 0)
    regs = [(n + k) % 32 for k in range(length + 1)]
    table = " ".join(f"memcpy(t_ + {16 * k}, ctx->v[{r}].b, 16);" for k, r in enumerate(regs))
    size = 16 * len(regs)
    miss = "r_.b[i]" if tbx else "0"
    body = (f"uint8_t t_[{size}]; {table} " +
            per_lane(16 if q else 8, f"r_.b[i] = m_.b[i] < {size} ? t_[m_.b[i]] : {miss};"))
    if tbx and not q:
        body += " r_.d[1] = 0;"
    return vec(d, {"m_": m}, body, keep_d=bool(tbx))


# --------------------------------------------------------------------------- SIMD structure loads/stores
#
# LD1-LD4 / ST1-ST4 (multiple structures), LD1-LD4 / ST1-ST4 (single lane), LD1R-LD4R, with
# optional post-index (Rm = 31: by the transfer size, else by Xm). Emitted as the
# pseudocode's loop, unrolled at translation time.

@dataclass(frozen=True)
class Struct:
    load: bool
    q: int
    n: int
    t: int
    m: int | None        # post-index register (31 = immediate), None = no writeback
    ebytes: int
    selem: int           # registers per structure (LD2 = 2, ...)
    rpt: int             # repetitions (LD1 with 2-4 registers)
    index: int | None    # single-lane forms: the lane
    replicate: bool = False

    @property
    def regs(self) -> tuple[int, ...]:
        return tuple((self.t + k) % 32 for k in range(self.selem * self.rpt))

    @property
    def nbytes(self) -> int:
        if self.index is not None or self.replicate:
            return self.ebytes * self.selem
        return (16 if self.q else 8) * self.selem * self.rpt


MULTI = {0b0000: (4, 1), 0b0010: (1, 4), 0b0100: (3, 1), 0b0110: (1, 3), 0b0111: (1, 1), 0b1000: (2, 1), 0b1010: (1, 2)}


def _decode_struct(e: int) -> Struct | None:
    q, load, post, single = bits(e, 30, 30), bool(bits(e, 22, 22)), bits(e, 23, 23), bits(e, 24, 24)
    n, t = bits(e, 9, 5), bits(e, 4, 0)
    m = bits(e, 20, 16) if post else None
    if not post and bits(e, 20, 16):
        return None
    if not single:
        if bits(e, 21, 21) or bits(e, 15, 12) not in MULTI:
            return None
        size = bits(e, 11, 10)
        if size == 3 and not q and MULTI[bits(e, 15, 12)][0] != 1:
            return None  # .1d only for LD1/ST1
        selem, rpt = MULTI[bits(e, 15, 12)]
        return Struct(load, q, n, t, m, 1 << size, selem, rpt, None)
    r, opcode, s, size = bits(e, 21, 21), bits(e, 15, 13), bits(e, 12, 12), bits(e, 11, 10)
    selem = ((opcode & 1) << 1 | r) + 1
    scale = opcode >> 1
    if scale == 3:  # LDnR
        if not load or s:
            return None
        return Struct(load, q, n, t, m, 1 << size, selem, 1, None, replicate=True)
    if scale == 0:
        index = q << 3 | s << 2 | size
    elif scale == 1:
        if size & 1:
            return None
        index = q << 2 | s << 1 | size >> 1
    elif size == 0:
        index = q << 1 | s
    elif size == 1 and not s:
        scale, index = 3, q
    else:
        return None
    return Struct(load, q, n, t, m, 1 << scale, selem, 1, index)


def _struct_mem(e: int) -> MemAccess | None:
    st = _decode_struct(e)
    if st is None:
        return None
    return MemAccess(st.n, 0, "post" if st.m is not None else "offset", None, st.load, st.regs, st.ebytes, vec=True)


def _struct_emit(e: int) -> str:
    st = _decode_struct(e)
    lane = LANE[{1: 0, 2: 1, 4: 2, 8: 3}[st.ebytes]]
    w = st.ebytes * 8
    out = [f"uint64_t a = {rd_reg(st.n, 1, sp=True)};"]
    if st.load:  # build the new registers in locals: all reads come from memory
        out += [f"v128 t{k}_ = {'ctx->v[%d]' % r if st.index is not None else '{{0}}'};" for k, r in enumerate(st.regs)]
    off = 0
    if st.index is not None or st.replicate:
        for k in range(st.selem):
            if st.replicate:
                count = (16 if st.q else 8) // st.ebytes
                out.append(f"{{ uint{w}_t x_ = ld{w}(a + {off}); for (int i = 0; i < {count}; i++) t{k}_.{lane}[i] = x_; }}")
            elif st.load:
                out.append(f"t{k}_.{lane}[{st.index}] = ld{w}(a + {off});")
            else:
                out.append(f"st{w}(a + {off}, ctx->v[{st.regs[k]}].{lane}[{st.index}]);")
            off += st.ebytes
    else:
        elements = (16 if st.q else 8) // st.ebytes
        for r in range(st.rpt):
            for el in range(elements):
                for s in range(st.selem):
                    k = r + s  # register index within regs (rpt > 1 implies selem == 1 and vice versa)
                    if st.load:
                        out.append(f"t{k}_.{lane}[{el}] = ld{w}(a + {off});")
                    else:
                        out.append(f"st{w}(a + {off}, ctx->v[{st.regs[k]}].{lane}[{el}]);")
                    off += st.ebytes
    if st.load:
        out += [f"ctx->v[{r}] = t{k}_;" for k, r in enumerate(st.regs)]
    if st.m is not None:
        step = f"{st.nbytes:#x}" if st.m == 31 else rd_reg(st.m, 1)
        out.append(wr_reg(st.n, 1, f"a + {step}", sp=True))
    return "{ " + " ".join(out) + " }"


# Advanced SIMD load/store multiple structures (+ post-index): 0 Q 0011 00 P L 0|1 Rm opcode size Rn Rt
FORMS.append(Form("simd_ldst_multi", 0xBF000000, 0x0C000000, valid=lambda e: _decode_struct(e) is not None,
                  emit=_struct_emit, mem=_struct_mem))
# Advanced SIMD load/store single structure (+ post-index): 0 Q 0011 01 P L R Rm opcode S size Rn Rt
FORMS.append(Form("simd_ldst_single", 0xBF000000, 0x0D000000, valid=lambda e: _decode_struct(e) is not None,
                  emit=_struct_emit, mem=_struct_mem))


# --------------------------------------------------------------------------- lookup

def find_form(enc: int) -> Form | None:
    for f in FORMS:
        if enc & f.mask == f.value:
            return f
    return None


def lift(enc: int) -> str:
    f = find_form(enc)
    if f is None:
        raise LiftError(f"no form for {enc:08x}")
    if not f.valid(enc):
        raise LiftError(f"{enc:08x} is unallocated in form {f.name}")
    return f.emit(enc)
