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
- `state.map`: map dimensions, complete visible terrain tiles, tile-level visible visitable/blocking object ids,
  and complete visible objects gathered through player-specific fog-of-war checks.
- `state.ownedObjects`: the full player-specific list of currently owned/flagged map objects, using public
  object fields and stable ids. This is intentionally separate from visible-map scans, because a normal player can
  inspect owned mines, dwellings, and other flagged assets even when they are not near a hero.
- `state.quests`: current player quest-log entries in the same stable shape as the mirrored quest-log window
  update. Visible quest object details are attached only when they are visible to the player.
- Visible quest objects include a `quest` block. Requirement details are exposed only when the quest is already
  active/known for the player; inactive visible quest objects expose only active/completed flags.
- `actionSpace.questObjectOptions`: a capped index of visible quest/guard/gate objects with owned-hero
  completion candidates.
- `actionSpace.adventureSpellOptions`: checked adventure spell cast candidates for owned heroes. Each option
  includes stable numeric ids for the hero, spell, target kind, and spell kind; mana/cost/cast-limit accounting;
  effect-specific numeric metadata for Dimension Door, Town Portal, Summon Boat, and ranged spells; water-walk
  and fly capability flags; visible target object data where a target tile contains a visible object; and a
  `planAction` that still goes through the normal server-side spell validation. Spells whose useful target choice
  is owned by native pathfinding, such as Dimension Door, Town Portal, Summon Boat, water walk, and fly, also include
  `nativePlanner` bounded Nullkiller `tasksAction`, `stepAction`, and `passAction` payloads.
- `updates`: capped revisioned journal of recent visible changes. Scripts can store the last consumed revision
  in memory when they want delta processing.
- `updates` also includes player-visible non-query windows such as generic info dialogs, shipyard dialogs,
  hill-fort windows, and thieves-guild windows. Localized text is trace context only; stable component/object ids
  are the script-facing data.
- The update journal mirrors visible adventure state changes: hero movement/stat/mana/skill/bonus changes,
  garrison changes, artifact movement, available creature/artifact changes, resource receipts, adventure spell
  casts, revealed tiles, hidden tiles, and object property changes such as ownership/visited/available-creature
  updates. Owned objects include detailed snapshots; visible non-owned objects stay on public object fields.
- It also mirrors player-visible lifecycle/window events such as hero visits, quest-log/world-view/puzzle-map
  windows, player turn starts/ends, battle-finished notifications, and game-over messages without adding hidden
  map state.
- `opponentUpdates`: the same journal filtered to visible opponent-related changes.
- `progress`: result of the previous plan execution, including executed, failed, and remaining actions.
- `memory`: script-owned long-term context from previous calls/days.
- current day/week/month and active player color under `state`. `state.calendar` also includes stable numeric
  `currentDay`, `dayOfWeek`, `dayOfMonth`, `week`, `month`, `daysInWeek`, `weeksInMonth`, and `daysInMonth`
  fields for scripts that reason about growth, timed quests, or build timing.
- `state.map`: map dimensions plus the current player-visible/explored tile and object snapshot. In addition to
  the raw visible tile/object arrays, it includes compact explored-area counters and ratios, per-level explored
  counts, terrain-class counts, and `visibleControl` summaries by object kind, owner, and relation/control id.
  These summaries are derived only from the same visible tile/object scan that populates the raw arrays.
- `state.grail`: player-specific puzzle-map knowledge. `knownRatio` is visible to Lua, but the exact `position`
  is included only when the puzzle is fully revealed; partially revealed puzzle maps intentionally do not expose
  the hidden grail tile even though the client internally needs it for rendering.
- `state.players`: public player status and diplomacy metadata. Each entry uses stable numeric ids for color,
  status, and relation, plus trace-friendly labels and booleans for self/ally/enemy checks. Player-state details
  such as team id and controller type are included only when the normal callback exposes that player state. This
  table intentionally does not expose enemy resources, army strength, hero counts, town counts, or other hidden
  intelligence.
- `state.turn.queries`: typed pending dialog/window queries with query ids, stable `typeId` values, trace-friendly
  type labels, answer ids, and mode-specific numeric fields. Real server queries include `answerAction`,
  `nullkillerAnswerAction`, and
  per-choice `planAction` fields where an answer id exists. Queries that use the player's optional reply channel,
  such as map-object selection windows, expose `cancelAction`; blocking dialogs with a visible cancel button use
  normal `answer_query` answer `0`. Local script-only prompts use their own checked action fields such as
  `ignoreAction` or artifact assembly actions. This is the read-side model for Lua dialog callbacks.
- `actionSpace`: currently legal or relevant high-level candidates.
- `analysis`: host-provided derived facts such as reachability, danger, town build options, recruitment options,
  and object values.
- `analysis.nullkiller.economy`: read-only native economy analysis derived from owned objects, owned towns, and
  current resources. It includes daily income, gold pressure, missing resources now/total, and free resources after
  planned development costs.
- `analysis.nullkiller.townDevelopment`: read-only native BuildAnalyzer development records for owned towns that
  can still build today. Each record includes the town id, town-level development costs, required resources, army
  cost/strength, and `toBuild` / `built` building arrays with stable building, creature, cost, income, prerequisite,
  buildability, and missing-resource fields.
- `analysis.nullkiller.heroRecruitment`: read-only native hero-cap and recruitability analysis. It includes owned
  hero counts, town count, Nullkiller's effective roaming cap, engine on-map/total hero caps, whether the cap is
  reached, whether any owned town can recruit, and per-owned-town recruitability records. Per-town
  `blockedReasonId` values are stable numeric ids: `0` none, `1` no free tavern slot, `2` not enough gold, `3`
  hero cap reached, `4` no available heroes.
- `limits`: time, action count, max candidates, and max memory size limits for this call.

The `ai` facade:

- `ai:state()`, `ai:updates()`, `ai:opponentUpdates()`, `ai:progress()`, `ai:actionSpace()`, `ai:analysis()`,
  `ai:limits()`: read current visible input sections.
- `ai:nullkillerAnalysis()`, `ai:nullkillerState()`, `ai:nullkillerSettings()`, `ai:nullkillerEconomy()`, and
  `ai:nullkillerHeroRecruitment()`: read the native Nullkiller-derived snapshot without hard-coding the nested
  `analysis.nullkiller.*` path in policy code. These are read-only strategy inputs for deciding whether to run
  bounded Nullkiller helpers; scripts still execute through checked host actions.
- `ai:pendingQueries()`: return `state.turn.queries` for dialog/window decisions.
- `ai:memory()` and `ai:setMemory(memory)`: read/replace script-owned memory for this day.
- `ai:inspect(request)`: yield to C++ for a checked read-only binding call against current visible game state.
  Convenience wrappers include `ai:getState()`, `ai:getActionSpace()`, `ai:getAnalysis()`, `ai:getQueries()`,
  `ai:getUpdates(opponentOnly?)`, `ai:getLimits()`, `ai:getGrail()`, `ai:getObject(objectId, heroId?)`,
  `ai:getHero(heroId)`, `ai:getTown(townId)`, `ai:getTile(x, y, z?)`, `ai:getObjectsAt(x, y, z?)`, and
  `ai:getAvailableHeroes(sourceId)`. Movement-specific wrappers include `ai:getPath(heroId, x, y, z?)`,
  `ai:getPathToObject(heroId, objectId)`, and `ai:getReachable(heroId, options?)`; they return current route ids
  and executable `planAction` records, but movement execution still recalculates the route and rejects stale ids.
  Risk wrappers include `ai:getDanger(heroId, target, ...)`, `ai:getTileDanger(heroId, x, y, z?, options?)`, and
  `ai:getObjectDanger(heroId, objectId, options?)`; these ask the native Nullkiller danger evaluator for a visible
  tile or object using an owned visible hero and return stable numeric risk fields (`danger`, `dangerRatio`,
  `riskId`, `targetKindId`, `safe`, and related detail fields).
  Nullkiller-specific read wrappers include `ai:getNullkillerTaskCandidates(mode?, maxCandidates?)`, also exposed as
  `ai:getNullkillerTasks`, `ai:nullkillerTaskCandidates`, and `ai:nullkillerCandidates`; these return native task
  handles and summaries without executing a game action. A script can then pass a selected `task_id` to
  `ai:runNullkillerTask`.
  These calls do not expose hidden map data; unknown, hidden, or invalid targets are host errors that Lua may catch
  with `pcall`.
- `ai:runAction(action)` and `ai:runOption(option, actionField?)`: execute a checked action table directly,
  or execute an action embedded in an action-space option. `actionField` defaults to `planAction`, and can be
  `tasksAction`, `stepAction`, or `passAction` for bounded Nullkiller subroutine options.
  Action dispatch accepts stable numeric `type_id` values from `ai.actionTypeIds`, with the legacy string `type`
  kept for traces and compatibility. When both are present, the host verifies that the id and string agree. Camel-case
  `typeId` remains read-side metadata for objects, queries, and action-space records, not an action dispatch field.
- `ai:tryCall(function, ...)`, `ai:tryExecute(action)`, `ai:tryRunAction(action)`, `ai:tryRunOption(option, actionField?)`,
  `ai:tryInspect(request)`, and `ai:tryRefresh()`: structured wrappers around the same checked host calls. They
  return `{ ok = true, result = ... }` on success, or `{ ok = false, error = "..." }` when the host rejects the
  request or the local facade validation fails. These helpers are for recoverable probes and fallback decisions;
  they do not bypass C++ validation or expose any extra authority.
- `ai:refresh()`: yield to C++ and receive a new visible input snapshot after side effects.
- `ai:build`, `ai:recruit`, `ai:hireHero`, `ai:transferArmy`, `ai:moveHero`, `ai:visitObject`,
  `ai:answerQuery`, `ai:endTurn`: request checked host actions.
  `ai:hireHero(sourceId, heroTypeId, nextHeroTypeId?)` accepts the generic tavern source id; town sources keep
  `town_id` compatibility, and adventure-map tavern sources use the same server-validated `HireHero` request.
- `ai:swapCreatures`, `ai:mergeStacks`, `ai:mergeOrSwapStacks`, `ai:splitStack`, `ai:bulkMoveArmy`,
  `ai:bulkSplitStack`, `ai:bulkMergeStacks`, `ai:bulkSplitAndRebalanceStack`, `ai:dismissCreature`, `ai:upgradeCreature`, `ai:setFormation`,
  `ai:setTactics`, `ai:setTownName`, and `ai:swapGarrisonHero`: request exact army stack, upgrade, formation,
  tactics, town rename, and
  town-garrison operations using stable object, slot, creature, and formation ids.
- `ai:pickBestCreatures(destinationId, sourceId)`: ask the host to run Nullkiller's creature-preparation helper
  for two co-located owned army holders, moving the strongest useful stacks into the destination army.
- `ai:pickBestArtifacts(heroId, otherHeroId?)`: ask the host to run Nullkiller's artifact-preparation helper for
  one owned hero, or two co-located owned heroes, through the normal artifact swap callback path.
