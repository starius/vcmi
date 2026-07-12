local HostCommands = require("Actions.HostCommands")

local called = {}
local host = {
	command = function(_, name, payload)
		table.insert(called, { name = name, payload = payload })
		return { ok = true, executed = false }
	end
}

local adapter = HostCommands.new(host)
local lockedHeroes = {}
local targetObject = nil
local activeHero = nil
local activeTile = nil
local pathfinderInvalidated = false
local objectClusterizerReset = false
adapter.nullkiller = {
	lockHero = function(_, hero, reason)
		lockedHeroes[hero.id] = reason
	end,
	unlockHero = function(_, hero)
		lockedHeroes[hero.id] = nil
	end,
	setTargetObject = function(_, objid)
		targetObject = objid
	end,
	setActive = function(_, hero, tile)
		activeHero = hero.id
		activeTile = tile
	end,
	invalidatePathfinderData = function()
		pathfinderInvalidated = true
	end,
	objectClusterizer = {
		reset = function()
			objectClusterizerReset = true
		end
	}
}

local recruit = adapter:recruitHero({ id = 11 }, { id = 22 })
assert(recruit.ok == true)
assert(recruit.executed == false)

local chainHero = { id = 77 }
adapter:buildBuilding({ objectID = 33 }, { num = 44 })
adapter:castSpell({ id = 55 }, { id = 66 }, { x = 1, y = 2, z = 0 })
adapter:executeHeroChain({ targetHero = chainHero, tile = { x = 3, y = 4, z = 0 } }, 88)
adapter:recruitCreatures({ id = 91 }, { id = 92 }, { id = 93 }, 7, 2)
adapter:upgradeCreature({ id = 94 }, 5, { id = 95 })
adapter:mergeStacks({ id = 96 }, 6, 4)
adapter:mergeOrSwapStacks({ id = 96 }, { id = 97 }, 6, 4)
adapter:splitStack({ id = 96 }, { id = 97 }, 6, 4, 3)
adapter:swapArtifacts({ id = 96 }, 19, { id = 97 }, 0)
adapter:dismissCreature({ id = 98 }, 3)
adapter:lockHero({ id = 95 }, 3)
assert(lockedHeroes[95] == 3)
adapter:unlockHero({ id = 95 })
adapter:setTargetObject({ id = 96 })
adapter:setActive({ id = 97 }, { x = 9, y = 8, z = 0 })
adapter:invalidatePathfinderData()
adapter:resetObjectClusterizer()
adapter:endTurn()

