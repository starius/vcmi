# Lua Nullkiller2 AI Implementation Plan

## Objective

Create a new adventure AI player named `LuaNullkiller2` whose strategic behavior is implemented in Lua and first
matches the current C++ `Nullkiller2` behavior as closely as practical. Only after parity is achieved should Lua
policy changes try to outperform `Nullkiller2`.

This is not a wrapper around `Nullkiller2`. The new AI must not fall back to `Nullkiller2`, execute native
`Nullkiller2` tasks, inherit from `NK2AI::AIGateway`, link the `Nullkiller2` target, or call `Nullkiller2`
business-logic helpers. Existing C++ game-rule validation and neutral compute services may be exposed to Lua, but
all AI choices must live in Lua.

The intended maintenance model is a 1-to-1 Lua/C++ implementation pair. When a future change lands in the C++
`Nullkiller2` implementation, it should be obvious where the matching Lua change belongs. When a Lua parity fix
finds a C++ bug or desirable cleanup, it should be equally easy to mirror that change back to C++.

## Current Findings

The `/home/user/vcmi/script-ai` branch has useful infrastructure, but it is not the target architecture:

- Useful: Lua runner infrastructure, JSON input/output conversion, imperative `ai` facade ideas, checked host
  action execution, visible-state snapshots, event/update journaling, trace tools, batch-evaluation scripts, and AI
  registration patterns.
- Not usable as-is: `AI/ScriptedAdventure/CScriptedAdventureAI` derives from `NK2AI::AIGateway`, includes many
  `AI/Nullkiller2` headers, links `Nullkiller2`, owns native task handles, exposes bounded native task helpers, and
  has explicit `fallbackToNullkiller` behavior.
- The existing script candidates, especially `boundedNullkillerControl.lua`, are valuable regression and tracing
  material, but they intentionally compose bounded native `Nullkiller2` work. They are not a Lua reimplementation.

The current `Nullkiller2` surface is large enough to require a staged port. A quick source inventory shows about
24k lines under `AI/Nullkiller2`, with roughly 21k lines in the engine, behavior, goal, analyzer, helper, marker,
and pathfinding areas that affect decisions.

## Dependency Boundary

Treat these as Lua-owned business logic:

- turn loop structure and pass control
- hero roles, hero/resource locks, target memory, and scan-depth policy
- behavior decomposition and goal/task construction
- priority scoring, thresholds, fuzzy heuristics, and tie-breaking
- object value, reward, conquest, danger/risk classification, and army-loss policy
- town building, recruitment, trading, army gathering, artifact optimization, hero dismissal, and query choices
- hero-chain decisions, special adventure spell choices, and object interaction policy
- surrender/retreat decisions that belong to the adventure AI

Treat these as allowed C++ host or compute services when exposed through neutral APIs:

- read-only visible game-state snapshots from callbacks
- stable constants, identifiers, schemas, and localization-independent metadata
- checked execution of exact player actions requested by Lua
- request/result waiting, event journaling, save/load of Lua memory, and trace writing
- generic pathfinding over visible/known map state when Lua supplies the actors, options, and targets
- generic legal-action enumeration that reports what can be requested, without ranking strategic choices
- game-mechanics calculations such as movement cost, legal battle/action validation, resource affordability, and
  raw combat/army strength formulas already used outside AI policy

Do not expose C++ helpers that already contain `Nullkiller2` policy. Examples to avoid include native task
candidate generation, `PriorityEvaluator`, `BuildAnalyzer`, `HeroManager`, `ArmyManager`, `ObjectClusterizer`,
`DangerHitMapAnalyzer` policy outputs, `ResourceTrader`, `ArmyFormation` policy routines, `AIGateway` army/artifact
selection helpers, and `AIPathfinder` configurations that choose AI-specific special actions. If a helper is needed
for speed, first split out a neutral lower-level calculation and keep the decision formula in Lua.

## Target Architecture

Add a separate target:

