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
#include "../../lib/battle/BattleStateInfoForRetreat.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/CCreatureHandler.h"
#include "../../lib/GameConstants.h"
#include "../../lib/StartInfo.h"
#include "../../lib/entities/artifact/CArtifact.h"
#include "../../lib/entities/artifact/CArtifactInstance.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CStackInstance.h"
#include "../../lib/mapObjects/IObjectInterface.h"
#include "../../lib/networkPacks/ArtifactLocation.h"
#include "../../lib/networkPacks/Component.h"

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

std::string componentTypeName(ComponentType type)
{
	switch(type)
	{
	case ComponentType::NONE:
		return "NONE";
	case ComponentType::PRIM_SKILL:
		return "PRIM_SKILL";
	case ComponentType::SEC_SKILL:
		return "SEC_SKILL";
	case ComponentType::RESOURCE:
		return "RESOURCE";
	case ComponentType::RESOURCE_PER_DAY:
		return "RESOURCE_PER_DAY";
	case ComponentType::CREATURE:
		return "CREATURE";
	case ComponentType::ARTIFACT:
		return "ARTIFACT";
	case ComponentType::SPELL_SCROLL:
		return "SPELL_SCROLL";
	case ComponentType::MANA:
		return "MANA";
	case ComponentType::EXPERIENCE:
		return "EXPERIENCE";
	case ComponentType::LEVEL:
		return "LEVEL";
	case ComponentType::SPELL:
		return "SPELL";
	case ComponentType::MORALE:
		return "MORALE";
	case ComponentType::LUCK:
		return "LUCK";
	case ComponentType::BUILDING:
		return "BUILDING";
	case ComponentType::HERO_PORTRAIT:
		return "HERO_PORTRAIT";
	case ComponentType::FLAG:
		return "FLAG";
	}
	return "UNKNOWN";
}

JsonNode componentSnapshot(const Component & component)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["type"].String() = componentTypeName(component.type);
	result["typeID"].Integer() = static_cast<int>(component.type);
	if(component.subType.hasValue())
		result["subType"].Integer() = component.subType.getNum();
	if(component.value)
		result["value"].Integer() = *component.value;
	return result;
}

JsonNode componentsSnapshot(const std::vector<Component> & components)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & component : components)
		result.Vector().push_back(componentSnapshot(component));
	return result;
}

void addArmySnapshotFields(JsonNode & result, const CArmedInstance * army);
JsonNode armySnapshot(const CArmedInstance * army);

JsonNode artifactTypeSnapshot(const CArtifact * artifactType)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	if(!artifactType)
		return result;

	result["id"].Integer() = artifactType->getId().getNum();
	result["ID"].String() = artifactType->getJsonKey();
	result["name"].String() = artifactType->getJsonKey();
	result["price"].Integer() = artifactType->getPrice();
	result["isScroll"].Bool() = artifactType->isScroll();
	result["spellScroll"].Bool() = artifactType->isScroll();
	result["possibleSlots"].setType(JsonNode::JsonType::DATA_VECTOR);
	if(const auto slots = artifactType->getPossibleSlots().find(ArtBearer::HERO); slots != artifactType->getPossibleSlots().end())
	{
		for(const auto & slot : slots->second)
			result["possibleSlots"].Vector().push_back(JsonNode(static_cast<int64_t>(slot.getNum())));
	}
	result["exportedBonuses"] = artifactType->getExportedBonusList().toJsonNode();
	result["constituents"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto * constituent : artifactType->getConstituents())
		result["constituents"].Vector().push_back(artifactTypeSnapshot(constituent));
	return result;
}

JsonNode artifactSnapshot(const CArtifactInstance * artifact, const CArtifactSet * holder)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	if(!artifact)
		return result;

	const auto * artifactType = artifact->getType();
	result["id"].Integer() = artifact->getId().getNum();
	result["instanceID"].Integer() = artifact->getId().getNum();
	result["artifactID"].Integer() = artifact->getTypeId().getNum();
	result["artifactType"] = artifactTypeSnapshot(artifactType);
	if(artifactType)
	{
		result["typeID"].Integer() = artifactType->getId().getNum();
		result["typeName"].String() = artifactType->getJsonKey();
		result["possibleSlots"] = result["artifactType"]["possibleSlots"];
		result["exportedBonuses"] = result["artifactType"]["exportedBonuses"];
		result["constituents"] = result["artifactType"]["constituents"];
	}
	result["isScroll"].Bool() = artifact->isScroll();
	result["spellScroll"].Bool() = artifact->isScroll();
	if(artifact->isScroll())
		result["scrollSpellID"].Integer() = artifact->getScrollSpellID().getNum();
	if(holder)
	{
		result["canBePutAt"].setType(JsonNode::JsonType::DATA_STRUCT);
		for(ArtifactPosition slot(0); slot <= ArtifactPosition::BACKPACK_START; slot = ArtifactPosition(slot + 1))
			result["canBePutAt"][std::to_string(slot.getNum())].Bool() = artifact->canBePutAt(holder, slot, true);
	}
	return result;
}

