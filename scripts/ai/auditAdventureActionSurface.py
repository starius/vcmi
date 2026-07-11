#!/usr/bin/env python3
"""Audit ScriptedAdventureAI Lua actions against player server requests."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


PACK_RE = re.compile(r"struct\s+DLL_LINKAGE\s+(\w+)\s*:\s*public\s+CPackForServer")
ACTION_RE = re.compile(r"\{\s*\d+\s*,\s*\"([^\"]+)\"\s*\}")
ACTION_ID_RE = re.compile(r"\{\s*(\d+)\s*,\s*\"([^\"]+)\"\s*\}")
ACTION_SPACE_VECTOR_RE = re.compile(r"actionSpace\s*\[\s*\"([^\"]+)\"\s*\]\.Vector\s*\(")
LUA_ACTION_ID_RE = re.compile(r"^\s*([A-Za-z]\w*)\s*=\s*(\d+)\s*,?\s*$")
LUA_ACTION_NAME_RE = re.compile(r"^\s*([A-Za-z]\w*)\s*=\s*ai\.actionTypeIds\.([A-Za-z]\w*)\s*,?\s*$")
LUA_ACTION_LITERAL_RE = re.compile(r"\btype\s*=\s*\"([a-z_]+)\"")

PACK_HEADERS = (
    "lib/networkPacks/PacksForServer.h",
    "lib/networkPacks/SaveLocalState.h",
)

# Each listed packet must have at least one stable Lua action registered. Some
# packets are represented by a semantic helper instead of a one-to-one wrapper.
PACK_ACTION_COVERAGE: dict[str, tuple[str, ...]] = {
    "EndTurn": ("end_turn",),
    "DismissHero": ("dismiss_hero", "nullkiller_dismiss_weak_hero"),
    "MoveHero": ("move_hero", "visit_object"),
    "CastleTeleportHero": ("castle_teleport",),
    "ArrangeStacks": ("swap_creatures", "merge_stacks", "merge_or_swap_stacks", "split_stack"),
    "BulkMoveArmy": ("transfer_army", "bulk_move_army", "pick_best_creatures", "prepare_hero"),
    "BulkSplitStack": ("bulk_split_stack",),
    "BulkMergeStacks": ("bulk_merge_stacks",),
    "BulkSplitAndRebalanceStack": ("bulk_split_rebalance_stack",),
    "DisbandCreature": ("dismiss_creature",),
    "BuildStructure": ("build", "nullkiller_build_army"),
    "VisitTownBuilding": ("visit_town_building",),
    "SpellResearch": ("spell_research",),
    "RecruitCreatures": ("recruit", "nullkiller_recruit_creatures"),
    "UpgradeCreature": ("upgrade_creature", "nullkiller_upgrade_army"),
    "GarrisonHeroSwap": ("swap_garrison_hero",),
    "ExchangeArtifacts": ("swap_artifacts",),
    "BulkExchangeArtifacts": ("bulk_move_artifacts",),
    "ManageBackpackArtifacts": ("sort_backpack_artifacts", "scroll_backpack_artifacts"),
    "ManageEquippedArtifacts": ("manage_hero_costume",),
    "AssembleArtifacts": ("assemble_artifacts",),
    "EraseArtifactByClient": ("erase_transition_artifact",),
    "BuyArtifact": ("buy_artifact",),
    "TradeOnMarketplace": ("trade_resources", "market_trade", "nullkiller_trade"),
    "SetFormation": ("set_formation",),
    "SetTactics": ("set_tactics",),
    "SetTownName": ("set_town_name",),
    "HireHero": ("hire_hero",),
    "BuildBoat": ("build_boat",),
    "QueryReply": ("answer_query", "cancel_query", "nullkiller_answer_query"),
    "DigWithHero": ("dig",),
    "CastAdvSpell": ("cast_spell",),
    "RequestStatistic": ("request_statistic",),
}

INTENTIONAL_EXCLUSIONS: dict[str, str] = {
    "GamePause": "session/UI control, not adventure AI strategy",
    "SaveGame": "session persistence, not adventure AI strategy",
    "PlayerMessage": "chat/text commands are intentionally not exposed",
    "AdvInterfaceReady": "client handshake, not a player strategy action",
    "SaveLocalState": "script memory replaces player-local UI state",
    "MakeAction": "battle action packet; adventure AI delegates battle control to BattleAI except retreat/surrender callback",
}

# Direct, player-visible actions whose legality depends on current visible game
# state should have typed action-space option lists. Raw Lua wrappers remain
# available for scripts that already know exact ids, but these fields let scripts
# discover normal UI-equivalent choices without probing hidden state.
DISCOVERABLE_ACTION_OPTIONS: dict[str, str] = {
    "build": "buildOptions",
    "recruit": "recruitOptions",
    "hire_hero": "hireHeroOptions",
    "dismiss_hero": "dismissHeroOptions",
    "transfer_army": "armyTransferOptions",
    "swap_creatures": "stackManagementOptions",
    "merge_stacks": "stackManagementOptions",
    "merge_or_swap_stacks": "stackManagementOptions",
    "split_stack": "stackManagementOptions",
    "bulk_move_army": "stackManagementOptions",
    "bulk_split_stack": "stackManagementOptions",
    "bulk_merge_stacks": "stackManagementOptions",
    "bulk_split_rebalance_stack": "stackManagementOptions",
    "dismiss_creature": "dismissCreatureOptions",
    "upgrade_creature": "upgradeCreatureOptions",
    "set_formation": "formationOptions",
    "set_tactics": "tacticsOptions",
    "swap_garrison_hero": "garrisonSwapOptions",
    "move_hero": "movementOptions",
    "visit_object": "reachableObjects",
    "build_boat": "shipyardOptions",
    "castle_teleport": "castleTeleportOptions",
    "dig": "digOptions",
    "cast_spell": "adventureSpellOptions",
    "buy_artifact": "buyArtifactOptions",
    "swap_artifacts": "artifactManagementOptions",
    "bulk_move_artifacts": "artifactManagementOptions",
    "sort_backpack_artifacts": "artifactManagementOptions",
    "scroll_backpack_artifacts": "artifactManagementOptions",
    "manage_hero_costume": "artifactManagementOptions",
    "assemble_artifacts": "artifactManagementOptions",
    "erase_transition_artifact": "artifactManagementOptions",
    "trade_resources": "marketTradeOptions",
    "market_trade": "marketTradeOptions",
    "spell_research": "spellResearchOptions",
    "visit_town_building": "visitTownBuildingOptions",
    "nullkiller_object_interaction": "objectInteractionOptions",
    "nullkiller_defend_town": "defenseResponseOptions",
}


def pack_for_server_types(repo_root: Path) -> set[str]:
    packs: set[str] = set()
    for relative in PACK_HEADERS:
        text = (repo_root / relative).read_text(encoding="utf-8")
        packs.update(PACK_RE.findall(text))
    return packs


def registered_action_types(repo_root: Path) -> set[str]:
    text = (repo_root / "AI/ScriptedAdventure/CScriptedAdventureAI.cpp").read_text(encoding="utf-8")
    return set(ACTION_RE.findall(text))


def registered_action_ids(repo_root: Path) -> dict[str, int]:
    text = (repo_root / "AI/ScriptedAdventure/CScriptedAdventureAI.cpp").read_text(encoding="utf-8")
    return {name: int(action_id) for action_id, name in ACTION_ID_RE.findall(text)}


def table_body_after(text: str, marker: str) -> str:
    marker_index = text.index(marker)
    open_index = text.index("{", marker_index)
    depth = 0
    for index in range(open_index, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[open_index + 1:index]
    raise ValueError(f"Lua table is not closed after marker: {marker}")


def lua_action_ids(repo_root: Path) -> dict[str, int]:
    text = (repo_root / "luascript/LuaAdventureScriptRunner.cpp").read_text(encoding="utf-8")

    id_body = table_body_after(text, "ai.actionTypeIds =")
    ids_by_lua_name: dict[str, int] = {}
    for line in id_body.splitlines():
        match = LUA_ACTION_ID_RE.match(line)
        if match:
            ids_by_lua_name[match.group(1)] = int(match.group(2))

    name_body = table_body_after(text, "ai.actionTypeIdsByName =")
    result: dict[str, int] = {}
    unresolved: dict[str, str] = {}
    for line in name_body.splitlines():
        match = LUA_ACTION_NAME_RE.match(line)
        if not match:
            continue
        action_name, lua_id_name = match.groups()
        if lua_id_name in ids_by_lua_name:
            result[action_name] = ids_by_lua_name[lua_id_name]
        else:
            unresolved[action_name] = lua_id_name

    if unresolved:
        missing = ", ".join(f"{name}->{lua_id_name}" for name, lua_id_name in sorted(unresolved.items()))
        raise ValueError(f"Lua actionTypeIdsByName references unknown actionTypeIds entries: {missing}")

    return result


def lua_facade_action_types(repo_root: Path) -> set[str]:
    text = (repo_root / "luascript/LuaAdventureScriptRunner.cpp").read_text(encoding="utf-8")
    return set(LUA_ACTION_LITERAL_RE.findall(text))


def action_space_vector_fields(repo_root: Path) -> set[str]:
    text = (repo_root / "AI/ScriptedAdventure/CScriptedAdventureAI.cpp").read_text(encoding="utf-8")
    return set(ACTION_SPACE_VECTOR_RE.findall(text))


def audit(repo_root: Path) -> dict[str, Any]:
    packs = pack_for_server_types(repo_root)
    actions = registered_action_types(repo_root)
    cpp_action_ids = registered_action_ids(repo_root)
    lua_ids = lua_action_ids(repo_root)
    lua_facades = lua_facade_action_types(repo_root)
    action_space_fields = action_space_vector_fields(repo_root)

    missing_classifications = sorted(
        pack
        for pack in packs
        if pack not in PACK_ACTION_COVERAGE and pack not in INTENTIONAL_EXCLUSIONS
    )
    stale_classifications = sorted(
        pack
        for pack in set(PACK_ACTION_COVERAGE) | set(INTENTIONAL_EXCLUSIONS)
        if pack not in packs
    )
    missing_registered_actions = {
        pack: [action for action in action_types if action not in actions]
        for pack, action_types in sorted(PACK_ACTION_COVERAGE.items())
        if any(action not in actions for action in action_types)
    }
    missing_lua_action_ids = {
        action: cpp_action_ids[action]
        for action in sorted(cpp_action_ids)
        if action not in lua_ids
    }
    extra_lua_action_ids = {
        action: lua_ids[action]
        for action in sorted(lua_ids)
        if action not in cpp_action_ids
    }
    mismatched_lua_action_ids = {
        action: {"cpp": cpp_action_ids[action], "lua": lua_ids[action]}
        for action in sorted(cpp_action_ids.keys() & lua_ids.keys())
        if cpp_action_ids[action] != lua_ids[action]
    }
    missing_lua_facade_actions = sorted(actions - lua_facades)
    extra_lua_facade_actions = sorted(lua_facades - actions)
    missing_discoverable_option_fields = {
        action: field
        for action, field in sorted(DISCOVERABLE_ACTION_OPTIONS.items())
        if action in actions and field not in action_space_fields
    }

    return {
        "ok": (
            not missing_classifications
            and not stale_classifications
            and not missing_registered_actions
            and not missing_lua_action_ids
            and not extra_lua_action_ids
            and not mismatched_lua_action_ids
            and not missing_lua_facade_actions
            and not extra_lua_facade_actions
            and not missing_discoverable_option_fields
        ),
        "packCount": len(packs),
        "coveredPackCount": len(PACK_ACTION_COVERAGE),
        "excludedPackCount": len(INTENTIONAL_EXCLUSIONS),
        "registeredActionCount": len(actions),
        "luaActionIdCount": len(lua_ids),
        "luaFacadeActionCount": len(lua_facades),
        "actionSpaceOptionFieldCount": len(action_space_fields),
        "missingPackClassifications": missing_classifications,
        "stalePackClassifications": stale_classifications,
        "missingRegisteredActions": missing_registered_actions,
        "missingLuaActionIds": missing_lua_action_ids,
        "extraLuaActionIds": extra_lua_action_ids,
        "mismatchedLuaActionIds": mismatched_lua_action_ids,
        "missingLuaFacadeActions": missing_lua_facade_actions,
        "extraLuaFacadeActions": extra_lua_facade_actions,
        "missingDiscoverableOptionFields": missing_discoverable_option_fields,
        "intentionalExclusions": INTENTIONAL_EXCLUSIONS,
    }


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=default_repo_root())
    args = parser.parse_args()

    result = audit(args.repo_root.resolve())
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
