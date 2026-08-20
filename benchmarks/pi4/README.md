# Pi 4 benchmark harness

Three scripts, run on the Pi. They exist in this form because a long list of
measurements taken without them had to be withdrawn, and each guard below maps
to one of those.

## `bench_guarded.sh`

Paired A/B, alternating A/B then B/A, one JSON object per run appended to
`<out>.jsonl` — discarded runs included, with their reason and their raw
samples, so an exclusion can be audited instead of trusted.

```
bench_guarded.sh <A.so> <A-label> <B.so> <B-label> <out-prefix> <title> <cputhread> [pairs]
```

`title` is `GC6E01` (Colosseum; the intro drives itself, fixed 150 s settle) or
`GLME01` (Luigi's Mansion; needs `drive_loop.py` to reach the foyer).

What it refuses to measure, and why each rule is there:

| rule | the measurement it prevents |
| --- | --- |
| `flock`, not `pgrep` | two benchmarks racing — `pgrep` can see zero twice |
| instance count re-checked *after* launch | sway autostarting a second emulator behind the run |
| config rewritten and read back each run | Dolphin rewrites `CPUThread` on exit; the next run inherits it |
| `cycles > 1e9` | the module not executing at all |
| two framebuffer hashes plus a size floor | a black screen reporting a plausible FPS |
| throttle word compared before/after | one arm throttling and the other not |
| sha256 of DOL, module and runtime recorded | a module built from a patched DOL |

Two limits, stated rather than worked around. FPS is scraped from Dolphin's
window title and is an **average**, so per-frame times are not available and no
frame-time percentiles are produced; the spread of the sampled series is
reported instead and is a different quantity. And this Pi reports
`throttled=0x50000` — under-voltage and throttling have both occurred since
boot — so the thermal guard is load-bearing here, not ceremonial.

## `build_module.sh`

Links a cross-emitted `generated/` tree into a loadable module.

```
build_module.sh <arm-name> <game-id>
```

Refuses to build unless the recompiled `generated/main.dol` matches the DOL the
runtime will load. Colosseum modules were once built from a patched image that
differed in 15 bytes and measured for a day before anyone noticed.

## `profile_run.sh`

Settles into the measured scene, then runs `perf stat` and `perf record` against
the live process and reports samples by object and by symbol.

```
profile_run.sh <module.so> <title> <cputhread> <out-prefix>
```

`perf_event_paranoid` is 2 on this Pi, so this is userspace only. That is enough
to separate the generated module from the JIT from the runtime, which is the
split worth having: it is what turns "the framerate went up" into "66% of
samples are in the module and the fallback count is zero".

## Cache

The object cache is keyed on a digest of the sources that generate the objects,
so an emitter change invalidates it without anyone editing a version string.
`DOLRECOMP_LLVM_CACHE_EXPLAIN=1` prints why each job hit or missed.
