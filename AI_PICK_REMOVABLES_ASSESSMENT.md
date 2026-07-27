# NK2 removable/rewardable-object branch assessment

## Bottom line

The branch implements the direction discussed on 20 July: keep `MAIN` as a
long-term hero-development role, independently recognize heroes carrying a
meaningful army, let scouts clean up routine rewards, and keep MAIN heroes
focused on guarded, critical, and developmental targets.

The sent implementation had several correctness holes around composed goals,
path/battle semantics, and deciding whether another hero can really perform the
work. Those confirmed defects have each been corrected in a separate
`fixup!` commit. One additional integration correction restores the
profile-based meaning of `MAIN`, as explicitly agreed in the chat; that
army-based role scoring came from the rebased `develop`, not from Manfred's
commits.

The empirical result is **promising, but not independently confirmed**. Batch
1's balanced outcome was +0.119 pooled SD in favor of the policy (95%
map-bootstrap CI [+0.024, +0.207], exact two-sided p=0.0547). A fresh
eight-map replication kept the direction positive at +0.051 SD, but its
interval crossed zero ([-0.041, +0.147], p=0.3594). The secondary combined
estimate was +0.085 SD ([+0.017, +0.153], p=0.0313; 12/16 maps positive).
Because the replication was commissioned after inspecting batch 1, the
combined p-value is not a clean confirmatory test. The replication itself does
not establish the requested statistical confidence.

The combined point estimates nevertheless have a coherent gameplay shape:
total army strength was +18.8%, all-hero army strength +23.3%, non-gold
resource value +7.7%, MAIN level +0.28, and towns +0.03. The intervals for
individual outcomes remain wide, and first-elimination results were neutral
(10 enabled versus 9 disabled first losses). Gold is excluded from the primary
score, so the favorable composite is not a gold-only conclusion.

Recommendation: do not use Manfred's sent commits without the fixups. After
autosquashing them, the policy is reasonable to continue testing, but this
experiment is not enough to claim a proven improvement or to justify removing
the gate. Repeat on full SoD data and more independently generated maps if a
merge/default-on decision requires confirmatory performance evidence. The
experiment-control commit is intentionally separate and can be dropped from a
production PR once it is no longer needed.

## Scope and provenance

- Sent branch: `MichalZr6/vcmi:ai-pick-removables`.
- Rebase command: `git pull --rebase upstream develop`.
- Exact fetched `develop` base: `24579d36addd8e169e794f9fe5060f29c8c2e1cf`.
- The rebase conflict was at the top of
  `AI/Nullkiller2/Engine/PriorityEvaluator.cpp`: recent conquest helpers and
  the branch's new resource helper occupied the same area. Both independent
  changes were retained.
- A temporary garrison-related commit was skipped because its change was
  already present upstream.
- All compilation, tests, map generation, and game execution were performed
  on the pinned GCP Spot VM. Nothing was built, tested, or run locally.

The nine rebased commits originally sent by Manfred are:

| Commit | Purpose |
|---|---|
| `1facafb67` | Scale resource priority by shortage severity, preserve path actions, and distinguish routes that really require battle. |
| `89c3cfdd3` | Correctly initialize and reconstruct the path-node action field. |
| `4c5d87eaf` | Replan after a multi-hero army exchange changes the relevant state. |
| `32c20e3ad` | Introduce the independent “meaningful army carrier” predicate. |
| `4a3103688` | Implement the MAIN/SCOUT removable and rewardable-object policy (with temporary diagnostics). |
| `80e14c5a7` | Refresh hero roles and paths after battle results. |
| `e812a4bc7` | Apply the carrier predicate in army gathering/upgrading. |
| `f62f8195d` | Refresh hero roles whenever AI state is updated, not only during a full path rebuild. |
| `6c15c1ca6` | Remove an unused include. |

## What the chat established

The relevant discussion is concentrated in the 20 July part of `/tmp/log-20`.
It is internally consistent with the branch:

