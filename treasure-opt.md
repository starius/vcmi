# Treasure/ObjectManager optimization plan

## What is slow now

From the latest per-worker timeline capture on
`vcmi:Clash of Dragons` `252x252x2` (`threads=16`, `seed=1`, fixed timestamp):

- Wall time increased from about `13.7s` (old) to `15.8s` (new).
- The critical worker chains are dominated by long `TreasurePlacer` tasks,
  often preceded by expensive `ObjectManager` work.
- Biggest single chunks are in treasure placement long-tail zones
  (multiple `TR` items in `3-9s` range on one worker).

So the most promising optimization target is the `ObjectManager ->
TreasurePlacer -> ObstaclePlacer` segment, with first focus on
`TreasurePlacer` and the `ObjectManager` calls it triggers.

## Do workers contend on global state now?

There is no single global mutex that serializes all workers in this phase.
Current synchronization is mostly:

- `Modificator::mx`: per-modificator lock (guards one job instance).
- `Zone::areaMutex`: per-zone lock used heavily by `TreasurePlacer` and
  `ObjectManager` while mutating zone geometry/placement state.
- Selective multi-zone locking in connections logic (`ConnectionsPlacer`),
  not the dominant hotspot in the new timing profile.

Important conclusion: slowdown is mainly not a "global lock bottleneck".
It is mostly compute and load-balance pressure in treasure/object placement:

- repeated expensive placement scans,
- uneven zone difficulty causing worker tail,
- neighbor-isolation batching limiting how many heavy treasure zones can run
  at once.

## Root-cause hypothesis for current slowdown

1. `TreasurePlacer::createTreasures` does many expensive placement attempts in
   hard zones, with repeated checks and path/guard constraints.
2. `ObjectManager` placement helpers repeatedly scan large candidate spaces and
   update distance maps many times.
3. Work distribution is deterministic but not load-aware enough for long-tail
   treasure zones, so a few workers become stragglers.

## Optimization roadmap

### Phase 1: make cost visible and stable

Goal: optimize with evidence, not guesses.

- Add lightweight per-zone stats (behind a compile flag or bench-only path):
  treasure attempts, successful placements, rejected placements by reason,
  `ObjectManager` scan counts, and time spent in:
  - search area filtering,
  - `findPlaceForObject` / `placeAndConnectObject`,
  - distance updates after placement.
- Keep this deterministic and side-effect free.

Why this helps:
- Confirms whether dominant cost is scan volume, guard/path checks, distance
  updates, or scheduler tail.

### Phase 2: reduce repeated candidate-space work in TreasurePlacer

Goal: remove obvious repeated O(area) work.

- In each zone/tier loop, build deterministic filtered candidate sets once,
  then reuse them across attempts in that tier instead of re-deriving large
  search areas from scratch each time.
- Keep candidate order canonical (`int3` strict order) so tie-breaking remains
  stable.
- Avoid rebuilding identical temporary vectors when static constraints do not
  change (for example road-adjacent exclusions for the same object class).

Why this helps:
- Reduces repeated scans and allocations in the hottest loops.

Determinism/correctness:
- Same acceptance predicates, same ordering, same tie-breakers.
- No change in game rules; only cached/reused intermediates.

### Phase 3: optimize ObjectManager placement scanning path

Goal: lower per-placement cost without changing decision semantics.

- Introduce a fast deterministic path for weighted candidate evaluation:
  - iterate prefiltered candidate tiles (already canonical order),
  - keep existing quantized weight comparison and deterministic tie-breaks,
  - avoid unnecessary map/tile lookups when early rejection is certain.
- Reuse immutable computed structures across nearby placement attempts where
  safe (for the same zone state snapshot).

Why this helps:
- `ObjectManager` is the next largest contributor and directly amplifies
  `TreasurePlacer` latency.

Determinism/correctness:
- Preserve exact comparator semantics and deterministic fallback order.
- Keep existing quantization and stable tie-breaking rules.

### Phase 4: deterministic load-balancing improvements for treasure waves

Goal: reduce long-tail worker idle time without changing legal parallelism.

- Keep neighbor isolation rule intact (no adjacent treasure zones together).
- Replace naive greedy batch fill with deterministic cost-aware ordering:
  - score zones by stable estimate (zone area, expected treasure count,
    historical per-zone cost from phase 1 data),
  - choose next batch in deterministic descending score, tie-break by zone id.
- Keep per-wave barrier model unchanged.

Why this helps:
- Better packing of heavy zones reduces final straggler tail.

Determinism/correctness:
- Ordering policy is deterministic and key-based.
- No new races: still wave/barrier based, same isolation constraints.

### Phase 5: optional structural split for very heavy treasure zones

Goal: improve parallelism ceiling if phases 2-4 are insufficient.

- Split one zone's treasure work into deterministic sub-units keyed by
  `(zoneId, tierIndex, chunkIndex)` with independent derived RNG streams.
- Process sub-units in parallel and merge effects in deterministic order.

Why this helps:
- Prevents a single huge zone from monopolizing one worker.

Determinism/correctness safeguards:
- Never share mutable RNG streams between chunks.
- Deterministic merge order before applying map mutations.
- Keep map mutation stage serialized per zone if needed.

## Non-determinism guardrails (must hold for every phase)

- No shared mutable RNG streams across concurrently executed units.
- All new random draws use derived seeds from stable keys.
- No iteration order based on hash-table/pointer identity.
- Keep quantized float comparisons and explicit deterministic tie-breakers.
- Keep worker-count-invariant scheduling decisions key-based, never timing-based.

## Validation plan per optimization slice

For each slice (small commit-sized change):

1. Build and run determinism tests (`RmgDeterminism` suite).
2. Run cross-worker hash-scan (`threads=1/8/16`) and compare exact hashes.
3. Run cross-arch hash-scan (`x86_64` vs `aarch64`) for same seeds/options.
4. Run benchmark scenario and compare against current baseline.
5. Run reproducibility fuzzer smoke and then long run (as previously used).

Only keep slices that improve runtime and keep all deterministic checks green.

## Expected outcome

- Reduce treasure/object placement tail latency first.
- Improve overall throughput while preserving correctness and deterministic
  behavior across worker counts and architectures.
- Avoid reverting to non-deterministic scheduling or RNG sharing.
