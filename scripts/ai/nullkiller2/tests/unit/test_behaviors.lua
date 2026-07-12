local AbstractGoal = require("Goals.AbstractGoal")
local BuyArmyBehavior = require("Behaviors.BuyArmyBehavior")
local ExchangeSwapTownHeroes = require("Goals.ExchangeSwapTownHeroes")
local PriorityEvaluator = require("Engine.PriorityEvaluator")
local RecruitHeroBehavior = require("Behaviors.RecruitHeroBehavior")
local State = require("Engine.State")

local town = { id = 10, name = "Castle", factionID = 1, townLevel = 5, canRecruitHero = true }
local weakHero = { id = 20, name = "Weak", armyCost = 500, totalStrength = 1000, evaluateHeroScore = 10, factionID = 1 }
local strongHero = { id = 21, name = "Strong", armyCost = 3000, totalStrength = 7000, evaluateHeroScore = 20, factionID = 1 }

assert(RecruitHeroBehavior.isDefensiveRecruitEmergency(
	{ upperArmyStrength = 1000 },
	strongHero,
	{ danger = 3000, turn = 1 },
	1.1) == true)
assert(RecruitHeroBehavior.isDefensiveRecruitEmergency(
	{ upperArmyStrength = 5000 },
	strongHero,
	{ danger = 3000, turn = 1 },
	1.1) == false)

assert(RecruitHeroBehavior.shouldRecruitHero(0, { hero = weakHero, score = 1 }, false, 0, 0, true) == true)
assert(RecruitHeroBehavior.shouldRecruitHero(3, { hero = weakHero, score = 1 }, false, 16, 0, true) == true)
assert(RecruitHeroBehavior.shouldRecruitHero(3, { hero = strongHero, score = 1, defensiveEmergency = false }, false, 0, 0, false) == true)
assert(RecruitHeroBehavior.shouldRecruitHero(3, { hero = weakHero, score = 0 }, false, 0, 0, false) == false)

local bestChoice = RecruitHeroBehavior.RecruitHeroChoice()
RecruitHeroBehavior.calculateBestHero(
	{ weakHero, strongHero },
	{},
	bestChoice,
	town,
	{ { danger = 3000, turn = 1 } },
	1.1,
	0)
assert(bestChoice.hero == strongHero)
assert(bestChoice.town == town)
assert(bestChoice.defensiveEmergency == true)

local aiNk = {
	townsInfo = {
		{
			id = 30,
			name = "Town",
			factionID = 1,
			townLevel = 4,
			canRecruitHero = true,
			threats = {
				{ danger = 3000, turn = 1 }
			},
			availableHeroes = { weakHero, strongHero },
			upperArmyStrength = 1000
		}
	},
	heroesInfo = { { id = 40, role = PriorityEvaluator.HeroRole.MAIN, visitablePos = { x = 1, y = 1, z = 0 } } },
	freeResources = { [6] = 5000 },
	settings = { safeAttackRatio = 1.1 },
	heroManager = {},
	dangerHitMap = {},
	objectClusterizer = {
		getNearbyObjects = function()
			return {}
		end
	}
}
local recruitTasks = RecruitHeroBehavior.new():decompose(aiNk)
assert(#recruitTasks == 1)
assert(recruitTasks[1].goalType == AbstractGoal.EGoals.COMPOSITION)
assert(recruitTasks[1].priority == RecruitHeroBehavior.DEFENSIVE_EMERGENCY_RECRUIT_PRIORITY)
local sequence = recruitTasks[1]:decompose({})
assert(sequence[1].goalType == AbstractGoal.EGoals.RECRUIT_HERO)
assert(sequence[2].goalType == AbstractGoal.EGoals.EXCHANGE_SWAP_TOWN_HEROES)

local exchangeTown = { id = 50, name = "Rampart", visitingHero = { id = 51 }, garrisonHero = { id = 52 } }
local exchange = ExchangeSwapTownHeroes.new(exchangeTown, exchangeTown.garrisonHero, State.HeroLockedReason.DEFENCE)
assert(exchange:toString() == "Exchange and swap heroes of Rampart")
assert(exchange:isObjectAffected(50) == true)
assert(exchange:isObjectAffected(51) == true)
assert(exchange:isObjectAffected(52) == true)
assert(#exchange:getAffectedObjects() == 3)
assert(exchange:accept({}).action == "exchangeSwapTownHeroes")

local buyTasks = BuyArmyBehavior.new():decompose({
	heroesInfo = {
		{ id = 70, role = PriorityEvaluator.HeroRole.MAIN },
		{ id = 71, role = PriorityEvaluator.HeroRole.SCOUT }
	},
	townsInfo = {
		{
			id = 80,
			name = "Inferno",
			closestThreatTurn = 0,
			reinforcementsByHero = {
				[70] = 1500
			},
			reinforcementsCanBuy = 1000
		}
	},
	heroManager = {},
	armyManager = {}
})
assert(#buyTasks == 1)
assert(buyTasks[1].goalType == AbstractGoal.EGoals.BUY_ARMY)
assert(buyTasks[1].value == 1000)
assert(buyTasks[1].priority == 1000)

local blockedBuyTasks = BuyArmyBehavior.new():decompose({
	heroesInfo = {
		{ id = 70, role = PriorityEvaluator.HeroRole.MAIN }
	},
	townsInfo = {
		{ id = 90, closestThreatTurn = 3, hasCityHall = false, canBuildCityHall = true }
	},
	goldPressureOverMax = true
})
assert(#blockedBuyTasks == 0)
