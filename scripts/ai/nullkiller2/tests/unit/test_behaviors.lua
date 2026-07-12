local AbstractGoal = require("Goals.AbstractGoal")
local BuildingBehavior = require("Behaviors.BuildingBehavior")
local BuyArmyBehavior = require("Behaviors.BuyArmyBehavior")
local CaptureObjectsBehavior = require("Behaviors.CaptureObjectsBehavior")
local ClusterBehavior = require("Behaviors.ClusterBehavior")
local DefenceBehavior = require("Behaviors.DefenceBehavior")
local EscapeBehavior = require("Behaviors.EscapeBehavior")
local ExplorationBehavior = require("Behaviors.ExplorationBehavior")
local GatherArmyBehavior = require("Behaviors.GatherArmyBehavior")
local ExchangeSwapTownHeroes = require("Goals.ExchangeSwapTownHeroes")
local CaptureObject = require("Goals.CaptureObject")
local ArmyUpgrade = require("Markers.ArmyUpgrade")
local DefendTown = require("Markers.DefendTown")
local ExplorationHelper = require("Helpers.ExplorationHelper")
local ExplorationPoint = require("Markers.ExplorationPoint")
local HeroExchange = require("Markers.HeroExchange")
local PriorityEvaluator = require("Engine.PriorityEvaluator")
local RecruitHeroBehavior = require("Behaviors.RecruitHeroBehavior")
local State = require("Engine.State")
local StayAtTownBehavior = require("Behaviors.StayAtTownBehavior")
local StartupBehavior = require("Behaviors.StartupBehavior")
local UnlockCluster = require("Markers.UnlockCluster")

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

