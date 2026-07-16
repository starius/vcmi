/*
 * VGTRecorder.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "../lib/constants/EntityIdentifiers.h"

#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>

class BattleAction;
class CGameState;
class CGameHandler;
struct CPackForClient;
struct CPackForServer;

class VGTRecorder final
{
	std::ofstream output;
	mutable std::mutex outputMutex;
	std::string outputPath;
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
	std::optional<std::string> activeBattleBlock;
	std::map<PlayerColor, std::string> latestTimerStates;
	std::map<PlayerColor, std::string> turnStartTimerStates;

	VGTRecorder() = default;

	void initializeFromEnvironment();
	void ensureHeader(const CGameState & gameState);
	void startTurnDocument(const CGameState & gameState, PlayerColor player);
	void startWorldDocument(const CGameState & gameState, const std::string & phase);
	void writeActionLine(const CGameState & gameState, const std::string & line);
	void writeBaselineSave(CGameHandler & gameHandler);
	void writeTurnState(CGameHandler & gameHandler);

public:
	static VGTRecorder & get();

	bool isEnabled();
	void setRandomSeed(int seed);

	void recordDecision(const CGameState & gameState, CPackForServer & pack);
	void recordEffect(const CGameState & gameState, CPackForClient & pack);
	void recordTimerEndTurn(const CGameState & gameState, PlayerColor player);
	void recordTimerBattleAction(const CGameState & gameState, PlayerColor player, BattleID battleID, const BattleAction & action);
	void recordAppliedState(CGameHandler & gameHandler);
};
