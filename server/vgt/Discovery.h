/*
 * Discovery.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "../../lib/constants/EntityIdentifiers.h"
#include "../../lib/int3.h"

#include <map>
#include <set>
#include <vector>

class CGameState;
class CMap;

/// Tracks first sightings of the deliberately small, strategic VGT discovery set.
/// It never changes game state; capture and replay both derive sightings from FoW.
class VGTDiscoveryTracker final
{
	std::map<TeamID, std::set<ObjectInstanceID>> knownObjects;
	const CMap * indexedMap = nullptr;
	std::map<int3, std::vector<ObjectInstanceID>> stationaryObjectsByTile;
	std::vector<ObjectInstanceID> stationaryArmies;

	std::set<ObjectInstanceID> & initialize(const CGameState & gameState, PlayerColor player);
	void buildStationaryIndex(const CGameState & gameState);

public:
	/// Marks every currently visible strategic object as already known.
	void observeVisible(const CGameState & gameState, PlayerColor player);

	/// Marks strategic objects whose visible graphics overlap tiles and returns only first sightings.
	std::vector<ObjectInstanceID> discoverInTiles(
		const CGameState & gameState,
		PlayerColor player,
		const std::set<int3> & tiles);

	/// Returns whether normal fog-of-war processing has discovered an object for the player's team.
	bool wasDiscovered(const CGameState & gameState, PlayerColor player, ObjectInstanceID objectID);
};