- Manfred said he was improving “resource piles and weekly rewardable objects.”
  His key need was to distinguish a hero carrying a meaningful army from an
  ordinary scout. He deliberately modeled the threshold after
  `GatherArmyBehavior`: the strongest army carrier, or a hero carrying a
  non-trivial reinforcement relative to the strongest army.
- He explicitly described `MAIN` as the hero worth developing based primarily
  on profile and fighting potential, rather than merely whoever temporarily
  owns the army.
- Mircea confirmed that design: MAIN heroes should fight, explore, and consume
  important development rewards; scouts should clean up routine resources and
  periodically deliver armies/artifacts.
- `starius` agreed that the follow-up should restore profile-based MAIN
  selection and use a separate carrier predicate at tactical decision points.
- The MaxPass/stale hero-exchange investigation in the same log was a separate
  issue. The branch's post-exchange replan helps state invalidation after a
  successful chain exchange, but it is not presented here as a general fix for
  that older retry bug.
- The empty Dwarven Treasury noticed through temporary logs was also adjacent
  investigation, not part of this feature's intended scope.

## Behavioral model

The implementation has five cooperating parts:

1. **Resource pressure.** Missing resources are valued using market value,
   current income, and current stock. A zero stock of wood or ore receives an
   additional early/mid-game pressure boost.
2. **Accurate path intent.** Reconstructed paths carry their original node
   actions. The evaluator separately tracks a battle somewhere on the route and
   a battle caused by visiting the final target.
3. **Stable roles.** MAIN remains profile-driven. Army ownership is represented
   by the separate carrier predicate: always the strongest carrier, or a
   non-strongest army strictly above both 500 strength and 10% of the strongest
   army.
4. **Delegation.** MAIN yields ordinary loose resources/mines only when a valid,
   safe, unlocked, same-turn SCOUT path can actually visit them. A meaningful
   carrier prefers a real, safe army delivery to MAIN and avoids routine weekly
   visits.
5. **Replanning.** Roles are refreshed after relevant state changes, army
   delivery to MAIN is boosted, and a multi-hero exchange causes the current
   plan to be rebuilt.

### State/transition table

| Acting state | Target/path state | Other relevant state | Result |
|---|---|---|---|
| MAIN | Final target starts a battle | — | Strongly favor the target; keep guarded banks and similar fights on MAIN. |
| MAIN | Unguarded target supplies a currently critical resource | No eligible SCOUT pickup | Strongly favor it despite cleanup cost. |
| MAIN | Loose resource or mine, no final-target battle | A safe, unlocked, visit-valid SCOUT can collect it this turn | Reject this MAIN task and delegate it. |
| MAIN | Ordinary non-weekly unguarded reward | No eligible SCOUT pickup | Keep it possible but discount it to one third. |
| MAIN that is also a meaningful carrier | Weekly revisitable, unguarded, non-critical | — | Reject the routine visit. |
| Meaningful carrier SCOUT | Non-battle exploration/gather task | A genuinely safe and useful delivery to MAIN is reachable within one turn | Reject cleanup and deliver the army. |
| Meaningful carrier SCOUT | Weekly revisitable, unguarded, non-critical | No eligible delivery | Reject the routine visit. |
| Meaningful carrier SCOUT | Critical reward or guarded target | No eligible delivery | Evaluate normally; it is not discarded as routine cleanup. |
| Ordinary SCOUT | Reward/removable | — | Existing scout behavior remains in force. |
| Any hero | ESCAPE tier | Delivery is possible | Escape is never suppressed by the delivery policy. |
| Any hero | Route battle, unguarded final target | — | Route is battle-bearing, but final target is not mislabeled as guarded. |
| Carrier → MAIN exchange | Safe useful exchange, at most one turn | — | Triple the exchange army-growth incentive. |
| Multi-hero exchange completes | — | — | Stop executing the stale batch and replan. |
| Battle/state update | Hero profiles or armies changed | Feature enabled | Refresh roles and invalidate/rebuild paths as needed. |

