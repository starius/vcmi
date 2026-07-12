local HostCommands = require("Actions.HostCommands")

local called = {}
local host = {
	command = function(_, name, payload)
		table.insert(called, { name = name, payload = payload })
		return { ok = true, executed = false }
	end
}

local adapter = HostCommands.new(host)
local lockedHeroes = {}
local targetObject = nil
adapter.nullkiller = {
	lockHero = function(_, hero, reason)
		lockedHeroes[hero.id] = reason
	end,
	unlockHero = function(_, hero)
		lockedHeroes[hero.id] = nil
	end,
	setTargetObject = function(_, objid)
		targetObject = objid
	end
}

local recruit = adapter:recruitHero({ id = 11 }, { id = 22 })
assert(recruit.ok == true)
assert(recruit.executed == false)

adapter:buildBuilding({ objectID = 33 }, { num = 44 })
adapter:castSpell({ id = 55 }, { id = 66 }, { x = 1, y = 2, z = 0 })
adapter:executeHeroChain({ targetHero = { id = 77 }, tile = { x = 3, y = 4, z = 0 } }, 88)
adapter:recruitCreatures({ id = 91 }, { id = 92 }, { id = 93 }, 7, 2)
adapter:dismissCreature({ id = 94 }, 5)
adapter:lockHero({ id = 95 }, 3)
assert(lockedHeroes[95] == 3)
adapter:unlockHero({ id = 95 })
adapter:setTargetObject({ id = 96 })
adapter:endTurn()

local journal = adapter:getJournal()
assert(#journal == 7)
assert(journal[1].name == "recruitHero")
assert(journal[1].payload.town == 11)
assert(journal[1].payload.hero == 22)
assert(journal[2].name == "buildBuilding")
assert(journal[2].payload.town == 33)
assert(journal[2].payload.bid == 44)
assert(journal[3].name == "castSpell")
assert(journal[3].payload.hero == 55)
assert(journal[3].payload.spell == 66)
assert(journal[3].payload.x == 1)
assert(journal[4].name == "executeHeroChain")
assert(journal[4].payload.hero == 77)
assert(journal[4].payload.objid == 88)
assert(journal[4].payload.x == 3)
assert(journal[5].name == "recruitCreatures")
assert(journal[5].payload.town == 91)
assert(journal[5].payload.dst == 92)
assert(journal[5].payload.creature == 93)
assert(journal[5].payload.count == 7)
assert(journal[5].payload.level == 2)
assert(journal[6].name == "dismissCreature")
assert(journal[6].payload.army == 94)
assert(journal[6].payload.slot == 5)
assert(journal[7].name == "endTurn")
assert(lockedHeroes[95] == nil)
assert(targetObject == 96)

assert(#called == 7)
assert(called[1].name == "recruitHero")
