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
- `updates`: revisioned changes since the previous script call.
- `opponentUpdates`: visible opponent movement and state changes since the previous owned turn.
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
- Memory should be saved in the AI save state with a script id and schema/version field.
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
  "trace": true
}
```

For development, add an opt-in reload mode so behavior can be edited and rerun without rebuilding. For normal
games, load once per map/session for deterministic behavior.

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
- Done: invalid or rejected actions stop script control and fall back to Nullkiller.
- Remaining: feed a real revisioned update/opponent-move journal into `updates` and `opponentUpdates`.

### Milestone 6: Host Analysis Surface

- Done: script input includes structured player, resource, hero, town, army, build, recruit, and reachable-object
  data.
- Done: action candidates include allowed build actions, affordable recruitment actions, route-id guarded
  movement actions, reachable object targets, and end turn.
- Done: route ids are generated with the same shape as MCP route ids and are validated before execution.
- Partial: danger estimates, defense alerts, and Nullkiller task fragments are not exposed yet.
- Partial: MCP and scripted AI still duplicate some JSON assembly code; extraction can happen once the surface
  stabilizes.

### Milestone 7: Default Script

- Done: `scripts/ai/defaultAdventure.lua` is a readable Lua policy with explicit scoring functions.
- Done: it scores allowed builds, recruitment, and reachable object pickups, then returns declarative actions.
- Done: it requests replanning after useful work and ends turn when no useful scripted candidate remains.
- Partial: main/scout role assignment, defense policy, and opponent-aware choices still need richer host analysis.

### Milestone 8: Save/Load and Development Reload

- Done: script memory is versioned at the Lua policy level and bounded by `maxMemoryBytes`.
- Done: `config/ai/scriptedAdventure.json` controls script path, reload behavior, action/memory/call limits,
  tracing, and repeated-failure throttling.
- Done: development reload is available through `reloadScriptEachTurn`.
- Not done: script memory is not serialized into savegames yet. Current VCMI save/load serializes `CGameState`,
  while adventure AI interface instances are client-side runtime objects with no save/load hook. Persisting
  script memory cleanly needs an explicit AI lifecycle serialization hook rather than storing AI-private data
  in game rules state.

### Milestone 9: Evaluation Loop

- Partial: opt-in trace files record script input, output, and execution progress under the user log directory.
- Remaining: add trace summaries, map-run scripts, and fixed-map comparison against Nullkiller.
- Remaining: use collected failures to expand host analysis and improve the default Lua policy.

## Open Design Questions

- Should `AdventurePlan` live in `lib/ai`, `AI/ScriptedAdventure`, or another shared target?
- Should the first script output only concrete actions, or also high-level intents that C++ decomposes?
- Which Nullkiller task abstractions can be safely exposed as candidate plan fragments?
- How much opponent movement can be reconstructed from visible updates without leaking hidden information?
- What is the right default memory size limit?
- Should script reload be per turn, per day, or only via explicit debug command?

## Recommended Next Step

Add a small AI lifecycle serialization hook so `ScriptedAdventureAI` can persist script-owned memory across
save/load without putting AI-private planning state into `CGameState`. After that, expand host analysis with
danger/defense estimates and Nullkiller-generated task fragments.
