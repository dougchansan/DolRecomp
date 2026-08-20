# Raspberry Pi 4: what is left between static recompilation and the JIT

The AArch64 bring-up established that the module executes, that alias metadata
and the MEM1 base hoist are worth having, and that roughly 1.9× still separates
this backend from Dolphin's own JIT. This document is the next step: confirm the
baseline, measure where the time actually goes, and decide what to do about it
on evidence rather than on the plausibility of the explanation.

Everything below was measured on one Raspberry Pi 4 with Pokémon Colosseum
unless stated otherwise. Numbers from other configurations are not mixed in;
that is how the "1.54× behind" figure had to be withdrawn.

## 1. Confirmed starting state

### Repositories

| what | value |
| --- | --- |
| DolRecomp branch | `pi4-gap-analysis`, off `aarch64-integration` (`519e5f9`) |
| commits added here | `a0595ca` cache key, `f70ae29` reservation guard, `1b39dac` cache design |
| build host toolchain | clang/LLVM 20.1.8, x86-64 Windows |
| Pi toolchain | gcc 14.2.0, clang 19.1.7 (Debian 13 trixie) |

One thing worth stating plainly, because it is easy to get wrong: the branch
carrying the engineering report, `feature/llvm-aot-regions` (`20cb163`), **cannot
emit AArch64 at all**. `dolllvm_emit_object` rejects any triple that is not
x86-64 Linux or Windows. The AArch64 target registration and both optimisations
live on `aarch64-integration`, and that is the tree every Pi module was built
from. Work aimed at the Pi that lands on the report branch is not testable on
the Pi.

The Pi's `ModernGekko` tree is an rsync'd copy, not a git checkout, so there is
no commit to quote for it. Provenance there is by content hash only. Its
`DolRecomp` tree is stale (`dolllvm-v7`, no alias metadata) and is not what
built any recent module.

### Hardware and drivers

| what | value |
| --- | --- |
| board | Raspberry Pi 4 Model B Rev 1.2, revision `c03112` |
| clock | `arm_freq=1850`, `over_voltage=0`, measured 1,850,449,280 Hz |
| kernel | `6.18.39+rpt-rpi-v8` |
| OS | Debian GNU/Linux 13 (trixie) |
| firmware | `288930ab4712b99596f32732664aaaeb881ef1e0` |
| graphics | Mesa `mesa-252`, `libvulkan_broadcom.so`, ICD API 1.3.318 |
| thermal | `throttled=0x50000` |

`0x50000` is bits 16 and 18: **under-voltage has occurred and throttling has
occurred** since boot, though neither was active when read. This is a live risk
to every measurement on this machine and nothing before now recorded it. The
harness now samples it before and after each run and discards any run whose
throttle word changed.

### Artefacts

| what | sha256 | size |
| --- | --- | --- |
| Colosseum DOL (`GC6E01`) | `7e6c00a3…52c9f4` | — |
| Luigi's Mansion DOL (`GLME01`) | `5fa47c05…65276` | — |
| runtime `mgnew` | `3ea0a3a6…f45b4a` | 20,697,744 |
| module A (baseline) | `d8bf5bee…0528a35` | 56,942,240 |
| module B (reservation guard) | `63317e99…e177ec0f` | 57,204,384 |

Colosseum ships no `.rel` files, so `main.dol` is the whole of its code. Both
modules were generated in this session from the hash-verified DOL with
`--backend llvm-aot`, region mode `cfg`, `--memory-mode fast`, opt level 2, IPO
on — the settings the existing modules were built with, read back out of their
CMake caches rather than assumed.

Module A came out at **exactly** the byte count of the previously established
`pc-opt` baseline, 56,942,240. That is a useful accident: it says the toolchain
has not drifted between the earlier measurements and these.

### The fixes are actually present

| claim | how it was checked |
| --- | --- |
| AArch64 cross-emission | emitted object is `elf64-littleaarch64`, `EM_AARCH64` |
| alias metadata | `createAnonymousAliasScope` in the emitter, 3 sites |
| MEM1 base hoist | `ram_base_` set in `emitEntry`, used at both lowering sites |
| gather-pipe fix | `GetExceptionCheckTarget` in `JitBase.h`, `StaticRecompCore.h`, and used at `JitInterface.cpp:329` |
| ARM64 yields to the static core | `g_static_recomp_core->ShouldYieldAt` in `JitBaseBlockCache::Dispatch()` |
| correct DOL recompiled | build refuses unless `generated/main.dol` matches the runtime's DOL |

