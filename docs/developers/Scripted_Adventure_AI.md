# Scripted Adventure AI Plan

This document describes the path from the current MCP day-plan work to a real scripted adventure AI.
The goal is to let VCMI AI behavior be improved by editing scripts, without rebuilding the heavy C++
binaries, while keeping the game rules unchanged and the engine authoritative over all state changes.

The scripting layer should be expressive enough to encode meaningful Heroes III strategy, but it should
not expose imperative game-action calls. A script should be a planner: it receives facts and memory, then
returns a declarative plan. C++ validates and executes that plan through the same adventure AI callback
mechanisms used today.

## Goals

- Make adventure AI behavior scriptable without recompiling the engine.
- Reuse the current MCP day-plan contract as the first concrete action vocabulary.
- Keep scripts functional in shape: state in, memory plus plan out.
- Preserve existing rules and validation. Scripts never modify `CGameState` and never bypass callbacks.
- Keep Nullkiller as the safe fallback whenever script loading, planning, validation, or execution fails.
- Support iterative improvement: run scripted AI, inspect mistakes, edit script, repeat.
- Support long-term strategy through script-owned memory serialized in saves.
- Support partial-day replanning when an action fails, a query appears, a battle starts, terrain is revealed,
  a teleport has unknown destination, or the returned plan intentionally stops early.

## Long-Term Iteration Vision

The target development loop is a stable VCMI binary with a fully capable scripted adventure AI host. Once the
host exposes enough visible state, host analysis, candidate tasks, and validated declarative actions, most AI
improvement should happen by editing scripts rather than rebuilding C++.

The desired loop:

1. Run scripted AI versus Nullkiller, older script versions, and other script profiles on fixed maps/seeds.
2. Summarize traces and automatically flag interesting mistakes: idle heroes with valuable reachable targets,
   town losses after defense warnings, bad target ordering, repeated fallbacks, unsafe hero movement, weak
   army gathering, and regressions against older scripts.
3. Improve Lua scoring, memory, role assignment, defense, gathering, and target-selection logic.
4. Rerun the same scenarios without rebuilding the engine.
5. Promote new C++ host data only when the script cannot express a strategy from existing read-only inputs.

This makes the binary the stable rules, validation, pathfinding, and analysis provider, while Lua becomes the
fast-changing strategic brain. The intended endpoint is not an LLM running inside the game, but a strong
scripted computer player whose policy can be iterated quickly, including with LLM assistance outside the game.

## Non-Goals

- Do not move game mechanics or rule calculations into AI scripts.
- Do not expose mutable `CCallback` or server-side state mutation APIs to scripts.
- Do not replace Nullkiller immediately. Scripted AI should wrap, steer, or override parts incrementally.
- Do not require an LLM at runtime. LLMs may generate and refine scripts offline, but the game should run
  the resulting script normally.
- Do not make MCP the script engine. MCP remains a useful external-agent harness.

## First Architectural Step

The MCP-owned plan contract has been moved to a transport-neutral module:

```text
client/mcp/McpAdventurePlan.*  ->  lib/ai/AdventurePlan.*
```

The module should contain only concepts shared by MCP and script AI:

- accepted day-plan action types
- action aliases and normalization
- schema or structural validation for plans
- plan result/status types
- helpers for partial execution and remaining actions

MCP depends on `AdventurePlan`, but does not own it. The scripted AI runner should depend on the same module.

Placement decision:

- `AdventurePlan` now lives under `lib/ai/` because both MCP and adventure AI targets can link `vcmiMain`.
- Script-specific runner code should live under `AI/ScriptedAdventure/` unless it becomes generally useful.

## Script Backend

Use Lua first.

Reasons:

- VCMI already ships a restricted Lua scripting system.
- Scripts are loaded as data from VFS/mod files.
- Lua can return structured tables that map naturally to `JsonNode`.
- The current Lua system already documents a "scripts must be constant and should not generate side effects"
  rule, which matches the desired functional planner style.

The AI scripting API should still be backend-shaped as an interface:

```text
IAdventureScriptRunner
  planDay(input) -> output
```

