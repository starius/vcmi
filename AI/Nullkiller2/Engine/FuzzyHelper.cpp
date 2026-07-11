/*
 * FuzzyHelper.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
*/
#include "../StdInc.h"
#include "FuzzyHelper.h"

#include "../Goals/Goals.h"
#include "Nullkiller.h"

#include "../../../lib/mapObjectConstructors/AObjectTypeHandler.h"
#include "../../../lib/mapObjectConstructors/CObjectClassesHandler.h"
#include "../../../lib/mapObjects/army/CStackInstance.h"
#include "../../../lib/spells/CSpell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace NK2AI
{
namespace
{
struct BattlePredictionFeature
{
	double coefficient;
	double mean;
	double scale;
};

constexpr double V2_BATTLE_PREDICTION_SAFE_PROBABILITY = 0.55;
constexpr double V3_BATTLE_PREDICTION_SAFE_PROBABILITY = 0.60;
constexpr double MIN_CALIBRATED_REQUIRED_RATIO = 0.25;
constexpr double MAX_CALIBRATED_REQUIRED_RATIO = 4.0;

constexpr BattlePredictionFeature V2_LOG_STRENGTH_RATIO{2.29045846667, 0.437265671215, 0.806169412392};
constexpr double V2_BATTLE_PREDICTION_INTERCEPT = 1.25491514231;
constexpr std::array<BattlePredictionFeature, 12> V2_BATTLE_PREDICTION_FEATURES{{
	{0.188331947115, 0.490706319703, 0.499913620045}, // defender has hero
	{1.65620941787, 10.224091904, 0.736716600813}, // log attacker army strength
	{-1.63940215415, 10.1120818809, 0.760765130202}, // log defender army strength
	{-0.884313954049, 0.429082368368, 0.673181372288}, // log stack count ratio
	{-0.134371471319, -0.266542491361, 0.389411454666}, // largest stack share difference
	{0.272808633443, 5.9405204461, 8.76055886064}, // attack difference
	{0.124692078314, 4.44237918216, 9.56650695427}, // defense difference
	{0.443293797233, 5.20074349442, 8.70353512148}, // spell power difference
	{-0.0418897387296, 5.13382899628, 8.76440548779}, // knowledge difference
	{-0.122638318073, 0.508122743381, 0.498525145772}, // mana ratio difference
	{-0.391808195656, 0.565055762082, 0.495749682622}, // attacker has spellbook
	{-0.00131909457523, 0.275092936803, 0.446561096519}, // defender has spellbook
}};

constexpr BattlePredictionFeature V3_LOG_STRENGTH_RATIO{2.62135643331, 0.446922572487, 0.815917259918};
constexpr double V3_BATTLE_PREDICTION_INTERCEPT = 1.19022780253;
constexpr std::array<BattlePredictionFeature, 17> V3_BATTLE_PREDICTION_FEATURES{{
	{0.468810332299, 0.494545454545, 0.499970247049}, // defender has hero
	{1.45543395361, 10.192502792, 0.772118513706}, // log attacker army strength
	{-1.44810294879, 10.08671394, 0.744063816548}, // log defender army strength
	{-1.13950296625, 0.424568267977, 0.700102370093}, // log stack count ratio
	{-0.403380522643, -0.273682343684, 0.397677129925}, // largest stack share difference
	{0.336767948616, 5.80727272727, 8.74461610042}, // attack difference
	{0.244887696405, 5.01454545455, 9.47416426023}, // defense difference
	{0.318006419965, 5.30181818182, 9.13095615643}, // spell power difference
	{0.0688971253683, 4.37454545455, 9.22061362939}, // knowledge difference
	{-0.468810332299, 0.505454545455, 0.499970247049}, // level difference
	{0.442212589111, 0.505317851301, 0.496692983083}, // mana ratio difference
	{0.211822037923, 44.56, 93.8397039444}, // current mana difference
	{-0.288005257378, 44.5636363636, 94.0816209733}, // mana limit difference
	{-0.163787797423, 0.581818181818, 0.493260362409}, // attacker has spellbook
	{-0.042127370793, 0.258181818182, 0.43763451297}, // defender has spellbook
	{-0.39778987901, 0.323636363636, 0.682765402639}, // combat spell count difference
	{0.0305726479297, 0.341133720491, 0.450081992853}, // log hero strength ratio
}};

double scaledFeature(double value, const BattlePredictionFeature & feature)
{
	return feature.coefficient * ((value - feature.mean) / feature.scale);
}

double heroStrengthOrOne(const CGHeroInstance * hero)
{
	return hero ? getNormalizedHeroStrength(hero) : 1.0;
}

double manaRatio(const CGHeroInstance * hero)
{
	if(!hero)
		return 0.0;

	const auto manaLimit = hero->manaLimit();
	if(manaLimit <= 0)
		return 0.0;

	return static_cast<double>(hero->mana) / manaLimit;
}

double primarySkill(const CGHeroInstance * hero, PrimarySkill skill)
{
	return hero ? hero->getPrimSkillLevel(skill) : 0.0;
}

double heroLevel(const CGHeroInstance * hero)
{
	return hero ? hero->level : 0.0;
}

double currentMana(const CGHeroInstance * hero)
{
	return hero ? hero->mana : 0.0;
}

double manaLimit(const CGHeroInstance * hero)
{
	return hero ? hero->manaLimit() : 0.0;
}

double stackCount(const CCreatureSet * army)
{
	return army ? army->stacksCount() : 0.0;
}

double largestStackShare(const CCreatureSet * army, double armyStrength)
{
	if(!army || armyStrength <= 0)
		return 0.0;

	double result = 0.0;
	for(const auto & [slot, stack] : army->Slots())
		vstd::amax(result, static_cast<double>(stack->getPower()) / armyStrength);

	return result;
}

double logit(double probability)
{
	return std::log(probability / (1.0 - probability));
}

double combatSpellCount(const CGHeroInstance * hero)
{
	if(!hero || !hero->hasSpellbook())
		return 0.0;

	double result = 0.0;
	for(const auto & spellID : hero->getSpellsInSpellbook())
	{
		const auto * spell = spellID.toSpell();
		if(spell && !spell->isAdventure())
			result += 1.0;
	}

	return result;
}

template<size_t N>
double requiredRatioForModel(
	double intercept,
	double safeProbability,
	const BattlePredictionFeature & logStrengthRatio,
	const std::array<double, N> & featureValues,
	const std::array<BattlePredictionFeature, N> & features)
{
	double scoreWithoutRatio = intercept;
	for(size_t index = 0; index < featureValues.size(); ++index)
		scoreWithoutRatio += scaledFeature(featureValues[index], features[index]);

	const double requiredLogRatio = logStrengthRatio.mean
		+ logStrengthRatio.scale * (logit(safeProbability) - scoreWithoutRatio) / logStrengthRatio.coefficient;

	return std::clamp(std::exp(requiredLogRatio), MIN_CALIBRATED_REQUIRED_RATIO, MAX_CALIBRATED_REQUIRED_RATIO);
}

uint64_t calibrateDangerForVisitor(
	const CGHeroInstance * visitor,
	const CArmedInstance * defender,
	const CGHeroInstance * defenderHero,
	uint64_t fallbackDanger,
	BattlePredictionModel battlePredictionModel,
	float safeAttackRatio)
{
	if(!visitor || !defender || fallbackDanger == 0 || safeAttackRatio <= 0)
		return fallbackDanger;

	// Towns defended by heroes need siege-specific calibration data; keep legacy danger there.
	if(defenderHero && defenderHero->getVisitedTown())
		return fallbackDanger;

	const double attackerArmyStrength = std::max<double>(visitor->getArmyStrength(), 1.0);
	double defenderArmyStrength = std::max<double>(defender->getArmyStrength(), 1.0);
	const double defenderStrength = defenderArmyStrength * heroStrengthOrOne(defenderHero);

	if(!std::isfinite(defenderStrength) || defenderStrength <= 0)
		return fallbackDanger;

	const std::array<double, V2_BATTLE_PREDICTION_FEATURES.size()> v2FeatureValues{{
		defenderHero ? 1.0 : 0.0,
		std::log(attackerArmyStrength),
		std::log(defenderArmyStrength),
		std::log((stackCount(visitor) + 1.0) / (stackCount(defender) + 1.0)),
		largestStackShare(visitor, attackerArmyStrength) - largestStackShare(defender, defenderArmyStrength),
		primarySkill(visitor, PrimarySkill::ATTACK) - primarySkill(defenderHero, PrimarySkill::ATTACK),
		primarySkill(visitor, PrimarySkill::DEFENSE) - primarySkill(defenderHero, PrimarySkill::DEFENSE),
		primarySkill(visitor, PrimarySkill::SPELL_POWER) - primarySkill(defenderHero, PrimarySkill::SPELL_POWER),
		primarySkill(visitor, PrimarySkill::KNOWLEDGE) - primarySkill(defenderHero, PrimarySkill::KNOWLEDGE),
		manaRatio(visitor) - manaRatio(defenderHero),
		visitor->hasSpellbook() ? 1.0 : 0.0,
		(defenderHero && defenderHero->hasSpellbook()) ? 1.0 : 0.0,
	}};

	double requiredRatio = requiredRatioForModel(V2_BATTLE_PREDICTION_INTERCEPT, V2_BATTLE_PREDICTION_SAFE_PROBABILITY, V2_LOG_STRENGTH_RATIO, v2FeatureValues, V2_BATTLE_PREDICTION_FEATURES);
	if(battlePredictionModel == BattlePredictionModel::V3)
	{
		const std::array<double, V3_BATTLE_PREDICTION_FEATURES.size()> v3FeatureValues{{
			defenderHero ? 1.0 : 0.0,
			std::log(attackerArmyStrength),
			std::log(defenderArmyStrength),
			std::log((stackCount(visitor) + 1.0) / (stackCount(defender) + 1.0)),
			largestStackShare(visitor, attackerArmyStrength) - largestStackShare(defender, defenderArmyStrength),
			primarySkill(visitor, PrimarySkill::ATTACK) - primarySkill(defenderHero, PrimarySkill::ATTACK),
			primarySkill(visitor, PrimarySkill::DEFENSE) - primarySkill(defenderHero, PrimarySkill::DEFENSE),
			primarySkill(visitor, PrimarySkill::SPELL_POWER) - primarySkill(defenderHero, PrimarySkill::SPELL_POWER),
			primarySkill(visitor, PrimarySkill::KNOWLEDGE) - primarySkill(defenderHero, PrimarySkill::KNOWLEDGE),
			heroLevel(visitor) - heroLevel(defenderHero),
			manaRatio(visitor) - manaRatio(defenderHero),
			currentMana(visitor) - currentMana(defenderHero),
			manaLimit(visitor) - manaLimit(defenderHero),
			visitor->hasSpellbook() ? 1.0 : 0.0,
			(defenderHero && defenderHero->hasSpellbook()) ? 1.0 : 0.0,
			combatSpellCount(visitor) - combatSpellCount(defenderHero),
			std::log(heroStrengthOrOne(visitor) / heroStrengthOrOne(defenderHero)),
		}};
		requiredRatio = requiredRatioForModel(V3_BATTLE_PREDICTION_INTERCEPT, V3_BATTLE_PREDICTION_SAFE_PROBABILITY, V3_LOG_STRENGTH_RATIO, v3FeatureValues, V3_BATTLE_PREDICTION_FEATURES);
	}
	const double adjustedDanger = defenderStrength * requiredRatio / safeAttackRatio;

	if(!std::isfinite(adjustedDanger) || adjustedDanger <= 0)
		return fallbackDanger;
	if(adjustedDanger >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
		return std::numeric_limits<uint64_t>::max();

	return std::max<uint64_t>(1, static_cast<uint64_t>(adjustedDanger));
}

uint64_t calibrateDangerForVisitor(
	const CGHeroInstance * visitor,
	const CGObjectInstance * object,
	uint64_t fallbackDanger,
	BattlePredictionModel battlePredictionModel,
	float safeAttackRatio)
{
	if(battlePredictionModel != BattlePredictionModel::V2 && battlePredictionModel != BattlePredictionModel::V3)
		return fallbackDanger;

	const auto * defender = dynamic_cast<const CArmedInstance *>(object);
	if(!defender)
		return fallbackDanger;

	if(!objWithID<Obj::HERO>(object) && !objWithID<Obj::MONSTER>(object))
		return fallbackDanger;

	const auto * defenderHero = dynamic_cast<const CGHeroInstance *>(object);
	return calibrateDangerForVisitor(visitor, defender, defenderHero, fallbackDanger, battlePredictionModel, safeAttackRatio);
}
}

ui64 FuzzyHelper::evaluateDanger(const int3 & tile, const CGHeroInstance * visitor, bool checkGuards)
{
	auto cb = aiNk->cc.get();
	const TerrainTile * t = cb->getTile(tile, false);
	if(!t) //we can know about guard but can't check its tile (the edge of fow)
		return 190000000; //MUCH

	ui64 objectDanger = 0;
	ui64 guardDanger = 0;

	auto visitableObjects = cb->getVisitableObjs(tile);
	// in some scenarios hero happens to be "under" the object (eg town). Then we consider ONLY the hero.
	if(vstd::contains_if(visitableObjects, objWithID<Obj::HERO>))
	{
		vstd::erase_if(visitableObjects, [](const CGObjectInstance * obj) -> bool
		{
				return !objWithID<Obj::HERO>(obj);
		});
	}

	if(const CGObjectInstance * dangerousObject = vstd::backOrNull(visitableObjects))
	{
		objectDanger = evaluateDanger(dangerousObject); //unguarded objects can also be dangerous or unhandled

		if(objWithID<Obj::HERO>(dangerousObject))
		{
			auto hero = dynamic_cast<const CGHeroInstance *>(dangerousObject);

			if(hero->getVisitedTown() && !hero->getVisitedTown()->getGarrisonHero())
			{
				objectDanger += evaluateDanger(hero->getVisitedTown());
			}
			objectDanger *= aiNk->heroManager->getFightingStrengthCached(hero);
		}
		if (objWithID<Obj::TOWN>(dangerousObject))
		{
			auto town = dynamic_cast<const CGTownInstance*>(dangerousObject);
			auto hero = town->getGarrisonHero();

			if (hero)
				objectDanger *= aiNk->heroManager->getFightingStrengthCached(hero);
		}

		objectDanger = calibrateDangerForVisitor(visitor, dangerousObject, objectDanger, aiNk->settings->getBattlePredictionModel(), aiNk->settings->getSafeAttackRatio());

		if(dangerousObject->ID == Obj::SUBTERRANEAN_GATE)
		{
			//check guard on the other side of the gate
			auto it = aiNk->memory->knownSubterraneanGates.find(dangerousObject);
			if(it != aiNk->memory->knownSubterraneanGates.end())
			{
				auto guards = cb->getGuardingCreatures(it->second->visitablePos());

				for(auto cre : guards)
				{
					auto danger = evaluateDanger(cre);
					danger = calibrateDangerForVisitor(visitor, cre, danger, aiNk->settings->getBattlePredictionModel(), aiNk->settings->getSafeAttackRatio());
					vstd::amax(guardDanger, danger);
				}
			}
		}
	}

	if(checkGuards)
	{
		auto guards = cb->getGuardingCreatures(tile);
		for(auto cre : guards)
		{
			auto danger = evaluateDanger(cre);
			danger = calibrateDangerForVisitor(visitor, cre, danger, aiNk->settings->getBattlePredictionModel(), aiNk->settings->getSafeAttackRatio());
			vstd::amax(guardDanger, danger); //we are interested in strongest monster around
		}
	}

	//TODO mozna odwiedzic blockvis nie ruszajac straznika
	return std::max(objectDanger, guardDanger);
}

ui64 FuzzyHelper::evaluateDanger(const CGObjectInstance * obj)
{
	auto cb = aiNk->cc.get();

	if(obj->tempOwner.isValidPlayer() && cb->getPlayerRelations(obj->tempOwner, aiNk->playerID) != PlayerRelations::ENEMIES) //owned or allied objects don't pose any threat
		return 0;

	switch(obj->ID)
	{
	case Obj::TOWN:
	{
		const CGTownInstance * town = dynamic_cast<const CGTownInstance *>(obj);
		auto danger = town->getUpperArmy()->getArmyStrength();

		if(danger || town->getVisitingHero())
		{
			auto fortLevel = town->fortLevel();

			if (fortLevel == CGTownInstance::EFortLevel::CASTLE)
				danger += 10000;
			else if(fortLevel == CGTownInstance::EFortLevel::CITADEL)
				danger += 4000;
		}

		return danger;
	}

	case Obj::HERO:
	{
		const CGHeroInstance * hero = dynamic_cast<const CGHeroInstance *>(obj);
		return getHeroArmyStrengthWithCommander(hero, hero);
	}

	case Obj::ARTIFACT:
	case Obj::RESOURCE:
	{
		if(!vstd::contains(aiNk->memory->alreadyVisited, obj->id))
			return 0;
		[[fallthrough]];
	}
	default:
	{
		const CArmedInstance * a = dynamic_cast<const CArmedInstance *>(obj);
		if (a)
			return a->getArmyStrength();
		else
			return 0;
	}
	}
}
}