The yield lives in `JitCache.cpp`, which both Jit64 and JitArm64 use — not in
`JitArm64/`. Looking for it in the ARM64 directory finds nothing and invites the
wrong conclusion.

### The module is executing

Not inferred from the framerate. From the runtime's own counters over a 30
second window in the measured scene:

```
native=132692538  fallback=0  native_exc=40889  hook_fb=0
bursts=2731429    cycles=25072849566            verifications=1255
```

and independently from `perf`: **66.1% of samples are in
`gGC6E01_recomp.so`**, against 0.58% in the fallback JIT. Static code is not
merely running, it owns the workload, and the fallback count is zero rather than
small.

## 2. Benchmark integrity

Each guard exists because its absence produced a number that had to be
withdrawn. They are now in `bench_guarded.sh`, which writes one JSON object per
run to a `.jsonl` file — including the discarded runs, with their reason and
their raw samples, so an exclusion can be audited rather than taken on trust.

| guard | failure it prevents |
| --- | --- |
| `flock` on a lock file | two benchmarks racing; `pgrep` can see zero twice |
| instance count re-checked after launch | sway autostarting a second emulator behind the run |
| config rewritten every run, then read back | Dolphin rewriting `CPUThread` on exit and the next run inheriting it |
| DOL / module / runtime sha256 recorded | measuring a module built from a patched DOL |
| codegen source hash in the cache key | measuring the previous emitter's objects |
| `cycles > 1e9` | the module not executing at all |
| two framebuffer hashes, size floor | a black screen reporting a plausible FPS |
| throttle word before and after | one arm throttling and the other not |
| alternating A/B and B/A order | warm-up or drift landing on one arm |

Two limits worth stating rather than papering over. Framerate is scraped from
Dolphin's window title, which reports an average — **per-frame times are not
available**, so no frame-time percentiles are reported here. The spread of the
sampled FPS series is reported instead and is a different quantity. And
`perf_event_paranoid` is 2, so profiles are userspace only; that is sufficient
to separate the module from the JIT from the runtime, which is the split that
matters.

### The object cache could not tell emitters apart

The cache key hashed the guest instructions, the target triple, a codegen
fingerprint and the opt level — and nothing about the emitter. The only barrier
between an emitter change and a stale object was a hand-edited version string,
and the comment above that string in `pipeline.c` already recorded the
consequence: *"because the cache key does not hash the emitter's source, nothing
else would have noticed."*

It went unnoticed five times, twice into published numbers.

CMake now hashes the files that decide what is emitted — the LLVM emitter,
memory and runtime lowering, DolIR, and the `CPUState` layout — and folds the
digest into the key. Those files are configure dependencies, so editing one
re-runs configure. A configure-time check fails the build if a file appears
under `src/backend/llvm/` without being listed. When the digest is absent the
cache is written but never read, because a slow build is recoverable and a wrong
benchmark is not. `DOLRECOMP_LLVM_CACHE_EXPLAIN=1` prints why each job hit or
missed.

Verified both ways: a one-line comment in the emitter changes the digest, and
reverting it restores the digest exactly. The reservation change in this
document invalidated the cache on its own, with no version bump.

## 3. Where the time goes

Colosseum, intro reference scene, single core, arm B, 30 seconds of `perf` at
499 Hz. 16K samples, 0 lost.

| object | share |
| --- | --- |
| `gGC6E01_recomp.so` (generated) | 66.11% |
| `mgnew` (runtime, Dolphin, DSP, video) | 24.64% |
| `libc` | 3.76% |
| `libvulkan_broadcom` | 1.88% |
| pulse / dbus / wayland | 2.55% |
| `[JIT]` | 0.58% |

Ranked symbols:

| symbol | share | what it is |
| --- | --- | --- |
| `func_800FE9B0_budget` | 21.67% | one generated function |
| `chassis_dispatch` | 3.00% | dispatch into generated code |
| `Mixer::MixerFifo::Mix` | 2.76% | audio mixing |
| `StaticRecompCore::Run` | 2.62% | the static core's own loop |
| `GetHash64_ARMv8_CRC32` | 2.03% | texture hashing |
| `__memset_zva64` | 1.72% | — |
| `func_8009DF64_budget` | 1.19% | generated |
| `func_8015D7D0_budget` | 1.07% | generated |
| `HookExternalWrite` | 0.86% | MMIO path |
| `AXUCode::ProcessPBList` | 0.84% | DSP HLE |
| `CopyRenderTargetToTexture` | 0.76% | EFB copy |
| `psq_load_value` | 0.63% | paired-single helper, out of line |
| `ppc_fma` | 0.62% | FMA helper, out of line |
| `CompileExceptionCheck` | 0.52% | the gather-pipe fix |
| `psq_store_value` | 0.51% | paired-single helper, out of line |

