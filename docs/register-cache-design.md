# A successor state model for guest register caching

Guest state lives in `CPUState`. Every read of a guest register is a load from
it and every write is a store to it, and on AArch64 that traffic is the largest
single cost in the generated code — roughly two thirds of hot-loop time in the
profile that motivated this work. Dolphin's JIT does not pay it, because it
keeps guest registers in host registers between guest instructions.

Four attempts have been made to close that gap by promoting guest state into
SSA values or allocas. All four were reverted. The last one is the reason this
document exists rather than a fifth patch: it ran for some time and then
produced

    unknown guest instruction pc=0x80394ac4 opcode=0x4c322049

`0x4c322049` is ASCII `L2 I`. The guest PC had been given a value that was never
a code address, and the failure surfaced far from the promotion that caused it.

The pattern across all four is the same. Each was a localized change to the
emitter that promoted a few slots where it looked safe, with the knowledge of
what is safe living in the head of whoever wrote the patch. Nothing in the tree
recorded which slots a helper could write, which values had to be settled before
a barrier, or which values were allowed to become a branch target. The emitter
and the state layout were edited independently and agreed only by convention.

The conclusion carried forward from that work — that state promotion should not
be attempted again until the state model and the emitter derive from one shared
description — is the premise here.

## What exists today

`DolIRStateSlot` enumerates the architectural state as a flat list: `GPR0`–
`GPR31`, `FPR0`–`FPR31`, `PS1_0`–`PS1_31`, then `PC`, `LR`, `CTR`, `CR`, `XER`,
`FPSCR`, `MSR`, `SRR0`, `SRR1`, `DAR`, `DSISR`, `EAR`, `HID2`, `TIMEBASE`,
`SR0`–`SR15`, `GQR0`–`GQR7`, `EXCEPTION`, `PROGRAM_EXCEPTION`, `RESERVE_ADDR`,
`RESERVE_VALID`, `DOWNCOUNT`.

A slot is an index and nothing else. It carries no width, no lane structure, no
statement about who may write it, and no statement about whether it is visible
to an exception, a savestate, or a debugger. `FunctionEmitter` holds a parallel
`state_[]` array of pointers and a `used_[]` array of flags, and the
correspondence between a slot's index, its `CPUState` offset, its LLVM type and
its liveness is re-established by hand at each use site.

Effects are a seven-bit mask on an instruction: `READ_STATE`, `WRITE_STATE`,
`READ_MEMORY`, `WRITE_MEMORY`, `MAY_EXIT`, `MAY_RAISE`, `BARRIER`. `WRITE_STATE`
means *some* state, so a helper that writes one CR field is indistinguishable
from one that writes everything. Under a caching model that difference is the
whole problem: the coarse mask forces a full flush at every helper call, which
is exactly what made the guard-counter experiment slower than the per-
instruction accounting it replaced.

That is the shape of the gap. The slots are a list of names; what is needed is a
description of what each name *is*.

## The description

One table, checked into the tree, from which both the state layout and the
emitter's bindings are derived. Nothing that consumes guest state may hardcode a
field offset, a width, or a lane index; it reads them from here.

Each entry declares:

| field | meaning |
| --- | --- |
| `name` | `gpr3`, `fpr7.ps0`, `cr.field2`, `xer.ca` |
| `class` | `gpr`, `fpr`, `ps`, `cr_field`, `xer_field`, `spr`, `control`, `timing`, `internal` |
| `guest_width` | architectural width in bits |
| `host_repr` | `i32`, `i64`, `f64`, `v2f32` |
| `lane` | lane index within a paired register, or none |
| `storage` | the `CPUState` member and byte offset this derives from |
| `aliases` | other entries that overlap this storage |
| `read` / `write` | `plain`, `synthesised` (CR fields), `side_effecting` (FPSCR) |
| `helper_clobber` | default assumption when a helper does not declare otherwise |
| `exception_visible` | must hold the architectural value when an exception is taken |
| `savestate_visible` | must be settled before a savestate boundary |
| `debugger_visible` | must be settled before the debugger reads state |
| `commit` | `on_barrier`, `on_exit`, `always` |

Two details in that table matter more than the rest.

