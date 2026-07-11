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

For each setup, store enough stable identifiers to group repeated simulations into an empirical outcome distribution. For generated repeated-simulation datasets, evaluate by shard key rather than by the full realized `setup_key`: stochastic battle setup can change fields such as battlefield across repeats, and grouping by full realized setup splits one generated setup into multiple smaller buckets.

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
- use `--group-key shard` for generated repeated-simulation datasets; reserve full setup grouping for datasets where each distinct realized setup is the intended unit
- use deterministic group cross-validation for small or still-growing generated datasets before interpreting fitted town/siege models, for example:
  `python3 scripts/battle_prediction/evaluate_nullkiller_predictor.py <dataset> --scope town --group-key shard --complete-shards-only --summary-only --cv-folds 5`
- when testing a candidate town/siege model, add `--cv-threshold-mode train-best-safety --cv-print-failures N` to choose the safety threshold only on each training fold and inspect the worst held-out false-safe and false-unsafe groups before considering any C++ coefficient port
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

### Best Result So Far

As of 2026-07-11, the best empirical result is fallback-only deterministic repeated simulation with MMAI labels and an all-wins safety rule. These are still offline proxy results, not a deployed Nullkiller2 runtime result.

- corrected 5k mixed run, current deployed non-town scope (`schema3-richstats-mmai-real-mixed-5k-20260711`, `simulation-fallback-cxx-v3-deployed-static-allwins.txt`):
  - 3 samples: 97.23% win/loss accuracy, 100.00% safety accuracy on 649 held-out rows
  - 20 samples: 99.69% win/loss accuracy, 98.44% safety accuracy on 321 held-out rows
- corrected 5k mixed run, town-without-hero scope (`simulation-fallback-cxx-v3-town-allwins.txt`):
  - 1 and 3 samples: 100.00% win/loss and safety accuracy on 401 and 377 held-out rows
  - 10 samples: 100.00% win/loss and safety accuracy on 293 held-out rows
- corrected 2k town-hero run (`schema3-richstats-mmai-town-hero-2k-fix-20260711`, `simulation-fallback-cxx-v3-town-allwins.txt`):
  - 3 samples: 100.00% win/loss accuracy, 95.37% safety accuracy on 367 held-out rows
  - 5 samples: 98.39% win/loss accuracy, 95.18% safety accuracy on 311 held-out rows
- completed corrected 5k town-hero run (`schema3-richstats-mmai-town-hero-5k-20260711`, `simulation-fallback-cxx-v3-town-allwins.txt`):
  - 5 samples: 98.33% win/loss accuracy, 95.38% safety accuracy on 1255 held-out rows
  - 10 samples: 98.44% win/loss accuracy, 95.32% safety accuracy on 1025 held-out rows
  - 20 samples: 97.94% win/loss accuracy, 100.00% safety accuracy on 583 held-out rows
  - 30 samples: 100.00% win/loss accuracy, 93.33% safety accuracy on 300 held-out rows; this bucket had one false-unsafe case and no false-safe cases
- completed corrected 5k town-hero run, regrouped by generated shard (`--group-key shard`):
  - 3 samples: 96.30% win/loss accuracy, 96.30% safety accuracy on 27 held-out shards; one false-safe case
  - 5 samples: 100.00% win/loss accuracy, 96.30% safety accuracy on 27 held-out shards; one false-safe case
  - 10 samples: 100.00% win/loss accuracy, 100.00% safety accuracy on 27 held-out shards / 1080 eligible holdout rows
  - 20 samples: 100.00% win/loss accuracy, 100.00% safety accuracy on 27 held-out shards / 810 eligible holdout rows
  - 30 samples: 100.00% win/loss accuracy, 96.30% safety accuracy on 27 held-out shards; one false-unsafe case and no false-safe cases

The best static models are not merge-ready: current cxx-v3 was about 69.39% / Brier 0.287 on corrected 5k non-town deployed-static holdout rows, about 58.76% / Brier 0.363 on corrected 2k town-hero holdout rows, and 59.26% / Brier 0.372 on the completed corrected 5k town-hero shard holdout. On the completed 5k town-hero shard split, skill/spell static features reached 88.89% / Brier 0.107 on holdout after about 98.63% training accuracy, so this is still overfit offline evidence and not enough to merge as Nullkiller2 logic. Runtime simulation is available through a client-side cloned-state evaluator, but it is disabled by default and previously timed out when enabled in live Nullkiller turns. The practical direction is still offline MMAI simulation for labels plus a better validated static predictor, with runtime simulation as the high-confidence reference/fallback once live-turn cost is controlled.

### Static Town Prototype

A compact `town-deployable` evaluator prototype was added to `scripts/battle_prediction/evaluate_nullkiller_predictor.py`. It uses features that are plausible to port into Nullkiller2 danger evaluation: deployed town danger ratio, raw army strengths, hero primary/mana/spell counts, stack shape, fortification state, town faction, terrain, and battlefield buckets. Schema5 rows also expose pre-merge visiting-hero siege components: separate town-garrison and defending-hero army strengths, stack counts, largest-stack shares before `mergeGarrisonOnSiege`, and derived merge-loss features such as pre-merge army left outside the started battle, participating share, and post-merge town-army share.

On the completed corrected 5k town-hero run:

- deployed town danger baseline: 68.15% held-out win/loss accuracy, Brier 0.2916
- current cxx-v3 probability applied to towns for diagnostics only: 54.48% held-out win/loss accuracy, Brier 0.3918
- compact `town-deployable` model with regularization: about 81-83% held-out win/loss accuracy, Brier about 0.105-0.114 depending on threshold and L2
- richer fitted static models with non-deployable or harder-to-port features: about 84-85% held-out win/loss accuracy

On the same run grouped by generated shard (`--group-key shard`, 73 train shards / 27 holdout shards):

