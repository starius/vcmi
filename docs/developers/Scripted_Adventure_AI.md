# Scripted Adventure AI Plan

This document describes the path from the current ScriptedAdventureAI work to a real editable adventure AI.
The goal is to let VCMI AI behavior be improved by editing Lua scripts, without rebuilding the heavy C++
binaries, while keeping the game rules unchanged and the engine authoritative over all state changes.

The current direction is an imperative Lua AI facade. A script gets visible state, persistent memory, and a
checked `ai` object. The script can call methods such as `ai:build`, `ai:visitObject`, `ai:answerQuery`, and
`ai:nullkiller`, and those calls yield back to C++. C++ validates and executes every request through the normal
adventure AI callback/server path. Lua never receives mutable `CGameState`, raw server authority, hidden map
information, or unchecked game-rule hooks.

## Goals

- Make adventure AI behavior scriptable without recompiling the engine.
- Reuse the current AdventurePlan/MCP action vocabulary as the checked host command format.
- Make scripts ergonomic to write: one `runDay(ai, input)` coroutine per player day, with refresh/yield points
  after uncertain actions.
- Preserve existing rules and validation. Scripts never modify `CGameState` and never bypass callbacks.
- Keep Nullkiller as the safe fallback whenever script loading, planning, validation, or execution fails.
- Make Nullkiller available as a callable fallback/subroutine surface, then expose more of its analyzers and
  candidate generators as read-only script helpers over time.
- Support iterative improvement: run scripted AI, inspect mistakes, edit script, repeat.
- Support long-term strategy through script-owned memory serialized in saves.
- Support partial-day decisions when an action fails, a query appears, a battle starts, terrain is revealed,
  a teleport has unknown destination, or the script intentionally asks for fresh state.

## Long-Term Iteration Vision

The target development loop is a stable VCMI binary with a fully capable scripted adventure AI host. Once the
host exposes enough visible state, host analysis, candidate tasks, and validated imperative commands, most AI
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
- Do not expose hidden information or anything unavailable to a normal player/AI callback.
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
  rule. The AI facade keeps that spirit by routing every requested game action back through checked C++ code.

The AI scripting API should still be backend-shaped as an interface:

```text
IAdventureScriptRunner
  runDay(ai, input) -> output
```

Lua is the first implementation. A later backend could be added without changing the C++ command executor.

## Core Script Contract

The primary script entry point is a per-day coroutine:

```lua
function Script.runDay(ai, input)
    local memory = input.memory or { version = 1 }
    ai:setMemory(memory)

    local state = ai:state()
    local candidates = ai:actionSpace()

    if shouldDelegate(state, candidates, memory) then
        return ai:nullkiller("delegate difficult turn")
    end

    local result = ai:visitObject(bestHeroId, bestObjectId, bestRouteId)
    if result.stop then
        input = ai:refresh()
    end

    ai:endTurn()
    return ai:output("end_turn", "script completed day")
end
```

Input:

- `state`: complete visible player state or selected state sections.
- `state.map`: map dimensions, visible tile count, a capped sample of visible terrain tiles, and a capped list
  of visible objects gathered through player-specific fog-of-war checks.
- Visible quest objects include a `quest` block. Requirement details are exposed only when the quest is already
  active/known for the player; inactive visible quest objects expose only active/completed flags.
- `updates`: capped revisioned journal of recent visible changes. Scripts can store the last consumed revision
  in memory when they want delta processing.
- `opponentUpdates`: the same journal filtered to visible opponent-related changes.
- `progress`: result of the previous plan execution, including executed, failed, and remaining actions.
- `memory`: script-owned long-term context from previous calls/days.
- current day/week/month and active player color under `state`.
- `state.turn.queries`: typed pending dialog/window queries with query ids, stable type labels, answer ids, and
  mode-specific numeric fields. This is the read-side model for Lua dialog callbacks.
- `actionSpace`: currently legal or relevant high-level candidates.
- `analysis`: host-provided derived facts such as reachability, danger, town build options, recruitment options,
  and object values.
- `limits`: time, action count, max candidates, and max memory size limits for this call.

The `ai` facade:

- `ai:state()`, `ai:updates()`, `ai:opponentUpdates()`, `ai:progress()`, `ai:actionSpace()`, `ai:analysis()`,
  `ai:limits()`: read current visible input sections.
- `ai:pendingQueries()`: return `state.turn.queries` for dialog/window decisions.
- `ai:memory()` and `ai:setMemory(memory)`: read/replace script-owned memory for this day.
- `ai:refresh()`: yield to C++ and receive a new visible input snapshot after side effects.
- `ai:build`, `ai:recruit`, `ai:hireHero`, `ai:transferArmy`, `ai:moveHero`, `ai:visitObject`,
  `ai:answerQuery`, `ai:endTurn`: request checked host actions.
- `ai:swapCreatures`, `ai:mergeStacks`, `ai:splitStack`, `ai:bulkSplitStack`, `ai:bulkMergeStacks`,
  `ai:bulkSplitAndRebalanceStack`, `ai:dismissCreature`, `ai:upgradeCreature`, `ai:setFormation`,
  `ai:setTactics`, and `ai:swapGarrisonHero`: request exact army stack, upgrade, formation, tactics, and
  town-garrison operations using stable object, slot, creature, and formation ids.
- `ai:pickBestArtifacts(heroId, otherHeroId?)`: ask the host to run Nullkiller's artifact-preparation helper for
  one owned hero, or two co-located owned heroes, through the normal artifact swap callback path.
- `ai:swapArtifacts(src, dst)`, `ai:bulkMoveArtifacts`, `ai:sortBackpackArtifacts`,
  `ai:scrollBackpackArtifacts`, `ai:manageHeroCostume`, `ai:assembleArtifacts`,
  `ai:disassembleArtifact`: request exact artifact management operations through the normal callback/server
  path. Artifact locations use `{ holder_id, slot, creature_slot? }`.
