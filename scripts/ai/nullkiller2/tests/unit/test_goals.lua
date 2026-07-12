local AbstractGoal = require("Goals.AbstractGoal")
local AdventureSpellCast = require("Goals.AdventureSpellCast")
local BuildBoat = require("Goals.BuildBoat")
local BuildThis = require("Goals.BuildThis")
local BuyArmy = require("Goals.BuyArmy")
local CaptureObject = require("Goals.CaptureObject")
local DigAtTile = require("Goals.DigAtTile")
local DismissHero = require("Goals.DismissHero")
local ExchangeSwapTownHeroes = require("Goals.ExchangeSwapTownHeroes")
local ExecuteHeroChain = require("Goals.ExecuteHeroChain")
local ExploreNeighbourTile = require("Goals.ExploreNeighbourTile")
local Goals = require("Goals.Goals")
local Invalid = require("Goals.Invalid")
local RecruitHero = require("Goals.RecruitHero")
local SaveResources = require("Goals.SaveResources")
local State = require("Engine.State")
local StayAtTown = require("Goals.StayAtTown")
local Trade = require("Goals.Trade")

assert(AbstractGoal.EGoals.INVALID == -1)
assert(AbstractGoal.EGoals.BUILD_STRUCTURE == 8)
assert(AbstractGoal.EGoals.EXPLORE_NEIGHBOUR_TILE == 37)

local base = AbstractGoal.new(AbstractGoal.EGoals.DIG_AT_TILE)
base:settile({ x = 1, y = 2, z = 0 }):sethero({ id = 7, name = "Sir Mullich" })
assert(base:toString() == "DIG AT TILE (1 2 0) (Sir Mullich)")
assert(base:invalid() == false)
assert(base:isElementar() == false)

local ok, err = pcall(function()
	AbstractGoal.taskptr(base)
end)
assert(ok == false)
assert(string.find(err, "is not elementar", 1, true) ~= nil)

local invalid = Invalid.new()
assert(invalid:invalid() == true)
assert(invalid:isElementar() == true)
assert(invalid.priority == -1)
assert(invalid:toString() == "Invalid")
assert(AbstractGoal.sptr(invalid):equals(Invalid.new()) == true)

local tradeA = Trade.new(6, 2500, 44)
local tradeB = Trade.new({ num = 6 }, 1, 99)
local tradeC = Trade.new(0, 2500, 44)
assert(tradeA:equals(tradeB) == true)
assert(tradeA:equals(tradeC) == false)
assert(tradeA:toString() == "TRADE 2500 of gold at objid 44")

local town = {
	id = 101,
	name = "Castle Black",
	buildings = {
		[5] = { name = "Tavern" }
	}
}

local buildA = BuildThis.new(5, town)
local buildB = BuildThis.new({ id = { num = 5 }, name = "Tavern" }, { town = town })
local buildC = BuildThis.new(6, town)
assert(buildA:equals(buildB) == true)
assert(buildA:equals(buildC) == false)
assert(buildA:toString() == "Build Tavern in Castle Black")

local buyArmy = BuyArmy.new(town, 1200):setpriority(4.5)
assert(buyArmy.priority == 4.5)
assert(buyArmy:toString() == "Buy army at Castle Black")
assert(buyArmy:isObjectAffected(101) == true)
assert(buyArmy:isObjectAffected(102) == false)