- deployed town danger baseline: 77.78% held-out win/loss accuracy, Brier 0.2093, with 3 false-safe and 2 false-unsafe groups
- current cxx-v3 probability applied to towns for diagnostics only: 59.26% held-out win/loss accuracy, Brier 0.3721, with 7 false-safe and 3 false-unsafe groups
- compact `town-deployable` model with regularization: 85.19% held-out win/loss accuracy, Brier 0.1164
- richer deployable town/siege interactions did not improve holdout on this split: 77.78% held-out win/loss accuracy, Brier 0.1243
- broader skill/spell static features reached 88.89% held-out win/loss accuracy, Brier 0.1073, but remain diagnostic-only rather than a deployable Nullkiller2 model

Conservative thresholds can eliminate false-safe groups on this small town holdout, but they do not approach 95% win/loss accuracy and introduce false-unsafe groups. This is not merge-ready as a production town predictor by itself.

Early schema5 smoke check on the live 20k town-hero run, at only 21 complete shards / 1050 complete-shard rows, shows the new merge-loss features are wired end-to-end but does not prove generalization. In that small snapshot, current cxx-v3 remained unsafe for town rows (`test` 0/2 groups, two false-safe groups), while the deployable town prototype fit the two held-out groups. This should be treated as a feature-path check only until substantially more schema5 shards complete.

The schema5 segment analyzer also now emits specific combat-spell, secondary-skill, high-share creature-ID, and exact town-building-ID segments. On the live run at 40 complete shards / 2000 complete-shard town rows, a false-safe-focused cxx-v3 segment report found 17 predicted-safe town groups below 95% empirical win rate. Those groups averaged only about 11.8% attacker win rate while cxx-v3 averaged about 93.8% predicted win probability. The small-sample false-safe segments were dominated by visiting-hero sieges with defender spell advantages, large negative combat-spell count differences, moat/full-wall states, and sometimes pre-merge army left outside the started battle. Town building IDs are now visible in these reports, including fortification and town-structure IDs, but they are currently diagnostic inputs for root-cause search and the richer prototype only; they are not yet a merge-ready Nullkiller2 feature set.

An interim 300-epoch schema5 smoke fit on the same 40 complete shards produced only 5 held-out groups, so it is not statistical evidence. It still reinforces the direction: cxx-v3 was 20.00% held-out accuracy / Brier 0.5782 with 4 false-safe groups, the compact town-deployable prototype was 60.00% / 0.1787, and the richer town-deployable prototype was 60.00% / 0.1162. Treat this as a wiring and failure-shape check until the full schema5 run completes.

The main observed static-model failure mode is interaction-heavy siege behavior: creature composition, battlefield layout, and terrain can change outcomes substantially for otherwise similar army/town setups. Repeated MMAI simulation remains the only result above the 95% target.

