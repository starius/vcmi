local AbstractGoal = require("Goals.AbstractGoal")
local BuildThis = require("Goals.BuildThis")
local BuyArmy = require("Goals.BuyArmy")
local Goals = require("Goals.Goals")
local Invalid = require("Goals.Invalid")
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