- `ai:ignoreScriptDecision(queryId)`: clear a script-local decision prompt such as an artifact assembly prompt
  without sending a server `QueryReply`.
- `ai:nullkillerTrade()`: ask the host to run Nullkiller's resource-trading helper once, returning whether it
  traded anything.
- `ai:tradeResources(marketId, sellResourceId, buyResourceId, amount, heroId?)`: request an exact
  resource-to-resource market trade using stable numeric resource ids and a visible market object id.
- `ai:marketTrade({...})` and wrappers `ai:sendResources`, `ai:sellCreatures`, `ai:buyMarketArtifact`,
  `ai:sellArtifact`, `ai:sacrificeArtifact`, `ai:sacrificeCreatures`, `ai:transformToUndead`, and `ai:buySkill`:
  request every native market mode through stable numeric mode/resource/player/slot/artifact/skill ids.
- `ai:dismissHero`, `ai:buildBoat`, `ai:dig`, `ai:castSpell`, `ai:buyArtifact`: request checked primitive
  adventure/town actions through the normal callback/server path.
- `ai:nullkiller()` / `ai:nullkillerForRestOfDay()`: stop script control and let Nullkiller finish the turn.
- `ai:nullkillerTasks(mode, maxCandidates)`: ask Nullkiller for a bounded snapshot of native task candidates.
  `mode` is `priority`, `adventure`, or `all`. Returned `task_id` values are opaque handles that expire on
  `ai:refresh()` or the next candidate snapshot.
- `ai:runNullkillerTask(taskId)`: execute one previously returned native Nullkiller task through C++ validation
  and Nullkiller's normal task machinery, then return control to Lua.
- `ai:nullkillerStep(mode, maxCandidates)`: ask for candidates and execute the best one as a single bounded
  Nullkiller subroutine. Unlike `ai:nullkiller()`, this does not intentionally give away the rest of the day.
- `ai:output(status, intent, confidence)`: return final status plus current memory.

Output from `runDay`:

- `memory`: replacement script-owned memory to persist.
- `actions`: normally empty for imperative scripts. The host command boundary still uses AdventurePlan-shaped
  action tables internally, but scripts should execute them through `ai:*` methods instead of returning a batch.
- `intent`: optional high-level explanation or strategy labels for trace/debugging.
- `status`:
  - `continue`: script returned without ending the turn; current host treats this as unsafe and falls back.
  - `end_turn`: script has ended or asks the host to end the turn.
  - `need_replan`: legacy planner status; imperative scripts should prefer `ai:refresh()`.
  - `fallback`: stop script control and let Nullkiller finish the turn.
- `confidence`: optional numeric value for trace/debugging and future arbitration.

Every script action call is a request, not an order. C++ validates ownership, visibility, route freshness,
resource availability, pending queries, battle state, and server request results before anything changes. If a
request is invalid, C++ returns an error to Lua; `ai:*` raises a Lua error that scripts can catch with `pcall`.
Uncaught errors abort the script for the day and fall back to Nullkiller.

## Stable Identifiers

The script interface must not make strategic decisions from user-visible text. Localized fields such as town
names, building names, creature names, object names, subtype names, and hover text may appear in traces for
debugging, but they are labels only.

Planner input should expose stable machine fields for every meaningful concept:

- object instance ids, hero ids, town ids, building ids, creature ids, resource ids, artifact ids, and spell ids
  as integers
- map object `typeId`/`subtypeId` plus host-provided `kindId` categories such as resource, mine, artifact,
  town, creature bank, dwelling, monster, teleport, shrine, market, and quest
- town building `building_id` plus host-provided `buildingKindId`, `buildingLevel`, and `buildingUpgrade`
- path metadata `pathActionId`, `layerId`, coordinates, movement cost, and teleport flags
- danger and threat metadata as numeric fields such as `riskId`, `danger`, `dangerRatio`, and alert `levelId`
- optional stable ASCII identifiers such as object JSON keys for traces and external tooling

Lua policies should prefer the integer kind/id fields. Stable ASCII identifiers are acceptable for tools and
debugging, but they should not replace numeric ids in hot scoring paths. User-visible strings are never a
contract and may change with language packs, mods, or translation fixes.

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

## Imperative Command Style

Scripts may call the checked AI facade, not raw game mutation APIs. From Lua this feels imperative:

```lua
local ok, err = pcall(function()
    ai:build(17, 5)
end)

if not ok then
    memory.blockedBuilds[17] = err
end

local move = ai:visitObject(34, 180, routeId)
if move.stop then
    input = ai:refresh()
end
```

Internally each facade method yields an AdventurePlan-shaped command table to C++. The command still looks like:

```lua
{ type = "visit_object", hero_id = 34, object_id = 180, route_id = "..." }
```

C++ then validates and executes it through normal callback/server requests.

Allowed script operations:

- inspect visible input tables
- compute scores
- sort/filter/rank candidate actions
- update and return memory
- call checked `ai:*` facade methods
- yield for `ai:refresh()` when state may have changed
- catch invalid-action errors and choose another strategy
- delegate to Nullkiller for the rest of the day

Disallowed script operations:

- direct game state mutation
- direct callback calls
- hidden map/state access
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
- adventure spell, dig, boat/shipyard, and other direct player-action candidates with checked `planAction`
  payloads
- abstract blockers and unlock chains, reusing Nullkiller analysis where possible

Required action candidates:

- concrete actions: build, recruit creatures, hire hero, transfer army, move hero, visit object, answer query,
  end turn
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

## Imperative Execution Loop

The day loop should be:

1. Build script input from visible callback state, update journal, previous progress, memory, and host analysis.
2. Create the restricted `ai` facade and start `runDay(ai, input)` as a Lua coroutine.
3. When Lua yields `execute`, validate and execute the requested action through the normal callback request path.
4. Return success or an error object to Lua. The facade converts host errors into catchable Lua errors.
5. When Lua yields `refresh`, rebuild visible input and return it to the coroutine.
6. When Lua yields `fallback`, stop the coroutine and let Nullkiller finish the turn.
7. When Lua returns, validate output shape and memory size, then persist memory.
8. If Lua ended the turn or the host has no active turn, finish successfully.
9. If Lua returns while the turn is still active, fall back to Nullkiller.
10. If the script fails, exceeds limits, or yields unsupported commands, fall back to Nullkiller.

Stop conditions:

- invalid action
- stale route id or route no longer reachable
- pending query
- battle or tactics phase
- teleport/portal/ship/water action with unknown resulting state
- object visit reveals important new state
- server request rejected
- script explicitly calls `ai:refresh()` or delegates to Nullkiller
- call/action/time budget exhausted

The ideal script should usually make enough decisions to control the whole day. `ai:refresh()` exists for
uncertain steps such as object visits, portals, dialogs, and failed requests.

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

Current bounded subroutine surface:

- `nullkiller_tasks` exposes priority tasks from `RecruitHeroBehavior`, `BuyArmyBehavior`, and
  `BuildingBehavior`, plus adventure tasks from capture, cluster, defense, escape, gather-army, and exploration
  behavior decomposition.
- Task search modes can be aggregate (`priority`, `adventure`, `all`) or granular (`recruit_hero`, `buy_army`,
  `building`, `capture`, `cluster`, `defense`, `escape`, `gather_army`, `exploration`). Lua also exposes these
  as numeric constants under `ai.nullkillerTaskModes`, so scripts can avoid magic numbers and brittle strings.
- Candidate JSON contains stable machine fields such as `task_id`, `goalTypeId`, `priority`, `priorityTier`,
  `hero_id`, `town_id`, `object_id`, `tile`, affected object ids, and hero role ids. Debug descriptions may be
  present for traces, but scripts should use stable ids for strategy.
- `nullkiller_task` executes exactly one stored candidate snapshot through `Nullkiller::executeScriptTask`.
  Native Nullkiller dialog handlers stay active for this path, so the subroutine behaves like Nullkiller rather
  than simplified script auto-answer logic.
- Nullkiller script-task state is reset once at the beginning of the scripted turn, not before every candidate
  query. This preserves native state such as locked heroes, scan-depth changes, previous task success, and
  hero-chain decisions across bounded Lua subroutine calls.
- `nullkiller_step` is a convenience call that selects the current best bounded candidate, can try later
  candidates using Nullkiller's own failure policy, then returns to Lua for refresh, more decisions, or end-turn.
  It accepts `max_attempts` and returns stable fields including `outcomeId`, `failureActionId`, `didExecute`,
  `shouldReplan`, `shouldStopTurn`, `exhaustedCandidates`, `selectedTaskIndex`, and `attempts`.
- Lua exposes `ai.nullkillerStepOutcomes`, `ai.nullkillerFailureActions`, and `ai.nullkillerTaskModes` numeric
  constants. Scripts should branch on these constants rather than trace strings.

The script engine should reuse these Nullkiller systems where possible:

- analyzers for hero roles, builds, dangers, and reachable objects
- pathfinder and route ids
- object clusterizer and blocker information
- deep decomposer for quest/guard/unlock chains
- task execution wrappers for complex multi-step behaviors

Current read-only Nullkiller analyzer exposure:

- Owned hero records include `nullkillerRoleId` / `nullkillerRole`, using the same main/scout labels as
  `HeroManager`.
- Owned hero records include `nullkillerFightingScore` and `nullkillerMagicScore`, which mirror Nullkiller's
  hero evaluator outputs for script ranking. These are advisory scores, not game rules.

## Public Related Work

Reviewed in July 2026. The useful conclusion is that VCMI has had adjacent scripting, AI, LLM, and
training-environment discussions for years, but no discovered public implementation appears to provide an
in-process, editable adventure AI strategy script with coroutine-style checked player actions and Nullkiller
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
  rules stay in C++/server code, while AI scripts only request checked player/AI actions.
