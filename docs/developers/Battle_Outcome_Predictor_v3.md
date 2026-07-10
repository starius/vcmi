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

## Merge Strategy

Start with the safest mergeable step:

1. Ratio-only calibration or equivalent-danger calibration for open-field hero-v-monster and hero-v-hero battles.
2. Keep old heuristic fallback for towns, sieges, missing data, and unsupported contexts.
3. Add richer schema v3 collection.
4. Train town/siege-aware models.
5. Enable richer model behind a setting for A/B testing.
6. Promote to default only after offline and end-to-end metrics are both favorable.
