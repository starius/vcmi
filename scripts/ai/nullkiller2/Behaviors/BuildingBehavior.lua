-- Mirrors AI/Nullkiller2/Behaviors/BuildingBehavior.{h,cpp}: BuildingBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local BuildThis = require("Goals.BuildThis")
local CGoal = require("Goals.CGoal")
local Composition = require("Goals.Composition")
local SaveResources = require("Goals.SaveResources")

local BuildingBehavior = CGoal.derive("BuildingBehavior", AbstractGoal.EGoals.BUILD)

local BuildingID = {
	CITADEL = 8,
	CASTLE = 9
}

local FortLevel = {
	CASTLE = 3
}

local GOLD = 6
local RESOURCE_COUNT = 7

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
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

local function getResource(resources, resourceID)
	if not resources then
		return 0
	end
	if resources[0] == nil and resources[7] ~= nil then
		return resources[resourceID + 1] or 0
	end
	return resources[resourceID] or 0
end

local function fortLevel(town)
	local value = call(town, "fortLevel")
	if value ~= nil then
		return value
	end
	return town and (town.fortLevel or town.fortLevelValue) or 0
end

local function townThreats(aiNk, town)
	local threats = call(aiNk and aiNk.dangerHitMap, "getTownThreats", town)
	if threats ~= nil then
		return threats
	end
	return town and town.threats or {}
end

local function developmentInfo(aiNk)
	local buildAnalyzer = aiNk and aiNk.buildAnalyzer
	local result = call(buildAnalyzer, "getDevelopmentInfo")
	if result ~= nil then
		return result
	end
	return buildAnalyzer and buildAnalyzer.developmentInfos or aiNk and aiNk.developmentInfos or {}
end

local function isGoldPressureOverMax(aiNk)
	local buildAnalyzer = aiNk and aiNk.buildAnalyzer
	local result = call(buildAnalyzer, "isGoldPressureOverMax")
	if result ~= nil then
		return result
	end
	return aiNk and aiNk.goldPressureOverMax or buildAnalyzer and buildAnalyzer.goldPressureOverMax or false
end

local function lockedResources(aiNk)
	local result = call(aiNk, "getLockedResources")
	if result ~= nil then
		return result
	end
	return aiNk and aiNk.lockedResources or {}
end

local function canAffordLockedResources(locked, cost)
	local result = call(locked, "canAfford", cost)
	if result ~= nil then
		return result
	end

	if locked and type(locked.canAfford) == "boolean" then
		return locked.canAfford
	end

	for resourceID = 0, RESOURCE_COUNT - 1 do
		if getResource(locked, resourceID) < getResource(cost, resourceID) then
			return false
		end
	end
	return true
end

local function buildCost(buildingInfo)
	return buildingInfo and (buildingInfo.buildCost or buildingInfo.cost) or {}
end

local function isEmergencyDefenseBuilding(buildingInfo)
	local id = numericID(buildingInfo and buildingInfo.id)
	return id == BuildingID.CITADEL or id == BuildingID.CASTLE
end

local function readsDevelopmentResourceInputs(aiNk)
	local buildAnalyzer = aiNk and aiNk.buildAnalyzer
	call(buildAnalyzer, "getMissingResourcesNow")
	call(buildAnalyzer, "getMissingResourcesInTotal")
	call(aiNk, "getFreeResources")
	call(buildAnalyzer, "getDailyIncome")
end

function BuildingBehavior:init()
	self.goalType = AbstractGoal.EGoals.BUILD
end

function BuildingBehavior:toString()
	return "Build"
end

function BuildingBehavior:equalsTyped(_other)
	return true
end

function BuildingBehavior:decompose(aiNk)
	local tasks = {}

	readsDevelopmentResourceInputs(aiNk)

	local isGoldPressureLow = not isGoldPressureOverMax(aiNk)
	local locked = lockedResources(aiNk)

	for _, townDevelopmentInfo in ipairs(developmentInfo(aiNk)) do
		local emergencyDefense = false
		local closestThreat = math.huge

		for _, threat in ipairs(townThreats(aiNk, townDevelopmentInfo.town)) do
			closestThreat = math.min(closestThreat, threat.turn or math.huge)
		end

		if closestThreat <= 1 and fortLevel(townDevelopmentInfo.town) < FortLevel.CASTLE then
			for _, buildingInfo in ipairs(townDevelopmentInfo.toBuild or {}) do
				if not buildingInfo.isMissingResources and isEmergencyDefenseBuilding(buildingInfo) then
					table.insert(tasks, BuildThis.new(buildingInfo, townDevelopmentInfo))
					emergencyDefense = true
				end
			end
		end

		if not emergencyDefense then
			for _, buildingInfo in ipairs(townDevelopmentInfo.toBuild or {}) do
				if isGoldPressureLow or getResource(buildingInfo.dailyIncome, GOLD) > 0 then
					if buildingInfo.isMissingResources then
						if not canAffordLockedResources(locked, buildCost(buildingInfo)) then
							local composition = Composition.new()
							composition:addNext(BuildThis.new(buildingInfo, townDevelopmentInfo))
							composition:addNext(SaveResources.new(buildCost(buildingInfo)))
							table.insert(tasks, composition)
						end
					else
						table.insert(tasks, BuildThis.new(buildingInfo, townDevelopmentInfo))
					end
				end
			end
		end
	end

	return tasks
end

BuildingBehavior.BuildingID = BuildingID
BuildingBehavior.FortLevel = FortLevel

return BuildingBehavior
