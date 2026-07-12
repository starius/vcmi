local Script = require("main")

local events = {}

local ai = {
	trace = function(_, event, data)
		table.insert(events, { event = event, data = data or {} })
	end,
	endTurn = function(_)
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

for _, item in ipairs(events) do
	if item.event == "Nullkiller.makeTurn.start" then
		print(string.format(
			"event=%s maxPass=%s maxPriorityPass=%s",
			item.event,
			tostring(item.data.maxPass),
			tostring(item.data.maxPriorityPass)))
	elseif item.event == "Nullkiller.makeTurn.end" then
		print(string.format(
			"event=%s status=%s actionOk=%s",
			item.event,
			tostring(item.data.status),
			tostring(item.data.actionResult and item.data.actionResult.ok)))
	end
end

print("output.status=" .. tostring(result.status))
print("output.intent=" .. tostring(result.intent))