Counters for the same window:

```
instructions:u   64,859,682,964      IPC 1.10
cycles:u         58,929,096,793
branch-misses:u     215,889,249      0.33% of instructions
cache-misses:u       78,492,628
```

Four things follow, and they redirect the effort.

**A third of the time is not the recompiler's to win.** 33.9% of samples are
outside the generated module. Audio mixing, DSP HLE, texture hashing and EFB
copies alone are about 7%. Even an infinitely fast module would be bounded at
roughly 2.9× here, against the 1.9× gap — so the gap is addressable in
principle, but not by a wide margin, and any claim that a CPU-side change will
recover all of it is wrong before it is tested.

**One function is 21.67% of everything.** That concentration is unusual and
makes `func_800FE9B0` the single most valuable object of study in the whole
module. Its instruction mix says what it is spending time on:

| | count | share |
| --- | ---: | ---: |
| total instructions | 3,935 | |
| loads | 836 | 21.2% |
| stores | 458 | 11.6% |
| branches | 568 | 14.4% |
| `rev` (byte swaps) | 114 | 2.9% |
| floating point | 0 | 0% |

There are 1,294 memory instructions but only 114 byte swaps, and a guest memory
access always carries one. So on the order of 200 of those 1,294 touch guest
memory and **roughly a thousand — about a quarter of the whole function — are
`CPUState` traffic**. The function is integer-only: not one floating-point
instruction in 3,935.

That is the register-cache argument made specific. In the hottest code in the
program the dominant cost is loading and storing guest registers to memory, in
exactly the integer/compare-heavy shape that GPR and CR/XER caching targets, and
`b.hi` appears 92 times against 114 `rev` — the MEM1 bound checks, roughly one
per access, already about as cheap as the current scheme allows.

**The gather-pipe fix costs 0.52%, not 2.3%.** The 2.3% figure came from a
Luigi's Mansion comparison. On the title that actually needs the fix it is a
fifth of that. Phase 7's optimisation work is correspondingly less attractive.

**Paired-single and FMA helpers are out-of-line calls** totalling ~1.8%. They
are not inlined into generated code, and each call is also a state barrier.

IPC of 1.10 on a 3-wide out-of-order core, with branch misses at only 0.33% of
instructions, points at dependency stalls and memory traffic rather than
mispredicted control flow — which is consistent with the `CPUState` traffic
thesis and inconsistent with a branch-density explanation.

## 4. Memory: what the fast path actually costs

`--memory-mode fast` already does what Phase 4 calls guard hoisting, at module
granularity: `ram_size == GC_MAIN_RAM_SIZE` and "no write journal" are checked
once at dispatch entry, and if either fails the module refuses to run natively
and the interpreter takes over. A violated assumption costs speed, not
correctness. That mechanism is sound and is the right place to hang further
assumptions.

The emitted AArch64 for a 32-bit guest store, with the MEM1 bound folded to a
constant:

```
and  x11, x8, #0xffffffffbfffffff   ; normalise (clear bit 30)
eor  x12, x11, #0x80000000          ; rebase off 0x80000000
cmp  w12, w15                       ; against the folded bound
b.hs <slow>
rev  w8, w2                         ; byte swap
str  w8, [x10, x12]                 ; x10 is the hoisted MEM1 base
```

Six instructions where Dolphin's fastmem needs two. The remaining check is one
compare and one never-taken branch; removing it entirely requires
fault-assisted fastmem, and on the evidence above that would buy at most a few
percent of the 66% while introducing signal handling that interacts with the
runtime's own fault handling. It is not where the next win is.

The MEM2 path is dead code for both test titles — a GameCube image has no MEM2 —
but it sits on the *not-MEM1* edge, so it costs module size and instruction
cache rather than instructions on the hot path. Removing it is a real but small
win and cannot use a one-time entry guard the way `ram_size` does, because
`cpu_alloc_mem2` may be called by the embedding runtime after entry. Left
undone; recorded as a candidate rather than attempted.

