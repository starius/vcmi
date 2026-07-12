-- Mirrors AI/Nullkiller2/Behaviors/CaptureObjectsBehavior.{h,cpp}: CaptureObjectsBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")
local Composition = require("Goals.Composition")
local ExecuteHeroChain = require("Goals.ExecuteHeroChain")
local Invalid = require("Goals.Invalid")
local PriorityEvaluator = require("Engine.PriorityEvaluator")
local State = require("Engine.State")

local CaptureObjectsBehavior = CGoal.derive("CaptureObjectsBehavior", AbstractGoal.EGoals.CAPTURE_OBJECTS)

local Obj = {
	ARTIFACT = 5,
	BOAT = 8,
	BORDERGUARD = 9,
	KEYMASTER = 10,
	CREATURE_GENERATOR1 = 17,
	EYE_OF_MAGI = 27,
	HERO = 34,
	HILL_FORT = 35,
	LIBRARY_OF_ENLIGHTENMENT = 41,
	MONOLITH_ONE_WAY_ENTRANCE = 43,
	MONOLITH_ONE_WAY_EXIT = 44,
	MONOLITH_TWO_WAY = 45,
	SCHOOL_OF_MAGIC = 47,
	MAGIC_WELL = 49,
	PRISON = 62,
	SEER_HUT = 83,
	SIGN = 91,
	TAVERN = 95,
	TOWN = 98,
	TREE_OF_KNOWLEDGE = 102,
	WHIRLPOOL = 111,
	SCHOOL_OF_WAR = 107
}

local EGameResID = {
	GEMS = 5,
	GOLD = 6
}

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	return CGoal.objectID(value)
end

local function contains(values, value)
	for _, item in ipairs(values or {}) do
		if item == value then
			return true
		end
	end
	return false
end

local function vectorEquals(v1, v2)
	for _, value in ipairs(v1 or {}) do
		if contains(v2, value) then
			return true
		end
	end
	return false
end

local function objectType(object)
	return object and (object.ID or object.idType or object.type)
end

local function objectSubType(object)
	return object and (object.subID or object.subId or object.subType)
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile)
end

local function movementCost(path)
	local result = call(path, "movementCost")
	if result ~= nil then
		return result
	end
	return path and path.movementCost or math.huge
end

local function pathTurn(path)
	local result = call(path, "turn")
	if result ~= nil then
		return result
	end
	return path and (path.turn or path.turns) or 0
end

local function totalDanger(path)
	local result = call(path, "getTotalDanger")
	if result ~= nil then
		return result
	end
	return path and path.totalDanger or 0
end

local function firstBlockedAction(path)
	local result = call(path, "getFirstBlockedAction")
	if result ~= nil then
		return result
	end
	return path and path.firstBlockedAction
end

local function normalizedHeroStrength(hero)
	local result = call(hero, "getHeroStrength") or hero and (hero.heroStrength or hero.normalizedHeroStrength)
	if type(result) ~= "number" or result <= 0 or result ~= result or result == math.huge or result == -math.huge then
		return 1.0
	end
	return result
end

local function armyStrength(army)
	local result = call(army, "getArmyStrength")
	if result ~= nil then
		return result
	end
	return army and (army.armyStrength or army.totalStrength or army.strength) or 0
end

local function isSafeToVisit(hero, heroArmy, dangerStrength, safeAttackRatio)
	if not dangerStrength or dangerStrength == 0 then
		return true
	end
	return normalizedHeroStrength(hero) * armyStrength(heroArmy or hero) > dangerStrength * safeAttackRatio
end

local function safeAttackRatio(aiNk)
	return call(aiNk and aiNk.settings, "getSafeAttackRatio")
		or aiNk and aiNk.settings and aiNk.settings.safeAttackRatio
		or 1.1
end

local function heroRole(aiNk, hero)
	return call(aiNk and aiNk.heroManager, "getHeroRoleOrDefaultInefficient", hero)
		or hero and hero.role
		or PriorityEvaluator.HeroRole.SCOUT
