local Settings = require("Engine.Settings")

local config = {
	rook = {
		maxRoamingHeroes = 2,
		maxRoamingHeroesPerTown = 2,
		maxPass = 40,
		maxPriorityPass = 40,
		mainHeroTurnDistanceLimit = 20,
		scoutHeroTurnDistanceLimit = 15,
		threatTurnDistanceLimit = 5,
		maxGoldPressure = 0.3,
		updateHitmapOnTileReveal = true,
		useTroopsFromGarrisons = true,
		useOneWayMonoliths = false,
		openMap = false,
		allowObjectGraph = false,
		pathfinderBucketsCount = 1,
		pathfinderBucketSize = 32,
		retreatThresholdRelative = 0.3,
		retreatThresholdAbsolute = 10000,
		safeAttackRatio = 1.1,
		maxArmyLossTarget = 0.35
	}
}

local settings = Settings.fromDifficultyConfig(config, "rook")
assert(settings.difficultyName == "rook")
assert(settings.maxPass == 40)
assert(settings.maxPriorityPass == 40)
assert(settings.openMap == false)
assert(settings.safeAttackRatio == 1.1)

local defaults = Settings.withDefaults({ maxPass = 7 })
assert(defaults.maxPass == 7)
assert(defaults.maxPriorityPass == Settings.DEFAULTS.maxPriorityPass)