The evaluator can now emit reproducible town-deployable diagnostics with `--print-worst`, `--print-town-deployable-false-safe`, `--print-town-deployable-false-unsafe`, and `--town-deployable-safe-probability`. Use these reports on the next large run to inspect close/even and safety-threshold failures without relying on ad hoc one-off scripts.

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
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-partial-complete-20260711T065855Z`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-partial-complete-20260711T065855Z/evaluation-town-deployed-danger.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-partial-complete-20260711T071521Z`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-partial-complete-20260711T071521Z/evaluation-town-deployed-danger.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-partial-complete-20260711T071521Z/deployed-danger-close-even-town-scope.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-partial-complete-20260711T071521Z/deployed-danger-false-safe-town-scope.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-partial-complete-20260711T071521Z/deployed-danger-false-unsafe-town-scope.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711/validation.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711/evaluation-town-scope.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711/evaluation-town-deployed-danger.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711/simulation-fallback-cxx-v3-town-allwins.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711/v3-close-even-town-scope.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711/deployed-danger-close-even-town-scope.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711/deployed-danger-false-safe-town-scope.txt`
- `/root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711/deployed-danger-false-unsafe-town-scope.txt`

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
- the completed corrected 5k town-hero run from 2026-07-11 has 5000 rows, 100 complete generated setup shards, 159 setup groups, all town-hero rows with visiting defending heroes, and 0 MMAI fallback lines. It passes the schema3 rich-field gate with defender hero data, town buildings, fortifications, tower damage, and final wall state.
- schema4 battle rows add explicit `initialWallState` for town/siege battles. Schema3 kept only the post-battle `finalWallState`; schema4 records both the planner input wall state derived by the same rules as `BattleInfo::setupBattle` and the final wall state for analysis.
- schema5 battle rows add `townPreMergeState` for visiting-hero inside sieges. This fixes the schema3/schema4 blind spot where the town garrison had already been merged into the defending hero before JSONL recording, leaving `defendedTown.armyStrength` as zero and making static town features unable to see the original town/hero split.
- schema6 battle rows add `battleStartStacks` and `battleStartObstacles`. `battleStartStacks` is captured after tactics, summons, stack enchantment triggers, and opening spells, before first-round turn selection, and records every non-ghost stack's effective attack/defense/damage/speed, morale/luck, current health, current/initial hex, shooting/casting capability, war-machine/tower flags, and turret aggregates. `battleStartObstacles` records obstacle type/position/affected tiles plus blocking, stopping, trigger, hidden/trap, moat, and spell-created obstacle fields. This targets a remaining root-cause gap: schema5 still has base creature stats and army power, but not the effective stack values or battlefield obstacle layout after hero, town, terrain, artifact, tactics, and battle-start effects are applied.
- close-even diagnostics on the completed 5k town-hero run found 11 groups / 315 rows with actual win rate 25-75% and cxx-v3 error at least 0.25. The worst false-safe cluster was `predicted >= 0.95`: 5 groups / 200 rows, actual average 37.00%, predicted average 98.82%.
- town cxx-v3 false-safe segments are dominated by siege mechanics absent from the open-field model: moat/castle/tower/mage-guild/grail effects and defender spell access. The false-unsafe side is different: attacker combat spell advantage, flyers, shooters, high-speed stacks, and special abilities can overcome town defenses, while cxx-v3 still assigns very low probabilities.
- cxx-v3 has no deployed static scope for town-hero rows. Town/siege prediction should remain on legacy danger plus targeted safety fixes until the runtime simulation service is available or a separately validated town model clears the same holdout and A/B gates.
- the corrected deployed legacy town-danger mirror is weak in both directions on the completed 5k town-hero run. At factor 1.0 it reached 68.15% held-out win/loss accuracy, Brier 0.292, 2 false-safe groups, and 7 false-unsafe groups. Segment reports across train and test contain 20 predicted-safe groups below 95% actual win rate and 17 predicted-unsafe groups above 95% actual win rate. This reinforces that town/siege prediction needs runtime simulation or a richer validated town model, not only a scalar danger patch.
- on that corrected schema3 MMAI data, static prediction is not good enough: held-out cxx-v3 accuracy was about 69%, v3-compatible fitted accuracy about 85%, and full fitted model accuracy about 73%. The high training accuracy did not generalize.
- after separating the current deployed static scope from town/siege rows, cxx-v3 is still not good enough: on corrected 5k non-town rows, held-out cxx-v3 accuracy was about 69.4% with Brier score 0.287, versus about 85.9% / 0.150 for the baseline and 85.9% / 0.120 for the ratio-only fitted model. A v3-compatible refit reached about 92.4% on training rows but only about 82.5% on held-out rows, so another C++ coefficient update is not supported.
- adding secondary-skill identities and combat-spell identities to the corrected 5k non-town analysis improved held-out accuracy to about 93.0% and Brier score to about 0.049, but training accuracy was about 99.3%, so this is promising feature evidence rather than a mergeable static model yet.
- corrected schema3 worst cxx-v3 errors include confident sign mistakes: e.g. predicted probabilities below 1% for setups that MMAI won 100% of the time, and a 98.5% predicted win for a setup lost 100% of the time. This points to root-cause modeling gaps, not a threshold-only problem.
- close/even diagnostics on the corrected 5k slice show the same confident-static-error shape: one non-town 47.6% empirical hero-vs-hero setup was predicted at 0.94%, and town close/even errors include high-fort/mage/moat sieges predicted above 88-96% despite empirical win rates around 38-47%.
- close/even diagnostics on the corrected town-hero 2k run show 9 close/even groups with cxx-v3 absolute error above 0.25. Examples include a 30% attacker win rate predicted at 99.9% against a visiting defender with several combat spells, and a 72.7% attacker win rate predicted at 6.7% into a fort-3/mage-4/grail town. The failures are bidirectional and siege-specific, not a threshold-only issue.
- close/even diagnostics on the live schema5 town-hero run at 51 complete shards / 2550 complete-shard rows found only 2 groups in the 25-75% empirical win-rate band with cxx-v3 error at least 0.25, both false-safe. The worst current group had actual attacker win rate 30.00% and cxx-v3 prediction 99.51% against a fort-3/castle/moat town with a visiting defender, large defender mana advantage, and town pre-merge participation loss.
- a later no-fit diagnostic snapshot on the live schema5 town-hero run at 71 complete shards / 3550 complete-shard rows kept 71 town groups. Current cxx-v3 was 54.93% accurate with Brier 0.3256, 26 false-safe groups, and 4 false-unsafe groups. Corrected deployed-danger was better but still insufficient: factor 1.0 reached 74.65% accuracy / Brier 0.2128 with 7 false-safe and 5 false-unsafe groups; factor 1.5 reduced false-safe groups to 2 but left 8 false-unsafe groups; factor 2 removed false-safe groups only by marking nearly everything unsafe and dropping accuracy to 66.20%. The same snapshot found 4 close-even cxx-v3 misses in the 25-75% empirical win-rate band with error at least 0.25. False-safe segments were dominated by visiting-hero sieges with defender spellbooks and defender combat-spell advantage: 19 false-safe groups / 950 rows with defender combat-spell advantage of at least 4 averaged only 2.32% attacker win rate while cxx-v3 averaged 94.74%. This remains incomplete-run evidence, but it strongly confirms that town/siege needs separate modeling or runtime simulation.
- compact town safety-guard probing on the same 71 complete-shard snapshot did not produce a mergeable rule. The best zero-false-safe candidate, deployed-danger factor 1.75 plus a defender-combat-spell-advantage guard, reached only 69.01% overall accuracy and left 14 false-unsafe groups while marking only 6/71 groups safe. Attacker-spell overrides improved accuracy to about 80% in the current snapshot but reintroduced 6-7 false-safe groups. This argues against a small scalar/rule patch as the path to 95% town/siege prediction.
- the live schema5 town-hero run at 84 complete shards / 4200 complete-shard rows reinforces the same conclusion. Current cxx-v3 was 52.38% accurate with Brier 0.3397, 31 false-safe groups, and 5 false-unsafe groups. Corrected deployed-danger factor 1.0 was better but still unsafe at 76.19% accuracy / Brier 0.2008 with 9 false-safe and 5 false-unsafe groups. Raising the factor to 2.0 removed false-safe groups only by allowing 5/84 groups as safe and leaving 17 false-unsafe groups. Compact guard probing did not find a mergeable town rule: the best zero-false-safe prototypes were about 70.24% accurate and allowed only 7/84 groups as safe.
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
- the current Nullkiller runtime hook is a final movement gate in `AIGateway::moveHeroToTile`: it can reject a battle that static planning already chose, but it cannot recover false-unsafe opportunities that were filtered out earlier by `isSafeToVisit`. The first end-to-end V3+runtime A/B test should therefore be interpreted primarily as a false-safe reduction test unless we also loosen or replace the planner-side static safety checks.
- `battlePredictionSimulationPlanningSafeAttackRatio` is the first narrow planner-side relaxation. It defaults to 0, which keeps existing behavior. When V3 runtime simulation samples are enabled and this setting is positive, offensive capture/gather planning uses this ratio instead of the normal `safeAttackRatio`, while defense, escape, and threat-map checks keep the normal ratio. The conservative first A/B value is 1.0 so more attack candidates reach the final runtime simulator gate without broadly weakening defensive reasoning.
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
- `BattleSimulationSummary`, `BattleSimulationRecordResult`, and `BattleSimulationEvaluation` live in `BattleSimulationResult`, separate from the JSONL batch collector. `BattleSimulationEvaluation` exposes the safety outputs needed by the fallback plan: empirical attacker win rate, likely win, probability-safe, all-attacker-wins, all-defender-wins, and Wilson lower-bound safety.
- `BattleSimulationReplaySession` separates repeated-sample bookkeeping from dataset writing: sample limit, recorded summary, and initial hero mana restoration are now reusable outside `BattleSimulationBatch`.
- `BattleSimulationSeed` provides stable deterministic sample seed derivation from game seed, player, hero, target, battle type, turn, sample index, and evaluator version, including reproducible per-sample seed lists for parallel execution.
- `BattleSimulationLocalGameServer` provides the isolated server sink needed by a future cloned simulation handler: state-changing packs are applied to the local `CGameState`, while network-only sends are suppressed.
- `CGameState::cloneForSimulation` clones game state through the in-memory serializer using load-time restoration. `BattleSimulationIsolatedState` wraps that clone path and remaps `BattleStartInfo` object pointers by ID into the cloned state.
- `BattleSimulationRequest` and `BattleSimulationResponse` name the future server-owned evaluator boundary: battle setup, deterministic seed context, sample count, thresholds, response status, summary, interpreted evaluation, and small validity/completion checks.
- `BattleSimulationSetup` builds deterministic `BattleSimulationRequest` instances from the same visit context Nullkiller reasons about: attacking hero, target object, current calendar day, stable battle type, default layout, state fingerprint, and seed context. It mirrors field battles, hero-in-town delegation, and town/siege defender selection, including outside-town battles.
- `BattleSimulationEvaluator` is owned by `BattleProcessor` and exposed through `BattleProcessor::evaluateBattleSimulation`. It validates requests, returns exact or composable cached summaries, and falls through to an injected `IBattleSimulationRunner` when cache-only evaluation can not answer. The runner is injected through `CGameHandler::setBattleSimulationRunner` so AI-linked code can provide BattleAI/MMAI execution without making `vcmiservercommon` depend directly on AI object libraries.
- `BattleProcessor::storeBattleSimulationSummary` exposes the cache population side of the boundary, so future simulation workers can publish deterministic sample summaries without exposing the evaluator internals.
- `CGameHandler::evaluateBattleSimulationForVisit` wraps visit-context request construction and cached evaluation in one server-owned call. Unsupported visit setups return `INVALID_REQUEST`, so Nullkiller integration does not need to duplicate town/siege setup rules or silently simulate unsafe cases.
- `BattleSimulationCache` stores simulation summaries by explicit state fingerprint plus deterministic seed/sample context. Requests require a non-zero state fingerprint so future runtime callers do not accidentally cache by object pointer identity.
- `BattleSimulationFingerprint` provides deterministic normalized battle-start cache fingerprints. `BattleSimulationRequest::effectiveStateFingerprint` uses an explicit request fingerprint when present and otherwise derives one from `BattleStartInfo`.
- `BattleSimulationEvaluator` can compose a complete N-sample response from cached one-sample entries for the requested deterministic seed range. This lets future parallel simulation workers publish independent samples while the planner consumes a single aggregate response.
- Schema5 battle setup captures pre-merge town siege state before `CGTownInstance::mergeGarrisonOnSiege`: town army snapshot, defending hero army snapshot, and the IDs needed to match the next started battle. The isolated runtime runner can now support visiting-hero inside sieges by applying that merge only inside the cloned game state and recomputing layout before battle start.
- `CClient::evaluateBattleSimulationForVisit` provides the current runtime Nullkiller evaluator by cloning the client's mirrored `CGameState`, remapping the battle setup into the clone, and running an isolated MMAI-backed simulation runner. This is linked through `vcmiclientcommon`'s existing dependency on `vcmiservercommon`, not through the Nullkiller2 AI object library.
- Nullkiller's `battlePredictionSimulationSamples` final movement gate currently calls through `CCallback` to `IClient::evaluateBattleSimulationForVisit`. This setting defaults to 0, so normal games use static danger only. When samples are configured, the AI logs bounded incomplete-request diagnostics and aggregate status counters, so A/B runs do not silently look like they are using runtime simulation.
- `AI/Nullkiller2/tools/compare_battle_predictors.py` can now temporarily enable `battlePredictionSimulationSamples` through `--config-replace` and records `runtimeBattleSimulation` totals by model in `summary.json`. Use `--require-runtime-simulation candidate` for V3 runtime A/B runs so the script fails if candidate games do not show simulation requests or if fewer than 90% of those requests complete.
- Runtime target selection now mirrors server-side request eligibility more closely: final-gate simulation is only attempted for neutral/enemy armed objects with actual stacks, enemy defended towns, or valid guards. This avoids counting unguarded reward objects, friendly blocking visits, and battle-marked movement to non-simulatable visitable objects as failed runtime simulation requests.
- Runtime stats also count `skippedNoTarget`: movement steps that may start a battle while runtime simulation is enabled but where target selection finds no enemy/neutral armed object, defended enemy town, or valid guard. This keeps real requests clean while still showing whether the planner is generating battle-like movement that cannot be simulated.

Example 250-game V3+runtime simulation A/B command:

```bash
python3 AI/Nullkiller2/tools/compare_battle_predictors.py \
  --comparison-mode color-swap \
  --random-map \
  --randommap-size S \
  --randommap-levels 2 \
  --randommap-water none \
  --randommap-players 2 \
  --samples 125 \
  --legacy-ai Nullkiller2 \
  --candidate-ai Nullkiller2V3 \
  --testdays 28 \
  --adjudicate-testdays \
  --require-runtime-simulation candidate \
  --config-replace config/ai/nk2ai/nk2ai-settings.json \
    '"battlePredictionSimulationSamples" : 0' \
    '"battlePredictionSimulationSamples" : 15' \
  --config-replace config/ai/nk2ai/nk2ai-settings.json \
    '"battlePredictionSimulationPlanningSafeAttackRatio" : 0' \
    '"battlePredictionSimulationPlanningSafeAttackRatio" : 1.0'