- `ai:swapArtifacts(src, dst)`, `ai:bulkMoveArtifacts`, `ai:sortBackpackArtifacts`,
  `ai:scrollBackpackArtifacts`, `ai:manageHeroCostume`, `ai:assembleArtifacts`,
  `ai:disassembleArtifact`, and `ai:eraseTransitionArtifact`: request exact artifact management operations
  through the normal callback/server path. Artifact locations use `{ holder_id, slot, creature_slot? }`.
  Transition artifact erase is intentionally limited to the same illegal transition-slot cleanup accepted by the
  server. Owned hero artifact slots include Nullkiller advisory scores (`nullkillerArtifactScore` and
  `nullkillerPotentialArtifactScore`) so scripts can rank visible artifacts without parsing names.
- `ai:ignoreScriptDecision(queryId)`: clear a script-local decision prompt such as an artifact assembly prompt
  without sending a server `QueryReply`.
- `ai:cancelQuery(queryId)`: send an optional empty `QueryReply` for query records that advertise a
  `cancelAction`, matching the real client path for cancelable generic object-list queries.
- `ai:chooseChestReward(query, preference?)`: answer a chest-style blocking dialog by stable component ids.
  `preference` may be `experience` or `gold`; the default is experience.
- `ai:nullkiller(intent?)` and `ai:nullkillerForRestOfDay(intent?)`: intentionally request full fallback to
  Nullkiller for the rest of the current day. Scripts that only want native help for one decision should use the
  bounded helpers below instead.
- `ai:getNullkillerTaskCandidates(mode?, maxCandidates?)`: read the current native task candidates for a stable
  search mode without executing them. The returned handles are short-lived and should be used before a refresh or
  replanning step invalidates them.
- `ai:runBestNullkillerTask(modeOrOptions?, selector?, maxCandidates?)`: inspect native task candidates, let Lua
  select one candidate with an optional predicate, and execute only that single opaque task handle. The options form
  may pass `mode`, `max_candidates`, and `selector`/`predicate`. If no candidate matches, it returns
  `{ ok = true, executed = false, reason = "no_matching_task" }` without falling back to the rest of Nullkiller's
  day. `ai:runNullkillerCandidate` and `ai:runFirstNullkillerTask` are aliases.
- `ai:nullkillerTrade()`: ask the host to run Nullkiller's resource-trading helper once, returning whether it
  traded anything.
- `ai:nullkillerPriorityPass(passIndex?)`: ask the host to run Nullkiller's bounded native priority pass once.
  This can execute several build/recruit/hire tasks from the priority loop, returns metrics plus the last selected
  priority task in the same stable JSON shape as `ai:nullkillerTasks`, and then gives control back to Lua.
- `ai:nullkillerBuildArmy(townId)`: ask the host to run Nullkiller's bounded town-army helper once for one
  visible owned town. It may upgrade, recruit, and move creatures to the visiting hero, then returns control to
  Lua.
- `ai:nullkillerUpgradeArmy(armyId)`: ask the host to run Nullkiller's bounded possible-upgrades helper once for
  one visible owned army holder, choosing the best affordable upgrade per stack.
- `ai:nullkillerRecruitCreatures(sourceId, destinationId?)`: ask the host to run Nullkiller's bounded recruitment
  helper once for one visible owned dwelling/town. It recruits affordable creatures that fit the destination army,
  including Nullkiller's duplicate-stack merge attempt.
- `ai:nullkillerMoveCreaturesToHero(townId)`: ask the host to run Nullkiller's bounded town-garrison pickup
  helper for one visible owned town with an owned visiting hero. This moves useful garrison stacks to the visitor
  without also running the broader build-army sequence.
- `ai:nullkillerDismissWeakHero(options?)`: ask the host to use Nullkiller's weak-hero selector and send one
  checked dismiss request for the selected owned hero. By default this only acts when Nullkiller's hero cap is
  reached; scripts can pass `{ require_cap_reached = false }` for explicit advanced use, plus optional
  `army_limit` and `town_to_spare_id` numeric fields.
- `ai:nullkillerOptimizeArtifacts(heroId?)`: ask the host to run Nullkiller's bounded artifact cleanup for one owned
  visible hero, or all currently owned visible heroes when `heroId` is omitted. This mirrors the native post-pass
  cleanup step without delegating the rest of the day.
- `ai:nullkillerAddSingleCreatureStacks(heroId)`, `ai:nullkillerRearrangeForWhirlpool(heroId)`, and
  `ai:nullkillerRearrangeForSiege(heroId, townId)`: ask the host to run Nullkiller's bounded army-formation
  helpers for one visible owned hero, with the siege helper additionally requiring a visible enemy town. These
  expose the native pre-whirlpool and pre-siege preparation tactics without delegating the rest of the day.
- `ai:prepareHero(heroId, sourceId?, otherHeroId?)`: run a bounded semantic hero-preparation helper. The host can
  reuse Nullkiller's legal creature rearrangement from a co-located source army, visited owned town, or other hero,
  and artifact optimization for the target hero plus an optional co-located other hero. `include_artifacts` and
  `include_creatures` can be set in the table form to narrow the operation.
- `ai:requestStatistic()`: request the normal player statistics dataset; the server response is mirrored into
  `updates` as `statistics_response`.
- `ai:tradeResources(marketId, sellResourceId, buyResourceId, amount, heroId?)`: request an exact
  resource-to-resource market trade using stable numeric resource ids and a visible market object id.
- `ai:marketTrade({...})` and wrappers `ai:sendResources`, `ai:sellCreatures`, `ai:buyMarketArtifact`,
  `ai:sellArtifact`, `ai:sacrificeArtifact`, `ai:sacrificeArtifacts`, `ai:sacrificeCreatures`,
  `ai:sacrificeCreatureStacks`, `ai:transformToUndead`, and `ai:buySkill`: request every native market mode
  through stable numeric mode/resource/player/slot/artifact/skill ids. Bulk altar helpers use the same vector
  `TradeOnMarketplace` path as the normal UI.
- `ai.artifactSlots` exposes stable numeric artifact positions for `transition`, `firstAvailable`, `altar`, and
  `backpackStart`. `ai:moveArtifactToAltar`, `ai:returnArtifactFromAltar`, `ai:moveArtifactsToAltar`, and
  `ai:returnArtifactsFromAltar` wrap the native artifact exchange/bulk-exchange callbacks for sacrifice-altars.
  Artifact sacrifice is a two-step UI-equivalent sequence: stage artifacts into the altar holder, then send the
  `market_trade` sacrifice action for the staged artifact instance ids. Directly sacrificing artifacts still
  means "sacrifice artifacts already present in the altar storage."
- `ai:dismissHero`, `ai:buildBoat`, `ai:castleTeleport`, `ai:dig`, `ai:castSpell`, `ai:buyArtifact`,
  `ai:spellResearch`, and `ai:visitTownBuilding`: request checked primitive adventure/town actions through the
  normal callback/server path.
- `ai:nullkiller()` / `ai:nullkillerForRestOfDay()`: stop script control and let Nullkiller finish the turn.
- `ai:nullkillerReset()`: reset bounded Nullkiller planner-local state, including resource locks, hero locks,
  scan-depth escalation, and opaque candidate handles, without changing game state or delegating the rest of the day.
- `ai:nullkillerTasks(mode, maxCandidates)`: ask Nullkiller for a bounded snapshot of native task candidates.
  `mode` is `priority`, `adventure`, `startup`, or `all`. Returned `task_id` values are opaque handles that
  expire on `ai:refresh()` or the next candidate snapshot.
- `ai:runNullkillerTask(taskId)`: execute one previously returned native Nullkiller task through C++ validation
  and Nullkiller's normal task machinery, then return control to Lua.
- `ai:runBestNullkillerTask(modeOrOptions?, selector?, maxCandidates?)`: convenience wrapper around
  `ai:getNullkillerTaskCandidates` plus `ai:runNullkillerTask`. This is the preferred shape when Lua wants to ask
  Nullkiller for candidate ideas but still apply script policy before executing exactly one native task.
- `ai:nullkillerStep(mode, maxCandidates, maxAttempts?)`: ask for candidates and execute the best one as a single
  bounded Nullkiller subroutine. Unlike `ai:nullkiller()`, this does not intentionally give away the rest of the
  day.
- `ai:nullkillerPass(modeOrOptions?, maxSteps?, maxCandidates?, maxAttempts?)` and
  `ai:nullkillerAdventurePass(maxSteps?, maxCandidates?, maxAttempts?)`: run several bounded Nullkiller steps as
  one capped native subroutine, defaulting to the adventure task family, then return control to Lua. The pass
  stops early when native failure policy asks to stop, candidates are exhausted, a query needs script input, or
  the cap is reached.
- `ai:nullkillerTurnSlice(optionsOrMaxPasses?, maxCandidates?, maxAttempts?)`: run a bounded native
  Nullkiller-style turn slice, then return control to Lua. A slice performs the same broad structure as one
  native turn pass: priority work, one bounded adventure step, resource trading, and artifact cleanup. Options can
  disable priority/adventure/trade/artifact phases or set an `adventure_mode` numeric task mode when Lua wants a
  specific native behavior family.
- `ai:nullkillerNativePass(passIndex?, maxCandidates?, maxAttempts?)` and
  `ai:nullkillerNativePasses(optionsOrMaxPasses?, maxCandidates?, maxAttempts?)`: convenience wrappers around
  `nullkiller_turn_slice` for the common parity shape "run one native pass" or "run N bounded native passes",
  without delegating the rest of the day. They keep the same checked C++ implementation, default to all native
  phases enabled, and return the same structured slice result.
- `ai:nullkillerBoundedDay(options?)`, also available as `ai:nullkillerRunDay` and `ai:nullkillerNativeDay`: a
  Lua-side control helper that composes native passes one at a time with `max_passes = 1`, optionally answers
  pending queries before each pass, refreshes between successful passes, and returns a summary instead of calling
  full-day fallback. Scripts use this when they want Nullkiller parity while preserving a Lua decision boundary
  between native passes.
- Named wrappers are available for every bounded Nullkiller task mode:
  `ai:nullkillerAllTasks/Step/Pass`, `ai:nullkillerPriorityTasks/Step/Pass`,
  `ai:nullkillerAdventureTasks/Step/Pass`, `ai:nullkillerRecruitHeroTasks/Step/Pass`,
  `ai:nullkillerBuyArmyTasks/Step/Pass`, `ai:nullkillerBuildingTasks/Step/Pass`,
  `ai:nullkillerCaptureTasks/Step/Pass`, `ai:nullkillerClusterTasks/Step/Pass`,
  `ai:nullkillerDefenseTasks/Step/Pass`, `ai:nullkillerEscapeTasks/Step/Pass`,
  `ai:nullkillerGatherArmyTasks/Step/Pass`, and `ai:nullkillerExplorationTasks/Step/Pass`, plus
  `ai:nullkillerStartupTasks/Step/Pass`. These wrappers pass stable numeric mode ids.
- `ai:nullkillerAnswerQuery(queryOrId, defaultAnswer?)`: ask Nullkiller to handle one pending query through its
  native dialog heuristic, then return control to Lua. This is bounded to that one query and does not delegate the
  rest of the day. The Lua wrapper marks snapshot-derived query answers as stale-tolerant, so a query that expires
  before the command reaches C++ becomes a checked no-op instead of normal fallback.