JsonNode artifactSlotSnapshot(const ArtifactPosition & slot, const ArtSlotInfo & slotInfo, const CArtifactSet * holder)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["slot"].Integer() = slot.getNum();
	result["locked"].Bool() = slotInfo.locked;
	if(const auto * artifact = slotInfo.getArt())
		result["artifact"] = artifactSnapshot(artifact, holder);
	return result;
}

void addHeroArtifactSnapshotFields(JsonNode & result, const CGHeroInstance * hero)
{
	if(!hero)
		return;

	result["artifactsWorn"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & [slot, slotInfo] : hero->artifactsWorn)
		result["artifactsWorn"].Vector().push_back(artifactSlotSnapshot(slot, slotInfo, hero));

	result["artifactsInBackpack"].setType(JsonNode::JsonType::DATA_VECTOR);
	ArtifactPosition backpackSlot = ArtifactPosition::BACKPACK_START;
	for(const auto & slotInfo : hero->artifactsInBackpack)
	{
		result["artifactsInBackpack"].Vector().push_back(artifactSlotSnapshot(backpackSlot, slotInfo, hero));
		backpackSlot = ArtifactPosition(backpackSlot + 1);
	}
}

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
	addHeroArtifactSnapshotFields(result, hero);
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

	if(command.name == "endTurn")
		return true;

	if(command.name == "answerQuery")
	{
		const auto queryID = commandInteger(command, "query");
		if(!queryID)
			return false;

		answerQuery(QueryID(*queryID), commandInteger(command, "selection").value_or(0));
		return true;
	}

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

	if(command.name == "upgradeCreature")
	{
		const auto armyID = commandInteger(command, "army");
		const auto slotID = commandInteger(command, "slot");
		const auto creatureID = commandInteger(command, "creature");
		if(!armyID || !slotID || !creatureID)
			return false;

		const auto * army = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(*armyID), false));
		if(!army)
			return false;

		cc->upgradeCreature(army, SlotID(*slotID), CreatureID(*creatureID));
		return true;
	}

	if(command.name == "mergeStacks")
	{
		const auto armyID = commandInteger(command, "army");
		const auto fromSlot = commandInteger(command, "fromSlot");
		const auto toSlot = commandInteger(command, "toSlot");
		if(!armyID || !fromSlot || !toSlot)
			return false;

		const auto * army = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(*armyID), false));
		if(!army)
			return false;

		cc->mergeStacks(army, army, SlotID(*fromSlot), SlotID(*toSlot));
		return true;
	}

	if(command.name == "mergeOrSwapStacks")
	{
		const auto sourceID = commandInteger(command, "src");
		const auto destinationID = commandInteger(command, "dst");
		const auto fromSlot = commandInteger(command, "fromSlot");
		const auto toSlot = commandInteger(command, "toSlot");
		if(!sourceID || !destinationID || !fromSlot || !toSlot)
			return false;

		const auto * source = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(*sourceID), false));
		const auto * destination = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(*destinationID), false));
		if(!source || !destination)
			return false;

		cc->mergeOrSwapStacks(source, destination, SlotID(*fromSlot), SlotID(*toSlot));
		return true;
	}

	if(command.name == "splitStack")
	{
		const auto sourceID = commandInteger(command, "src");
		const auto destinationID = commandInteger(command, "dst");
		const auto fromSlot = commandInteger(command, "fromSlot");
		const auto toSlot = commandInteger(command, "toSlot");
		const auto count = commandInteger(command, "count");
		if(!sourceID || !destinationID || !fromSlot || !toSlot || !count)
			return false;

		const auto * source = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(*sourceID), false));
		const auto * destination = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(*destinationID), false));
		if(!source || !destination)
			return false;

		cc->splitStack(source, destination, SlotID(*fromSlot), SlotID(*toSlot), *count);
		return true;
	}

	if(command.name == "swapArtifacts")
	{
		const auto sourceHeroID = commandInteger(command, "srcHero");
		const auto sourceSlot = commandInteger(command, "srcSlot");
		const auto destinationHeroID = commandInteger(command, "dstHero");
		const auto destinationSlot = commandInteger(command, "dstSlot");
		if(!sourceHeroID || !sourceSlot || !destinationHeroID || !destinationSlot)
			return false;

		const auto * sourceHero = cc->getHero(ObjectInstanceID(*sourceHeroID));
		const auto * destinationHero = cc->getHero(ObjectInstanceID(*destinationHeroID));
		if(!sourceHero || !destinationHero)
			return false;

		return cc->swapArtifacts(
			ArtifactLocation(sourceHero->id, ArtifactPosition(*sourceSlot)),
			ArtifactLocation(destinationHero->id, ArtifactPosition(*destinationSlot)));
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

void CLuaNullkiller2AI::heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2, QueryID query)
{
	const auto * firstHero = cc->getHero(hero1);
	const auto * secondHero = cc->getHero(hero2);
	if(!firstHero || !secondHero)
	{
		answerQuery(query);
		return;
	}

	LuaNullkiller2Runner runner;
	LuaRunInput input;
	input.difficultyLevel = cc->getStartInfo()->difficulty;
	input.snapshot.setType(JsonNode::JsonType::DATA_STRUCT);
	input.snapshot["firstHero"] = heroSnapshot(firstHero);
	input.snapshot["secondHero"] = heroSnapshot(secondHero);
	input.snapshot["activeHeroID"].Integer() = activeHeroID;
	input.snapshot["queryID"].Integer() = query.getNum();

	bool queryAnswered = false;
	input.commandHandler = [this, &queryAnswered](const LuaCommand & command)
	{
		const bool executed = executeCommand(command);
		if(executed && command.name == "answerQuery")
			queryAnswered = true;
		return executed;
	};

	const LuaTurnResult result = runner.runFunction("heroExchangeStarted", [](){}, input);

	if(!result.ok)
		logAi->error("LuaNullkiller2 heroExchangeStarted failed: %s", result.error);

	if(!queryAnswered)
		answerQuery(query);
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
	LuaNullkiller2Runner runner;
	LuaRunInput input;
	input.difficultyLevel = cc->getStartInfo()->difficulty;
	input.snapshot.setType(JsonNode::JsonType::DATA_STRUCT);
	input.snapshot["queryID"].Integer() = askID.getNum();
	input.snapshot["selection"].Bool() = selection;
	input.snapshot["cancel"].Bool() = cancel;
	input.snapshot["safeToAutoaccept"].Bool() = safeToAutoaccept;
	input.snapshot["components"] = componentsSnapshot(components);
	input.snapshot["goalObjectID"].Integer() = targetObjectID;
	if(targetX >= 0 && targetY >= 0 && targetZ >= 0)
		input.snapshot["target"] = tileSnapshot(int3(targetX, targetY, targetZ));
	if(activeHeroID >= 0)
	{
		if(const auto * activeHero = cc->getHero(ObjectInstanceID(activeHeroID)))
			input.snapshot["hero"] = heroSnapshot(activeHero);
	}

	bool queryAnswered = false;
	input.commandHandler = [this, &queryAnswered](const LuaCommand & command)
	{
		const bool executed = executeCommand(command);
		if(executed && command.name == "answerQuery")
			queryAnswered = true;
		return executed;
	};

	const LuaTurnResult result = runner.runFunction("showBlockingDialog", [](){}, input);

	if(!result.ok)
		logAi->error("LuaNullkiller2 showBlockingDialog failed: %s", result.error);

	if(!queryAnswered)
		answerQuery(askID);
}

