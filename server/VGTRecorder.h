/*
 * VGTRecorder.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "VGTDiscovery.h"

#include "../lib/constants/EntityIdentifiers.h"

#include <fstream>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

class BattleAction;
class CGameState;
class CGameHandler;
struct CPackForClient;
struct CPackForServer;
struct SetAvailableCreatures;
struct TryMoveHero;
struct Bonus;

class VGTRecorder final
{
	struct PendingMove
	{
		std::string actor;
		std::string hero;
		std::array<int, 3> start = {};
		std::vector<std::array<int, 3>> route;
		std::vector<std::string> discoveries;
		int z = 0;
		bool transit = false;
	};

	struct PendingRecruit
	{
		struct Unit
		{
			std::string creature;
			uint32_t count = 0;
			int slot = -1;
			std::optional<std::string> pool;
		};

		std::string actor;
		std::string source;
		std::string destination;
		std::vector<Unit> units;
		std::map<std::string, int64_t> paid;
		std::map<std::string, int64_t> remaining;
	};

	struct PendingTrade
	{
		struct Exchange
		{
			std::string sold;
			uint32_t soldAmount = 0;
			std::string bought;
			uint32_t boughtAmount = 0;
		};

		std::string actor;
		std::string market;
		std::vector<Exchange> exchanges;
	};

	struct PendingArtifactTransfer
	{
		struct Move
		{
			std::string artifact;
			std::string from;
			std::string to;
		};

		std::string actor;
		std::string from;
		std::string to;
		std::vector<Move> moves;
	};

	struct PendingArmyArrangement
	{
		std::string actor;
		std::set<ObjectInstanceID> armies;
		std::map<ObjectInstanceID, std::shared_ptr<const Bonus>> initialUndeadMoraleBonuses;
	};

	struct PendingEncounter
	{
		struct TeleportExit
		{
			std::string object;
			std::array<int, 3> position = {};
		};

		std::string actor;
		std::string hero;
		ObjectInstanceID heroID;
		std::string object;
		int query = -1;
		bool selection = false;
		bool cancel = false;
		bool answered = false;
		bool battleFollows = false;
		std::optional<int32_t> answer;
		std::optional<std::string> text;
		std::optional<std::string> quest;
		bool standardQuestText = false;
		std::vector<std::string> outcomes;
		std::vector<std::string> discoveries;
		std::optional<PendingMove> approach;
		bool teleport = false;
		bool impassable = false;
		std::array<int, 3> teleportStart = {};
		std::vector<TeleportExit> teleportExits;
	};

	struct PendingHeroScene
	{
		struct Action
		{
			std::string original;
			std::string nested;
		};

		std::string hero;
		std::vector<Action> actions;
	};

	struct PendingQuery
	{
		std::string kind;
		std::string subject;
		std::vector<std::string> choices;
		bool selection = false;
		bool cancel = false;
	};

	struct PendingBattle
	{
		std::string id;
		std::string attacker;
		std::string defender;
		std::vector<std::string> units;
		std::map<std::string, int> positions;
		std::vector<std::string> events;
		std::string initialRandom;
		std::vector<std::string> outcome;
		std::vector<std::string> survivors;
		std::vector<std::string> createdUnits;
		std::string randomBeforeAftermath;
		std::string continuation;
		std::map<std::string, int64_t> manaChanges;
		std::set<ObjectInstanceID> randomizerParticipants;
		std::set<HeroTypeID> randomizerHeroes;
		std::vector<ObjectInstanceID> armyIDs;
		std::map<std::string, std::string> armies;
		std::vector<std::string> aftermath;
		bool ended = false;
		bool aftermathDecisionStarted = false;
	};

	struct PendingWeeklyReward
	{
		std::string object;
		std::string reward;
		std::string text;
	};

	std::ofstream output;
	std::ofstream battleOutput;
	mutable std::mutex outputMutex;
	std::string outputPath;
	std::string battleOutputPath;
	std::string baselineSavePath;
	std::string baselineGameStateSavePath;
	std::string turnStateDirectory;
	bool checkedEnvironment = false;
	bool enabled = false;
	bool baselineSaveEnabled = false;
	bool baselineGameStateSaveEnabled = false;
	bool turnStateArchiveEnabled = false;
	bool headerWritten = false;
	bool documentOpen = false;
	bool exitAfterAppliedState = false;
	std::optional<int> randomSeed;
	std::optional<int> exitAfterTurnEnds;
	int observedTurnEnds = 0;
	int archivedTurnStates = 0;
	std::optional<PlayerColor> currentTurnPlayer;
	std::optional<PlayerColor> pendingTurnStatePlayer;
	std::optional<PendingBattle> pendingBattle;
	std::map<std::string, std::string> lastBattleDecisions;
	std::optional<PendingMove> pendingMove;
	std::optional<PendingRecruit> pendingRecruit;
	std::optional<PendingTrade> pendingTrade;
	std::optional<PendingArtifactTransfer> pendingArtifactTransfer;
	std::optional<PendingArmyArrangement> pendingArmyArrangement;
	std::optional<PendingEncounter> pendingEncounter;
	std::optional<PendingHeroScene> pendingHeroScene;
	std::map<int, PendingQuery> pendingQueries;
	std::map<std::string, std::string> pendingInitialTownAvailability;
	std::map<std::string, std::string> pendingInitialDwellingAvailability;
	std::map<std::string, std::string> pendingWeeklyTownAvailability;
	std::map<std::string, std::string> pendingWeeklyDwellingAvailability;
	std::vector<PendingWeeklyReward> pendingWeeklyRewards;
	std::map<std::string, std::vector<std::string>> pendingWeeklySpawns;
	bool suppressDerivedEffects = false;
	bool betweenPlayerTurns = false;
	std::map<PlayerColor, std::string> latestTimerStates;
	std::map<PlayerColor, std::string> turnStartTimerStates;
	VGTDiscoveryTracker discoveryTracker;

	VGTRecorder() = default;

	void initializeFromEnvironment();
	void ensureHeader(const CGameState & gameState);
	void startTurnDocument(const CGameState & gameState, PlayerColor player);
	void startWorldDocument(const CGameState & gameState, const std::string & phase);
	void writeActionLine(const CGameState & gameState, const std::string & line);
	void flushPendingMove(const CGameState & gameState);
	void flushPendingRecruit(const CGameState & gameState);
	void flushPendingTrade(const CGameState & gameState);
	void flushPendingArtifactTransfer(const CGameState & gameState);
	void flushPendingArmyArrangement(const CGameState & gameState);
	void flushPendingEncounter(const CGameState & gameState);
	void flushPendingHeroScene();
	void flushPendingBattle(const CGameState & gameState);
	void collectInitialAvailability(const CGameState & gameState, const SetAvailableCreatures & availability);
	void flushPendingInitialAvailability(const CGameState & gameState);
	void collectWeeklyAvailability(const CGameState & gameState, const SetAvailableCreatures & availability);
	void flushPendingWeeklyWorldEvents(const CGameState & gameState);
	void collectDiscoveries(const CGameState & gameState, const TryMoveHero & move);
	void writeBaselineSave(CGameHandler & gameHandler);
	void writeTurnState(CGameHandler & gameHandler);

public:
	static VGTRecorder & get();

	bool isEnabled();
	void setRandomSeed(int seed);

	void recordDecision(CGameHandler & gameHandler, CPackForServer & pack);
	void recordEffect(CGameHandler & gameHandler, CPackForClient & pack);
	void recordTimerEndTurn(const CGameState & gameState, PlayerColor player);
	void recordTimerBattleAction(const CGameState & gameState, PlayerColor player, BattleID battleID, const BattleAction & action);
	void recordAppliedState(CGameHandler & gameHandler);
};