The “critical reward” test applies to any implementation of
`Rewardable::Interface`, not only one concrete weekly-object class.

## Confirmed defects and fixups

Each confirmed issue is isolated in its own `fixup!` commit so the author can
autosquash it into the relevant sent commit.

| Fixup | Confirmed failure mode | Correction |
|---|---|---|
| `14b18257de` | The rebased `develop` scored current army ownership directly into MAIN selection, so a temporary handoff could flip the long-term development role. This contradicted the chat agreement and destabilized skill/reward focus. | Restore profile-only MAIN selection; keep army carrier as an independent predicate; reuse it in UI diagnostics and add threshold tests. |
| `042c3cbfa8` | Carrier-delivery suppression lived in the shared `EXPLORE_AND_GATHER`/`ESCAPE` case and could reject an escape route. | Restrict suppression to `EXPLORE_AND_GATHER`. |
| `c55f20babd` | One `requiresBattle` bit conflated a fight anywhere on the route with a guard/fight at the reward target. That could treat an unguarded weekly object as guarded, or misroute MAIN/SCOUT work. | Preserve explicit final-target battle state and expose separate `targetRequiresBattle()` and whole-route `requiresBattle()` queries, with focused tests. |
| `fd68bf29be` | MAIN yielded a resource when a cached SCOUT node merely said “reachable this turn,” even if the hero/path was locked, blocked, unsafe, invalid for the visit, or depended on an unsuitable chain. | Validate the concrete custom path, ownership/role, turn, locks, blocker, visit eligibility, safety, and chain constraints. |
| `a7107027ed` | A carrier SCOUT that would itself reject cleanup in favor of delivery could still make MAIN yield the same resource, leaving nobody committed to collect it. | Exclude such carrier paths from the MAIN-yield decision. |
| `8190ad9338` | Temporary warning logs built several strings on every hot-path priority evaluation, materially distorting AI throughput and the proposed timing experiment. | Remove the temporary hot-path logging and string construction. |
| `2b93674cf2` | Carrier cleanup was suppressed on a raw “MAIN reachable” path even when `GatherArmyBehavior` would reject the exchange (too little reinforcement, locks, unsafe route, wrong MAIN ordering, etc.). | Extract and share the actual safe/useful delivery eligibility test. |
| `de6bb896b6` | Critical-resource inspection recognized only a concrete weekly rewardable type, missing other rewardable implementations with available resource rewards. | Inspect any `Rewardable::Interface` first-visit reward. |
| `3aa0802d6f` | A composed goal could lose its actual target object because the wrapper had no `objid`; removable policy would then be skipped or evaluated against no target. | Carry the concrete child target through `EvaluationContext`. |
| `1c2f188263` | A composed goal OR-ed target-battle state from prerequisite steps, so a fight on the way could falsely mark the final reward as guarded. | Let the actual final target step define final-target battle state while retaining route-wide battle state separately. |
| `55d81c8875` | A composed goal could evaluate carrier status, rewards, and delegation using the wrapper's/null hero instead of the hero that actually reaches the target. | Carry the acting target hero through `EvaluationContext` and use it consistently. |

`e45f5b8c2` is a separate test-fixture correction: two newly added stack
`AIPath` fixtures did not initialize `targetHero` before a method compared it.
It does not change production behavior.

## Remaining code risks

No additional defect had a sufficiently safe, obvious correction to justify
another fixup, but these limitations remain:

1. **Object-graph endpoint semantics are still approximate.**
   `GraphPaths` synthesizes some nodes with `EPathNodeAction::NORMAL`. When a
   graph route already contains a dangerous transition it also disables a
   target-tile guard check to avoid double-counting. A path containing both a
   route battle and a separately guarded endpoint can therefore under-report
   `targetRequiresBattle`. The in-code TODO is accurate: a complete fix needs
   endpoint visit semantics preserved by the object graph, not another danger
   heuristic.