```text
AI/LuaNullkiller2/
  CLuaNullkiller2AI.h
  CLuaNullkiller2AI.cpp
  LuaNullkiller2Host.*
  LuaNullkiller2State.*
  CMakeLists.txt

scripts/ai/nullkiller2/
  main.lua
  Engine/
    Nullkiller.lua
    Settings.lua
    PriorityEvaluator.lua
    FuzzyHelper.lua
    ResourceTrader.lua
    State.lua
  Goals/
  Behaviors/
  Analyzers/
  Helpers/
  Markers/
  Pathfinding/
  Actions/
  Queries/
  Util/
```

`CLuaNullkiller2AI` should derive directly from `CAdventureAI`, not from `NK2AI::AIGateway`. It should own a Lua
runner, host state, request tracking, query tracking, and Lua memory. It should not have a `std::unique_ptr` to any
native `Nullkiller2` class.

The C++ host should provide:

- script loading and reload settings
- `runDay(ai, input)` and narrow callback hooks, including battle preservation policy
- visible-state and update snapshots
- exact checked action commands such as move, visit, build, recruit, trade, transfer, answer query, cast spell, dig,
  dismiss, artifact operations, and end turn
- neutral inspect calls such as `getHero`, `getTown`, `getObject`, `getReachable`, `getPath`, and legal option lists
- trace events for every Lua decision, host command, refresh, failure, and final day status

The Lua package should mirror `Nullkiller2` concepts using data records rather than C++ inheritance:

- `Engine/Nullkiller.lua`: equivalent of `Nullkiller::makeTurn`, `updateStateAndExecutePriorityPass`, task failure
  policy, and end-of-pass cleanup.
- `Engine/Settings.lua`: reads the same `config/ai/nk2ai/nk2ai-settings` values through host-provided JSON.
- `Engine/State.lua`: owns locks, active hero/target, scan depth, memory, and cached analysis.
- `Goals/`: goal and task records for the existing `EGoals` set, plus `decompose`, `accept`, equality/hash, affected
  objects, and hero-exchange counts.
- `Behaviors/`: `Startup`, `RecruitHero`, `BuyArmy`, `Building`, `CaptureObjects`, `Cluster`, `Defence`, `Escape`,
  `GatherArmy`, `Exploration`, and `StayAtTown`.
- `Engine/PriorityEvaluator.lua` and related modules: `PriorityEvaluator`, `RewardEvaluator`, fuzzy danger scoring,
  priority tiers, and deterministic sorting/tie-breaking.
- `Analyzers/`: Lua versions of build, hero, army, object-cluster, memory, and hit-map analysis.
- `Pathfinding/`: Lua-owned interpretation of route records, hero chains, special actions, object graph choices, and
  path task construction. Heavy route enumeration can call neutral host pathfinding.
- `Actions/`: exact action execution wrappers that call `ai.player:*` host commands.
- `Queries/`: level-up, blocking dialog, teleport, tavern, market, recruitment, garrison, university, artifact
  assembly, and map-object selection policies.

## Current Branch Status

The branch now has the initial standalone AI and parity infrastructure in place:

- `ENABLE_LUA_NULLKILLER2_AI`, the `AI/LuaNullkiller2` target, and `AIFactory` registration are present.
- `CLuaNullkiller2AI` derives directly from `CAdventureAI`, loads the Lua runner, passes visible turn and callback
  snapshots plus persistent Lua memory, stores returned Lua memory for later calls, and executes checked host
  commands without linking to or instantiating native `Nullkiller2`.
- The Lua runner loads `scripts/ai/nullkiller2/main.lua`, exposes settings, trace, command, and snapshot input,
  supports named entry points such as `runDay`, `commanderGotLevel`, `heroGotLevel`, `heroExchangeStarted`,
  `showBlockingDialog`, `showGarrisonDialog`, `showRecruitmentDialog`, `showTeleportDialog`, `showTavernWindow`,
  `showMarketWindow`, `showUniversityWindow`, and `showMapObjectSelectDialog`, records a command journal that is
  usable by differential tests, returns updated memory, and returns small decision fields for non-command callbacks
  such as surrender/retreat.