### What was found instead: reservations

Every guest store in the program began with

```
reserve_valid = reserve_valid AND stored_line_differs
```

computed unconditionally — two `CPUState` loads, an xor, a compare, a select and
a `CPUState` store — to maintain a flag that is almost never set. When the flag
is already false the expression is just false, so the whole computation is a
no-op and guarding it is *equivalent*, not approximate.

Scanning the images for the only instruction that can set the flag:

| title | reservation instructions in `main.dol` |
| --- | --- |
| Pokémon Colosseum | **none at all** |
| Luigi's Mansion | 2 `lwarx`, 2 `stwcx.` |
| Mario Kart: Double Dash | 1 `lwarx` |

Colosseum ships no RELs, so its reservation flag can never be set by anything —
none of that work could ever have done anything. Statically the unguarded form
was 216,002 reservation-slot instructions in the Colosseum module and 294,856 in
the Luigi's Mansion module.

The correctness of the guard does not depend on the scan. The scan only sets the
expected size of the win.

On AArch64 the store site goes from 19 executed instructions to 15, and loses
one `CPUState` load and one `CPUState` store — the traffic the profile says
dominates. The module does not shrink; it grows by 262,144 bytes, because the
work moves behind a never-taken branch rather than disappearing.

### And it makes no measurable difference

Colosseum, single core, five alternating pairs, twenty samples per run. All ten
runs valid — no discards, throttle word unchanged in every one, `CPUThread`
verified `False` in every one, both modules built in the same session from the
same hash-verified DOL.

| pair | baseline | guarded | delta |
| ---: | ---: | ---: | ---: |
| 1 | 15.93 | 15.84 | −0.57% |
| 2 | 15.96 | 16.07 | +0.69% |
| 3 | 15.88 | 15.73 | −0.94% |
| 4 | 15.94 | 15.72 | −1.38% |
| 5 | 15.56 | 15.73 | +1.09% |
| | **15.857** | **15.820** | **−0.23%** |

Three of five pairs favour the baseline. The mean pair delta is −0.22% with a
standard error of 0.43%, so this design resolves roughly ±1% and the effect —
in either direction — is inside the noise.

The change removes four executed instructions from every guest store in the
program, including one `CPUState` load and one `CPUState` store, on a title
where the work it removes is provably dead. It buys nothing measurable.

**This is the most useful result in this document, because it is a controlled
test of the thesis the whole plan rests on.** The report attributes ~67% of
hot-loop time to `CPUState` traffic. If that traffic were the binding
constraint, deleting a load and a store from the single most frequent operation
in the program should have moved something. It did not.

The profile says why. IPC is 1.10 on a 3-wide out-of-order core with branch
misses at 0.33% of instructions: the core is stalled on dependencies and memory
latency, not starved of issue slots. The removed accesses were to two adjacent
bytes of a `CPUState` that is hot in L1 on every access, so they consumed issue
slots the core had spare and never stalled it. Statically the module even grew
0.50% — over the same 60 regions, 1,060,570 instructions became 1,065,851, and
the reservation instructions are still present, having moved behind a branch
rather than disappeared.

The correction this forces is specific: **reducing `CPUState` instruction count
is not the same as reducing `CPUState` cost.** A register cache is not valuable
because it removes loads and stores; it is valuable only if it breaks the
load-use dependency chains those loads and stores create, by keeping a value in
a host register across the guest instructions that use it. An incremental change
that removes a fraction of the traffic while leaving every chain intact should
be expected to measure zero — as this one did.

That is a direct argument for the staged plan in
[register-cache-design.md](register-cache-design.md) over any further
peephole work on state traffic, and an argument against expecting partial
credit from the early stages.

The change is kept: it is provably equivalent, it removes work that can never do
anything on this title, and it makes the state model marginally smaller for the
cache work. It is **not** presented as an improvement, and a reviewer who
preferred to revert it on the grounds that it costs 0.5% of module size for no
measured return would have the evidence on their side.

## 5. Cycle accounting

Not attempted here, and the reason is in the profile rather than in an opinion
about the design. The per-instruction accounting is visible in the emitted code
as a load, an add and a store against the cycle guard pointer on every guest
instruction, and it is genuinely expensive. But the previous attempt — guard
counters accumulated and settled at every barrier — was slower than what it
replaced, because every barrier had to settle the counter, and barriers are
frequent.

