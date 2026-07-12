-- Mirrors AI/Nullkiller2/Behaviors/DefenceBehavior.{h,cpp}: DefenceBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local BuyArmy = require("Goals.BuyArmy")
local CaptureObjectsBehavior = require("Behaviors.CaptureObjectsBehavior")
local CGoal = require("Goals.CGoal")
local Composition = require("Goals.Composition")
local DefendTown = require("Markers.DefendTown")
local DismissHero = require("Goals.DismissHero")
local ExchangeSwapTownHeroes = require("Goals.ExchangeSwapTownHeroes")
local ExecuteHeroChain = require("Goals.ExecuteHeroChain")
local PriorityEvaluator = require("Engine.PriorityEvaluator")
local RecruitHero = require("Goals.RecruitHero")
local RecruitHeroBehavior = require("Behaviors.RecruitHeroBehavior")
local State = require("Engine.State")

local DefenceBehavior = CGoal.derive("DefenceBehavior", AbstractGoal.EGoals.DEFENCE)

DefenceBehavior.THREAT_IGNORE_RATIO = 2
DefenceBehavior.DEFENSIVE_EMERGENCY_PRIORITY = 1000000.0

local FortLevel = {
	CITADEL = 2,
	CASTLE = 3
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

local function sameObject(lhs, rhs)
	if lhs == rhs then
		return true
	end
	return objectID(lhs) ~= nil and objectID(lhs) == objectID(rhs)
end

local function armyStrength(object)
	return call(object, "getArmyStrength")
		or call(object, "getTotalStrength")
		or object and (object.totalStrength or object.armyStrength or object.strength)
		or 0
end

local function fortLevel(town)
	return call(town, "fortLevel") or town and (town.fortLevel or 0) or 0
end

local function garrisonHero(town)
	return call(town, "getGarrisonHero") or town and town.garrisonHero
end

local function visitingHero(town)
	return call(town, "getVisitingHero") or town and town.visitingHero
end

local function upperArmy(town)
	return call(town, "getUpperArmy") or town and (town.upperArmy or town)
end

local function pathTurn(path)
	local result = call(path, "turn")
	if result ~= nil then
		return result
	end
	return path and (path.turn or path.turns) or 0
end

local function movementCost(path)
	local result = call(path, "movementCost")
	if result ~= nil then
		return result
	end
	return path and path.movementCost or math.huge
end

local function pathHeroStrength(path)
	return call(path, "getHeroStrength") or path and (path.heroStrength or armyStrength(path.heroArmy)) or 0
end

local function firstBlockedAction(path)
	local result = call(path, "getFirstBlockedAction")
	if result ~= nil then
		return result
	end
	return path and path.firstBlockedAction
end

local function containsHero(path, hero)
	local result = call(path, "containsHero", hero)
	if result ~= nil then
		return result
	end
	for _, node in ipairs(path and path.nodes or {}) do
		if sameObject(node.targetHero, hero) then
			return true
		end
	end
	return false
end

local function canBeMergedWith(hero, town)
	local result = call(hero, "canBeMergedWith", town)
	if result ~= nil then
		return result
	end
	return hero and hero.canMergeWithTown ~= false
end

local function safeAttackRatio(aiNk)
	return call(aiNk and aiNk.settings, "getSafeAttackRatio")
		or aiNk and aiNk.settings and aiNk.settings.safeAttackRatio
		or 1.1
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

local function heroLockedReason(aiNk, hero)
	return call(aiNk, "getHeroLockedReason", hero)
		or aiNk and aiNk.lockedHeroes and aiNk.lockedHeroes[objectID(hero)]
		or State.HeroLockedReason.NOT_LOCKED
end

local function isHeroLocked(aiNk, hero)
	local result = call(aiNk, "isHeroLocked", hero)
	if result ~= nil then
		return result
	end
	return heroLockedReason(aiNk, hero) ~= State.HeroLockedReason.NOT_LOCKED
end

local function townsInfo(aiNk)
	return call(aiNk and aiNk.cc, "getTownsInfo") or aiNk and aiNk.townsInfo or {}
end

local function heroesInfo(aiNk)
	return call(aiNk and aiNk.cc, "getHeroesInfo") or aiNk and aiNk.heroesInfo or {}
end

local function availableHeroes(aiNk, town)
	return call(aiNk and aiNk.cc, "getAvailableHeroes", town) or town and town.availableHeroes or {}
end

local function heroRole(aiNk, hero)
	return call(aiNk and aiNk.heroManager, "getHeroRoleOrDefaultInefficient", hero)
		or hero and hero.role
		or PriorityEvaluator.HeroRole.SCOUT
end

local function evaluateHero(aiNk, hero)
	return call(aiNk and aiNk.heroManager, "evaluateHero", hero) or hero and hero.evaluateHeroScore or 0
end

local function pathInfo(aiNk, town)
	return call(aiNk and aiNk.pathfinder, "getPathInfo", town and (town.visitablePos or town.tile)) or town and town.defencePaths or {}
end

local function townThreats(aiNk, town)
	return call(aiNk and aiNk.dangerHitMap, "getTownThreats", town) or town and town.threats or {}
end

local function objectThreat(aiNk, town)
	return call(aiNk and aiNk.dangerHitMap, "getObjectThreat", town)
		or town and town.threatNode
		or { fastestDanger = { danger = 0, turn = 255 }, maximumDanger = { danger = 0, turn = 255 } }
end

local function threatVerified(threat)
	if threat == nil then
		return false
	end
	if threat.verified ~= nil then
		return threat.verified
	end
	return threat.hero ~= nil or threat.heroPtr ~= nil
end

local function estimateTownFortificationDefence(town, hasDefenders)
	if not hasDefenders then
		return 0
	end
	if fortLevel(town) == FortLevel.CASTLE then
		return 10000
	end
	if fortLevel(town) == FortLevel.CITADEL then
		return 4000
	end
	return 0
end

local function estimateTownDefence(town, committedDefender)
	local result = armyStrength(town)
	if committedDefender then
		result = math.max(result, armyStrength(committedDefender))
	end
	return result + estimateTownFortificationDefence(town, committedDefender ~= nil and result > 0)
end

local function isTownDefenceSufficient(defenceStrength, threat, ratio)
	if (threat.danger or 0) == 0 then
		return true
	end
	local requiredDefence = threat.turn == 0 and threat.danger or threat.danger * ratio
	return defenceStrength >= requiredDefence
end

local function shouldLockTownDefender(town, defender, threat, ratio)
	if (threat.danger or 0) == 0 or (threat.turn or 255) > 1 then
		return false
	end
	if isTownDefenceSufficient(estimateTownDefence(town, nil), threat, ratio) then
		return false
	end
	return isTownDefenceSufficient(estimateTownDefence(town, defender), threat, ratio)
end

local function countTownThreatsCoveredByDefender(town, defender, threats, ratio)
	local result = 0
	local townDefence = estimateTownDefence(town, nil)
	local defenceWithHero = estimateTownDefence(town, defender)

	for _, threat in ipairs(threats or {}) do
		if (threat.danger or 0) ~= 0 and (threat.turn or 255) <= 1
			and not isTownDefenceSufficient(townDefence, threat, ratio)
			and isTownDefenceSufficient(defenceWithHero, threat, ratio) then
			result = result + 1
		end
	end

	return result
end

local function shouldReserveTownDefender(town, defender, threats, ratio)
	local townDefence = estimateTownDefence(town, nil)
	local defenceWithHero = estimateTownDefence(town, defender)
	if defenceWithHero <= townDefence then
		return false
	end

	for _, threat in ipairs(threats or {}) do
		if (threat.danger or 0) ~= 0 and (threat.turn or 255) <= 1
			and not isTownDefenceSufficient(townDefence, threat, ratio) then
			return true
		end
	end

	return false
end

local function isHeroRequiredForTownDefence(town, defender, threats, ratio)
	return countTownThreatsCoveredByDefender(town, defender, threats, ratio) > 0
end

local function estimateTownMobileDefence(town)
	return math.max(armyStrength(town), armyStrength(visitingHero(town)), armyStrength(garrisonHero(town)))
end

local function stableTownDefence(town, aiNk)
	local result = estimateTownDefence(town, nil)
	for _, hero in ipairs({ garrisonHero(town), visitingHero(town) }) do
		if hero and heroLockedReason(aiNk, hero) == State.HeroLockedReason.DEFENCE then
			result = math.max(result, estimateTownDefence(town, hero))
		end
	end
	return result
end

local function hasStableTownDefence(town, threat, aiNk)
	if (threat.danger or 0) == 0 then
		return true
	end
	return isTownDefenceSufficient(stableTownDefence(town, aiNk), threat, safeAttackRatio(aiNk))
end

local function shouldLockPathDefender(town, threat, path, aiNk)
	return pathTurn(path) == 0 and shouldLockTownDefender(town, path.targetHero, threat, safeAttackRatio(aiNk))
end

local function setDefensiveEmergencyPriority(composition, emergency)
	if emergency then
		composition:setpriority(DefenceBehavior.DEFENSIVE_EMERGENCY_PRIORITY)
	end
end

local function isThreatUnderControl(_town, threat, _aiNk, paths)
	for _, path in ipairs(paths or {}) do
		local threatIsWeak = (threat.danger or 0) > 0
			and pathHeroStrength(path) / threat.danger > DefenceBehavior.THREAT_IGNORE_RATIO
		if threatIsWeak
			and (((path.exchangeCount or 0) == 1 and pathTurn(path) < threat.turn)
				or pathTurn(path) < threat.turn - 1
				or (pathTurn(path) < threat.turn and threat.turn >= 2)) then
			return true
		end
	end
	return false
end

local function handleCounterAttack(town, threat, maximumDanger, aiNk, tasks)
	if not hasStableTownDefence(town, threat, aiNk) then
		return
	end
	if threatVerified(threat) and (threat.turn or 255) <= 1
		and ((threat.danger or 0) == (maximumDanger and maximumDanger.danger or 0)
			or (threat.turn or 255) < (maximumDanger and maximumDanger.turn or 255)) then
		local threatHero = threat.hero or threat.heroPtr
		local paths = threat.heroCapturingPaths or pathInfo(aiNk, { visitablePos = threatHero and (threatHero.visitablePos or threatHero.tile) })
		local goals = CaptureObjectsBehavior.getVisitGoals(paths, aiNk, threatHero)
		for index, path in ipairs(paths) do
			local goal = goals[index]
			if goal and not goal:invalid() and goal:isElementar() then
				local composition = Composition.new()
				composition:addNext(DefendTown.new(town, threat, path, true))
				composition:addNext(goal)
				table.insert(tasks, composition)
			end
		end
	end
end

function DefenceBehavior:init()
	self.goalType = AbstractGoal.EGoals.DEFENCE
end

function DefenceBehavior:toString()
	return "Defend towns"
end

function DefenceBehavior:equalsTyped(_other)
	return true
end

function DefenceBehavior.evaluateRecruitingHero(tasks, threat, town, aiNk)
	if not town.hasTavern and not town.hasBuiltTavern then
		return
	end
	for _, hero in ipairs(availableHeroes(aiNk, town)) do
		if RecruitHeroBehavior.isDefensiveRecruitEmergency(town, hero, threat, safeAttackRatio(aiNk)) then
			local sequence = {}
			if visitingHero(town) and not garrisonHero(town) then
				table.insert(sequence, ExchangeSwapTownHeroes.new(town, visitingHero(town)))
			end
			table.insert(sequence, RecruitHero.new(town, hero))
			table.insert(sequence, ExchangeSwapTownHeroes.new(town, hero, State.HeroLockedReason.DEFENCE))

			local composition = Composition.new()
			composition:addNext(DefendTown.new(town, threat, hero))
			composition:addNextSequence(sequence)
			composition:setpriority(DefenceBehavior.DEFENSIVE_EMERGENCY_PRIORITY)
			table.insert(tasks, composition)
		end
	end
end

function DefenceBehavior.handleGarrisonHeroFromPreviousTurn(town, tasks, aiNk, threats)
	local hero = garrisonHero(town)
	if not hero then
		return false
	end
	if isHeroLocked(aiNk, hero) or shouldReserveTownDefender(town, hero, threats, safeAttackRatio(aiNk)) then
		return true
	end
	if not visitingHero(town) and town.canExtractGarrisonHero then
		table.insert(tasks, ExchangeSwapTownHeroes.new(town, nil):setpriority(5))
	end
	return false
end

function DefenceBehavior:evaluateDefence(tasks, town, aiNk)
	local threatNode = objectThreat(aiNk, town)
	local threats = {}
	for _, threat in ipairs(townThreats(aiNk, town)) do
		table.insert(threats, threat)
	end
	table.insert(threats, threatNode.fastestDanger)

	if DefenceBehavior.handleGarrisonHeroFromPreviousTurn(town, tasks, aiNk, threats) then
		return
	end

	if not threatVerified(threatNode.fastestDanger) then
		return
	end

	local reinforcement = call(aiNk and aiNk.armyManager, "howManyReinforcementsCanBuy", upperArmy(town), town)
		or town.reinforcementsCanBuy
		or 0
	if reinforcement ~= 0 then
		table.insert(tasks, BuyArmy.new(town, reinforcement):setpriority(0.5))
	end

	local paths = pathInfo(aiNk, town)
	local closestWay = nil
	for _, path in ipairs(paths) do
		if not closestWay or movementCost(path) < movementCost(closestWay) then
			closestWay = path
		end
	end

	for _, threat in ipairs(threats) do
		handleCounterAttack(town, threat, threatNode.maximumDanger, aiNk, tasks)
		if not isThreatUnderControl(town, threat, aiNk, paths) then
			DefenceBehavior.evaluateRecruitingHero(tasks, threat, town, aiNk)

			for _, path in ipairs(paths) do
				local lockDefenderNow = shouldLockPathDefender(town, threat, path, aiNk)
				local townDefenseStrength = estimateTownMobileDefence(town)
				local heroStrengthCoversThreat = pathTurn(path) <= (threat.turn or 0)
					and pathHeroStrength(path) >= (threat.danger or 0) * safeAttackRatio(aiNk)

				if ((not visitingHero(town) or not sameObject(path.targetHero, visitingHero(town)) or pathHeroStrength(path) >= townDefenseStrength)
					and (not garrisonHero(town) or not sameObject(path.targetHero, garrisonHero(town)) or pathHeroStrength(path) >= townDefenseStrength)
					and not (pathTurn(path) <= (threat.turn or 0) - 2)
					and canBeMergedWith(path.targetHero, town)
					and ((threat.turn or 0) == 0 or lockDefenderNow or heroStrengthCoversThreat)
					and not pathHeroesLocked(aiNk, path)) then
					local composition = Composition.new()
					composition:addNext(DefendTown.new(town, threat, path))

					local sequence = {}
					if visitingHero(town) and not sameObject(path.targetHero, visitingHero(town)) and not containsHero(path, visitingHero(town)) then
						if garrisonHero(town) and not sameObject(garrisonHero(town), path.targetHero) then
							sequence = nil
						elseif pathTurn(path) == 0 then
							table.insert(sequence, ExchangeSwapTownHeroes.new(town, visitingHero(town)))
						end
					end

					if sequence then
						local heroChain = ExecuteHeroChain.new(path, town)
						if closestWay then
							heroChain.closestWayRatio = movementCost(closestWay) / movementCost(path)
						end
						table.insert(sequence, heroChain)
						if lockDefenderNow then
							table.insert(sequence, ExchangeSwapTownHeroes.new(town, path.targetHero, State.HeroLockedReason.DEFENCE))
						end
						composition:addNextSequence(sequence)
						setDefensiveEmergencyPriority(composition, lockDefenderNow)

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
	end
end

function DefenceBehavior:decompose(aiNk)
	local tasks = {}
	for _, town in ipairs(townsInfo(aiNk)) do
		self:evaluateDefence(tasks, town, aiNk)
	end
	return tasks
end

DefenceBehavior.estimateTownFortificationDefence = estimateTownFortificationDefence
DefenceBehavior.estimateTownDefence = estimateTownDefence
DefenceBehavior.isTownDefenceSufficient = isTownDefenceSufficient
DefenceBehavior.shouldLockTownDefender = shouldLockTownDefender
DefenceBehavior.countTownThreatsCoveredByDefender = countTownThreatsCoveredByDefender
DefenceBehavior.shouldReserveTownDefender = shouldReserveTownDefender
DefenceBehavior.isHeroRequiredForTownDefence = isHeroRequiredForTownDefence

return DefenceBehavior
