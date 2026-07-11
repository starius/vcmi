/*
 * BattleSimulationBatch.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationBatch.h"

#include "BattleProcessor.h"
#include "BattleSimulationReplay.h"

#include "../CGameHandler.h"

#include "../../lib/CConfigHandler.h"
#include "../../lib/battle/BattleInfo.h"
#include "../../lib/battle/CBattleInfoCallback.h"
#include "../../lib/battle/IBattleState.h"
#include "../../lib/bonuses/IBonusBearer.h"
#include "../../lib/constants/Enumerations.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CCreatureSet.h"
#include "../../lib/mapObjects/army/CStackInstance.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"
#include "../../lib/spells/CSpell.h"

#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace BattleSimulationBatch
{
namespace
{
struct Config
{
	bool enabled = false;
	std::string outputPath;
	int64_t maxBattles = 0;
	int shardIndex = 0;
	int shardCount = 1;
	int64_t globalSeed = 0;
	int64_t shardSeed = 0;
};

struct State
{
	bool initialized = false;
	Config config;
	std::ofstream output;
	BattleSimulation::BattleSimulationReplaySession replay;
};

State state;

std::string quote(const std::string & value)
{
	std::ostringstream out;
	out << '"';
	for(const char ch : value)
	{
		switch(ch)
		{
			case '\\':
				out << "\\\\";
				break;
			case '"':
				out << "\\\"";
				break;
			case '\n':
				out << "\\n";
				break;
			case '\r':
				out << "\\r";
				break;
			case '\t':
				out << "\\t";
				break;
			default:
				out << ch;
				break;
		}
	}
	out << '"';
	return out.str();
}

template<typename Identifier>
void appendNullableIdentifier(std::ostream & out, const Identifier & identifier)
{
	if(identifier.hasValue())
		out << identifier.getNum();
	else
		out << "null";
}

std::string battleSideToString(BattleSide side)
{
	switch(side)
	{
		case BattleSide::ATTACKER:
			return "attacker";
		case BattleSide::DEFENDER:
			return "defender";
		case BattleSide::NONE:
			return "none";
		default:
			return "draw";
	}
}

std::string battleResultToString(EBattleResult result)
{
	switch(result)
	{
		case EBattleResult::NORMAL:
			return "normal";
		case EBattleResult::ESCAPE:
			return "escape";
		case EBattleResult::SURRENDER:
			return "surrender";
		default:
			return "unknown";
	}
}

std::string battleTypeToString(const IBattleInfo * info)
{
	if(info->getDefendedTown())
		return info->getSideHero(BattleSide::DEFENDER) ? "town-hero" : "town";
	if(info->getSideHero(BattleSide::DEFENDER))
		return "hero-hero";
	return "hero-monster";
}

bool hasCreatureBonus(const Creature * creature, BonusType type)
{
	const auto * bearer = dynamic_cast<const IBonusBearer *>(creature);
	return bearer && bearer->hasBonusOfType(type);
}

int creatureBonusValue(const Creature * creature, BonusType type)
{
	const auto * bearer = dynamic_cast<const IBonusBearer *>(creature);
	return bearer ? bearer->valOfBonuses(type) : 0;
}

void appendCreatureStats(std::ostream & out, const Creature * creature)
{
	if(!creature)
	{
		out << "null";
		return;
	}

	out << "{";
	out << "\"level\":" << creature->getLevel();
	out << ",\"faction\":" << creature->getFactionID().getNum();
	out << ",\"fightValue\":" << creature->getFightValue();
	out << ",\"aiValue\":" << creature->getAIValue();
	out << ",\"growth\":" << creature->getGrowth();
	out << ",\"attack\":" << creature->getBaseAttack();
	out << ",\"defense\":" << creature->getBaseDefense();
	out << ",\"damageMin\":" << creature->getBaseDamageMin();
	out << ",\"damageMax\":" << creature->getBaseDamageMax();
	out << ",\"hitPoints\":" << creature->getBaseHitPoints();
	out << ",\"speed\":" << creature->getBaseSpeed();
	out << ",\"shots\":" << creature->getBaseShots();
	out << ",\"spellPoints\":" << creature->getBaseSpellPoints();
	out << ",\"doubleWide\":" << (creature->isDoubleWide() ? "true" : "false");
	out << ",\"shooter\":" << (hasCreatureBonus(creature, BonusType::SHOOTER) ? "true" : "false");
	out << ",\"flying\":" << (hasCreatureBonus(creature, BonusType::FLYING) ? "true" : "false");
	out << ",\"blocksRetaliation\":" << (hasCreatureBonus(creature, BonusType::BLOCKS_RETALIATION) ? "true" : "false");
	out << ",\"unlimitedRetaliations\":" << (hasCreatureBonus(creature, BonusType::UNLIMITED_RETALIATIONS) ? "true" : "false");
	out << ",\"additionalAttack\":" << creatureBonusValue(creature, BonusType::ADDITIONAL_ATTACK);
	out << ",\"additionalRetaliation\":" << creatureBonusValue(creature, BonusType::ADDITIONAL_RETALIATION);
	out << ",\"returnAfterStrike\":" << (hasCreatureBonus(creature, BonusType::RETURN_AFTER_STRIKE) ? "true" : "false");
	out << ",\"twoHexAttackBreath\":" << (hasCreatureBonus(creature, BonusType::TWO_HEX_ATTACK_BREATH) ? "true" : "false");
	out << ",\"attacksAllAdjacent\":" << (hasCreatureBonus(creature, BonusType::ATTACKS_ALL_ADJACENT) ? "true" : "false");
	out << ",\"threeHeadedAttack\":" << (hasCreatureBonus(creature, BonusType::THREE_HEADED_ATTACK) ? "true" : "false");
	out << ",\"spellAfterAttack\":" << (hasCreatureBonus(creature, BonusType::SPELL_AFTER_ATTACK) ? "true" : "false");
	out << ",\"spellcaster\":" << (hasCreatureBonus(creature, BonusType::SPELLCASTER) ? "true" : "false");
	out << ",\"mindImmune\":" << (hasCreatureBonus(creature, BonusType::MIND_IMMUNITY) ? "true" : "false");
	out << ",\"undead\":" << (hasCreatureBonus(creature, BonusType::UNDEAD) ? "true" : "false");
	out << ",\"nonLiving\":" << (hasCreatureBonus(creature, BonusType::NON_LIVING) ? "true" : "false");
	out << ",\"magicResistance\":" << creatureBonusValue(creature, BonusType::MAGIC_RESISTANCE);
	out << ",\"levelSpellImmunity\":" << creatureBonusValue(creature, BonusType::LEVEL_SPELL_IMMUNITY);
	out << ",\"spellDamageReduction\":" << creatureBonusValue(creature, BonusType::SPELL_DAMAGE_REDUCTION);
	out << ",\"blockAllMagic\":" << (hasCreatureBonus(creature, BonusType::BLOCK_ALL_MAGIC) ? "true" : "false");
	out << ",\"spellSchoolImmunity\":" << (hasCreatureBonus(creature, BonusType::SPELL_SCHOOL_IMMUNITY) ? "true" : "false");
	out << "}";
}

void appendArmy(std::ostream & out, const CCreatureSet * army)
{
	out << '[';
	bool first = true;
	if(army)
	{
		for(const auto & [slot, stack] : army->Slots())
		{
			if(!first)
				out << ',';
			first = false;

			out << "{";
			out << "\"slot\":" << slot.getNum();
			out << ",\"creature\":" << stack->getCreatureID().getNum();
			out << ",\"count\":" << stack->getCount();
			out << ",\"power\":" << stack->getPower();
			out << ",\"stats\":";
			appendCreatureStats(out, stack->getType());
			out << ",\"experience\":" << stack->getTotalExperience();
			out << "}";
		}
	}
	out << ']';
}

void appendArmySnapshot(std::ostream & out, const BattleStartArmySnapshot & army)
{
	out << "{";
	out << "\"objectId\":";
	appendNullableIdentifier(out, army.objectId);
	out << ",\"armyStrength\":" << army.armyStrength;
	out << ",\"stacks\":[";
	bool first = true;
	for(const auto & stack : army.stacks)
	{
		if(!first)
			out << ',';
		first = false;

		out << "{";
		out << "\"slot\":" << stack.slot.getNum();
		out << ",\"creature\":" << stack.creature.getNum();
		out << ",\"count\":" << stack.count;
		out << ",\"power\":" << stack.power;
		out << ",\"stats\":";
		appendCreatureStats(out, stack.creature.toCreature());
		out << ",\"experience\":" << stack.experience;
		out << "}";
	}
	out << "]}";
}

void appendTownPreMergeState(std::ostream & out, const BattleStartTownPreMergeSnapshot * snapshot)
{
	if(!snapshot)
	{
		out << "null";
		return;
	}

	out << "{";
	out << "\"townId\":";
	appendNullableIdentifier(out, snapshot->townId);
	out << ",\"defendingHeroId\":";
	appendNullableIdentifier(out, snapshot->defendingHeroId);
	out << ",\"townArmy\":";
	appendArmySnapshot(out, snapshot->townArmy);
	out << ",\"defendingHeroArmy\":";
	appendArmySnapshot(out, snapshot->defendingHeroArmy);
	out << '}';
}

void appendBattleStartStacks(std::ostream & out, const BattleStartStateSnapshot * snapshot)
{
	if(!snapshot)
	{
		out << "null";
		return;
	}

	out << '[';
	bool first = true;
	for(const auto & stack : snapshot->stacks)
	{
		if(!first)
			out << ',';
		first = false;

		out << "{";
		out << "\"unitId\":" << stack.unitId;
		out << ",\"side\":" << static_cast<int>(stack.side);
		out << ",\"slot\":" << stack.slot.getNum();
		out << ",\"creature\":" << stack.creature.getNum();
		out << ",\"count\":" << stack.count;
		out << ",\"baseAmount\":" << stack.baseAmount;
		out << ",\"position\":" << stack.position;
		out << ",\"initialPosition\":" << stack.initialPosition;
		out << ",\"availableHealth\":" << stack.availableHealth;
		out << ",\"totalHealth\":" << stack.totalHealth;
		out << ",\"maxHealth\":" << stack.maxHealth;
		out << ",\"firstHPLeft\":" << stack.firstHPLeft;
		out << ",\"meleeAttack\":" << stack.meleeAttack;
		out << ",\"rangedAttack\":" << stack.rangedAttack;
		out << ",\"meleeDefense\":" << stack.meleeDefense;
		out << ",\"rangedDefense\":" << stack.rangedDefense;
		out << ",\"meleeDamageMin\":" << stack.meleeDamageMin;
		out << ",\"meleeDamageMax\":" << stack.meleeDamageMax;
		out << ",\"rangedDamageMin\":" << stack.rangedDamageMin;
		out << ",\"rangedDamageMax\":" << stack.rangedDamageMax;
		out << ",\"speed\":" << stack.speed;
		out << ",\"movementRange\":" << stack.movementRange;
		out << ",\"morale\":" << stack.morale;
		out << ",\"luck\":" << stack.luck;
		out << ",\"shotsAvailable\":" << stack.shotsAvailable;
		out << ",\"shotsTotal\":" << stack.shotsTotal;
		out << ",\"castsAvailable\":" << stack.castsAvailable;
		out << ",\"castsTotal\":" << stack.castsTotal;
		out << ",\"retaliationsAvailable\":" << stack.retaliationsAvailable;
		out << ",\"retaliationsTotal\":" << stack.retaliationsTotal;
		out << ",\"alive\":" << (stack.alive ? "true" : "false");
		out << ",\"validTarget\":" << (stack.validTarget ? "true" : "false");
		out << ",\"doubleWide\":" << (stack.doubleWide ? "true" : "false");
		out << ",\"shooter\":" << (stack.shooter ? "true" : "false");
		out << ",\"canShoot\":" << (stack.canShoot ? "true" : "false");
		out << ",\"caster\":" << (stack.caster ? "true" : "false");
		out << ",\"canCast\":" << (stack.canCast ? "true" : "false");
		out << ",\"turret\":" << (stack.turret ? "true" : "false");
		out << ",\"catapult\":" << (stack.catapult ? "true" : "false");
		out << ",\"ballista\":" << (stack.ballista ? "true" : "false");
		out << ",\"firstAidTent\":" << (stack.firstAidTent ? "true" : "false");
		out << ",\"ammoCart\":" << (stack.ammoCart ? "true" : "false");
		out << ",\"summoned\":" << (stack.summoned ? "true" : "false");
		out << "}";
	}
	out << ']';
}

void appendBattleStartObstacles(std::ostream & out, const BattleStartStateSnapshot * snapshot)
{
	if(!snapshot)
	{
		out << "null";
		return;
	}

	out << '[';
	bool firstObstacle = true;
	for(const auto & obstacle : snapshot->obstacles)
	{
		if(!firstObstacle)
			out << ',';
		firstObstacle = false;

		out << "{";
		out << "\"uniqueId\":" << obstacle.uniqueId;
		out << ",\"id\":" << obstacle.id;
		out << ",\"type\":" << obstacle.type;
		out << ",\"position\":" << obstacle.position;
		out << ",\"trigger\":" << obstacle.trigger;
		out << ",\"turnsRemaining\":" << obstacle.turnsRemaining;
		out << ",\"spellLevel\":" << obstacle.spellLevel;
		out << ",\"casterSide\":" << obstacle.casterSide;
		out << ",\"blocksTiles\":" << (obstacle.blocksTiles ? "true" : "false");
		out << ",\"stopsMovement\":" << (obstacle.stopsMovement ? "true" : "false");
		out << ",\"triggersEffects\":" << (obstacle.triggersEffects ? "true" : "false");
		out << ",\"hidden\":" << (obstacle.hidden ? "true" : "false");
		out << ",\"passable\":" << (obstacle.passable ? "true" : "false");
		out << ",\"trap\":" << (obstacle.trap ? "true" : "false");
		out << ",\"removeOnTrigger\":" << (obstacle.removeOnTrigger ? "true" : "false");
		out << ",\"revealed\":" << (obstacle.revealed ? "true" : "false");
		out << ",\"affectedTiles\":[";
		bool firstTile = true;
		for(const auto tile : obstacle.affectedTiles)
		{
			if(!firstTile)
				out << ',';
			firstTile = false;
			out << tile;
		}
		out << "]}";
	}
	out << ']';
}

void appendSpellList(std::ostream & out, const std::set<SpellID> & spells, bool combatOnly)
{
	out << '[';
	bool first = true;
	for(const auto & spellID : spells)
	{
		const auto * spell = spellID.toSpell();
		if(combatOnly && (!spell || !spell->isCombat()))
			continue;

		if(!first)
			out << ',';
		first = false;
		out << spellID.getNum();
	}
	out << ']';
}

void appendSpellList(std::ostream & out, const std::vector<SpellID> & spells)
{
	out << '[';
	bool first = true;
	for(const auto & spellID : spells)
	{
		if(!first)
			out << ',';
		first = false;
		out << spellID.getNum();
	}
	out << ']';
}

void appendSecondarySkills(std::ostream & out, const CGHeroInstance * hero)
{
	out << '[';
	bool first = true;
	if(hero)
	{
		for(const auto & [skillID, level] : hero->secSkills)
		{
			if(!skillID.hasValue())
				continue;

			if(!first)
				out << ',';
			first = false;
			out << "{\"skill\":" << skillID.getNum() << ",\"level\":" << static_cast<int>(level) << '}';
		}
	}
	out << ']';
}

void appendDamageRange(std::ostream & out, const DamageRange & damage)
{
	out << "{\"min\":" << damage.min << ",\"max\":" << damage.max << '}';
}

void appendTownBuildings(std::ostream & out, const CGTownInstance * town)
{
	out << '[';
	bool first = true;
	if(town)
	{
		for(const auto & building : town->getBuildings())
		{
			if(!first)
				out << ',';
			first = false;
			out << building.getNum();
		}
	}
	out << ']';
}

void appendTownFortifications(std::ostream & out, const CGTownInstance * town)
{
	if(!town)
	{
		out << "null";
		return;
	}

	const auto fortifications = town->fortificationsLevel();
	out << "{";
	out << "\"wallsHealth\":" << static_cast<int>(fortifications.wallsHealth);
	out << ",\"citadelHealth\":" << static_cast<int>(fortifications.citadelHealth);
	out << ",\"upperTowerHealth\":" << static_cast<int>(fortifications.upperTowerHealth);
	out << ",\"lowerTowerHealth\":" << static_cast<int>(fortifications.lowerTowerHealth);
	out << ",\"hasMoat\":" << (fortifications.hasMoat ? "true" : "false");
	out << ",\"citadelShooter\":";
	appendNullableIdentifier(out, fortifications.citadelShooter);
	out << ",\"upperTowerShooter\":";
	appendNullableIdentifier(out, fortifications.upperTowerShooter);
	out << ",\"lowerTowerShooter\":";
	appendNullableIdentifier(out, fortifications.lowerTowerShooter);
	out << ",\"moatSpell\":";
	appendNullableIdentifier(out, fortifications.moatSpell);
	out << '}';
}

void appendWallState(
	std::ostream & out,
	EWallState keep,
	EWallState bottomTower,
	EWallState bottomWall,
	EWallState belowGate,
	EWallState overGate,
	EWallState upperWall,
	EWallState upperTower,
	EWallState gate,
	EGateState gateState)
{
	out << "{";
	out << "\"keep\":" << static_cast<int>(keep);
	out << ",\"bottomTower\":" << static_cast<int>(bottomTower);
	out << ",\"bottomWall\":" << static_cast<int>(bottomWall);
	out << ",\"belowGate\":" << static_cast<int>(belowGate);
	out << ",\"overGate\":" << static_cast<int>(overGate);
	out << ",\"upperWall\":" << static_cast<int>(upperWall);
	out << ",\"upperTower\":" << static_cast<int>(upperTower);
	out << ",\"gate\":" << static_cast<int>(gate);
	out << ",\"gateState\":" << static_cast<int>(gateState);
	out << '}';
}

void appendInitialWallState(std::ostream & out, const CGTownInstance * town)
{
	if(!town)
	{
		out << "null";
		return;
	}

	const auto none = EWallState::NONE;
	auto keep = none;
	auto bottomTower = none;
	auto bottomWall = none;
	auto belowGate = none;
	auto overGate = none;
	auto upperWall = none;
	auto upperTower = none;
	auto gate = none;
	auto gateState = EGateState::NONE;
	const auto fortifications = town->fortificationsLevel();

	if(fortifications.wallsHealth != 0)
	{
		gateState = EGateState::CLOSED;
		gate = EWallState::INTACT;
		bottomWall = static_cast<EWallState>(fortifications.wallsHealth);
		belowGate = static_cast<EWallState>(fortifications.wallsHealth);
		overGate = static_cast<EWallState>(fortifications.wallsHealth);
		upperWall = static_cast<EWallState>(fortifications.wallsHealth);

		if(fortifications.citadelHealth != 0)
			keep = static_cast<EWallState>(fortifications.citadelHealth);
		if(fortifications.upperTowerHealth != 0)
			upperTower = static_cast<EWallState>(fortifications.upperTowerHealth);
		if(fortifications.lowerTowerHealth != 0)
			bottomTower = static_cast<EWallState>(fortifications.lowerTowerHealth);
	}

	appendWallState(out, keep, bottomTower, bottomWall, belowGate, overGate, upperWall, upperTower, gate, gateState);
}

void appendFinalWallState(std::ostream & out, const IBattleInfo * info)
{
	if(!info->getDefendedTown())
	{
		out << "null";
		return;
	}

	appendWallState(
		out,
		info->getWallState(EWallPart::KEEP),
		info->getWallState(EWallPart::BOTTOM_TOWER),
		info->getWallState(EWallPart::BOTTOM_WALL),
		info->getWallState(EWallPart::BELOW_GATE),
		info->getWallState(EWallPart::OVER_GATE),
		info->getWallState(EWallPart::UPPER_WALL),
		info->getWallState(EWallPart::UPPER_TOWER),
		info->getWallState(EWallPart::GATE),
		info->getGateState());
}

void appendBattleStartWallState(std::ostream & out, const BattleStartStateSnapshot * snapshot)
{
	if(!snapshot || !snapshot->wallState)
	{
		out << "null";
		return;
	}

	const auto & wallState = *snapshot->wallState;
	appendWallState(
		out,
		wallState.keep,
		wallState.bottomTower,
		wallState.bottomWall,
		wallState.belowGate,
		wallState.overGate,
		wallState.upperWall,
		wallState.upperTower,
		wallState.gate,
		wallState.gateState);
}

std::string defendedHeroSource(const CGTownInstance * town, const CGHeroInstance * defenderHero)
{
	if(!town || !defenderHero)
		return "none";

	const auto * visitingHero = town->getVisitingHero();
	if(visitingHero && visitingHero->id == defenderHero->id)
		return "visiting";

	const auto * garrisonHero = town->getGarrisonHero();
	if(garrisonHero && garrisonHero->id == defenderHero->id)
		return "garrison";

	return "unknown";
}

void appendTown(std::ostream & out, const CGTownInstance * town, const CGHeroInstance * defenderHero)
{
	if(!town)
	{
		out << "null";
		return;
	}

	out << "{";
	out << "\"objectId\":" << town->id.getNum();
	out << ",\"faction\":" << town->getFactionID().getNum();
	out << ",\"name\":" << quote(town->getNameTranslated());
	out << ",\"fortLevel\":" << static_cast<int>(town->fortLevel());
	out << ",\"hallLevel\":" << town->hallLevel();
	out << ",\"mageGuildLevel\":" << town->mageGuildLevel();
	out << ",\"hasFort\":" << (town->hasFort() ? "true" : "false");
	out << ",\"hasBuiltTavern\":" << (town->hasBuilt(BuildingID::TAVERN) ? "true" : "false");
	out << ",\"hasBuiltGrail\":" << (town->hasBuilt(BuildingID::GRAIL) ? "true" : "false");
	out << ",\"hasVisitingHero\":" << (town->getVisitingHero() ? "true" : "false");
	out << ",\"hasGarrisonHero\":" << (town->getGarrisonHero() ? "true" : "false");
	out << ",\"defendingHeroSource\":" << quote(defendedHeroSource(town, defenderHero));
	out << ",\"battleTerrain\":" << town->getBattleTerrain().getNum();
	out << ",\"armyStrength\":" << town->getArmyStrength();
	out << ",\"buildings\":";
	appendTownBuildings(out, town);
	out << ",\"fortifications\":";
	appendTownFortifications(out, town);
	out << ",\"towerDamage\":";
	appendDamageRange(out, town->getTowerDamageRange());
	out << ",\"keepDamage\":";
	appendDamageRange(out, town->getKeepDamageRange());
	out << '}';
}

void appendCasualties(std::ostream & out, const std::map<CreatureID, si32> & casualties)
{
	out << '[';
	bool first = true;
	for(const auto & [creature, killed] : casualties)
	{
		if(!first)
			out << ',';
		first = false;

		out << "{\"creature\":" << creature.getNum() << ",\"killed\":" << killed << '}';
	}
	out << ']';
}

int32_t rememberInitialMana(const CGHeroInstance * hero, int32_t fallback)
{
	return state.replay.rememberInitialMana(hero, fallback);
}

int countCombatSpells(const CGHeroInstance * hero)
{
	int result = 0;
	for(const auto & spellID : hero->getSpellsInSpellbook())
	{
		if(spellID.toSpell()->isCombat())
			++result;
	}
	return result;
}

double calculateMagicStrength(const CGHeroInstance * hero, int32_t mana, int combatSpellCount)
{
	if(!hero || !hero->hasSpellbook() || combatSpellCount == 0)
		return 1.0;

	const auto manaLimit = hero->manaLimit();
	if(manaLimit <= 0)
		return 1.0;

	const auto spellPower = hero->getPrimSkillLevel(PrimarySkill::SPELL_POWER);
	const auto knowledge = hero->getPrimSkillLevel(PrimarySkill::KNOWLEDGE);
	const double manaRatio = static_cast<double>(mana) / manaLimit;
	return std::sqrt((1.0 + 0.05 * knowledge * manaRatio) * (1.0 + 0.05 * spellPower * manaRatio));
}

int32_t getBattleInitialMana(const IBattleInfo * info, BattleSide side)
{
	const auto * battleInfo = dynamic_cast<const BattleInfo *>(info);
	if(battleInfo)
		return battleInfo->getSide(side).initialMana;

	const auto * hero = info->getSideHero(side);
	return hero ? hero->mana : 0;
}

void appendHero(std::ostream & out, const CGHeroInstance * hero, int32_t initialMana)
{
	if(!hero)
	{
		out << "null";
		return;
	}

	initialMana = rememberInitialMana(hero, initialMana);
	const int combatSpellCount = countCombatSpells(hero);
	const double fightingStrength = hero->getFightingStrength();
	const double magicStrength = calculateMagicStrength(hero, initialMana, combatSpellCount);

	out << "{";
	out << "\"objectId\":" << hero->id.getNum();
	out << ",\"type\":" << hero->getHeroTypeID().getNum();
	out << ",\"name\":" << quote(hero->getNameTranslated());
	out << ",\"level\":" << hero->level;
	out << ",\"mana\":" << initialMana;
	out << ",\"currentMana\":" << hero->mana;
	out << ",\"manaLimit\":" << hero->manaLimit();
	out << ",\"hasSpellbook\":" << (hero->hasSpellbook() ? "true" : "false");
	out << ",\"combatSpellCount\":" << combatSpellCount;
	out << ",\"fightingStrength\":" << std::setprecision(12) << fightingStrength;
	out << ",\"magicStrength\":" << std::setprecision(12) << magicStrength;
	out << ",\"heroStrength\":" << std::setprecision(12) << fightingStrength * magicStrength;
	out << ",\"secondary\":";
	appendSecondarySkills(out, hero);
	out << ",\"spells\":";
	appendSpellList(out, hero->getSpellsInSpellbook(), false);
	out << ",\"combatSpells\":";
	appendSpellList(out, hero->getSpellsInSpellbook(), true);
	out << ",\"primary\":[";
	for(size_t i = 0; i < GameConstants::PRIMARY_SKILLS; ++i)
	{
		if(i)
			out << ',';
		out << hero->getPrimSkillLevel(PrimarySkill(static_cast<int32_t>(i)));
	}
	out << "]}";
}

Config readConfig()
{
	Config config;
	const auto batchSettings = settings["server"]["battleSimulation"];
	config.enabled = batchSettings["enabled"].Bool();
	if(!config.enabled)
		return config;

	config.outputPath = batchSettings["output"].String();
	config.maxBattles = batchSettings["maxBattles"].Integer();
	config.shardIndex = batchSettings["shardIndex"].Integer();
	config.shardCount = batchSettings["shardCount"].Integer();
	config.globalSeed = batchSettings["globalSeed"].Integer();
	config.shardSeed = batchSettings["shardSeed"].Integer();

	if(config.shardCount <= 0)
		config.shardCount = 1;
	if(config.shardIndex < 0)
		config.shardIndex = 0;

	return config;
}

void initialize()
{
	if(state.initialized)
		return;

	state.initialized = true;
	state.config = readConfig();

	if(!state.config.enabled)
		return;

	if(state.config.maxBattles <= 0)
		throw std::runtime_error("Battle simulation max battle count must be positive");

	if(state.config.outputPath.empty())
		throw std::runtime_error("Battle simulation output path must not be empty");

	state.output.open(state.config.outputPath, std::ios::out | std::ios::trunc);
	if(!state.output)
		throw std::runtime_error("Unable to open battle simulation output: " + state.config.outputPath);

	state.replay.setSampleLimit(state.config.maxBattles);
}

void appendResultRow(CGameHandler & gameHandler, const CBattleInfoCallback & battle, const BattleResult & result)
{
	const auto * info = battle.getBattle();
	const int64_t rowIndex = state.replay.recordedSamples();
	const auto * townPreMerge = gameHandler.battles->getTownPreMergeSnapshot(info->getBattleID());
	const auto * battleStart = gameHandler.battles->getBattleStartSnapshot(info->getBattleID());

	state.output << "{";
	state.output << "\"schema\":6";
	state.output << ",\"row\":" << rowIndex;
	state.output << ",\"shardIndex\":" << state.config.shardIndex;
	state.output << ",\"shardCount\":" << state.config.shardCount;
	state.output << ",\"globalSeed\":" << state.config.globalSeed;
	state.output << ",\"shardSeed\":" << state.config.shardSeed;
	state.output << ",\"battleId\":" << info->getBattleID().getNum();
	state.output << ",\"round\":" << info->getRound();
	state.output << ",\"result\":" << quote(battleResultToString(result.result));
	state.output << ",\"winner\":" << quote(battleSideToString(result.winner));
	state.output << ",\"terrain\":" << info->getTerrainType().getNum();
	state.output << ",\"battlefield\":" << info->getBattlefieldType().getNum();
	state.output << ",\"attackerPlayer\":" << info->getSidePlayer(BattleSide::ATTACKER).getNum();
	state.output << ",\"defenderPlayer\":" << info->getSidePlayer(BattleSide::DEFENDER).getNum();
	state.output << ",\"combatEnemyAI\":" << quote(settings["ai"]["combatEnemyAI"].String());
	state.output << ",\"combatNeutralAI\":" << quote(settings["ai"]["combatNeutralAI"].String());
	state.output << ",\"battleType\":" << quote(battleTypeToString(info));
	state.output << ",\"hasFortifications\":" << (battle.hasFortifications() ? "true" : "false");
	state.output << ",\"hasMoat\":" << (battle.hasMoat() ? "true" : "false");
	state.output << ",\"defendedTown\":";
	appendTown(state.output, info->getDefendedTown(), info->getSideHero(BattleSide::DEFENDER));
	state.output << ",\"townPreMergeState\":";
	appendTownPreMergeState(state.output, townPreMerge);
	state.output << ",\"battleStartStacks\":";
	appendBattleStartStacks(state.output, battleStart);
	state.output << ",\"battleStartObstacles\":";
	appendBattleStartObstacles(state.output, battleStart);
	state.output << ",\"battleStartWallState\":";
	appendBattleStartWallState(state.output, battleStart);
	state.output << ",\"initialWallState\":";
	appendInitialWallState(state.output, info->getDefendedTown());
	state.output << ",\"finalWallState\":";
	appendFinalWallState(state.output, info);
	state.output << ",\"attackerHero\":";
	appendHero(state.output, info->getSideHero(BattleSide::ATTACKER), getBattleInitialMana(info, BattleSide::ATTACKER));
	state.output << ",\"defenderHero\":";
	appendHero(state.output, info->getSideHero(BattleSide::DEFENDER), getBattleInitialMana(info, BattleSide::DEFENDER));
	state.output << ",\"attackerArmyStrength\":" << info->getSideArmy(BattleSide::ATTACKER)->getArmyStrength();
	state.output << ",\"defenderArmyStrength\":" << info->getSideArmy(BattleSide::DEFENDER)->getArmyStrength();
	state.output << ",\"attackerArmy\":";
	appendArmy(state.output, info->getSideArmy(BattleSide::ATTACKER));
	state.output << ",\"defenderArmy\":";
	appendArmy(state.output, info->getSideArmy(BattleSide::DEFENDER));
	state.output << ",\"attackerUsedSpells\":";
	appendSpellList(state.output, info->getUsedSpells(BattleSide::ATTACKER));
	state.output << ",\"defenderUsedSpells\":";
	appendSpellList(state.output, info->getUsedSpells(BattleSide::DEFENDER));
	state.output << ",\"attackerCasualties\":";
	appendCasualties(state.output, result.casualties[BattleSide::ATTACKER]);
	state.output << ",\"defenderCasualties\":";
	appendCasualties(state.output, result.casualties[BattleSide::DEFENDER]);
	state.output << "}\n";

	if(!state.output)
		throw std::runtime_error("Failed to write battle simulation result");

	state.output.flush();

	const auto rowsWritten = rowIndex + 1;
	if(rowsWritten % 1000 == 0)
	{
		logGlobal->info("Battle simulation wrote %lld/%lld rows to %s",
			static_cast<long long>(rowsWritten),
			static_cast<long long>(state.config.maxBattles),
			state.config.outputPath);
	}
}
}

bool isEnabled()
{
	initialize();
	return state.config.enabled;
}

bool hasRecordedRows()
{
	initialize();
	return state.replay.hasRecordedSamples();
}

BattleSimulationSummary getSummary()
{
	initialize();
	return state.replay.getSummary();
}

int32_t getReplayInitialMana(const CGHeroInstance * hero, int32_t fallback)
{
	initialize();
	if(!state.config.enabled || !hero)
		return fallback;

	return state.replay.getReplayInitialMana(hero, fallback);
}

BattleSimulationRecordResult recordResult(CGameHandler & gameHandler, const CBattleInfoCallback & battle, const BattleResult & result)
{
	initialize();
	if(!state.config.enabled)
		return {};

	appendResultRow(gameHandler, battle, result);
	return state.replay.recordResult(result);
}

bool recordResultAndShouldReplay(CGameHandler & gameHandler, const CBattleInfoCallback & battle, const BattleResult & result)
{
	return recordResult(gameHandler, battle, result).shouldReplay;
}
}
