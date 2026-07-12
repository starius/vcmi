-- Mirrors AI/Nullkiller2/Goals/AbstractGoal.{h,cpp}: AbstractGoal, ITask helpers, EGoals.

local AbstractGoal = {}
AbstractGoal.__index = AbstractGoal

AbstractGoal.EGoals = {
	INVALID = -1,
	WIN = 0,
	CONQUER = 1,
	BUILD = 2,
	EXPLORE = 3,
	GATHER_ARMY = 4,
	BOOST_HERO = 5,
	RECRUIT_HERO = 6,
	RECRUIT_HERO_BEHAVIOR = 7,
	BUILD_STRUCTURE = 8,
	COLLECT_RES = 9,
	GATHER_TROOPS = 10,
	CAPTURE_OBJECTS = 11,
	GET_ART_TYPE = 12,
	DEFENCE = 13,
	STARTUP = 14,
	DIG_AT_TILE = 15,
	BUY_ARMY = 16,
	TRADE = 17,
	BUILD_BOAT = 18,
	COMPLETE_QUEST = 19,
	ADVENTURE_SPELL_CAST = 20,
	EXECUTE_HERO_CHAIN = 21,
	EXCHANGE_SWAP_TOWN_HEROES = 22,
	DISMISS_HERO = 23,
	COMPOSITION = 24,
	CLUSTER_BEHAVIOR = 25,
	UNLOCK_CLUSTER = 26,
	HERO_EXCHANGE = 27,
	ARMY_UPGRADE = 28,
	DEFEND_TOWN = 29,
	CAPTURE_OBJECT = 30,
	SAVE_RESOURCES = 31,
	STAY_AT_TOWN_BEHAVIOR = 32,
	STAY_AT_TOWN = 33,
	EXPLORATION_BEHAVIOR = 34,
	ESCAPE_BEHAVIOR = 35,
	EXPLORATION_POINT = 36,
	EXPLORE_NEIGHBOUR_TILE = 37
}

AbstractGoal.LOW_PR = -1

local RESOURCE_JSON_KEYS = {
	[0] = "wood",
	[1] = "mercury",
	[2] = "ore",
	[3] = "sulfur",
	[4] = "crystal",
	[5] = "gems",
	[6] = "gold"
}

local function copyResourceVector(values)
	local result = {}
	for index = 1, 7 do
		result[index] = values and values[index] or 0
	end
	return result
end

local function numericID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.num or value.id or value.objectID or value.objectId or value[1]
	end
	return value
end

local function translatedName(value, fallback)
	if type(value) == "table" then
		if type(value.getNameTranslated) == "function" then
			local ok, result = pcall(value.getNameTranslated, value)
			if ok and result then
				return result
			end
		end
		return value.nameTranslated or value.translatedName or value.name or value.jsonKey or fallback
	end
	return fallback
end

local function resourceJsonKey(resID)
	if type(resID) == "table" then
		return resID.jsonKey or resID.key or resID.name or RESOURCE_JSON_KEYS[numericID(resID)] or tostring(numericID(resID))
	end
	return RESOURCE_JSON_KEYS[resID] or tostring(resID)
end

local function artifactName(aid)
	if type(aid) == "table" then
		return translatedName(aid, tostring(numericID(aid)))
	end
	return tostring(aid)
end

local function tileToString(tile)
	tile = tile or {}
	return string.format("(%s %s %s)", tostring(tile.x), tostring(tile.y), tostring(tile.z))
end

function AbstractGoal.new(goalType)
	return setmetatable({
		isAbstract = true,
		value = 0,
		goldCost = 0,
		buildingCost = copyResourceVector(),
		resID = -1,
		objid = -1,
		aid = -1,
		tile = { x = -1, y = -1, z = -1 },
		hero = nil,
		town = nil,
		bid = -1,
		goalType = goalType or AbstractGoal.EGoals.INVALID
	}, AbstractGoal)
end

function AbstractGoal:clone()
	return self
end

function AbstractGoal:decompose(_aiNk)
	return {}
end

function AbstractGoal:toString()
	if self.goalType == AbstractGoal.EGoals.COLLECT_RES then
		local desc = "COLLECT RESOURCE " .. resourceJsonKey(self.resID) .. " (" .. tostring(self.value) .. ")"
		if self.hero then
			desc = desc .. " (" .. translatedName(self.hero, tostring(numericID(self.hero))) .. ")"
		end
		return desc
	end

	if self.goalType == AbstractGoal.EGoals.TRADE then
		local desc = string.format(
			"TRADE %d of %s at objid %d",
			self.value,
			resourceJsonKey(self.resID),
			self.objid)
		if self.hero then
			desc = desc .. " (" .. translatedName(self.hero, tostring(numericID(self.hero))) .. ")"
		end
		return desc
	end

	if self.goalType == AbstractGoal.EGoals.GATHER_TROOPS then
		local desc = "GATHER TROOPS"
		if self.hero then
			desc = desc .. " (" .. translatedName(self.hero, tostring(numericID(self.hero))) .. ")"
		end
		return desc
	end

	if self.goalType == AbstractGoal.EGoals.GET_ART_TYPE then
		local desc = "GET ARTIFACT OF TYPE " .. artifactName(self.aid)
		if self.hero then
			desc = desc .. " (" .. translatedName(self.hero, tostring(numericID(self.hero))) .. ")"
		end
		return desc
	end

	if self.goalType == AbstractGoal.EGoals.DIG_AT_TILE then
		local desc = "DIG AT TILE " .. tileToString(self.tile)
		if self.hero then
			desc = desc .. " (" .. translatedName(self.hero, tostring(numericID(self.hero))) .. ")"
		end
		return desc
	end

	return tostring(self.goalType)
end

function AbstractGoal:invalid()
	return self.goalType == AbstractGoal.EGoals.INVALID
end

function AbstractGoal:equals(_other)
	return false
end

function AbstractGoal:isElementar()
	return false
end

function AbstractGoal:hasHash()
	return false
end

function AbstractGoal:getHash()
	return 0
end

function AbstractGoal:asTask()
	error("Abstract goal is not a task", 2)
end

local setterFields = {
	"isAbstract",
	"value",
	"goldCost",
	"buildingCost",
	"resID",
	"objid",
	"aid",
	"tile",
	"hero",
	"town",
	"bid"
}

for _, field in ipairs(setterFields) do
	AbstractGoal["set" .. field] = function(self, rhs)
		self[field] = rhs
		return self
	end
end

function AbstractGoal.sptr(tmp)
	assert(tmp and tmp.clone, "goal is required")
	return tmp:clone()
end

function AbstractGoal.taskptr(tmp)
	assert(tmp, "goal is required")
	if not tmp:isElementar() then
		error(tmp:toString() .. " is not elementar", 2)
	end

	return tmp:clone():asTask()
end

function AbstractGoal.subgoalEquals(lhs, rhs)
	if not lhs or not rhs then
		return false
	end
	return lhs:equals(rhs)
end

function AbstractGoal.cannotFulfillGoalException(message)
	return message
end

function AbstractGoal.goalFulfilledException(goal)
	return goal
end

AbstractGoal._helpers = {
	numericID = numericID,
	resourceJsonKey = resourceJsonKey,
	tileToString = tileToString,
	translatedName = translatedName
}

return AbstractGoal