```

Batch collection caveat: when running `vcmibattlesim` with MMAI in parallel, each shard needs an isolated XDG config/cache profile. A shared profile can be rewritten by clients and silently disable the MMAI mod for later shards. Use `--xdg-config-template` and, if needed, `--xdg-profile-root` so each client starts from the same active-mod configuration.

The MMAI model payload must also be visible to the current checkout. A profile that names `MMAI` as the combat AI is not sufficient: logs must show `Parsing mod: OK (mmai)`, `Loading mod: OK (mmai)`, and `MMAI version 13 initialized` without fallback/config-error lines. On the 2026-07-11 remote runner, `/root/vcmi-nk-ratio-src/Mods` did not contain `mmai`, while the model payload was present in `/root/vcmi-predict-battle-outcome-src/Mods/mmai`. The working one-time setup was:

```bash
ln -s /root/vcmi-predict-battle-outcome-src/Mods/mmai /root/vcmi-nk-ratio-src/Mods/mmai
```

Example schema4 town-hero collection command:

```bash
mkdir -p /root/vcmi-nk-ratio-results/schema4-mmai-town-hero-20k-20260711

cd /root/vcmi-nk-ratio-build/bin

vcmibattlesim \
  --client ./vcmiclient \
  --generate-map \
  --generated-mode town-hero \
  --output-dir /root/vcmi-nk-ratio-results/schema4-mmai-town-hero-20k-20260711 \
  --battles 20000 \
  --shards 400 \
  --jobs 8 \
  --seed 20260711 \
  --combat-ai MMAI \
  --xdg-config-template /root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711/profiles/shard-00000/config \
  --skip-complete-shards
