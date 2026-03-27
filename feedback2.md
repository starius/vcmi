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