2. **Delivery eligibility adds evaluator work.** The corrected logic may search
   paths to MAIN during priority evaluation. The hot temporary logs were
   removed, but CPU cost should still be watched on large maps.
3. **Delivery currently wins over pickup for a carrier.** If a meaningful
   carrier has a valid delivery within one turn, all non-battle
   explore/gather tasks are suppressed before critical-resource scoring. This
   appears deliberate—army delivery is treated as urgent—but it is a policy
   choice worth confirming rather than a correctness fact.
4. **Cleared-but-still-visitable banks are outside this patch.** The empty
   Dwarven Treasury discussed in chat needs separate reward/visited-state work.

## Remote build and focused validation

- VM: pinned GCP Spot instance, `n2-standard-8` fallback after the preferred
  `c3-standard-8` shape was unavailable.
- Build: C++20 `RelWithDebInfo`, strict compiler checks enabled; the full
  configured build completed successfully (673 Ninja steps). The only warning
  observed was an unrelated existing Boost array-bounds warning.
- Final focused test filter:
  `Nullkiller2_Analyzers_HeroManager.*:Nullkiller2_Pathfinding_ArmyLoss.*:Nullkiller2_Engine_PriorityEvaluator.*:Nullkiller2_Engine_TaskFailure.*`
- Result: **21/21 tests passed**, including the carrier thresholds and the
  route-battle versus target-battle cases.
- A wider Nullkiller test attempt was not a clean signal with the legally
  available data: full mode lacked expansion sprites, while proper RoE demo
  mode intentionally disables map formats used by unrelated map-based tests.
  This is recorded as an environment/data limitation, not counted as a branch
  pass or failure.
- Legal runtime data was the official Loki Heroes III demo payload, SHA-1
  `74c28240794d0aa2fb52cadcd088ab6dd47478c1`.

## Empirical design

The remote-only harness generated two independent eight-map batches after
loading the official data set. Batch 2 was added with the endpoint and stopping
rules frozen after batch 1's exact primary p-value landed just above 0.05.
Every map had eight Nullkiller players and no teams. Each map was run twice:

- Run E enabled colors `0,2,4,6`; run O enabled `1,3,5,7`.
- Both runs reused the exact same saved `.vmap` artifact and server seed.
- Thus every color was observed once with the policy and once without it.
- Assignment order alternated by map to counterbalance VM/time drift.
- A run ended after 300 seconds or immediately after the first player loss.
- All maps first passed a short remote load/8-player/gate/telemetry smoke test.

### Batch 1 map matrix

| Map | Size | Levels | Water | Monsters | RMG seed | Template | SHA-256 |
|---|---:|---:|---|---|---:|---|---|
| `g01-xl-islands` | 144×144 | 2 | islands | normal | 51001 | 8MM6 | `c6e58e93160441c2063ce04c7fb35d3b450c591fb6ef90c007bd2fb64ce6d6a2` |
| `g02-m-dry-weak` | 72×72 | 1 | none | weak | 51002 | 8MM0e | `e43385b8815f003a7e906197865bdad3e9568868436e54f4b40c3a67eb6b78bc` |
| `g03-m-water-normal` | 72×72 | 1 | normal | normal | 51003 | 8MM0e | `19d6ceded510066d5f86250b64ba0a46438f1ac6141b6fcdd3316a7d800ec65c` |
| `g04-l-dry-strong` | 108×108 | 1 | none | strong | 51004 | 8MM6 | `f036a2f68b3bcda94373e2d84a5d2f03d1d7cba12140ad7c181259cba6b0a6c1` |
| `g05-xl-water-weak` | 144×144 | 1 | normal | weak | 51005 | 8MM6 | `8f5c60b658efa8f4c1d2c6672bc63e9832ab95d2b63c28855b2a8c742fefd491` |
| `g06-hu-dry-normal` | 180×180 | 2 | none | normal | 51006 | 6LM10a | `3710f40f9cb4f03dd386d2fdf5c7db1699677d7ef573c52d9267a353b45c2425` |
| `g07-hu-water-strong` | 180×180 | 2 | normal | strong | 51007 | 6LM10a | `88c826acdc215a510b7eccd07ffdac4c6aca1b94b178b4a801bec91822768556` |
| `g08-gu-islands` | 252×252 | 2 | islands | normal | 51008 | Coldshadow's Fantasy | `85eb0d731600158227c80c932aacad1eff295207dc5ff0938359aaed933b3e83` |

