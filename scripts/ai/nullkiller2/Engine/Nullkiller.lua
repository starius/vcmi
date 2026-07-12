-- Mirrors AI/Nullkiller2/Engine/Nullkiller.cpp: Nullkiller::makeTurn.

local Settings = require("Engine.Settings")
local State = require("Engine.State")

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
	if hasAnySuccess then
		return "REPLAN"
	end

	if hasRemainingTasks then
		return "TRY_NEXT_TASK"
	end

	if canReplan then
		return "REPLAN"
	end

	return "STOP_TURN"
end

function Nullkiller.updateStateAndExecutePriorityPass(ai, state, settings, passIndex)
	trace(ai, "Nullkiller.updateStateAndExecutePriorityPass", {
		passIndex = passIndex,
		maxPriorityPass = settings.maxPriorityPass
	})

	return true
end

function Nullkiller.makeTurn(ai, input)
	local settings = loadSettings(input or {})
	local state = State.new()

	State.resetState(state)
	trace(ai, "Nullkiller.makeTurn.start", {
		maxPass = settings.maxPass,
		maxPriorityPass = settings.maxPriorityPass
	})

	local actionResult = endTurn(ai)

	trace(ai, "Nullkiller.makeTurn.end", {
		status = "end_turn",
		actionResult = actionResult
	})

	return {
		status = "end_turn",
		intent = "lua-nullkiller2 skeleton ended turn without native fallback",
		memory = input.memory or {},
		trace = {
			implemented = "turn_loop_skeleton"
		}
	}
end

return Nullkiller
