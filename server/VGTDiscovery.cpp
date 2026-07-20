/*
 * VGTDiscovery.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */

#include "StdInc.h"
#include "VGTDiscovery.h"

#include "../lib/CCreatureHandler.h"
#include "../lib/CPlayerState.h"
#include "../lib/GameLibrary.h"
#include "../lib/entities/artifact/CArtifact.h"
#include "../lib/entities/artifact/CArtifactInstance.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/mapping/CMap.h"
#include "../lib/mapObjects/CGDwelling.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapObjects/CGObjectInstance.h"
#include "../lib/mapObjects/CGTownInstance.h"
#include "../lib/mapObjects/CQuest.h"
#include "../lib/mapObjects/MiscObjects.h"
#include "../lib/mapObjects/army/CArmedInstance.h"

namespace
{
bool isCreatureBank(const CGObjectInstance & object)
{
	switch(object.ID.toEnum())
	{
	case MapObjectID::CREATURE_BANK:
	case MapObjectID::DERELICT_SHIP:
	case MapObjectID::DRAGON_UTOPIA:
	case MapObjectID::CRYPT:
	case MapObjectID::SHIPWRECK:
		return true;
	case MapObjectID::PYRAMID:
		return object.getObjTypeIndex().getNum() == 0;
	default:
		return false;
	}
}

bool isHighLevelDwelling(const CGObjectInstance & object)
{
	const auto * dwelling = dynamic_cast<const CGDwelling *>(&object);
	if(!dwelling || object.ID == Obj::TOWN || object.ID == Obj::REFUGEE_CAMP ||
		object.ID == Obj::WAR_MACHINE_FACTORY)
		return false;

	for(const auto & pool : dwelling->creatures)
	{
		for(const CreatureID creatureID : pool.second)
		{
			const auto * creature = LIBRARY->creatures()->getById(creatureID);
			if(creature && creature->getLevel() >= 5)
				return true;
		}
	}
	return false;
}

bool isHighTierArtifact(const CGObjectInstance & object)
{
	const auto * artifactObject = dynamic_cast<const CGArtifact *>(&object);
	const auto * instance = artifactObject ? artifactObject->getArtifactInstance() : nullptr;
	const auto * type = instance ? instance->getType() : nullptr;
	return type && (type->aClass == EArtifactClass::ART_MAJOR || type->aClass == EArtifactClass::ART_RELIC);
}

bool isStrategicDiscovery(const CGameState & gameState, PlayerColor player, const CGObjectInstance & object)
{
	if(object.ID == Obj::TOWN || object.ID == Obj::LIBRARY_OF_ENLIGHTENMENT ||
		isCreatureBank(object) || dynamic_cast<const IQuestObject *>(&object) ||
		isHighTierArtifact(object) || isHighLevelDwelling(object))
		return true;

	const auto * army = dynamic_cast<const CArmedInstance *>(&object);
	return army && object.tempOwner.isValidPlayer() &&
		gameState.getPlayerRelations(player, object.tempOwner) == PlayerRelations::ENEMIES;
}

bool isStationaryStrategicCandidate(const CGObjectInstance & object)
{
	if(dynamic_cast<const CGHeroInstance *>(&object))
		return false;
	if(object.ID == Obj::TOWN || object.ID == Obj::LIBRARY_OF_ENLIGHTENMENT ||
		isCreatureBank(object) || dynamic_cast<const IQuestObject *>(&object) ||
		isHighTierArtifact(object) || isHighLevelDwelling(object))
		return true;
	return dynamic_cast<const CArmedInstance *>(&object) != nullptr;
}
}

std::set<ObjectInstanceID> & VGTDiscoveryTracker::initialize(const CGameState & gameState, PlayerColor player)
{
	const auto * team = gameState.getPlayerTeam(player);
	if(!team)
		throw std::runtime_error("Unable to track VGT discoveries for a player without a team");
	auto [known, inserted] = knownObjects.try_emplace(team->id);
	if(inserted)
	{
		for(const auto * object : gameState.getMap().getObjects())
		{
			if(isStrategicDiscovery(gameState, player, *object) && gameState.isVisibleFor(object, player))
				known->second.insert(object->id);
		}
	}
	return known->second;
}

void VGTDiscoveryTracker::buildStationaryIndex(const CGameState & gameState)
{
	const CMap & map = gameState.getMap();
	if(indexedMap == &map)
		return;

	indexedMap = &map;
	stationaryObjectsByTile.clear();
	stationaryArmies.clear();
	for(const auto * object : map.getObjects())
	{
		if(!isStationaryStrategicCandidate(*object))
			continue;
		if(dynamic_cast<const CArmedInstance *>(object) &&
			(object->tempOwner.isValidPlayer() || object->ID == Obj::GARRISON || object->ID == Obj::GARRISON2))
			stationaryArmies.push_back(object->id);
		for(int y = 0; y < object->getHeight(); ++y)
		{
			for(int x = 0; x < object->getWidth(); ++x)
			{
				const int3 tile = object->anchorPos() + int3(-x, -y, 0);
				if(map.isInTheMap(tile) && object->coveringAt(tile))
					stationaryObjectsByTile[tile].push_back(object->id);
			}
		}
	}
}

void VGTDiscoveryTracker::observeVisible(const CGameState & gameState, PlayerColor player)
{
	auto & known = initialize(gameState, player);
	buildStationaryIndex(gameState);
	auto observe = [&](ObjectInstanceID objectID)
	{
		if(known.contains(objectID))
			return;
		const auto * object = gameState.getMap().getObject(objectID);
		if(object && isStrategicDiscovery(gameState, player, *object) && gameState.isVisibleFor(object, player))
			known.insert(objectID);
	};
	for(const ObjectInstanceID objectID : stationaryArmies)
		observe(objectID);
	for(const ObjectInstanceID heroID : gameState.getMap().getHeroesOnMap())
		observe(heroID);
}

std::vector<ObjectInstanceID> VGTDiscoveryTracker::discoverInTiles(
	const CGameState & gameState,
	PlayerColor player,
	const std::set<int3> & tiles)
{
	auto & known = initialize(gameState, player);
	buildStationaryIndex(gameState);
	std::vector<ObjectInstanceID> result;
	auto discover = [&](ObjectInstanceID objectID)
	{
		if(known.contains(objectID))
			return;
		const auto * object = gameState.getMap().getObject(objectID);
		if(!object || !isStrategicDiscovery(gameState, player, *object))
			return;
		known.insert(objectID);
		result.push_back(objectID);
	};

	for(const int3 & tile : tiles)
	{
		if(const auto objects = stationaryObjectsByTile.find(tile); objects != stationaryObjectsByTile.end())
		{
			for(const ObjectInstanceID objectID : objects->second)
				discover(objectID);
		}
	}

	for(const ObjectInstanceID heroID : gameState.getMap().getHeroesOnMap())
	{
		const auto * hero = gameState.getMap().getObject(heroID);
		if(hero && CGameState::iteratePositionsUntilTrue(hero, [&](const int3 & tile)
		{
			return hero->coveringAt(tile) && tiles.contains(tile);
		}))
			discover(heroID);
	}
	return result;
}