Lua is the first implementation. A later backend could be added without changing the adventure plan executor.

## Core Script Contract

The primary script entry point should be a pure planner function:

```lua
function ScriptedAI.planDay(input)
    return {
        memory = newMemory,
        actions = actions,
        status = "continue" -- or "end_turn", "need_replan", "fallback"
    }
end
```

Input:

- `state`: complete visible player state or selected state sections.
- `updates`: capped revisioned journal of recent visible changes. Scripts can store the last consumed revision
  in memory when they want delta processing.
- `opponentUpdates`: the same journal filtered to visible opponent-related changes.
- `progress`: result of the previous plan execution, including executed, failed, and remaining actions.
- `memory`: script-owned long-term context from previous calls/days.
- `day`: current day/week/month and active player color.
- `actionSpace`: currently legal or relevant high-level candidates.
- `analysis`: host-provided derived facts such as reachability, danger, town build options, recruitment options,
  and object values.
- `limits`: time, action count, max candidates, and max memory size limits for this call.

Output:

- `memory`: replacement script-owned memory to persist.
- `actions`: declarative daily actions, initially the same action vocabulary as `AdventurePlan`.
- `intent`: optional high-level explanation or strategy labels for trace/debugging.
- `status`:
  - `continue`: execute actions, then call again if there is remaining turn capacity.
  - `end_turn`: execute actions and end the turn if execution reaches the end.
  - `need_replan`: do not end turn; call script again after applying returned actions.
  - `fallback`: stop script control and let Nullkiller finish the turn.
- `returnSelect`: optional state sections to refresh after execution.
- `confidence`: optional numeric value for trace/debugging and future arbitration.

The script output is a request, not an order. C++ validates ownership, visibility, route freshness, resource
availability, pending queries, battle state, and server request results before anything changes.

## Memory Model

Script memory is a JSON-like value owned by the script and persisted by the AI wrapper.

Rules:

- Memory is strategy state, not game rules state.
- The engine should not interpret arbitrary memory keys except for generic metadata such as version and size.
- Memory is saved under the player-local JSON state with a script id and schema/version field. This uses the
  existing `SaveLocalState`/`PlayerState::playerLocalSettings` path, which is serialized with saves but is
  documented as client-defined local data rather than game mechanics.
- If memory is missing, too large, invalid, or from an incompatible script version, the runner should pass an
  empty/default memory value and log the reset.
- Scripts should keep memory compact: chosen main hero, assigned scout roles, target object ids, known threats,
  desired town plans, blocked objectives, and observations from previous days.

Example:

```json
{
  "version": 1,
  "strategy": "expand",
  "mainHero": 34,
  "scouts": [41, 52],
  "targets": [
    {"kind": "capture", "object_id": 180, "priority": 820},
    {"kind": "build", "town_id": 17, "building_id": 5, "priority": 500}
  ],
  "avoid": [
    {"position": {"x": 31, "y": 19, "z": 0}, "reason": "enemy-threat", "untilDay": 9}
  ]
}
```

## Functional Planner Style

Scripts should not call `moveHero`, `buildBuilding`, `recruitCreatures`, or similar mutation APIs.
Instead, they return data:

```lua
return {
    memory = memory,
    actions = {
        { id = "build-town-17", type = "build", town_id = 17, building_id = 5 },
        { id = "visit-gold", type = "visit_object", hero_id = 34, object_id = 180, route_id = "..." },
        { id = "end", type = "end_turn" }
    },
    status = "end_turn"
}
```

Allowed script operations:

- inspect immutable input tables
- compute scores
- sort/filter/rank candidate actions
- update and return memory
- produce action/intention data

Disallowed script operations:

- direct game state mutation
- direct callback calls
- filesystem/network access
- global mutable state that affects future calls outside returned memory
- random decisions not mediated by deterministic host-provided randomness

If randomness is needed, expose a deterministic per-player/per-turn seed in input or a pure helper that returns
precomputed rolls. This keeps saves and replays easier to reason about.

## Capability Surface

To encode meaningful strategy, the script needs a rich read-only input and a rich candidate set. The script
should not need to reconstruct low-level engine facts by scraping raw objects.