```

Paused remote schema4 run:

- output: `/root/vcmi-nk-ratio-results/schema4-mmai-town-hero-20k-20260711`
- started after a 4-row schema4 smoke passed strict validation with 4/4 MMAI-initialized shard logs and 0 fallback lines
- early live shards `00000` through `00007` parsed `mmai` and initialized both MMAI side models
- this run predates schema5 and does not contain `townPreMergeState`
- paused on 2026-07-11 at 2944 rows / 55 complete shards to free CPU for the schema5 collector; complete shards remain usable and resumable with `--skip-complete-shards`

Current live remote schema5 run:

- source/build: `/root/vcmi-schema5-src` and `/root/vcmi-schema5-build`, source revision marker `7303cd79d`
- output: `/root/vcmi-nk-ratio-results/schema5-mmai-town-hero-20k-20260711`
- PID: `256999`
- command wrapper: `/root/vcmi-nk-ratio-results/schema5-mmai-town-hero-20k-20260711/run-jobs8.sh`
- started after a 4-row schema5 smoke passed strict validation with 4/4 MMAI-initialized shard logs, 0 fallback lines, and `townPreMergeState` present in every row
- initial partial validation on the live run passed with 173/173 schema5 town-hero rows, 173/173 pre-merge snapshots, rich fields present, and 0 MMAI fallback lines
- restarted on 2026-07-11 from jobs=2 to jobs=8 after confirming completed shards are skipped exactly and incomplete shards are deterministically rerun from clean profiles with output truncation
- shard clients currently exit 139 after writing their complete rows and cleanly logging `Client stopped`; `vcmibattlesim` accepts the shard when the row count is complete. Treat this as a shutdown issue to investigate separately, not as invalid MMAI label evidence by itself.
- live validation snapshot on 2026-07-11: 3043 schema5 town-hero rows, 64 shard logs with MMAI initialized, 0 MMAI fallback lines, and 56 complete shards / 2800 complete-shard rows usable for `--complete-shards-only` analysis
- later live validation snapshot on 2026-07-11: 5799 schema5 town-hero rows, 121 shard logs with MMAI initialized, 0 MMAI fallback lines, and 109 complete shards / 5450 complete-shard rows usable for complete-shard analysis. All rows were `town-hero` with visiting defenders; fort levels were represented across 0-3.
- later live validation snapshot on 2026-07-11: 7468 schema5 town-hero rows, 153 shard logs with MMAI initialized, 0 MMAI fallback lines, 258 setup groups, and 153 shard groups. The live collector was still writing during this read-only check, so analysis scripts may see a few more rows depending on timing.
- latest complete-shard no-fit town snapshot while the run was still live: train/test split kept 112 complete groups / 5600 rows. Current cxx-v3 remained poor on the test split at 43.48% accuracy, Brier 0.4287, 10 false-safe groups, and 2 false-unsafe groups. Deployed-danger factor 1.0 improved test accuracy to 65.22% but still had 4 false-safe and 3 false-unsafe groups. Factor 2.0 removed test false-safe groups only by allowing 1/23 test groups as safe, so it is not a useful town/siege fix.
- latest fallback-only repeated-simulation proxy on the same live schema5 town data, using shard groups and all-wins safety: 8 MMAI samples was the first clean safety point on the held-out split, with 91.30% win/loss accuracy, 100% safety accuracy, Brier 0.0278, and 0 false-safe / 0 false-unsafe groups. Ten samples kept 91.30% / 100% with Brier 0.0205; 15 and 20 samples reached 95.65% / 100% with Brier 0.0095 and 0.0029 respectively. This is still a proxy over existing repeated rows, not an end-to-end runtime benchmark.
- latest close-even town diagnostics at 114 complete groups / 5700 rows found 9 cxx-v3 misses in the 25-75% empirical win-rate band with error at least 0.25; 5 were false-safe. The worst close-even misses were bidirectional, from actual 70% predicted 0.07% to actual 30% predicted 99.51%, reinforcing that a static scalar threshold cannot fix town/siege prediction.
- runtime gate smoke after filtering target eligibility: a 5-pair generated-map run with `battlePredictionSimulationSamples = 3` and `--require-runtime-simulation candidate` passed with 4/4 candidate runtime requests complete, 0 incomplete, 0 invalid, 2 safe, and 2 rejected. The preceding repro had 11 requests with 8 invalid due to unguarded reward objects; the intermediate stack-count-only fix left one invalid friendly/blocking hero visit, which the relation-aware filter removed. Follow-up 5-pair smokes with 8 and 15 samples also passed with no incomplete or invalid runtime requests; 8 samples completed 6/6 requests and 15 samples completed 5/5 requests. The next statistically meaningful A/B should use 15 samples because current town proxy data first crossed 95% win/loss accuracy at 15 samples.
- refreshed live schema5 snapshot at 122 complete groups / 6100 complete-shard rows: cxx-v3 test accuracy was 44.00%, Brier 0.4169, with 11 false-safe and 2 false-unsafe groups. Fallback-only repeated simulation with all-wins safety stayed strong: 8 samples reached 92.00% win/loss accuracy, 100% safety accuracy, Brier 0.0256; 15 samples reached 96.00% / 100%, Brier 0.0087; 20 samples reached 96.00% / 100%, Brier 0.0026.
- refreshed live schema5 snapshot at 145 complete shard groups: cxx-v3 test accuracy remained weak at 40.62%, Brier 0.4104, with 12 false-safe and 5 false-unsafe groups. Fallback-only repeated simulation with all-wins safety remained the best proxy: 8 samples reached 93.75% win/loss accuracy, 100% safety accuracy, Brier 0.0204; 10 samples stayed 93.75% / 100%, Brier 0.0148; 15 and 20 samples reached 96.88% accuracy with Brier 0.0069 and 0.0021 respectively. There were still 0 false-safe groups at 15/20 samples, but one false-unsafe group, so the all-wins rule is conservative rather than over-aggressive.
- refreshed static town miss diagnostics at 146 shard groups / 7300 filtered rows: cxx-v3 was 54.11% accurate overall, Brier 0.3420, with 53 false-safe and 13 false-unsafe groups. Deployed-danger factor 1.0 reached 75.34% accuracy but still had 14 false-safe groups; factor 2.0 removed false-safe groups only by leaving 34 false-unsafe groups and 9 safe groups. Close-even cxx-v3 misses remained bidirectional, again ranging from actual 70% predicted 0.07% to actual 30% predicted 99.51%. This continues to argue against scalar town danger tuning.
- after adding `skippedNoTarget`, a 20-pair generated-map smoke with `battlePredictionSimulationSamples = 15`, planning safe attack ratio `1.0`, and `--require-runtime-simulation candidate` passed: 40/40 games valid, candidate 22-18 by games, 3 candidate sweeps vs 1 legacy sweep, 15/15 runtime requests complete, 0 incomplete, 0 invalid, 8 safe, 7 rejected, and 217 skipped-no-target movements.
- the completed 250-pair runtime A/B on the remote idle build, output `/root/vcmi-nk-ratio-results/runtime-ab-250-samples15-planner1-20260711`, used samples 15, planning safe attack ratio 1.0, jobs 2, and runtime-simulation requirement enabled. It was neutral as an end-to-end strength test: 500/500 games valid, candidate 251-249 by games, candidate win-rate 50.20% with Wilson 95% interval 45.83%-54.56%, 38 candidate sweeps vs 37 legacy sweeps with 175 splits, one-sided candidate-better sign-test p 0.5, and two-sided p 1.0. Runtime evidence was active and nearly clean: 257 requests, 256 complete, 1 incomplete/invalid, 165 safe, 91 rejected, and 2946 skipped-no-target movements. The invalid case was a battle-marked move to a town visitable object that could not build a request, now handled by the stricter runtime target filter. This result does not prove an end-to-end improvement; the current runtime hook is better viewed as a safety gate than a strength gain until planner-side battle selection uses the simulator earlier.
- after the stricter battle-marked target filter, a seed-specific smoke reproducing the 250-pair invalid sample seed (`100114`) passed with 2/2 games valid, 1/1 runtime request complete, 0 incomplete, 0 invalid, 1 safe, and 7 skipped-no-target movements.
- later live validation snapshot on 2026-07-11: 8930 schema5 town-hero rows, 181 shard logs with MMAI initialized, 0 MMAI fallback lines, 181 shard groups, and all rows were visiting-defender town battles. Complete shards through shard 172 were stable at 50 rows each; later shards were still partial.
- latest fallback-only repeated-simulation proxy at 173 complete shard groups / 8650 complete-shard rows is the best predictor result so far. With an all-wins safety rule, 8 and 10 MMAI samples reached 94.59% win/loss accuracy and 100% safety accuracy with Brier 0.0182 and 0.0135 respectively. Fifteen and twenty samples reached 97.30% win/loss accuracy with Brier 0.0064 and 0.0020; both had 0 false-safe groups and 1 conservative false-unsafe group. This is still an offline proxy over repeated battle rows, not proof that Nullkiller wins more games end to end.
- refreshed static town miss diagnostics at 174 complete shard groups / 8700 filtered rows: cxx-v3 was 55.17% accurate overall, Brier 0.3358, with 66 false-safe and 13 false-unsafe groups. Deployed-danger factors were not enough: factor 1.0 reached 74.71% accuracy but still had 19 false-safe groups; factor 1.5 still had 5 false-safe groups; factor 2.0 removed false-safe groups only by collapsing to 13 safe groups and 39 false-unsafe groups. Close-even misses remained bidirectional, from actual 70% predicted 0.07% to actual 30% predicted 99.51%, so scalar town tuning is not a credible fix.
- refreshed fallback proxy at 181 complete shard groups / 9050 complete-shard rows remained strong. The held-out split kept 39 groups: static cxx-v3 was 41.03% accurate with Brier 0.4113, while fallback-only all-wins simulation reached 94.87% accuracy and 100% safety accuracy at 8/10 samples, and 97.44% accuracy at 15/20 samples. Fifteen samples had Brier 0.0061, 0 false-safe groups, and 1 conservative false-unsafe group; twenty samples had Brier 0.0019 with the same safety result.
- refreshed static town miss diagnostics at 181 complete shard groups / 9050 filtered rows: cxx-v3 was 55.80% accurate overall, Brier 0.3309, with 68 false-safe and 13 false-unsafe groups. Deployed-danger factor 1.0 reached 75.14% accuracy but still had 19 false-safe groups; factor 1.5 still had 5 false-safe groups; factor 2.0 removed false-safe groups only by leaving 13 safe groups and 42 false-unsafe groups. The close-even misses and segment profile stayed bidirectional, so the next runtime candidate moves simulation earlier into planning rather than only vetoing the final movement step.
- implementation update: runtime simulation target eligibility is now shared between the final movement gate and capture planning. For v3 with simulation samples enabled, capture planning can rescue same-turn, current-army paths that static danger marks unsafe if the route's non-target path danger is already statically safe and deterministic repeated simulation says the target battle is safe; future-turn and projected-army chain paths still use static planning because the current simulation API cannot represent their future army state. The A/B tool now parses `planningAccepted`, `planningRejected`, and `planningIncomplete` counters from logs, and can require a minimum number of completed planner-side simulation decisions with `--min-runtime-simulation-planning-decisions`.
- refreshed validation at 200 live shard groups / 9837 parsed rows stayed clean: all rows were schema5 `town-hero`, all 200 logs had MMAI initialized, and there were 0 fallback lines. Complete 50-row shard groups were 192 at the time of the fallback proxy and 193 by the subsequent static miss report.
- refreshed fallback proxy at 192 complete shard groups: static cxx-v3 held-out accuracy was 40.48% with Brier 0.4098, while fallback-only all-wins simulation reached 95.24% accuracy at 8/10 samples and 97.62% at 15/20 samples. Fifteen samples had Brier 0.0056, 0 false-safe groups, and 1 conservative false-unsafe group; twenty samples had Brier 0.0018 with the same safety result.
- refreshed runtime-policy check at 198 live shard groups / 10071 parsed rows: fallback-only all-wins with 20 samples reached 97.67% held-out accuracy, Brier 0.0017, 0 false-safe groups, and 1 conservative false-unsafe group. Fallback-only probability/Wilson with the same samples had the same accuracy and Brier but introduced 1 false-safe group from a 19/20 sample case whose empirical shard win rate was 93.33%. Nullkiller2 runtime and planner rescue now share an all-wins safety predicate so deployed decisions match the best observed safety policy.
- refreshed static town miss diagnostics at 193 complete shard groups / 9650 filtered rows: cxx-v3 was 56.99% accurate overall, Brier 0.3242, with 70 false-safe and 13 false-unsafe groups. Deployed-danger factor 1.0 reached 75.13% accuracy but still had 21 false-safe groups; factor 1.5 still had 5 false-safe groups; factor 2.0 removed false-safe groups only by leaving 14 safe groups and 42 false-unsafe groups. Syntax-only validation passed locally for `AIUtility.cpp`, `AIGateway.cpp`, and `CaptureObjectsBehavior.cpp` in a minimal Nix shell with Boost/TBB headers, but a full linked build and runtime smoke for the planner rescue remain pending.

Then validate and inspect the schema5 model failures:

```bash
python3 scripts/battle_prediction/validate_battle_dataset.py \
  /root/vcmi-nk-ratio-results/schema5-mmai-town-hero-20k-20260711 \
  --expected-rows 20000 \
  --expected-schema 5 \
  --expected-shards 400 \
  --expected-shard-size 50 \
  --group-key shard \
  --expected-groups 400 \
  --require-complete-shards \
  --require-battle-types town-hero \
  --require-no-mmai-fallback \
  --require-mmai-initialized \
  --require-schema3-rich-fields

