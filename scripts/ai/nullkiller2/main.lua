-- Lua Nullkiller2 entry point.
-- Mirrors the adventure-AI handoff into AI/Nullkiller2/Engine/Nullkiller.cpp.

local Nullkiller = require("Engine.Nullkiller")

local Script = {}

function Script.runDay(ai, input)
	return Nullkiller.makeTurn(ai, input or {})
end

return Script
