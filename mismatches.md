# Plan: Debug and Fix Remaining Cross-Arch Mismatches

## Goal

Eliminate the remaining cross-architecture map hash mismatches for commit
`e2d1674b38ed7b7013f3cc4a9185c3d25e677123` without adding more performance
regression.

Current status from replay:

- Rechecked historical nightly mismatch seeds: `62`
- Still failing on new: `12`
- All 12 are cross-arch (`x86 == x86`, `arm == arm`, but `x86 != arm`)

Failing pairs:

- `2LM2a-large-l1`: seeds `93693`, `97688`, `99788`
- `2LM2a-large-l2`: seeds `92432`, `98050`, `100443`
- `2SM2a-small-l1`: seeds `94432`, `98409`
- `clash-g+u`: seeds `92158`, `93112`, `94719`, `99514`

## Phase 1: Stabilize Repro Harness

1. Keep a single deterministic replay script for the failing seed list.
2. For each seed/scenario, collect:
   - `x86-t1`
   - `x86-t16`
   - `arm-t1`
   - `arm-t8`
3. Assert expected shape before debugging:
   - worker-invariance holds per arch
   - mismatch is strictly cross-arch
4. Save all outputs in one place for repeatable before/after comparison.

Acceptance for phase:

- Repro is fast and deterministic enough to rerun many times during debugging.

## Phase 2: Find First Divergent Generation Stage

1. Add temporary trace points (behind env flags, not permanent yet) at
   scheduler stage boundaries.
2. For each traced step, print stable hashes of map state and zone-local state.
3. Compare x86 vs arm traces for one representative failing seed per scenario
   class.
4. Locate the first step where hashes diverge.

Likely we will cluster failures into one or two divergence roots. If not,
split into multiple independent roots.

Acceptance for phase:

- We can name the first diverging modificator/job for each failing scenario.

## Phase 3: Root-Cause Audit in Diverging Job(s)

For each first-diverging job, audit and instrument:

1. Candidate-set construction and ordering:
   - any `unordered_*` iteration
   - pointer-address ordering
   - implicit order from map/object insertion
2. Numeric decision paths:
   - float scoring/comparison
   - trigonometric/geometry helper usage
   - equality/tie checks that may drift by FPU/arch
3. RNG consumption:
   - make sure random draws depend on canonicalized, sorted candidate vectors
   - verify no order-dependent RNG stream consumption from unstable iteration

Acceptance for phase:

- We have a concrete explanation of why x86 and arm choose different branch or
  consume RNG differently.

## Phase 4: Apply Deterministic Fixes

Apply minimal fixes per root cause:

1. Canonical ordering fixes:
   - sort candidate vectors with explicit comparator before any choice/min/max
   - add deterministic tie-breakers where scores can tie
2. Numeric stability fixes:
   - quantize float scores before comparisons if used for branch decisions
   - avoid architecture-sensitive float tie behavior in ranking logic
3. RNG stream hygiene:
   - ensure each choice draws after canonical ordering
   - avoid hidden extra draws in architecture-dependent code paths

Implementation rule:

- One logical cause => one focused fixup commit to the relevant original commit.

## Phase 5: Validation Matrix

After each fix:

1. Re-run failing 12 seed list across four configs (`x86-t1`, `x86-t16`,
   `arm-t1`, `arm-t8`).
2. Re-run full 62 historical mismatch seed list.
3. Re-run deterministic tests (`RmgDeterminism` suite).
4. Re-run short nightly-style scenario sweep to catch new drift.

Completion criteria:

- `0` failures in the 12-seed list
- `0` failures in the 62-seed replay list
- tests green

## Phase 6: Performance Guard

After determinism is restored:

1. Run benchmark on both servers:
   - same Clash G+U settings used previously
   - compare pre-fix-new vs post-fix-new
2. Ensure fixes do not add extra slowdown.
3. If slowdown grows, profile and optimize only hot deterministic code paths.

Target:

- no extra meaningful regression beyond current new baseline.

## Commit and Reporting Plan

1. Keep fixups mapped to existing determinism commits.
2. Keep temporary tracing out of final history.
3. Update report docs with:
   - root causes
   - before/after seed replay counts
   - perf before/after on both servers