Game seeds were 61001–61008 respectively.

### Independent replication map matrix

| Map | Size | Levels | Water | Monsters | RMG seed | Template | SHA-256 |
|---|---:|---:|---|---|---:|---|---|
| `g09-xl-islands` | 144×144 | 2 | islands | normal | 52001 | 8MM6 | `711669949d0192d6f8fdb699445b63c08f5fd4dea74d9078553e2f2efe1a3033` |
| `g10-m-dry-weak` | 72×72 | 1 | none | weak | 52002 | 8MM0e | `7579d3d97604af84f9dd9e3bbcb9c5910efcb5ea916dd29c432ff77f472c8f5f` |
| `g11-m-water-normal` | 72×72 | 1 | normal | normal | 52003 | 8MM0e | `eaa3d84d7a62abd208617c3a91fea23b43394327e4bf01bd460aa1b48390ad82` |
| `g12-l-dry-strong` | 108×108 | 1 | none | strong | 52004 | 8MM6 | `226b3da0c47cc06c130308e4cd6d6e4b4e8d072886b571f9a8d978c52947d698` |
| `g13-xl-water-weak` | 144×144 | 1 | normal | weak | 52005 | 8MM6 | `569ea8d7b45da3a1cf0310900a2356fea0d767ac0fdbd21bf6561ad7a16dcc94` |
| `g14-hu-dry-normal` | 180×180 | 2 | none | normal | 52006 | 8XM12 | `651e23c66474351c1f69748beb115bdcc5a113293c39bb3a966da173f7d2bd18` |
| `g15-hu-water-strong` | 180×180 | 2 | normal | strong | 52007 | 8XM12 | `55a208fb5cbf912df5a56f8df00e721fa11edd417c1905ae7f9cf6fd0fa0e23c` |
| `g16-gu-islands` | 252×252 | 2 | islands | normal | 52008 | Coldshadow's Fantasy | `d2236b81b960e52f63f85e8958b10c8eec5d8ee984db0647b152a6486fdf7c50` |

Replication game seeds were 62001–62008. Each batch deliberately spans medium
through giant maps, one and two levels, no water/normal water/islands, and all
three monster settings.

### Endpoint and statistics

Wall-clock limits can produce different simulated day counts when AI choices
diverge. Comparing each process's final line would therefore mix policy effect
with “had more turns.” For each map, the primary endpoint is the latest game
day for which both assignment runs contain an end-of-turn snapshot for all
eight colors. This is the same simulated day on the same map for each paired
color. First-loss identity is analyzed separately, so an elimination is not
silently discarded.

Each batch contains 64 paired color/map observations; the combined set contains
128. Colors within one map are not independent. Inference therefore uses the
eight maps in each batch (sixteen combined) as conservative blocks:

- Compute treatment minus control for each color, then average the eight color
  effects within each map.
- Report a 95% percentile bootstrap interval resampling whole map blocks
  (50,000 deterministic resamples).
- Report an exact two-sided sign-flip test over all map-block sign assignments
  (`2^8` per batch, `2^16` combined).
- Treat the balanced composite as the sole primary endpoint; component p-values
  are exploratory.

Composite standardization was fitted to pooled batch-1 endpoints and then
frozen for the independent replication and combined analysis.

The balanced composite intentionally prevents gold from dominating the
verdict. It gives equal weight to:

1. expansion: towns and total town building levels;
2. force: `log1p` total army strength, MAIN army strength, and MAIN total
   strength;
3. development: maximum level among heroes currently assigned the actual
   `MAIN` role;
4. economy: `log1p` non-gold resource market value.

Each component is pooled-standardized before pairing. “Army” in the telemetry
is VCMI AI strength, not raw creature headcount. When several MAIN heroes exist,
the level/army/total-strength fields are maxima among actual MAIN-role heroes;
the three maxima need not belong to the same hero.

### Primary endpoint

| Analysis | Maps | Policy effect (SD) | 95% map-bootstrap CI | Exact p | Positive maps |
|---|---:|---:|---:|---:|---:|
| Batch 1 | 8 | +0.119 | [+0.024, +0.207] | 0.0547 | 7/8 |
| Independent replication | 8 | +0.051 | [-0.041, +0.147] | 0.3594 | 5/8 |
| Combined, secondary | 16 | +0.085 | [+0.017, +0.153] | 0.0313 | 12/16 |

The independent replication is the clean confirmatory read: it is directionally
consistent but inconclusive. The combined result is useful as an effect-size
estimate, not as an unqualified p=0.0313 claim, because the decision to collect
batch 2 followed inspection of batch 1.

Replication was not uniformly favorable. Its standardized effects were -0.054
for expansion, +0.127 for force, -0.011 for MAIN development, and +0.144 for
non-gold economy. Thus its small positive composite came from army and economy,
not better towns or MAIN levels in that batch.

### Combined standardized domains

| Domain | Policy effect (SD) | 95% map-bootstrap CI | Exact p | Positive maps |
|---|---:|---:|---:|---:|
| Expansion | +0.052 | [-0.055, +0.174] | 0.4338 | 8/16 |
| Force | +0.071 | [-0.070, +0.200] | 0.3300 | 12/16 |
| MAIN development | +0.048 | [-0.069, +0.164] | 0.4602 | 11/16 |
| Non-gold economy | +0.170 | [-0.015, +0.354] | 0.1018 | 12/16 |
| Balanced composite | +0.085 | [+0.017, +0.153] | 0.0313 | 12/16 |

No individual domain has a conclusive exact test. The designated balanced
composite is more stable because it asks whether the overall game state moves
in a favorable direction instead of requiring every noisy component to do so
on every map.

### Combined gameplay state

| Outcome | Enabled mean | Disabled mean | Paired effect | 95% map-bootstrap CI | Exact p |
|---|---:|---:|---:|---:|---:|
| Towns | 1.20 | 1.16 | +0.03 | [-0.03, +0.10] | 0.5312 |
| Town building levels | 13.89 | 13.66 | +0.23 | [-0.37, +0.90] | 0.5345 |
| Heroes | 2.79 | 2.77 | +0.02 | [-0.07, +0.12] | 0.7568 |
| MAIN hero level | 9.02 | 8.73 | +0.28 | [-0.41, +0.97] | 0.4602 |
| MAIN army strength | 70,790 | 64,291 | +15.6% | [-30.0%, +78.6%] | 0.5817 |
| MAIN total strength | 121,611 | 115,909 | +16.1% | [-30.0%, +78.6%] | 0.5732 |
| All-hero army strength | 95,777 | 87,111 | +23.3% | [+3.0%, +51.6%] | 0.0501 |
| Town garrison strength | 3,629 | 3,192 | +186.7% | [+2.9%, +764.4%] | 0.0806 |
| Total army strength | 99,407 | 90,303 | +18.8% | [+1.2%, +41.3%] | 0.0677 |
| Non-gold resource value | 46,221 | 42,570 | +7.7% | [-0.6%, +16.6%] | 0.1018 |
| Gold | 19,054 | 15,560 | +13.4% | [-16.9%, +57.0%] | 0.4626 |
| All-resource value | 65,274 | 58,130 | +11.3% | [+1.2%, +22.3%] | 0.0508 |