local vectorGoldRecruitTasks = RecruitHeroBehavior.new():decompose({
	townsInfo = {
		{
			id = 22,
			name = "VectorGoldTown",
			factionID = 1,
			townLevel = 1,
			canRecruitHero = true,
			availableHeroes = {
				{
					id = 23,
					name = "VectorGoldHero",
					armyCost = 100,
					totalStrength = 1000,
					evaluateHeroScore = 10,
					factionID = 1
				}
			}
		}
	},
	heroesInfo = {
		{ id = 24, name = "ExistingHero" }
	},
	freeResources = { [1] = 0, [2] = 0, [3] = 0, [4] = 0, [5] = 0, [6] = 0, [7] = 30001 },
	heroManager = {},
	dangerHitMap = {},
	objectClusterizer = {
		getNearbyObjects = function()
			return {}
		end
	}
})
assert(#vectorGoldRecruitTasks == 1)

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

local exchangeLog = {}
local executableExchangeTown = {
	id = 55,
	name = "Executable Rampart",
	visitablePos = { x = 1, y = 2, z = 0 },
	visitingHero = { id = 56 },
	garrisonHero = { id = 57 },
	upperArmy = { id = 58, stacksCount = 0, slots = {} }
}
ExchangeSwapTownHeroes.new(
	executableExchangeTown,
	executableExchangeTown.garrisonHero,
	State.HeroLockedReason.DEFENCE):accept({
	swapGarrisonHero = function(_, townArg)
		table.insert(exchangeLog, "swap:" .. townArg.id)
	end,
	executeHeroChain = function(_, path, objid)
		table.insert(exchangeLog, "move:" .. path.targetHero.id .. ":" .. objid .. ":" .. path.targetTile.x)
	end,
	lockHero = function(_, hero, reason)
		table.insert(exchangeLog, "lock:" .. hero.id .. ":" .. reason)
	end,
	unlockHero = function(_, hero)
		table.insert(exchangeLog, "unlock:" .. hero.id)
	end
})
assert(exchangeLog[1] == "swap:55")
assert(exchangeLog[2] == "move:57:55:1")
assert(exchangeLog[3] == "swap:55")
assert(exchangeLog[4] == "lock:57:2")
assert(exchangeLog[5] == "unlock:56")

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

local analyzerBuyTasks = BuyArmyBehavior.new():decompose({
	heroesInfo = {
		{
			id = 72,
			role = PriorityEvaluator.HeroRole.MAIN,
			armyStrength = 100,
			slots = {
				{ creature = { id = 7001, factionID = 1, aiValue = 20 }, count = 5, power = 100 }
			}
		}
	},
	townsInfo = {
		{
			id = 82,
			name = "Analyzer Town",
			factionID = 1,
			closestThreatTurn = 0,
			upperArmy = { armySize = 7, stacksCount = 0, slots = {} },
			availableToBuy = {
				{
					level = 0,
					count = 5,
					creature = {
						id = 7002,
						factionID = 1,
						aiValue = 50,
						fullRecruitCost = { [7] = 100 }
					}
				}
			}
		}
	},
	freeResources = { [7] = 500 },
	heroManager = {}
})
assert(#analyzerBuyTasks == 1)
assert(analyzerBuyTasks[1].value == 250)
assert(analyzerBuyTasks[1].priority == 250)

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

local buildBehavior = BuildingBehavior.new()
assert(buildBehavior:toString() == "Build")
assert(buildBehavior:equals(BuildingBehavior.new()) == true)

local threatenedTown = {
	id = 100,
	name = "Stonewatch",
	fortLevel = BuildingBehavior.FortLevel.CASTLE - 1,
	threats = { { turn = 1 } }
}
local emergencyBuildTasks = buildBehavior:decompose({
	buildAnalyzer = {
		developmentInfos = {
			{
				town = threatenedTown,
				toBuild = {
					{ id = 1, name = "Mage Guild", isMissingResources = false },
					{ id = BuildingBehavior.BuildingID.CITADEL, name = "Citadel", isMissingResources = false },
					{ id = BuildingBehavior.BuildingID.CASTLE, name = "Castle", isMissingResources = true }
				}
			}
		}
	},
	dangerHitMap = {
		getTownThreats = function(_, townInfo)
			return townInfo.threats
		end
	}
})
assert(#emergencyBuildTasks == 1)
assert(emergencyBuildTasks[1].goalType == AbstractGoal.EGoals.BUILD_STRUCTURE)
assert(emergencyBuildTasks[1].bid == BuildingBehavior.BuildingID.CITADEL)

local missingCost = { [0] = 5, [6] = 1000 }
local developmentTown = { id = 101, name = "Clearwater", fortLevel = 0 }
local missingResourceTasks = buildBehavior:decompose({
	buildAnalyzer = {
		goldPressureOverMax = false,
		developmentInfos = {
			{
				town = developmentTown,
				toBuild = {
					{
						id = 14,
						name = "Marketplace",
						isMissingResources = true,
						buildCost = missingCost,
						dailyIncome = {}
					}
				}
			}
		}
	},
	lockedResources = {
		canAfford = function(_, cost)
			assert(cost == missingCost)
			return false
		end
	}
})
assert(#missingResourceTasks == 1)
assert(missingResourceTasks[1].goalType == AbstractGoal.EGoals.COMPOSITION)
local missingResourceSequence = missingResourceTasks[1]:decompose({})
assert(#missingResourceSequence == 2)
assert(missingResourceSequence[1].goalType == AbstractGoal.EGoals.BUILD_STRUCTURE)
assert(missingResourceSequence[2].goalType == AbstractGoal.EGoals.SAVE_RESOURCES)
assert(missingResourceSequence[2].resources == missingCost)

local lockedResourceTasks = buildBehavior:decompose({
	buildAnalyzer = {
		goldPressureOverMax = false,
		developmentInfos = {
			{
				town = developmentTown,
				toBuild = {
					{
						id = 14,
						name = "Marketplace",
						isMissingResources = true,
						buildCost = missingCost,
						dailyIncome = {}
					}
				}
			}
		}
	},
	lockedResources = {
		canAfford = function()
			return true
		end
	}
})
assert(#lockedResourceTasks == 0)

local goldPressureTasks = buildBehavior:decompose({
	buildAnalyzer = {
		isGoldPressureOverMax = function()
			return true
		end,
		developmentInfos = {
			{
				town = { id = 102, name = "Goldkeep", fortLevel = BuildingBehavior.FortLevel.CASTLE },
				toBuild = {
					{ id = 11, name = "Town Hall", isMissingResources = false, dailyIncome = { [6] = 0 } },
					{ id = 12, name = "City Hall", isMissingResources = false, dailyIncome = { [6] = 1000 } }
				}
			}
		}
	}
})
assert(#goldPressureTasks == 1)
assert(goldPressureTasks[1].bid == 12)

local stayBehavior = StayAtTownBehavior.new()
assert(stayBehavior:toString() == "StayAtTownBehavior")
assert(stayBehavior:equals(StayAtTownBehavior.new()) == true)

local restingHero = { id = 201, name = "Resting", movementPointsRemaining = 600, movementPointsLimit = 1200 }
local blockedHero = { id = 202, name = "Blocked", movementPointsRemaining = 600, movementPointsLimit = 1200 }
local occupiedHero = { id = 203, name = "Guest", movementPointsRemaining = 600, movementPointsLimit = 1200 }
local stayTown = { id = 204, name = "Tower", visitablePos = { x = 5, y = 6, z = 0 } }
local stayTasks = stayBehavior:decompose({
	townsInfo = { stayTown },
	pathfinder = {
		calculatePathInfo = function(_, paths, tile)
			assert(tile == stayTown.visitablePos)
			paths[1] = {
				targetHero = restingHero,
				tile = tile,
				nodes = { { targetHero = restingHero } },
				exchangeCount = 1,
				movementCost = 0.25
			}
			paths[2] = {
				targetHero = blockedHero,
				tile = tile,
				nodes = { { targetHero = blockedHero } },
				exchangeCount = 0,
				firstBlockedAction = {}
			}
			paths[3] = {
				targetHero = blockedHero,
				tile = tile,
				nodes = { { targetHero = blockedHero } },
				exchangeCount = 2
			}
		end
	}
})
assert(#stayTasks == 1)
assert(stayTasks[1].goalType == AbstractGoal.EGoals.COMPOSITION)
local staySequence = stayTasks[1]:decompose({})
assert(staySequence[1].goalType == AbstractGoal.EGoals.EXECUTE_HERO_CHAIN)
assert(staySequence[2].goalType == AbstractGoal.EGoals.STAY_AT_TOWN)
assert(staySequence[2].hero == restingHero)

local occupiedStayTasks = stayBehavior:decompose({
	townsInfo = {
		{
			id = 205,
			name = "Occupied",
			visitingHero = occupiedHero,
			paths = {
				{ targetHero = restingHero, tile = { x = 1, y = 1, z = 0 }, nodes = {}, exchangeCount = 0 }
			}
		}
	}
})
assert(#occupiedStayTasks == 0)

local startupBehavior = StartupBehavior.new()
assert(startupBehavior:toString() == "Startup")
assert(startupBehavior:equals(StartupBehavior.new()) == true)

local startupTown = {
	id = 301,
	name = "Castle",
	hasTavern = false,
	canBuildTavern = true
}
local startupBuildTasks = startupBehavior:decompose({
	townsInfo = { startupTown }
})
assert(#startupBuildTasks == 1)
assert(startupBuildTasks[1].goalType == AbstractGoal.EGoals.BUILD_STRUCTURE)
assert(startupBuildTasks[1].bid == StartupBehavior.BuildingID.TAVERN)
assert(startupBuildTasks[1].priority == 100)

local startupHero = {
	id = 302,
	name = "Nearest",
	visitablePos = { x = 1, y = 0, z = 0 },
	movementPointsRemaining = 1000
}
local chainTown = {
	id = 303,
	name = "Rampart",
	hasTavern = true,
	canRecruitHero = false,
	visitablePos = { x = 0, y = 0, z = 0 },
	upperArmy = {}
}
local chainPath = {
	targetHero = startupHero,
	tile = chainTown.visitablePos,
	nodes = { { targetHero = startupHero } },
	turn = 0,
	movementCost = 1
}
local startupChainTasks = startupBehavior:decompose({
	townsInfo = { chainTown },
	pathfinder = {
		getPathInfo = function(_, tile)
			assert(tile == chainTown.visitablePos)
			return { chainPath }
		end
	},
	armyManager = {
		howManyReinforcementsCanGet = function()
			return 250
		end
	}
})
assert(#startupChainTasks == 1)
assert(startupChainTasks[1].goalType == AbstractGoal.EGoals.EXECUTE_HERO_CHAIN)
assert(startupChainTasks[1].priority == 100)

local richTown = {
	id = 304,
	name = "Tower",
	hasTavern = true,
	canRecruitHero = true,
	garrisonHero = { id = 305 },
	visitablePos = { x = 0, y = 0, z = 0 }
}
assert(StartupBehavior.needToRecruitHero({
	townsInfo = { richTown },
	heroCount = 2,
	mapSize = { x = 100 },
	nearbyObjects = {
		{ ID = StartupBehavior.Obj.TREASURE_CHEST },
		{ ID = "CAMPFIRE" },
		{ resourceID = 6 },
		{ rewardResources = { [6] = 500 } },
		{ ID = StartupBehavior.Obj.WATER_WHEEL, armyStrength = 10 }
	}
}, richTown) == true)

local visitingMain = {
	id = 306,
	name = "Main",
	role = PriorityEvaluator.HeroRole.MAIN,
	evaluateHeroScore = 100,
	visitablePos = { x = 1, y = 0, z = 0 }
}
local garrisonScout = { id = 307, name = "Scout", role = PriorityEvaluator.HeroRole.SCOUT, evaluateHeroScore = 50 }
local swapTown = {
	id = 308,
	name = "Inferno",
	hasTavern = true,
	canRecruitHero = true,
	visitingHero = visitingMain,
	garrisonHero = garrisonScout,
	visitablePos = { x = 0, y = 0, z = 0 }
}
local startupSwapTasks = startupBehavior:decompose({
	townsInfo = { swapTown },
	heroCount = 1,
	pathfinder = {
		getPathInfo = function()
			return {
				{
					targetHero = visitingMain,
					tile = swapTown.visitablePos,
					nodes = { { targetHero = visitingMain } },
					turn = 0,
					movementCost = 1
				}
			}
		end
	}
})
assert(#startupSwapTasks == 1)
assert(startupSwapTasks[1].goalType == AbstractGoal.EGoals.EXCHANGE_SWAP_TOWN_HEROES)
assert(startupSwapTasks[1].priority == 100)
assert(startupSwapTasks[1]:getLockingReason() == State.HeroLockedReason.STARTUP)

local recruitStartupTown = {
	id = 309,
	name = "Conflux",
	hasTavern = true,
	canRecruitHero = true
}
local startupRecruitTasks = startupBehavior:decompose({
	townsInfo = { recruitStartupTown }
})
assert(#startupRecruitTasks == 1)
assert(startupRecruitTasks[1].goalType == AbstractGoal.EGoals.RECRUIT_HERO)

local escapeEvaluation = EscapeBehavior.evaluateEscapePathCandidate({
	currentTileThreatensHero = true,
	sameDay = true,
	sameTile = false,
	blockedAction = false,
	singleHeroPath = true,
	destinationSafe = true,
	destinationIsSafer = true,
	threatReduction = 40,
	movementCost = 2
})
assert(escapeEvaluation.accepted == true)
assert(escapeEvaluation.score == 20)
assert(EscapeBehavior.evaluateEscapePathCandidate({
	currentTileThreatensHero = true,
	sameDay = true,
	sameTile = true,
	blockedAction = false,
	singleHeroPath = true,
	destinationSafe = true,
	destinationIsSafer = true
}).accepted == false)

local escapingHero = {
	id = 401,
	name = "Endangered",
	visitablePos = { x = 0, y = 0, z = 0 },
	heroStrength = 1,
	armyStrength = 100
}
local escapePathSlow = {
	targetHero = escapingHero,
	tile = { x = 1, y = 0, z = 0 },
	nodes = {},
	turn = 0,
	exchangeCount = 1,
	heroArmy = { armyStrength = 1000 },
	totalDanger = 0,
	movementCost = 5
}
local escapePathFast = {
	targetHero = escapingHero,
	tile = { x = 2, y = 0, z = 0 },
	nodes = {},
	turn = 0,
	exchangeCount = 1,
	heroArmy = { armyStrength = 1000 },
	totalDanger = 0,
	movementCost = 2
}
local escapeTasks = EscapeBehavior.new():decompose({
	heroesInfo = { escapingHero },
	settings = { safeAttackRatio = 1.1 },
	dangerHitMap = {
		getTileThreat = function(_, tile)
			if tile.x == 0 then
				return { fastestDanger = { turn = 0, danger = 500, threat = 500 } }
			end
			if tile.x == 1 then
				return { fastestDanger = { turn = 2, danger = 0, threat = 100 } }
			end
			return { fastestDanger = { turn = 2, danger = 0, threat = 200 } }
		end
	},
	escapePaths = {
		escapePathSlow,
		escapePathFast
	}
})
assert(#escapeTasks == 1)
assert(escapeTasks[1].goalType == AbstractGoal.EGoals.EXECUTE_HERO_CHAIN)
assert(escapeTasks[1].chainPath == escapePathFast)

local captureHero = {
	id = 501,
	name = "Collector",
	owner = 1,
	role = PriorityEvaluator.HeroRole.MAIN,
	heroStrength = 1,
	armyStrength = 1000
}
local captureObject = {
	id = 502,
	ID = "MINE",
	typeName = "Mine",
	visitablePos = { x = 3, y = 3, z = 0 }
}
local capturePathFast = {
	targetHero = captureHero,
	tile = captureObject.visitablePos,
	nodes = {},
	turn = 0,
	exchangeCount = 0,
	totalDanger = 0,
	heroArmy = { armyStrength = 1000 },
	movementCost = 2
}
local capturePathSlow = {
	targetHero = captureHero,
	tile = captureObject.visitablePos,
	nodes = {},
	turn = 0,
	exchangeCount = 0,
	totalDanger = 0,
	heroArmy = { armyStrength = 1000 },
	movementCost = 4
}
local visitGoals = CaptureObjectsBehavior.getVisitGoals(
	{ capturePathFast, capturePathSlow },
	{ playerID = 1, settings = { safeAttackRatio = 1.1 } },
	captureObject)
assert(#visitGoals == 2)
assert(visitGoals[1].goalType == AbstractGoal.EGoals.EXECUTE_HERO_CHAIN)
assert(visitGoals[1].closestWayRatio == 1)
assert(visitGoals[2].closestWayRatio == 0.5)

local blockedSubGoal = BuildingBehavior.new()
local blockedGoals = CaptureObjectsBehavior.getVisitGoals(
	{
		{
			targetHero = captureHero,
			tile = captureObject.visitablePos,
			nodes = {},
			turn = 0,
			exchangeCount = 0,
			totalDanger = 0,
			firstBlockedAction = {
				decompose = function()
					return blockedSubGoal
				end
			}
		}
	},
	{ playerID = 1 },
	captureObject)
assert(#blockedGoals == 1)
assert(blockedGoals[1].goalType == AbstractGoal.EGoals.COMPOSITION)
local blockedSequence = blockedGoals[1]:decompose({})
assert(blockedSequence[1].goalType == AbstractGoal.EGoals.EXECUTE_HERO_CHAIN)
assert(blockedSequence[2] == blockedSubGoal)

local captureBehavior = CaptureObjectsBehavior.new():ofType("MINE")
local captureTasks = captureBehavior:decompose({
	playerID = 1,
	settings = { safeAttackRatio = 1.1 },
	visitableObjects = {
		captureObject,
		{ id = 503, ID = "BOAT", visitablePos = { x = 0, y = 0, z = 0 }, paths = { capturePathFast } }
	},
	pathfinder = {
		calculatePathInfo = function(_, paths, tile)
			assert(tile == captureObject.visitablePos)
			paths[1] = capturePathFast
		end
	}
})
assert(#captureTasks == 1)
assert(captureTasks[1].goalType == AbstractGoal.EGoals.EXECUTE_HERO_CHAIN)
assert(CaptureObjectsBehavior.new():equals(CaptureObjectsBehavior.new()) == false)

local clusterHero = {
	id = 601,
	name = "Breaker",
	owner = 1,
	role = PriorityEvaluator.HeroRole.MAIN,
	heroStrength = 1,
	armyStrength = 1000
}
local clusterBlocker = {
	id = 602,
	ID = "MONSTER",
	objectName = "Guard",
	visitablePos = { x = 2, y = 2, z = 0 },
	shouldVisit = true
}
local clusterCenter = {
	id = 603,
	visitablePos = { x = 4, y = 4, z = 0 }
}
local clusterPath = {
	targetHero = clusterHero,
	tile = clusterCenter.visitablePos,
	blocker = clusterBlocker,
	nodes = {
		{ coord = clusterCenter.visitablePos, targetHero = clusterHero, parentIndex = 1 },
		{ coord = clusterBlocker.visitablePos, targetHero = clusterHero, parentIndex = 0 }
	},
	turn = 0,
	exchangeCount = 0,
	totalDanger = 0,
	heroArmy = { armyStrength = 1000 },
	movementCost = 3
}
local cluster = {
	blocker = clusterBlocker,
	center = clusterCenter
}
local unlockCluster = UnlockCluster.new(cluster, clusterPath)
assert(unlockCluster:equals(UnlockCluster.new(cluster, clusterPath)) == true)
assert(unlockCluster:toString() == "Unlock Cluster Guard(2 2 0)")
local clusterTasks = ClusterBehavior.new():decompose({
	playerID = 1,
	settings = { safeAttackRatio = 1.1 },
	lockedClusters = { cluster },
	pathfinder = {
		getPathInfo = function(_, tile)
			assert(tile == clusterCenter.visitablePos)
			return { clusterPath }
		end
	},
	objectClusterizer = {
		getBlocker = function(_, path)
			return path.blocker
		end
	}
})
assert(#clusterTasks == 1)
assert(clusterTasks[1].goalType == AbstractGoal.EGoals.COMPOSITION)
local clusterSequence = clusterTasks[1]:decompose({})
assert(clusterSequence[1].goalType == AbstractGoal.EGoals.UNLOCK_CLUSTER)
assert(clusterSequence[2].goalType == AbstractGoal.EGoals.EXECUTE_HERO_CHAIN)
assert(clusterSequence[2].tile == clusterBlocker.visitablePos)

local explorationPoint = ExplorationPoint.new({ x = 7, y = 8, z = 0 }, 9)
assert(explorationPoint:equals(ExplorationPoint.new({ x = 7, y = 8, z = 0 }, 9)) == false)
assert(explorationPoint:toString() == "Explore (7 8 0) for 9 tiles")

local dimensionDoorEvaluation = ExplorationHelper.evaluateDimensionDoorExplorationCandidate({
	visible = true,
	tilesDiscovered = 3,
	continuationTilesDiscovered = 2,
	chainTilesDiscovered = 1,
	strategicScore = 1,
	reachableWithoutDimensionDoor = false,
	dimensionDoorTriggersGuards = true,
	guardedLandingDanger = 0,
	guardedLandingSafe = true,
	movementPointsRemaining = 600,
	movementPointsLimit = 1200,
	movementPointsTaken = 300,
	currentBestValue = 0
})
assert(dimensionDoorEvaluation.accepted == true)
assert(dimensionDoorEvaluation.value == 156)
assert(dimensionDoorEvaluation.tilesDiscovered == 4)
assert(ExplorationHelper.evaluateDimensionDoorExplorationCandidate({
	tilesDiscovered = 1,
	reachableWithoutDimensionDoor = true
}).accepted == false)

local explorationObject = {
	id = 701,
	ID = "MINE",
	typeName = "Mine",
	visitablePos = { x = 3, y = 4, z = 0 }
}
local explorationHero = {
	id = 702,
	name = "Scout",
	explorationHelper = {
		scanSector = {
			[1] = {
				bestGoal = CaptureObject.new(explorationObject),
				bestTile = { x = 3, y = 4, z = 0 },
				bestTilesDiscovered = 5
			}
		}
	}
}
local boatObject = {
	id = 703,
	ID = "BOAT",
	typeName = "Boat",
	available = true,
	hiddenTilesDiscovered = 6,
	visitablePos = { x = 5, y = 5, z = 0 }
}
local explorationTasks = ExplorationBehavior.new():decompose({
	visitableObjects = { boatObject },
	heroesInfo = { explorationHero }
})
assert(#explorationTasks == 2)
local boatSequence = explorationTasks[1]:decompose({})
assert(boatSequence[1].goalType == AbstractGoal.EGoals.EXPLORATION_POINT)
assert(boatSequence[1].value == 6)
assert(boatSequence[2].goalType == AbstractGoal.EGoals.CAPTURE_OBJECT)
local scanSequence = explorationTasks[2]:decompose({})
assert(scanSequence[1].goalType == AbstractGoal.EGoals.EXPLORATION_POINT)
assert(scanSequence[2].goalType == AbstractGoal.EGoals.CAPTURE_OBJECT)
assert(scanSequence[3].goalType == AbstractGoal.EGoals.EXPLORE_NEIGHBOUR_TILE)

local neighbourHero = {
	id = 704,
	name = "Walker",
	neighbourExplorationCandidates = {
		{ tile = { x = 8, y = 8, z = 0 }, sameDay = true, accessible = true, safe = true, tilesDiscovered = 4, movementCost = 1 }
	}
}
local neighbourTasks = ExplorationBehavior.new():decompose({
	heroesInfo = { neighbourHero },
	scanDepth = State.ScanDepth.ALL_FULL
})
assert(#neighbourTasks == 1)
local neighbourSequence = neighbourTasks[1]:decompose({})
assert(neighbourSequence[1].goalType == AbstractGoal.EGoals.EXPLORATION_POINT)
assert(neighbourSequence[2].goalType == AbstractGoal.EGoals.EXPLORE_NEIGHBOUR_TILE)

local exchangeTargetHero = { id = 801, name = "Main", armyStrength = 1000 }
local exchangePath = {
	name = "path-to-main",
	heroArmy = { armyStrength = 2000 },
	reinforcementArmyStrength = 750
}
local heroExchange = HeroExchange.new(exchangeTargetHero, exchangePath)
assert(heroExchange:equals(HeroExchange.new(exchangeTargetHero, exchangePath)) == false)
assert(heroExchange:toString() == "Hero exchange for Main by path-to-main")
assert(heroExchange:getReinforcementArmyStrength({}) == 750)

local upgrader = { id = 802, objectName = "Castle", visitablePos = { x = 1, y = 2, z = 0 } }
local armyUpgrade = ArmyUpgrade.new(
	{ targetHero = exchangeTargetHero, heroArmy = { armyStrength = 1500 } },
	upgrader,
	{ upgradeValue = 5000, upgradeCost = { [6] = 1200 } })
assert(armyUpgrade:equals(ArmyUpgrade.new(exchangeTargetHero, upgrader, {})) == false)
assert(armyUpgrade:getUpgradeValue() == 5000)
assert(armyUpgrade:getInitialArmyValue() == 1500)
assert(armyUpgrade:toString() == "Army upgrade at Castle(1 2 0)")

local gatherBehavior = GatherArmyBehavior.new()
assert(gatherBehavior:toString() == "Gather army")
assert(gatherBehavior:equals(GatherArmyBehavior.new()) == true)

local receiverHero = {
	id = 901,
	name = "Receiver",
	owner = 1,
	role = PriorityEvaluator.HeroRole.MAIN,
	evaluateHeroScore = 100,
	armyStrength = 5000,
	visitablePos = { x = 1, y = 1, z = 0 }
}
local donorHero = {
	id = 902,
	name = "Donor",
	owner = 1,
	role = PriorityEvaluator.HeroRole.SCOUT,
	evaluateHeroScore = 10,
	heroStrength = 1
}
local gatherPath = {
	targetHero = donorHero,
	tile = receiverHero.visitablePos,
	nodes = { { targetHero = donorHero } },
	turn = 0,
	exchangeCount = 0,
	totalDanger = 0,
	heroArmy = { armyStrength = 4000 },
	reinforcementArmyStrength = 1200,
	movementCost = 2
}
local gatherTasks = gatherBehavior:deliverArmyToHero({
	playerID = 1,
	settings = { safeAttackRatio = 1.1 },
	heroManager = {},
	pathfinder = {
		getPathInfo = function()
			return { gatherPath }
		end
	}
}, receiverHero)
assert(#gatherTasks == 1)
local gatherSequence = gatherTasks[1]:decompose({})
assert(gatherSequence[1].goalType == AbstractGoal.EGoals.HERO_EXCHANGE)
assert(gatherSequence[2].goalType == AbstractGoal.EGoals.EXECUTE_HERO_CHAIN)

local upgradeTown = {
	id = 903,
	name = "Castle",
	ID = "TOWN",
	visitablePos = { x = 4, y = 4, z = 0 },
	shouldVisit = true
}
local upgradePath = {
	targetHero = receiverHero,
	tile = upgradeTown.visitablePos,
	nodes = {},
	turn = 0,
	exchangeCount = 0,
	totalDanger = 0,
	heroArmy = { armyStrength = 10000 },
	heroStrength = 10000,
	upgrade = { upgradeValue = 5000, upgradeCost = { [6] = 1000 } },
	movementCost = 1
}
local upgradeTasks = gatherBehavior:upgradeArmy({
	playerID = 1,
	settings = { safeAttackRatio = 1.1, scoutHeroTurnDistanceLimit = 2 },
	pathfinder = {
		getPathInfo = function()
			return { upgradePath }
		end
	}
}, upgradeTown)
assert(#upgradeTasks == 1)
local upgradeSequence = upgradeTasks[1]:decompose({})
assert(upgradeSequence[1].goalType == AbstractGoal.EGoals.ARMY_UPGRADE)
assert(upgradeSequence[2].goalType == AbstractGoal.EGoals.EXECUTE_HERO_CHAIN)

local defendTown = {
	id = 1001,
	name = "Town",
	armyStrength = 500,
	fortLevel = 2,
	visitablePos = { x = 1, y = 1, z = 0 }
}
local defendHero = {
	id = 1002,
	name = "Defender",
	owner = 1,
	totalStrength = 2000,
	armyStrength = 2000,
	heroStrength = 2000,
	canMergeWithTown = true
}
local defendThreat = {
	danger = 1000,
	turn = 0,
	hero = { id = 1003 },
	verified = true
}
assert(DefenceBehavior.estimateTownFortificationDefence(defendTown, true) == 4000)
assert(DefenceBehavior.isTownDefenceSufficient(1000, defendThreat, 1.1) == true)
assert(DefenceBehavior.shouldLockTownDefender(defendTown, defendHero, defendThreat, 1.1) == true)
assert(DefenceBehavior.isHeroRequiredForTownDefence(defendTown, defendHero, { defendThreat }, 1.1) == true)
local defendMarker = DefendTown.new(defendTown, defendThreat, defendHero)
assert(defendMarker:equals(DefendTown.new(defendTown, defendThreat, defendHero)) == false)
assert(defendMarker:toString() == "Defend town Town")

local threatenedTown = {
	id = 1004,
	name = "Threatened",
	armyStrength = 100,
	fortLevel = 0,
	visitablePos = { x = 5, y = 5, z = 0 },
	threatNode = {
		fastestDanger = defendThreat,
		maximumDanger = defendThreat
	},
	threats = {}
}
local defencePath = {
	targetHero = defendHero,
	tile = threatenedTown.visitablePos,
	nodes = {},
	turn = 0,
	exchangeCount = 0,
	totalDanger = 0,
	heroArmy = { armyStrength = 2000 },
	heroStrength = 2000,
	movementCost = 2
}
local defenceTasks = DefenceBehavior.new():decompose({
	playerID = 1,
	settings = { safeAttackRatio = 1.1 },
	townsInfo = { threatenedTown },
	pathfinder = {
		getPathInfo = function()
			return { defencePath }
		end
	}
})
assert(#defenceTasks == 1)
local defenceSequence = defenceTasks[1]:decompose({})
assert(defenceSequence[1].goalType == AbstractGoal.EGoals.DEFEND_TOWN)
assert(defenceSequence[2].goalType == AbstractGoal.EGoals.EXECUTE_HERO_CHAIN)
assert(defenceTasks[1].priority == DefenceBehavior.DEFENSIVE_EMERGENCY_PRIORITY)
