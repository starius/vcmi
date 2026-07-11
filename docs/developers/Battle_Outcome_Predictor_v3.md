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

End-to-end validation:

- run paired AI games on identical maps and seeds
- swap sides/colors between old and new predictor
- use deterministic AI seeds
- collect win rate, score, towns, heroes, army value, resources, turns survived, and crash/assertions
- compare old predictor vs new predictor with confidence intervals
- treat battle-level improvement and game-level improvement as separate evidence

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
- worst errors are repeated matchup/special-case failures, not just calibration threshold mistakes

The next likely useful model needs either a stronger non-linear model with better generalization evidence or a deterministic simulation fallback for high-impact uncertain battles. Another global ratio-only coefficient update is unlikely to reach the target by itself.

## Runtime Simulation Fallback Direction

Measured fallback behavior uses repeated MMAI outcomes as a proxy for running a battle several times at decision time. This is not a direct implementation yet, but it gives a target:

- use the static v3/rich model as a cheap first pass
- invoke battle simulation for high-impact decisions and for probabilities below a conservative safety cutoff, not for every object on every path
- use deterministic seeds derived from game seed, hero id, target object id, turn, and fallback sample index
- evaluate at least 5 samples for win/loss prediction; use 10+ samples or a stricter all-wins style rule for safety-sensitive attacks
- treat win/loss probability and safety as separate outputs:
  - use sampled win rate, possibly calibrated/shrunk, for expected-value decisions
  - use `all samples won` or a Wilson lower-bound threshold for safety-sensitive attacks, especially when the army loss or strategic exposure is high
- cache simulation results per `(hero army state, hero stats, target state, battle context, model seed)` so pathfinding does not replay the same battle repeatedly
- expose the fallback behind a setting until end-to-end AI games prove it improves outcomes

Architecture caveat: current quick combat/autofight goes through normal battle flow with server/client combat AI interfaces. There is no small in-process Nullkiller API that clones an arbitrary visible battle state and returns a deterministic win distribution. The next implementation step is therefore to build a reusable headless battle-evaluation service from the existing battle simulation batch path, not to call client quick combat directly from pathfinding.

## Runtime Simulation Service Plan

The runtime fallback should be implemented as a separate branch after the schema/tooling work is committed cleanly. Keep the experimental Nullkiller heuristic coefficient changes separate from the data-generation and analysis tools.

Implementation outline:

1. Extract the reusable parts of `BattleSimulationBatch` into a server-side battle-evaluation service that can run a fixed battle setup repeatedly and return aggregate counts, not JSONL-only side effects.
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

## Merge Strategy

Start with the safest mergeable step:

1. Ratio-only calibration or equivalent-danger calibration for open-field hero-v-monster and hero-v-hero battles.
2. Keep old heuristic fallback for towns, sieges, missing data, and unsupported contexts.
3. Add richer schema v3 collection.
4. Train town/siege-aware models.
5. Enable richer model behind a setting for A/B testing.
6. Promote to default only after offline and end-to-end metrics are both favorable.