- Lua normalizes vector snapshots into `heroesByID` and `objectsByID` lookup tables before turn planning, so C++
  snapshots do not need duplicate indexed maps for task presence checks.
- `scripts/ai/nullkiller2/PORT_MAP.json` tracks mirrored C++ files and symbols, with audit coverage for forbidden
  native dependencies and unmapped/stale Lua policy files.
- Pure Lua tests, fixture-based differential smoke tests, and JSON replay fixtures run through
  `scripts/ai/nullkiller2/tests/run_lua_tests.py`.

The current Lua policy surface includes the core day loop, settings, state locks, task plan execution, priority
formula scaffolding, deterministic `RewardEvaluator` resource, reward, growth, cost, strategic, and conquest helpers,
hero-specific `AIUtility` artifact scoring, resource trading, goal records, marker records, priority-pass behaviors,
regular behavior decomposition, persistent `AIMemory` object-id sets, and command emission for recruit hero, build,
build boat, dismiss hero, swap garrison hero, recruit creatures, upgrade creatures, merge stacks, cross-army
merge/swap, split stack, dismiss creatures, cast spell, artifact swaps, granular hero movement, resource locks,
answer query, and end turn. `ExchangeSwapTownHeroes`
extraction now mirrors the `buildArmyIn` order through upgrades, recruitment, first-slot army correction, and
Lua-owned transfer command sequencing from snapshots. Turn snapshots export callback-derived `UpgradeInfo` data as
town and hero `upgradeSlots`; Lua `BuyArmy` and `ExchangeSwapTownHeroes` choose upgrade targets from those raw
candidates and emit `upgradeCreature` commands without calling native `AIGateway::makePossibleUpgrades`.
`Analyzers/ArmyManager.lua` now owns sorted-slot
consolidation, faction/morale best-army filtering, scout-unit choice, scout last-stack retention, dwelling purchase
selection, reinforcement purchase value, reinforcement transfer value, stack-power evaluation, total-army
aggregation, and hill-fort/dwelling upgrade calculation; the transfer sequencer emits the matching scout split
commands when a source army must keep one stack. The turn loop now invokes `GatewayPolicy.pickBestArtifacts` after
successful regular passes, and that policy owns first-pass artifact equip/swap sequencing from exported hero
artifact snapshots for empty legal equipment slots, higher-scoring replacement artifacts, and the displaced-artifact
backpack fallback when a direct swap is illegal. The `heroExchangeStarted` callback is wired through Lua, chooses
the same transfer direction as native `AIGateway`, emits Lua-owned army/artifact exchange commands, and answers the
pending query through the same host command journal. Map-object selection dialogs now route through Lua
`GatewayPolicy.chooseMapObjectSelection` before emitting `answerQuery`. Garrison dialogs route through Lua
`GatewayPolicy.shouldUseGarrisonTroops` and the Lua army-transfer sequencer before answering. Recruitment dialogs
route dwelling and destination-army snapshots through `GatewayPolicy.chooseDwellingRecruitment`, including the
native duplicate-stack merge-before-recruit case and full resource-vector affordability. The `heroMoved`,
`tileHidden`, `tileRevealed`, `newObject`, `heroVisit`, `objectRemoved`, and `objectPropertyChanged` event
callbacks now route through Lua and mutate persistent `AIMemory` object-id sets while the host keeps raw pathfinder
invalidation as a non-policy state flag; object removal, owner changes, and setting-gated tile reveal updates also
record the native hitmap/tile-owner reset markers in Lua memory. Lightweight native state callbacks for artifact
movement, resource/creature availability, hero stat changes, garrisons, buildings, adventure spell casts, and request
realization now route through Lua status-memory entry points. The `playerBlocked`, `heroCreated`, `battleStart`,
`battleEnd`, `battleResultsApplied`, and `battleEnded` event callbacks update Lua-owned status memory for
battle/movement state while preserving host-side base battle notifications and raw pathfinder invalidation.
Surrender/retreat
decisions now return from Lua `GatewayPolicy.makeSurrenderRetreatDecision` and are converted to `BattleAction`
only at the host boundary. Blocking dialogs route component snapshots through Lua selection policy; danger-aware
yes/no parity still needs richer object and danger snapshots. Teleport dialogs route exit snapshots through Lua
selection policy; destination/probing memory parity is still thinner than native `AIGateway`. Fixed-answer
commander, tavern, market, and university queries are represented as Lua entry points that emit `answerQuery(0)`.
Hero level-up secondary-skill choice uses a Lua port of the native `HeroManager` score maps, role-map update, and
main/scout selection rules from visible hero/town snapshots; exact fighting-strength order still needs richer
speciality bonus snapshots. `ExecuteHeroChain` now executes composite, Dimension Door, adventure-spell, Build Boat,
and explicit command special-action descriptors through Lua-owned primitive host commands, stale Dimension Door
recovery locks the hero and invalidates pathfinding like native `recoverStaleDimensionDoorAction`, and object-graph
shortcutting uses neutral live path-info fields to skip obsolete path nodes.

