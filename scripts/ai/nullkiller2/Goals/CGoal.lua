-- Mirrors AI/Nullkiller2/Goals/CGoal.h: CGoal<T> and ElementarGoal<T>.

local AbstractGoal = require("Goals.AbstractGoal")

local CGoal = {}

local CGoalBase = {}
CGoalBase.__index = CGoalBase
setmetatable(CGoalBase, { __index = AbstractGoal })

local ElementarGoalBase = {}
ElementarGoalBase.__index = ElementarGoalBase
setmetatable(ElementarGoalBase, { __index = CGoalBase })

local function copyValue(value, seen)
	if type(value) ~= "table" then
		return value
	end

	seen = seen or {}
	if seen[value] then
		return seen[value]
	end

	local result = {}
	seen[value] = result
	for key, item in pairs(value) do
		result[copyValue(key, seen)] = copyValue(item, seen)
	end

	return setmetatable(result, getmetatable(value))
end

local function objectID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.id or value.objectID or value.objectId or value.num or value[1]
	end
	return value
end

function CGoalBase:clone()
	return copyValue(self)
end

function CGoalBase:equals(other)
	if not other or self.goalType ~= other.goalType then
		return false
	end

	if type(self.equalsTyped) == "function" then
		return self:equalsTyped(other)
	end

	return false
end

function CGoalBase:decompose(aiNk)
	local single = nil
	if type(self.decomposeSingle) == "function" then
		single = self:decomposeSingle(aiNk)
	end

	if not single or single:invalid() then
		return {}
	end

	return { single }
end

function ElementarGoalBase:setpriority(priority)
	self.priority = priority
	return self
end

function ElementarGoalBase:isElementar()
	return true
end

function ElementarGoalBase:getHero()
	return self.hero
end

function ElementarGoalBase:getHeroExchangeCount()
	return 0
end

function ElementarGoalBase:isObjectAffected(id)
	local targetID = objectID(id)
	return (self.hero and objectID(self.hero) == targetID)
		or self.objid == targetID
		or (self.town and objectID(self.town) == targetID)
end

function ElementarGoalBase:getAffectedObjects()
	local result = {}

	if self.hero then
		table.insert(result, objectID(self.hero))
	end

	if self.objid ~= -1 then
		table.insert(result, self.objid)
	end

	if self.town then
		table.insert(result, objectID(self.town))
	end

	return result
end

function ElementarGoalBase:asTask()
	return self
end

function ElementarGoalBase:accept(_aiGw)
	error("Unhandled goal: " .. self:toString(), 2)
end

function CGoal.derive(className, goalType, options)
	local class = {}
	class.__index = class
	class.className = className
	class.goalTypeDefault = goalType

	local base = options and options.elementar and ElementarGoalBase or CGoalBase
	setmetatable(class, { __index = base })

	function class.new(...)
		local instance = AbstractGoal.new(goalType)
		setmetatable(instance, class)

		if options and options.elementar then
			instance.isAbstract = false
			instance.priority = 0
		end

		if type(class.init) == "function" then
			class.init(instance, ...)
		end

		return instance
	end

	return class
end

CGoal.CGoalBase = CGoalBase
CGoal.ElementarGoalBase = ElementarGoalBase
CGoal.objectID = objectID

return CGoal
