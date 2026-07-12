local GatewayPolicy = require("Actions.GatewayPolicy")
local PriorityEvaluator = require("Engine.PriorityEvaluator")

local hero = {
	id = 10,
	verified = true,
	totalStrength = 1000,
	role = PriorityEvaluator.HeroRole.SCOUT
}

local resource = {
	id = 20,
	ID = GatewayPolicy.ObjectType.RESOURCE
}

assert(GatewayPolicy.chooseBlockingDialogAnswer({
	hero = hero,
	target = { x = 1, y = 2, z = 0 },
	objects = { hero, resource },
	goalObjectID = 20,
	danger = 500,
	settings = { safeAttackRatio = 1.1 }
}) == 1)

assert(GatewayPolicy.chooseBlockingDialogAnswer({
	hero = hero,
	target = { x = 1, y = 2, z = 0 },
	objects = { hero, resource },
	goalObjectID = 20,
	danger = 0,
	settings = { safeAttackRatio = 1.1 }
}) == 0)

assert(GatewayPolicy.chooseBlockingDialogAnswer({
	hero = hero,
	target = { x = 1, y = 2, z = 0 },
	objects = { hero, resource },
	goalObjectID = 999,
	objectDanger = { [20] = 100 },
	danger = 500,
	settings = { safeAttackRatio = 1.1 }
}) == 1)

assert(GatewayPolicy.chooseBlockingDialogAnswer({
	hero = hero,
	target = { x = 1, y = 2, z = 0 },
	objects = { hero, { id = 22, ID = "CRYPT" } },
	goalObjectID = 999,
	objectDanger = { [22] = 100 },
	danger = 500,
	settings = { safeAttackRatio = 1.1 }
}) == 0)

assert(GatewayPolicy.chooseBlockingDialogAnswer({
	hero = hero,
	target = { x = 1, y = 2, z = 0 },
	objects = { hero, { id = 21, ID = GatewayPolicy.ObjectType.QUEST_GUARD, danger = 100000 } },
	goalObjectID = 999,
	danger = 0
}) == 1)

assert(GatewayPolicy.chooseBlockingDialogSelection({
	selection = true,
	components = {
		{ type = GatewayPolicy.ComponentType.RESOURCE },
		{ type = "EXPERIENCE" }
	},
	hero = hero,
	goldPressureOverMax = false
}) == 1)

assert(GatewayPolicy.chooseBlockingDialogSelection({
	selection = true,
	components = {
		{ type = GatewayPolicy.ComponentType.RESOURCE },
		{ type = "EXPERIENCE" },
		{ type = "OTHER" }
	},
	hero = { id = 11, verified = true, role = PriorityEvaluator.HeroRole.MAIN },
	goldPressureOverMax = false
}) == 3)

