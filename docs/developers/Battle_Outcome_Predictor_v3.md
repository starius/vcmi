# Battle Outcome Predictor v3 Plan

## Goal

Extend battle outcome training data and Nullkiller2 prediction so it can model richer open-field battles and town/siege battles without training on information Nullkiller2 cannot know at decision time.

The production predictor should remain lightweight and deterministic. MMAI is used to generate training labels, not as a runtime dependency for Nullkiller2 planning.

## Core Principle

For every feature that may be hidden from the AI, collect two views:

- `actual`: exact game-state value, used for analysis and debugging.
- `estimated`: value available to Nullkiller2 at planning time, including inferred or predicted values.

Production training should use `estimated` features. `actual` features are useful to measure how much prediction error comes from hidden information.

## Battle Types

Train and evaluate separate battle-type buckets, or include explicit battle-type features:

- hero vs wandering monsters
- hero vs hero
- hero vs town/garrison
- hero vs town with defending hero
- siege-specific variants, including fort/citadel/castle

Town and siege battles should not be calibrated from open-field data only.

## Hero Features

Collect for attacker and defender, with `actual` and `estimated` variants where applicable:

- hero type, class, level
- primary skills: attack, defense, spell power, knowledge
- raw mana values: current mana, mana limit, missing mana
- mana-derived values: mana ratio, castable turns proxy
- spellbook presence
- battle-relevant secondary skill levels:
  - Offense
  - Armorer
  - Archery
  - Tactics
  - Ballistics
  - Artillery
  - Sorcery
  - Intelligence
  - Resistance / Interference
  - Wisdom
  - Air, Earth, Fire, Water Magic
  - Leadership
  - Luck
- hero specialty if it affects battle strength
- relevant artifact-derived battle bonuses when known or inferred

## Spell Features

Collect raw spellbook data and derived spell categories:

- combat spell IDs in spellbook
- school level for each castable spell
- spell mana cost after modifiers if available
- castable flag at current mana
- category power summaries:
  - direct damage
  - mass buffs
  - mass debuffs
  - resurrection / animate dead
  - summon / clone
  - blind / disable
  - haste / slow
  - shield / stone skin / bloodlust-style buffs
  - anti-siege spells such as Earthquake
  - mobility spells such as Teleport

For enemy heroes, estimated spell features should account for observed casts, known mage guild access, hero class expectations, and missing-information flags.

## Army Features

Keep raw army data:

- creature ID
- count
- stack slot
- stack experience
- stack power

Also derive stable battle features:

- total army strength
- total HP
- total damage proxy
- shooter power
- flyer power
- caster power
- fast stack power
- slow stack power
- no-retaliation power
- spell-immune or magic-resistant power
- undead / elemental / mind-immune power
- largest stack share
- stack count
- average and maximum speed
- morale and luck relevant bonuses if known

## Town And Siege Features

Collect town context as first-class data:

- town type / faction
- owner and player relation
- fortification level: none, fort, citadel, castle
- wall state at battle start:
  - gate
  - each wall segment
  - towers
- moat presence and type
- battlefield ID and terrain
- garrison army before battle
- visiting hero army before battle
- combined defending army after battle setup, if available
- defending hero source: none, garrison, visiting
- town buildings that affect battle directly or indirectly:
  - fort / citadel / castle
  - tower-related buildings
  - moat or faction defensive buildings
  - grail or special faction buildings
  - morale-affecting buildings such as tavern-like effects when applicable
  - mage guild level and known spells for defender spell inference
  - other buildings that provide creature bonuses
- known town bonuses affecting units

Do not apply town/siege model calibration to non-town battles unless evaluation proves shared features are stable.

## Enemy Approximation

Enemy data should be represented with uncertainty:

- exact known values from visibility or previous battle actions
- inferred values from hero class, hero level, faction, map progress, and visited objects
- observed spells and inferred spellbook candidates
- missing flags for unknown skill/spell/mana fields
- pessimistic and expected estimates where safety matters

For safety decisions, evaluate either:

- expected profile plus a conservative safety threshold, or
- several plausible enemy profiles and use a low-percentile win probability.

## Data Generation

Generate deterministic datasets with:

- fixed global seed
- fixed MMAI model seed
- recorded MMAI model version and config
- repeated simulations per generated setup
- separate shards per setup
- both hero-v-monster and hero-v-hero setups
- targeted near-threshold battles, not only uniform random budgets
- dedicated town/siege generators

For each setup, store enough stable identifiers to group repeated simulations into an empirical outcome distribution.

## Training And Runtime Model

Prefer compact deterministic models for Nullkiller2:

- logistic model for win probability / safety
- calibrated equivalent-danger output for compatibility with existing pathfinding
- separate loss model for expected army loss
- separate coefficients by battle type if needed

Avoid adding MMAI, ONNX Runtime, or heavyweight ML dependencies to Nullkiller2 planning.

## Validation

Offline validation:

- Brier score for probability calibration
- accuracy for win/loss classification
- false-safe and false-unsafe counts
- separate metrics by battle type
- holdout by generated setup, not by individual replay row
- dataset integrity gate before analysis:
  - expected row count and schema
  - expected shard count and repeated rows per shard
  - required battle-type coverage
  - no MMAI fallback/config-error log lines
  - MMAI model initialization in every shard log when MMAI labels are expected

End-to-end validation:

- run paired AI games on identical maps and seeds
- swap sides/colors between old and new predictor
- use deterministic AI seeds
- collect win rate, score, towns, heroes, army value, resources, turns survived, and crash/assertions
- compare old predictor vs new predictor with confidence intervals
- treat battle-level improvement and game-level improvement as separate evidence
- use the existing `AI/Nullkiller2/tools/compare_battle_predictors.py` harness for large A/B runs. It already supports color-swapped paired samples, deterministic random-map seeds, parallel jobs, day-limit adjudication from `statistics.csv`, and candidate AI names such as `Nullkiller2Ratio`, `Nullkiller2V2`, and `Nullkiller2V3`.

## Current Empirical Findings

Recent MMAI-labeled datasets show that simple global ratio/logistic tuning is not enough for the 95% target.

Remote analysis outputs:

- `/root/vcmi-battle-results/mmai-schema2-5k-evaluation-v3-compatible.txt`
- `/root/vcmi-battle-results/mmai-schema2-5k-evaluation-composition-diff.txt`
- `/root/vcmi-battle-results/mmai-100k-creature-value-model.txt`
- `/root/vcmi-battle-results/mmai-100k-creature-value-pair100-model.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-mixed-2k/evaluation-richstats.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-mixed-2k/simulation-fallback-all-cxx.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-mixed-2k/simulation-fallback-allwins-diagnostics.txt`
- `/root/vcmi-battle-results/mmai-100k-simulation-fallback.txt`
- `/root/vcmi-battle-results/mmai-100k-simulation-fallback-safe095.txt`
- `/root/vcmi-battle-results/mmai-100k-simulation-fallback-prob095-diagnostics.txt`
- `/root/vcmi-battle-results/mmai-100k-simulation-fallback-allwins-diagnostics.txt`
- `/root/vcmi-battle-results/mmai-100k-simulation-fallback-wilson093-diagnostics.txt`
- `/root/vcmi-battle-results/mmai-100k-simulation-fallback-wilson094-diagnostics.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-real-parallel-smoke`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-real-mixed-2k-combined-20260711/evaluation.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-real-mixed-2k-combined-20260711/simulation-fallback-allwins.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-real-mixed-5k-20260711`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-2k-fix-20260711`

Observed pattern:

- current v3-compatible features are around the high-80% range on held-out battle setups
- naive raw creature composition features overfit badly
- learned per-creature strength multipliers improve held-out 50% accuracy to about 92%, but safety accuracy remains below 90%
- exact dominant-creature pair biases do not generalize enough on the 100k dataset
- schema3 stack stats and richer aggregate creature features improve a small mixed/town 2k held-out set to about 89% at threshold 0.5 and about 94% at the best threshold, but still miss the 95% target
- repeated simulation is the first approach that clears the 95% win/loss target on broad data: on the 100k MMAI set, using existing repeated rows as a proxy, fallback-only sampling gives about 96% win/loss accuracy with 1 sample, 98% with 3 samples, and 98-99% with 5+ samples
- safety decisions need a more conservative threshold than the current 0.60 probability cutoff: with a 0.95 cutoff, 5 simulated samples give about 98.6% win/loss accuracy and 96.6% safety accuracy on the 100k set, while 10 samples give about 98.8% and 97.1%
- the 100k fallback diagnostics show why safety should not be driven only by the sampled win-rate probability: at 50 samples, accepting `sampleWinRate >= 0.95` still produced 4 false-safe groups / 155 false-safe holdout rows; those were mostly 48/50 or 49/50 sampled-win cases whose holdout win rates were only 80-92%
- an all-wins safety policy is conservative but directly attacks false-safe risk: at 50 samples on the 100k holdout it kept 99.38% win/loss accuracy and produced 0 false-safe rows, at the cost of 10 false-unsafe groups / 266 rows
- Wilson lower-bound policies give a tunable version of the same tradeoff: with one-sided 90% Wilson lower bound and threshold 0.94, 50 samples again produced 0 false-safe rows and 10 false-unsafe groups / 266 rows; threshold 0.93 allowed one 49/50 sampled-win false-safe case
- the schema3 mixed dataset with town battles is still too small, but all-wins safety had 100% win/loss and safety accuracy on its eligible held-out rows at 5 and 10 samples, including town rows
- the initial schema3 mixed/town run that was named `mmai` was not valid MMAI training evidence: shard logs showed missing MMAI config and fallback to BattleAI. Correct MMAI collection requires the MMAI mod to be active and isolated per parallel client.
- the corrected schema3 MMAI mixed run from 2026-07-11 has 2000 rows, 100 generated setup shards, 20 repeats per shard, 760 town rows, and 0 MMAI fallback lines. It used per-shard XDG config/cache profiles to avoid profile races between parallel clients.
- the corrected schema3 MMAI mixed run also passes the schema3 rich-field gate: attacker/defender raw mana, secondary skills, full spell lists, combat spell lists, primary skills, rich creature stack stats, town faction/buildings, fortifications, moat/tower shooters, tower/keep damage ranges, and final wall state are present for every applicable row.
- the corrected 5k schema3 MMAI mixed run from 2026-07-11 has 5000 rows, 100 generated setup shards, 50 repeats per shard, 1050 hero-vs-hero rows, 2200 hero-vs-monster rows, 1750 town rows, and 0 MMAI fallback lines. It also passes the schema3 rich-field gate.
- generated battle mode now supports explicit `town-hero` battles, and `mixed` mode includes them for future datasets. The corrected town-hero 2k run from 2026-07-11 has 2000 rows, 100 generated setup shards, 20 repeats per shard, 2000 town-hero rows with visiting defending heroes, and 0 MMAI fallback lines. It passes the schema3 rich-field gate with defender hero data, town buildings, fortifications, tower damage, and final wall state.
- on that corrected schema3 MMAI data, static prediction is not good enough: held-out cxx-v3 accuracy was about 69%, v3-compatible fitted accuracy about 85%, and full fitted model accuracy about 73%. The high training accuracy did not generalize.
- after separating the current deployed static scope from town/siege rows, cxx-v3 is still not good enough: on corrected 5k non-town rows, held-out cxx-v3 accuracy was about 69.4% with Brier score 0.287, versus about 85.9% / 0.150 for the baseline and 85.9% / 0.120 for the ratio-only fitted model. A v3-compatible refit reached about 92.4% on training rows but only about 82.5% on held-out rows, so another C++ coefficient update is not supported.
- adding secondary-skill identities and combat-spell identities to the corrected 5k non-town analysis improved held-out accuracy to about 93.0% and Brier score to about 0.049, but training accuracy was about 99.3%, so this is promising feature evidence rather than a mergeable static model yet.
- corrected schema3 worst cxx-v3 errors include confident sign mistakes: e.g. predicted probabilities below 1% for setups that MMAI won 100% of the time, and a 98.5% predicted win for a setup lost 100% of the time. This points to root-cause modeling gaps, not a threshold-only problem.
- close/even diagnostics on the corrected 5k slice show the same confident-static-error shape: one non-town 47.6% empirical hero-vs-hero setup was predicted at 0.94%, and town close/even errors include high-fort/mage/moat sieges predicted above 88-96% despite empirical win rates around 38-47%.
- close/even diagnostics on the corrected town-hero 2k run show 9 close/even groups with cxx-v3 absolute error above 0.25. Examples include a 30% attacker win rate predicted at 99.9% against a visiting defender with several combat spells, and a 72.7% attacker win rate predicted at 6.7% into a fort-3/mage-4/grail town. The failures are bidirectional and siege-specific, not a threshold-only issue.
- on the same corrected schema3 data, fallback-only repeated simulation with an all-wins safety rule reached 97.6% win/loss accuracy and 100% safety accuracy with 3 samples, 98.5% / 100% with 5 samples, and 100% / 100% with 10 samples on eligible held-out rows. The dataset is still small, but it matches the broader 100k proxy direction.
- using current deployed cxx-v3 coefficients as the static/hybrid baseline on the corrected 5k run, fallback-only repeated simulation still clears the target: on non-town deployed-static holdout rows, all-wins fallback reached about 97.2% win/loss accuracy and 100% safety accuracy with 3 samples, 97.8% / 97.3% with 10 samples, and 99.7% / 98.4% with 20 samples. On town rows it reached 100% / 100% with 1-3 samples, 95.2% / 100% with 5 samples, and 100% / 100% with 10 samples. These are still proxy numbers, but they show the fix must be runtime simulation, not another cxx-v3 coefficient patch.
- on the corrected town-hero 2k run, current cxx-v3 alone reached only about 58.8% held-out win/loss accuracy, 56.5% safety accuracy, and Brier score 0.363. Fallback-only repeated simulation with all-wins safety reached 100% win/loss accuracy and 95.4% safety accuracy with 3 samples, and 98.4% / 95.2% with 5 samples. Ten-sample results had fewer eligible holdout groups and stayed 100% win/loss but only 88.3% safety because of one false-safe and one false-unsafe group.
- a hybrid static-probability band such as `[0.20, 0.95]` is not enough yet: one corrected schema3 holdout setup had static probability below 1% while actual holdout win rate was 100%, so static confidence cannot currently decide when simulation may be skipped.
- the same issue remains when the hybrid baseline is explicitly current cxx-v3: on the corrected 5k non-town holdout, `[0.20,0.95]` with all-wins fallback stayed around 77-80% win/loss accuracy for 1-30 samples because confidently wrong static cases were left unsimulated. On corrected town-hero 2k holdout rows, the same hybrid band stayed around 61-63% accuracy for 3-10 samples.
- static v3 town/siege calibration is not currently deployed in Nullkiller2. Corrected schema3 data shows static town prediction is not reliable enough, so towns continue to use legacy danger until runtime simulation or a separately validated town model is available.
- worst errors are repeated matchup/special-case failures, not just calibration threshold mistakes