**Subfields are entries, not extractions.** `CR` is one 32-bit `CPUState` member
but eight independently written fields, and `XER` carries `SO`, `OV` and `CA`
plus a byte count. Today an emitter that wants `cr2` reads `CR`, shifts and
masks; a cache built on that granularity cannot keep `cr2` in a register without
keeping all of `CR`, and every write to any field invalidates every other. The
description names the fields, declares `CR` as their alias, and the derivation
generates the packing and unpacking. This is where a large share of the win
lives: comparison-heavy guest code writes one CR field per instruction and reads
it one instruction later.

**Paired singles are two entries with a declared relationship.** `FPR<n>` and
`PS1_<n>` are separate slots today, and their relationship — that some
instructions write `ps0` and splat, some write both lanes, and some write `ps0`
while leaving `ps1` untouched — is encoded only in `dolir_builder.c`. A previous
bug in exactly this area (`lfd` and `fmr`/`fneg`/`fabs`/`fsel` splatting into the
high lane when they should not) is what forced cache version `v7`. The
description states the lane structure once and the emitter derives from it.

## Cache model

```
GuestValue      one entry in the description
CachedValue     what the backend currently holds for a GuestValue
RegisterCache   the map, plus the invalidation rules
StateBarrier    a point where some subset must be settled
HelperEffects   what a call does to the map
BlockEntryState what a block may assume on entry
BlockExitState  what a block guarantees on exit
```

A `CachedValue` tracks:

```
valid       the cache holds something for this value
dirty       what it holds differs from CPUState
constant    the value is known, and its constant
ssa         the backend value, when not constant
guest       which GuestValue this is
lane        which lane, for paired registers
repr        the representation currently held
```

`repr` is explicit because the same guest value is usefully held in more than one
form. A GPR used as an address wants `i64` zero-extended; the same GPR used
arithmetically wants `i32`. Allowing both, and recording which one is in hand,
avoids a round trip through `CPUState` to change representation — but only if the
cache knows they are the same value, which is why the representation is a field
rather than a separate entry.

`constant` is not an optimisation detail. It is the mechanism by which a promoted
value can become a branch target, and therefore the mechanism that produced
`L2 I`. It is addressed below.

## Helper effects

The coarse mask is replaced by a declaration per helper:

```
reads          the set of GuestValues read
writes         the set written
clobbers       the set whose value becomes unknown
may_raise      an exception may be taken here
may_change_pc  control may not return to the next instruction
may_schedule   the scheduler may run
touches_mmio   ordering against guest memory matters
needs_full     everything must be settled: fallback, savestate, debugger
no_return      control does not come back
```

`needs_full` remains, and some helpers genuinely need it — the interpreter
fallback reads and writes arbitrary state and must see a fully settled
`CPUState`. The point is that it is declared per helper rather than assumed for
all of them. `ppc_stwcx_op` reads `RESERVE_VALID`, `RESERVE_ADDR` and the
addressed GPR and writes `CR0` and `RESERVE_VALID`; under the current mask it
forces everything to be flushed and reloaded.

Every helper must carry a declaration. A helper without one is a build error,
not a default — a silent default is how the coarse mask became load-bearing.

## Barriers

A barrier names a subset, not "everything". The subsets:

- **Exception barrier.** Everything with `exception_visible`. The guest PC must
  be the architectural PC of the faulting instruction, not the block entry.
- **Exit barrier.** Everything with `commit = on_exit`, plus every dirty value.
  Taken at any return to the chassis.
- **Call barrier.** What the callee's declaration says it reads, plus anything
  it may clobber written back first.
- **Scheduler barrier.** Timing state and anything `savestate_visible`, because
  a scheduled event may serialise.
- **Fallback barrier.** Everything. The interpreter is not described.

The rule that the guard-counter experiment violated: a barrier that must settle
*everything* at *every* point is not a barrier, it is a flush, and the bookkeeping
costs more than it saves. A barrier is only worth having if its subset is
usually small.

## PC and control-flow target typing

This is the part that failed catastrophically, so it is specified before the
optimisation it enables.

The promotion layer must not represent these as interchangeable integers:

```
GuestCodeAddress    an address believed to hold guest instructions
GuestDataAddress    an address in the guest address space, not code
HostPointer         a host address
BlockTarget         a target within the current region
FunctionTarget      the entry of a generated function
ReturnTarget        a value produced by a link operation
```

