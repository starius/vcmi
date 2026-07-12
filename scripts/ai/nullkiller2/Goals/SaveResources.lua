-- Mirrors AI/Nullkiller2/Goals/SaveResources.{h,cpp}: SaveResources.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local SaveResources = CGoal.derive("SaveResources", AbstractGoal.EGoals.SAVE_RESOURCES, { elementar = true })

function SaveResources:init(resources)
	self.resources = resources or {}
end

function SaveResources:equalsTyped(_other)
	return true
end

function SaveResources:accept(aiGw)
	if aiGw and aiGw.nullkiller and type(aiGw.nullkiller.lockResources) == "function" then
		aiGw.nullkiller:lockResources(self.resources)
		return { fulfilled = self }
	end

	if aiGw and type(aiGw.lockResources) == "function" then
		aiGw:lockResources(self.resources)
		return { fulfilled = self }
	end

	error("No resource lock target.", 2)
end

function SaveResources:toString()
	return "SaveResources " .. AbstractGoal._helpers.resourcesToString(self.resources)
end

return SaveResources
