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

function Script.playerBlocked(ai, input)
	return Nullkiller.playerBlocked(ai, input or {})
end

function Script.availableCreaturesChanged(ai, input)
	return Nullkiller.availableCreaturesChanged(ai, input or {})
end

function Script.heroInGarrisonChange(ai, input)
	return Nullkiller.heroInGarrisonChange(ai, input or {})
end

function Script.artifactMoved(ai, input)
	return Nullkiller.artifactMoved(ai, input or {})
end

function Script.artifactAssembled(ai, input)
	return Nullkiller.artifactAssembled(ai, input or {})
end

function Script.artifactPut(ai, input)
	return Nullkiller.artifactPut(ai, input or {})
end

function Script.artifactRemoved(ai, input)
	return Nullkiller.artifactRemoved(ai, input or {})
end

function Script.artifactDisassembled(ai, input)
	return Nullkiller.artifactDisassembled(ai, input or {})
end

function Script.availableArtifactsChanged(ai, input)
	return Nullkiller.availableArtifactsChanged(ai, input or {})
end

function Script.heroVisitsTown(ai, input)
	return Nullkiller.heroVisitsTown(ai, input or {})
end

function Script.heroExperienceChanged(ai, input)
	return Nullkiller.heroExperienceChanged(ai, input or {})
end

function Script.heroPrimarySkillChanged(ai, input)
	return Nullkiller.heroPrimarySkillChanged(ai, input or {})
end

function Script.heroMovePointsChanged(ai, input)
	return Nullkiller.heroMovePointsChanged(ai, input or {})
end

function Script.garrisonsChanged(ai, input)
	return Nullkiller.garrisonsChanged(ai, input or {})
end

function Script.playerBonusChanged(ai, input)
	return Nullkiller.playerBonusChanged(ai, input or {})
end

function Script.advmapSpellCast(ai, input)
	return Nullkiller.advmapSpellCast(ai, input or {})
end

function Script.requestRealized(ai, input)
	return Nullkiller.requestRealized(ai, input or {})
end

function Script.receivedResource(ai, input)
	return Nullkiller.receivedResource(ai, input or {})
end

function Script.heroManaPointsChanged(ai, input)
	return Nullkiller.heroManaPointsChanged(ai, input or {})
end

function Script.heroSecondarySkillChanged(ai, input)
	return Nullkiller.heroSecondarySkillChanged(ai, input or {})
end

function Script.beforeObjectPropertyChanged(ai, input)
	return Nullkiller.beforeObjectPropertyChanged(ai, input or {})
end

function Script.buildChanged(ai, input)
	return Nullkiller.buildChanged(ai, input or {})
end

function Script.heroBonusChanged(ai, input)
	return Nullkiller.heroBonusChanged(ai, input or {})
end

function Script.heroCreated(ai, input)
	return Nullkiller.heroCreated(ai, input or {})
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

function Script.battleStart(ai, input)
	return Nullkiller.battleStart(ai, input or {})
end

function Script.battleEnd(ai, input)
	return Nullkiller.battleEnd(ai, input or {})
end

function Script.battleResultsApplied(ai, input)
	return Nullkiller.battleResultsApplied(ai, input or {})
end

function Script.battleEnded(ai, input)
	return Nullkiller.battleEnded(ai, input or {})
end

function Script.heroGotLevel(ai, input)
	return Nullkiller.heroGotLevel(ai, input or {})
end

function Script.showTavernWindow(ai, input)
	return Nullkiller.showTavernWindow(ai, input or {})
end

function Script.showThievesGuildWindow(ai, input)
	return Nullkiller.showThievesGuildWindow(ai, input or {})
end

function Script.showShipyardDialog(ai, input)
	return Nullkiller.showShipyardDialog(ai, input or {})
end

function Script.showHillFortWindow(ai, input)
	return Nullkiller.showHillFortWindow(ai, input or {})
end

function Script.showInfoDialog(ai, input)
	return Nullkiller.showInfoDialog(ai, input or {})
end

function Script.showMarketWindow(ai, input)
	return Nullkiller.showMarketWindow(ai, input or {})
end

function Script.showUniversityWindow(ai, input)
	return Nullkiller.showUniversityWindow(ai, input or {})
end

return Script
