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


def pack_for_server_types(repo_root: Path) -> set[str]:
    packs: set[str] = set()
    for relative in PACK_HEADERS:
        text = (repo_root / relative).read_text(encoding="utf-8")
        packs.update(PACK_RE.findall(text))
    return packs


def registered_action_types(repo_root: Path) -> set[str]:
    text = (repo_root / "AI/ScriptedAdventure/CScriptedAdventureAI.cpp").read_text(encoding="utf-8")
    return set(ACTION_RE.findall(text))


def audit(repo_root: Path) -> dict[str, Any]:
    packs = pack_for_server_types(repo_root)
    actions = registered_action_types(repo_root)

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

    return {
        "ok": not missing_classifications and not stale_classifications and not missing_registered_actions,
        "packCount": len(packs),
        "coveredPackCount": len(PACK_ACTION_COVERAGE),
        "excludedPackCount": len(INTENTIONAL_EXCLUSIONS),
        "registeredActionCount": len(actions),
        "missingPackClassifications": missing_classifications,
        "stalePackClassifications": stale_classifications,
        "missingRegisteredActions": missing_registered_actions,
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