void CLuaNullkiller2AI::showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle)
{
	if(!up || !down)
	{
		answerQuery(queryID);
		return;
	}

	LuaNullkiller2Runner runner;
	LuaRunInput input;
	input.difficultyLevel = cc->getStartInfo()->difficulty;
	input.snapshot.setType(JsonNode::JsonType::DATA_STRUCT);
	input.snapshot["queryID"].Integer() = queryID.getNum();
	input.snapshot["up"] = armySnapshot(up);
	input.snapshot["down"] = heroSnapshot(down);
	input.snapshot["removableUnits"].Bool() = removableUnits;
	input.snapshot["restrictedGarrisonsForAI"].Bool() = cc->getStartInfo()->restrictedGarrisonsForAI();

	bool queryAnswered = false;
	input.commandHandler = [this, &queryAnswered](const LuaCommand & command)
	{
		const bool executed = executeCommand(command);
		if(executed && command.name == "answerQuery")
			queryAnswered = true;
		return executed;
	};

	const LuaTurnResult result = runner.runFunction("showGarrisonDialog", [](){}, input);

	if(!result.ok)
		logAi->error("LuaNullkiller2 showGarrisonDialog failed: %s", result.error);

	if(!queryAnswered)
		answerQuery(queryID);
}

void CLuaNullkiller2AI::showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID)
{
	answerQuery(askID);
}

