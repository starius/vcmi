/*
 * CommandLine.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */

#include "StdInc.h"
#include "CommandLine.h"

#include "Replay.h"

#include "../../lib/logging/CBasicLogConfigurator.h"
#include "../../lib/logging/CLogger.h"

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include <stdexcept>

namespace vgt
{

void addCommandLineOptions(boost::program_options::options_description & options)
{
	options.add_options()
		("vgt-replay-json", boost::program_options::value<std::string>(), "Replay a normalized VGT transcript JSON file")
		("vgt-replay-save", boost::program_options::value<std::string>(), "Write replayed state to this save file")
		("vgt-replay-no-save", "Replay without writing a final save file")
		("vgt-replay-game-state-save", boost::program_options::value<std::string>(), "Write replayed game state only to this save file")
		("vgt-replay-expected-turn-states", boost::program_options::value<std::string>(), "Compare every replayed turn state with this directory")
		("vgt-replay-turn-states", boost::program_options::value<std::string>(), "Write every replayed turn state to this directory")
		("vgt-replay-captured-battle-outcomes", boost::program_options::value<std::string>(), "Write tactical-end survivor and randomizer state as JSON")
		("vgt-replay-fast-forward-battles", "Apply recorded battle outcomes without replaying tactical events")
		("vgt-replay-log-level",
			boost::program_options::value<std::string>()->default_value("info"),
			"Set VGT replay log level")
		("vgt-dump-game-state-save", boost::program_options::value<std::string>(), "Write a VGT game state save summary for diagnostics")
		("vgt-dump-output", boost::program_options::value<std::string>(), "Path for --vgt-dump-game-state-save output")
		("vgt-normalize-game-state-save", boost::program_options::value<std::string>(), "Load and rewrite a VGT game state save for diagnostics")
		("vgt-normalize-output", boost::program_options::value<std::string>(), "Path for --vgt-normalize-game-state-save output");
}

void configureLogLevel(const boost::program_options::variables_map & options)
{
	if(!options.count("vgt-replay-json"))
		return;

	const auto logLevel = CBasicLogConfigurator::getLogLevel(options["vgt-replay-log-level"].as<std::string>());
	CLogger::getGlobalLogger()->setLevel(logLevel);
}

std::optional<int> handleCommandLine(const boost::program_options::variables_map & options)
{
	if(options.count("vgt-replay-json"))
	{
		if(options.count("vgt-replay-save") == options.count("vgt-replay-no-save"))
		{
			throw std::runtime_error(
				"Exactly one of --vgt-replay-save and --vgt-replay-no-save is required with --vgt-replay-json");
		}

		VGTReplayOptions replayOptions;
		replayOptions.inputJson = options["vgt-replay-json"].as<std::string>();
		if(options.count("vgt-replay-save"))
			replayOptions.outputSave = options["vgt-replay-save"].as<std::string>();
		if(options.count("vgt-replay-game-state-save"))
			replayOptions.outputGameStateSave = options["vgt-replay-game-state-save"].as<std::string>();
		if(options.count("vgt-replay-expected-turn-states"))
			replayOptions.expectedTurnStateDirectory = options["vgt-replay-expected-turn-states"].as<std::string>();
		if(options.count("vgt-replay-turn-states"))
			replayOptions.outputTurnStateDirectory = options["vgt-replay-turn-states"].as<std::string>();
		if(options.count("vgt-replay-captured-battle-outcomes"))
			replayOptions.capturedBattleOutcomes = options["vgt-replay-captured-battle-outcomes"].as<std::string>();
		replayOptions.fastForwardBattles = options.count("vgt-replay-fast-forward-battles") != 0;
		return replayVGTJson(replayOptions);
	}

	if(options.count("vgt-dump-game-state-save"))
	{
		if(!options.count("vgt-dump-output"))
			throw std::runtime_error("--vgt-dump-output is required with --vgt-dump-game-state-save");

		VGTGameStateSummaryOptions summaryOptions;
		summaryOptions.inputSave = options["vgt-dump-game-state-save"].as<std::string>();
		summaryOptions.outputSummary = options["vgt-dump-output"].as<std::string>();
		return dumpVGTGameStateSummary(summaryOptions);
	}

	if(options.count("vgt-normalize-game-state-save"))
	{
		if(!options.count("vgt-normalize-output"))
			throw std::runtime_error("--vgt-normalize-output is required with --vgt-normalize-game-state-save");

		VGTGameStateNormalizeOptions normalizeOptions;
		normalizeOptions.inputSave = options["vgt-normalize-game-state-save"].as<std::string>();
		normalizeOptions.outputSave = options["vgt-normalize-output"].as<std::string>();
		return normalizeVGTGameStateSave(normalizeOptions);
	}

	return std::nullopt;
}

}
