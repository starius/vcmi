/*
 * VGTReplay.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <string>

struct VGTReplayOptions
{
	std::string inputJson;
	std::string outputSave;
	std::string outputGameStateSave;
	std::string expectedTurnStateDirectory;
	std::string outputTurnStateDirectory;
	std::string capturedBattleOutcomes;
	bool fastForwardBattles = false;
};

struct VGTGameStateSummaryOptions
{
	std::string inputSave;
	std::string outputSummary;
};

struct VGTGameStateNormalizeOptions
{
	std::string inputSave;
	std::string outputSave;
};

int replayVGTJson(const VGTReplayOptions & options);
int dumpVGTGameStateSummary(const VGTGameStateSummaryOptions & options);
int normalizeVGTGameStateSave(const VGTGameStateNormalizeOptions & options);