The next likely useful model needs either a stronger non-linear model with better generalization evidence or a deterministic simulation fallback for high-impact uncertain battles. Another global ratio-only coefficient update is unlikely to reach the target by itself.

## Runtime Simulation Fallback Direction

Measured fallback behavior uses repeated MMAI outcomes as a proxy for running a battle several times at decision time. This is not a direct implementation yet, but it gives a target:

- use the static v3/rich model as a cheap first pass
- invoke battle simulation for attack decisions until static confidence is empirically trustworthy. The corrected schema3 run shows static confidence can be confidently wrong, so a narrow uncertainty band is unsafe as the first runtime gate.
- after runtime simulation exists, a cheap static model can still be used to order candidates, cache keys, or skip strategically irrelevant checks, but not as the only safety gate for taking a battle.
- use deterministic seeds derived from game seed, hero id, target object id, turn, and fallback sample index
- evaluate at least 5 samples for win/loss prediction; use 10+ samples or a stricter all-wins style rule for safety-sensitive attacks
- treat win/loss probability and safety as separate outputs:
  - use sampled win rate, possibly calibrated/shrunk, for expected-value decisions
  - use `all samples won` or a Wilson lower-bound threshold for safety-sensitive attacks, especially when the army loss or strategic exposure is high
- cache simulation results per `(hero army state, hero stats, target state, battle context, model seed)` so pathfinding does not replay the same battle repeatedly
- expose the fallback behind a setting until end-to-end AI games prove it improves outcomes

Architecture caveat: current quick combat/autofight goes through normal battle flow with server/client combat AI interfaces. `BattleSimulationBatch` repeats an already-started live battle through `BattleProcessor::restartBattle`, a live `CBattleQuery`, network packs, and real `CGameState` mutation. There is no small in-process Nullkiller API that clones an arbitrary visible battle state and returns a deterministic win distribution. The next implementation step is therefore to build a reusable headless battle-evaluation service from the existing battle simulation batch path, not to call client quick combat directly from pathfinding.