Major parity gaps remain:

- visible snapshots are still too thin for full analyzer, object, path, threat, query, and broader ArmyManager parity
- hero fighting-strength ranking still needs full bonus-derived speciality snapshots for exact main/scout ordering
- `ExecuteHeroChain` replays path nodes in native backward order, executes the first set of Lua-owned special-action
  descriptors, applies object-graph shortcutting, rejects stale zero-turn live path snapshots, and recovers stale
  Dimension Door plans, but siege formation, richer special-action descriptors, and live host validation are still
  incomplete
- cross-hero artifact legality breadth, full combined-artifact legality data, full Rewardable inspection, and richer
  live object inspection remain incomplete outside the deterministic scoring helpers and first-pass artifact
  equip/swap sequencing
- garrison, artifact, and remaining hero-exchange and army-transfer edge cases need complete Lua-owned sequencing
  plus host validators
- upgrade parity still needs native-vs-Lua differential traces for multi-step modded upgrade chains and unusual
  unavailable-upgrade sources; current coverage is unit/fixture level plus raw `UpgradeInfo` snapshot plumbing
- differential tests currently cover command journals, end-turn smoke, and JSON replay of normalized Lua decision
  fixtures; they do not yet compare real native `Nullkiller2` traces against Lua traces at each decision point

## Mirrored Structure and Naming

The Lua port should deliberately preserve the names and boundaries of the C++ implementation unless Lua syntax or
runtime constraints make that impractical. Readability for side-by-side comparison is more important than idiomatic
Lua abstraction at this stage.

Mirroring rules:

- Keep directory names aligned: `Engine`, `Goals`, `Behaviors`, `Analyzers`, `Helpers`, `Markers`, and
  `Pathfinding` in C++ map to matching Lua module groups under `scripts/ai/nullkiller2/`.
- Keep function names recognizable. For example, `Nullkiller::makeTurn` maps to `Nullkiller.makeTurn`,
  `updateStateAndExecutePriorityPass` keeps that exact Lua function name, and `PriorityEvaluator::evaluate` maps to
  `PriorityEvaluator.evaluate`.
- Keep enum and constant names stable. Lua tables should expose `HeroLockedReason.DEFENCE`,
  `ScanDepth.MAIN_FULL`, `PriorityTier.INSTAKILL`, and goal names matching the C++ identifiers.
- Keep task/goal fields close to C++ names: `priority`, `hero`, `town`, `objid`, `tile`, `bid`, `resID`,
  `goldCost`, `buildingCost`, and `affectedObjects`.
- Preserve control-flow shape before improving style. If C++ uses priority passes, behavior decomposition, task
  filtering, then action execution, the Lua version should have the same named stages.
- Add a comment with the source C++ symbol at the top of non-trivial Lua ports, such as
  `-- Mirrors AI/Nullkiller2/Engine/Nullkiller.cpp: Nullkiller::makeTurn`.
- Avoid merging multiple C++ concepts into one Lua module during parity work. Refactors can happen after parity, but
  only with tests that keep side-by-side trace comparison intact.

