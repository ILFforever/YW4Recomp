# recomp/

The AArch64 → C recompiler (Python, see `docs/decisions/0006`). Its output is derived from game code and goes to `work/`, never into the repo.

| File | What |
|---|---|
| `lift.py` | Single-instruction translation: one `Form` per encoding layout, emitting C that works on a `CPUState` (`runtime/include/yw4cpu.h`) |
| `golden.py` | Makes test cases per form and runs them on the ARM64 reference machine (`pi/`) to get golden results |
| `check.py` | Compiles the lifted C for every test case on the host and compares against the golden results |
| `coverage.py` | How much of `main` the lifter handles, and what to implement next |
| `pi/` | The harness that runs one instruction natively on ARM64 Linux (copied to and built on the Pi) |

Workflow when adding or changing a form:

```
python -m recomp.golden <form>     # needs [tools] pi_host / pi_key
python -m recomp.check <form>      # must print "all forms match"
python -m recomp.coverage          # see what's still missing
```
