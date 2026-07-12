local ArmyFormation = require("Helpers.ArmyFormation")
local HostCommands = require("Actions.HostCommands")

local splitCalls = {}
local splitAdapter = {
	splitStack = function(_, source, destination, fromSlot, toSlot, count)
		table.insert(splitCalls, {
			source = source.id,
			destination = destination.id,
			fromSlot = fromSlot,
			toSlot = toSlot,
			count = count
		})
	end
}

local splitHero = {
	id = 10,
	armySize = 4,
	slots = {
		{ slot = 0, count = 5, creature = { id = 1, aiValue = 20 } },
		{ slot = 1, count = 2, creature = { id = 2, aiValue = 5 } }
	}
}

ArmyFormation.addSingleCreatureStacks(splitAdapter, splitHero)
assert(#splitCalls == 2)
assert(splitCalls[1].fromSlot == 1)
assert(splitCalls[1].toSlot == 3)
assert(splitCalls[2].fromSlot == 0)
assert(splitCalls[2].toSlot == 2)
assert(#splitHero.slots == 4)

local siegeCalls = {}
local siegeAdapter = HostCommands.new({
	command = function(_, name, payload)
		table.insert(siegeCalls, { name = name, payload = payload })
		return { ok = true, executed = true }
	end
})

local siegeHero = {
	id = 20,
	owner = 1,
	armySize = 3,
	slots = {
		{ slot = 0, count = 1, creature = { id = 11, aiValue = 50, flying = true } },
		{ slot = 1, count = 1, creature = { id = 12, aiValue = 30, flying = false } },
		{ slot = 2, count = 1, creature = { id = 13, aiValue = 10, flying = false } }
	}
}
local enemyCastle = {
	id = 30,
	isTown = true,
	owner = 2,
	fortLevel = ArmyFormation.FortLevel.CASTLE
}

local rearranged = siegeAdapter:rearrangeArmyForSiege(enemyCastle, siegeHero, { turn = 0 })
assert(rearranged.executed == true)
assert(#siegeCalls == 1)
assert(siegeCalls[1].name == "swapCreatures")
assert(siegeCalls[1].payload.src == 20)
assert(siegeCalls[1].payload.dst == 20)
assert(siegeCalls[1].payload.fromSlot == 0)
assert(siegeCalls[1].payload.toSlot == 2)
assert(siegeHero.slots[1].slot == 2)
assert(siegeHero.slots[3].slot == 0)

local skippedCalls = {}
local skippedAdapter = HostCommands.new({
	command = function(_, name, payload)
		table.insert(skippedCalls, { name = name, payload = payload })
		return { ok = true, executed = true }
	end
})
local skipped = skippedAdapter:rearrangeArmyForSiege(enemyCastle, siegeHero, { turn = 1 })
assert(skipped.skipped == true)
assert(#skippedCalls == 0)