Add a generated port index, for example `scripts/ai/nullkiller2/PORT_MAP.md` or JSON, that maps every mirrored Lua
module/function to the C++ file and symbol it currently follows. The index should be updated with code changes and
used by review scripts to find unmapped C++ or Lua policy functions.

## Regression Test Infrastructure

Lua engine regression tests need to be convenient enough to run after every small port. They should not require a
full game unless the test specifically covers integrated game behavior.

Required layers:

- Pure Lua unit tests for deterministic modules: settings parsing, resources, goal equality/hash, task filtering,
  priority formulas, reward calculations, behavior decomposition from fixture snapshots, and query-choice policies.
- JSON fixture tests for host input/output: visible state snapshots, legal action options, route records, query
  records, and action results captured from real games and reduced to stable fixtures.
- Golden trace tests for turn slices: given the same fixture input, Lua should emit the same ordered trace events,
  task families, selected priorities, locks, and requested actions as the checked baseline.
- Integration smoke tests that run `LuaNullkiller2` through a short fixed generated-map scenario and verify it ends
  turns, answers mandatory queries, and does not call forbidden native helpers.
- Regression corpus tests that replay known discrepancy fixtures and previously fixed parity bugs.

Planned layout:

```text
scripts/ai/nullkiller2/tests/
  unit/
  fixtures/
    replay/
    discrepancies/
  golden/
  integration/
  run_lua_tests.py
  replay_lua_decisions.py
  update_golden.py
```

The Lua test runner should run without compiling VCMI when testing pure Lua modules. Tests that need host bindings
or real game execution run only in the remote build/test workflow. Golden updates must be explicit: normal test
runs compare against checked-in expected output and fail on drift.
`replay_lua_decisions.py` replays normalized JSON decision fixtures through `main.lua`, compares expected subsets of
status, command journals, trace, and memory, and writes actual JSON plus a first-difference summary under
`tests/fixtures/discrepancies/` when a replay drifts.

## Differential Testing

Differential testing is the main tool for maintaining 1-to-1 parity. It should compare C++ `Nullkiller2` and Lua
`LuaNullkiller2` at several levels, not only by final win/loss.

Required modes:

- Snapshot mode: run C++ `Nullkiller2` on a state and export normalized decision inputs, analyzer outputs, generated
  goals/tasks, priority contexts, selected task, locks, and requested action. Feed the same normalized snapshot to
  Lua and diff the same records.
- Lockstep mode: execute both AIs from the same seed/map with deterministic settings. After every pass or action,
  normalize and compare trace events. Stop at the first divergence with a compact explanation.
- Replay mode: take a previously captured C++ trace and replay each decision point through Lua without running a
  full game, useful for fast local Lua tests and reduced discrepancy fixtures.
- Tolerance mode: allow explicitly documented benign differences such as unordered equal-priority candidates or
  floating-point epsilon, while treating selected task/action/query-answer drift as a failure.
- Bisect mode: run the lockstep corpus across recent commits or fixture revisions to find the first change that
  introduced a Lua/C++ discrepancy.

Trace records must use stable machine fields rather than localized strings. Each compared decision should include
the C++ source symbol, Lua module/function, pass index, priority tier, task id/type, affected object ids, hero ids,
town ids, route/action ids, raw priority context, final priority, and requested action.

The differential harness should produce three artifacts:

- a human-readable first-difference summary
- normalized JSON traces for both sides
- a minimized fixture that can be checked into `scripts/ai/nullkiller2/tests/fixtures/discrepancies/`

The parity target is exact selected-behavior parity. If exact parity is blocked by missing host data or a neutral
helper gap, the discrepancy fixture stays in the corpus with an expected-failure marker and a linked TODO.

## Implementation Phases

### Phase 0: Inventory and Port Checklist

- Add a machine-readable checklist that maps every `AI/Nullkiller2` file and function to one of: Lua port, neutral
  C++ helper, host action executor, obsolete, or deferred.