## Runtime Simulation Service Plan

The runtime fallback should be implemented as a separate branch after the schema/tooling work is committed cleanly. Keep the experimental Nullkiller heuristic coefficient changes separate from the data-generation and analysis tools.

Implementation outline:

1. Extract the reusable parts of `BattleSimulationBatch` into a server-side battle-evaluation service that can run a fixed battle setup repeatedly and return aggregate counts, not JSONL-only side effects. The first extraction point is the replay setup around `BattleProcessor::restartBattle`; it must be separated from the batch writer and made explicit about the state it mutates/restores.
2. Keep the first implementation process-isolated or server-owned. Nullkiller should ask for an evaluation through a controlled API/cache; it should not mutate live game state or call client quick combat directly.
3. Define deterministic seed derivation from stable context: game seed, player, hero instance id, target object id, battle type, turn, and sample index.
4. Store an evaluation cache keyed by a normalized battle state fingerprint: attacker army/stats/mana/spells, defender army/stats/mana/spells, town/siege state, terrain/battlefield, and evaluator version.
5. Return at least:
   - simulated sample count
   - attacker win count
   - no-winner count
   - empirical win probability
   - all-wins safety flag
   - Wilson lower-bound safety score
   - optional expected surviving army value / loss distribution
6. Add a Nullkiller setting for fallback mode:
   - disabled
   - uncertain-only
   - high-impact-only
   - always for attack decisions
7. Run paired end-to-end AI games with old predictor vs static-v3+fallback before making it default. The battle-level proxy proves the fallback can predict outcomes; it does not by itself prove better adventure-map play.

Current branch progress toward the service boundary:

- `BattleStartInfo` now names the battle setup passed to start/restart battle flow: attacking and defending armies, heroes, tile, layout, and defended town. Existing UI retry and batch replay both use this same setup path through `BattleProcessor::restartBattle`.
- `BattleSimulationBatch` now tracks `BattleSimulationSummary` counts in memory and returns a `BattleSimulationRecordResult` from result recording. The current batch collector still writes JSONL, but future runtime evaluation code can now consume aggregate attacker/defender/no-winner counts without parsing the output file.
- `BattleSimulationSummary` and `BattleSimulationRecordResult` live in `BattleSimulationResult`, separate from the JSONL batch collector. `BattleSimulationSummary` exposes the safety outputs needed by the fallback plan: empirical attacker win rate, all-attacker-wins, all-defender-wins, and Wilson lower-bound attacker safety score.
- This is still not a runtime Nullkiller evaluator. The remaining hard part is isolating repeated simulations from live adventure-map state and exposing them through a controlled server-owned API/cache.

Batch collection caveat: when running `vcmibattlesim` with MMAI in parallel, each shard needs an isolated XDG config/cache profile. A shared profile can be rewritten by clients and silently disable the MMAI mod for later shards. Use `--xdg-config-template` and, if needed, `--xdg-profile-root` so each client starts from the same active-mod configuration.

Use the dataset validator before fitting or reporting numbers:

```bash
python3 scripts/battle_prediction/validate_battle_dataset.py \
  schema3-richstats-mmai-real-mixed-2k-combined-20260711.tar.gz \
  --expected-rows 2000 \
  --expected-schema 3 \
  --expected-shards 100 \
  --expected-shard-size 20 \
  --require-complete-shards \
  --require-battle-types hero-hero,hero-monster,town \
  --require-no-mmai-fallback \
  --require-mmai-initialized
```

The corrected archive passes this gate. The earlier contaminated `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-mixed-2k` run fails with 8980 MMAI fallback/config-error log lines and 0/100 shard logs initializing MMAI.

## Merge Strategy

Start with the safest mergeable step:

1. Ratio-only calibration or equivalent-danger calibration for open-field hero-v-monster and hero-v-hero battles.
2. Keep old heuristic fallback for towns, sieges, missing data, and unsupported contexts.
3. Add richer schema v3 collection.
4. Train town/siege-aware models.
5. Enable richer model behind a setting for A/B testing.
6. Promote to default only after offline and end-to-end metrics are both favorable.
