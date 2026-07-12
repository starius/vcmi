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
