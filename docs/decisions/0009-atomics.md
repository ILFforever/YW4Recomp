# 0009 — Atomics: exclusives, acquire/release

Date: 2026-10-20
Status: accepted

## What main uses (measured)

| Instruction | Count |
|---|---|
| `ldarb` / `ldar` (32-bit) | 7,177 / 1,119 |
| `stlr` (32-bit) / `stlrb` | 373 / 83 |
| `ldaxr` + `stlxr` (32-bit) | 2,581 + 2,580 |
| byte exclusives (`ldaxrb`/`stlxrb`/`stxrb`) | 90 |
| 64-bit exclusives (`ldxr`/`stxr`) | 10 |
| `clrex` | 1,189 |
| `dmb` | 16 |

There are no exclusive pairs (`ldxp`/`stxp`) and no ARMv8.1 atomics (the A57 has none).

## Decision

Guest threads will be host threads on shared memory (decision 0005), so these become real host atomics (`__atomic` builtins on the guest address):

- **`ldxr`/`ldaxr`** do an atomic load (relaxed / acquire) and record a reservation in `CPUState`: address, size, and the value seen.
- **`stxr`/`stlxr`** succeed only if there is a reservation for the same address and size *and* a host compare-and-swap from the recorded value to the new one succeeds. Status 0 = stored, 1 = failed. The reservation is always cleared, as on hardware.
- **`clrex`** clears the reservation.
- **`ldar`** is an acquire load.
- **`stlr` is a sequentially consistent store, not just a release.** On ARM, `stlr` followed by `ldar` can't be reordered (RCsc). On x86 a release store followed by an acquire load can be reordered (store buffering); a seq_cst store (`xchg`) prevents it.
- **`dmb`** becomes a full fence.

## Known difference: ABA

A hardware exclusive store fails if *any* write hit the location since the `ldxr`. The CAS only checks that the value is the same, so it succeeds if another thread changed the value and changed it back. This doesn't matter for the loops compilers emit for counters, flags, locks and plain compare-and-swap: they only care about the value. It would matter for hand-written lock-free stacks that rely on LL/SC to avoid ABA. If such code turns up, the fix is a per-address version counter in a side table.

## Testing

- Single instructions (golden tests): `ldxr`/`ldaxr` (value loaded) and `ldar`/`stlr`, with naturally aligned addresses: ARMv8.0 faults on unaligned exclusive/acquire/release accesses.
- A lone `stxr` is not golden-tested, because its result after the harness's own code is IMPLEMENTATION DEFINED.
- Whole loops (`tests/functest/atomics.S`): atomic add, compare-and-swap with `clrex` on the failure path, `stxr` without a reservation, byte and 64-bit exclusives, and acquire/release. All match the Pi. Planted bugs ("store ignores the reservation", "clrex does nothing") are caught.
- Between threads (2026-10-21, decision 0015): `tests/test_kernel.py` runs 8 host threads × 100,000 increments through the recompiler's own `ldaxr`/`stlxr` output; all 800,000 land. With `stlxr` planted as a plain store, 167,482 did.