- Start with `Nullkiller::makeTurn`, `Settings`, `AbstractGoal`, concrete goals, behaviors, analyzers,
  `PriorityEvaluator`, `AIGateway` policy routines, and pathfinding special actions.
- Add an audit script that fails if `AI/LuaNullkiller2` includes `AI/Nullkiller2`, links `Nullkiller2`, or exposes
  host commands named after native `Nullkiller2` task helpers.
- Add the port index and require every Lua policy function to reference the C++ symbol it mirrors.

### Phase 1: Host Skeleton

- Import only the neutral pieces from `script-ai`: Lua runner shape, JSON script contract, event journal, checked
  command transport, and trace tooling.
- Add `ENABLE_LUA_NULLKILLER2_AI` and register `LuaNullkiller2` in `AIFactory`.
- Implement `CLuaNullkiller2AI` directly on `CAdventureAI`.
- Build the minimal Lua facade: `ai.game`, `ai.player`, `ai.memory`, `ai.trace`, and `ai:refresh()`.
- Make failure behavior explicit: Lua errors are logged and traced, but control never transfers to `Nullkiller2`.
  The initial fail-closed behavior can end the turn only when no mandatory query or active operation is pending.

### Phase 2: Lua Data Model and Settings

- Expose stable identifiers and constants to Lua without localized strings as decision inputs.
- Load the same `nk2ai-settings` values for the active difficulty.
- Implement Lua records for resources, heroes, towns, objects, paths, goals, tasks, and priority tiers.
- Add deterministic comparison helpers so Lua ordering matches C++ ordering where possible.
- Add pure Lua unit tests for these records before porting behavior logic.

### Phase 3: Core Turn Loop Parity

- Port `Nullkiller::makeTurn` to `Engine/Nullkiller.lua`.
- Port resource locks, hero locks, scan depth, active target, object presence checks, task failure policy, and pass
  loop limits.
- Implement priority pass structure before ordinary adventure behavior selection.
- Add trace output that can be compared with native `Nullkiller2` traces at pass/task/action granularity.
- Add the first lockstep differential test for a one-pass no-op or end-turn scenario.

### Phase 4: Goal and Behavior Port

Port in this order, verifying each stage against native traces:

1. Goal/task base types and concrete action goals.
2. Priority pass: recruit hero, buy army, building.
3. Startup behavior.
4. Capture objects and object-visit tasks.
5. Defence and escape.
6. Gather army and army upgrades.
7. Exploration and cluster behavior.
8. Quest, boat, adventure spell, teleport, whirlpool, and hero-chain special cases.
9. Stay-at-town and cleanup behaviors if they are still reachable.

Each port should include small Lua fixtures for decomposition and task ranking, plus at least one fixed-map remote
trace comparison before moving on.

### Phase 5: Analyzer and Evaluator Port

- Port `BuildAnalyzer`, `HeroManager`, `ArmyManager`, `ObjectClusterizer`, `AIMemory`, danger/hit-map policy,
  `FuzzyHelper`, `RewardEvaluator`, and `PriorityEvaluator` into Lua.
- Keep raw mechanics in host helpers only where they are not decisions. For example, Lua may ask for reachable paths
  and raw battle/army strength, but Lua computes thresholds, risk classes, rewards, and final priority.
- Preserve C++ formulas exactly first, including quirks and thresholds. Improvement work comes later.
- Add fixture-level differential tests for priority contexts and final priority values before relying on integrated
  map outcomes.

### Phase 6: Action and Query Parity

- Replace every native `AIGateway` policy routine with Lua-owned choices plus exact host commands.
- Implement Lua policies for level-up, commander level-up, blocking dialogs, teleport, tavern, market, garrison,
  shipyard, university, artifact assembly, object selection, battle preservation, and end-turn handling.
- Ensure checked host commands validate ownership, visibility, freshness, resource availability, pending query ids,
  and request results without choosing strategy.

### Phase 7: Performance Helpers

- Profile only on `ssh dev`.
- If Lua is too slow, add narrow C++ helpers for neutral loops such as route enumeration, visible tile scans, or
  bulk strength calculations.