They are distinct types in the backend, with no implicit conversion. Producing a
`BlockTarget` or `FunctionTarget` from a `GuestCodeAddress` goes through one
function, and that function is the single place where the check lives:

- every emitted control-flow target resolves to an address the region plan
  classified as code;
- indirect target resolution that finds a non-code address side-exits to the
  chassis rather than branching;
- the guest PC is materialised at every side exit, every exception, and every
  helper declared `may_change_pc`;
- a value that has been constant-folded cannot become a control-flow target
  without passing the same check.

The last point is the specific failure. A constant-folded value that was never a
code address became the PC. Under this model the fold is still allowed — it just
cannot cross into `BlockTarget` without the check, and the check fails closed by
side-exiting.

Two assertions, on in debug builds and in every differential run: no
`BlockTarget` outside the region's own blocks, and no `FunctionTarget` that is
not in the generated function table.

## Savestate, debugger, and fallback

All three read `CPUState` directly and know nothing about the cache. Each is a
barrier with the subset defined by the `*_visible` flags. The savestate boundary
is the sharpest constraint: it can occur between any two guest instructions, so
either every such point is a barrier — which forbids caching entirely — or the
runtime requests a settle and the module complies at the next safe point. The
second is what the chassis already does for yields, and the cache follows the
same mechanism.

## Differential validation

Reaching a title screen is not evidence. The harness runs generated blocks
against the reference path with randomised state:

1. seed `CPUState` with deterministic pseudorandom values in every described
   entry, including the ones the block does not appear to touch;
2. seed guest memory the same way;
3. run the reference path;
4. run the cached path from the identical starting state;
5. compare every described entry, plus guest memory, plus the sequence of
   external calls;
6. sweep seeds; keep every failing seed as a regression case.

Comparing "every described entry" is the point of deriving both sides from one
description: a slot that exists but that nobody remembered is compared anyway.

Block shapes that must appear: arithmetic with and without CR updates, `XER`
carry chains, direct and conditional branches, `LR`/`CTR` use, loads and stores,
scalar float, paired single including the lane cases that caused `v7`, helper
calls of each declared effect class, exceptions, MMIO side exits, gather-pipe
writes, and interpreter fallback.

The existing `gen_differential` harness already does the two-backend comparison
with a fixed seed and a sweepable `DOLRECOMP_DIFF_SEED`. It does not currently
emit `lwarx`/`stwcx.`, and it only touches a scratch offset deep inside MEM1 —
both gaps to close before it can validate a cache.

## Staged implementation

Each stage is independently testable, independently benchmarkable, and
independently revertable. A stage that does not pay for itself is dropped rather
than carried in the hope that a later stage redeems it.

1. **The description, with nothing consuming it.** Generate the existing
   `CPUState` layout and the existing `state_[]` bindings from it and confirm the
   output is byte-identical to what the emitter produces today. No behaviour
   change, and no benchmark — this stage is correct precisely when nothing moves.
2. **Per-helper effect declarations, still conservative.** Replace the mask with
   declarations that all say `needs_full`. Again byte-identical. This is where
   the build error for an undeclared helper arrives.
3. **Narrow the declarations** to what each helper actually touches, with the
   barrier subsets following. Measurable on its own, because the fallback path
   stops flushing state that no helper reads.
4. **GPR caching within a basic block.** The smallest scope where the cache can
   be wrong, and the easiest to diff.
5. **GPR caching across directly linked blocks** in one region, with entry and
   exit contracts.
6. **CR and XER subfield caching.** Expected to be the largest single win.
7. **`LR` and `CTR`**, which brings return targets and therefore the typing rules
   into play.
8. **Scalar FPR**, then **paired-single lanes**.
9. **Helper-aware caching across calls**, using stage 3's declarations.
10. **Indirect control flow**, then **region-level caching**.

Stages 1 and 2 produce no speedup by construction. They are the stages that were
skipped four times.

## What this does not claim

The gap attributed to `CPUState` traffic is not all recoverable by caching. Some
of it is barrier traffic that a cache still has to pay, some is the fallback
boundary, and the AArch64 register file — 31 GPRs against x86-64's ~14 — makes
the ceiling higher here than the earlier x86-64 experiments suggested, not
lower. No estimate of the recoverable fraction is offered, because the only
honest source of one is stage 4 measured against stage 3.
