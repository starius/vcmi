local Script = require("main")

local events = {}
local ended = false

local ai = {
	trace = function(_, event, data)
		table.insert(events, { event = event, data = data })
	end,
	endTurn = function(_)
		ended = true
		return { ok = true }
	end
}

local result = Script.runDay(ai, {
	settings = {
		values = {
			maxPass = 3,
			maxPriorityPass = 2
		}
	},
	memory = {
		version = 1
	}
})

assert(ended == true)
assert(result.status == "end_turn")
assert(result.memory.version == 1)
assert(#result.commandJournal == 1)
assert(result.commandJournal[1].name == "endTurn")
assert(#events == 2)
assert(events[1].event == "Nullkiller.makeTurn.start")
assert(events[1].data.maxPass == 3)
assert(events[2].event == "Nullkiller.makeTurn.end")
