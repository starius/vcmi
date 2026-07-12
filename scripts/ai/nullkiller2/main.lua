-- Lua Nullkiller2 entry point.
-- Mirrors the adventure-AI handoff into AI/Nullkiller2/Engine/Nullkiller.cpp.

local Nullkiller = require("Engine.Nullkiller")

local Script = {}

function Script.runDay(ai, input)
	return Nullkiller.makeTurn(ai, input or {})
end

function Script.heroExchangeStarted(ai, input)
	return Nullkiller.heroExchangeStarted(ai, input or {})
end

function Script.showMapObjectSelectDialog(ai, input)
	return Nullkiller.showMapObjectSelectDialog(ai, input or {})
end

function Script.showGarrisonDialog(ai, input)
	return Nullkiller.showGarrisonDialog(ai, input or {})
end

function Script.showRecruitmentDialog(ai, input)
	return Nullkiller.showRecruitmentDialog(ai, input or {})
end

function Script.makeSurrenderRetreatDecision(ai, input)
	return Nullkiller.makeSurrenderRetreatDecision(ai, input or {})
end

function Script.showBlockingDialog(ai, input)
	return Nullkiller.showBlockingDialog(ai, input or {})
end

function Script.showTeleportDialog(ai, input)
	return Nullkiller.showTeleportDialog(ai, input or {})
end

function Script.commanderGotLevel(ai, input)
	return Nullkiller.commanderGotLevel(ai, input or {})
end

function Script.heroVisit(ai, input)
	return Nullkiller.heroVisit(ai, input or {})
end

function Script.heroMoved(ai, input)
	return Nullkiller.heroMoved(ai, input or {})
end

function Script.newObject(ai, input)
	return Nullkiller.newObject(ai, input or {})
end

function Script.objectRemoved(ai, input)
	return Nullkiller.objectRemoved(ai, input or {})
end

function Script.objectPropertyChanged(ai, input)
	return Nullkiller.objectPropertyChanged(ai, input or {})
end

function Script.tileHidden(ai, input)
	return Nullkiller.tileHidden(ai, input or {})
end

function Script.tileRevealed(ai, input)
	return Nullkiller.tileRevealed(ai, input or {})
end

function Script.heroGotLevel(ai, input)
	return Nullkiller.heroGotLevel(ai, input or {})
end

function Script.showTavernWindow(ai, input)
	return Nullkiller.showTavernWindow(ai, input or {})
end

function Script.showMarketWindow(ai, input)
	return Nullkiller.showMarketWindow(ai, input or {})
end

function Script.showUniversityWindow(ai, input)
	return Nullkiller.showUniversityWindow(ai, input or {})
end

return Script
