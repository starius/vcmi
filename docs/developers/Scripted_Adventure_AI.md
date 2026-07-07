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

## First Architectural Rename

The current `McpAdventurePlan` module should become a transport-neutral module:

```text
client/mcp/McpAdventurePlan.*  ->  AI/ScriptedAdventure/AdventurePlan.* or lib/ai/AdventurePlan.*
```

Recommended target name: `AdventurePlan`.

The module should contain only concepts shared by MCP and script AI:

- accepted day-plan action types
- action aliases and normalization
- schema or structural validation for plans
- plan result/status types
- helpers for partial execution and remaining actions

MCP should depend on `AdventurePlan`, not own it. The scripted AI runner should depend on the same module.

Placement decision:

- Short term: put `AdventurePlan` under `client/mcp` only long enough to avoid a large move.
- Medium term: move it under an AI-neutral location such as `lib/ai/` if both MCP and AI targets can link it
  cleanly, or under `AI/ScriptedAdventure/` if it is only used by adventure AI implementations.

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

- Rename `McpAdventurePlan` to `AdventurePlan`.
- Move accepted actions, aliases, normalization, and schema out of MCP ownership.
- Keep MCP behavior unchanged.
- Keep existing MCP tests passing.

### Milestone 2: Script Contract Types

- Add C++ structs for script input, output, progress, memory, and status.
- Add JSON conversion and validation.
- Add tests for invalid output and memory limits.

### Milestone 3: Lua Runner Prototype

- Load a Lua script from VFS/config.
- Call `planDay(input)`.
- Convert returned Lua table to JSON/contract structs.
- Do not execute actions yet.
- Add trace output for script input/output.

### Milestone 4: Scripted AI Wrapper

- Add `ScriptedAdventureAI` as an adventure AI option.
- On turn start, call script runner.
- Execute returned `AdventurePlan` actions through existing validated callback flow.
- Fall back to Nullkiller on failure.

### Milestone 5: Replanning Loop

- Add bounded repeated script calls within one day.
- Feed previous progress and updates back into the next input.
- Stop cleanly on unknown outcomes, pending queries, battles, and invalid actions.

### Milestone 6: Host Analysis Surface

- Reuse MCP day-context and Nullkiller analyzers to provide rich script input.
- Add reachable targets, build options, recruitment options, danger estimates, and suggested plan fragments.
- Keep all analysis read-only.

### Milestone 7: Default Script

- Implement a readable default Lua script with explicit scoring functions.
- Cover build, recruit, exploration, safe pickups, main/scout roles, and basic defense.
- Keep it easy for an LLM or developer to modify.

### Milestone 8: Save/Load and Development Reload

- Serialize script memory.
- Add memory versioning.
- Add opt-in script reload for development runs.
- Keep normal runs deterministic.

### Milestone 9: Evaluation Loop

- Add trace summaries and map-run scripts.
- Compare scripted AI against Nullkiller on fixed maps.
- Use failures to improve the default script and host analysis.

## Open Design Questions

- Should `AdventurePlan` live in `lib/ai`, `AI/ScriptedAdventure`, or another shared target?
- Should the first script output only concrete actions, or also high-level intents that C++ decomposes?
- Which Nullkiller task abstractions can be safely exposed as candidate plan fragments?
- How much opponent movement can be reconstructed from visible updates without leaking hidden information?
- What is the right default memory size limit?
- Should script reload be per turn, per day, or only via explicit debug command?

## Recommended Next Step

Implement Milestone 1 completely: rename `McpAdventurePlan` to `AdventurePlan` and make MCP depend on the
neutral module. Then add contract structs for script input/output before writing the Lua runner. This keeps the
architecture honest: MCP is a transport, `AdventurePlan` is the planning language, and `ScriptedAdventureAI`
is the in-game scripted player.
