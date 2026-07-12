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
struct CPackForClient;
struct CPackForServer;

class VGTRecorder final
{
	std::ofstream output;
	mutable std::mutex outputMutex;
	std::string outputPath;
	bool checkedEnvironment = false;
	bool enabled = false;
	bool headerWritten = false;
	bool documentOpen = false;
	std::optional<PlayerColor> currentTurnPlayer;

	VGTRecorder() = default;

	void initializeFromEnvironment();
	void ensureHeader(const CGameState & gameState);
	void startTurnDocument(const CGameState & gameState, PlayerColor player);
	void startWorldDocument(const CGameState & gameState, const std::string & phase);
	void writeActionLine(const CGameState & gameState, const std::string & line);

public:
	static VGTRecorder & get();

	bool isEnabled();

	void recordDecision(const CGameState & gameState, CPackForServer & pack);
	void recordEffect(const CGameState & gameState, CPackForClient & pack);
};
