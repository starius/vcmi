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
#include <mutex>
#include <optional>
#include <string>

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
	bool checkedEnvironment = false;
	bool enabled = false;
	bool baselineSaveEnabled = false;
	bool baselineGameStateSaveEnabled = false;
	bool headerWritten = false;
	bool documentOpen = false;
	bool exitAfterAppliedState = false;
	std::optional<int> randomSeed;
	std::optional<int> exitAfterTurnEnds;
	int observedTurnEnds = 0;
	std::optional<PlayerColor> currentTurnPlayer;

	VGTRecorder() = default;

	void initializeFromEnvironment();
	void ensureHeader(const CGameState & gameState);
	void startTurnDocument(const CGameState & gameState, PlayerColor player);
	void startWorldDocument(const CGameState & gameState, const std::string & phase);
	void writeActionLine(const CGameState & gameState, const std::string & line);
	void writeBaselineSave(CGameHandler & gameHandler);

public:
	static VGTRecorder & get();

	bool isEnabled();
	void setRandomSeed(int seed);

	void recordDecision(const CGameState & gameState, CPackForServer & pack);
	void recordEffect(const CGameState & gameState, CPackForClient & pack);
	void recordAppliedState(CGameHandler & gameHandler);
};