end

local function pathHeroesLocked(aiNk, path)
	local result = call(aiNk, "arePathHeroesLocked", path)
	if result ~= nil then
		return result
	end

	local lockedHeroes = aiNk and aiNk.lockedHeroes or {}
	if lockedHeroes[objectID(path.targetHero)] == State.HeroLockedReason.STARTUP then
		return true
	end

	for _, node in ipairs(path and path.nodes or {}) do
		if lockedHeroes[objectID(node.targetHero)] ~= nil then
			return true
		end
	end

	return false
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

local function freeResources(aiNk)
	return call(aiNk, "getFreeResources") or aiNk and aiNk.freeResources or {}
end

local function freeGold(aiNk)
	local result = call(aiNk, "getFreeGold")
	if result ~= nil then
		return result
	end
	return getResource(freeResources(aiNk), EGameResID.GOLD)
end

local function relations(aiNk, object, hero)
	return object and object.relations
		or call(aiNk and aiNk.cc, "getPlayerRelations", object and object.tempOwner, hero and hero.tempOwner)
		or "NEUTRAL"
end

local function hasQuest(aiNk, object)
	if object and object.hasQuest ~= nil then
		return object.hasQuest
	end
	for _, quest in ipairs(call(aiNk and aiNk.cc, "getMyQuests") or aiNk and aiNk.myQuests or {}) do
		if quest.obj == objectID(object) then
			return true
		end
	end
	return false
end

local function wasVisited(object, hero)
	local result = call(object, "wasVisited", hero)
	if result ~= nil then
		return result
	end
	return object and object.wasVisited == true
end

local function rewardableHasNoRewards(object)
	if object and object.availableRewardsEmpty ~= nil then
		return object.availableRewardsEmpty
	end
	local rewards = object and object.availableRewards
	return rewards ~= nil and #rewards == 0
end

local function shouldVisit(aiNk, hero, object)
	local result = call(aiNk, "shouldVisit", hero, object)
	if result ~= nil then
		return result
	end
	if object and object.shouldVisit ~= nil then
		return object.shouldVisit
	end

	local typeID = objectType(object)
	local objectRelations = relations(aiNk, object, hero)

	if typeID == Obj.TOWN or typeID == "TOWN" or typeID == Obj.HERO or typeID == "HERO" then
		return objectRelations == "ENEMIES"
	end

	if typeID == "BORDER_GATE" then
		return not hasQuest(aiNk, object)
	end

	if typeID == Obj.BORDERGUARD or typeID == "BORDERGUARD" then
		return object and object.wasMyColorVisited == true
	end

	if typeID == Obj.SEER_HUT or typeID == "SEER_HUT" then
		if hasQuest(aiNk, object) then
			return object and object.questCompleted == true
		end
		return true
	end

	if typeID == Obj.CREATURE_GENERATOR1 or typeID == "CREATURE_GENERATOR1" then
		if objectRelations == "ENEMIES" then
			return true
		end
		if objectRelations == "ALLIES" then
			return false
		end
		return object and object.canRecruit == true
	end

	if typeID == Obj.HILL_FORT or typeID == "HILL_FORT" then
		return hero and hero.hasUpgradeableStacks == true
	end

	if typeID == Obj.MONOLITH_ONE_WAY_ENTRANCE
		or typeID == Obj.MONOLITH_ONE_WAY_EXIT
		or typeID == Obj.MONOLITH_TWO_WAY
		or typeID == Obj.WHIRLPOOL
		or typeID == "MONOLITH_ONE_WAY_ENTRANCE"
		or typeID == "MONOLITH_ONE_WAY_EXIT"
		or typeID == "MONOLITH_TWO_WAY"
		or typeID == "WHIRLPOOL" then
		return false
	end

	if typeID == Obj.SCHOOL_OF_MAGIC or typeID == Obj.SCHOOL_OF_WAR
		or typeID == "SCHOOL_OF_MAGIC" or typeID == "SCHOOL_OF_WAR" then
		return freeGold(aiNk) >= 1000
	end

	if typeID == Obj.LIBRARY_OF_ENLIGHTENMENT or typeID == "LIBRARY_OF_ENLIGHTENMENT" then
		return (hero and hero.level or 0) >= 10
	end

	if typeID == Obj.TREE_OF_KNOWLEDGE or typeID == "TREE_OF_KNOWLEDGE" then
		return heroRole(aiNk, hero) ~= PriorityEvaluator.HeroRole.SCOUT
			and getResource(freeResources(aiNk), EGameResID.GOLD) >= 2000
			and getResource(freeResources(aiNk), EGameResID.GEMS) >= 10
	end

	if typeID == Obj.MAGIC_WELL or typeID == "MAGIC_WELL" then
		return (hero and hero.mana or 0) < (hero and hero.manaLimit or 0)
	end

	if typeID == Obj.PRISON or typeID == "PRISON" then
		return not (call(aiNk and aiNk.heroManager, "heroCapReached") or aiNk and aiNk.heroCapReached)
	end

	if typeID == Obj.TAVERN or typeID == Obj.EYE_OF_MAGI or typeID == Obj.BOAT or typeID == Obj.SIGN
		or typeID == "TAVERN" or typeID == "EYE_OF_MAGI" or typeID == "BOAT" or typeID == "SIGN" then
		return false
	end

	if wasVisited(object, hero) or rewardableHasNoRewards(object) then
		return false
	end

	return true
