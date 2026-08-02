/*
 * Integration.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <string>

class BattleAction;
class BattleID;
class CGameHandler;
class CGameState;
class PlayerColor;
struct CPackForClient;
struct CPackForServer;

namespace vgt
{

#if defined(VCMI_ENABLE_VGT)
void onGameInitialized(CGameHandler & gameHandler, int seed) noexcept;
void onGameLoaded(CGameHandler & gameHandler, const std::string & savePath) noexcept;
void onGameClosing(CGameHandler & gameHandler) noexcept;
void onBeforeGameSave(CGameHandler & gameHandler) noexcept;
void onAfterGameSave(CGameHandler & gameHandler, const std::string & savePath) noexcept;
void onDecision(CGameHandler & gameHandler, CPackForServer & pack);
void onEffectBeforeApply(CGameHandler & gameHandler, CPackForClient & pack);
void onEffectAfterApply(CGameHandler & gameHandler);
void onAutomaticEndTurn(const CGameState & gameState, const PlayerColor & player);
void onAutomaticBattleAction(
	const CGameState & gameState,
	const PlayerColor & player,
	const BattleID & battleID,
	const BattleAction & action);
#else
inline void onGameInitialized(CGameHandler &, int) noexcept
{
}

inline void onGameLoaded(CGameHandler &, const std::string &) noexcept
{
}

inline void onGameClosing(CGameHandler &) noexcept
{
}

inline void onBeforeGameSave(CGameHandler &) noexcept
{
}

inline void onAfterGameSave(CGameHandler &, const std::string &) noexcept
{
}

inline void onDecision(CGameHandler &, CPackForServer &)
{
}

inline void onEffectBeforeApply(CGameHandler &, CPackForClient &)
{
}

inline void onEffectAfterApply(CGameHandler &)
{
}

inline void onAutomaticEndTurn(const CGameState &, const PlayerColor &)
{
}

inline void onAutomaticBattleAction(
	const CGameState &,
	const PlayerColor &,
	const BattleID &,
	const BattleAction &)
{
}
#endif

}
