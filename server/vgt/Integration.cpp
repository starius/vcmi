/*
 * Integration.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */

#include "StdInc.h"
#include "Integration.h"

#include "Recorder.h"

namespace vgt
{

void onGameInitialized(CGameHandler & gameHandler, int seed) noexcept
{
	try
	{
		VGTRecorder::get().beginGame(gameHandler, seed);
	}
	catch(const std::exception & error)
	{
		logGlobal->error("Unable to start VGT recording: %s", error.what());
	}
}

void onGameLoaded(CGameHandler & gameHandler, const std::string & savePath) noexcept
{
	try
	{
		VGTRecorder::get().resumeGame(gameHandler, savePath);
	}
	catch(const std::exception & error)
	{
		logGlobal->error("Unable to resume VGT recording: %s", error.what());
	}
}

void onGameClosing(CGameHandler & gameHandler) noexcept
{
	try
	{
		VGTRecorder::get().finishGame(gameHandler);
	}
	catch(const std::exception & error)
	{
		logGlobal->error("Unable to finish VGT recording: %s", error.what());
	}
}

void onBeforeGameSave(CGameHandler & gameHandler) noexcept
{
	try
	{
		VGTRecorder::get().prepareSave(gameHandler);
	}
	catch(const std::exception & error)
	{
		logGlobal->error("Unable to prepare a VGT savepoint: %s", error.what());
	}
}

void onAfterGameSave(CGameHandler & gameHandler, const std::string & savePath) noexcept
{
	try
	{
		VGTRecorder::get().recordSave(gameHandler, savePath);
	}
	catch(const std::exception & error)
	{
		logGlobal->error("Unable to write a VGT savepoint: %s", error.what());
	}
}

void onDecision(CGameHandler & gameHandler, CPackForServer & pack)
{
	VGTRecorder::get().recordDecision(gameHandler, pack);
}

void onEffectBeforeApply(CGameHandler & gameHandler, CPackForClient & pack)
{
	VGTRecorder::get().recordEffect(gameHandler, pack);
}

void onEffectAfterApply(CGameHandler & gameHandler)
{
	VGTRecorder::get().recordAppliedState(gameHandler);
}

void onAutomaticEndTurn(const CGameState & gameState, const PlayerColor & player)
{
	VGTRecorder::get().recordTimerEndTurn(gameState, player);
}

void onAutomaticBattleAction(
	const CGameState & gameState,
	const PlayerColor & player,
	const BattleID & battleID,
	const BattleAction & action)
{
	VGTRecorder::get().recordTimerBattleAction(gameState, player, battleID, action);
}

}
