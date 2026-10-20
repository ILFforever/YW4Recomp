# recomp/

The AArch64 → C recompiler (Python, see `docs/decisions/0006`). Its output is derived from game code and goes to `work/`, never into the repo.

| File | What |
|---|---|
| `lift.py` | Single-instruction translation: one `Form` per encoding layout, emitting C that works on a `CPUState` (`runtime/include/yw4cpu.h`) |
| `function.py` | Whole-function translation: finds function bodies by following control flow, branches, calls, jump tables, PC-relative values (`docs/decisions/0007`) |
| `translate.py` | Translates all of `main` into `work/recomp/` (version-locked) |
| `functest.py` | Whole-function tests: code from `tests/functest/` compiled and run on the Pi vs. its translation on the host |
| `golden.py` | Makes test cases per form and runs them on the ARM64 reference machine (`pi/`) to get golden results |
| `check.py` | Compiles the lifted C for every test case on the host and compares against the golden results |
| `coverage.py` | How much of `main` the lifter handles, and what to implement next |
| `pi/` | Runs test instructions (`harness.c`) and test functions (`funcrun.c`) natively on ARM64 Linux; `testcase.h` is shared with the host checkers |

Workflow when adding or changing a form:

```
python -m recomp.golden <form>     # needs [tools] pi_host / pi_key
python -m recomp.check <form>      # must print "all forms match"
python -m recomp.coverage          # see what's still missing
```

After changing control flow or function translation:

```
python -m recomp.functest          # must print "all functions match"
python -m recomp.translate         # all of main -> work/recomp/ (prints what is untranslated)
```