Percentage effects and their intervals use the paired map-block effect on the
`log1p` scale; the displayed means are raw arithmetic means. That is why a
percentage need not equal the percentage difference between the two displayed
means.

The army point estimates are favorable, but the MAIN-specific intervals are
especially wide. Town ownership and MAIN development are nearly unchanged.
This is not a case where extra gold masks materially worse towns, army, or MAIN
heroes: gold was omitted from the composite, total army was higher, non-gold
economy was higher, and the other two domains were slightly positive overall.
It is also not evidence that each of those components improved conclusively.

### Resources at matched endpoints

| Resource | Enabled mean | Disabled mean | Raw paired delta |
|---|---:|---:|---:|
| Wood | 39.69 | 36.19 | +3.50 |
| Mercury | 12.46 | 10.70 | +1.76 |
| Ore | 37.68 | 33.28 | +4.40 |
| Sulfur | 13.20 | 11.79 | +1.41 |
| Crystal | 12.57 | 12.72 | -0.15 |
| Gems | 15.53 | 15.20 | +0.34 |
| Gold | 19,054 | 15,560 | +3,494 |
| Non-gold market value | 46,221 | 42,570 | +3,650 |
| All-resource market value | 65,274 | 58,130 | +7,144 |

### First eliminations

| Analysis | Enabled losses | Disabled losses | No-loss runs | Survival effect | 95% CI | Exact p |
|---|---:|---:|---:|---:|---:|---:|
| Batch 1 | 6 | 4 | 6 | -0.125 | [-0.375, +0.000] | 1.0000 |
| Independent replication | 4 | 5 | 7 | +0.062 | [-0.188, +0.375] | 1.0000 |
| Combined | 10 | 9 | 13 | -0.031 | [-0.219, +0.156] | 1.0000 |

A positive survival effect means a disabled player was more likely to be the
first eliminated. The combined result is effectively neutral and supplies no
evidence that the policy either prevents or causes early elimination.

### Matched run endpoints

| Map | Common day | Even assignment | Odd assignment | Max complete day E/O |
|---|---:|---|---|---:|
| `g01-xl-islands` | 61 | time limit, 301 s | time limit, 302 s | 65/61 |
| `g02-m-dry-weak` | 6 | first loss, 7 s | first loss, 13 s | 6/12 |
| `g03-m-water-normal` | 8 | first loss, 11 s | first loss, 24 s | 8/18 |
| `g04-l-dry-strong` | 20 | first loss, 21 s | first loss, 21 s | 20/21 |
| `g05-xl-water-weak` | 34 | first loss, 136 s | first loss, 85 s | 46/34 |
| `g06-hu-dry-normal` | 52 | time limit, 302 s | time limit, 301 s | 71/52 |
| `g07-hu-water-strong` | 48 | first loss, 218 s | first loss, 160 s | 62/48 |
| `g08-gu-islands` | 41 | time limit, 301 s | time limit, 301 s | 42/41 |
| `g09-xl-islands` | 54 | time limit, 301 s | first loss, 195 s | 64/54 |
| `g10-m-dry-weak` | 7 | first loss, 9 s | first loss, 8 s | 8/7 |
| `g11-m-water-normal` | 8 | first loss, 22 s | process exit at loss, 10 s | 16/8 |
| `g12-l-dry-strong` | 20 | first loss, 36 s | first loss, 18 s | 31/20 |
| `g13-xl-water-weak` | 65 | first loss, 231 s | first loss, 204 s | 71/65 |
| `g14-hu-dry-normal` | 62 | time limit, 301 s | time limit, 301 s | 66/62 |
| `g15-hu-water-strong` | 62 | time limit, 301 s | time limit, 301 s | 62/63 |
| `g16-gu-islands` | 42 | time limit, 301 s | time limit, 301 s | 42/44 |

The `g11` odd process exited cleanly immediately after emitting the same loss
marker used by the runner; it is a first-loss stop, not a failed trial.

### Interpretation limits

