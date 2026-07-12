-- Mirrors AI/Nullkiller2/Engine/Settings.{h,cpp}: Settings.

local Settings = {}
Settings.__index = Settings

Settings.DEFAULTS = {
	maxRoamingHeroes = 8,
	maxRoamingHeroesPerTown = 0,
	mainHeroTurnDistanceLimit = 10,
	scoutHeroTurnDistanceLimit = 5,
	threatTurnDistanceLimit = 5,
	maxGoldPressure = 0.3,
	retreatThresholdRelative = 0.3,
	retreatThresholdAbsolute = 10000,
	safeAttackRatio = 1.1,
	maxPass = 10,
	maxPriorityPass = 10,
	pathfinderBucketsCount = 1,
	pathfinderBucketSize = 32,
	allowObjectGraph = true,
	useOneWayMonoliths = false,
	useTroopsFromGarrisons = false,
	updateHitmapOnTileReveal = false,
	openMap = true,
	maxArmyLossTarget = 0.35
}

Settings.REQUIRED_FIELDS = {
	"maxRoamingHeroes",
	"maxRoamingHeroesPerTown",
	"mainHeroTurnDistanceLimit",
	"scoutHeroTurnDistanceLimit",
	"threatTurnDistanceLimit",
	"maxGoldPressure",
	"retreatThresholdRelative",
	"retreatThresholdAbsolute",
	"safeAttackRatio",
	"maxPass",
	"maxPriorityPass",
	"pathfinderBucketsCount",
	"pathfinderBucketSize",
	"allowObjectGraph",
	"useOneWayMonoliths",
	"useTroopsFromGarrisons",
	"updateHitmapOnTileReveal",
	"openMap",
	"maxArmyLossTarget"
}

local function copyTable(source)
	local result = {}
	for key, value in pairs(source or {}) do
		result[key] = value
	end
	return result
end

function Settings.withDefaults(values)
	local result = copyTable(Settings.DEFAULTS)
	for key, value in pairs(values or {}) do
		result[key] = value
	end
	return setmetatable(result, Settings)
end

function Settings.fromDifficultyConfig(root, difficultyName)
	assert(type(root) == "table", "settings root must be a table")
	assert(type(difficultyName) == "string", "difficulty name must be a string")

	local node = root[difficultyName]
	assert(type(node) == "table", "missing settings for difficulty: " .. difficultyName)

	local result = Settings.withDefaults(node)
	for _, field in ipairs(Settings.REQUIRED_FIELDS) do
		assert(result[field] ~= nil, "missing settings field: " .. field)
	end

	result.difficultyName = difficultyName
	return result
end

function Settings:getMaxPass()
	return self.maxPass
end

function Settings:getMaxPriorityPass()
	return self.maxPriorityPass
end

function Settings:getMaxGoldPressure()
	return self.maxGoldPressure
end

function Settings:getRetreatThresholdRelative()
	return self.retreatThresholdRelative
end

function Settings:getRetreatThresholdAbsolute()
	return self.retreatThresholdAbsolute
end

function Settings:getSafeAttackRatio()
	return self.safeAttackRatio
end

function Settings:getMaxArmyLossTarget()
	return self.maxArmyLossTarget
end

function Settings:getMaxRoamingHeroes()
	return self.maxRoamingHeroes
end

function Settings:getMaxRoamingHeroesPerTown()
	return self.maxRoamingHeroesPerTown
end

function Settings:getMainHeroTurnDistanceLimit()
	return self.mainHeroTurnDistanceLimit
end

function Settings:getScoutHeroTurnDistanceLimit()
	return self.scoutHeroTurnDistanceLimit
end

function Settings:getThreatTurnDistanceLimit()
	return self.threatTurnDistanceLimit
end

function Settings:getPathfinderBucketsCount()
	return self.pathfinderBucketsCount
end

function Settings:getPathfinderBucketSize()
	return self.pathfinderBucketSize
end

function Settings:isObjectGraphAllowed()
	return self.allowObjectGraph
end

function Settings:isGarrisonTroopsUsageAllowed()
	return self.useTroopsFromGarrisons
end

function Settings:isOneWayMonolithUsageAllowed()
	return self.useOneWayMonoliths
end

function Settings:isUpdateHitmapOnTileReveal()
	return self.updateHitmapOnTileReveal
end

function Settings:isOpenMap()
	return self.openMap
end

return Settings
