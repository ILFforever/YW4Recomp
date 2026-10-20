"""Translate single ARM64 instructions to C statements.

Each instruction *form* (a group of encodings sharing one bit layout, e.g.
"add/sub immediate") is a `Form` with:

    mask, value   enc & mask == value identifies the form
    valid(enc)    False for unallocated/reserved encodings within the form
    emit(enc)     C statements operating on `ctx` (a CPUState *)
    mem(enc)      loads/stores only: the decoded MemAccess (the test generator
                  uses it to point the base register into its scratch buffer)

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


FORMS: list[Form] = []


def form(name: str, mask: int, value: int, valid=lambda e: True):
    def register(fn: Callable[[int], str]) -> Callable[[int], str]:
        FORMS.append(Form(name, mask, value, valid, fn))
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

    @property
    def writeback(self) -> bool:
        return self.index != "offset"


def sign_extend(v: int, width: int) -> int:
    return v - (1 << width) if v >> (width - 1) & 1 else v


def _checked(a: MemAccess) -> MemAccess | None:
    # Both cases are CONSTRAINED UNPREDICTABLE in the Arm ARM; compilers never emit them.
    if a.writeback and a.n != 31 and a.n in a.regs:
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
    for k, t in enumerate(a.regs):
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