void CLuaNullkiller2AI::showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects)
{
	LuaNullkiller2Runner runner;
	LuaRunInput input;
	input.difficultyLevel = cc->getStartInfo()->difficulty;
	input.snapshot.setType(JsonNode::JsonType::DATA_STRUCT);
	input.snapshot["queryID"].Integer() = askID.getNum();
	input.snapshot["selectedObject"].Integer() = targetObjectID;
	input.snapshot["objects"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & object : objects)
		input.snapshot["objects"].Vector().push_back(JsonNode(static_cast<int64_t>(object.getNum())));

	bool queryAnswered = false;
	input.commandHandler = [this, &queryAnswered](const LuaCommand & command)
	{
		const bool executed = executeCommand(command);
		if(executed && command.name == "answerQuery")
			queryAnswered = true;
		return executed;
	};

	const LuaTurnResult result = runner.runFunction("showMapObjectSelectDialog", [](){}, input);

	if(!result.ok)
		logAi->error("LuaNullkiller2 showMapObjectSelectDialog failed: %s", result.error);

	if(!queryAnswered)
		answerQuery(askID);
}

std::optional<BattleAction> CLuaNullkiller2AI::makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState)
{
	LuaNullkiller2Runner runner;
	LuaRunInput input;
	input.difficultyLevel = cc->getStartInfo()->difficulty;
	input.snapshot.setType(JsonNode::JsonType::DATA_STRUCT);
	input.snapshot["townsCount"].Integer() = static_cast<int64_t>(cc->getTownsInfo().size());
	input.snapshot["battleState"].setType(JsonNode::JsonType::DATA_STRUCT);
	input.snapshot["battleState"]["ourStrength"].Integer() = static_cast<int64_t>(battleState.getOurStrength());
	input.snapshot["battleState"]["enemyStrength"].Integer() = static_cast<int64_t>(battleState.getEnemyStrength());
	input.snapshot["battleState"]["canFlee"].Bool() = battleState.canFlee;
	input.snapshot["battleState"]["canSurrender"].Bool() = battleState.canSurrender;
	input.snapshot["battleState"]["isLastTurnBeforeDie"].Bool() = battleState.isLastTurnBeforeDie;
	input.snapshot["battleState"]["ourSide"].Integer() = static_cast<int>(battleState.ourSide);
	if(battleState.ourHero)
	{
		input.snapshot["battleState"]["ourHero"].setType(JsonNode::JsonType::DATA_STRUCT);
		input.snapshot["battleState"]["ourHero"]["id"].Integer() = battleState.ourHero->id.getNum();
		input.snapshot["battleState"]["ourHero"]["patrol"].setType(JsonNode::JsonType::DATA_STRUCT);
		input.snapshot["battleState"]["ourHero"]["patrol"]["patrolling"].Bool() = battleState.ourHero->patrol.patrolling;
	}

	const LuaTurnResult result = runner.runFunction("makeSurrenderRetreatDecision", [](){}, input);
	if(!result.ok)
	{
		logAi->error("LuaNullkiller2 makeSurrenderRetreatDecision failed: %s", result.error);
		return std::nullopt;
	}

	if(result.status == "retreat")
	{
		const auto side = result.integers.find("side");
		if(side != result.integers.end())
			return BattleAction::makeRetreat(static_cast<BattleSide>(side->second));

		logAi->error("LuaNullkiller2 makeSurrenderRetreatDecision returned retreat without side");
	}

	return std::nullopt;
}

}
