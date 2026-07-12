local AIUtility = require("Helpers.AIUtility")

local function almostEquals(lhs, rhs)
	return math.abs(lhs - rhs) < 0.0001
end

local hero = {
	hasSpellbook = true,
	spellsInSpellbook = {
		[1] = true
	},
	defaultAllowedSpells = {
		{ id = 1, level = 1, schools = { "AIR" } },
		{ id = 2, level = 2, schools = { "AIR" } },
		{ id = 3, level = 1, schools = { "FIRE" } }
	},
	slots = {
		{ creatureID = 10, power = 600, bonusTypes = { SHOOTER = true } },
		{ creatureID = 11, power = 400, bonusTypes = { UNDEAD = true } }
	}
}

assert(AIUtility.getArtifactBonusRelevance(hero, { type = "MOVEMENT", subtype = "heroMovementLand" }) == 1.0)
assert(AIUtility.getArtifactBonusRelevance(hero, { type = "MOVEMENT", subtype = "heroMovementSea" }) == 0.0)
assert(AIUtility.getArtifactBonusRelevance({ boat = true }, { type = "MOVEMENT", subtype = "heroMovementSea" }) == 2.0)
assert(AIUtility.getArtifactBonusRelevance({ boat = true }, { type = "FLYING_MOVEMENT" }) == 0.0)
assert(AIUtility.getArtifactBonusRelevance({ boat = true }, { type = "WHIRLPOOL_PROTECTION" }) == 1.0)

assert(almostEquals(AIUtility.getArtifactBonusRelevance(hero, {
	type = "STACK_HEALTH",
	limiter = { creatureIDs = { 10 } }
}), 0.6))
assert(almostEquals(AIUtility.getArtifactBonusRelevance(hero, { type = "MORALE" }), 0.6))
assert(almostEquals(AIUtility.getArtifactBonusRelevance(hero, {
	type = "PERCENTAGE_DAMAGE_BOOST",
	subtype = "damageTypeRanged"
}), 1.2))
assert(almostEquals(AIUtility.getArtifactBonusRelevance(hero, {
	type = "PERCENTAGE_DAMAGE_BOOST",
	subtype = "damageTypeMelee"
}), 0.8))
assert(almostEquals(AIUtility.getArtifactBonusRelevance(hero, { type = "NO_DISTANCE_PENALTY" }), 1.2))
assert(almostEquals(AIUtility.getArtifactBonusRelevance(hero, {
	type = "SPELLS_OF_SCHOOL",
	subtype = "AIR"
}), 0.2))
assert(almostEquals(AIUtility.getArtifactBonusRelevance(hero, {
	type = "SPELLS_OF_LEVEL",
	subtype = 1
}), 0.5))

assert(AIUtility.getArtifactScoreForHero(hero, {
	isScroll = true,
	scrollSpell = { id = 2, level = 3 }
}) == 300)
assert(AIUtility.getArtifactScoreForHero(hero, {
	isScroll = true,
	scrollSpell = { id = 1, level = 4 }
}) == 0)
assert(AIUtility.getArtifactScoreForHero(hero, {
	ID = "SPELLBOOK",
	bonuses = {
		{ type = "PRIMARY_SKILL", val = 99 }
	}
}) == 0)

assert(almostEquals(AIUtility.getArtifactScoreForHero(hero, {
	bonuses = {
		{ type = "MOVEMENT", subtype = "heroMovementLand", val = 100 },
		{ type = "MOVEMENT", subtype = "heroMovementSea", val = 100 },
		{ type = "MORALE", val = 1 }
	},
	parts = {
		{
			bonuses = {
				{ type = "NO_DISTANCE_PENALTY" }
			}
		}
	}
}), 14900))