The block-segment model described in the mission is the right shape, and the
prerequisite for it is the same prerequisite the register cache has: a barrier
that names a subset instead of "everything". That is stage 3 of
[register-cache-design.md](register-cache-design.md), and doing cycle accounting
before it repeats the guard-counter mistake with extra steps.

## 6. Ranking

The null result above reorders this list, because it retires a whole class of
candidate. Anything whose mechanism is "emit fewer `CPUState` accesses" now has
a measured precedent saying it will not register.

| candidate | expected | evidence | risk | complexity | general |
| --- | --- | --- | --- | --- | --- |
| register cache, staged, breaking dependency chains | largest | 66% in module; hottest function ~27% state traffic, IPC 1.10 | high without the schema | very high | yes |
| CR/XER subfield caching | large | hottest function is integer/compare-heavy, zero FP | high | high | yes |
| study `func_800FE9B0` | unknown, concentrated | 21.67% of all samples | none — analysis | low | title-specific |
| inline psq/FMA helpers | ≤1.8% | profile; also removes state barriers | low | medium | yes |
| block-segment cycles | medium | visible in emitted code | medium | high | yes |
| representative PGO | unknown | not re-measured this round | none | low | per title |
| ~~reservation guard~~ | ~~small, broad~~ | **measured: −0.23%, inside noise** | very low | done | yes |
| ~~drop MEM2 path~~ | ~~small~~ | same mechanism as the null result | low | low | GC only |
| gather-pipe specialisation | ≤0.52% | profile | medium | medium | no |
| fault-assisted fastmem | ≤4 insn/access | §4; same mechanism as the null result | high | high | yes |

Three demotions, all on measurement rather than taste:

- **The reservation guard** measured zero. It is struck through because it was
  tested, not because it was rejected on reasoning.
- **Dropping the MEM2 path** and **fault-assisted fastmem** both work by
  emitting fewer instructions per memory operation. That is precisely the
  mechanism that just measured zero, and fault-assisted fastmem would buy at
  most four instructions per access — fewer than the reservation guard removed
  per store — in exchange for signal handling that interacts with the runtime's
  own fault handling. Neither is worth doing next.
- **Gather-pipe specialisation** costs 0.52% on the title that needs it, not the
  2.3% measured on a title that does not.

What survives is the work that changes the *shape* of the generated code rather
than its instruction count: keeping values in host registers across guest
instructions, and removing out-of-line helper calls that are also state
barriers.

## 7. Not done, and why

- **Representative PGO (Phase 8).** The `.profdata` files from the earlier round
  were deleted in a disk cleanup and profiles must be collected again. The
  existing 22–25% figures remain same-scene upper bounds; the only held-out
  number in the project is +5.6%, on x86-64. No AArch64 held-out figure exists
  and none is claimed.
- **Colosseum beyond the intro (Phase 9).** Not exercised past the boot videos
  in this round. The gather-pipe fix is confirmed present and the title reaches
  a rendering intro with zero fallbacks, which is not the same as a validated
  playthrough.
- **Luigi's Mansion re-measurement.** The reservation change was measured on
  Colosseum, where the win should be largest and the harness needs no pad. LM
  has two `lwarx` sites, so its flag can be set and its branch occasionally
  taken; that arm is unmeasured.
- **Held-out scenes.** One scene per title. The mission asks for held-out
  gameplay scenes and they are not yet automated for Colosseum.

## 8. Next

1. Study `func_800FE9B0`. One function at 21.67% of all samples, integer-only,
   about a quarter of it state traffic, is worth more than any broad change on
   the list — and it is analysis, so it costs nothing but time.
2. Stages 1–3 of the register cache: the description, per-helper effect
   declarations, then narrowing them. Stages 1 and 2 are byte-identical by
   construction and produce no speedup; they are the ones skipped four times.
3. Inline the paired-single and FMA helpers. Bounded at ~1.8%, but each call is
   also a state barrier, so it is one of the few cheap changes that alters code
   shape rather than instruction count.
4. Re-collect profiles and measure held-out PGO on AArch64 properly.

And one methodological item, given §4: **any candidate whose mechanism is
"fewer `CPUState` accesses" should be expected to measure zero** until something
demonstrates otherwise. The next state-related change to be benchmarked should
be one that keeps a value in a host register across guest instructions — if that
also measures zero, the 67% attribution itself is wrong and the plan needs
rebuilding from the profile rather than from the report.