local affected = buyArmy:getAffectedObjects()
assert(#affected == 1)
assert(affected[1] == 101)

local task = AbstractGoal.taskptr(buyArmy)
assert(task:isElementar() == true)
assert(task:getHeroExchangeCount() == 0)
assert(task:toString() == "Buy army at Castle Black")

assert(BuyArmy.needsFreeSlotToRecruit({ stacksCount = 7, armySize = 7, slotsByCreature = {} }, 3) == true)
assert(BuyArmy.needsFreeSlotToRecruit({ stacksCount = 7, armySize = 7, slotsByCreature = { [3] = 1 } }, 3) == false)
assert(BuyArmy.needsFreeSlotToRecruit({ stacksCount = 7, armySize = 7, slotsByCreature = { ["3"] = 1 } }, 3) == false)
assert(BuyArmy.needsFreeSlotToRecruit({ stacksCount = 6, armySize = 7, slotsByCreature = {} }, 3) == false)

town.factionID = 1
town.upperArmy = {
	id = 104,
	stacksCount = 0,
	armySize = 7,
	slotsByCreature = {}
}
town.availableToBuy = {
	{
		creature = { id = 105, aiValue = 10, factionID = 1, fullRecruitCost = { [7] = 50 } },
		count = 10,
		level = 0
	},
	{
		creature = { id = 106, aiValue = 100, factionID = 1, fullRecruitCost = { [7] = 200 } },
		count = 10,
		level = 1
	}
}
local buyArmyJournal = {}
local buyArmyResult = BuyArmy.new(town, 80):accept({
	freeResources = { [7] = 1000 },
	getFreeResources = function(self)
		return self.freeResources
	end,
	recruitCreatures = function(_, sourceTown, destination, creature, count, level)
		table.insert(buyArmyJournal, {
			town = sourceTown,
			destination = destination,
			creature = creature,
			count = count,
			level = level
		})
	end
})
assert(buyArmyResult.valueBought == 500)
assert(#buyArmyJournal == 1)
assert(buyArmyJournal[1].town == town)
assert(buyArmyJournal[1].destination == town.upperArmy)
assert(buyArmyJournal[1].creature.id == 106)
assert(buyArmyJournal[1].count == 5)
assert(buyArmyJournal[1].level == 1)

local movementTown = {
	id = 130,
	name = "Movement Town",
	visitablePos = { x = 4, y = 5, z = 0 },
	visitingHero = { id = 131, name = "Visitor" },
	upperArmy = { id = 132, stacksCount = 0, armySize = 7 },
	availableToBuy = {
		{
			creature = { id = 133, aiValue = 100, factionID = 1, fullRecruitCost = { [7] = 100 } },
			count = 1,
			level = 0
		}
	}
}
local movedHero = nil
local movedTile = nil
BuyArmy.new(movementTown, 100):accept({
	freeResources = { [7] = 1000 },
	recruitCreatures = function() end,
	moveHeroToTile = function(_, tile, hero)
		movedTile = tile
		movedHero = hero
	end
})
assert(movedHero == movementTown.visitingHero)
assert(movedTile == movementTown.visitablePos)

local fullTown = {
	id = 201,
	name = "Full Town",
	factionID = 1,
	upperArmy = {
		id = 202,
		stacksCount = 7,
		armySize = 7,
		slotsByCreature = {
			["205"] = 3
		},
		slots = {
			{
				slot = 3,
				count = 1,
				marketValue = 1,
				creature = { id = 205, factionID = 2, aiValue = 1 }
			}
		}
	},
	availableToBuy = {
		{
			creature = { id = 206, aiValue = 100, factionID = 1, fullRecruitCost = { [7] = 100 } },
			count = 2,
			level = 2
		}
	}
}
local dismissSlot = nil
local recruitedCreature = nil
BuyArmy.new(fullTown, 100):accept({
	freeResources = { [7] = 1000 },
	dismissCreature = function(_, army, slot)
		assert(army == fullTown.upperArmy)
		dismissSlot = slot
	end,
	recruitCreatures = function(_, _, destination, creature)
		assert(destination == fullTown.upperArmy)
		recruitedCreature = creature.id
	end
})
assert(dismissSlot == 3)
assert(recruitedCreature == 206)

local upgradeOnlyTown = {
	id = 301,
	name = "Upgrade Town",
	upperArmy = {
		id = 302,
		upgradeSlots = {
			{
				slot = 4,
				stack = { count = 10, aiValue = 10 },
				upgradeInfo = {
					availableUpgrades = {
						{ id = 303, aiValue = 30, cost = { gold = 20 } }
					}
				}
			}
		}
	},
	availableToBuy = {}
}
local upgraded = nil
local upgradeOnlyResult = BuyArmy.new(upgradeOnlyTown, 100):accept({
	freeResources = { gold = 1000 },
	upgradeCreature = function(_, army, slot, creature)
		upgraded = {
			army = army,
			slot = slot,
			creature = creature
		}
	end
})
assert(upgradeOnlyResult.valueBought == 0)
assert(upgradeOnlyResult.upgradeSuccessful == true)
assert(upgraded.army == upgradeOnlyTown.upperArmy)
assert(upgraded.slot == 4)
assert(upgraded.creature.id == 303)

local exchangeTown = {
	id = 330,
	name = "Exchange Town",
	visitablePos = { x = 7, y = 8, z = 0 },
	visitingHero = {
		id = 331,
		name = "Visitor",
		upgradeSlots = {
			{
				slot = 2,
				stack = { count = 3, aiValue = 5 },
				upgradeInfo = {
					availableUpgrades = {
						{ id = 332, aiValue = 9, cost = { gold = 10 } }
					}
				}
			}
		}
	},
	upperArmy = { id = 333, stacksCount = 0 },
	upgradeSlots = {
		{
			slot = 1,
			stack = { count = 5, aiValue = 10 },
			upgradeInfo = {
				availableUpgrades = {
					{ id = 334, aiValue = 20, cost = { gold = 10 } }
				}
			}
		}
	}
}
local targetGarrison = { id = 335, name = "Defender" }
local exchangeLog = {}
ExchangeSwapTownHeroes.new(exchangeTown, targetGarrison, State.HeroLockedReason.DEFENCE):accept({
	freeResources = { gold = 1000 },
	swapGarrisonHero = function(_, townArg)
		table.insert(exchangeLog, "swap:" .. townArg.id)
	end,
	upgradeCreature = function(_, army, slot, creature)
		table.insert(exchangeLog, "upgrade:" .. army.id .. ":" .. slot .. ":" .. creature.id)
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
assert(exchangeLog[1] == "swap:330")
assert(exchangeLog[2] == "upgrade:330:1:334")
assert(exchangeLog[3] == "move:335:330:7")
assert(exchangeLog[4] == "swap:330")
assert(exchangeLog[5] == "lock:335:" .. State.HeroLockedReason.DEFENCE)
assert(exchangeLog[6] == "unlock:331")
assert(exchangeLog[7] == "upgrade:331:2:332")

assert(Goals.Invalid == Invalid)
assert(Goals.BuildThis == BuildThis)

local digA = DigAtTile.new({ x = 4, y = 5, z = 0 }):sethero({ id = 11, name = "Gem" })
local digB = DigAtTile.new({ x = 4, y = 5, z = 0 }):sethero(digA.hero)
local digC = DigAtTile.new({ x = 4, y = 6, z = 0 }):sethero(digA.hero)
assert(digA:equals(digB) == true)
assert(digA:equals(digC) == false)
assert(digA:toString() == "DIG AT TILE (4 5 0) (Gem)")

local shipyard = { id = 202 }
local buildBoat = BuildBoat.new(shipyard)
assert(buildBoat:equals(BuildBoat.new(shipyard)) == true)
assert(buildBoat:equals(BuildBoat.new({ id = 202 })) == false)
assert(buildBoat:toString() == "BuildBoat")
local builtBoatAt = nil
BuildBoat.new({ id = 203, boatCost = { [7] = 500 }, shipyardStatus = 0, relation = "ALLIES" }):accept({
	freeResources = { [7] = 1000 },
	getFreeResources = function(self)
		return self.freeResources
	end,
	buildBoat = function(_, targetShipyard)
		builtBoatAt = targetShipyard.id
	end
})
assert(builtBoatAt == 203)
local okBoat, boatError = pcall(function()
	BuildBoat.new({ id = 204, boatCost = { [7] = 500 }, shipyardStatus = 0 }):accept({
		freeResources = { [7] = 499 },
		getFreeResources = function(self)
			return self.freeResources
		end,
		buildBoat = function()
			error("should not build")
		end
	})
end)
assert(okBoat == false)
assert(string.find(boatError, "Can not afford boat", 1, true) ~= nil)
okBoat, boatError = pcall(function()
	BuildBoat.new({ id = 205, enemy = true, shipyardStatus = 0 }):accept({
		buildBoat = function()
			error("should not build")
		end
	})
end)
assert(okBoat == false)
assert(string.find(boatError, "enemy shipyard", 1, true) ~= nil)
okBoat, boatError = pcall(function()
	BuildBoat.new({ id = 206, shipyardStatus = 1 }):accept({
		buildBoat = function()
			error("should not build")
		end
	})
end)
assert(okBoat == false)
assert(string.find(boatError, "Shipyard is busy.", 1, true) ~= nil)

local heroA = { id = 301, name = "Aine", totalStrength = 10, mana = 20 }
local heroB = { id = 302, name = "Dessa", totalStrength = 30 }
local recruitTown = { id = 303, name = "Rampart", availableHeroes = { heroA, heroB } }
local recruitAny = RecruitHero.new(recruitTown)
assert(recruitAny.priority == 1)
assert(recruitAny:toString() == "Recruit hero at Rampart")
local intent = recruitAny:accept({})
assert(intent.action == "recruitHero")
assert(intent.hero == heroB)

local recruitSpecific = RecruitHero.new(recruitTown, heroA)
assert(recruitSpecific:getHero() == heroA)
assert(recruitSpecific:toString() == "Recruit Aine at Rampart")
assert(recruitSpecific:equals(RecruitHero.new({ id = 1 }, { id = 2 })) == true)

local spell = { id = 401, name = "Town Portal", cost = 16, isAdventure = true }
local spellCast = AdventureSpellCast.new(heroA, spell):settile({ x = 7, y = 8, z = 0 })
local sameSpellCast = AdventureSpellCast.new(heroA, spell):settile({ x = 7, y = 8, z = 0 })
local otherSpellCast = AdventureSpellCast.new(heroA, spell):settile({ x = 7, y = 9, z = 0 })
assert(spellCast:getSpell() == spell)
assert(spellCast:equals(sameSpellCast) == true)
assert(spellCast:equals(otherSpellCast) == false)
assert(spellCast:toString() == "AdventureSpellCast Town Portal")
local castIntent = spellCast:accept({})
assert(castIntent.action == "castSpell")
assert(castIntent.hero == heroA)

local portalHero = {
	id = 405,
	name = "Town Portal Caster",
	mana = 30,
	garrisoned = true,
	visitedTown = { id = 406, name = "Visited Tower" }
}
local portalTown = {
	id = 407,
	name = "Portal Castle",
	owner = 1,
	visitablePos = { x = 4, y = 5, z = 0 },
	visitingHero = { id = 408, name = "Town Visitor" },
	upperArmy = { stacksCount = 0 }
}
local portalLog = {}
local portalGoal = AdventureSpellCast.new(portalHero, {
	id = 409,
	name = "Town Portal",
	cost = 16,
	isAdventure = true,
	townPortal = true
}):settown(portalTown):settile({ x = 4, y = 5, z = 0 })
portalGoal:accept({
	playerID = 1,
	setTargetObject = function(_, townArg)
		table.insert(portalLog, "target:" .. townArg.id)
	end,
	swapGarrisonHero = function(_, townArg)
		table.insert(portalLog, "swap:" .. townArg.id)
	end,
	castSpell = function(_, hero, spellArg, tileArg)
		table.insert(portalLog, "cast:" .. hero.id .. ":" .. spellArg.id .. ":" .. tileArg.x)
	end,
	executeHeroChain = function(_, path, objid)
		table.insert(portalLog, "move:" .. path.targetHero.id .. ":" .. objid .. ":" .. path.targetTile.x)
	end
})
assert(portalLog[1] == "target:407")
assert(portalLog[2] == "swap:407")
assert(portalLog[3] == "swap:406")
assert(portalLog[4] == "cast:405:409:4")
assert(portalLog[5] == "move:405:407:4")

local blockedPortal = AdventureSpellCast.new(
	{ id = 410, name = "Blocked Caster", mana = 30 },
	{ id = 411, name = "Town Portal", cost = 16, isAdventure = true, townPortal = true }
):settown({
	id = 412,
	name = "Blocked Town",
	owner = 1,
	visitingHero = { id = 413, name = "Blocker" },
	upperArmy = { stacksCount = 1 }
})
local okPortal, portalError = pcall(function()
	blockedPortal:accept({ playerID = 1, castSpell = function() end })
end)
assert(okPortal == false)
assert(string.find(portalError, "already occupied", 1, true) ~= nil)

local saveResources = SaveResources.new({ 1, 2, 3, 4, 5, 6, 7 })
assert(saveResources:equals(SaveResources.new({ 0, 0, 0, 0, 0, 0, 0 })) == true)
assert(saveResources:toString() == "SaveResources [1, 2, 3, 4, 5, 6, 7]")
local locked = nil
saveResources:accept({
	lockResources = function(_, resources)
		locked = resources
	end
})
assert(locked == saveResources.resources)

local dismiss = DismissHero.new(heroA)
assert(dismiss:equals(DismissHero.new(heroA)) == true)
assert(dismiss:equals(DismissHero.new(heroB)) == false)
assert(dismiss:toString() == "DismissHero Aine")
assert(dismiss:accept({}).action == "dismissHero")

assert(Goals.AdventureSpellCast == AdventureSpellCast)
assert(Goals.BuildBoat == BuildBoat)
assert(Goals.DigAtTile == DigAtTile)
assert(Goals.DismissHero == DismissHero)
assert(Goals.ExploreNeighbourTile == ExploreNeighbourTile)
assert(Goals.RecruitHero == RecruitHero)
assert(Goals.SaveResources == SaveResources)

local object = { id = 800, typeName = "Mine", visitablePos = { x = 2, y = 3, z = 0 } }
local capture = CaptureObject.new(object)
assert(capture:equals(CaptureObject.new(object)) == true)
assert(capture:hasHash() == true)
assert(capture:getHash() == 800)
assert(capture:toString() == "Capture Mine at (2 3 0)")

local candidate = ExploreNeighbourTile.evaluateNeighbourExplorationCandidate({
	sameDay = true,
	accessible = true,
	safe = true,
	tilesDiscovered = 4,
	movementCost = 2
})
assert(candidate.accepted == true)
assert(candidate.value == 8)
assert(ExploreNeighbourTile.evaluateNeighbourExplorationCandidate({
	sameDay = false,
	accessible = true,
	safe = true,
	tilesDiscovered = 4,
	movementCost = 2
}).accepted == false)
assert(ExploreNeighbourTile.evaluateTileScore(4, 2) == 2)

local explorer = {
	id = 801,
	name = "Scout",
	movementPointsRemaining = 100,
	neighbourExplorationCandidates = {
		{ tile = { x = 1, y = 0, z = 0 }, sameDay = true, accessible = true, safe = true, tilesDiscovered = 2, movementCost = 1 },
		{ tile = { x = 2, y = 0, z = 0 }, sameDay = true, accessible = true, safe = true, tilesDiscovered = 3, movementCost = 1 }
	}
}
local target = ExploreNeighbourTile.findTarget(explorer, {})
assert(target.tile.x == 2)
local exploreIntent = ExploreNeighbourTile.new(explorer, 1):accept({})
assert(exploreIntent.action == "moveHeroToTile")
assert(exploreIntent.tile.x == 2)

local restTown = { id = 901, name = "Tower" }
local restHero = { id = 902, name = "Solmyr", movementPointsRemaining = 600, movementPointsLimit = 1200, mana = 7, manaLimit = 20 }
local stay = StayAtTown.new(restTown, { targetHero = restHero, movementCost = 0.2 })
assert(stay:equals(StayAtTown.new(restTown, { targetHero = restHero, movementCost = 0 })) == true)
assert(math.abs(stay:getMovementWasted() - 0.3) < 0.0001)
assert(stay:toString() == "Stay at town Tower hero Solmyr, mana: 7 / 20")
local stayIntent = stay:accept({})
assert(stayIntent.action == "lockHero")
assert(stayIntent.hero == restHero)

local chainHero = { id = 950, name = "Crag Hack" }
local helperHero = { id = 951, name = "Logistics" }
local chainPath = {
	targetHero = chainHero,
	tile = { x = 8, y = 9, z = 0 },
	nodes = {
		{ targetHero = helperHero },
		{ targetHero = chainHero }
	},
	chainMask = 17,
	exchangeCount = 1
}
local chain = ExecuteHeroChain.new(chainPath, { id = 952, typeName = "Mine" })
assert(chain:equals(ExecuteHeroChain.new(chainPath, { id = 952, typeName = "Mine" })) == true)
assert(chain:equals(ExecuteHeroChain.new({
	targetHero = chainHero,
	tile = { x = 8, y = 10, z = 0 },
	nodes = chainPath.nodes,
	chainMask = 17
})) == false)
assert(chain:getHeroExchangeCount() == 1)
assert(chain:isObjectAffected(950) == true)
assert(chain:isObjectAffected(951) == true)
assert(chain:isObjectAffected(952) == true)
assert(#chain:getAffectedObjects() == 3)
assert(chain:toString() == "ExecuteHeroChain Mine(8 9 0) by Crag Hack")
local chainIntent = chain:accept({})
assert(chainIntent.action == "executeHeroChain")
assert(chainIntent.hero == chainHero)
assert(chainIntent.objid == 952)
local chainLog = {}
chain:accept({
	setActive = function(_, hero, tile)
		table.insert(chainLog, "active:" .. hero.id .. ":" .. tile.x)
	end,
	setTargetObject = function(_, objid)
		table.insert(chainLog, "target:" .. objid)
	end,
	resetObjectClusterizer = function()
		table.insert(chainLog, "reset")
	end,
	executeHeroChain = function(_, path, objid)
		table.insert(chainLog, "execute:" .. path.targetHero.id .. ":" .. objid)
	end
})
assert(chainLog[1] == "active:950:8")
assert(chainLog[2] == "target:952")
assert(chainLog[3] == "reset")
assert(chainLog[4] == "execute:950:952")
local blockedChain = ExecuteHeroChain.new({
	targetHero = chainHero,
	tile = { x = 10, y = 11, z = 0 },
	nodes = {
		{
			targetHero = helperHero,
			coord = { x = 10, y = 11, z = 0 },
			specialAction = { name = "Dimension Door" },
			actionIsBlocked = true
		}
	}
})
local blockedLog = {}
local okBlocked, blockedError = pcall(function()
	blockedChain:accept({
		setActive = function()
			table.insert(blockedLog, "active")
		end,
		setTargetObject = function()
			table.insert(blockedLog, "target")
		end,
		resetObjectClusterizer = function()
			table.insert(blockedLog, "reset")
		end,
		lockHero = function(_, hero, reason)
			table.insert(blockedLog, "lock:" .. hero.id .. ":" .. reason)
		end,
		invalidatePathfinderData = function()
			table.insert(blockedLog, "invalidate")
		end,
		executeHeroChain = function()
			error("should not execute blocked path")
		end
	})
end)
assert(okBlocked == false)
assert(string.find(blockedError, "Path is nondeterministic.", 1, true) ~= nil)
assert(blockedLog[1] == "active")
assert(blockedLog[2] == "target")
assert(blockedLog[3] == "reset")
assert(blockedLog[4] == "lock:951:3")
assert(blockedLog[5] == "invalidate")

assert(Goals.CaptureObject == CaptureObject)
assert(Goals.ExecuteHeroChain == ExecuteHeroChain)
assert(Goals.StayAtTown == StayAtTown)
