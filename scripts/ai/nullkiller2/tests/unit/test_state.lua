local State = require("Engine.State")

local state = State.new()
assert(state.scanDepth == State.ScanDepth.MAIN_FULL)
assert(State.getHeroLockedReason(state, 17) == State.HeroLockedReason.NOT_LOCKED)

State.lockHero(state, 17, State.HeroLockedReason.STARTUP)
assert(State.getHeroLockedReason(state, 17) == State.HeroLockedReason.STARTUP)

State.unlockHero(state, 17)
assert(State.getHeroLockedReason(state, 17) == State.HeroLockedReason.NOT_LOCKED)

State.lockResources(state, { 1, 2, 3, 4, 5, 6, 7 })
local free = State.getFreeResources(state, { 10, 10, 10, 10, 10, 10, 10 })
assert(free[1] == 9)
assert(free[7] == 3)

State.resetState(state)
assert(State.getFreeResources(state, { 1, 1, 1, 1, 1, 1, 1 })[7] == 1)
