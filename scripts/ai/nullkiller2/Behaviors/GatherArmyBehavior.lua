-- Mirrors AI/Nullkiller2/Behaviors/GatherArmyBehavior.{h,cpp}: GatherArmyBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local ArmyUpgrade = require("Markers.ArmyUpgrade")
local CaptureObjectsBehavior = require("Behaviors.CaptureObjectsBehavior")
local CGoal = require("Goals.CGoal")
local Composition = require("Goals.Composition")
local ExchangeSwapTownHeroes = require("Goals.ExchangeSwapTownHeroes")
local ExecuteHeroChain = require("Goals.ExecuteHeroChain")
local HeroExchange = require("Markers.HeroExchange")
local PriorityEvaluator = require("Engine.PriorityEvaluator")
local State = require("Engine.State")

local GatherArmyBehavior = CGoal.derive("GatherArmyBehavior", AbstractGoal.EGoals.GATHER_ARMY)

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	return CGoal.objectID(value)
end

local function sameObject(lhs, rhs)
	if lhs == rhs then
		return true
	end
	return objectID(lhs) ~= nil and objectID(lhs) == objectID(rhs)
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile)
end

local function heroesInfo(aiNk)
	return call(aiNk and aiNk.cc, "getHeroesInfo") or aiNk and aiNk.heroesInfo or {}
end

local function townsInfo(aiNk)
	return call(aiNk and aiNk.cc, "getTownsInfo") or aiNk and aiNk.townsInfo or {}
end

local function heroRole(aiNk, hero)
	return call(aiNk and aiNk.heroManager, "getHeroRoleOrDefaultInefficient", hero)
		or hero and hero.role
		or PriorityEvaluator.HeroRole.SCOUT
end

local function evaluateHero(aiNk, hero)
	return call(aiNk and aiNk.heroManager, "evaluateHero", hero) or hero and hero.evaluateHeroScore or 0
end

local function armyStrength(army)
	local result = call(army, "getArmyStrength")
	if result ~= nil then
		return result
	end
	return army and (army.armyStrength or army.totalStrength or army.strength) or 0
end

local function heroArmyStrength(hero)
	return armyStrength(hero)
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

local function pathHeroStrength(path)
	local result = call(path, "getHeroStrength")
	if result ~= nil then
		return result
	end
	return path and (path.heroStrength or armyStrength(path.heroArmy)) or 0
end

local function normalizedHeroStrength(hero)
	local result = call(hero, "getHeroStrength") or hero and (hero.heroStrength or hero.normalizedHeroStrength)
	if type(result) ~= "number" or result <= 0 or result ~= result or result == math.huge or result == -math.huge then
		return 1.0
	end
	return result
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

local function scoutHeroTurnDistanceLimit(aiNk)
	return call(aiNk and aiNk.settings, "getScoutHeroTurnDistanceLimit")
		or aiNk and aiNk.settings and aiNk.settings.scoutHeroTurnDistanceLimit
		or 1
end

local function objectGraphAllowed(aiNk)
	local result = call(aiNk, "isObjectGraphAllowed")
	if result ~= nil then
		return result
	end
	return aiNk and aiNk.isObjectGraphAllowedValue or false
end

local function pathInfo(aiNk, tile, fallback)
	return call(aiNk and aiNk.pathfinder, "getPathInfo", tile, objectGraphAllowed(aiNk)) or fallback or {}
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

local function containsHero(path, hero)
	if call(path, "containsHero", hero) ~= nil then
		return call(path, "containsHero", hero)
	end
	for _, pathHero in ipairs(path.containsHeroes or {}) do
		if sameObject(pathHero, hero) then
			return true
		end
	end
	for _, node in ipairs(path.nodes or {}) do
		if sameObject(node.targetHero, hero) then
			return true
		end
	end
	return false
end

local function visitedTown(hero)
	return call(hero, "getVisitedTown") or hero and hero.visitedTown
end

local function isGarrisoned(hero)
	local result = call(hero, "isGarrisoned")
	if result ~= nil then
		return result
	end
	return hero and hero.garrisoned == true
end

local function heroLockedReason(aiNk, hero)
	return call(aiNk, "getHeroLockedReason", hero)
		or aiNk and aiNk.lockedHeroes and aiNk.lockedHeroes[objectID(hero)]
		or State.HeroLockedReason.NOT_LOCKED
end

local function visitingHero(town)
	return call(town, "getVisitingHero") or town and town.visitingHero
end

local function garrisonHero(town)
	return call(town, "getGarrisonHero") or town and town.garrisonHero
end

local function upperArmy(town)
	return call(town, "getUpperArmy") or town and (town.upperArmy or town)
end

local function freeResources(aiNk)
	return call(aiNk, "getFreeResources") or aiNk and aiNk.freeResources or {}
end

local function calculateCreaturesUpgrade(aiNk, path, upgrader)
	return call(aiNk and aiNk.armyManager, "calculateCreaturesUpgrade", path.heroArmy, upgrader, freeResources(aiNk))
		or path.upgrade
		or { upgradeValue = 0, upgradeCost = {} }
end

local function addUpgradeInfo(upgrade, extra)
	if not extra then
		return upgrade
	end
	upgrade.upgradeValue = (upgrade.upgradeValue or 0) + (extra.upgradeValue or 0)
	if extra.upgradeCost then
		upgrade.upgradeCost = upgrade.upgradeCost or {}
		for key, value in pairs(extra.upgradeCost) do
			upgrade.upgradeCost[key] = (upgrade.upgradeCost[key] or 0) + value
		end
	end
	return upgrade
end

function GatherArmyBehavior:init()
	self.goalType = AbstractGoal.EGoals.GATHER_ARMY