local teleportChoice = GatewayPolicy.chooseTeleportExit({
	channel = 7,
	exits = {
		{ id = 101, pos = { x = 1, y = 1, z = 0 }, visible = true },
		{ id = 102, pos = { x = 2, y = 2, z = 0 }, visible = false }
	},
	destinationTeleport = 102,
	destinationTeleportPos = { x = 2, y = 2, z = 0 },
	teleportChannelProbingList = {}
})
assert(teleportChoice.selection == 1)
assert(#teleportChoice.teleportChannelProbingList == 0)

local probingChoice = GatewayPolicy.chooseTeleportExit({
	channel = 7,
	exits = {
		{ id = 101, pos = { x = 1, y = 1, z = 0 }, visible = false },
		{ id = 102, pos = { x = 2, y = 2, z = 0 }, visible = false }
	},
	destinationTeleport = 102,
	destinationTeleportPos = { x = -1, y = -1, z = -1 },
	teleportChannelProbingList = {}
})
assert(probingChoice.selection == -1)
assert(#probingChoice.teleportChannelProbingList == 1)
assert(probingChoice.teleportChannelProbingList[1] == 101)

local impassableChoice = GatewayPolicy.chooseTeleportExit({
	channel = 7,
	impassable = true,
	exits = {}
})
assert(impassableChoice.selection == -1)
assert(impassableChoice.passability == GatewayPolicy.TeleportPassability.IMPASSABLE)

local retreat = GatewayPolicy.makeSurrenderRetreatDecision({
	townsCount = 1,
	settings = {
		retreatThresholdAbsolute = 5000,
		retreatThresholdRelative = 0.5
	},
	battleState = {
		ourStrength = 1000,
		enemyStrength = 4000,
		canFlee = true,
		ourSide = 0
	}
})
assert(retreat.action == "retreat")
assert(retreat.side == 0)

assert(GatewayPolicy.makeSurrenderRetreatDecision({
	townsCount = 1,
	settings = {
		retreatThresholdAbsolute = 5000,
		retreatThresholdRelative = 0.5
	},
	battleState = {
		ourHero = { patrol = { patrolling = true } },
		ourStrength = 1000,
		enemyStrength = 4000,
		canFlee = true,
		ourSide = 0
	}
}) == nil)

assert(GatewayPolicy.shouldUseGarrisonTroops({
	up = { tempOwner = 1 },
	down = { tempOwner = 1 },
	removableUnits = true,
	settings = { garrisonTroopsUsageAllowed = true },
	restrictedGarrisonsForAI = false
}) == true)

assert(GatewayPolicy.shouldUseGarrisonTroops({
	up = { tempOwner = 1 },
	down = { tempOwner = 2 },
	removableUnits = true,
	settings = { garrisonTroopsUsageAllowed = true },
	restrictedGarrisonsForAI = false
}) == false)

assert(GatewayPolicy.chooseMapObjectSelection({
	selectedObject = 71
}) == 71)

assert(GatewayPolicy.chooseMapObjectSelection({
	selectedObject = { id = 72 }
}) == 72)

local firstHero = { id = 201, tempOwner = 1 }
local secondHero = { id = 202, tempOwner = 1 }
local exchange = GatewayPolicy.chooseHeroExchange({
	firstHero = firstHero,
	secondHero = secondHero,
	firstHeroActive = true
})
assert(exchange.destination == secondHero)
assert(exchange.source == firstHero)

assert(GatewayPolicy.chooseHeroExchange({
	firstHero = firstHero,
	secondHero = { id = 203, tempOwner = 2 },
	firstHeroActive = true
}) == nil)

local upgrade = GatewayPolicy.chooseUpgrade({
	availableUpgrades = {
		{ id = 301, aiValue = 20, cost = { gold = 100 } },
		{ id = 302, aiValue = 30, cost = { gold = 120 } }
	}
}, {
	count = 10,
	aiValue = 10
}, {
	gold = 2000
})
assert(upgrade.creature.id == 302)
assert(upgrade.cost.gold == 1200)

local vectorUpgrade = GatewayPolicy.chooseUpgrade({
	availableUpgrades = {
		{ id = 304, aiValue = 25, cost = { [7] = 25 } }
	}
}, {
	count = 4,
	creature = { id = 303, aiValue = 10 }
}, {
	[7] = 100
})
assert(vectorUpgrade.creature.id == 304)
assert(vectorUpgrade.cost[7] == 100)

assert(GatewayPolicy.chooseUpgrade({
	availableUpgrades = {
		{ id = 305, aiValue = 25, cost = { [7] = 25 } }
	}
}, {
	count = 4,
	creature = { id = 303, aiValue = 10 }
}, {
	[7] = 99
}) == nil)

assert(GatewayPolicy.chooseUpgrade({
	availableUpgrades = {
		{ id = 303, aiValue = 5, cost = { gold = 100 } }
	}
}, {
	count = 10,
	aiValue = 10
}, {
	gold = 2000
}) == nil)

local recruitment = GatewayPolicy.chooseDwellingRecruitment({
	creatures = {
		{ 5, { { id = 401, fullRecruitCost = 100 }, { id = 402, fullRecruitCost = 120 } } },
		{ 0, { { id = 403, fullRecruitCost = 50 } } }
	}
}, {
	slotsByCreature = { [402] = 0 }
}, {
	gold = 500
})
assert(#recruitment == 1)
assert(recruitment[1].level == 0)
assert(recruitment[1].creature.id == 402)
assert(recruitment[1].count == 4)

local mergedRecruitment = GatewayPolicy.chooseDwellingRecruitment({
	creatures = {
		{
			count = 5,
			creatures = {
				{ id = 404, fullRecruitCost = { [7] = 120 } }
			}
		}
	}
}, {
	armySize = 7,
	slots = {
		{ slot = 0, creature = { id = 501 } },
		{ slot = 1, creature = { id = 501 } },
		{ slot = 2, creature = { id = 502 } },
		{ slot = 3, creature = { id = 503 } },
		{ slot = 4, creature = { id = 504 } },
		{ slot = 5, creature = { id = 505 } },
		{ slot = 6, creature = { id = 506 } }
	}
}, {
	[7] = 500
})
assert(#mergedRecruitment == 1)
assert(mergedRecruitment[1].creature.id == 404)
assert(mergedRecruitment[1].count == 4)
assert(mergedRecruitment[1].merge.fromSlot == 1)
assert(mergedRecruitment[1].merge.toSlot == 0)

local function movementArtifact(id, movementBonus, options)
	options = options or {}
	local bonuses = movementBonus and {
		{
			type = "MOVEMENT",
			subtype = "heroMovementLand",
			val = movementBonus
		}
	} or {}
	return {
		id = id,
		possibleSlots = options.possibleSlots or { 0 },
		deniedSlots = options.deniedSlots,
		exportedBonuses = bonuses
	}
end

local function snapshotMovementArtifact(id, movementBonus)
	local artifact = movementArtifact(id, nil)
	artifact.instanceID = id
	artifact.artifactType = {
		ID = "artifact-" .. tostring(id),
		exportedBonuses = movementArtifact(id, movementBonus).exportedBonuses,
		constituents = {}
	}
	return artifact
end

local function artifactJournal()
	local journal = {}
	return journal, {
		swapArtifacts = function(_, sourceHero, sourceSlot, destinationHero, destinationSlot)
			table.insert(journal, {
				sourceHero = sourceHero,
				sourceSlot = sourceSlot,
				destinationHero = destinationHero,
				destinationSlot = destinationSlot
			})
		end
	}
end

local equipHero = {
	id = 501,
	artifactsInBackpack = {
		{ slot = 19, artifact = movementArtifact(601, 50) }
	}
}
local equipJournal, equipGateway = artifactJournal()
assert(GatewayPolicy.pickBestArtifacts(equipGateway, equipHero) == 1)
assert(#equipJournal == 1)
assert(equipJournal[1].sourceHero == equipHero)
assert(equipJournal[1].sourceSlot == 19)
assert(equipJournal[1].destinationHero == equipHero)
assert(equipJournal[1].destinationSlot == 0)
assert(equipHero.artifactsWorn[1].artifact.id == 601)

local deniedArtifact = movementArtifact(606, 50)
deniedArtifact.canBePutAt = { ["0"] = false }
local deniedHero = {
	id = 505,
	artifactsInBackpack = {
		{ slot = 19, artifact = deniedArtifact }
	}
}
local deniedJournal, deniedGateway = artifactJournal()
assert(GatewayPolicy.pickBestArtifacts(deniedGateway, deniedHero) == 0)
assert(#deniedJournal == 0)

local swapHero = {
	id = 502,
	artifactsWorn = {
		{ slot = 0, artifact = movementArtifact(602, 1) }
	},
	artifactsInBackpack = {
		{ slot = 19, artifact = snapshotMovementArtifact(603, 50) }
	}
}
local swapJournal, swapGateway = artifactJournal()
assert(GatewayPolicy.pickBestArtifacts(swapGateway, swapHero) == 1)
assert(#swapJournal == 1)
assert(swapJournal[1].sourceHero == swapHero)
assert(swapJournal[1].sourceSlot == 19)
assert(swapJournal[1].destinationHero == swapHero)
assert(swapJournal[1].destinationSlot == 0)
assert(swapHero.artifactsWorn[1].artifact.id == 603)

local fallbackHero = {
	id = 503,
	artifactsWorn = {
		{
			slot = 0,
			artifact = movementArtifact(604, 1, {
				possibleSlots = {},
				deniedSlots = { [1] = true }
			})
		}
	}
}
local fallbackOtherHero = {
	id = 504,
	artifactsWorn = {
		{ slot = 1, artifact = movementArtifact(605, 50) }
	}
}
local fallbackJournal, fallbackGateway = artifactJournal()
assert(GatewayPolicy.pickBestArtifacts(fallbackGateway, fallbackHero, fallbackOtherHero) == 2)
assert(#fallbackJournal == 2)
assert(fallbackJournal[1].sourceHero == fallbackHero)
assert(fallbackJournal[1].sourceSlot == 0)
assert(fallbackJournal[1].destinationHero == fallbackOtherHero)
assert(fallbackJournal[1].destinationSlot == 19)
assert(fallbackJournal[2].sourceHero == fallbackOtherHero)
assert(fallbackJournal[2].sourceSlot == 1)
assert(fallbackJournal[2].destinationHero == fallbackHero)
assert(fallbackJournal[2].destinationSlot == 0)
assert(fallbackHero.artifactsWorn[1].artifact.id == 605)
