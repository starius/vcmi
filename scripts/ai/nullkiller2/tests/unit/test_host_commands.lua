local HostCommands = require("Actions.HostCommands")

local called = {}
local host = {
	command = function(_, name, payload)
		table.insert(called, { name = name, payload = payload })
		return { ok = true, executed = false }
	end
}

local adapter = HostCommands.new(host)

local recruit = adapter:recruitHero({ id = 11 }, { id = 22 })
assert(recruit.ok == true)
assert(recruit.executed == false)

adapter:buildBuilding({ objectID = 33 }, { num = 44 })
adapter:castSpell({ id = 55 }, { id = 66 }, { x = 1, y = 2, z = 0 })
adapter:endTurn()

local journal = adapter:getJournal()
assert(#journal == 4)
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
assert(journal[4].name == "endTurn")

assert(#called == 4)
assert(called[1].name == "recruitHero")