python3 scripts/battle_prediction/evaluate_nullkiller_predictor.py \
  /root/vcmi-nk-ratio-results/schema5-mmai-town-hero-20k-20260711 \
  --scope town \
  --group-key shard \
  --complete-shards-only \
  --l2 0.03 \
  --print-near-even 40 \
  --print-worst 40 \
  --print-v3-false-safe 40 \
  --print-v3-false-unsafe 40 \
  --print-town-deployable-false-safe 40 \
  --print-town-deployable-false-unsafe 40 \
  --town-deployable-safe-probability 0.62

python3 scripts/battle_prediction/report_v3_static_misses.py \
  /root/vcmi-nk-ratio-results/schema5-mmai-town-hero-20k-20260711 \
  --scope town \
  --group-key shard \
  --complete-shards-only \
  --limit 40 \
  --segments 40 \
  --town-danger-factors 1.0,1.25,1.5,1.75,2.0 \
  --probe-town-guards \
  --guard-results 40

python3 scripts/battle_prediction/analyze_v3_failure_segments.py \
  /root/vcmi-nk-ratio-results/schema5-mmai-town-hero-20k-20260711 \
  --scope town \
  --group-key shard \
  --complete-shards-only \
  --predictor cxx-v3 \
  --actual-min 0.25 \
  --actual-max 0.75 \
  --error-min 0.25 \
  --print-groups 40