local journal = adapter:getJournal()
assert(#journal == 17)
assert(journal[1].name == "recruitHero")
assert(journal[1].payload.town == 11)
assert(journal[1].payload.hero == 22)
assert(journal[2].name == "buildBuilding")
assert(journal[2].payload.town == 33)
assert(journal[2].payload.bid == 44)
assert(journal[3].name == "castSpell")
assert(journal[3].payload.hero == 55)
assert(journal[3].payload.spell == 66)
assert(journal[3].payload.x == 1)
assert(journal[4].name == "setActive")
assert(journal[4].payload.hero == 77)
assert(journal[4].payload.x == 3)
assert(journal[5].name == "moveHeroToTile")
assert(journal[5].payload.hero == 77)
assert(journal[5].payload.x == 3)
assert(chainHero.visitablePos.x == 3)
assert(journal[6].name == "recruitCreatures")
assert(journal[6].payload.town == 91)
assert(journal[6].payload.dst == 92)
assert(journal[6].payload.creature == 93)
assert(journal[6].payload.count == 7)
assert(journal[6].payload.level == 2)
assert(journal[7].name == "upgradeCreature")
assert(journal[7].payload.army == 94)
assert(journal[7].payload.slot == 5)
assert(journal[7].payload.creature == 95)
assert(journal[8].name == "mergeStacks")
assert(journal[8].payload.army == 96)
assert(journal[8].payload.fromSlot == 6)
assert(journal[8].payload.toSlot == 4)
assert(journal[9].name == "mergeOrSwapStacks")
assert(journal[9].payload.src == 96)
assert(journal[9].payload.dst == 97)
assert(journal[9].payload.fromSlot == 6)
assert(journal[9].payload.toSlot == 4)
assert(journal[10].name == "splitStack")
assert(journal[10].payload.src == 96)
assert(journal[10].payload.dst == 97)
assert(journal[10].payload.fromSlot == 6)
assert(journal[10].payload.toSlot == 4)
assert(journal[10].payload.count == 3)
assert(journal[11].name == "swapArtifacts")
assert(journal[11].payload.srcHero == 96)
assert(journal[11].payload.srcSlot == 19)
assert(journal[11].payload.dstHero == 97)
assert(journal[11].payload.dstSlot == 0)
assert(journal[12].name == "dismissCreature")
assert(journal[12].payload.army == 98)
assert(journal[12].payload.slot == 3)
assert(journal[13].name == "setTargetObject")
assert(journal[13].payload.objid == 96)
assert(journal[14].name == "setActive")
assert(journal[14].payload.hero == 97)
assert(journal[14].payload.x == 9)
assert(journal[15].name == "invalidatePathfinderData")
assert(journal[16].name == "resetObjectClusterizer")
assert(journal[17].name == "endTurn")
assert(lockedHeroes[95] == nil)
assert(targetObject == 96)
assert(activeHero == 97)
assert(activeTile.x == 9)
assert(pathfinderInvalidated == true)
assert(objectClusterizerReset == true)

assert(#called == 17)
assert(called[1].name == "recruitHero")

local chainCalled = {}
local chainAdapter = HostCommands.new({
	command = function(_, name, payload)
		table.insert(chainCalled, { name = name, payload = payload })
		return { ok = true, executed = true }
	end
})
chainAdapter:executeHeroChain({
	targetHero = { id = 101, movementPointsRemaining = 1000, visitablePos = { x = 0, y = 0, z = 0 } },
	nodes = {
		{ targetHero = { id = 102, movementPointsRemaining = 1000, visitablePos = { x = 1, y = 1, z = 0 } }, coord = { x = 2, y = 2, z = 0 }, parentIndex = 0 },
		{ targetHero = { id = 101, movementPointsRemaining = 1000, visitablePos = { x = 0, y = 0, z = 0 } }, coord = { x = 3, y = 3, z = 0 }, parentIndex = 1 }
	}
}, 103)
assert(chainCalled[1].name == "setActive")
assert(chainCalled[1].payload.hero == 101)
assert(chainCalled[1].payload.x == 3)
assert(chainCalled[2].name == "moveHeroToTile")
assert(chainCalled[2].payload.hero == 101)
assert(chainCalled[3].name == "setActive")
assert(chainCalled[3].payload.hero == 102)
assert(chainCalled[3].payload.x == 2)
assert(chainCalled[4].name == "moveHeroToTile")
assert(chainCalled[4].payload.hero == 102)

local staleCalled = {}
local staleAdapter = HostCommands.new({
	command = function(_, name, payload)
		table.insert(staleCalled, { name = name, payload = payload })
		return { ok = true, executed = true }
	end
})
local staleResult = staleAdapter:executeHeroChain({
	targetHero = { id = 111, movementPointsRemaining = 1000, visitablePos = { x = 0, y = 0, z = 0 } },
	nodes = {
		{
			targetHero = { id = 111, movementPointsRemaining = 1000, visitablePos = { x = 0, y = 0, z = 0 } },
			coord = { x = 4, y = 4, z = 0 },
			turns = 0,
			pathInfo = { turns = 1, accessible = "ACCESSIBLE" }
		}
	}
}, 112)
assert(staleResult.ok == false)
assert(staleResult.stale == true)
assert(#staleCalled == 1)
assert(staleCalled[1].name == "setActive")
assert(staleCalled[1].payload.hero == 111)