- [GitHub issue #5586: LLM Learning Game Integration with VCMI](https://github.com/vcmi/vcmi/issues/5586):
  proposes structured game-state export and external commands for LLM learning. This aligns with the MCP/external
  agent path. `AdventurePlan` should remain reusable by that path, but the scripted adventure AI is an in-process
  Lua AI that can run without an LLM or command server.
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
  to evaluation and training, not a replacement for the scripted adventure AI.
- Existing AI mods such as [Boost AI](https://vcmi.eu/Mod%20Repository/AI/Boost%20AI/) are useful precedent for
  player-facing AI configuration, but they change bonuses/resources or difficulty pressure, not the computer
  player's adventure decision policy.

When sending this proposal upstream, reference #5586 as the external-agent/LLM cousin, #7108 as the read-only
advisor cousin, the Lua PRs as scripting infrastructure, and the forum threads as evidence that the hard boundary
is not "can Lua run" but "can AI strategy be scriptable without giving scripts mutable rule authority".

Ideas to adopt from this related work:

- Keep the `AdventurePlan` schema transport-neutral. The same state/action vocabulary should serve Lua facade
  commands, MCP/external agents, trace replay, and evaluation tools where possible.
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
- Preserve the strict boundary learned from old scripting discussions: scripts receive facts and request checked
  actions; they do not receive mutable callbacks, direct server authority, hidden information, or game-rule hooks.

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
scripts/ai/candidates/fallbackAdventure.lua
```

Example configuration fields:

```json
{
  "script": "ai/candidates/fallbackAdventure.lua",
  "fallbackAI": "Nullkiller2",
  "reloadScriptEachTurn": false,
  "maxScriptCallsPerTurn": 8,
  "maxActionsPerPlan": 64,
  "maxMemoryBytes": 262144,
  "maxUpdateEvents": 256,
  "trace": true
}
```

For development, add an opt-in reload mode so behavior can be edited and rerun without rebuilding. For normal
games, load once per map/session for deterministic behavior.

Per-player entries override the global script and limits for a specific computer player. Keys can be color
names such as `red`/`blue` or numeric player ids. Keep personality profiles such as `aggressiveAdventure.lua`,
`economyAdventure.lua`, and `explorerAdventure.lua` opt-in until they beat the fallback control.

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
7. Action execution:
   - build/recruit first when clearly beneficial
   - move scouts to safe value
   - move main hero to strategic target
   - answer obvious queries
   - end turn only when no useful safe action remains

The script should be written as scoring functions over host-provided candidates, followed by checked facade
calls:

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

local target = chooseBestTarget(input, memory)
if target then
    local result = ai:visitObject(target.hero_id, target.object.id, target.route_id)
    if result.stop then
        input = ai:refresh()
    end
end
```

This keeps the policy readable: the script scores and selects data, then calls a small checked API rather than
touching engine internals.

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
- yielded commands
- host responses
- script output
- executed/failed actions and progress
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
script intents, final visible-state quality, and mined policy-mistake counts. Imperative runs emit
`imperative-input`, `imperative-command`, and `imperative-output` trace events. Add `--json` for
machine-readable output.

The current quality score is intentionally trace-local and explainable. It uses the final visible input for each
scripted player and combines resources, towns, heroes, army strength, movement, useful candidates, and visible
threat pressure. It is not a replacement for win/loss, but it gives the improvement loop a stable control signal
before full map-control metrics exist.

Trace sets from two script versions can be compared with:

```bash
scripts/ai/compareAdventureTrace.py <baseline-trace-dir> --candidate <candidate-trace-dir>
```

The comparison reports deltas for fallback outputs, failed actions, unsafe candidates, hero/town threat alerts,
executed actions, mined mistakes, important mistakes, and trace-local quality. This is intentionally trace-based
so it can compare script versions without rebuilding.

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
Use `--jobs N` to run independent headless games in parallel. Each run writes a `run.json`, raw stdout, logs, and
trace files under its run directory; the batch root also gets `manifest.json` and a compact `results.json` with
winner, outcome, completed-day, seed, script, AI order, and trace paths for downstream analysis.
Use `--scenario-file scripts/ai/evaluationScenarios.json` to run the graduated evaluation ladder instead of
repeating `--map` options. Scenario entries define:

- `stage`: `smoke`, `early`, `mid`, or `outcome`
- `group`: `training` or `heldout`
- `kind`: `handcrafted`, generated random-map experiments, or fixed random-map artifacts
- `gameSeed`, `runs`, `testdays`, `timeout`, tags, and optional fixed random-map seed/template metadata

Scenario entries can set `randomMap` instead of `map`/`save` to generate a map at run time. The random-map fields
are `size`, `levels`, `players`, `teams`, `compOnlyPlayers`, `compOnlyTeams`, `water`, `monsterStrength`,
`template`, `seed` or `mapSeed`, and `gameSeed`. `seed`/`mapSeed` controls map generation; `gameSeed` controls
gameplay randomness after the map exists. For example:

```json
{
  "name": "rmg-small-underground-nowater-1",
  "kind": "generated-random",
  "randomMap": { "size": "S", "levels": 2, "players": 2, "water": "none" },
  "seed": 51001,
  "gameSeed": 61001,
  "testdays": 0,
  "timeout": 1800
}
```

The cheap smoke stage is active by default and uses fixed server RNG seeds via `vcmiclient --seed`. Longer
handcrafted scenarios and fixed-seed random-map corpus entries are present in the same file and can be enabled when
the runtime budget and generated random map files are available. Disabled scenarios are skipped unless
`--include-disabled` is passed. `--stage`, `--group`, and `--kind` can restrict a batch to a subset of the ladder.
For random-map corpus entries, `seed` identifies the generated map artifact and `gameSeed` identifies the gameplay
RNG used when evaluating that artifact.

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
  --scenario-file scripts/ai/evaluationScenarios.json \
  --baseline-script scripts/ai/defaultAdventure.lua \
  --candidate-script /tmp/candidateAdventure.lua \
  --no-trace \
  --jobs 8 \
  --output scripted-ai-eval \
  --clean
```

The evaluator snapshots script files into the output directory when possible, writes `evaluation.json`, and prints
a heuristic score delta, quality delta, and promotion verdict. Tracing is enabled by default for trace-local
quality, action, and mistake metrics, but promotion/control runs can pass `--no-trace` to avoid trace I/O changing
timing-sensitive full-game outcomes. With `--no-trace`, trace-local metrics are zeroed and scoring is focused on
run safety plus win/loss outcomes. Use traced reruns on selected failures for diagnosis and JSON fixture mining.
The score is not a gameplay rating; it is an iteration signal that rewards completed runs, useful actions, final
visible-state quality when traces are enabled, and real red-player wins while penalizing timeouts, nonzero exits,
parse errors, fallbacks, failed actions, stopped batches, mined mistakes, and red-player losses. The evaluator
writes global metrics plus bucketed metrics by `group`, `stage`, and `kind`.

Promotion gates now encode the training/held-out workflow: the candidate must improve on the `training` bucket and
must not regress on the `heldout` bucket when those buckets are present. The fixed-seed random-map entries should be
generated once and kept stable, then split between training and held-out groups. Script authors may iterate against
training maps, but promotion should continue to require held-out non-regression. Higher-level map-control metrics
should replace or augment the current trace-local quality score as the host exposes richer state.

Trace mistakes can be mined into review notes and draft JSON policy fixtures:

```bash
scripts/ai/mineAdventureTraceMistakes.py \
  scripted-ai-eval/baseline \
  --write-fixtures test/testdata/ai/adventure-script/generated
```

The generated fixtures are review material, not automatic truth. Each one should be trimmed into a small
input-output example before being committed as a Lua policy test.

After an evaluation verdict says `promote`, the candidate can replace the current champion script while archiving
the previous champion:

```bash
scripts/ai/promoteAdventureAIScript.py \
  --evaluation scripted-ai-eval/evaluation.json \
  --champion scripts/ai/defaultAdventure.lua \
  --archive-dir scripts/ai/archive \
  --dry-run
```

Remove `--dry-run` after checking the planned file operations. The archive directory is reserved for old champion
Lua files and promotion manifests; candidate experiments can live under `scripts/ai/candidates/`.

The current measured control/champion for full-game outcomes is `scripts/ai/candidates/fallbackAdventure.lua`.
It delegates the whole turn to native Nullkiller through the scripted wrapper. `scripts/ai/defaultAdventure.lua`
is still the readable experimental policy and fixture target, but recent no-trace full-game runs show it
oversteers Nullkiller and should not be promoted over the fallback control until it wins repeated training runs
and does not regress held-out runs.

This enables the intended loop:

1. Put a candidate Lua file under `scripts/ai/candidates/` or another local path.
2. Run baseline-vs-candidate promotion scenarios in no-trace mode against the fallback control.
3. Iterate against training scenarios first; use held-out scenarios only as the promotion guard.
4. Inspect `evaluation.json`, bucket deltas, win/loss outcomes, run safety, and score deltas.
5. Rerun selected losses with tracing enabled, then convert representative mistakes into JSON policy fixtures.
6. Edit the Lua script and rerun without rebuilding.
7. Promote the candidate only when hard safety gates pass, training improves, and held-out scenarios do not regress.
8. Promote useful host analysis or action types into C++ only when scripts cannot express them cleanly.

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
  validation, pathfinding, checked execution, and fallback while Lua owns strategy and calls a restricted AI facade.
- Script memory persistence uses the existing `PlayerState::playerLocalSettings` serialized JSON, namespaced
  under `scriptedAdventureAI`. This avoids adding AI-private strategy memory to authoritative game-rule objects.
- Candidate actions now carry read-only explanation fields: `reason`, `value`, `riskId`, `risk`, `safe`, `danger`,
  `dangerRatio`, `estimatedLoss`, and `blockedBy`. Scripts can score these fields and traces can summarize them.
- Candidate actions expose stable machine identifiers for policy decisions. Map objects provide numeric
  `typeId`/`subtypeId` plus `kindId`; town build options provide `building_id`, `buildingKindId`,
  `buildingLevel`, and `buildingUpgrade`; paths provide `pathActionId`; visible threat alerts provide `levelId`.
  Localized display strings remain useful in traces but are not part of the strategic contract.
- Script input now includes `state.map.visibleTilesCount`, capped `state.map.visibleTiles` terrain samples, and
  capped `state.map.visibleObjects`. These are produced through player-specific visibility checks. The caps keep
  traces bounded; scripts should use counts and object ids, and request richer host candidates when a full-map
  operation would be too large for direct Lua input.
- Visible quest objects now expose known requirements as stable ids after they become active for the player:
  mission id, last day, required resources, artifacts, creatures, skills, heroes/classes, players, spells, nested
  limiter counts, kill targets, and `canCompleteWithContextHero` for reachable-object context.
- Candidate actions now include `hire_hero` and `transfer_army` for two previously missing Nullkiller-level
  capabilities: adding tavern heroes and concentrating or reinforcing armies through the normal server-validated
  request path. Their candidate generation is controlled by `experimentalSupportActions` in
  `config/ai/scriptedAdventure.json`, so the loop can disable them for bisection without rebuilding.
- Lua can now call `ai:pickBestArtifacts(heroId, otherHeroId?)` to reuse Nullkiller's artifact-preparation helper
  for one owned hero or two co-located owned heroes. This is a coarse helper, not yet a full artifact-slot API.
- Hero input now includes artifact state: worn slots and backpack entries expose stable slot ids, artifact type ids,
  artifact instance ids, lock state, and possible slot ids. Lua can request exact artifact swaps, bulk transfers,
  backpack sorting/scrolling, hero costume operations, artifact assembly, and artifact disassembly through checked
  host calls. Assembly prompts are exposed as typed script-local decisions with stable artifact ids; Lua may choose
  an offered assembly action or explicitly ignore the prompt without sending a normal server query reply.
- Hero input now exposes `formationId` and `tacticsEnabled`, and Lua can request exact creature stack
  rearrangement, stack splitting, stack merging, creature dismissal, creature upgrades, formation/tactics changes,
  and town garrison-hero swaps through the same checked callback/server packet path used by native clients and AI.
- Lua can now call `ai:nullkillerTrade()` to run Nullkiller's build-driven resource trader once,
  `ai:tradeResources(marketId, sellResourceId, buyResourceId, amount, heroId?)` for the common exact
  resource-to-resource path, or `ai:marketTrade({...})` and its mode-specific wrappers for every native
  `EMarketMode`. Visible market objects expose their supported market modes; richer market-rate inspection is
  still a separate read-side improvement.
- Script input now includes typed `state.turn.queries` records for level-up, blocking, teleport, object-selection,
  tavern, hero-exchange, garrison, recruitment, university, market dialogs, and script-local artifact assembly
  prompts. Dialogs raised during direct Lua actions now pause the action with `pending_query = true`; Lua can
  `refresh()`, inspect `state.turn.queries`, answer server dialogs through `ai:answerQuery(queryId, answer)`, and
  clear script-local prompts through `ai:ignoreScriptDecision(queryId)`.
- Lua can now request checked primitive adventure actions for dismissing heroes, building boats, digging, and
  casting adventure spells. C++ validates ownership and visible target tiles before forwarding to the server.
- Lua can now request exact spellbook and blacksmith war-machine purchases through `ai:buyArtifact(heroId,
  artifactId)`. `actionSpace.buyArtifactOptions` lists currently available purchases for visiting owned heroes
  using stable artifact ids; the server still enforces Mage Guild, blacksmith, gold, and slot rules.
- `analysis.heroThreatAlerts` complements `analysis.defenseAlerts`, so scripts can respond to threatened roaming
  heroes as well as threatened towns.
- Trace tooling now supports single-run summaries, baseline-vs-candidate comparisons, final visible-state quality
  estimates, mined mistake categories, and draft JSON fixture extraction for script iteration.
- A headless batch runner can launch fixed-day AI-vs-AI runs and summarize traces. The client-side `--testdays`
  option makes `--testmap`/`--testsave` runs exit after N completed adventure days. Batch and evaluation tools can
  also consume named scenario files for repeated graduated ladders with training, held-out, handcrafted, and
  fixed-seed random-map entries.
- `ScriptedAdventureAI` supports environment overrides for trace enablement and script path, including external
  `file:/...` Lua scripts. This makes script edits and candidate snapshots testable without rebuilding or editing
  packaged config.
- The imperative Lua path is now present as `runDay(ai, input)`. The runner executes it as a coroutine, handles
  yielded `execute`/`refresh`/`fallback` commands, and keeps the legacy `planDay(input)` path for old scripts.
  A smoke run on `Dwarven Gold` and `Ready or Not` reached the one-day limit cleanly and wrote
  `imperative-input`/`imperative-output` traces with fallback status through the configured fallback script.
- Explicit script delegation through `fallback`/`ai:nullkiller()` is not counted as a script failure; syntax,
  runtime, invalid-output, and exhausted-limit failures still use the repeated-failure throttle.
- `scripts/ai/evaluateAdventureAIScripts.py` runs baseline and candidate scripts through the same fixed maps,
  collects traces, snapshots script files, and emits an evaluation JSON with heuristic score deltas, quality deltas,
  mistake penalties, stage/group/kind buckets, outcome counts, and a promotion verdict.
- `scripts/ai/promoteAdventureAIScript.py` archives the current champion script and installs a candidate after an
  evaluation verdict passes, keeping champion promotion as a script-only operation.
- First seeded smoke-loop iteration tested a scout-only exploration fallback candidate. The ladder completed
  deterministically with fixed `gameSeed` values and rejected the candidate because it was safe but did not improve
  the training bucket. The default champion script was left unchanged.
- The first full 10-game random-map corpus on small no-water two-level maps showed the default scripted policy
  still lost all games to regular Nullkiller2. The baseline average loss day was 44.3; stable build/object IDs,
  Nullkiller delegation for unsupported remainder work, and localized-string removal improved the average to 47.6
  but did not produce wins. Trace mining pointed at capability gaps rather than string drift: scripted fallback
  dependence, defense pressure without response, hero threat escape gaps, and missing high-level Nullkiller actions.
  The first follow-up target is now implemented for tavern hero hiring and army transfer; remaining gaps include
  richer defense planning, hero chaining, and deeper blocker plans.
- A 10-game support-action run requested `hire_hero` 63 times and `transfer_army` 160 times. It still lost 10/10
  against Nullkiller2, but the average loss day rose from 36.7 with support candidates gated to 44.9 with them
  enabled on the same seed corpus. After moving threatened-hero escape selection before the support-action replan
  return, the same corpus improved to a 47.7 average loss day with `hire_hero` 146 times and `transfer_army` 153
  times. Support actions are now enabled by default for the improvement loop. Remaining failures are still
  concentrated around defense pressure, hero threat handling, dialogs during multi-action batches, hero chaining,
  and deeper blocker plans.
- An all-fallback control script is kept at `scripts/ai/candidates/fallbackAdventure.lua`. On the same 10-map
  corpus it also lost 10/10, but averaged 50.5 loss day, which means the default script is still oversteering
  Nullkiller in some openings. A no-object-routing experiment averaged 47.8 and was rejected. Future candidates
  should beat the all-fallback control before promotion.
- Stable-id hardening removed the remaining strategic dependency on display strings in bundled Lua policies and
  added the reusable small, two-level, no-water fixed-seed corpus. A fresh run on the initial 10-map version still
  lost 10/10 to Nullkiller2, with average loss day 49.3, min 21, max 108.
  The result improves the previous default-script average, but still trails the all-fallback control; the next
  behavioral target is reducing oversteering rather than more identifier cleanup.
- The live small random-map promotion corpus is now `scripts/ai/rmgSmallUndergroundNoWater16.json`, expanded to
  16 fixed generated maps. The promotion target for this corpus is 16/16 victories, not 10/10.
- On the 16-map corpus, the imperative all-fallback control is the current champion at 11/16 wins. The first
  converted `defaultAdventure.lua` compatibility wrapper lost 16/16 because it oversteered map movement and object
  routing; traced runs showed 3050 `visit_object` commands, 927 `move_hero` commands, and 397 replan-limit
  fallbacks. A safer support-only wrapper that delegates map movement to Nullkiller improved the default candidate
  to 5/16, but it is still below the all-fallback control and should not be promoted.
- A later Nullkiller2-vs-Nullkiller2 mirror check showed red wins 7/10 on the same fixed-seed corpus, while
  ScriptedAdventureAI with the all-fallback Lua control initially won only 1/10. The root cause was not Lua policy:
  fallback still ran through `CScriptedAdventureAI` dialog overrides, so Nullkiller inherited simplified scripted
  answers for level-ups, blocking dialogs, garrisons, recruitment, markets, and hero exchange. Native Nullkiller
  dialog handling is now preserved for fallback turns. After the fix, all-fallback ScriptedAdventureAI also won
  7/10 on the corpus, matching the mirror by win count.
- The first bounded Nullkiller subroutine smoke used `scripts/ai/candidates/boundedNullkillerAdventure.lua` on
  `smoke-training-dwarven-gold` for one day. It executed four `nullkiller_step` actions with no failed host
  actions, then intentionally delegated the remaining turn when the bounded task surface had no executable native
  task left. This confirms the Lua coroutine can call native Nullkiller task fragments and regain control, while
  also showing the next API gap: failed or exhausted native task searches should provide richer replan/try-next
  details to Lua.
- The same debugging pass found two opposite modal-query hazards. First, Lua-owned `visit_object` actions could
  leave clients asleep on a stale modal query, for example a garrison dialog opened by movement, so scripted query
  replies are now always sent asynchronously. Second, delegating rich modal callbacks to Nullkiller during a
  script-owned action let native helper code perform hidden side effects, especially hero exchange artifact/army
  rearrangement. Fallback turns still use native Nullkiller dialog handling, but script-owned movement/object
  actions now answer garrison, hero-exchange, recruitment, teleport, map-object-selection, and blocking dialogs
  through the scripted executor boundary. This is a containment fix, not the final strategy interface.
- Dialogs should become typed facade decision points instead of raw UI automation. Lua should receive visible
  dialog/query data and call helpers such as `ai:chooseChestReward`, `ai:chooseTeleportExit`, or
  `ai:answerQuery` with stable ids. If an unplanned query appears, the executor should return structured progress
  such as `needs_choice` with the visible dialog state so Lua can refresh and decide.
- Artifact and army rearrangement should be added back as explicit strategy capabilities, not hidden callback
  behavior. Facade methods should stay semantic, for example `ai:prepareHero`, `ai:transferArmy`, and
  `ai:rearrangeArtifacts`, with intent fields such as `combat`, `mobility`, `scout`, `defend_town`, or
  `deliver_army`. The C++ executor can then use existing legal Nullkiller mechanics and trace every resulting
  transfer so script iterations can learn whether the preparation helped.
- A fresh 10-game default-script run after the callback-boundary fix ended without timeouts: ScriptedAdventureAI
  won 3/10 and Nullkiller2 won 7/10, average completion day 60.4. The run confirms that the integration bug is
  contained, but the default script is still strategically weaker than native Nullkiller/fallback. Trace summaries
  point at oversteering, one-action replanning, visible-threat escape gaps, and missing high-level Nullkiller task
  fragments rather than game-data or run-control failures.
- Previous full-game controls on the 10-map corpus are timing-sensitive. A plain Nullkiller2 mirror gave red
  6/10. The all-fallback ScriptedAdventureAI control gave 4/10 with tracing enabled and 5/10 without tracing, with
  different seed-level winners. Treat single full-outcome runs as noisy; use no-trace aggregate promotion batches
  for win/loss control and traced reruns only for explanation, mistake mining, and regression fixtures.
- A no-trace run of the richer `defaultAdventure.lua` policy on the same corpus won only 2/10, with wins on seeds
  03 and 08. Because those wins are a subset of the all-fallback no-trace wins, current evidence says the readable
  default policy is useful for experimentation and unit fixtures but is not yet the champion behavior.
- Existing eager personality profiles are weaker than the fallback control on the previous 10-map corpus:
  `aggressiveAdventure.lua` and `economyAdventure.lua` were already all losses in partial/full scans, and
  `explorerAdventure.lua` lost 10/10. The packaged config now uses `ai/candidates/fallbackAdventure.lua` as the
  default script so normal ScriptedAdventureAI runs start from the measured control. Experimental profiles remain
  available through per-player config or environment overrides.
- `scripts/ai/runAdventureAIBatch.py` terminates a stale client process after a terminal game outcome has appeared
  in stdout and a short grace period has elapsed. This keeps unattended evaluation batches from hanging while still
  recording the completed outcome and traces.
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

- Done: load a restricted Lua runner and call legacy `planDay(input)`.
- Done: convert returned Lua tables to JSON/contract structs.
- Done: validate output status, memory size, action count, and normalized `AdventurePlan` actions.
- Done: cover the Lua runner and default script with unit tests.
- Done: load scripts with `runDay(ai, input)` and run them as coroutines.
- Done: expose a restricted Lua `ai` facade with visible input access, memory access, checked action helpers,
  `refresh`, and Nullkiller delegation.
- Done: host action errors are returned to Lua as catchable errors; uncaught Lua failures abort script control and
  fall back through the normal wrapper path.

### Milestone 4: Scripted AI Wrapper

- Done: `ScriptedAdventureAI` is an adventure AI option built on top of Nullkiller's gateway.
- Done: on turn start, it calls the script runner and executes returned `AdventurePlan` actions.
- Done: scripts that define `runDay` take the imperative coroutine path before the legacy `planDay` path.
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
- Done: script input includes map dimensions, visible tile counts/samples, and a capped list of visible objects
  gathered through player-specific fog-of-war checks.
- Done: action candidates include allowed build actions, affordable recruitment actions, route-id guarded
  movement actions, reachable object targets, and end turn.
- Done: route ids are generated with the same shape as MCP route ids and are validated before execution.
- Done: visible enemy heroes/towns and nearby town defense alerts are exposed in `analysis`.
- Done: movement/object candidates include read-only `reason`, `value`, `risk`, `safe`, `danger`, `dangerRatio`,
  `estimatedLoss`, and `blockedBy` fields.
- Done: nearby visible enemy pressure against owned heroes is exposed as `analysis.heroThreatAlerts`.
- Done: owned hero records expose Nullkiller's main/scout role plus fighting and magic evaluator scores, so Lua
  does not need to re-infer hero roles from scratch.
- Partial: bounded Nullkiller task fragments are exposed through `ai:nullkillerTasks`, `ai:runNullkillerTask`,
  and `ai:nullkillerStep`. The bounded step now preserves Nullkiller script-task state across a scripted turn,
  returns structured execution outcomes, and can be restricted to granular behavior families such as defense,
  gather-army, exploration, building, recruitment, or capture. This is still not full parity: scripts need richer
  direct access to typed dialogs and deeper analyzer details before serious script optimization should be treated
  as meaningful.
- Done: known quest requirements are exposed on visible quest objects using stable ids without revealing inactive
  quest internals. Lua can reason about known blockers, while still using movement actions or bounded Nullkiller
  tasks for actual unlock-chain execution.
- Done: owned hero artifact state, exact artifact management calls, typed artifact assembly prompts, and checked
  assemble/disassemble actions are exposed through Lua facade methods. Remaining artifact work is richer artifact
  scoring helpers.
- Done: exact army stack management, creature upgrades, formation/tactics changes, and town garrison-hero swaps
  are exposed through checked Lua facade methods.
- Done: pending dialog/window queries are exposed as typed read-side data under `state.turn.queries`, and direct
  Lua actions that open these dialogs now pause for a script answer instead of auto-answering. The bundled default
  script includes a conservative fallback answer policy; richer per-dialog strategy remains Lua policy work.
- Done: market operations are exposed through a coarse `nullkillerTrade` helper, an exact resource-resource
  helper, and a generic `marketTrade` action with wrappers for resource transfer, creature/resource sale,
  artifact purchase/sale/sacrifice, creature sacrifice, undead transformation, and university skill purchase.
- Done: visible owned/neutral market objects expose read-side mode details, available items, available unit
  counts, efficiency, and resource-resource exchange rates. Enemy market details remain hidden beyond public
  visible-object mode metadata.
- Partial: primitive adventure spells, digging, boat building, hero dismissal, and spellbook/war-machine purchases
  are exposed. Owned hero records now include spellbook ids, and action space includes read-side `digOptions`,
  `shipyardOptions`, `adventureSpellOptions`, and `buyArtifactOptions` with checked `planAction` payloads. Deeper
  adventure-spell routing remains partial: Lua gets default casts, owned-town targets, and nearby visible tile
  samples, while complex Dimension Door, Town Portal, boat, and unlock-chain planning should still use bounded
  Nullkiller tasks until richer analyzer exports exist.
- Partial: full danger-map estimates are not exposed yet.
- Partial: MCP and scripted AI still duplicate some JSON assembly code; extraction can happen once the surface
  stabilizes.

### Milestone 7: Default Script

- Done: `scripts/ai/defaultAdventure.lua` is a readable Lua policy with explicit scoring functions.
- Done: it scores allowed builds, recruitment, and reachable object pickups.
- Done: its active `runDay(ai, input)` path is imperative: Lua owns the day loop, executes checked `ai:*`
  calls directly, refreshes visible state after side effects, answers pending queries, and uses bounded
  Nullkiller steps before delegating the remaining turn. The older `planDay(input)` path remains only as a
  compatibility shim for fixtures and legacy callers.
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
- Done: batch and evaluation tools accept scenario files, including `scripts/ai/evaluationScenarios.json`, so the
  same script versions can be tested across a stable map/testday matrix.
- Done: `scripts/ai/evaluationScenarios.json` is now a graduated ladder with active smoke scenarios, longer
  handcrafted stages, and documented fixed-seed random-map training/held-out corpus entries.
- Done: headless test runs accept `vcmiclient --seed N`, and scenario `gameSeed` values are passed through so
  baseline/candidate comparisons use the same gameplay RNG.
- Done: headless test runs accept generated random maps with explicit map seeds, and the batch runner can execute
  those scenarios in parallel with compact win/day result summaries.
- Done: evaluations report bucketed metrics by training/held-out group, ladder stage, and scenario kind.
- Done: promotion gates require training improvement and held-out non-regression when those buckets are present.
- Done: batch runs parse red-player win/loss and day-limit outcomes from client logs and include those signals in
  evaluation scoring.
- Done: trace summaries and evaluations include explainable quality metrics from final visible state snapshots.
- Done: trace summaries mine likely policy mistakes, including unsafe choices, ignored better safe objects, failed
  actions, idle turns with candidates, and missing responses to visible threats.
- Done: `scripts/ai/mineAdventureTraceMistakes.py` exports mined mistakes as draft JSON fixtures for Lua policy
  tests.
- Done: `scripts/ai/promoteAdventureAIScript.py` supports champion-script promotion with archive manifests after a
  passing evaluation verdict.
- Done: `vcmiclient --testdays N` stops `--testmap`/`--testsave` benchmark runs after N completed adventure days.
- Done: first seeded script-loop policy iteration ran end-to-end and rejected a safe but non-improving exploration
  candidate, leaving the champion script unchanged.
- Done: stable-id hardening now covers default, aggressive, economy, and explorer Lua policies. Host path actions,
  risk labels, and threat-alert levels have numeric IDs, with display strings retained only for traces and legacy
  fixture compatibility.
- Remaining: decide which generated-map seeds graduate into the stable training/held-out corpus, then add
  engine-level explored-area and map-control deltas.

## Open Design Questions

- Should `AdventurePlan` live in `lib/ai`, `AI/ScriptedAdventure`, or another shared target?
- Should the first script output only concrete actions, or also high-level intents that C++ decomposes?
- Which additional Nullkiller task/analyzer abstractions should be exposed next, and which ones need narrower
  checked facades before Lua can safely select them?
- How much opponent movement can be reconstructed from visible updates without leaking hidden information?
- What is the right default memory size limit?
- Should script reload be per turn, per day, or only via explicit debug command?

## Recommended Next Step

The next high-value implementation steps are:

- Add richer Lua decision policies and read-side candidate data for dialogs and remaining player choices:
  quests/gates, level-up choices, university choices, object selection, and deeper adventure-spell target
  ranking beyond the current sampled candidate surface.
- Expose richer Nullkiller analyzer data, especially danger-map and blocker/cluster details, as read-only
  candidate fields instead of rebuilding those analyses in Lua.
- Expand the default Lua policy to rank and compose bounded Nullkiller candidates after the API can express the
  same meaningful choices Nullkiller can make.
- Run fixed-map `--testdays N` batches comparing default, aggressive, economy, explorer, Nullkiller, and older
  script versions, then feed trace deltas and mined JSON fixtures back into the Lua policy.