```

Next schema6 collection should use the same generated `town-hero` setup. A real schema6 remote build is available at `/root/vcmi-schema6-real-build`; do not use `/root/vcmi-schema6-build` for collection because that directory was configured against `/root/vcmi-schema5-src` and emitted schema5 rows. The real build was configured from `/root/vcmi-schema6-src` with `ENABLE_VIDEO=OFF`, `ENABLE_EDITOR=OFF`, `ENABLE_LAUNCHER=OFF`, and `ENABLE_DISCORD=OFF`, then linked `Data` and `Maps` in `bin/` to the same data paths as the working schema5 build.

A strict 4-row smoke passed on 2026-07-11:

- output: `/root/vcmi-nk-ratio-results/schema6-mmai-town-hero-smoke-real-20260711`
- command used `/root/vcmi-schema6-real-build/bin/vcmibattlesim` and `/root/vcmi-schema6-real-build/bin/vcmiclient`
- validation: 4/4 rows schema6, 4/4 `town-hero`, 4/4 complete shards, 4/4 logs with MMAI initialized, 0 MMAI fallback lines, and `--require-schema3-rich-fields` passed
- row inspection: the first row had 12 `battleStartStacks`, 1 `battleStartObstacles`, and `townPreMergeState`

A remote watcher is running as PID `286543` with log `/root/vcmi-nk-ratio-results/schema6-mmai-town-hero-20k-20260711-watcher.log`. It waits for schema5 PID `256999` to exit, validates schema5 with 20000 expected rows, schema 5, 400 complete shards, MMAI initialization, no fallback lines, and rich fields, then starts the schema6 20000-row town-hero collection from `/root/vcmi-schema6-real-build/bin`. As of a lightweight remote check at 9429 schema5 rows, schema5 was still running and schema6 had not started.

Validate schema6 data with the same gate but `--expected-schema 6`; `--require-schema3-rich-fields` now also requires `battleStartStacks` and `battleStartObstacles` on schema6 rows. The rich town prototype and segment analyzer consume schema6 start-stack and obstacle aggregates automatically and emit `battle_start_*` features/segments:

```bash
cd /root/vcmi-schema6-real-build/bin