- `ai:nullkillerAnswerPendingQueries(defaultAnswer?, maxQueries?)`: repeatedly apply `ai:nullkillerAnswerQuery` to
  currently pending typed query records, refreshing visible input between answers, then return control to Lua. This
  is a Lua convenience wrapper around the bounded one-query helper, not full-day delegation.
- `ai:nullkillerObjectInteraction(heroId, objectId)`: run Nullkiller's local post-visit helper for one owned hero
  currently visiting or standing at a visible object, then return control to Lua. This covers bounded native
  handling such as owned-town creature pickup, opportunistic spellbook purchase for a main hero, and hill-fort
  upgrades without delegating the rest of the day.
- `ai:nullkillerLockResources(resources)`, `ai:nullkillerLockHero(heroId, reasonId?)`, and
  `ai:nullkillerUnlockHero(heroId)`: constrain later bounded Nullkiller candidate generation and helper calls
  without changing game state or delegating the day. Resources should be passed either as a 7-item amount vector
  in stable resource-id order or as `{ resource_id, amount }` entries. Hero lock reason ids are exposed as
  `ai.nullkillerHeroLockReasons`; the default is `defense`.
- `ai:output(status, intent, confidence)`: return final status plus current memory.

Current API coverage boundary:

- Covered player actions: every adventure-relevant `IGameActionCallback` operation is available through a checked
  Lua action or facade helper: hero movement, hero dismissal, digging, adventure spell casting, hero recruitment,
  town building/visits, creature recruitment/upgrades, spell research, garrison swaps, all market modes, query
  replies/cancel replies, creature stack operations, artifact swaps/sorts/assembly/costumes/altar staging, creature
  dismissal, end turn, artifact purchase, formation/tactics/town naming, boat building, statistics requests, and
  army/artifact bulk operations.
- Intentionally not exposed as game actions: `saveLocalState` is represented by script memory, while `save`,
  `sendMessage`, and `gamePause` are meta-client operations rather than adventure strategy controls. Raw
  `selectionMade` is covered by typed query records plus `answer_query` / `nullkiller_answer_query`.
- Covered Nullkiller integration: Lua can inspect the native Nullkiller snapshot, constrain resources/heroes,
  list task candidates, execute a selected task, run one step, run capped passes/slices, run the priority loop once,
  run native trade, answer one or several queries, run post-object interaction, and run the exposed army/artifact
  preparation helpers. These all return control to Lua instead of intentionally letting Nullkiller finish the day.
- Remaining parity test: if a future script needs a strategy decision that cannot be represented by visible input,
  `analysis.nullkiller.*`, checked actions, or bounded task/helper calls, that is an API gap to add before tuning
  policy logic.

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

Mage-guild spell snapshots expose only spells that are currently visible to a real player. Spell research exposes
the next replacement spell only through explicit `spellResearchOptions`, matching what the research dialog reveals;
deeper queued spells are not exposed to scripts.

## Memory Model

Script memory is a JSON-like value owned by the script and persisted by the AI wrapper.

Rules:

- Memory is strategy state, not game rules state.
- With `reloadScriptEachTurn = false`, the AI keeps one Lua runner instance for the game and invokes `runDay`
  once per scripted day. Lua module locals and script table fields therefore survive across days in that running
  game.
- `reloadScriptEachTurn = true` intentionally recreates the Lua runner every day for script-development reload.
  Long-term strategy must still live in `memory` if it needs to survive reloads, saves, or game restarts.
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

- `nullkiller_tasks` exposes startup tasks from `StartupBehavior`, priority tasks from `RecruitHeroBehavior`,
  `BuyArmyBehavior`, and `BuildingBehavior`, plus adventure tasks from capture, cluster, defense, escape,
  gather-army, and exploration behavior decomposition.
- Task search modes can be aggregate (`priority`, `adventure`, `all`) or granular (`recruit_hero`, `buy_army`,
  `building`, `capture`, `cluster`, `defense`, `escape`, `gather_army`, `exploration`, `startup`). Lua also
  exposes these as numeric constants under `ai.nullkillerTaskModes`, so scripts can avoid magic numbers and
  brittle strings.
- Candidate JSON contains stable machine fields such as `task_id`, `goalTypeId`, `priority`, `priorityTier`,
  `hero_id`, `town_id`, `object_id`, `tile`, affected object ids, and hero role ids. Raw native task debug
  descriptions are intentionally omitted; scripts should use stable ids and typed records for strategy. Candidate
  records also include visible structured `hero`, `object`, `townObject`, and `affectedObjects` payloads where
  those referenced entities are visible to the scripted player.
- Candidate JSON also contains a bounded structured `goal` summary. Its `details` object exposes typed native
  planning context such as composition subtask sequences, hero-chain paths, hero-exchange paths, visible
  defense threats, unlock-cluster blockers, army-upgrade value, building costs, boat locations, and adventure
  spell ids. This is read-only planner state; executing still goes through `nullkiller_task` or
  `nullkiller_step`.
- Native path nodes include a typed `specialAction` object for special movement/planning actions, with stable
  `kindId`/`kind` values for composite, Dimension Door, Town Portal, boat, whirlpool, quest, and adventure-cast
  actions. Scripts should branch on these fields rather than parsing native debug text. Typed parameters are
  exposed as ids and numbers, not localized display strings: Dimension Door and adventure-cast actions include
  `spell_id`, mana and movement accounting, planned cast counts, and guarded landing risk; Town Portal includes
  `spell_id` plus the visible/owned target town id; boat actions include summon `spell_id` or visible shipyard,
  boat type, boat layer, build status, build position, and cost; quest actions include visible quest object ids.
- Modal query records expose stable `query_id`, type ids/names, and answer ids. Level-up, blocking, teleport,
  map-object-select, artifact assembly, tavern, garrison, recruitment, university, and market dialogs include
  typed context where available. Real server query records include a default `answerAction`, a bounded
  `nullkillerAnswerAction`, and `planAction` fields on answer-bearing choices such as level-up skills, blocking
  dialog components, teleport exits, and map-object selections. Teleport and map-object-select choices include
  visible object payloads when the target object is visible to the scripted player, plus `nullkillerPreferred`
  flags and selected answer/object ids when Nullkiller had already set a target that appears in the offered
  choices. Tavern, recruitment, university, and market dialogs reuse the same hire, recruit, army, and
  market-detail payloads exposed in normal action-space snapshots.
- University and market dialog records include `modeDetails` and `skillOptions` entries. `skillOptions` provide
  stable `skill_id`, affordability/learnability flags, gold cost, Nullkiller's advisory `nullkillerSkillScore`
  when the visitor is owned, and a ready checked `market_trade` `planAction` for buying a secondary skill;
  scripts should use these fields instead of parsing dialog text. Level-up skill choices expose the same stable
  skill identifiers plus Nullkiller skill score/preferred-choice fields when the leveling hero is owned. Market
  dialogs with a visiting hero also include `altarOptions`, which expose legal artifact/creature sacrifice candidates,
  expected experience, staged altar contents, and ready action/action-sequence payloads. Artifact options model
  the native UI explicitly: a `swap_artifacts` or `bulk_move_artifacts` staging action must happen before the
  `market_trade` sacrifice action can consume those artifact instance ids.
- `nullkiller_task` executes exactly one stored candidate snapshot through `Nullkiller::executeScriptTask`.
  Native Nullkiller dialog handlers stay active for this path, so the subroutine behaves like Nullkiller rather
  than simplified script auto-answer logic.
- Nullkiller script-task state is reset once at the beginning of the scripted turn, not before every candidate
  query. This preserves native state such as locked heroes, scan-depth changes, previous task success, and
  hero-chain decisions across bounded Lua subroutine calls. Lua can explicitly call `nullkiller_reset` when it wants
  to discard these planner-local constraints and opaque task handles while keeping the actual game state unchanged.
- `nullkiller_step` is a convenience call that selects the current best bounded candidate, can try later
  candidates using Nullkiller's own failure policy, then returns to Lua for refresh, more decisions, or end-turn.
  It accepts `max_attempts` and returns stable fields including `outcomeId`, `failureActionId`, `didExecute`,
  `shouldReplan`, `shouldStopTurn`, `exhaustedCandidates`, `selectedTaskIndex`, `attempts`, and
  `attemptedTasks`. Each attempted task record includes the candidate index, `task_id`, the visible task payload
  when still available, whether it executed, stable failure-action ids, and any native error text.
- `nullkiller_pass` is a capped multi-step wrapper around `nullkiller_step`. It is useful when Lua wants a
  native Nullkiller adventure subroutine larger than one task, but still wants control back before the whole day
  is delegated. It returns a `steps` array plus `executedSteps`, `replanSteps`, `stopTurnSteps`,
  `exhaustedSteps`, `didTrade`, and `paused`.
- `nullkiller_priority_pass` runs Nullkiller's native priority loop once and then returns to Lua. This exposes the
  build/recruit/hire pre-adventure subroutine that native Nullkiller normally performs before adventure task
  planning, without handing over the rest of the day.
- `nullkiller_optimize_artifacts` runs Nullkiller's native artifact cleanup step for one visible owned hero or all
  visible owned heroes, then returns to Lua. This is the standalone form of the artifact phase that native
  `Nullkiller::makeTurn` runs after a successful pass.
- `nullkiller_turn_slice` composes the native priority loop, one bounded adventure step, the resource trader, and
  artifact cleanup into a capped pass-shaped helper. This is the preferred bridge when a Lua policy wants parity
  with the shape of `Nullkiller::makeTurn` but must keep control after one or a few passes. It returns a `passes`
  array plus counters such as `priorityTasksExecuted`, `adventureStepsExecuted`, `tradePasses`, `didWork`,
  `paused`, and `shouldStopTurn`. A full priority+adventure+trade slice runs the native resource trader even if
  priority/adventure produced no task, and reports `shouldStopTurn` when no phase made progress, matching
  Nullkiller's normal "nothing was done this turn pass" stop condition without delegating the rest of the day.
  The bundled default Lua policy now treats productive trade-only slices as real work, refreshes state, and asks the
  script to decide again instead of delegating the rest of the day.
- Bounded native helper loops run their internal server requests in synchronous-realization mode. Lua still
  regains control after the helper returns or pauses for a query, but native helper loops do not queue follow-up
  build, movement, trade, or artifact requests before the previous request has been accepted or rejected by the
  server. Query-answer helpers and explicit `end_turn` stay on the normal query/request-confirmation path instead
  of blocking inside request send.
- After an explicit `end_turn` request is accepted by the server, `ScriptedAdventureAI` marks its local AI status
  as no longer owning the turn. This prevents the script runner from calling the inherited Nullkiller `endTurn()`
  loop a second time after the server has already advanced the active player.
- The cached Lua script instance is single-threaded. `ScriptedAdventureAI` serializes its asynchronous turn tasks
  before entering `runDay`, and stale duplicate turn tasks exit if the turn already ended, including before
  waiting on any old query or movement blocker.
- Lua exposes numeric constants for stable host ids used by the strategic contract:
  `ai.actionTypeIds`, `ai.buildingKinds`, `ai.objectKinds`, `ai.armyTransferKinds`, `ai.queryTypes`,
  `ai.pathActions`, `ai.threatLevels`, `ai.riskLevels`, `ai.specialActionKinds`, `ai.adventureSpellKinds`,
  `ai.nullkillerStepOutcomes`,
  `ai.nullkillerFailureActions`, `ai.nullkillerTaskModes`, `ai.nullkillerPriorityTiers`,
  `ai.nullkillerHeroLockReasons`, and `ai.nullkillerHeroRoles`. Scripts should
  branch on these constants rather than trace strings or raw magic numbers.
