local AbstractGoal = require("Goals.AbstractGoal")
local AdventureSpellCast = require("Goals.AdventureSpellCast")
local BuildBoat = require("Goals.BuildBoat")
local BuildThis = require("Goals.BuildThis")
local BuyArmy = require("Goals.BuyArmy")
local DigAtTile = require("Goals.DigAtTile")
local DismissHero = require("Goals.DismissHero")
local Goals = require("Goals.Goals")
local Invalid = require("Goals.Invalid")
local RecruitHero = require("Goals.RecruitHero")
local SaveResources = require("Goals.SaveResources")
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
assert(BuyArmy.needsFreeSlotToRecruit({ stacksCount = 6, armySize = 7, slotsByCreature = {} }, 3) == false)

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
assert(Goals.RecruitHero == RecruitHero)
assert(Goals.SaveResources == SaveResources)