./vcmibattlesim \
  --client ./vcmiclient \
  --generate-map \
  --generated-mode town-hero \
  --output-dir /root/vcmi-nk-ratio-results/schema6-mmai-town-hero-20k-20260711 \
  --battles 20000 \
  --shards 400 \
  --jobs 8 \
  --seed 20260711 \
  --combat-ai MMAI \
  --xdg-config-template /root/vcmi-nk-ratio-results/schema3-richstats-mmai-town-hero-5k-20260711/profiles/shard-00000/config \
  --skip-complete-shards
```

```bash
python3 scripts/battle_prediction/validate_battle_dataset.py \
  /root/vcmi-nk-ratio-results/schema6-mmai-town-hero-20k-20260711 \
  --expected-rows 20000 \
  --expected-schema 6 \
  --expected-shards 400 \
  --expected-shard-size 50 \
  --group-key shard \
  --expected-groups 400 \
  --require-complete-shards \
  --require-battle-types town-hero \
  --require-no-mmai-fallback \
  --require-mmai-initialized \
  --require-schema3-rich-fields
```

The schema4 run can still be validated and inspected for wall-state-only evidence:

```bash
python3 scripts/battle_prediction/validate_battle_dataset.py \
  /root/vcmi-nk-ratio-results/schema4-mmai-town-hero-20k-20260711 \
  --expected-schema 4 \
  --group-key shard \
  --require-battle-types town-hero \
  --require-no-mmai-fallback \
  --require-mmai-initialized \
  --require-schema3-rich-fields

python3 scripts/battle_prediction/evaluate_nullkiller_predictor.py \
  /root/vcmi-nk-ratio-results/schema4-mmai-town-hero-20k-20260711 \
  --scope town \
  --group-key shard \
  --complete-shards-only \
  --l2 0.03 \
  --print-near-even 40 \
  --print-worst 40 \
  --print-v3-false-safe 40 \
  --print-v3-false-unsafe 40 \
  --print-town-deployable-false-safe 40 \
  --print-town-deployable-false-unsafe 40 \
  --town-deployable-safe-probability 0.62

python3 scripts/battle_prediction/analyze_v3_failure_segments.py \
  /root/vcmi-nk-ratio-results/schema4-mmai-town-hero-20k-20260711 \
  --scope town \
  --group-key shard \
  --complete-shards-only \
  --predictor cxx-v3 \
  --actual-min 0.25 \
  --actual-max 0.75 \
  --error-min 0.25 \
  --print-groups 40
```

Interim schema4 complete-shard findings on 2026-07-11:

- `--group-key shard --complete-shards-only` kept 55 complete groups / 2750 rows from 2965 validated rows.
- cxx-v3 diagnostic town accuracy remained poor: train 52.17% / Brier 0.3357 with 16 false-safe groups; test 22.22% / Brier 0.5846 with 6 false-safe groups and 1 false-unsafe group.
- close-even cxx-v3 misses on complete shards narrowed to 2 groups / 100 rows in the 25-75% empirical win-rate window, both false-safe.
- simple deployed town danger multipliers are not enough: factor 1.5 removed train false-safes but still left 2 test false-safes; factor 2 removed test false-safes only by marking every test town group unsafe.
- this reinforces the current direction: do not deploy open-field cxx-v3 calibration to town/siege rows; use schema5 pre-merge data plus simulation labels to validate a separate town model or runtime simulation fallback.

Use the dataset validator before fitting or reporting numbers:

```bash
python3 scripts/battle_prediction/validate_battle_dataset.py \
  schema3-richstats-mmai-real-mixed-2k-combined-20260711.tar.gz \
  --expected-rows 2000 \
  --expected-schema 3 \
  --expected-shards 100 \
  --expected-shard-size 20 \
  --group-key shard \
  --expected-groups 100 \
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
