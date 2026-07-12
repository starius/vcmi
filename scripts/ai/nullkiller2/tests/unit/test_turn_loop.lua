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
