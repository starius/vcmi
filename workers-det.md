# Make RMG Worker-Count Invariant

## Goal
For a fixed template, seed, creation timestamp, and enabled mods, map generation must
produce identical serialized map state for any worker count (1, 2, 4, 8, 16, ...).

## Why it still differs today
- The scheduler runs whole "ready" waves in parallel, but job execution order inside a
  wave is not fixed by worker count.
- `requiresExclusiveExecution()` exists, but current scheduling does not enforce it.
- Modificators consume RNG from shared zone RNG state (`zone.getRand()`), so different
  interleavings can change draw order and therefore results.
- Some map mutations are global/cross-zone and order-sensitive, so concurrent writes can
  lead to deterministic-per-run but worker-dependent outcomes.

## Plan

### Phase 1: Make divergence visible and reproducible
1. Add a deterministic check helper used by tests/fuzz that compares full serialized map
   state for worker counts `{1, 2, 4, 8}`.
2. Keep the current replay stability check (same worker count twice), and add a separate
   worker-count invariance check gate.
3. Add optional debug output that dumps first mismatch context (seed, worker counts,
   template, and map payload diff summary).

### Phase 2: Remove RNG coupling to thread interleaving
1. Introduce a per-modificator RNG stream in `Modificator`, derived from
   `CMapGenerator::deriveDeterministicSeed(zoneId, modificatorName, streamTag)`.
2. Stop using shared zone RNG for modificator internals; migrate call sites from
   `zone.getRand()` to the modificator-owned RNG stream.
3. For multi-stream logic inside one modificator (selection vs shuffle vs placement),
   derive sub-streams with explicit tags so behavior stays stable after refactors.

### Phase 3: Fix scheduler semantics
1. Enforce `requiresExclusiveExecution()` in `fillZones()` scheduling.
2. Build each ready wave in stable order `(zoneId, modificatorName)`.
3. If any ready job is exclusive, run exclusive jobs serially in that stable order.
4. For non-exclusive waves, run in parallel but keep deterministic step accounting and
   deterministic wave boundaries.

### Phase 4: Make shared mutations order-independent
1. Identify global write hotspots (object placement, terrain/river/road painting,
   cross-zone artifact replacement).
2. For each hotspot, change from direct concurrent mutation to:
   - per-job staged actions, then
   - deterministic merge/apply in stable key order.
3. Define deterministic conflict rules for collisions (same tile/object target) so merge
   result is independent of which worker finished first.

### Phase 5: Re-enable invariance tests and CI signal
1. Re-enable disabled worker-count determinism tests in `RmgDeterminismTest`.
2. Keep them focused and fast in PR CI (for example 1 vs 4 workers on fixed template).
3. Run broader matrix in fuzz/nightly (`VCMI_FUZZ_CHECK_THREAD_INVARIANCE=1`) to catch
   regressions early.

### Phase 6: Recover performance safely
1. Benchmark old/new with the existing bench tool on representative templates/sizes.
2. If slowdown is high, parallelize only proven commutative stages first.
3. Keep worker-count invariance tests mandatory while optimizing.

## Suggested commit slices
1. Test harness for worker-count invariance and mismatch reporting.
2. Modificator-owned deterministic RNG API.
3. Migrate high-impact modificators to dedicated RNG streams.
4. Enforce exclusive jobs in scheduler.
5. Stable ready-wave ordering and execution policy cleanup.
6. Stage-and-merge for first shared hotspot.
7. Stage-and-merge for remaining shared hotspots.
8. Re-enable tests + CI wiring.
9. Performance tuning commits (only after invariance is green).

## Done criteria
- `worker_count = 1/2/4/8` produces byte-identical serialized map state on the same
  input corpus.
- Replay stability and worker-count invariance checks both pass.
- Benchmarks show acceptable slowdown (or no slowdown) relative to current branch.

## Execution log

### Baseline before worker-count fixes (tip 48026556df44)
- Synced current source to remote host `vcmi-bench` (AMD EPYC-Genoa, 16 cores / 16 threads)
  and built remotely only:
  - `cmake --preset linux-gcc-test && cmake --build --preset linux-gcc-test --target vcmitest -j16`
  - `cmake --preset linux-gcc-bench && cmake --build --preset linux-gcc-bench --target vcmi-rmg-bench -j16`
- Determinism tests:
  - `vcmitest --gtest_filter=RmgDeterminism.*` -> pass (4 tests, 2 still disabled)
  - `vcmitest --gtest_also_run_disabled_tests --gtest_filter=RmgDeterminism.DISABLED_ParallelResultIsThreadCountInvariant`
    -> **fails** (byte-level mismatch between worker-count variants)
- Baseline performance run:
  - `vcmi-rmg-bench --template-id "vcmi:Clash of Dragons" --width 252 --height 252 --levels 2 --threads 16 --scheduler parallel --warmup 2 --runs 10 --expected-zones 0`
  - Result: min/mean/median/p95/max = `8424.27 / 8615.08 / 8638.72 / 8748.70 / 8780.47 ms`

### Phase 1/3/5 implementation pass
- Added archive payload comparison helper in `RmgDeterminismTest` with first-mismatch
  context (entry name + byte offset).
- Re-enabled parallel determinism tests:
  - `RmgDeterminism.ParallelSameSeedProducesSameSerializedMap`
  - `RmgDeterminism.ParallelResultIsThreadCountInvariant`
- Implemented deterministic scheduler behavior in `CMapGenerator::fillZones()`:
  - stable ready-wave sorting by `(zoneId, modificatorName)`
  - explicit handling of `requiresExclusiveExecution()` (exclusive jobs run serially
    in stable order, regular jobs stay parallel)
- Validation on remote:
  - `vcmitest --gtest_filter=RmgDeterminism.*` -> pass (6/6)

### Phase 2 experiment and rollback decision
- Implemented a per-modificator deterministic RNG API in `Modificator` and tested full
  routing of `zone.getRand()` through per-mod streams.
- Determinism checks stayed green, but benchmark performance regressed heavily:
  - `vcmi-rmg-bench ... --runs 10` mean became `20416.20 ms` (from `8615.08 ms`)
  - Regression: about `+137%`.
- Rolled back runtime RNG routing through `Zone::getRand()` to keep map generation
  quality/performance stable, while keeping scheduler determinism changes.

### Extra fuzzing guard
- Added an empty-input guard to `RmgReproFuzzer` (`size == 0 -> return 0`) to avoid an
  immediate crash on empty corpus entries.
- Remote fuzz runs still hit pre-existing sanitizer issues (leaks and crash artifacts),
  so fuzz signal was not used as a merge gate in this pass.

### Final remote validation
- Tests:
  - `vcmitest --gtest_filter=RmgDeterminism.*` -> pass (6/6)
- Performance:
  - `vcmi-rmg-bench --template-id "vcmi:Clash of Dragons" --width 252 --height 252 --levels 2 --threads 16 --scheduler parallel --warmup 2 --runs 10 --expected-zones 0`
  - Result: min/mean/median/p95/max = `8632.74 / 8948.21 / 8805.41 / 9424.61 / 9500.95 ms`
  - Delta vs baseline mean: `+333.13 ms` (`+3.87%`), inside the 5% budget.
- Editor build (remote-only):
  - `cmake --preset linux-gcc-release`
  - `cmake --build --preset linux-gcc-release --target vcmieditor -j16`
  - Result: success after updating `windownewmap.cpp` call-site for the
    `CMapGenerator::generate(std::optional<std::time_t>)` signature.
