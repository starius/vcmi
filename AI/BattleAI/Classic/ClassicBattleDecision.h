/*
 * ClassicBattleDecision.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../../lib/battle/AutocombatPreferences.h"
#include "../../../lib/battle/BattleAction.h"
#include "ClassicAITargetRecord.h"

class CBattleInfoCallback;
class CStack;
class ClassicDecisionTrace;
class Environment;
class IClassicBattleAIRng;

struct ClassicDecisionContext
{
	std::map<uint32_t, SpellID> selectedCreatureSpells;
	ClassicAITargetRecords targetRecords;
	bool secondPhase = false;
};

enum class ClassicDecisionEntryPoint
{
	FULL_PIPELINE,
	DO_SPELL_AI,
	CHECK_RETREAT,
	DO_COMP_AI
};

/// Pure classic decision seam shared by the runtime controller and differential probe.
class ClassicBattleDecision
{
public:
	static BattleAction decide(
		const Environment * env,
		std::shared_ptr<CBattleInfoCallback> battle,
		BattleSide side,
		const CStack * stack,
		int32_t difficulty,
		const AutocombatPreferences & preferences,
		std::shared_ptr<IClassicBattleAIRng> randomGenerator,
		std::shared_ptr<ClassicDecisionTrace> trace,
		ClassicDecisionEntryPoint entryPoint = ClassicDecisionEntryPoint::FULL_PIPELINE,
		ClassicDecisionContext * context = nullptr
	);
};
