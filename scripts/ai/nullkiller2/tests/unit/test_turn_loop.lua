local Script = require("main")

local function makeAI()
	local events = {}
	local ended = false

	return {
		ai = {
			trace = function(_, event, data)
				table.insert(events, { event = event, data = data })
			end,
			endTurn = function(_)
				ended = true
				return { ok = true }
			end
		},
		events = events,
		ended = function()
			return ended
		end
	}
end

local emptyRun = makeAI()
local result = Script.runDay(emptyRun.ai, {
	settings = {
		values = {
			maxPass = 1,
			maxPriorityPass = 1
		}
	},
	memory = {
		version = 1
	}
})

assert(emptyRun.ended() == true)
assert(result.status == "end_turn")
assert(result.memory.version == 1)
assert(#result.commandJournal == 1)
assert(result.commandJournal[1].name == "endTurn")
assert(result.trace.implemented == "turn_loop")
assert(emptyRun.events[1].event == "Nullkiller.makeTurn.start")
assert(emptyRun.events[#emptyRun.events].event == "Nullkiller.makeTurn.end")

local recruitRun = makeAI()
local town = {
	id = 10,
	name = "Town",
	canRecruitHero = true,
	townLevel = 1,
	factionID = 1
}
local heroToBuy = {
	id = 20,
	name = "Hero",
	totalStrength = 5000,
	armyCost = 3000,
	evaluateHeroScore = 100,
	factionID = 1
}
town.availableHeroes = { heroToBuy }

local recruitResult = Script.runDay(recruitRun.ai, {
	settings = {
		values = {
			maxPass = 1,
			maxPriorityPass = 1
		}
	},
	townsInfo = { town },
	heroesInfo = {},
	freeResources = { [6] = 2500 }
})

assert(recruitRun.ended() == true)
assert(recruitResult.status == "end_turn")
assert(#recruitResult.commandJournal == 2)
assert(recruitResult.commandJournal[1].name == "recruitHero")
assert(recruitResult.commandJournal[1].payload.town == 10)
assert(recruitResult.commandJournal[1].payload.hero == 20)
assert(recruitResult.commandJournal[2].name == "endTurn")

local artifactRun = makeAI()
local artifactHero = {
	id = 30,
	name = "Artifact Hero",
	owner = 1,
	role = 1,
	totalStrength = 5000,
	armyStrength = 5000,
	movementPointsRemaining = 1000,
	visitablePos = { x = 0, y = 0, z = 0 },
	artifactsInBackpack = {
		{
			slot = 19,
			artifact = {
				id = 701,
				instanceID = 701,
				possibleSlots = { 0 },
				artifactType = {
					ID = "speed-boots",
					exportedBonuses = {
						{
							type = "MOVEMENT",
							subtype = "heroMovementLand",
							val = 50
						}
					},
					constituents = {}
				}
			}
		}
	}
}
local resource = {
	id = 31,
	ID = "RESOURCE",
	resource = "gold",
	visitablePos = { x = 1, y = 0, z = 0 },
	paths = {
		{
			targetHero = artifactHero,
			targetTile = { x = 1, y = 0, z = 0 },
			movementCost = 0.5,
			totalDanger = 0,
			turn = 0,
			exchangeCount = 0,
			heroArmy = { armyStrength = 5000 },
			nodes = {}
		}
	}
}
local artifactResult = Script.runDay(artifactRun.ai, {
	playerID = 1,
	settings = {
		values = {
			maxPass = 1,
			maxPriorityPass = 1
		}
	},
	heroesInfo = { artifactHero },
	nearbyObjects = { resource },
	freeResources = { [6] = 0 }
})

assert(artifactRun.ended() == true)
assert(artifactResult.status == "end_turn")
assert(artifactResult.commandJournal[1].name == "setActive")
assert(artifactResult.commandJournal[2].name == "setTargetObject")
assert(artifactResult.commandJournal[3].name == "resetObjectClusterizer")
assert(artifactResult.commandJournal[4].name == "setActive")
assert(artifactResult.commandJournal[5].name == "moveHeroToTile")
assert(artifactResult.commandJournal[6].name == "swapArtifacts")
assert(artifactResult.commandJournal[6].payload.srcHero == 30)
assert(artifactResult.commandJournal[6].payload.srcSlot == 19)
assert(artifactResult.commandJournal[6].payload.dstHero == 30)
assert(artifactResult.commandJournal[6].payload.dstSlot == 0)
assert(artifactResult.commandJournal[7].name == "endTurn")

local function movementArtifact(id, movementBonus)
	return {
		id = id,
		instanceID = id,
		possibleSlots = { 0 },
		artifactType = {
			ID = "artifact-" .. tostring(id),
			exportedBonuses = {
				{
					type = "MOVEMENT",
					subtype = "heroMovementLand",
					val = movementBonus
				}
			},
			constituents = {}
		}
	}
end

local exchangeRun = makeAI()
local firstExchangeHero = {
	id = 40,
	owner = 1,
	tempOwner = 1,
	stacksCount = 0,
	armySize = 7,
	slots = {},
	artifactsInBackpack = {
		{ slot = 19, artifact = movementArtifact(801, 50) }
	}
}
local secondExchangeHero = {
	id = 41,
	owner = 1,
	tempOwner = 1,
	stacksCount = 0,
	armySize = 7,
	slots = {},
	artifactsWorn = {},
	artifactsInBackpack = {}
}
local exchangeResult = Script.heroExchangeStarted(exchangeRun.ai, {
	firstHero = firstExchangeHero,
	secondHero = secondExchangeHero,
	activeHeroID = 40,
	queryID = 900
})
assert(exchangeRun.ended() == false)
assert(exchangeResult.status == "answered")
assert(#exchangeResult.commandJournal == 2)
assert(exchangeResult.commandJournal[1].name == "swapArtifacts")
assert(exchangeResult.commandJournal[1].payload.srcHero == 40)
assert(exchangeResult.commandJournal[1].payload.srcSlot == 19)
assert(exchangeResult.commandJournal[1].payload.dstHero == 41)
assert(exchangeResult.commandJournal[1].payload.dstSlot == 0)
assert(exchangeResult.commandJournal[2].name == "answerQuery")
assert(exchangeResult.commandJournal[2].payload.query == 900)
assert(exchangeResult.commandJournal[2].payload.selection == 0)

local allyVisitRun = makeAI()
local allyVisitResult = Script.heroExchangeStarted(allyVisitRun.ai, {
	firstHero = {
		id = 42,
		owner = 1,
		tempOwner = 1,
		artifactsInBackpack = {
			{ slot = 19, artifact = movementArtifact(802, 50) }
		}
	},
	secondHero = {
		id = 43,
		owner = 2,
		tempOwner = 2,
		artifactsWorn = {},
		artifactsInBackpack = {}
	},
	activeHeroID = 42,
	queryID = 901
})
assert(allyVisitRun.ended() == false)
assert(allyVisitResult.status == "answered")
assert(#allyVisitResult.commandJournal == 1)
assert(allyVisitResult.commandJournal[1].name == "answerQuery")
assert(allyVisitResult.commandJournal[1].payload.query == 901)

local mapObjectRun = makeAI()
local mapObjectResult = Script.showMapObjectSelectDialog(mapObjectRun.ai, {
	queryID = 902,
	selectedObject = { id = 52 },
	objects = {
		{ id = 51 },
		{ id = 52 }
	}
})
assert(mapObjectRun.ended() == false)
assert(mapObjectResult.status == "answered")
assert(mapObjectResult.selection == 52)
assert(#mapObjectResult.commandJournal == 1)
assert(mapObjectResult.commandJournal[1].name == "answerQuery")
assert(mapObjectResult.commandJournal[1].payload.query == 902)
assert(mapObjectResult.commandJournal[1].payload.selection == 52)

local garrisonRun = makeAI()
local garrisonResult = Script.showGarrisonDialog(garrisonRun.ai, {
	queryID = 903,
	removableUnits = true,
	restrictedGarrisonsForAI = false,
	up = {
		id = 60,
		owner = 1,
		tempOwner = 1,
		stacksCount = 1,
		armySize = 7,
		slots = {
			{
				slot = 0,
				creature = { id = 61, factionID = 1, level = 2, movementRange = 5 },
				count = 10,
				power = 100
			}
		}
	},
	down = {
		id = 62,
		owner = 1,
		tempOwner = 1,
		stacksCount = 0,
		armySize = 7,
		slots = {}
	},
	bestArmy = {
		{ creature = { id = 61 } }
	}
})
assert(garrisonRun.ended() == false)
assert(garrisonResult.status == "answered")
assert(garrisonResult.moved == true)
assert(#garrisonResult.commandJournal == 2)
assert(garrisonResult.commandJournal[1].name == "mergeOrSwapStacks")
assert(garrisonResult.commandJournal[1].payload.src == 60)
assert(garrisonResult.commandJournal[1].payload.dst == 62)
assert(garrisonResult.commandJournal[1].payload.fromSlot == 0)
assert(garrisonResult.commandJournal[1].payload.toSlot == 0)
assert(garrisonResult.commandJournal[2].name == "answerQuery")
assert(garrisonResult.commandJournal[2].payload.query == 903)
assert(garrisonResult.commandJournal[2].payload.selection == 0)

local restrictedGarrisonRun = makeAI()
local restrictedGarrisonResult = Script.showGarrisonDialog(restrictedGarrisonRun.ai, {
	queryID = 904,
	removableUnits = true,
	restrictedGarrisonsForAI = true,
	up = { id = 63, owner = 1, tempOwner = 1, stacksCount = 1 },
	down = { id = 64, owner = 1, tempOwner = 1, stacksCount = 0 }
})
assert(restrictedGarrisonResult.status == "answered")
assert(restrictedGarrisonResult.moved == false)
assert(#restrictedGarrisonResult.commandJournal == 1)
assert(restrictedGarrisonResult.commandJournal[1].name == "answerQuery")
assert(restrictedGarrisonResult.commandJournal[1].payload.query == 904)

local retreatResult = Script.makeSurrenderRetreatDecision(makeAI().ai, {
	townsCount = 1,
	settings = {
		values = {
			retreatThresholdAbsolute = 5000,
			retreatThresholdRelative = 0.5
		}
	},
	battleState = {
		ourStrength = 1000,
		enemyStrength = 4000,
		canFlee = true,
		ourSide = 1
	}
})
assert(retreatResult.status == "retreat")
assert(retreatResult.side == 1)

local patrolRetreatResult = Script.makeSurrenderRetreatDecision(makeAI().ai, {
	townsCount = 1,
	settings = {
		values = {
			retreatThresholdAbsolute = 5000,
			retreatThresholdRelative = 0.5
		}
	},
	battleState = {
		ourHero = { patrol = { patrolling = true } },
		ourStrength = 1000,
		enemyStrength = 4000,
		canFlee = true,
		ourSide = 1
	}
})
assert(patrolRetreatResult.status == "none")

local blockingAnswerRun = makeAI()
local blockingAnswerResult = Script.showBlockingDialog(blockingAnswerRun.ai, {
	queryID = 905,
	selection = false,
	cancel = true
})
assert(blockingAnswerResult.status == "answered")
assert(blockingAnswerResult.selection == 1)
assert(#blockingAnswerResult.commandJournal == 1)
assert(blockingAnswerResult.commandJournal[1].name == "answerQuery")
assert(blockingAnswerResult.commandJournal[1].payload.query == 905)
assert(blockingAnswerResult.commandJournal[1].payload.selection == 1)

local blockingSelectionRun = makeAI()
local blockingSelectionResult = Script.showBlockingDialog(blockingSelectionRun.ai, {
	queryID = 906,
	selection = true,
	cancel = false,
	hero = { id = 70, verified = true },
	components = {
		{ type = "RESOURCE" },
		{ type = "EXPERIENCE" }
	}
})
assert(blockingSelectionResult.status == "answered")
assert(blockingSelectionResult.selection == 1)
assert(#blockingSelectionResult.commandJournal == 1)
assert(blockingSelectionResult.commandJournal[1].name == "answerQuery")
assert(blockingSelectionResult.commandJournal[1].payload.query == 906)
assert(blockingSelectionResult.commandJournal[1].payload.selection == 1)

local teleportRun = makeAI()
local teleportResult = Script.showTeleportDialog(teleportRun.ai, {
	queryID = 907,
	channel = 12,
	exits = {
		{ id = 80, pos = { x = 1, y = 1, z = 0 }, visible = true },
		{ id = 81, pos = { x = 2, y = 2, z = 0 }, visible = true }
	},
	destinationTeleport = 81,
	destinationTeleportPos = { x = 2, y = 2, z = 0 }
})
assert(teleportResult.status == "answered")
assert(teleportResult.selection == 1)
assert(#teleportResult.commandJournal == 1)
assert(teleportResult.commandJournal[1].name == "answerQuery")
assert(teleportResult.commandJournal[1].payload.query == 907)
assert(teleportResult.commandJournal[1].payload.selection == 1)

for index, functionName in ipairs({
	"commanderGotLevel",
	"showTavernWindow",
	"showMarketWindow",
	"showUniversityWindow"
}) do
	local simpleQueryRun = makeAI()
	local simpleQueryResult = Script[functionName](simpleQueryRun.ai, {
		queryID = 910 + index
	})
	assert(simpleQueryResult.status == "answered")
	assert(simpleQueryResult.selection == 0)
	assert(#simpleQueryResult.commandJournal == 1)
	assert(simpleQueryResult.commandJournal[1].name == "answerQuery")
	assert(simpleQueryResult.commandJournal[1].payload.query == 910 + index)
	assert(simpleQueryResult.commandJournal[1].payload.selection == 0)
end

local levelRun = makeAI()
local levelResult = Script.heroGotLevel(levelRun.ai, {
	queryID = 920,
	hero = {
		level = 12,
		role = 1,
		secSkills = {}
	},
	skills = {
		7,
		2
	}
})
assert(levelResult.status == "answered")
assert(levelResult.selection == 0)
assert(#levelResult.commandJournal == 1)
assert(levelResult.commandJournal[1].name == "answerQuery")
assert(levelResult.commandJournal[1].payload.query == 920)
assert(levelResult.commandJournal[1].payload.selection == 0)

local recruitmentRun = makeAI()
local recruitmentDialogResult = Script.showRecruitmentDialog(recruitmentRun.ai, {
	queryID = 930,
	dwelling = {
		id = 100,
		creatures = {
			{
				count = 5,
				creatures = {
					{ id = 401, fullRecruitCost = { [7] = 100 } },
					{ id = 402, fullRecruitCost = { [7] = 120 } }
				}
			}
		}
	},
	dst = {
		id = 101,
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
	},
	freeResources = {
		[7] = 500
	}
})
assert(recruitmentDialogResult.status == "answered")
assert(#recruitmentDialogResult.commandJournal == 3)
assert(recruitmentDialogResult.commandJournal[1].name == "mergeStacks")
assert(recruitmentDialogResult.commandJournal[1].payload.army == 101)
assert(recruitmentDialogResult.commandJournal[1].payload.fromSlot == 1)
assert(recruitmentDialogResult.commandJournal[1].payload.toSlot == 0)
assert(recruitmentDialogResult.commandJournal[2].name == "recruitCreatures")
assert(recruitmentDialogResult.commandJournal[2].payload.town == 100)
assert(recruitmentDialogResult.commandJournal[2].payload.dst == 101)
assert(recruitmentDialogResult.commandJournal[2].payload.creature == 402)
assert(recruitmentDialogResult.commandJournal[2].payload.count == 4)
assert(recruitmentDialogResult.commandJournal[2].payload.level == 0)
assert(recruitmentDialogResult.commandJournal[3].name == "answerQuery")
assert(recruitmentDialogResult.commandJournal[3].payload.query == 930)
assert(recruitmentDialogResult.commandJournal[3].payload.selection == 0)