end

function GatherArmyBehavior:toString()
	return "Gather army"
end

function GatherArmyBehavior:equalsTyped(_other)
	return true
end

function GatherArmyBehavior:deliverArmyToHero(aiNk, receiverHero)
	local tasks = {}
	local targetHeroScore = evaluateHero(aiNk, receiverHero)
	local paths = pathInfo(aiNk, visitablePos(receiverHero), receiverHero.gatherArmyPaths)

	for _, path in ipairs(paths) do
		if path.targetHero and path.targetHero.owner ~= nil and aiNk and aiNk.playerID ~= nil and path.targetHero.owner ~= aiNk.playerID then
			-- Skip foreign heroes.
		elseif containsHero(path, receiverHero) then
			-- Skip self-containing paths.
		elseif pathHeroesLocked(aiNk, path) then
			-- Skip locked chains.
		else
			local heroExchange = HeroExchange.new(receiverHero, path)
			local additionalArmyStrength = heroExchange:getReinforcementArmyStrength(aiNk)
			local additionalArmyRatio = additionalArmyStrength / math.max(1, heroArmyStrength(receiverHero))

			if not ((additionalArmyRatio < 0.1 and additionalArmyStrength < 20000) or additionalArmyStrength < 500) then
				local hasOtherMainInPath = false
				for _, node in ipairs(path.nodes or {}) do
					if node.targetHero and heroRole(aiNk, node.targetHero) == PriorityEvaluator.HeroRole.MAIN then
						if evaluateHero(aiNk, node.targetHero) >= targetHeroScore then
							hasOtherMainInPath = true
							break
						end
					end
				end

				if not hasOtherMainInPath
					and isSafeToVisit(receiverHero, path.heroArmy, totalDanger(path), safeAttackRatio(aiNk)) then
					local composition = Composition.new()
					local exchangePath = ExecuteHeroChain.new(path, receiverHero)
					exchangePath.closestWayRatio = 1
					composition:addNext(heroExchange)

					if isGarrisoned(receiverHero) and pathTurn(path) == 0 then
						local lockReason = heroLockedReason(aiNk, receiverHero)
						local town = visitedTown(receiverHero)
						if sameObject(visitedTown(path.targetHero), town) then
							composition:addNextSequence({ ExchangeSwapTownHeroes.new(town, receiverHero, lockReason) })
						else
							composition:addNextSequence({
								ExchangeSwapTownHeroes.new(town),
								exchangePath,
								ExchangeSwapTownHeroes.new(town, receiverHero, lockReason)
							})
						end
					else
						composition:addNext(exchangePath)
					end

					local blockedAction = firstBlockedAction(path)
					if blockedAction then
						local subGoal = call(blockedAction, "decompose", aiNk, path.targetHero) or blockedAction.subGoal
						if not subGoal or subGoal:invalid() then
							composition = nil
						else
							composition:addNext(subGoal)
						end
					end

					if composition then
						table.insert(tasks, composition)
					end
				end
			end
		end
	end

	return tasks
end

function GatherArmyBehavior:upgradeArmy(aiNk, upgrader)
	local tasks = {}
	local paths = pathInfo(aiNk, visitablePos(upgrader), upgrader.upgradePaths)
	local goals = CaptureObjectsBehavior.getVisitGoals(paths, aiNk)

	local hasMainAround = false
	for _, path in ipairs(paths) do
		if heroRole(aiNk, path.targetHero) == PriorityEvaluator.HeroRole.MAIN
			and pathTurn(path) < scoutHeroTurnDistanceLimit(aiNk) then
			hasMainAround = true
			break
		end
	end

	for index, path in ipairs(paths) do
		local visitGoal = goals[index]
		if visitGoal and not visitGoal:invalid()
			and not (visitingHero(upgrader) and (not sameObject(visitingHero(upgrader), path.targetHero) or (path.exchangeCount or 0) == 1))
			and not pathHeroesLocked(aiNk, path)
			and not firstBlockedAction(path) then
			local upgrade = calculateCreaturesUpgrade(aiNk, path, upgrader)

			if not garrisonHero(upgrader)
				and (hasMainAround or heroRole(aiNk, path.targetHero) == PriorityEvaluator.HeroRole.MAIN) then
				addUpgradeInfo(upgrade, path.armyToGetOrBuy)
			end

			local armyValue = (upgrade.upgradeValue or 0) / math.max(1, pathHeroStrength(path))
			if not ((armyValue < 0.25 and (upgrade.upgradeValue or 0) < 40000) or (upgrade.upgradeValue or 0) < 2000)
				and isSafeToVisit(path.targetHero, path.heroArmy, totalDanger(path), safeAttackRatio(aiNk)) then
				local composition = Composition.new()
				composition:addNext(ArmyUpgrade.new(path, upgrader, upgrade))
				composition:addNext(visitGoal)
				table.insert(tasks, composition)
			end
		end
	end

	return tasks
end

function GatherArmyBehavior:decompose(aiNk)
	local tasks = {}
	local heroes = heroesInfo(aiNk)

	if #heroes == 0 then
		return tasks
	end

	for _, hero in ipairs(heroes) do
		if heroRole(aiNk, hero) == PriorityEvaluator.HeroRole.MAIN then
			for _, task in ipairs(self:deliverArmyToHero(aiNk, hero)) do
				table.insert(tasks, task)
			end
		end
	end

	for _, town in ipairs(townsInfo(aiNk)) do
		for _, task in ipairs(self:upgradeArmy(aiNk, town)) do
			table.insert(tasks, task)
		end
	end

	return tasks
end

return GatherArmyBehavior