Required state sections:

- player resources, income, day/week/month, difficulty, team/diplomacy where visible
- owned heroes: position, movement, mana, skills, spells, army, artifacts, role hints
- owned towns: buildings, build availability, creature growth, recruitment pools, garrison, mage guild
- visible objects: type, owner, reward hints, guards, visitable position, last-seen revision
- visible enemy heroes and towns: position, owner, army estimates, danger estimates
- map knowledge: known tiles, terrain, roads, blocked passages, water/boat status, subterranean entrances
- quests and gates: known requirements and blockers
- pending queries: level-up, teleport, garrison, object selection, and other blocking choices
- recent updates: own progress and visible opponent moves since the last script decision

Required host analysis:

- reachable objects per hero, with route ids and path previews
- future-turn reachability summaries
- danger map and enemy reach estimates
- object value estimates and guard/loss estimates
- build options and build-chain prerequisites
- recruitment affordability and useful destination armies
- army gathering and hero-chain candidates
- defense alerts for threatened towns/heroes
- exploration frontier candidates
- abstract blockers and unlock chains, reusing Nullkiller analysis where possible

Required action candidates:

- concrete actions: build, recruit, move hero, visit object, answer query, end turn
- higher-level intents: assign hero role, target object, defend town, gather army, save resources, explore area
- plan fragments generated by C++/Nullkiller analyzers, so scripts can rank/select instead of hand-building
  every low-level move

This makes the script capable enough to express strategies such as:

- main/scout role assignment
- early economy versus army build choices
- resource saving for key buildings
- town specialization
- object prioritization by reward/risk/distance
- hero chaining and army consolidation
- defensive recalls and town protection
- exploration frontier management
- quest/gate unlock planning
- multi-day target pursuit
- opponent-aware avoidance or aggression

## Plan Execution Loop

The day loop should be:

1. Build script input from visible callback state, update journal, previous progress, memory, and host analysis.
2. Call `planDay`.
3. Validate output shape and memory size.
4. Normalize actions with `AdventurePlan`.
5. Execute actions sequentially through the normal callback request path.
6. Stop execution at the first stop condition.
7. If the script requested `end_turn` and execution reached `end_turn`, finish the turn.
8. If there is progress but no end turn, rebuild input and call the script again up to a bounded limit.
9. If the script fails or exceeds limits, fall back to Nullkiller.

Stop conditions:

- invalid action
- stale route id or route no longer reachable
- pending query
- battle or tactics phase
- teleport/portal/ship/water action with unknown resulting state
- object visit reveals important new state
- server request rejected
- script explicitly returns `need_replan`
- call/action/time budget exhausted

The ideal script should usually produce a full-day plan. Re-entry exists so one invalid or uncertain step does
not allow many unscripted actions to happen.

## Nullkiller Integration

Scripted AI should start as a wrapper around Nullkiller, not a fork.

Initial integration options:

1. Script-first with Nullkiller fallback:
   - script plans the day
   - Nullkiller finishes only on script failure or explicit fallback

2. Nullkiller candidate generator with script ranking:
   - Nullkiller computes goals/tasks/candidates
   - script ranks or filters them
   - C++ executes the selected candidate through existing Nullkiller mechanisms

3. Hybrid:
   - script controls high-level policy and persistent memory
   - Nullkiller continues to generate pathfinding, decompositions, and safety checks

Recommended path: hybrid. It gives scripts enough strategic influence while preserving mature pathfinding and
task execution code.

The script engine should reuse these Nullkiller systems where possible:

- analyzers for hero roles, builds, dangers, and reachable objects
- pathfinder and route ids
- object clusterizer and blocker information
- deep decomposer for quest/guard/unlock chains
- task execution wrappers for complex multi-step behaviors

## Public Related Work

Reviewed in July 2026. The useful conclusion is that VCMI has had adjacent scripting, AI, LLM, and
training-environment discussions for years, but no discovered public implementation appears to provide an
in-process, editable adventure AI strategy script with engine-validated declarative actions and Nullkiller
fallback.

Relevant references:

- [Python API forum thread](https://forum.vcmi.eu/t/python-api/838): discussed a Python callback API,
  scriptable map objects, script execution placement, safety/synchronization issues, and scriptable AI as a
  request. It is closest to general scripting/API design, but it focused on mutable callback exposure and map
  object scripting rather than a functional adventure AI planner.
- [On Visual Studio 11 support forum thread](https://forum.vcmi.eu/t/on-visual-studio-11-support/485):
  contains an early scripted-AI discussion. The key concern was not language choice but making VCMI's reasoning
  and callback model scriptable at all.
- [Scripting forum thread](https://forum.vcmi.eu/t/scripting/235?page=2) and
  [modding-system discussion](https://forum.vcmi.eu/t/modding-system-discussion/451): cover older server/client
  scripting and ERM-style gameplay scripting ideas. Those are intentionally different from this design: game
  rules stay in C++/server code, while AI scripts only return plans for normal AI callback execution.
- [GitHub issue #5586: LLM Learning Game Integration with VCMI](https://github.com/vcmi/vcmi/issues/5586):
  proposes structured game-state export and external commands for LLM learning. This aligns with the MCP/external
  agent path. `AdventurePlan` should remain reusable by that path, but the scripted adventure AI is an in-process
  Lua planner that can run without an LLM or command server.
- [GitHub issue #7108: VCMI Coach MVP](https://github.com/vcmi/vcmi/issues/7108): proposes a read-only coach
  that exposes and explains existing AI/engine recommendations. It is complementary: the same recommendation
  surfaces and trace explanations can help script authors, but it is explicitly not autonomous computer-player
  control.
- [Lua API reference](https://vcmi.eu/modders/Lua/API_Reference/),
  [Lua scripting support part 2 PR #7323](https://github.com/vcmi/vcmi/pull/7323), and
  [Lua scripting support part 3 PR #7392](https://github.com/vcmi/vcmi/pull/7392): provide the current Lua
  scripting direction. These are useful infrastructure references, but they target mod/game scripting rather
  than computer-player strategy.
- [PR #7453: remove dynamic loading of AI and scripting modules](https://github.com/vcmi/vcmi/pull/7453):
  confirms that AI modules should be normal in-tree/static-linked modules. This matches the current
  `ScriptedAdventureAI` wrapper plus data-loaded script files, rather than a dynamically loaded AI plugin.
- [vcmi-gym project notes](https://smanolloff.github.io/projects/vcmi-gym/): focus on reinforcement learning,
  currently battle-only AI, and explicitly leave adventure-only AI as a separate future project. This is related
  to evaluation and training, not a replacement for the scripted adventure planner.
- Existing AI mods such as [Boost AI](https://vcmi.eu/Mod%20Repository/AI/Boost%20AI/) are useful precedent for
  player-facing AI configuration, but they change bonuses/resources or difficulty pressure, not the computer
  player's adventure decision policy.

When sending this proposal upstream, reference #5586 as the external-agent/LLM cousin, #7108 as the read-only
advisor cousin, the Lua PRs as scripting infrastructure, and the forum threads as evidence that the hard boundary
is not "can Lua run" but "can AI strategy be scriptable without giving scripts mutable rule authority".

Ideas to adopt from this related work:

- Keep the `AdventurePlan` schema transport-neutral. The same state/action vocabulary should serve Lua scripts,
  MCP/external agents, trace replay, and evaluation tools where possible.
- Add lab-play evaluation scenarios alongside open-play map runs. Fixed tasks such as resource collection,
  town defense, guarded mine capture, exploration, and army gathering make script regressions easier to measure
  than full games alone.
- Keep MCP and LLM control as an experimental/external-agent path, not as the required runtime path for normal
  computer players. The in-game scripted AI should remain local, fast, reloadable, and safe to fall back from.
- Expose existing Nullkiller and engine recommendations as read-only candidate data before duplicating analysis
  in Lua. Useful candidates should include build advice, reachable-object scores, danger estimates, task
  fragments, blockers, defense alerts, and army-gathering options.
- Carry explanation fields with candidates and selected actions: `reason`, `risk`, `value`, `estimatedLoss`,
  `blockedBy`, and similar structured facts. These are useful for script scoring, trace review, future coach UI,
  and LLM-assisted script editing.
- Preserve the strict boundary learned from old scripting discussions: scripts receive facts and return plans;
  they do not receive mutable callbacks, direct server authority, or game-rule hooks.

## Files and Modules

Suggested new module:

```text
AI/ScriptedAdventure/
  CScriptedAdventureAI.h/.cpp
  AdventureScriptRunner.h/.cpp
  LuaAdventureScriptRunner.h/.cpp
  AdventureScriptContext.h/.cpp
  AdventurePlan.h/.cpp
  AdventureScriptConfig.h/.cpp
  scripts/default.lua
```

Possible shared support:

```text
lib/ai/
  AdventurePlan.h/.cpp
  AdventurePlanExecutionResult.h
```

Configuration:

```text
config/ai/scriptedAdventure.json
scripts/ai/defaultAdventure.lua
```

Example configuration fields:

```json
{
  "script": "vcmi:scripts/ai/defaultAdventure.lua",
  "fallbackAI": "Nullkiller2",
  "reloadScriptEachTurn": false,
  "maxScriptCallsPerTurn": 8,
  "maxActionsPerPlan": 64,
  "maxMemoryBytes": 262144,
  "maxUpdateEvents": 256,
  "trace": true,
  "players": {
    "red": { "script": "ai/aggressiveAdventure.lua" },
    "blue": { "script": "ai/economyAdventure.lua" }
  }
}
```

For development, add an opt-in reload mode so behavior can be edited and rerun without rebuilding. For normal
games, load once per map/session for deterministic behavior.

Per-player entries override the global script and limits for a specific computer player. Keys can be color
names such as `red`/`blue` or numeric player ids.

## Default Script Strategy

The first good script should be explicit and readable, not clever.

Recommended layers:

1. Memory initialization and migration.
2. Situation summary:
   - economy status
   - military status
   - town safety
   - enemy pressure
   - exploration status
3. Hero role assignment:
   - main hero
   - scouts
   - town defenders
4. Town policy:
   - key build targets
   - save-resource decisions
   - recruitment decisions
5. Map policy:
   - safe nearby pickups
   - exploration targets
   - guarded target thresholds
   - quest/unlock targets
6. Defense policy:
   - threatened towns
   - avoid zones
   - retreat to town or gather army
7. Action assembly:
   - build/recruit first when clearly beneficial
   - move scouts to safe value
   - move main hero to strategic target
   - answer obvious queries
   - end turn only when no useful safe action remains

The script should be written as scoring functions over host-provided candidates:

```lua
local function scoreObject(hero, candidate, memory, input)
    local score = candidate.value or 0
    score = score - 0.7 * (candidate.turns or 0)
    score = score - 2.0 * (candidate.expectedLoss or 0)
    if candidate.kind == "resource" and input.state.resources.gold < 2500 then
        score = score + 200
    end
    if memory.targetsByObject[candidate.object_id] then
        score = score + 150
    end
    return score
end
```

This is functional in style: the script scores and selects data, then returns a plan.

## Error Handling and Fallback

Fallback to Nullkiller on:

- script file missing
- syntax or initialization error
- runtime exception
- invalid output type
- memory too large or unserializable
- too many script calls in one turn
- too many rejected/invalid actions in one turn
- action execution makes no progress repeatedly
- unsupported pending query

Fallback behavior:

- log the script error and the input revision
- preserve or reset memory depending on error type
- append a trace event for debugging
- call Nullkiller to finish the current turn
- try the script again next turn unless disabled by repeated failures

Repeated failures should disable the script for the game or for a configurable number of turns.

## Tracing and Feedback Loop

The system should produce trace files suitable for AI improvement:

- script input summary
- script output
- normalized plan
- executed/failed/remaining actions
- state updates after execution
- fallback reason, if any
- final turn summary
- optional script `intent` field

When tracing is enabled, `ScriptedAdventureAI` writes JSON files under the user log directory in
`scriptedAdventureAI/`. The helper below summarizes those files:

```bash
scripts/ai/summarizeAdventureTrace.py <user-log-dir>/scriptedAdventureAI
```

It reports player/script/day counts, script output statuses, requested/executed/failed action types, failure
messages, visible update/opponent-update event types, defense-alert totals, hero-threat totals, candidate risks,
and script intents. Add `--json` for machine-readable output.

Trace sets from two script versions can be compared with:

```bash
scripts/ai/compareAdventureTrace.py <baseline-trace-dir> --candidate <candidate-trace-dir>
```

The comparison reports deltas for fallback outputs, failed actions, unsafe candidates, hero/town threat alerts,
and executed actions. This is intentionally trace-based so it can compare script versions without rebuilding.

Headless batches can be launched with:

```bash
scripts/ai/runAdventureAIBatch.py \
  --client <build-dir>/bin/vcmiclient \
  --map "Maps/Dwarven Gold.h3m" \
  --ai ScriptedAdventureAI --ai Nullkiller \
  --testdays 14 \
  --trace \
  --script scripts/ai/defaultAdventure.lua \
  --timeout 300 \
  --output scripted-ai-runs
```

The batch runner passes `--testdays N` to stop after N completed adventure days. The wall-clock timeout remains
as a safety guard for hangs or unexpectedly slow maps. `--trace` enables `ScriptedAdventureAI` tracing without
editing `config/ai/scriptedAdventure.json`. `--script` can be either a bundled script resource such as
`ai/defaultAdventure.lua` or a local Lua file; local files are passed to the AI as `file:/...` script overrides.

The same override can be supplied manually with environment variables:

```bash
VCMI_SCRIPTED_ADVENTURE_TRACE=1 \
VCMI_SCRIPTED_ADVENTURE_SCRIPT=file:/tmp/candidate.lua \
./vcmiclient --headless --testmap "Maps/Dwarven Gold.h3m" --ai ScriptedAdventureAI
```

Per-player script override variables are also supported, for example
`VCMI_SCRIPTED_ADVENTURE_RED_SCRIPT` or `VCMI_SCRIPTED_ADVENTURE_PLAYER_0_SCRIPT`.

Baseline-vs-candidate script evaluations can be launched with:

```bash
scripts/ai/evaluateAdventureAIScripts.py \
  --client <build-dir>/bin/vcmiclient \
  --map "Maps/Dwarven Gold.h3m" \
  --baseline-script scripts/ai/defaultAdventure.lua \
  --candidate-script /tmp/candidateAdventure.lua \
  --testdays 14 \
  --runs 5 \
  --timeout 300 \
  --output scripted-ai-eval \
  --clean
```

The evaluator runs both sides with tracing enabled, snapshots script files into the output directory when possible,
writes `evaluation.json`, and prints a heuristic score delta. The score is not a gameplay rating; it is an iteration
signal that rewards completed runs and useful actions while penalizing timeouts, nonzero exits, parse errors,
fallbacks, and failed actions. Higher-level win/loss and map-control metrics should replace or augment it as the
host exposes richer state.

This enables the intended loop:

1. Run scripted AI versus baseline.
2. Inspect traces and bad decisions.
3. Edit script.
4. Rerun without rebuilding.
5. Promote useful host analysis or action types into C++ only when scripts cannot express them cleanly.

## Testing Strategy

Unit tests:

- `AdventurePlan` normalization and schema.
- Script output validation.
- Memory migration/reset behavior.
- Fallback triggers.
- Partial execution result handling.

Integration tests:

- tiny maps with one town and one hero
- invalid action falls back or replans without crash
- stale route causes partial result and replan
- pending query pauses plan and allows scripted answer
- script memory survives save/load
- script disabled after repeated failures

Regression harness:

- run fixed maps for N days against Nullkiller
- compare trace-level metrics:
  - resources collected
  - objects captured
  - heroes recruited
  - towns defended/lost
  - turn completion rate
  - fallback count

## Implementation Notes for PR

- The stable-binary/script-iteration direction is implemented by keeping C++ responsible for state extraction,
  validation, pathfinding, and fallback while Lua only returns plans.
- Script memory persistence uses the existing `PlayerState::playerLocalSettings` serialized JSON, namespaced
  under `scriptedAdventureAI`. This avoids adding AI-private strategy memory to authoritative game-rule objects.
- Candidate actions now carry read-only explanation fields: `reason`, `value`, `risk`, `safe`, `danger`,
  `dangerRatio`, `estimatedLoss`, and `blockedBy`. Scripts can score these fields and traces can summarize them.
- `analysis.heroThreatAlerts` complements `analysis.defenseAlerts`, so scripts can respond to threatened roaming
  heroes as well as threatened towns.
- Trace tooling now supports both single-run summaries and baseline-vs-candidate comparisons for script iteration.
- A headless batch runner can launch fixed-day AI-vs-AI runs and summarize traces. The client-side `--testdays`
  option makes `--testmap`/`--testsave` runs exit after N completed adventure days.
- `ScriptedAdventureAI` supports environment overrides for trace enablement and script path, including external
  `file:/...` Lua scripts. This makes script edits and candidate snapshots testable without rebuilding or editing
  packaged config.
- `scripts/ai/evaluateAdventureAIScripts.py` runs baseline and candidate scripts through the same fixed maps,
  collects traces, snapshots script files, and emits an evaluation JSON with heuristic score deltas.
- Debugging `Emerald Isles` smoke runs showed the scripted host must not use Nullkiller helper methods that perform
  hidden side effects such as army exchange after movement. Scripted movement is now a direct, tracked `MoveHero`
  request over a route-id-validated path, and garrison/hero-exchange/recruitment dialogs are conservatively answered
  without implicit stack management.
- Scripted build, recruit, move, end-turn, memory-save, and blocking-dialog query replies tolerate
  `requestSent`/`requestRealized` callback ordering differences. This prevents trace I/O from masking timing bugs.
- Generic `visit_object` candidates currently exclude treasure chests and sea chests because they open blocking
  choice dialogs during movement. Add explicit query-aware chest handling before re-enabling them as scripted
  targets.
- Invalid or rejected actions are passed back to the script as `progress.failed` for bounded replanning. Nullkiller
  fallback remains for script failures, script-requested fallback, repeated failures, or exhausted script-call budget.
- Remaining C++ expansion should focus on additional read-only candidates, especially Nullkiller task fragments,
  blocker/unlock chains, and army-gathering options.

## Milestones

### Milestone 1: Neutral AdventurePlan Module

- Done: `AdventurePlan` lives in `lib/ai`.
- Done: accepted actions, aliases, normalization, and schema are out of MCP ownership.
- Done: MCP behavior is unchanged.
- Done: MCP/AdventurePlan tests cover the existing action contract.

### Milestone 2: Script Contract Types

- Add C++ structs for script input, output, progress, memory, and status.
- Add JSON conversion and validation.
- Add tests for invalid output and memory limits.

### Milestone 3: Lua Runner Prototype

- Done: load a restricted Lua planner and call `planDay(input)`.
- Done: convert returned Lua tables to JSON/contract structs.
- Done: validate output status, memory size, action count, and normalized `AdventurePlan` actions.
- Done: cover the Lua runner and default script with unit tests.

### Milestone 4: Scripted AI Wrapper

- Done: `ScriptedAdventureAI` is an adventure AI option built on top of Nullkiller's gateway.
- Done: on turn start, it calls the script runner and executes returned `AdventurePlan` actions.
- Done: build, recruit, move, visit-object, answer-query, and end-turn actions are validated through existing
  callback paths.
- Done: script failures, invalid actions, missing scripts, and repeated failures fall back to Nullkiller.

### Milestone 5: Replanning Loop

- Done: bounded repeated script calls within one day are implemented.
- Done: previous execution progress is passed into the next script call.
- Done: object visits and teleport-like moves stop the current action batch and force a fresh script decision.
- Done: invalid or rejected actions stop the current batch, populate `progress.failed`, and allow bounded script
  replanning before fallback.
- Done: `updates` and `opponentUpdates` are populated from a capped revisioned journal of visible AI events,
  including hero movement, new/removed objects, revealed tiles, town visits, building changes, and created heroes.

### Milestone 6: Host Analysis Surface

- Done: script input includes structured player, resource, hero, town, army, build, recruit, and reachable-object
  data.
- Done: action candidates include allowed build actions, affordable recruitment actions, route-id guarded
  movement actions, reachable object targets, and end turn.
- Done: route ids are generated with the same shape as MCP route ids and are validated before execution.
- Done: visible enemy heroes/towns and nearby town defense alerts are exposed in `analysis`.
- Done: movement/object candidates include read-only `reason`, `value`, `risk`, `safe`, `danger`, `dangerRatio`,
  `estimatedLoss`, and `blockedBy` fields.
- Done: nearby visible enemy pressure against owned heroes is exposed as `analysis.heroThreatAlerts`.
- Partial: full danger-map estimates and Nullkiller task fragments are not exposed yet.
- Partial: MCP and scripted AI still duplicate some JSON assembly code; extraction can happen once the surface
  stabilizes.

### Milestone 7: Default Script

- Done: `scripts/ai/defaultAdventure.lua` is a readable Lua policy with explicit scoring functions.
- Done: it scores allowed builds, recruitment, and reachable object pickups, then returns declarative actions.
- Done: it requests replanning after useful work and ends turn when no useful scripted candidate remains.
- Done: it assigns a main hero, tracks consumed opponent-update revisions, prioritizes recruitment under strong
  defense pressure, and penalizes scout targets near visible enemy heroes.
- Done: it consumes candidate risk/value fields, avoids unsafe object targets more aggressively, and can move a
  threatened hero away from a visible stronger enemy.
- Partial: deeper defense policy still needs richer host analysis and higher-level defend/gather candidates.

### Milestone 8: Save/Load and Development Reload

- Done: script memory is versioned at the Lua policy level and bounded by `maxMemoryBytes`.
- Done: script memory is persisted through save/load in `PlayerState::playerLocalSettings` under the
  `scriptedAdventureAI` key, with script-path and storage-version checks.
- Done: `config/ai/scriptedAdventure.json` controls script path, reload behavior, action/memory/call limits,
  tracing, and repeated-failure throttling.
- Done: development reload is available through `reloadScriptEachTurn`.

### Milestone 9: Evaluation Loop

- Done: opt-in trace files record script input, output, and execution progress under the user log directory.
- Done: `scripts/ai/summarizeAdventureTrace.py` summarizes trace files for the script-improvement loop.
- Done: `scripts/ai/compareAdventureTrace.py` compares baseline and candidate trace sets for script iteration.
- Done: `scripts/ai/runAdventureAIBatch.py` launches fixed-day headless map runs and summarizes traces, with a
  timeout guard for hangs.
- Done: `scripts/ai/evaluateAdventureAIScripts.py` runs baseline-vs-candidate script batches and writes a scored
  evaluation manifest for the run-trace-improve loop.
- Done: `vcmiclient --testdays N` stops `--testmap`/`--testsave` benchmark runs after N completed adventure days.
- Remaining: add real gameplay outcome metrics such as win/loss, towns held, hero strength, explored area, and map
  control, then use collected failures to expand host analysis and improve the default Lua policy.

## Open Design Questions

- Should `AdventurePlan` live in `lib/ai`, `AI/ScriptedAdventure`, or another shared target?
- Should the first script output only concrete actions, or also high-level intents that C++ decomposes?
- Which Nullkiller task abstractions can be safely exposed as candidate plan fragments?
- How much opponent movement can be reconstructed from visible updates without leaking hidden information?
- What is the right default memory size limit?
- Should script reload be per turn, per day, or only via explicit debug command?

## Recommended Next Step

The next high-value implementation steps are:

- Run fixed-map `--testdays N` batches comparing default, aggressive, economy, explorer, Nullkiller, and older
  script versions, then feed trace deltas back into the Lua policy.
- Expand the default Lua policy from basic defense/opponent awareness into real defend/gather/avoid-zone
  strategy once higher-level candidates are available.
- Expose richer Nullkiller-generated task fragments and danger estimates as read-only candidates for scripts to
  rank instead of rebuilding those analyses in Lua.
