-- Mirrors AI/Nullkiller2/Goals/CaptureObject.{h,cpp}: CaptureObject identity and hash.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local CaptureObject = CGoal.derive("CaptureObject", AbstractGoal.EGoals.CAPTURE_OBJECT)

local function tileOf(obj)
	if not obj then
		return { x = -1, y = -1, z = -1 }
	end
	return obj.visitablePos or obj.tile or { x = -1, y = -1, z = -1 }
end

local function nameOf(obj)
	if not obj then
		return ""
	end
	return obj.typeName or obj.objectName or obj.name or tostring(CGoal.objectID(obj))
end

function CaptureObject:init(obj)
	self.objid = CGoal.objectID(obj)
	self.tile = tileOf(obj)
	self.name = nameOf(obj)
end

function CaptureObject:equalsTyped(other)
	return self.objid == other.objid
end

function CaptureObject:decompose(aiNk)
	if aiNk and type(aiNk.decomposeCaptureObject) == "function" then
		return aiNk:decomposeCaptureObject(self)
	end

	return {}
end

function CaptureObject:toString()
	return "Capture " .. self.name .. " at " .. AbstractGoal._helpers.tileToString(self.tile)
end

function CaptureObject:hasHash()
	return true
end

function CaptureObject:getHash()
	return self.objid
end

return CaptureObject
