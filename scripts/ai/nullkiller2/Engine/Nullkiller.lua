-- Mirrors AI/Nullkiller2/Engine/Nullkiller.cpp: Nullkiller::makeTurn.

local HostCommands = require("Actions.HostCommands")
local Settings = require("Engine.Settings")
local State = require("Engine.State")
local TaskPlan = require("Engine.TaskPlan")

local Nullkiller = {}

local function trace(ai, event, data)
	if ai and ai.trace then
		ai:trace(event, data or {})
	end
end

local function endTurn(ai)
	if ai and ai.endTurn then
		return ai:endTurn()
	end

	return { ok = true, skippedHostEndTurn = true }
end

local function loadSettings(input)
	local settingsInput = input.settings or {}

	if settingsInput.root and settingsInput.difficultyName then
		return Settings.fromDifficultyConfig(settingsInput.root, settingsInput.difficultyName)
	end

	return Settings.withDefaults(settingsInput.values)
end

function Nullkiller.chooseTaskFailureAction(hasAnySuccess, hasRemainingTasks, canReplan)
	return TaskPlan.chooseTaskFailureAction(hasAnySuccess, hasRemainingTasks, canReplan)
end

function Nullkiller.updateStateAndExecutePriorityPass(ai, state, settings, passIndex)
	trace(ai, "Nullkiller.updateStateAndExecutePriorityPass", {
		passIndex = passIndex,
		maxPriorityPass = settings.maxPriorityPass
	})

	return true
end

function Nullkiller.makeTurn(ai, input)
	local host = HostCommands.new(ai)
	local settings = loadSettings(input or {})
	local state = State.new()

	State.resetState(state)
	trace(host, "Nullkiller.makeTurn.start", {
		maxPass = settings.maxPass,
		maxPriorityPass = settings.maxPriorityPass
	})

	local actionResult = endTurn(host)

	trace(host, "Nullkiller.makeTurn.end", {
		status = "end_turn",
		actionResult = actionResult
	})

	return {
		status = "end_turn",
		intent = "lua-nullkiller2 skeleton ended turn without native fallback",
		memory = input.memory or {},
		commandJournal = host:getJournal(),
		trace = {
			implemented = "turn_loop_skeleton"
		}
	}
end

return Nullkiller
