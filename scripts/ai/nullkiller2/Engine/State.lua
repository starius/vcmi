-- Mirrors AI/Nullkiller2/Engine/Nullkiller.h state fields and lightweight state helpers.

local State = {}

State.HeroLockedReason = {
	NOT_LOCKED = 0,
	STARTUP = 1,
	DEFENCE = 2,
	HERO_CHAIN = 3
}

State.ScanDepth = {
	MAIN_FULL = 0,
	SMALL = 1,
	ALL_FULL = 2
}

local function resourceVector(values)
	local result = {}
	for index = 1, 7 do
		result[index] = values and values[index] or 0
	end
	return result
end

function State.new()
	return {
		activeHero = nil,
		targetTile = nil,
		targetObject = nil,
		lockedHeroes = {},
		lockedResources = resourceVector(),
		scanDepth = State.ScanDepth.MAIN_FULL,
		useHeroChain = true,
		openMap = false,
		useObjectGraph = false,
		pathfinderInvalidated = false
	}
end

function State.resetState(state)
	state.lockedHeroes = {}
	state.lockedResources = resourceVector()
	state.scanDepth = State.ScanDepth.MAIN_FULL
	state.useHeroChain = true
	state.pathfinderInvalidated = false
end

function State.lockHero(state, heroID, lockReason)
	assert(heroID ~= nil, "hero id is required")
	state.lockedHeroes[heroID] = lockReason or State.HeroLockedReason.DEFENCE
end

function State.unlockHero(state, heroID)
	state.lockedHeroes[heroID] = nil
end

function State.getHeroLockedReason(state, heroID)
	return state.lockedHeroes[heroID] or State.HeroLockedReason.NOT_LOCKED
end

function State.lockResources(state, resources)
	for index = 1, 7 do
		state.lockedResources[index] = (state.lockedResources[index] or 0) + ((resources or {})[index] or 0)
	end
end

function State.getFreeResources(state, resources)
	local result = {}
	for index = 1, 7 do
		local amount = ((resources or {})[index] or 0) - (state.lockedResources[index] or 0)
		if amount < 0 then
			amount = 0
		end
		result[index] = amount
	end
	return result
end

return State