- Each deliberately varied batch still has only eight independent
  environments (sixteen combined). Batch-level exact p-values are
  correspondingly coarse, and confidence intervals should be read as
  within-suite evidence rather than universal performance guarantees.
- The legal RoE demo data substantially narrows factions, artifacts, creatures,
  map objects, and rewardable-object variants versus a complete SoD install.
  The run covers the movement/resource/weekly-policy machinery but not every
  expansion-content interaction.
- Four enabled and four disabled players share each game. Their decisions
  affect one another, so the estimand is relative performance in a mixed-policy
  game, not eight isolated single-agent counterfactuals. Swapping every color
  on the identical map/seed cancels fixed starting-position bias but not this
  strategic interference.
- There is one process per assignment/map, rather than repeated executions of
  each identical assignment. The fixed server seed and matched starting state
  constrain randomness, but any residual task-scheduling nondeterminism is not
  separately estimated.
- Runs that reach a first loss are censored by design. The matched last complete
  day is used for continuous state metrics, while the lost player's feature
  status is reported separately.
- The disabled cohort retains shared corrections: profile-based MAIN roles,
  initialized path actions, and faithful composed-goal metadata. It is the old
  removable/carrier policy on corrected plumbing, not an unmodified old binary.

## Experiment-control commit

`c5446b69bd` adds the reproducible experiment controls:

- `NK2AI_PICK_REMOVABLES_PLAYERS=0,2,4,6` (or another numeric list) selects
  feature-enabled colors independently inside one game.
- `NK2AI_EXPERIMENT_SEED` fixes server randomization.
- `NK2AI_EXPERIMENT_SNAPSHOTS` records begin/end-of-turn JSON snapshots with
  towns, total town building levels, heroes, actual role-based MAIN maxima,
  hero/town/total army strength, each resource, and resource market values.
- A loss marker lets the runner stop at the first player elimination.

The disabled group restores the prior policy decisions while retaining the
shared role and structural corrections described above. Consequently the
experiment estimates the new removable/carrier policy on top of those
corrections; it is not a byte-for-byte checkout of old `develop`.

## Reproducibility notes

- Tested implementation tree: `e45f5b8c229c73c66746863e9f0a68f00b49be60`.
  This includes the twelve isolated `fixup!` commits and experiment commit
  `c5446b69bd`, but predates this report-only commit.
- All 16 RMG generations, all 16 load/gate/telemetry smoke checks, and all 32
  five-minute-or-first-loss trial processes completed with successful harness
  statuses. The one `process_exit` label in the endpoint table was a clean
  process exit coincident with a recorded first loss.
- Analysis accepted 128 paired player/map observations in 16 independent map
  blocks. It verified every per-player feature flag, the paired map URI and
  server seed, all recorded map SHA-256 hashes, and equality of color 0's true
  pre-action snapshot across each assignment pair.
- Full analysis artifacts were copied off the VM before shutdown:

  | Artifact | SHA-256 |
  |---|---|
  | `analysis.md` | `979818b31839476b4693cae435f841f0e42c12b5f6d6cbd405a8059e5f6de994` |
  | `analysis.json` | `41807f90931ed5ba0388f5ba271811fb87ba3655ccf0d4d8467a9463bf662e50` |
  | `endpoints.csv` | `d6a228d597db033d64152ba38b1479e9bf6d19d5bdadb3702f1c8700b54bb6da` |

- The final analysis used 50,000 deterministic whole-map bootstrap resamples
  and exact enumeration of `2^8` or `2^16` block sign assignments.
- The custom RMG command-line driver and orchestration scripts were remote
  harness material; they did not modify the branch source. The committed
  per-player gate and snapshot fields are the reusable product-side controls.
- Pinned VM `codex-spot-ai-pick-review-03d9bc` was stopped after the artifacts
  were copied; its final state was `TERMINATED`.
- No compilation, test binary, map generator, game process, or analysis program
  was run locally.
