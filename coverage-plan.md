# RMG Determinism Coverage Expansion Plan

## Goal
Expand determinism coverage so we can catch remaining worker-count and
configuration-dependent mismatches across a wider map space, including:

- 1-level and 2-level maps
- multiple map sizes
- multiple gameplay settings (water, monsters, player setup)
- wider worker-count combinations (1..16)
- more seeds per scenario

## Guiding rules

- Keep tests deterministic and reproducible: no true randomness in test logic.
- Use fixed timestamps in all hash-based tests.
- Split fast PR coverage and broader nightly coverage.
- On mismatch, print enough context to reproduce quickly.

## Coverage structure

### 1) Fast PR test layer (unit/integration, mandatory)
Add a new parameterized determinism matrix test in
`test/map/RmgDeterminismTest.cpp` that:

- uses a curated set of scenario buckets (sizes/levels/settings),
- for each case runs:
  - baseline generation on `workers=1`,
  - comparison generation on a deterministic worker count in `1..16`,
  - payload equality check (`archivePayloadEquals`),
- for selected anchor cases also checks frozen hashes.

Deterministic worker choice per case:

- `worker = 1 + (stableHash(caseId, seed) % 16)`,
- if worker resolves to `1`, force `16` to keep a real cross-worker check.

This gives wider worker coverage without flaky behavior.

### 2) Extended PR-adjacent layer (medium runtime, optional in PR / required pre-merge)
Add a seed sweep test group (can be `DISABLED_` by default) that:

- iterates a fixed seed range per scenario bucket (for example 32-64 seeds),
- compares `workers=1` vs deterministic worker in `1..16`,
- stops on first mismatch with full repro context.

This is the "shoot in map space" layer with bounded runtime.

### 3) Nightly / long-run layer
Use benchmark hash-scan mode and fuzzer strict thread-invariance mode:

- `vcmi-rmg-bench --hash-scan-*` across larger seed ranges and multiple scenario
  presets,
- `RmgReproFuzzer` with `VCMI_FUZZ_CHECK_THREAD_INVARIANCE=1` for long runs.

Nightly jobs should include both x86_64 and ARM64 where available.

## Scenario bucket design

Use stratified buckets instead of only one template/small map.

### A) Core buckets (fast, for PR)

1. Small 1-level baseline
   - size: small
   - levels: 1
   - settings: current default-like values

2. Medium 1-level with no water
   - size: medium
   - levels: 1
   - water: none

3. Medium 2-level
   - size: medium
   - levels: 2
   - water: random

4. Large 2-level heavy case
   - size: large
   - levels: 2
   - template known to stress object/treasure placement

### B) Extended buckets (longer, sweep/nightly)

5. Large 2-level islands + strong monsters
6. Large 2-level weak monsters + no water
7. Alternate player mix (more standard players, comp-only > 0)
8. Template variation bucket (at least one additional deterministic template id)

For each bucket define a fixed timestamp and fixed seed list.

## Frozen hash policy

Keep frozen hashes small and intentional:

- freeze a small set of anchor cases only (for example 8-12),
- include both 1-level and 2-level cases,
- include at least one larger/heavier case,
- freeze hash for `workers=1` and verify equality with deterministic
  `workers in 1..16`.

Do not freeze huge seed matrices; use equality checks for broad sweeps.

## Fuzzer spec expansion plan

Current `RmgGenerationSpec` only covers seed/thread/timestamp. Expand it to include:

- width, height, levels
- player counts (standard/human/comp-only)
- water setting
- monster setting
- template selector/id

Then update:

- binary encoding/decoding in `fuzz/RmgFuzzSpec.cpp`,
- text seed format and corpus tooling in `RmgReproFuzzer.cpp`,
- normalization constraints to keep only valid combinations.

In strict invariance mode:

- compare baseline (`workers=1`) vs alternate deterministic worker (`1..16`)
  derived from spec bytes,
- optionally also compare one additional worker for better spread in long runs.

## Failure diagnostics

When mismatch happens, print:

- full scenario key (size/levels/settings/template/player setup),
- seed, timestamp, worker pair,
- first differing archive entry and byte offset (already available),
- optional hash values for baseline/candidate.

For long/nightly runs, optionally dump mismatching serialized maps to a temp dir.

## CI split

### PR required

- Core bucket matrix
- Frozen anchor hash checks
- Existing `RmgDeterminism.*` tests

### Nightly required

- Extended bucket seed sweeps
- Fuzzer strict invariance long run
- Hash-scan benchmark matrix
- Cross-arch hash comparison job (x86_64 vs ARM64)

## Implementation slices

1. Refactor test helper to accept full scenario config (not fixed small/1-level).
2. Add parameterized core bucket matrix with deterministic worker selection.
3. Add anchor frozen-hash set for mixed 1-level/2-level buckets.
4. Add optional extended seed sweep tests.
5. Expand fuzzer spec to include map configuration fields.
6. Add nightly scripts/jobs for hash-scan + cross-arch diff.

## Done criteria

- PR tests cover both 1-level and 2-level across multiple size/settings buckets.
- Worker-count checks in PR exercise worker counts across the `1..16` range
  (deterministically distributed).
- Nightly catches mismatches with reproducible scenario/seed context.
- No test flakiness introduced by runtime randomness.