end

local function isObjectList(value)
	return type(value) == "table" and #value > 0 and objectType(value) == nil and value.visitablePos == nil
end

function CaptureObjectsBehavior:init(objectsToCapture)
	self.objectTypes = {}
	self.objectSubTypes = {}
	self.objectsToCapture = {}
	self.specificObjects = false

	if objectsToCapture ~= nil then
		self.specificObjects = true
		if isObjectList(objectsToCapture) then
			self.objectsToCapture = objectsToCapture
		else
			self.objectsToCapture = { objectsToCapture }
		end
	end
end

function CaptureObjectsBehavior:toString()
	return "Capture objects"
end

function CaptureObjectsBehavior:equalsTyped(other)
	if self.specificObjects ~= other.specificObjects then
		return false
	end

	if self.specificObjects then
		return vectorEquals(self.objectsToCapture, other.objectsToCapture)
	end

	return vectorEquals(self.objectTypes, other.objectTypes)
		and vectorEquals(self.objectSubTypes, other.objectSubTypes)
end

function CaptureObjectsBehavior:ofType(typeID, subType)
	table.insert(self.objectTypes, typeID)
	if subType ~= nil then
		table.insert(self.objectSubTypes, subType)
	end
	return self
end

function CaptureObjectsBehavior.objectMatchesFilter(self, object)
	if #self.objectTypes > 0 and not contains(self.objectTypes, objectType(object)) then
		return false
	end
	if #self.objectSubTypes > 0 and not contains(self.objectSubTypes, objectSubType(object)) then
		return false
	end
	return true
end

