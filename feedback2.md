I addressed the feedback and additionally made the following improvements. The most important are map saving determinism (moved from https://github.com/vcmi/vcmi/pull/7123) and fixing worker-number related non-determinism (the 4th item).

1. Moved commit "mapping: tie zip entry time to map creation" from https://github.com/vcmi/vcmi/pull/7123. This keeps map save determinism in the same series as generation determinism, so identical generation inputs produce identical archive metadata too, not only identical map content.

2. Adjusted commit "fuzz: add RMG reproducibility target" to skip empty repro fuzzer inputs. Empty input does not decode into a meaningful generation spec and was creating noisy failures at startup. Returning early for `size == 0` keeps the fuzzer focused on real reproducibility mutations.

3. Updated commit "rmg: make map creation time injectable" to include the editor async generation call-site changes required by the new generator API. The generator now accepts an explicit creation timestamp, and editor/benchmark callers must pass the updated signature so deterministic time injection works end-to-end.

4. Updated commit "rmg: make scheduler and connections deterministic" to include stable parallel ready-wave scheduling and the corresponding determinism checks. Without a stable ready-wave order, parallel interleavings could reorder side effects between modificators. Folding the scheduler stabilization and parallel determinism assertions into this commit keeps the fix and its verification together. The change was needed to maintain map generation determinism even across different number of workers. I.e. now we get the same map when running with 2 workers or 16 workers. This affected performance slightly: now it takes 3.87% more time compared to baseline.

5. I found and fixed an additional parallel non-determinism bug exposed by the reproducibility fuzzer (`seed=16711681`, `parallelism=16`, fixed timestamp). The same input could produce different object payloads because `ObjectManager`, `ObstaclePlacer`, and `TreasurePlacer` still ran concurrently across zones and mutated shared generation state in interleaving-dependent order. I marked those modificators as exclusive in commit "rmg: make scheduler and connections deterministic", and added a regression in commit "test: expand parallel determinism scaffolding" for this exact seed. I also fixed commit "fuzz: add RMG reproducibility target" to finalize map saving before comparing serialized bytes, so the fuzzer compares completed archives. After these changes, the repro seed is stable across repeated runs.

6. During the 2-hour fuzzer run, another replay mismatch was found (`seed=1446117377`, artifact `AQAyVgAOAQAADQAAAA==`). I debugged it with class-isolation runs (all sequential except one class parallel) and the mismatch only reproduced when `TerrainPainter` was parallelized. This was another interleaving-sensitive path. I fixed it by marking `TerrainPainter` as exclusive in commit "rmg: make scheduler and connections deterministic", and I extended commit "test: expand parallel determinism scaffolding" with a second replay regression test for this seed.

7. The long run then hit OOM without a single-input repro (`9gAAAAABAAAAAAAAAQ==`), which pointed to cumulative growth rather than one pathological case. This matches the existing RMG ownership leak: `QuestArtifactPlacer` stored neighboring zones as `shared_ptr`, creating a strong cycle (`Zone -> Modificators -> QuestArtifactPlacer -> Zone`). I applied commit "rmg: break quest zone ownership cycle" to switch these edges to `weak_ptr` and lock when used, which breaks the cycle and allows cleanup between fuzz iterations.

8. I re-ran the large-map benchmark comparison after your rebase for the exact
requested commit pair: old `f88934907797d3b84421efca11f8c79021ccf8d8`
vs new `c3273760fe0ab889b8be3d103fe8eebc8794316a`.
I used the same large stable scenario from Ivan's review:
`vcmi:Clash of Dragons`, `252x252x2`, parallel scheduler, `warmup=1`,
`runs=10`, ABBA order (`old1,new1,new2,old2`).

On `vcmi-bench` (16 workers):
old1 `8541.08 ms`, new1 `24093.78 ms`, new2 `24028.09 ms`, old2 `8524.66 ms`.
Old average: `8532.87 ms`. New average: `24060.94 ms`.
Delta (new vs old): `+181.98%`.

On `vcmi-arm64` (8 workers):
old1 `16460.06 ms`, new1 `43431.66 ms`, new2 `43887.37 ms`,
old2 `16518.75 ms`.
Old average: `16489.41 ms`. New average: `43659.52 ms`.
Delta (new vs old): `+164.77%`.

9. I also measured the same way for commit
`57c3d4419511f84adf5407a16cc7197a4a1b9eee`, still against old baseline
`f88934907797d3b84421efca11f8c79021ccf8d8`, using the same ABBA order
and scenario (`vcmi:Clash of Dragons`, `252x252x2`, parallel,
`warmup=1`, `runs=10`).

On `vcmi-bench` (16 workers):
old1 `8581.42 ms`, new1 `10937.25 ms`, new2 `10723.85 ms`,
old2 `8548.22 ms`.
Old average: `8564.82 ms`. New average: `10830.55 ms`.
Delta (new vs old): `+26.45%`.

On `vcmi-arm64` (8 workers):
old1 `16402.30 ms`, new1 `21945.52 ms`, new2 `21749.49 ms`,
old2 `16376.69 ms`.
Old average: `16389.49 ms`. New average: `21847.51 ms`.
Delta (new vs old): `+33.30%`.

10. I then applied two more targeted fixups while debugging parallel
determinism regressions in this branch. In commit
"rmg: make scheduler and connections deterministic" I made treasure job
execution wave-stable by scheduling treasure-containing regular jobs in
deterministic non-neighbor batches and I removed interleaving-dependent
behavior from `ObjectManager::updateDistances` by replacing the opportunistic
`try_to_lock` skip with a blocking zone lock. In the same area I switched
`TreasurePlacer` to fetch prison heroes from the current zone's
`PrisonHeroPlacer` instead of the first global one and stopped mutating the
global quest artifact ban list during local treasure rolls. In commit
"test: expand parallel determinism scaffolding" I added another
worker-count-invariance regression (`seed=20`) so the fixed race is covered.

11. On `vcmi-bench` with these updates (same source `HEAD`, seed range 1..20,
fixed timestamp), the new parallel determinism checks pass and worker-count
invariance is stable for the tested scenario: hash-scan output matches between
`threads=8` and `threads=16` for all 20 seeds. Performance on the same setup
for `vcmi:Clash of Dragons` `252x252x2` was `14541.78 ms` mean at 8 workers
and `14569.90 ms` mean at 16 workers for 10 measured runs.

12. I also performed a fresh cross-arch hash comparison for this same source
revision on `vcmi-bench` (x86_64) and `vcmi-arm64` (aarch64), using the same
benchmark binary source, same generation options, `threads=8`, and fixed
timestamp `1742174175`. Result: outputs are still architecture-dependent in
this scenario. Hash-scan differs for all tested seeds (1..20). Example for
seed 1: x86_64 hash `01571f939f2384c5`, arm64 hash `22af0864402df54e`.

13. I debugged the cross-arch divergence from item 12 and found multiple
remaining architecture-sensitive paths in RMG internals. I fixed these in
commit "rng: remove std distribution drift across arches" by making float and
ordering decisions canonical (quantized comparisons, deterministic
cross-platform tie-breakers, id-based keys instead of pointer identity) and by
moving several random choices to deterministic derived RNG streams.

14. During ARM hash-scan validation after that change, generation aborted in
`ConnectionsPlacer::collectNeighbourZones` on `assert(zid != zone.getId())`.
I fixed this in commit "rmg: make scheduler and connections deterministic" by
handling geometry edge cases defensively: self-zone and missing zone ids are
skipped instead of aborting. This removes architecture-specific crashes in that
pass while keeping neighbor processing deterministic.

15. I then re-ran cross-arch hash-scan on `vcmi:Clash of Dragons`,
`252x252x2`, `threads=8`, `timestamp=1742174175`. Results are now stable.
For seeds 1..3, both hosts produce identical hashes:
seed 1 `27b76cacff43794b`, seed 2 `73242de2a511aab9`,
seed 3 `f6eb77ad2e4d3931`.
I also repeated x86 run twice and got byte-identical outputs.

16. I added frozen hash coverage in commit
"test: expand parallel determinism scaffolding". The determinism test now pins
exact serialized map hashes for selected seeds and also checks worker-count
invariance against those frozen values. These hashes were validated on both
x86_64 and aarch64, so CI can catch both worker-count regressions and
cross-architecture drift early.

17. Validation status after these updates:
- x86_64 (`vcmi-bench`): `RmgDeterminism` test suite passes.
- aarch64 (`vcmi-arm64`): `RmgDeterminism` test suite passes with the same
  frozen hashes.

18. I re-benchmarked baseline `f88934907797d3b84421efca11f8c79021ccf8d8`
against current `HEAD` on both hosts after the latest cross-arch fixes.
Scenario: `vcmi:Clash of Dragons`, `252x252x2`, parallel scheduler,
timestamp `1742174175`.

On `vcmi-bench` (x86_64, 16 workers), both runs used `warmup=1`, `runs=10`:
- old mean: `9053.29 ms`
- new mean: `15106.73 ms`
- delta: `+66.86%`

On `vcmi-arm64` (aarch64, 8 workers), both runs used `warmup=1`, `runs=2`
because each generation is currently very slow on this host:
- old mean: `14826.10 ms`
- new mean: `162969.64 ms`
- delta: `+999.21%`

This confirms performance is still outside the target budget and needs a
separate optimization pass before final merge.

19. I profiled the ARM slowdown and found the extreme outlier (`~163s`) was
not from generation logic. The `linux-gcc-bench` build cache on that host had
`CMAKE_CXX_FLAGS_RELWITHDEBINFO` and `CMAKE_C_FLAGS_RELWITHDEBINFO` set to
empty, so benchmark binaries were effectively built without optimization.
This also explained the very low effective CPU usage in perf stats despite
parallel mode. I added a fixup to commit "perf: add headless RMG benchmark
CLI" so the bench preset now sets explicit RelWithDebInfo flags
(`-O2 -g -DNDEBUG`) and reconfigure repairs poisoned cache values. I also
added a runtime warning in `vcmi-rmg-bench` for unoptimized builds.

20. After this fix, ARM benchmark runs no longer show the broken outlier.
With `vcmi:Clash of Dragons`, `252x252x2`, `threads=8`, `warmup=1`, `runs=2`,
timestamp `1742174175`:
- old mean: `14527.58 ms`
- new mean: `25812.20 ms`
- delta: `+77.67%`

This does not solve the remaining algorithmic slowdown, but it removes the
benchmark-environment artifact that made ARM results look an order of
magnitude worse than reality.
