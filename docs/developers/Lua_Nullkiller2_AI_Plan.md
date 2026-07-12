# Lua Nullkiller2 AI Implementation Plan

## Objective

Create a new adventure AI player named `LuaNullkiller2` whose strategic behavior is implemented in Lua and first
matches the current C++ `Nullkiller2` behavior as closely as practical. Only after parity is achieved should Lua
policy changes try to outperform `Nullkiller2`.

This is not a wrapper around `Nullkiller2`. The new AI must not fall back to `Nullkiller2`, execute native
`Nullkiller2` tasks, inherit from `NK2AI::AIGateway`, link the `Nullkiller2` target, or call `Nullkiller2`
business-logic helpers. Existing C++ game-rule validation and neutral compute services may be exposed to Lua, but
all AI choices must live in Lua.

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
  engine.lua
  settings.lua
  state.lua
  goals/
  behaviors/
  evaluators/
  analyzers/
  path/
  actions/
  queries/
  util/
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

- `engine.lua`: equivalent of `Nullkiller::makeTurn`, `updateStateAndExecutePriorityPass`, task failure policy, and
  end-of-pass cleanup.
- `settings.lua`: reads the same `config/ai/nk2ai/nk2ai-settings` values through host-provided JSON.
- `state.lua`: owns locks, active hero/target, scan depth, memory, and cached analysis.
- `goals/`: goal and task records for the existing `EGoals` set, plus `decompose`, `accept`, equality/hash, affected
  objects, and hero-exchange counts.
- `behaviors/`: `Startup`, `RecruitHero`, `BuyArmy`, `Building`, `CaptureObjects`, `Cluster`, `Defence`, `Escape`,
  `GatherArmy`, `Exploration`, and `StayAtTown`.
- `evaluators/`: `PriorityEvaluator`, `RewardEvaluator`, fuzzy danger scoring, priority tiers, and deterministic
  sorting/tie-breaking.
- `analyzers/`: Lua versions of build, hero, army, object-cluster, memory, and hit-map analysis.
- `path/`: Lua-owned interpretation of route records, hero chains, special actions, object graph choices, and path
  task construction. Heavy route enumeration can call neutral host pathfinding.
- `actions/`: exact action execution wrappers that call `ai.player:*` host commands.
- `queries/`: level-up, blocking dialog, teleport, tavern, market, recruitment, garrison, university, artifact
  assembly, and map-object selection policies.

## Implementation Phases

### Phase 0: Inventory and Port Checklist

- Add a machine-readable checklist that maps every `AI/Nullkiller2` file and function to one of: Lua port, neutral
  C++ helper, host action executor, obsolete, or deferred.
- Start with `Nullkiller::makeTurn`, `Settings`, `AbstractGoal`, concrete goals, behaviors, analyzers,
  `PriorityEvaluator`, `AIGateway` policy routines, and pathfinding special actions.
- Add an audit script that fails if `AI/LuaNullkiller2` includes `AI/Nullkiller2`, links `Nullkiller2`, or exposes
  host commands named after native `Nullkiller2` task helpers.

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

### Phase 3: Core Turn Loop Parity

- Port `Nullkiller::makeTurn` to `engine.lua`.
- Port resource locks, hero locks, scan depth, active target, object presence checks, task failure policy, and pass
  loop limits.
- Implement priority pass structure before ordinary adventure behavior selection.
- Add trace output that can be compared with native `Nullkiller2` traces at pass/task/action granularity.

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

### Phase 6: Action and Query Parity

- Replace every native `AIGateway` policy routine with Lua-owned choices plus exact host commands.
- Implement Lua policies for level-up, commander level-up, blocking dialogs, teleport, tavern, market, recruitment,
  garrison, shipyard, university, artifact assembly, object selection, battle preservation, and end-turn handling.
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
- Run native `Nullkiller2` and `LuaNullkiller2` on the same seeds and compare:
  - pass count and priority pass decisions
  - generated task families and selected priorities
  - exact player actions and query answers
  - final day state summaries
  - game outcome over longer runs
- Accept temporary divergence only when it is documented with a specific missing port item.

### Phase 9: No-Native-Dependency Gate

Before claiming parity, prove:

- `AI/LuaNullkiller2` builds with `ENABLE_NULLKILLER2_AI=OFF`.
- `rg "Nullkiller|NK2AI|AIGateway" AI/LuaNullkiller2 scripts/ai/nullkiller2 luascript` has only allowed comments,
  config labels, or trace labels.
- No Lua facade method executes a native task handle, native priority pass, native analyzer, native build-army helper,
  or full-day fallback.
- The AI can complete fixed-map turns when `Nullkiller2` is unavailable in the build.

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

1. Add the port checklist and dependency audit script.
2. Import the smallest useful subset of `script-ai` Lua runner/action infrastructure.
3. Create the standalone `AI/LuaNullkiller2` target and `LuaNullkiller2` factory registration.
4. Add the first Lua `engine.lua` skeleton that can load settings, trace a day start, and end the turn without
   native fallback.
5. Set up the remote work directory and run the first lightweight build check there.