- `analysis.nullkiller.settings` exposes Nullkiller's read-only operational thresholds, including max pass counts,
  safe attack ratio, retreat thresholds, army-loss target, hero roaming limits, pathfinder limits, and enabled
  native features. `analysis.nullkiller.state` exposes current bounded-planner state such as scan depth,
  open-map/object-graph flags, pathfinder storage misses, locked resources, and free resources. Scripts can use
  these fields to align policy with native Nullkiller without hard-coding engine constants.
- `analysis.nullkiller.heroRecruitment` exposes `HeroManager`'s read-only hero-cap and recruitability view using
  stable numeric ids. Lua policy can decide when to prefer `hire_hero`, bounded `recruit_hero` subroutines, or no
  hire attempt without duplicating Nullkiller's cap checks.
- Script-facing bounded Nullkiller candidate generation does not invoke Nullkiller's map-reveal helper. Candidate
  task handles may still be opaque native planner handles, but the JSON surface only exposes target object ids,
  affected object ids, goal tiles, and detailed path nodes when the referenced objects/tiles are visible or owned
  by the scripted player.
- Script-facing bounded Nullkiller candidate generation and execution also force native `openMap` and object-graph
  planning off for the duration of the helper call. This prevents Lua from using an opaque native task handle to
  act on hidden-map knowledge even when native Nullkiller would be allowed to use AI-only open-map mode elsewhere.
- `actionSpace.nullkillerSubroutineOptions` exposes the bounded task families as first-class candidate actions.
  Each option contains `tasksAction`, `stepAction`, and `passAction` payloads using numeric `mode` values, so Lua
  can discover and compose native subroutines the same way it discovers movement, build, recruitment, and
  preparation candidates. `actionSpace.nullkillerHelperOptions` similarly advertises bounded native helper calls
  such as the priority pass, resource trader, town-army preparation, army upgrades, and owned-dwelling
  recruitment. These options are not automatically added to `recommendedActions`.

## Current API Coverage Stance

The Lua bridge is an adventure-AI policy API, not a general client automation API. Current coverage targets the
meaningful choices a computer player can make during adventure turns: movement and object visits, town building and
building visits, creature recruitment and upgrades, hero hire and dismissal, army stack management, artifact
management, market trades, adventure spells, boat building, town/garrison actions, query answers, memory,
refresh/replan, and end turn.

API parity is the gate before strategic script tuning. A weak result from a Lua policy must first be classified as
one of two things:

- a policy mistake, where Lua had enough state/actions/helpers and chose badly
- a host capability gap, where the only practical way to use existing Nullkiller competence was full-day
  `ai:nullkiller()` delegation

Only the first category should drive script optimization. The second category should produce a checked action,
read-only analyzer field, or bounded Nullkiller helper that returns control to Lua after one defined subroutine.
This keeps the editable script in charge of strategy while still allowing it to reuse hard native systems such as
pathfinding, decomposed tasks, logistics, dialog heuristics, and artifact/army preparation.

At the raw player-action layer, the Lua facade covers the meaningful `IGameActionCallback` adventure actions:
destination/pathfinder-based hero movement, hero dismissal, digging, adventure spell casts, hero recruitment,
building construction and visits, creature recruitment and upgrades, spell research, garrison swaps, market trades,
query replies/cancels, army stack operations, artifact operations, end turn, artifact purchases, formation/tactics,
town rename, boat building, statistics requests, and bulk army/artifact management. The path-form native movement
callback is represented through checked route ids and submitted paths derived from the shared pathfinder, not by
letting scripts push arbitrary hidden path vectors.

Bounded Nullkiller helpers cover native subroutines that are expensive or brittle to reimplement in Lua: planner
reset, task candidate generation and execution by mode, one-step/pass/slice execution, priority passes, resource
trading, town army preparation, creature recruitment, army upgrading, town-garrison pickup, weak-hero dismissal,
single-creature stack setup, whirlpool formation, siege formation, one-query and pending-query answering, object
interaction callbacks, artifact preparation, all-hero artifact optimization, creature preparation, and combined hero
preparation.

The intentionally excluded `IGameActionCallback` methods are meta/client operations rather than adventure strategy:
save, pause, chat/message sending, and raw local-state writes. Script-owned memory replaces raw local-state writes,
and chat/message sending is not exposed because it can trigger cheat-like text commands in some contexts. If future
work finds another real adventure decision still reachable only through whole-day Nullkiller delegation, it should
be added as either a checked action or a bounded helper before tuning the Lua policy.

## Current Parity Audit

The current Lua API is close to the intended parity boundary for adventure AI policy:

- Nullkiller's normal `makeTurn()` phases are available without full-day delegation: priority tasks
  (`nullkiller_priority_pass`), adventure task search/execution (`nullkiller_tasks`, `nullkiller_step`,
  `nullkiller_pass`), resource trading (`nullkiller_trade` or the trade phase inside `nullkiller_turn_slice`),
  and artifact cleanup (`nullkiller_optimize_artifacts` or the artifact phase inside `nullkiller_turn_slice`).
- Player-visible callback actions that matter for adventure strategy are exposed as checked actions or typed
  query answers. The missing raw callbacks are intentionally outside AI strategy: save/pause/message/local-state
  plumbing.
- Read-side Lua bindings can inspect the current visible state on demand through `ai:inspect` / `ai:get*`
  helpers. The host validates visibility and ownership-sensitive detail level on each inspect call, so scripts can
  use binding-style reads after side effects without scanning stale day-start snapshots or accessing hidden data.
- Native helpers that can perform multiple internal requests use bounded calls and then return to Lua. This is the
  required shape for script-driven strategy: Lua can ask Nullkiller to do one pass, one task, one preparation helper,
  or one dialog answer, then inspect refreshed state and decide what comes next.
- Runtime progression has a passing corrected smoke check: `ScriptedAdventureAI` versus canonical `Nullkiller2`
  on a small two-level random map reached a three-day headless day limit with trace output. Earlier one-day
  timeouts were caused by using the non-canonical AI name `Nullkiller`, which the engine treats as an unknown
  adventure AI and silently maps to EmptyAI behavior. The Python batch/evaluation tooling now normalizes that
  shorthand to `Nullkiller2` before launch.

The script engine should reuse these Nullkiller systems where possible:

- analyzers for hero roles, builds, dangers, and reachable objects
- pathfinder and route ids
- object clusterizer and blocker information
- deep decomposer for quest/guard/unlock chains
- task execution wrappers for complex multi-step behaviors

Current read-only Nullkiller analyzer exposure:

- Owned hero records include `nullkillerRoleId` / `nullkillerRole`, using the same main/scout labels as
  `HeroManager`. Lua mirrors the stable numeric values through `ai.nullkillerHeroRoles`.
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
scripts/ai/candidates/boundedNullkillerControl.lua
scripts/ai/candidates/fallbackAdventure.lua
scripts/ai/candidates/statisticsProbeAdventure.lua
```

Example configuration fields:

```json
{
  "script": "ai/candidates/boundedNullkillerControl.lua",
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
`economyAdventure.lua`, and `explorerAdventure.lua` opt-in until they beat the bounded control.

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
script intents, final visible-state quality, longitudinal map-progress/control deltas, and mined policy-mistake counts. Imperative runs emit
`imperative-input`, `imperative-command`, and `imperative-output` trace events. Add `--json` for
machine-readable output.

The current quality score is intentionally trace-local and explainable. It uses the final visible input for each
scripted run/player series and combines resources, towns, heroes, army strength, movement, useful candidates, and
visible threat pressure. The separate `mapProgress` metric compares the first and final visible map snapshots for
each run/player series, including explored tile deltas, terrain-class deltas, and visible object control/kind/owner
deltas. These metrics are not replacements for win/loss, but they give the improvement loop stable control signals
between full outcome runs.

Trace sets from two script versions can be compared with:

```bash
scripts/ai/compareAdventureTrace.py <baseline-trace-dir> --candidate <candidate-trace-dir>
```

The comparison reports deltas for fallback outputs, failed actions, unsafe candidates, hero/town threat alerts,
executed actions, mined mistakes, important mistakes, trace-local quality, and map-progress score. This is
intentionally trace-based so it can compare script versions without rebuilding.

Headless batches can be launched with:

```bash
scripts/ai/runAdventureAIBatch.py \
  --client <build-dir>/bin/vcmiclient \
  --map "Maps/Dwarven Gold.h3m" \
  --ai ScriptedAdventureAI --ai Nullkiller2 \
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
Use `Nullkiller2` as the canonical native adventure AI name in commands. The runner accepts `Nullkiller` as a
compatibility shorthand and rewrites it to `Nullkiller2`; the engine factory itself does not recognize that
shorthand.
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
quality, map-progress, action, and mistake metrics, but promotion/control runs can pass `--no-trace` to avoid trace
I/O changing timing-sensitive full-game outcomes. With `--no-trace`, trace-local metrics are zeroed and scoring is
focused on run safety plus win/loss outcomes. Use traced reruns on selected failures for diagnosis and JSON fixture
mining.
The score is not a gameplay rating; it is an iteration signal that rewards completed runs, useful actions, final
visible-state quality and map-progress/control gains when traces are enabled, and real red-player wins while
penalizing timeouts, nonzero exits, parse errors, fallbacks, failed actions, stopped batches, mined mistakes, and
red-player losses. The evaluator writes global metrics plus bucketed metrics by `group`, `stage`, and `kind`.

Promotion gates now encode the training/held-out workflow: the candidate must improve on the `training` bucket and
must not regress on the `heldout` bucket when those buckets are present. The fixed-seed random-map entries should be
generated once and kept stable, then split between training and held-out groups. Script authors may iterate against
training maps, but promotion should continue to require held-out non-regression. Trace-local quality and
`mapProgress` remain control metrics; decisive promotion still requires outcome runs.

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

The current packaged scripted-AI default is `scripts/ai/candidates/boundedNullkillerControl.lua`. It is an API
parity control: Lua owns the day loop, calls bounded Nullkiller subroutines, refreshes after side effects, and avoids
normal full-day delegation. `scripts/ai/candidates/fallbackAdventure.lua` remains the full-day native Nullkiller
baseline/control for outcome comparisons. `scripts/ai/defaultAdventure.lua` is still the readable experimental policy
and fixture target, but recent no-trace full-game runs show it oversteers Nullkiller and should not be promoted over
the bounded control until it wins repeated training runs and does not regress held-out runs.

`scripts/ai/candidates/statisticsProbeAdventure.lua` is an API smoke script, not a promotion candidate. It
requests the normal statistics dataset, refreshes to observe `statistics_response`, records a small memory flag,
runs one bounded native turn slice, and then ends the turn through the script facade.

This enables the intended loop:

1. Put a candidate Lua file under `scripts/ai/candidates/` or another local path.
2. Run baseline-vs-candidate promotion scenarios in no-trace mode against the bounded control and keep the full
   fallback script as a native-Nullkiller reference.
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
- Script action dispatch now has stable numeric action ids. `actionSpace.acceptedActions` lists `{ type, typeId }`
  records, Lua publishes the same ids under `ai.actionTypeIds`, and the host accepts numeric-only `type_id` actions for
  checked execution. String `type` fields remain compatibility and trace affordances, not the preferred policy
  branching surface. Host-published executable action records such as `planAction`, `answerAction`, `stepAction`,
  `passAction`, `rerollAction`, and `endTurnAction` include `type_id` at the source.
- Pending query records expose stable numeric `typeId` values, mirrored by `ai.queryTypes`, so Lua can branch on
  dialog/window kinds without parsing trace labels.
- Script input now includes complete player-visible `state.map.visibleTiles` terrain records, tile-level visible
  visitable/blocking object ids, and complete player-visible `state.map.visibleObjects`, plus full
  `state.ownedObjects` from the player-specific owned-object callback. Visible map data is produced through
  fog-of-war checks and should be treated as ordinary player information, not hidden AI state.
  Candidate/action-space arrays may still be capped because they are helper recommendations, not the authoritative
  visible map.
- Visible quest objects now expose known requirements as stable ids after they become active for the player:
  mission id, last day, required resources, artifacts, creatures, skills, heroes/classes, players, spells, nested
  limiter counts, kill targets, and `canCompleteWithContextHero` for reachable-object context.
- Script input now includes the current player quest log under `state.quests`, using the same visible-only object
  details as the quest-log callback mirror. Scripts no longer need to wait for a quest-log window event to reason
  about accepted quests and known quest gates.
- Candidate actions now include `hire_hero` and `transfer_army` for two previously missing Nullkiller-level
  capabilities: adding tavern heroes and concentrating or reinforcing armies through the normal server-validated
  request path. Their candidate generation is controlled by `experimentalSupportActions` in
  `config/ai/scriptedAdventure.json`, so the loop can disable them for bisection without rebuilding. `hire_hero`
  uses `source_id` as the canonical tavern source field while keeping `town_id` compatibility; pending
  adventure-map tavern windows can expose the same checked hire action through `hireHeroOptions`.
- Lua can now call `ai:pickBestArtifacts(heroId, otherHeroId?)` to reuse Nullkiller's artifact-preparation helper
  for one owned hero or two co-located owned heroes. Scripts that need exact control can use the artifact-slot
  operations described below instead of treating the helper as the only artifact API.
- Hero input now includes artifact state: worn slots and backpack entries expose stable slot ids, artifact type ids,
  artifact instance ids, lock state, possible slot ids, and Nullkiller's read-only artifact scores for that hero
  and artifact type. Lua can request exact artifact swaps, bulk transfers, backpack sorting/scrolling, hero costume
  operations, artifact assembly, and artifact disassembly through checked host calls. Assembly prompts are exposed
  as typed script-local decisions with stable artifact ids; Lua may choose an offered assembly action or explicitly
  ignore the prompt without sending a normal server query reply.
- Hero input now exposes `formationId` and `tacticsEnabled`, and Lua can request exact creature stack
  rearrangement, stack splitting, stack merging, creature dismissal, creature upgrades, formation/tactics changes,
  and town garrison-hero swaps through the same checked callback/server packet path used by native clients and AI.
- `actionSpace.upgradeCreatureOptions` now lists currently available upgrades for owned town and hero armies using
  `fillUpgradeInfo`: source army id, slot, old/new creature ids, count, per-unit and total costs, affordability,
  and estimated value delta.
- Lua can now call `ai:nullkillerTrade()` to run Nullkiller's build-driven resource trader once,
  `ai:tradeResources(marketId, sellResourceId, buyResourceId, amount, heroId?)` for the common exact
  resource-to-resource path, or `ai:marketTrade({...})` and its mode-specific wrappers for every native
  `EMarketMode`. Visible safe market objects expose supported market modes plus `modeDetails` and resource
  market rates when the details are available to the player; pending market dialogs expose the same detail
  shape.
- Script input now includes typed `state.turn.queries` records for level-up, blocking, teleport, object-selection,
  tavern, hero-exchange, garrison, recruitment, university, market dialogs, and script-local artifact assembly
  prompts. Dialogs raised during direct Lua actions now pause the action with `pending_query = true`; Lua can
  `refresh()`, inspect `state.turn.queries`, answer server dialogs through `ai:answerQuery(queryId, answer)`, and
  clear script-local prompts through `ai:ignoreScriptDecision(queryId)`.
- Treasure chests and sea chests are eligible `visit_object` candidates again. Their gold/experience choice is
  handled as a normal blocking dialog; Lua can answer it directly or use `ai:chooseChestReward`, and the default
  script prefers experience when both stable reward components are present.
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
- The imperative Lua path is now present as `runDay(ai, input)`. The runner executes it as a coroutine and handles
  yielded `execute`/`refresh`/`fallback` commands. The legacy `planDay(input)` path remains only as a direct
  runner/testing compatibility API; active `ScriptedAdventureAI` turns require `runDay` and fall back to
  Nullkiller when it is missing.
- The script API parity target is practical rather than “bind every C++ private method”: Lua should see all
  relevant visible/derived strategy data, execute every useful player action through existing server-checked
  callback paths, and invoke Nullkiller logic only through bounded helpers such as planner reset, task candidate
  listing, one selected task, one step, one pass, one turn slice, one priority pass, resource trade, army/artifact
  preparation, query answers, and object interaction.
- Bounded Nullkiller helpers now run with a visible-only script memory view. `ScriptVisibleOnlyScope` temporarily
  filters Nullkiller's remembered objects, teleport channels, and subterranean-gate links to objects visible to
  the scripted player, while preserving hidden native Nullkiller memory outside the scoped script helper. Task
  candidate generation also uses a visible-only memorizer instead of seeding from the full map.
  A smoke run on `Dwarven Gold` and `Ready or Not` reached the one-day limit cleanly and wrote
  `imperative-input`/`imperative-output` traces with fallback status through the configured fallback script.
- A one-day `Dwarven Gold` ScriptedAdventureAI-vs-Nullkiller smoke after this containment change exited cleanly
  at the day limit and parsed 8 trace files.
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
  Several follow-up API gaps are now closed by checked support actions and bounded Nullkiller helpers; remaining
  work should keep classifying failures as API parity gaps before treating them as Lua policy tuning.
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
  task left. This confirms the Lua coroutine can call native Nullkiller task fragments and regain control.
  Bounded step responses now include `attemptedTasks`, so failed or exhausted native task searches provide
  per-candidate replan/try-next details to Lua and trace analysis.
- Lua can now apply current-turn Nullkiller planner constraints before calling bounded native helpers:
  resource locks preserve strategic reserves for later candidate generation and hero locks keep a selected owned
  hero out of subsequent native task searches. These are AI-planner constraints only; they do not mutate game
  state and are reset by the normal per-turn Nullkiller script-task state reset unless Lua records and reapplies
  the intent from script memory.
- The same debugging pass found two opposite modal-query hazards. First, Lua-owned `visit_object` actions could
  leave clients asleep on a stale modal query, for example a garrison dialog opened by movement, so scripted query
  replies are now always sent asynchronously. Second, delegating rich modal callbacks to Nullkiller during a
  script-owned action let native helper code perform hidden side effects, especially hero exchange artifact/army
  rearrangement. Fallback turns still use native Nullkiller dialog handling, but script-owned movement/object
  actions now answer garrison, hero-exchange, recruitment, teleport, map-object-selection, and blocking dialogs
  through the scripted executor boundary. This is a containment fix, not the final strategy interface.
- Dialogs are now typed facade decision points for the main adventure AI cases. Lua receives visible
  dialog/query data, direct actions pause with `pending_query`, and scripts can call helpers such as
  `ai:chooseChestReward`, `ai:nullkillerAnswerQuery`, or `ai:answerQuery` with stable ids. `ai:nullkillerAnswerQuery`
  is the explicit bounded escape hatch for reusing Nullkiller's native dialog handling without restoring hidden
  callback side effects for every scripted action. Future API work can add more semantic per-dialog helpers, but
  unplanned dialogs no longer require full-day Nullkiller delegation.
- Artifact and army rearrangement are now explicit strategy capabilities rather than hidden callback behavior.
  Current facade methods include exact stack/artifact operations plus semantic helpers such as `ai:prepareHero`,
  `ai:transferArmy`, `ai:pickBestCreatures`, and `ai:pickBestArtifacts`. Future wrappers may add richer intent
  labels such as `combat`, `mobility`, `scout`, `defend_town`, or `deliver_army`, but the current API can already
  route these decisions through checked C++ actions and trace the resulting transfers.
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
  `explorerAdventure.lua` lost 10/10. The packaged config previously used `ai/candidates/fallbackAdventure.lua` so
  normal ScriptedAdventureAI runs started from the measured native control. It now uses
  `ai/candidates/boundedNullkillerControl.lua` so normal runs exercise the bounded Lua API without rebuilding.
  Experimental profiles remain available through per-player config or environment overrides.
- `scripts/ai/runAdventureAIBatch.py` terminates a stale client process after a terminal game outcome has appeared
  in stdout and a short grace period has elapsed. This keeps unattended evaluation batches from hanging while still
  recording the completed outcome and traces.
- Debugging `Emerald Isles` smoke runs showed the scripted host must not use Nullkiller helper methods that perform
  hidden side effects such as army exchange after movement. Scripted movement is now a direct, tracked `MoveHero`
  request over a route-id-validated path, and garrison/hero-exchange/recruitment dialogs are conservatively answered
  without implicit stack management.
- Scripted build, recruit, move, end-turn, memory-save, and blocking-dialog query replies tolerate
  `requestSent`/`requestRealized` callback ordering differences. This prevents trace I/O from masking timing bugs.
- Generic `visit_object` candidates include treasure chests and sea chests again; scripted movement pauses on the
  reward dialog and resumes after Lua answers it by stable component ids.
- Invalid or rejected actions are passed back to the script as `progress.failed` for bounded replanning. Nullkiller
  fallback remains for script failures, script-requested fallback, repeated failures, or exhausted script-call budget.
- Further C++ expansion should be demand-driven: expose additional read-only analyzer fields when a Lua policy
  needs them, while using bounded Nullkiller task fragments for native blocker/unlock chains, army gathering, and
  pathfinder-owned decompositions.

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

- Done: load a restricted Lua runner and call legacy `planDay(input)` for tests/tooling compatibility.
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
- Done: on turn start, it calls imperative `runDay(ai, input)` as a coroutine and executes yielded checked
  actions. Scripts without `runDay` fall back to Nullkiller instead of running legacy plan batches.
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
- Done: script input includes map dimensions, complete visible terrain tiles, tile-level visible
  visitable/blocking object ids, and complete visible objects gathered through player-specific fog-of-war checks.
- Done: action candidates include allowed build actions, affordable recruitment actions, route-id guarded
  movement actions, reachable object targets, and end turn.
- Done: route ids are generated with the same shape as MCP route ids and are validated before execution.
- Done: visible enemy heroes/towns and nearby town defense alerts are exposed in `analysis`.
- Done: movement/object candidates include read-only `reason`, `value`, `risk`, `safe`, `danger`, `dangerRatio`,
  `estimatedLoss`, and `blockedBy` fields.
- Done: nearby visible enemy pressure against owned heroes is exposed as `analysis.heroThreatAlerts`.
- Done: owned hero records expose Nullkiller's main/scout role plus fighting and magic evaluator scores, so Lua
  does not need to re-infer hero roles from scratch.
- Done: bounded Nullkiller analyzer exports include visible enemy threat tiles and visible locked-object clusters.
  Threat tiles include only currently visible enemy heroes; clusters include only visible blockers and visible
  blocked objects, with caps for trace size. These exports are passive: script input reads already prepared
  Nullkiller analyzer state and does not recompute the planner as a side effect.
- Done: bounded Nullkiller task fragments are exposed through `ai:nullkillerTasks`, `ai:runNullkillerTask`,
  `ai:nullkillerStep`, and capped `ai:nullkillerPass` / `ai:nullkillerAdventurePass` helpers. The bounded step
  now preserves Nullkiller script-task state across a scripted turn, returns structured execution outcomes, and
  can be restricted to granular behavior families such as defense, gather-army, exploration, building,
  recruitment, startup, or capture. Candidate snapshots now include bounded structured goal details for
  composition plans, hero-chain paths, cluster blockers, defense threats, upgrades, buildings, boats, and
  adventure spells. Bounded step responses include per-attempt task diagnostics, so Lua and trace tooling can see
  which native candidates were tried, skipped, executed, or used to trigger replan/stop-turn policy. Single-query
  Nullkiller dialog handling is also exposed through `ai:nullkillerAnswerQuery`. The native priority-pass loop is
  exposed through `ai:nullkillerPriorityPass`, and the pass-shaped bridge is exposed through
  `ai:nullkillerTurnSlice` for scripts that want Nullkiller-style priority/adventure/trade phases without handing
  over the rest of the day.
  This closes the full-day delegation gap for native task families: Lua can choose, rank, and run bounded native
  subroutines, then refresh visible state instead of handing Nullkiller the rest of the turn.
- Contract: before optimizing a Lua policy, treat any required `ai:nullkiller()` / full-day fallback as a parity
  bug unless the script deliberately uses it as a safety escape. The preferred integration point is a checked
  action, read-only candidate/analyzer field, or bounded Nullkiller helper that returns control to Lua.
- Done: checked script actions can now be dispatched by stable numeric `type_id`, with `type` strings verified when
  present. This reduces string drift at the Lua/C++ boundary while preserving existing scripts and trace readability.
  Host-generated executable action records are annotated with `type_id`, so scripts can rank and dispatch candidate
  actions without parsing action-name strings.
- Done: Lua has checked read-side binding calls through `ai:inspect` and typed `ai:get*` wrappers for current
  visible state, action space, analysis, queries, updates, limits, visible objects/heroes/towns/tiles, visible
  objects at a tile, visible tavern hero availability, on-demand path lookup, and bounded current-turn reachable
  tile/object lookup. Invalid or hidden targets raise host errors that scripts can catch; uncaught errors keep the
  normal Nullkiller fallback path. Returned route ids are advisory and are revalidated by checked movement actions.
- Bounded Nullkiller helpers that invoke native task decomposition, pathfinding, task execution, priority passes,
  or resource trading use the same shared pathfinder-storage lock as native `Nullkiller::makeTurn`; add new native
  subroutines at that primitive boundary rather than around higher-level Lua loops to avoid nested lock attempts.
- Done: Lua can set current-turn Nullkiller planner constraints through bounded resource and hero locks, then
  call native task candidates, steps, passes, or turn slices with those constraints still in force. This is the
  script-side counterpart to Nullkiller's native `SaveResources` and `StayAtTown` planner effects.
- Done: Nullkiller path-node special actions are serialized with stable typed metadata, so Lua can identify and
  inspect native Dimension Door, Town Portal, boat, whirlpool, quest, adventure-cast, and composite path actions
  without relying on debug strings. The exposed action parameters use ids/numeric fields where possible and keep
  target object details behind visibility checks.
- Done: Nullkiller settings and current bounded-planner state are exposed under `analysis.nullkiller` as read-only
  numeric/boolean data, so Lua policy can use native pass limits, risk thresholds, scan depth, and resource locks
  without copying constants from C++.
- Done: Nullkiller task candidates include visible structured hero/object/town context in addition to stable ids,
  so Lua can rank native fragments by typed object data instead of relying on native debug descriptions.
- Done: owned hero/town snapshots expose richer inspectable state for script decisions: stable hero type/class,
  faction, creature, secondary-skill, building, spell, and artifact identifiers; hero progression and secondary
  skills; town hall/fort/mage-guild/town levels; built/destroyed counts; building detail records; owned mage-guild
  spells; dwelling pools/growth; horde structures; and blacksmith war-machine availability. Owned-only details
  stay out of visible enemy town/hero analysis to avoid turning the script bridge into a hidden-information path.
- Done: script input exposes `state.ownedObjects` from the normal player-specific owned-object callback, so Lua can
  reason over flagged mines/dwellings/assets independently from visible-map scans.
- Done: `analysis.nullkiller.economy` exposes Nullkiller's build-analyzer economy summary: daily income, gold
  pressure, missing resources, and free resources after planned development costs. These are derived from
  player-owned objects/towns and current resources, not hidden map data.
- Done: `analysis.nullkiller.townDevelopment` exposes Nullkiller's BuildAnalyzer town-development records for
  owned towns, including current native `toBuild` / `built` building info and town-level cost summaries. This lets
  Lua policy reason over Nullkiller's long-term building queue without delegating the full priority pass.
- Done: `analysis.nullkiller.heroRecruitment` exposes Nullkiller's hero-cap and recruitability analysis, including
  owned hero counts, cap state, recruitable owned towns, available hero type ids, and stable numeric blocker ids.
  This lets Lua make hire/defer decisions with the same native constraints while still executing only checked
  `hire_hero` actions or bounded Nullkiller recruitment subroutines.
- Done: known quest requirements are exposed on visible quest objects and current quest-log entries using stable
  ids without revealing inactive quest internals. Lua can reason about known blockers, while still using movement
  actions or bounded Nullkiller tasks for actual unlock-chain execution.
- Done: `actionSpace.questObjectOptions` indexes visible quest/guard/gate objects directly and reports which
  owned heroes can already complete each visible active quest. The actual visit still uses route-checked
  `reachableObjects` or bounded Nullkiller tasks.
- Done: owned hero artifact state, Nullkiller artifact scores, exact artifact management calls, typed artifact
  assembly prompts, checked assemble/disassemble actions, and transition-slot artifact cleanup are exposed through
  Lua facade methods. Remaining artifact work is higher-level artifact intent helpers.
- Done: exact army stack management, bulk army moves, creature upgrades, upgrade candidates, formation/tactics
  changes, and town garrison-hero swaps are exposed through checked Lua facade methods. The native merge-or-swap
  helper and town rename callback are also exposed for raw player-action parity.
- Done: Lua can call Nullkiller's bounded creature-preparation helper through `ai:pickBestCreatures`, matching
  the existing artifact-preparation helper and avoiding Lua-side reimplementation of stack logistics.
- Done: Lua can call Nullkiller's bounded town-army helper through `ai:nullkillerBuildArmy(townId)`, reusing the
  native upgrade/recruit/move-to-hero sequence for one visible owned town without delegating the rest of the day.
- Done: Lua can call Nullkiller's bounded army-upgrade helper through `ai:nullkillerUpgradeArmy(armyId)`, reusing
  native affordable upgrade selection for one visible owned army holder.
- Done: Lua can call Nullkiller's bounded recruitment helper through
  `ai:nullkillerRecruitCreatures(sourceId, destinationId?)`, reusing native affordable recruitment and stack-fit
  handling for one visible owned dwelling or town.
- Done: Lua can call `ai:nullkillerMoveCreaturesToHero(townId)` to reuse Nullkiller's native town-garrison pickup
  helper independently from the broader `nullkillerBuildArmy` sequence.
- Done: Lua can call `ai:nullkillerDismissWeakHero(options?)` to reuse Nullkiller's weak-hero selector for one
  checked dismissal attempt without handing over the remaining day. The helper is also discoverable through
  `actionSpace.nullkillerHelperOptions` when Nullkiller currently sees a cap-driven weak-hero dismissal candidate.
- Done: Lua can call `ai:nullkillerOptimizeArtifacts(heroId?)` to reuse Nullkiller's native post-pass artifact
  cleanup for one visible owned hero or all visible owned heroes. The all-hero helper is also discoverable through
  `actionSpace.nullkillerHelperOptions`, so scripts can explicitly compose artifact cleanup around custom actions or
  bounded task calls without running a full turn slice.
- Done: Lua can call `ai:prepareHero` for semantic hero preparation without full-day delegation. The checked host
  call combines Nullkiller's legal best-creature transfer and artifact optimization for an owned target hero, an
  optional co-located source army, visited owned town, and optional co-located other hero. `actionSpace` exposes
  `prepareHeroOptions` so scripts can discover these opportunities from visible state instead of reimplementing
  the co-location scan.
- Done: `actionSpace` advertises bounded Nullkiller subroutines through `nullkillerSubroutineOptions` and
  `nullkillerHelperOptions`. Scripts can execute the advertised `planAction`, `tasksAction`, `stepAction`, or
  `passAction` through `ai:runOption`, keeping native helper use discoverable and numeric-id based without
  delegating the rest of the day.
- Done: Lua can call `ai:nullkillerReset()` and discover `nullkiller_reset` in `nullkillerHelperOptions` to clear
  bounded planner-local state and candidate handles mid-script. This is useful after experimental resource/hero
  constraints or scan-depth escalation when Lua wants a fresh native planning pass without changing game state.
- Done: `nullkillerHelperOptions` also includes concrete visible owned-object helpers for
  `nullkiller_build_army`, `nullkiller_upgrade_army`, `nullkiller_recruit_creatures`, and
  `nullkiller_move_creatures_to_hero`, plus the cap-driven `nullkiller_dismiss_weak_hero` helper when available,
  so Lua can discover the semantic native logistics helpers instead of hard-coding town, army, or dwelling scans.
- Done: Lua can explicitly call Nullkiller's bounded army-formation helpers for single-stack splitting,
  whirlpool preparation, and siege preparation. `actionSpace.nullkillerHelperOptions` advertises these helpers for
  visible owned heroes and visible enemy towns, covering native tactics that previously were only reachable as side
  effects of larger movement/task execution.
- Done: `nullkillerHelperOptions` includes a bounded `nullkiller_turn_slice` helper. This gives scripts a
  discoverable way to run one native pass-shaped slice and inspect the result, which is the right baseline before
  tuning custom Lua strategy against Nullkiller.
- Done: pending dialog/window queries are exposed as typed read-side data under `state.turn.queries`, and direct
  Lua actions that open these dialogs now pause for a script answer instead of auto-answering. The bundled default
  script includes a conservative fallback answer policy; richer per-dialog strategy remains Lua policy work.
- Done: real server query records now include executable `answerAction`, `nullkillerAnswerAction`, and per-choice
  `planAction` payloads where answer ids exist, so scripts can use the same `ai:runOption` pattern for dialog
  decisions that they use for action-space candidates. Generic map-object selection records also expose
  `cancelAction`, allowing Lua to send the same optional empty `QueryReply` as the real client when closing the
  object-list window.
- Done: teleport and map-object-selection query choices expose Nullkiller's current preferred offered object or
  exit when it is one of the choices already shown to the player, closing the read-side object-selection policy
  gap without revealing hidden objects.
- Done: university and market dialog queries now expose stable mode/item details and buy-skill options with
  checked `market_trade` actions, closing one read-side gap for player-choice dialogs without changing native
  trade validation.
- Done: hero level-up and university/market skill choices expose Nullkiller's advisory secondary-skill scores
  for owned heroes, letting Lua rank skills with native policy data instead of delegating the whole dialog or
  parsing localized text.
- Done: Lua can ask Nullkiller to answer a single pending query through `ai:nullkillerAnswerQuery`, preserving
  native bounded handling for level-up skills, cautious yes/no prompts, teleport and map-object choices, hero
  exchanges, garrison pickup, dwelling recruitment, and simple window-closing dialogs without surrendering the
  rest of the day. Teleport and map-object-select query options now include visible object records, so scripts
  can choose by object type, owner, position, or quest state without parsing dialog strings.
- Done: `ai:nullkillerAnswerQuery` marks snapshot-derived answers with explicit stale-query tolerance. If a query
  expires between `ai:refresh()` and the answer command, the checked host call returns a successful no-op instead
  of forcing full-day fallback; raw `runAction({ type = "nullkiller_answer_query", ... })` remains strict unless
  the script explicitly sets the same option.
- Done: player-visible non-query adventure windows are mirrored into the script update journal. This covers
  generic info dialogs, shipyard dialogs, hill-fort windows, and thieves-guild windows, with stable component,
  object, hero, and shipyard fields where available.
- Done: additional player-visible adventure callbacks are mirrored into the script update journal, including
  hero stat/mana/skill/bonus changes, garrison changes, artifact movement, available creature/artifact changes,
  tile hiding, resource receipts, and adventure spell casts. Hidden bonuses are not exposed, and non-owned visible
  hero/object records use public fields only.
- Done: additional player-visible lifecycle/window callbacks are mirrored into the script update journal:
  hero-visit start/end, center-view hints on visible tiles, bulk artifact movement start, puzzle/world/quest-log
  windows, View Air/View Earth world-view object overlays, player block/start/end turn events, battle start/end
  boundaries, battle result summaries, battle-finished notifications, game-over messages, object-removal
  completion, and color-scheme changes. Puzzle-map updates do not expose the Grail location; world-view overlays
  expose only the object positions/types supplied to the player by the spell effect.
- Done: Lua can invoke Nullkiller's local object-interaction helper for one owned hero at one visible current
  object, then regain control. This exposes native post-visit handling for towns and hill forts as a bounded
  subroutine instead of requiring full-day delegation.
- Done: Lua has named task, step, and pass wrappers for every bounded Nullkiller task mode, using stable numeric
  mode ids while keeping scripts readable.
- Done: Lua can request Nullkiller startup tasks as a bounded task mode. This exposes the native startup helper
  for early tavern/build/recruit/hero-swap decisions without delegating the rest of the day. The compiled but
  disabled `StayAtTownBehavior` remains unexposed because native `makeTurn` does not currently use it.
- Done: spell research is exposed as checked `ai:spellResearch` plus read-side `spellResearchOptions`. Mage-guild
  snapshots now hide deeper research queues and expose only currently visible spells plus the next research-dialog
  candidate.
- Done: built town structures with immediate/manual visit effects are exposed through checked
  `ai:visitTownBuilding` and read-side `visitTownBuildingOptions`.
- Done: Castle Gate teleport is exposed through checked `ai:castleTeleport` and read-side
  `castleTeleportOptions`, matching normal player restrictions for source/destination towns and visiting heroes.
- Done: the normal player statistics request is exposed through checked `ai:requestStatistic`; responses are
  mirrored into the visible script update journal as `statistics_response` using the existing statistics JSON
  serializer.
- Done: market operations are exposed through a coarse `nullkillerTrade` helper, an exact resource-resource
  helper, and a generic `marketTrade` action with wrappers for resource transfer, creature/resource sale,
  artifact purchase/sale/sacrifice, bulk artifact sacrifice, creature sacrifice, bulk creature-stack sacrifice,
  undead transformation, and university skill purchase.
- Done: sacrifice-altar artifact staging is exposed as checked artifact movement. Lua can move one artifact or
  bulk equipped/backpack artifacts to and from the market altar through normal server callbacks, and market-dialog
  `altarOptions` now provide sacrifice candidates plus explicit staging-and-sacrifice action sequences. This
  keeps scripts aligned with the native UI instead of treating hero inventory artifact ids as directly
  sacrificable.
- Done: visible owned/neutral market objects expose read-side mode details, available items, available unit
  counts, efficiency, and resource-resource exchange rates. Enemy market details remain hidden beyond public
  visible-object mode metadata.
- Done: primitive adventure spells, digging, boat building, Castle Gate teleport, hero dismissal, and
  spellbook/war-machine purchases are exposed. Owned hero records now include spellbook ids, and action space
  includes read-side `digOptions`, `shipyardOptions`, `castleTeleportOptions`, `adventureSpellOptions`, and
  `buyArtifactOptions` with checked `planAction` payloads. Adventure spell options now carry typed spell-kind,
  remaining-cast, range, movement-cost, dialog, summon-boat, and visible-target metadata. Complex Dimension Door,
  Town Portal, boat, water-walk, fly, and unlock-chain planning remains intentionally delegated to bounded
  Nullkiller adventure subroutines through the option's `nativePlanner` actions, because the pathfinder owns
  those routing decisions.
- Partial: full danger-map estimates are not exposed intentionally; Lua gets capped visible-only enemy threat
  tiles and visible blocker clusters, while hidden enemy reach remains private to avoid cheating.
- Partial: MCP and scripted AI still duplicate some JSON assembly code; extraction can happen once the surface
  stabilizes.

### Milestone 7: Default Script

- Done: `scripts/ai/defaultAdventure.lua` is a readable Lua policy with explicit scoring functions.
- Done: it scores allowed builds, recruitment, and reachable object pickups.
- Done: its active `runDay(ai, input)` path is imperative: Lua owns the day loop, executes checked `ai:*`
  calls directly, refreshes visible state after side effects, answers pending queries, and uses capped bounded
  Nullkiller turn slices instead of delegating the remaining turn. The older `planDay(input)` path remains only as a
  compatibility shim for fixtures, tests, and the script's internal scoring reuse.
- Done: the default policy no longer calls full-day `ai:nullkiller()` during normal `runDay` control flow. Idle
  bounded slices end the turn, productive trade-only slices refresh and continue, and repeated checked-action or
  bounded-helper failures surface as script errors so the host safety fallback can take over.
- Done: opt-in personality profiles `aggressiveAdventure.lua`, `economyAdventure.lua`, and `explorerAdventure.lua`
  now expose imperative `runDay(ai, input)` wrappers. Their legacy `planDay(input)` scorers remain as readable
  policy cores, but active execution goes through checked host calls with refresh/query yield points.
- Done: it assigns a main hero, tracks consumed opponent-update revisions, prioritizes recruitment under strong
  defense pressure, and penalizes scout targets near visible enemy heroes.
- Done: it consumes candidate risk/value fields, avoids unsafe object targets more aggressively, and can move a
  threatened hero away from a visible stronger enemy.
- Partial: deeper defense policy remains Lua policy/analyzer work. Bounded defense and gather-army subroutines are
  available; richer host-side fields should be added only when concrete script ranking logic needs them.

### Milestone 8: Save/Load and Development Reload

- Done: script memory is versioned at the Lua policy level and bounded by `maxMemoryBytes`.
- Done: script memory is persisted through save/load in `PlayerState::playerLocalSettings` under the
  `scriptedAdventureAI` key, with script-path and storage-version checks.
- Done: `config/ai/scriptedAdventure.json` controls script path, reload behavior, action/memory/call limits,
  tracing, and repeated-failure throttling.
- Done: when development reload is disabled, the host keeps a cached Lua runner for the game so the script has a
  real per-game instance. Save/load durability still goes through explicit JSON memory, not arbitrary Lua VM
  internals.
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
- Done: `scripts/ai/candidates/boundedNullkillerControl.lua` is an API parity probe that drives Nullkiller's
  priority/adventure/trade/artifact phases through bounded Lua calls, answers dialogs through the bounded
  `ai:nullkillerAnswerQuery` helper, refreshes after side effects, and ends the turn when native slices report no
  remaining work. It intentionally avoids normal `ai:nullkiller()` full-day delegation; full fallback is reserved
  for actual script or host failures. Its loop budgets against the imperative `maxActions` limit and reads the
  exposed native `analysis.nullkiller.settings.maxPass`, so legacy per-turn script-call limits do not truncate
  bounded native day control.
- Done: bounded control now treats native candidate exhaustion as an end-turn signal only when the same slice did no
  useful priority, adventure, replan, trade, or pause work. If a bounded helper exhausts one generated candidate set
  after doing useful work, Lua refreshes and asks for another bounded slice, matching native Nullkiller's day-loop
  semantics more closely than immediate full-day fallback or immediate end-turn.
- Done: the Lua facade now includes `ai:nullkillerBoundedDay`, a reusable API-parity helper that runs Nullkiller's
  native day loop as one-pass checked `nullkiller_turn_slice` calls, answers pending queries between passes, refreshes
  after side effects, and returns a structured summary to Lua. This gives scripts a common bounded replacement for
  ordinary full-day `ai:nullkiller()` delegation.
- Done: `scripts/ai/candidates/boundedNullkillerControl.lua` now uses `ai:nullkillerBoundedDay`, so the parity probe
  also exercises the shared one-native-pass-at-a-time API instead of hiding multiple native passes inside one host
  command.
- Done: a traced 3-day small underground/no-water smoke of the bounded-day parity probe reached the day limit
  cleanly. The trace had 23 `nullkiller_turn_slice` commands, all with `max_passes = 1`, one bounded query answer,
  three script-requested end-turn outputs, zero failed checked actions, and zero fallback outputs.
- Done: `scripts/ai/candidates/boundedNullkillerAdventure.lua` now stays on the bounded-helper contract too. Native
  no-task and stop-turn signals become Lua-controlled end-turn outputs, while unexpected helper failures or exhausted
  bounded budgets raise script errors for the host safety path instead of explicitly delegating the rest of the day to
  Nullkiller.
- Done: `scripts/ai/candidates/statisticsProbeAdventure.lua` now exercises `ai:requestStatistic` plus one bounded
  native turn slice without calling full-day fallback in active `runDay`. The only bundled script that intentionally
  delegates the remaining day is the explicit fallback control.
- Done: a traced 16-map, 14-day parity run of `boundedNullkillerControl.lua` against `Nullkiller2` reached the day
  limit in all scenarios with zero fallback outputs and zero failed checked actions. The trace set contained 225
  bounded `nullkiller_turn_slice` calls, 69 bounded query answers, and 224 script-requested end turns. A no-trace
  full-outcome probe was stopped after 11/16 completed because the long tail continued through blue-only late
  turns; the completed subset was 4 red wins and 7 red losses, so outcome tuning still needs separate evaluation.
- Done: a 16-map, 3-day traced smoke after the bounded-control exhaustion fix completed all scenarios at the day
  limit with 48 `end_turn` outputs, 60 bounded `nullkiller_turn_slice` calls, 18 bounded query answers, zero failed
  checked actions, and zero fallback outputs.
- Done: after converting `defaultAdventure.lua` away from normal full-day fallback, an explicit 16-map, 3-day traced
  smoke completed all scenarios at the day limit with 48 `end_turn` outputs, 391 checked `visit_object` actions,
  74 bounded query answers, 58 bounded `nullkiller_turn_slice` calls, zero failed checked actions, and zero fallback
  outputs.
- Done: the active `defaultAdventure.lua` `runDay(ai, input)` path no longer calls its legacy `Script.planDay`
  compatibility entry point. It now calls a neutral `chooseDayActions` helper and executes the returned checked
  calls imperatively through the Lua facade. `Script.planDay` remains only as a wrapper for old tests/tooling. A
  3-day traced smoke on the fixed small random-map scenario reached the day limit with `end_turn` outputs only, no
  failed checked actions, and no fallback outputs.
- Done: Lua can now run one script-selected Nullkiller candidate through `ai:runBestNullkillerTask`. The helper
  inspects native candidates without side effects, applies an optional Lua predicate, executes only the chosen
  opaque handle, and reports `executed = false` when no candidate matches instead of falling back to a full native
  day.
- Done: the bundled aggressive, economy, and explorer profile scripts now follow the same active-entrypoint shape
  as the default script: `runDay(ai, input)` calls a local policy scorer directly, while `Script.planDay` remains a
  compatibility wrapper for tests and older hosts.
- Done: the bundled aggressive, economy, and explorer profiles no longer use normal full-day `ai:nullkiller()`
  delegation when their simple scorer has no confident action or reaches its command budget. They now use bounded
  `ai:nullkillerTurnSlice` calls and regain Lua control just like the default/parity scripts.
- Done: Lua can inspect player-specific puzzle-map progress through `state.grail` and `ai:getGrail()`. This exposes
  the revealed ratio but gates the exact grail tile until the puzzle is fully revealed, avoiding accidental hidden
  map leakage from the client-side `getGrailPos` callback.
- Done: `state.calendar` now exposes the normal player-visible calendar breakdown in addition to the legacy
  absolute day, so scripts can reason about weeks/months without recomputing settings-dependent calendar math.
- Done: Lua input now includes `state.players` with public player status/relation metadata for every valid player.
  The binding uses numeric ids for script decisions, keeps labels as trace context, gates detailed player-state
  fields through the normal callback access rules, and deliberately excludes enemy economy, army, hero-count, and
  town-count data.
- Done: `state.map` now includes compact explored-area and visible-control summaries derived from the existing
  visible tile/object snapshot. Scripts can reason over exploration progress, terrain composition, and visible
  control balance without rescanning raw tile/object arrays or looking at hidden map state.
- Done: trace summaries and script evaluations now report longitudinal `mapProgress` metrics from the first and
  final visible map snapshots per run/player series. The evaluator folds the map-progress score into traced
  iteration scoring and comparison deltas.
- Done: bounded `nullkiller_priority_pass` results now include `lastTask` as stable task JSON, so Lua policies and
  trace mining can inspect native build/recruit/hire choices instead of relying on a display-only task string.
- Done: after adding explicit `nullkiller_reset`, a 16-map, 1-day traced integration smoke completed all scenarios
  at the day limit with 16 `end_turn` outputs, 20 bounded `nullkiller_turn_slice` calls, 145 checked `visit_object`
  actions, 18 bounded query answers, zero failed checked actions, and zero fallback outputs.
- Done: API parity is now the explicit gate before Lua policy optimization. A partial full-outcome bounded-helper
  probe was stopped after 7/10 completed scenarios because it was already below target and the remaining games had
  moved into long late-month play; the result is treated as inconclusive smoke data, not a policy benchmark.
- Done: bounded scripted Nullkiller query handling now mirrors native Nullkiller's hero-exchange army/artifact
  transfer direction. This keeps `ai:nullkillerAnswerQuery` aligned with `AIGateway::heroExchangeStarted` instead
  of letting bounded control rearrange heroes differently after meetings.
- Done: `actionSpace.acceptedActions` is generated from the stable C++ script-action id registry. New checked
  actions now become advertised through the same authoritative table that validates `type_id`, reducing string/id
  drift across C++, Lua, traces, and tests.
- Done: the imperative Lua facade refreshes its `type` to `type_id` mapping from host-provided
  `actionSpace.acceptedActions`, including after `ai:refresh()`. The hardcoded Lua ids remain a compatibility
  fallback, but normal play now uses the host-advertised integer action contract.
- Done: the script update journal mirrors visible/owned `beforeObjectPropertyChanged` and
  `objectPropertyChanged` callbacks as `object_property_will_change` and `object_property_changed`. Each update
  carries stable object/property/identifier ids, trace labels, and a gated object snapshot only when the object is
  visible or owned by the script player. This closes a read-side gap for map-control and ownership-memory policies.
- Done: trace mistake mining now treats final `imperative-output` progress as part of the day-level script decision.
  Bounded `nullkiller_turn_slice` work can satisfy defense and hero-threat responses through native task
  `affectedObjectIds`, and normal imperative stop signals are no longer reported as stopped action batches. This
  keeps API-parity probes from looking idle when Lua deliberately called a bounded native subroutine and regained
  control before ending the turn.
- Done: bounded native task execution can now request all generated Nullkiller candidates without delegating the
  rest of the day. For `nullkiller_step`, `nullkiller_pass`, and `nullkiller_turn_slice`, `max_candidates = 0` and
  `max_attempts = 0` mean "use every generated native candidate/attempt"; positive values remain explicit bounds.
  The host still caps serialized candidate details by default during execution so traces do not become enormous,
  while `candidate_details_limit = 0` can be used by inspection-heavy scripts that deliberately want every returned
  candidate serialized. `boundedNullkillerControl.lua` uses the all-candidates mode as the current API-parity probe.
- Done: a patched-client 16-map, 3-day traced smoke of `boundedNullkillerControl.lua` completed all scenarios at the
  day limit with 611 parsed trace events, 438 all-candidate `nullkiller_turn_slice` commands, 29 bounded query
  answers, zero failed checked actions, zero parse errors, and zero mined mistakes. Raw command traces confirmed all
  turn slices used `max_candidates = 0` and `max_attempts = 0`; host native result traces reported
  `candidateLimit = 0` with serialized details capped at 64.
- Done: a patched-client 16-map no-trace full-outcome probe of `boundedNullkillerControl.lua` produced valid compact
  result JSON for every scenario without corrupt artifacts. Red/ScriptedAdventureAI had 6 terminal wins and 5
  terminal losses against blue/Nullkiller2. The remaining 5 games were manually terminated after about 24 minutes
  because stdout had not advanced for more than 20 minutes, all threads were sleeping in futex/epoll waits, and the
  last logs were around battle creation; the runner recorded these as `nonzero_exit` rather than strategic wins or
  losses. The stalled seeds were `02`, `04`, `08`, `10`, and `12`.
- Done: the batch/evaluation runner now has an opt-in `--idle-timeout` watchdog, also available per scenario as
  `idleTimeout` / `idle_timeout`. It terminates runs whose `stdout.log` stops growing for the configured duration,
  records `idleTimedOut = true`, classifies the outcome as `idle_timeout`, and reports `idleTimeouts` in aggregate
  summaries and promotion gates. Each `run.json` also records `stdoutSummary`: cleaned tail lines, stdout byte count,
  time since last output, and a coarse `tailSignature` such as `battle_ai_creation` or
  `battle_ai_creation_invalid_stack`; compact batch results include that signature. This makes battle/client stalls
  machine-readable instead of requiring manual SIGTERM or waiting for the broad per-run timeout.
- Done: rerunning the five previously stalled seeds (`02`, `04`, `08`, `10`, `12`) with a 90-second idle timeout
  completed all five without an idle timeout. The rerun outcomes were two red losses (`02`, `04`) and three red wins
  (`08`, `10`, `12`), finishing in roughly 43-107 seconds. Treat the earlier battle-creation stalls as
  nondeterministic/timing-sensitive until a future diagnostic batch captures a fresh `stdoutTailSignature`.
- Done: a fresh 16-game no-trace batch with 16 parallel jobs and a 120-second idle timeout produced 6 red wins, 7
  red losses, and 3 idle timeouts. The timeout signatures were `battle_ai_creation` for seed `05` and
  `battle_ai_creation_invalid_stack` for seeds `08` and `10`; seed `08` also logged a stale `QueryReply` attempt
  after the host reported no pending queries. Bounded Nullkiller query answering now treats `allow_expired` plus
  zero pending AI-status queries as a successful no-op and clears the stale script query cache entry instead of
  sending a server reply. A focused seed `08` rerun after this guard no longer emitted stale-query server errors,
  but still idle-timed-out at a pure `battle_ai_creation` signature.
- Done: a one-off seed `08` proc-stack capture at the battle idle showed the client process sleeping rather than
  spending CPU in Lua policy: main/network/worker threads were in futex waits, the server thread was in epoll wait,
  and the stdout tail ended after repeated `Creating battle AI BattleAI` lines. Until a userspace stack tool is
  available, treat this as a battle/client infrastructure stall that the evaluation ladder should classify via
  `idle_timeout`, not as a scripted adventure strategy loss.
- Remaining: decide which generated-map seeds graduate into the stable training/held-out corpus.

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

- Treat Lua API parity as the gate for script optimization. A script that cannot express the same meaningful
  choices as Nullkiller should use bounded Nullkiller subroutines and checked facades first; tuning before this
  point mostly optimizes around missing host capabilities.
- Do not optimize the default/champion policy against Nullkiller until the normal script path no longer needs
  full-day `ai:nullkiller()` for any ordinary player-visible action, dialog, or Nullkiller subroutine. Missing
  parity should produce a checked Lua facade, read-only visible-state field, or bounded native helper that returns
  control to Lua.
- Use `boundedNullkillerControl.lua` as the first parity regression script. If it needs ordinary full-day fallback
  to finish a day, fix the missing bounded helper, query answer, or observable state before tuning higher-level Lua
  strategy.
- Keep adventure-spell optimization on the hybrid path: scripts should rank when to use native adventure
  spell-routing subroutines, then inspect returned task/path `specialAction` records, instead of attempting to
  recreate Nullkiller's pathfinder in Lua.
- Keep auditing Lua API parity before tuning scripts: if a concrete player-visible action or native subroutine
  still requires full-day `ai:nullkiller()` delegation, treat that as an API gap and add a checked facade or
  bounded helper first. Full-day fallback should remain a safety/conservatism path, not the normal way to access
  Nullkiller competence.
- Expose any remaining Nullkiller analyzer data as read-only candidate fields only when a concrete script policy
  needs it; do not rebuild those analyses in Lua.
- Expand the default Lua policy to rank and compose bounded Nullkiller candidates only after the API can express
  the same meaningful choices Nullkiller can make.
- Run fixed-map `--testdays N` batches comparing default, aggressive, economy, explorer, Nullkiller, and older
  script versions, then feed trace deltas and mined JSON fixtures back into the Lua policy.