function CaptureObjectsBehavior.getVisitGoals(paths, aiNk, objToVisit, force)
	local tasks = {}
	local closestWaysByRole = {}
	local waysToVisitObj = {}

	for _, path in ipairs(paths or {}) do
		table.insert(tasks, Invalid.new())

		if objToVisit and not force and not shouldVisit(aiNk, path.targetHero, objToVisit) then
			-- Keep the Invalid placeholder.
		elseif path.targetHero and path.targetHero.owner ~= nil and aiNk and aiNk.playerID ~= nil and path.targetHero.owner ~= aiNk.playerID then
			-- Keep the Invalid placeholder.
		elseif heroRole(aiNk, path.targetHero) == PriorityEvaluator.HeroRole.SCOUT
			and (totalDanger(path) == 0 or pathTurn(path) > 0)
			and (path.exchangeCount or 0) > 1 then
			-- Keep the Invalid placeholder.
		elseif pathHeroesLocked(aiNk, path) then
			-- Keep the Invalid placeholder.
		else
			local blockedAction = firstBlockedAction(path)
			if blockedAction then
				local subGoal = call(blockedAction, "decompose", aiNk, path.targetHero) or blockedAction.subGoal
				if subGoal and not subGoal:invalid() then
					local composition = Composition.new()
					composition:addNext(ExecuteHeroChain.new(path, objToVisit))
					composition:addNext(subGoal)
					tasks[#tasks] = composition
				end
			elseif isSafeToVisit(path.targetHero, path.heroArmy, totalDanger(path), safeAttackRatio(aiNk)) then
				local newWay = ExecuteHeroChain.new(path, objToVisit)
				local role = heroRole(aiNk, path.targetHero)
				local closestWay = closestWaysByRole[role]

				if not closestWay or movementCost(closestWay) > movementCost(path) then
					closestWaysByRole[role] = path
				end

				table.insert(waysToVisitObj, newWay)
				tasks[#tasks] = newWay
			end
		end
	end

	for _, way in ipairs(waysToVisitObj) do
		local role = heroRole(aiNk, way.chainPath.targetHero)
		local closestWay = closestWaysByRole[role]
		if closestWay then
			way.closestWayRatio = movementCost(closestWay) / movementCost(way.chainPath)
		end
	end

	return tasks
end

function CaptureObjectsBehavior:decomposeObjects(result, objects, aiNk)
	if #objects == 0 then
		return
	end

	for _, objToVisit in ipairs(objects) do
		if CaptureObjectsBehavior.objectMatchesFilter(self, objToVisit) then
			local paths = {}
			local returnedPaths = nil
			if aiNk and aiNk.pathfinder and type(aiNk.pathfinder.calculatePathInfo) == "function" then
				returnedPaths = aiNk.pathfinder:calculatePathInfo(
					paths,
					visitablePos(objToVisit),
					call(aiNk, "isObjectGraphAllowed") or aiNk.isObjectGraphAllowedValue or false)
			end

			paths = returnedPaths or objToVisit.paths or paths
			for _, task in ipairs(CaptureObjectsBehavior.getVisitGoals(paths, aiNk, objToVisit, self.specificObjects)) do
				table.insert(result, task)
			end
		end
	end
end

local function memoryObjects(aiNk)
	return call(aiNk and aiNk.memory, "visitableIdsToObjsVector", aiNk and aiNk.cc) or aiNk and aiNk.visitableObjects or {}
end

local function nearbyObjects(aiNk)
	return call(aiNk and aiNk.objectClusterizer, "getNearbyObjects") or aiNk and aiNk.nearbyObjects or {}
end

local function farObjects(aiNk)
	return call(aiNk and aiNk.objectClusterizer, "getFarObjects") or aiNk and aiNk.farObjects or {}
end

local function scanDepth(aiNk)
	return call(aiNk, "getScanDepth") or aiNk and aiNk.scanDepth
end

function CaptureObjectsBehavior:decompose(aiNk)
	local tasks = {}

	if self.specificObjects then
		self:decomposeObjects(tasks, self.objectsToCapture, aiNk)
	elseif #self.objectTypes > 0 then
		self:decomposeObjects(tasks, memoryObjects(aiNk), aiNk)
	else
		self:decomposeObjects(tasks, nearbyObjects(aiNk), aiNk)

		if #tasks == 0 or scanDepth(aiNk) ~= State.ScanDepth.SMALL then
			self:decomposeObjects(tasks, farObjects(aiNk), aiNk)
		end
	end

	return tasks
end

CaptureObjectsBehavior.Obj = Obj
CaptureObjectsBehavior.shouldVisit = shouldVisit
CaptureObjectsBehavior.vectorEquals = vectorEquals

return CaptureObjectsBehavior
