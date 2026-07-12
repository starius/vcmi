/*
 * CLuaNullkiller2AI.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "CLuaNullkiller2AI.h"

#include "LuaNullkiller2Runner.h"

#include "../../lib/battle/BattleAction.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/CCreatureHandler.h"
#include "../../lib/GameConstants.h"
#include "../../lib/StartInfo.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CStackInstance.h"
#include "../../lib/mapObjects/IObjectInterface.h"

namespace LuaNullkiller2AI
{
namespace
{

std::optional<int> commandInteger(const LuaCommand & command, const std::string & key)
{
	const auto found = command.integers.find(key);
	if(found == command.integers.end())
		return std::nullopt;

	return found->second;
}

JsonNode tileSnapshot(const int3 & tile)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["x"].Integer() = tile.x;
	result["y"].Integer() = tile.y;
	result["z"].Integer() = tile.z;
	return result;
}

void addArmySnapshotFields(JsonNode & result, const CArmedInstance * army);
JsonNode armySnapshot(const CArmedInstance * army);

JsonNode heroSnapshot(const CGHeroInstance * hero)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	if(!hero)
		return result;

	result["id"].Integer() = hero->id.getNum();
	result["name"].String() = hero->getNameTranslated();
	result["owner"].Integer() = hero->tempOwner.getNum();
	result["factionID"].Integer() = hero->getFactionID().getNum();
	result["totalStrength"].Integer() = static_cast<int64_t>(hero->getTotalStrength());
	result["armyStrength"].Integer() = static_cast<int64_t>(hero->getArmyStrength());
	result["armyCost"].Integer() = static_cast<int64_t>(hero->getArmyCost());
	addArmySnapshotFields(result, hero);
	result["movementPointsRemaining"].Integer() = hero->movementPointsRemaining();
	result["garrisoned"].Bool() = hero->isGarrisoned();
	result["visitablePos"] = tileSnapshot(hero->visitablePos());
	return result;
}

JsonNode resourcesSnapshot(const TResources & resources)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);
	for(int resourceID = 0; resourceID < GameConstants::RESOURCE_QUANTITY; ++resourceID)
		result.Vector().push_back(JsonNode(static_cast<int64_t>(resources[GameResID(resourceID)])));
	return result;
}

JsonNode creatureSnapshot(CreatureID creatureID)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	const auto * creature = creatureID.toCreature();

	result["id"].Integer() = creatureID.getNum();
	result["aiValue"].Integer() = creature ? creature->getAIValue() : 0;
	result["factionID"].Integer() = creature ? creature->getFactionID().getNum() : -1;
	if(creature)
		result["fullRecruitCost"] = resourcesSnapshot(creature->getFullRecruitCost());

	return result;
}

JsonNode stackSnapshot(const SlotID & slotID, const CStackInstance * stack)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	if(!stack)
		return result;

	result["slot"].Integer() = slotID.getNum();
	result["count"].Integer() = static_cast<int64_t>(stack->getCount());
	result["power"].Integer() = static_cast<int64_t>(stack->getPower());
	result["marketValue"].Integer() = static_cast<int64_t>(stack->getMarketValue());
	result["creatureID"].Integer() = stack->getCreatureID().getNum();
	result["creature"] = creatureSnapshot(stack->getCreatureID());
	return result;
}

void addArmySnapshotFields(JsonNode & result, const CArmedInstance * army)
{
	if(!army)
		return;

	result["armySize"].Integer() = GameConstants::ARMY_SIZE;
	result["stacksCount"].Integer() = army->stacksCount();
	result["armyStrength"].Integer() = static_cast<int64_t>(army->getArmyStrength());
	result["armyCost"].Integer() = static_cast<int64_t>(army->getArmyCost());
	result["slots"].setType(JsonNode::JsonType::DATA_VECTOR);
	result["slotsByCreature"].setType(JsonNode::JsonType::DATA_STRUCT);

	for(const auto & slot : army->Slots())
	{
		const auto * stack = slot.second.get();
		result["slots"].Vector().push_back(stackSnapshot(slot.first, stack));
		if(stack)
			result["slotsByCreature"][std::to_string(stack->getCreatureID().getNum())].Integer() = slot.first.getNum();
	}
}

JsonNode armySnapshot(const CArmedInstance * army)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	if(!army)
		return result;

	result["id"].Integer() = army->id.getNum();
	result["owner"].Integer() = army->tempOwner.getNum();
	addArmySnapshotFields(result, army);
	return result;
}

JsonNode purchasableCreatureSnapshot(CreatureID creatureID, int count, int level)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["count"].Integer() = count;
	result["level"].Integer() = level;
	result["creature"] = creatureSnapshot(creatureID);
	return result;
}

JsonNode townSnapshot(const CGTownInstance * town, const std::shared_ptr<CCallback> & callback)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	if(!town)
		return result;

	result["id"].Integer() = town->id.getNum();
	result["name"].String() = town->getNameTranslated();
	result["owner"].Integer() = town->tempOwner.getNum();
	result["factionID"].Integer() = town->getFactionID().getNum();
	result["townLevel"].Integer() = town->getTownLevel();
	result["fortLevel"].Integer() = town->fortLevel();
	result["upperArmyStrength"].Integer() = static_cast<int64_t>(town->getUpperArmy()->getArmyStrength());
	result["armyStrength"].Integer() = static_cast<int64_t>(town->getUpperArmy()->getArmyStrength());
	result["upperArmy"] = armySnapshot(town->getUpperArmy());
	result["hasCapitol"].Bool() = town->hallLevel() >= 3;
	result["hasBuiltResourceMarketplace"].Bool() = town->getMarketEfficiency() > 0;
	result["visitablePos"] = tileSnapshot(town->visitablePos());

	if(const auto * visitingHero = town->getVisitingHero())
		result["visitingHero"] = heroSnapshot(visitingHero);
	if(const auto * garrisonHero = town->getGarrisonHero())
		result["garrisonHero"] = heroSnapshot(garrisonHero);

	auto availableHeroes = callback->getAvailableHeroes(town);
	result["canRecruitHero"].Bool() = !availableHeroes.empty();
	result["availableHeroes"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto * hero : availableHeroes)
		result["availableHeroes"].Vector().push_back(heroSnapshot(hero));

	result["availableToBuy"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(size_t level = 0; level < town->creatures.size(); ++level)
	{
		const auto & entry = town->creatures[level];
		if(entry.first > 0 && !entry.second.empty())
			result["availableToBuy"].Vector().push_back(purchasableCreatureSnapshot(entry.second.back(), entry.first, static_cast<int>(level)));
	}

	result["threats"].setType(JsonNode::JsonType::DATA_VECTOR);
	return result;
}

JsonNode makeSnapshot(const std::shared_ptr<CCallback> & callback)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["freeResources"] = resourcesSnapshot(callback->getResourceAmount());

	result["townsInfo"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto * town : callback->getTownsInfo())
		result["townsInfo"].Vector().push_back(townSnapshot(town, callback));

	result["heroesInfo"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto * hero : callback->getHeroesInfo())
		result["heroesInfo"].Vector().push_back(heroSnapshot(hero));

	return result;
}

}

std::string CLuaNullkiller2AI::getBattleAIName() const
{
	return "BattleAI";
}

void CLuaNullkiller2AI::initGameInterface(std::shared_ptr<Environment> env, std::shared_ptr<CCallback> callback)
{
	this->env = env;
	cc = callback;
	cbc = callback;
	human = false;
	playerID = *cc->getPlayerID();
}

void CLuaNullkiller2AI::answerQuery(QueryID queryID, int selection) const
{
	cc->selectionMade(selection, queryID);
}

bool CLuaNullkiller2AI::executeCommand(const LuaCommand & command)
{
	if(command.name == "setTargetObject")
	{
		targetObjectID = commandInteger(command, "objid").value_or(0);
		return true;
	}

	if(command.name == "setActive")
	{
		activeHeroID = commandInteger(command, "hero").value_or(-1);
		targetX = commandInteger(command, "x").value_or(-1);
		targetY = commandInteger(command, "y").value_or(-1);
		targetZ = commandInteger(command, "z").value_or(-1);
		return true;
	}

	if(command.name == "invalidatePathfinderData")
	{
		pathfinderInvalidated = true;
		return true;
	}

	if(command.name == "resetObjectClusterizer")
		return true;

	if(command.name == "lockResources")
		return true;

	if(command.name == "recruitHero")
	{
		const auto townID = commandInteger(command, "town");
		const auto heroID = commandInteger(command, "hero");
		if(!townID || !heroID)
			return false;

		const auto * townOrTavern = cc->getObj(ObjectInstanceID(*townID), false);
		const auto * hero = cc->getHero(ObjectInstanceID(*heroID));
		if(!townOrTavern || !hero)
			return false;

		cc->recruitHero(townOrTavern, hero);
		return true;
	}

	if(command.name == "buildBuilding")
	{
		const auto townID = commandInteger(command, "town");
		const auto buildingID = commandInteger(command, "bid");
		if(!townID || !buildingID)
			return false;

		const auto * town = cc->getTown(ObjectInstanceID(*townID));
		return town && cc->buildBuilding(town, BuildingID(*buildingID));
	}

	if(command.name == "buildBoat")
	{
		const auto shipyardID = commandInteger(command, "shipyard");
		if(!shipyardID)
			return false;

		const auto * object = cc->getObj(ObjectInstanceID(*shipyardID), false);
		const auto * shipyard = dynamic_cast<const IShipyard *>(object);
		if(!shipyard)
			return false;

		cc->buildBoat(shipyard);
		return true;
	}

	if(command.name == "dismissHero")
	{
		const auto heroID = commandInteger(command, "hero");
		if(!heroID)
			return false;

		const auto * hero = cc->getHero(ObjectInstanceID(*heroID));
		return hero && cc->dismissHero(hero);
	}

	if(command.name == "swapGarrisonHero")
	{
		const auto townID = commandInteger(command, "town");
		if(!townID)
			return false;

		const auto * town = cc->getTown(ObjectInstanceID(*townID));
		if(!town)
			return false;

		cc->swapGarrisonHero(town);
		return true;
	}

	if(command.name == "recruitCreatures")
	{
		const auto townID = commandInteger(command, "town");
		const auto destinationID = commandInteger(command, "dst");
		const auto creatureID = commandInteger(command, "creature");
		const auto count = commandInteger(command, "count");
		if(!townID || !destinationID || !creatureID || !count)
			return false;

		const auto * dwelling = dynamic_cast<const CGDwelling *>(cc->getObj(ObjectInstanceID(*townID), false));
		const auto * destination = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(*destinationID), false));
		if(!dwelling || !destination)
			return false;

		cc->recruitCreatures(dwelling, destination, CreatureID(*creatureID), *count, commandInteger(command, "level").value_or(-1));
		return true;
	}

	if(command.name == "dismissCreature")
	{
		const auto armyID = commandInteger(command, "army");
		const auto slotID = commandInteger(command, "slot");
		if(!armyID || !slotID)
			return false;

		const auto * army = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(*armyID), false));
		return army && cc->dismissCreature(army, SlotID(*slotID));
	}

	if(command.name == "castSpell")
	{
		const auto heroID = commandInteger(command, "hero");
		const auto spellID = commandInteger(command, "spell");
		if(!heroID || !spellID)
			return false;

		const auto * hero = cc->getHero(ObjectInstanceID(*heroID));
		if(!hero)
			return false;

		const int3 target(
			commandInteger(command, "x").value_or(-1),
			commandInteger(command, "y").value_or(-1),
			commandInteger(command, "z").value_or(-1));
		cc->castSpell(hero, SpellID(*spellID), target);
		return true;
	}

	if(command.name == "moveHeroToTile")
	{
		const auto heroID = commandInteger(command, "hero");
		const auto x = commandInteger(command, "x");
		const auto y = commandInteger(command, "y");
		const auto z = commandInteger(command, "z");
		if(!heroID || !x || !y || !z)
			return false;

		const auto * hero = cc->getHero(ObjectInstanceID(*heroID));
		if(!hero)
			return false;

		const int3 visitableDestination(*x, *y, *z);
		cc->moveHero(hero, hero->convertFromVisitablePos(visitableDestination), false);
		return true;
	}

	logAi->warn("LuaNullkiller2 unsupported command: %s", command.name.c_str());
	return false;
}

void CLuaNullkiller2AI::yourTurn(QueryID queryID)
{
	answerQuery(queryID);
	activeHeroID = -1;
	targetObjectID = 0;
	targetX = -1;
	targetY = -1;
	targetZ = -1;
	pathfinderInvalidated = false;

	LuaNullkiller2Runner runner;
	LuaRunInput input;
	input.difficultyLevel = cc->getStartInfo()->difficulty;
	input.snapshot = makeSnapshot(cc);
	input.commandHandler = [this](const LuaCommand & command)
	{
		return executeCommand(command);
	};

	const LuaTurnResult result = runner.runDay([this]()
	{
		cc->endTurn();
	}, input);

	if(!result.ok)
		logAi->error("LuaNullkiller2 runDay failed: %s", result.error);

	if(!result.requestedEndTurn)
		cc->endTurn();
}

void CLuaNullkiller2AI::heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID)
{
	answerQuery(queryID);
}

void CLuaNullkiller2AI::commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID)
{
	answerQuery(queryID);
}

void CLuaNullkiller2AI::showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept)
{
	answerQuery(askID);
}

void CLuaNullkiller2AI::showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle)
{
	answerQuery(queryID);
}

void CLuaNullkiller2AI::showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID)
{
	answerQuery(askID);
}

void CLuaNullkiller2AI::showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects)
{
	answerQuery(askID, std::max(targetObjectID, 0));
}

std::optional<BattleAction> CLuaNullkiller2AI::makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState)
{
	return std::nullopt;
}

}