- Every helper must be covered by a boundary test showing that changing strategic thresholds or rankings still
  happens in Lua.

### Phase 8: Parity Harness

- Reuse and adapt `script-ai` Python trace/batch tools, but remove all native bounded-task assumptions.
- Add a fixed corpus of small generated maps and saved scenarios.
- Add pure Lua, fixture replay, snapshot differential, lockstep differential, and short integrated game commands
  behind one runner with clear presets such as `unit`, `fixtures`, `snapshot`, `lockstep-smoke`, and `corpus`.
- Run native `Nullkiller2` and `LuaNullkiller2` on the same seeds and compare:
  - pass count and priority pass decisions
  - generated task families and selected priorities
  - exact player actions and query answers
  - final day state summaries
  - game outcome over longer runs
- Accept temporary divergence only when it is documented with a specific missing port item.
- Make first-difference reports the default output, because parity work needs the earliest cause, not only the final
  mismatch.

### Phase 9: No-Native-Dependency Gate

Before claiming parity, prove:

- `AI/LuaNullkiller2` builds with `ENABLE_NULLKILLER2_AI=OFF`.
- `rg "Nullkiller|NK2AI|AIGateway" AI/LuaNullkiller2 scripts/ai/nullkiller2 luascript` has only allowed comments,
  config labels, or trace labels.
- No Lua facade method executes a native task handle, native priority pass, native analyzer, native build-army helper,
  or full-day fallback.
- The AI can complete fixed-map turns when `Nullkiller2` is unavailable in the build.
- Mirrored structure and port-index checks pass, with no unmapped Lua policy modules and no unmapped C++ business
  logic that is marked as required for parity.

### Phase 10: Post-Parity Improvement

Only after parity gates pass:

- Keep `LuaNullkiller2` as the baseline parity script.
- Add separate candidate scripts or policy flags for improvements.
- Promote changes only after corpus results improve without regressing parity-critical fixtures.
- Preserve the ability to run the exact parity script for future comparisons.

## Remote-Only Build and Test Workflow

Source edits and commits happen in `/home/user/vcmi/lua-nullkiller`. Heavy configure, build, test, and game runs
must happen only through `ssh dev`.

Planned workflow:

1. Keep a separate remote work directory, for example `~/vcmi-lua-nullkiller`.
2. Synchronize committed local source to that directory before each remote build/test run.
3. Install or enter required dependencies on the remote machine, using Nix there if useful.
4. Run CMake, compile, CTest, Python trace tools, and generated-map batches only on the remote machine.
5. Bring back only logs, trace summaries, and reduced JSON fixtures needed for commits.

No local command should configure or compile VCMI, run CTest, launch games, or execute expensive simulation
batches.

## Commit Strategy

Commit small, reviewable increments:

- planning and audit scripts
- host skeleton
- Lua contract and facade
- each behavior/analyzer/evaluator port
- each parity fixture or trace-tool improvement
- dependency-gate fixes

Commit messages must stay focused on the code change and must not mention the remote machine.

## Immediate Next Steps

1. Expand the snapshot contract for heroes, towns, objects, paths, threats, queries, and army stacks until behavior
   fixtures no longer need hand-written placeholder fields.
2. Complete `ExecuteHeroChain` parity: siege formation, visit/attack selection, broader special-action descriptors,
   and live host validation.
3. Extend garrison, hero exchange, artifact sequencing, and checked host validators with richer edge-case coverage.
4. Finish `RewardEvaluator` and object-specific priority context builders, then add fixture tests for raw context and
   final priority parity.
5. Add a native `Nullkiller2` trace exporter and a Lua replay/snapshot comparator so discrepancies produce
   minimized fixtures under `scripts/ai/nullkiller2/tests/fixtures/discrepancies/`.
6. Wire remaining query callbacks into Lua policy modules for artifact assembly, shipyard, and battle preservation
   decisions.
7. Strengthen the no-native-dependency gate by building `LuaNullkiller2` with native `Nullkiller2` disabled and
   auditing C++ and Lua policy code for forbidden native-delegation strings.
