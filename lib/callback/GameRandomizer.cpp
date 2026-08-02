/*
 * GameRandomizer.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "GameRandomizer.h"

#include "IGameInfoCallback.h"

#include "../CRandomGenerator.h"
#include "../GameLibrary.h"
#include "../CCreatureHandler.h"
#include "../CSkillHandler.h"
#include "../IGameSettings.h"
#include "../json/JsonNode.h"
#include "../entities/artifact/CArtHandler.h"
#include "../entities/artifact/EArtifactClass.h"
#include "../entities/hero/CHeroClass.h"
#include "../mapObjects/CGHeroInstance.h"

namespace
{
const JsonNode & requireField(const JsonNode & node, const char * field)
{
	if(!node.isStruct())
		throw std::runtime_error(std::string("VGT randomizer state parent is not a mapping while reading: ") + field);

	const auto iter = node.Struct().find(field);
	if(iter == node.Struct().end() || iter->second.isNull())
		throw std::runtime_error(std::string("Missing VGT randomizer state field: ") + field);
	return iter->second;
}

const JsonNode * findField(const JsonNode & node, const char * field)
{
	if(!node.isStruct())
		return nullptr;

	const auto iter = node.Struct().find(field);
	if(iter == node.Struct().end() || iter->second.isNull())
		return nullptr;
	return &iter->second;
}

std::string requireString(const JsonNode & node, const char * field)
{
	const auto & child = requireField(node, field);
	if(!child.isString())
		throw std::runtime_error(std::string("VGT randomizer state field is not a string: ") + field);
	return child.String();
}

int requireInteger(const JsonNode & node, const char * field)
{
	const auto & child = requireField(node, field);
	if(!child.isNumber())
		throw std::runtime_error(std::string("VGT randomizer state field is not a number: ") + field);
	return static_cast<int>(child.Integer());
}

std::string vgtScopedIdentifier(std::string value)
{
	std::replace(value.begin(), value.end(), ':', '/');
	return value;
}

std::string vgtReadableIdentifier(std::string value)
{
	value = vgtScopedIdentifier(std::move(value));
	static const std::string corePrefix = "core/";
	if(value.starts_with(corePrefix))
		value.erase(0, corePrefix.size());
	return value;
}

std::string engineScopedIdentifier(std::string value)
{
	std::replace(value.begin(), value.end(), '/', ':');
	return value;
}
}

bool RandomizationBias::roll(vstd::RNG & generator, int successChance, int totalWeight, int biasValue)
{
	assert(successChance > 0);
	assert(totalWeight >= successChance);

	int failChance = totalWeight - successChance;
	int newRoll = generator.nextInt(1, totalWeight);
	// accumulated bias is stored as premultiplied to avoid precision loss on division
	// so multiply everything else in equation to compensate
	// precision loss is small, and generally insignificant, but better to play it safe
	bool success = newRoll * totalWeight - accumulatedBias <= successChance * totalWeight;
	if(success)
		accumulatedBias -= failChance * biasValue;
	else
		accumulatedBias += successChance * biasValue;

	return success;
}

int32_t RandomizationBias::getAccumulatedBias() const
{
	return accumulatedBias;
}

void RandomizationBias::setAccumulatedBias(int32_t value)
{
	accumulatedBias = value;
}

RandomGeneratorWithBias::RandomGeneratorWithBias(int seed)
	: generator(seed)
{
}

bool RandomGeneratorWithBias::roll(int successChance, int totalWeight, int biasValue)
{
	return bias.roll(generator, successChance, totalWeight, biasValue);
}

JsonNode RandomGeneratorWithBias::toVGTJson() const
{
	JsonNode result;
	result["generator"].String() = generator.getSerializedState();
	if(bias.getAccumulatedBias() != 0)
		result["bias"].Integer() = bias.getAccumulatedBias();
	return result;
}

void RandomGeneratorWithBias::loadVGTJson(const JsonNode & node)
{
	generator.setSerializedState(requireString(node, "generator"));
	if(const auto * biasNode = findField(node, "bias"))
	{
		if(!biasNode->isNumber())
			throw std::runtime_error("VGT randomizer state field is not a number: bias");
		bias.setAccumulatedBias(static_cast<int32_t>(biasNode->Integer()));
	}
	else
		bias.setAccumulatedBias(0);
}

GameRandomizer::GameRandomizer(const IGameInfoCallback & gameInfo)
	: gameInfo(gameInfo)
{
}

GameRandomizer::~GameRandomizer() = default;


bool GameRandomizer::rollMoraleLuck(std::map<ObjectInstanceID, RandomGeneratorWithBias> & seeds, ObjectInstanceID actor, int moraleLuckValue, EGameSettings biasValueSetting, EGameSettings diceSizeSetting, EGameSettings chanceVectorSetting)
{
	assert(moraleLuckValue > 0);
	auto chanceVector = gameInfo.getSettings().getVector(chanceVectorSetting);
	int diceSize = gameInfo.getSettings().getInteger(diceSizeSetting);
	int biasValue = gameInfo.getSettings().getInteger(biasValueSetting);
	size_t chanceIndex = std::min<size_t>(chanceVector.size(), moraleLuckValue) - 1; // array index, so 0-indexed

	if(!seeds.count(actor))
		seeds.try_emplace(actor, getDefault().nextInt());

	if(chanceVector.empty())
		return false;

	return seeds.at(actor).roll(chanceVector[chanceIndex], diceSize, biasValue);
}

bool GameRandomizer::rollGoodMorale(ObjectInstanceID actor, int moraleValue)
{
	return rollMoraleLuck(goodMoraleSeed, actor, moraleValue, EGameSettings::COMBAT_MORALE_BIAS, EGameSettings::COMBAT_MORALE_DICE_SIZE, EGameSettings::COMBAT_GOOD_MORALE_CHANCE);
}

bool GameRandomizer::rollBadMorale(ObjectInstanceID actor, int moraleValue)
{
	return rollMoraleLuck(badMoraleSeed, actor, moraleValue, EGameSettings::COMBAT_MORALE_BIAS, EGameSettings::COMBAT_MORALE_DICE_SIZE, EGameSettings::COMBAT_BAD_MORALE_CHANCE);
}

bool GameRandomizer::rollGoodLuck(ObjectInstanceID actor, int luckValue)
{
	return rollMoraleLuck(goodLuckSeed, actor, luckValue, EGameSettings::COMBAT_LUCK_BIAS, EGameSettings::COMBAT_LUCK_DICE_SIZE, EGameSettings::COMBAT_GOOD_LUCK_CHANCE);
}

bool GameRandomizer::rollBadLuck(ObjectInstanceID actor, int luckValue)
{
	return rollMoraleLuck(badLuckSeed, actor, luckValue, EGameSettings::COMBAT_LUCK_BIAS, EGameSettings::COMBAT_LUCK_DICE_SIZE, EGameSettings::COMBAT_BAD_LUCK_CHANCE);
}

bool GameRandomizer::rollCombatAbility(ObjectInstanceID actor, int percentageChance)
{
	if(!combatAbilitySeed.count(actor))
		combatAbilitySeed.try_emplace(actor, getDefault().nextInt());

	if(percentageChance <= 0)
		return false;

	if(percentageChance >= 100)
		return true;

	int biasValue = gameInfo.getSettings().getInteger(EGameSettings::COMBAT_ABILITY_BIAS);

	return combatAbilitySeed.at(actor).roll(percentageChance, 100, biasValue);
}

CreatureID GameRandomizer::rollCreature()
{
	std::vector<CreatureID> allowed;
	for(const auto & creatureID : LIBRARY->creh->getDefaultAllowed())
	{
		const auto * creaturePtr = creatureID.toCreature();
		if(!creaturePtr->excludeFromRandomization)
			allowed.push_back(creaturePtr->getId());
	}

	if(allowed.empty())
		throw std::runtime_error("Cannot pick a random creature!");

	return *RandomGeneratorUtil::nextItem(allowed, getDefault());
}

CreatureID GameRandomizer::rollCreature(int tier)
{
	std::vector<CreatureID> allowed;
	for(const auto & creatureID : LIBRARY->creh->getDefaultAllowed())
	{
		const auto * creaturePtr = creatureID.toCreature();
		if(creaturePtr->excludeFromRandomization)
			continue;

		if(creaturePtr->getLevel() == tier)
			allowed.push_back(creaturePtr->getId());
	}

	if(allowed.empty())
		throw std::runtime_error("Cannot pick a random creature!");

	return *RandomGeneratorUtil::nextItem(allowed, getDefault());
}

ArtifactID GameRandomizer::rollArtifact()
{
	std::set<ArtifactID> potentialPicks;

	for(const auto & artifactID : LIBRARY->arth->getDefaultAllowed())
	{
		if(!LIBRARY->arth->legalArtifact(artifactID))
			continue;

		potentialPicks.insert(artifactID);
	}

	return rollArtifact(potentialPicks);
}

ArtifactID GameRandomizer::rollArtifact(EArtifactClass type)
{
	std::set<ArtifactID> potentialPicks;

	for(const auto & artifactID : LIBRARY->arth->getDefaultAllowed())
	{
		if(!LIBRARY->arth->legalArtifact(artifactID))
			continue;

		if(!gameInfo.isAllowed(artifactID))
			continue;

		const auto * artifact = artifactID.toArtifact();

		if(type != artifact->aClass)
			continue;

		potentialPicks.insert(artifactID);
	}

	return rollArtifact(potentialPicks);
}

ArtifactID GameRandomizer::rollArtifact(std::set<ArtifactID> potentialPicks)
{
	// No allowed artifacts at all - give Grail - this can't be banned (hopefully)
	// FIXME: investigate how such cases are handled by H3 - some heavily customized user-made maps likely rely on H3 behavior
	if(potentialPicks.empty())
	{
		logGlobal->warn("Failed to find artifact that matches requested parameters!");
		return ArtifactID::GRAIL;
	}

	// Find how many times least used artifacts were picked by randomizer
	int leastUsedTimes = std::numeric_limits<int>::max();
	for(const auto & artifact : potentialPicks)
		if(allocatedArtifacts[artifact] < leastUsedTimes)
			leastUsedTimes = allocatedArtifacts[artifact];

	// Pick all artifacts that were used least number of times
	std::set<ArtifactID> preferredPicks;
	for(const auto & artifact : potentialPicks)
		if(allocatedArtifacts[artifact] == leastUsedTimes)
			preferredPicks.insert(artifact);

	assert(!preferredPicks.empty());

	ArtifactID artID = *RandomGeneratorUtil::nextItem(preferredPicks, getDefault());
	allocatedArtifacts[artID] += 1; // record +1 more usage
	return artID;
}

std::vector<ArtifactID> GameRandomizer::rollMarketArtifactSet()
{
	return {
		rollArtifact(EArtifactClass::ART_TREASURE),
		rollArtifact(EArtifactClass::ART_TREASURE),
		rollArtifact(EArtifactClass::ART_TREASURE),
		rollArtifact(EArtifactClass::ART_MINOR),
		rollArtifact(EArtifactClass::ART_MINOR),
		rollArtifact(EArtifactClass::ART_MINOR),
		rollArtifact(EArtifactClass::ART_MAJOR)
	};
}

vstd::RNG & GameRandomizer::getDefault()
{
	return globalRandomNumberGenerator;
}

void GameRandomizer::setSeed(int newSeed)
{
	globalRandomNumberGenerator.setSeed(newSeed);
}

int GameRandomizer::getDefaultSeed() const
{
	return globalRandomNumberGenerator.getSeed();
}

JsonNode GameRandomizer::heroSkillVGTJson(const std::set<HeroTypeID> * heroes) const
{
	JsonNode result;
	result.Struct();
	for(const auto & [hero, state] : heroSkillSeed)
	{
		if(heroes && !heroes->contains(hero))
			continue;
		JsonNode heroState;
		heroState["seed"].String() = state.seed.getSerializedState();
		heroState["magicSchoolCounter"].Integer() = state.magicSchoolCounter;
		heroState["wisdomCounter"].Integer() = state.wisdomCounter;
		result[vgtReadableIdentifier(HeroTypeID::encode(hero.getNum()))] = heroState;
	}
	return result;
}

void GameRandomizer::loadHeroSkillVGTJson(const JsonNode * heroSkills, bool clearExisting)
{
	if(clearExisting)
		heroSkillSeed.clear();
	if(!heroSkills)
		return;
	if(!heroSkills->isStruct())
		throw std::runtime_error("VGT randomizer heroSkill field is not a mapping");
	for(const auto & entry : heroSkills->Struct())
	{
		const HeroTypeID hero(HeroTypeID::decode(engineScopedIdentifier(entry.first)));
		auto [iter, inserted] = heroSkillSeed.try_emplace(hero);
		iter->second.seed.setSerializedState(requireString(entry.second, "seed"));
		iter->second.magicSchoolCounter = static_cast<int8_t>(requireInteger(entry.second, "magicSchoolCounter"));
		iter->second.wisdomCounter = static_cast<int8_t>(requireInteger(entry.second, "wisdomCounter"));
	}
}

JsonNode GameRandomizer::toVGTJson() const
{
	JsonNode result;
	result["global"].String() = globalRandomNumberGenerator.getSerializedState();

	for(const auto & [artifact, count] : allocatedArtifacts)
	{
		if(count != 0)
			result["allocatedArtifacts"][vgtScopedIdentifier(ArtifactID::encode(artifact.getNum()))].Integer() = count;
	}

	const JsonNode heroSkills = heroSkillVGTJson(nullptr);
	if(!heroSkills.Struct().empty())
		result["heroSkill"] = heroSkills;

	auto writeBiasMap = [](const auto & source) -> JsonNode
	{
		JsonNode result;
		result.Vector();
		for(const auto & [object, generator] : source)
		{
			JsonNode entry;
			entry["object"].Integer() = object.getNum();
			entry["state"] = generator.toVGTJson();
			result.Vector().push_back(entry);
		}
		return result;
	};

	if(!goodMoraleSeed.empty())
		result["goodMorale"] = writeBiasMap(goodMoraleSeed);
	if(!badMoraleSeed.empty())
		result["badMorale"] = writeBiasMap(badMoraleSeed);
	if(!goodLuckSeed.empty())
		result["goodLuck"] = writeBiasMap(goodLuckSeed);
	if(!badLuckSeed.empty())
		result["badLuck"] = writeBiasMap(badLuckSeed);
	if(!combatAbilitySeed.empty())
		result["combatAbility"] = writeBiasMap(combatAbilitySeed);
	return result;
}

JsonNode GameRandomizer::toVGTBattleJson(
	const std::set<ObjectInstanceID> & participants,
	const std::set<HeroTypeID> & heroes) const
{
	JsonNode result;
	result["global"].String() = globalRandomNumberGenerator.getSerializedState();
	const JsonNode heroSkills = heroSkillVGTJson(&heroes);
	if(!heroSkills.Struct().empty())
		result["heroSkill"] = heroSkills;

	auto writeBiasMap = [&participants](const auto & source) -> JsonNode
	{
		JsonNode result;
		result.Struct();
		for(const auto & [object, generator] : source)
		{
			if(!participants.contains(object))
				continue;
			const JsonNode state = generator.toVGTJson();
			const std::string serialized = requireString(state, "generator");
			size_t parsed = 0;
			const int64_t generatorState = std::stoll(serialized, &parsed);
			if(parsed != serialized.size())
				throw std::runtime_error("VGT battle randomizer generator state is not an integer");

			auto & entry = result[std::to_string(object.getNum())];
			if(const auto * bias = findField(state, "bias"))
			{
				entry["generator"].Integer() = generatorState;
				entry["bias"].Integer() = bias->Integer();
			}
			else
				entry.Integer() = generatorState;
		}
		return result;
	};

	for(const auto & [name, source] : {
		std::pair{"goodMorale", &goodMoraleSeed},
		std::pair{"badMorale", &badMoraleSeed},
		std::pair{"goodLuck", &goodLuckSeed},
		std::pair{"badLuck", &badLuckSeed},
		std::pair{"combatAbility", &combatAbilitySeed}})
	{
		const auto values = writeBiasMap(*source);
		if(!values.Struct().empty())
			result[name] = values;
	}
	return result;
}

void GameRandomizer::loadVGTJson(const JsonNode & node)
{
	globalRandomNumberGenerator.setSerializedState(requireString(node, "global"));

	allocatedArtifacts.clear();
	if(const auto * artifacts = findField(node, "allocatedArtifacts"))
	{
		if(!artifacts->isStruct())
			throw std::runtime_error("VGT randomizer allocatedArtifacts field is not a mapping");
		for(const auto & entry : artifacts->Struct())
		{
			if(!entry.second.isNumber())
				throw std::runtime_error("VGT randomizer allocated artifact count is not a number: " + entry.first);
			allocatedArtifacts[ArtifactID(ArtifactID::decode(engineScopedIdentifier(entry.first)))] = static_cast<int>(entry.second.Integer());
		}
	}

	loadHeroSkillVGTJson(findField(node, "heroSkill"), true);

	auto readBiasMap = [](auto & target, const JsonNode * source, const std::string & name)
	{
		target.clear();
		if(!source)
			return;
		if(!source->isVector())
			throw std::runtime_error("VGT randomizer " + name + " field is not a list");
		for(const auto & entry : source->Vector())
		{
			const ObjectInstanceID object(requireInteger(entry, "object"));
			auto [iter, inserted] = target.try_emplace(object);
			iter->second.loadVGTJson(requireField(entry, "state"));
		}
	};

	readBiasMap(goodMoraleSeed, findField(node, "goodMorale"), "goodMorale");
	readBiasMap(badMoraleSeed, findField(node, "badMorale"), "badMorale");
	readBiasMap(goodLuckSeed, findField(node, "goodLuck"), "goodLuck");
	readBiasMap(badLuckSeed, findField(node, "badLuck"), "badLuck");
	readBiasMap(combatAbilitySeed, findField(node, "combatAbility"), "combatAbility");
}

void GameRandomizer::loadVGTBattleJson(
	const JsonNode & node,
	const std::set<ObjectInstanceID> & participants,
	const std::set<HeroTypeID> & heroes)
{
	globalRandomNumberGenerator.setSerializedState(requireString(node, "global"));
	for(const auto hero : heroes)
		heroSkillSeed.erase(hero);
	loadHeroSkillVGTJson(findField(node, "heroSkill"), false);

	auto readBiasMap = [&participants](auto & target, const JsonNode * source, const std::string & name)
	{
		for(const auto participant : participants)
			target.erase(participant);
		if(!source)
			return;
		if(!source->isStruct())
			throw std::runtime_error("VGT battle randomizer " + name + " field is not a mapping");
		for(const auto & [objectText, value] : source->Struct())
		{
			size_t parsed = 0;
			const int64_t objectValue = std::stoll(objectText, &parsed);
			if(parsed != objectText.size() || objectValue < 0 || objectValue > std::numeric_limits<si32>::max())
				throw std::runtime_error("VGT battle randomizer " + name + " object is invalid: " + objectText);
			const ObjectInstanceID object(static_cast<si32>(objectValue));
			JsonNode state;
			if(value.isNumber())
				state["generator"].String() = std::to_string(value.Integer());
			else if(value.isStruct())
			{
				state["generator"].String() = std::to_string(requireInteger(value, "generator"));
				if(const auto * bias = findField(value, "bias"))
				{
					if(!bias->isNumber())
						throw std::runtime_error("VGT battle randomizer " + name + " bias is invalid: " + objectText);
					state["bias"].Integer() = bias->Integer();
				}
			}
			else
				throw std::runtime_error("VGT battle randomizer " + name + " state is invalid: " + objectText);
			auto [iter, inserted] = target.try_emplace(object);
			iter->second.loadVGTJson(state);
		}
	};

	readBiasMap(goodMoraleSeed, findField(node, "goodMorale"), "goodMorale");
	readBiasMap(badMoraleSeed, findField(node, "badMorale"), "badMorale");
	readBiasMap(goodLuckSeed, findField(node, "goodLuck"), "goodLuck");
	readBiasMap(badLuckSeed, findField(node, "badLuck"), "badLuck");
	readBiasMap(combatAbilitySeed, findField(node, "combatAbility"), "combatAbility");
}

PrimarySkill GameRandomizer::rollPrimarySkillForLevelup(const CGHeroInstance * hero)
{
	if(!heroSkillSeed.count(hero->getHeroTypeID()))
		heroSkillSeed.try_emplace(hero->getHeroTypeID(), getDefault().nextInt());

	const bool isLowLevelHero = hero->level < GameConstants::HERO_HIGH_LEVEL;
	const auto & skillChances = isLowLevelHero ? hero->getHeroClass()->primarySkillLowLevel : hero->getHeroClass()->primarySkillHighLevel;
	auto & heroRng = heroSkillSeed.at(hero->getHeroTypeID());

	if(hero->isCampaignYog())
	{
		// Yog can only receive Attack or Defence on level-up
		std::vector<int> yogChances = {skillChances[0], skillChances[1]};
		return static_cast<PrimarySkill>(RandomGeneratorUtil::nextItemWeighted(yogChances, heroRng.seed));
	}
	return static_cast<PrimarySkill>(RandomGeneratorUtil::nextItemWeighted(skillChances, heroRng.seed));
}

SecondarySkill GameRandomizer::rollSecondarySkillForLevelup(const CGHeroInstance * hero, const std::set<SecondarySkill> & options)
{
	if(!heroSkillSeed.count(hero->getHeroTypeID()))
		heroSkillSeed.try_emplace(hero->getHeroTypeID(), getDefault().nextInt());

	auto & heroRng = heroSkillSeed.at(hero->getHeroTypeID());

	auto getObligatorySkills = [&options](bool magicSchools)
	{
		std::set<SecondarySkill> obligatory;
		for(const auto option : options)
			if(magicSchools ? option.toSkill()->isSpellSchool() : option.toSkill()->isWisdom())
				obligatory.insert(option); //Always return all obligatory skills

		return obligatory;
	};

	std::set<SecondarySkill> wisdomList = getObligatorySkills(false);
	std::set<SecondarySkill> schoolList = getObligatorySkills(true);

	bool wantsWisdom = heroRng.wisdomCounter >= hero->maxlevelsToWisdom();
	bool wantsSchool = heroRng.magicSchoolCounter >= hero->maxlevelsToMagicSchool();
	bool selectWisdom = wantsWisdom && !wisdomList.empty();
	bool selectSchool = !selectWisdom && wantsSchool && !schoolList.empty();

	std::set<SecondarySkill> actualCandidates;

	if(selectWisdom)
		actualCandidates = wisdomList;
	else if(selectSchool)
		actualCandidates = schoolList;
	else
		actualCandidates = options;

	assert(!actualCandidates.empty());

	std::vector<int> weights;
	std::vector<SecondarySkill> skills;

	for(const auto & possible : actualCandidates)
	{
		skills.push_back(possible);
		if(hero->getHeroClass()->secSkillProbability.count(possible) != 0)
		{
			int weight = hero->getHeroClass()->secSkillProbability.at(possible);
			weights.push_back(std::max(1, weight));
		}
		else
			weights.push_back(1); // H3 behavior - banned skills have minimal (1) chance to be picked
	}

	int selectedIndex = RandomGeneratorUtil::nextItemWeighted(weights, heroRng.seed);
	SecondarySkill selectedSkill = skills.at(selectedIndex);

	if((*LIBRARY->skillh)[selectedSkill]->isWisdom())
		heroRng.wisdomCounter = 0;
	if((*LIBRARY->skillh)[selectedSkill]->isSpellSchool())
		heroRng.magicSchoolCounter = 0;

	return selectedSkill;
}

std::vector<SecondarySkill> GameRandomizer::rollSecondarySkills(const CGHeroInstance * hero)
{
	auto & heroRng = heroSkillSeed.at(hero->getHeroTypeID());

	//deterministic secondary skills
	++heroRng.magicSchoolCounter;
	++heroRng.wisdomCounter;

	std::set<SecondarySkill> basicAndAdv;
	std::set<SecondarySkill> none;
	std::vector<SecondarySkill>	skills;

	if (hero->canLearnSkill())
	{
		for(int i = 0; i < LIBRARY->skillh->size(); i++)
			if (hero->canLearnSkill(SecondarySkill(i)))
				none.insert(SecondarySkill(i));
	}

	for(const auto & elem : hero->secSkills)
	{
		if(elem.second < MasteryLevel::EXPERT)
			basicAndAdv.insert(elem.first);
		none.erase(elem.first);
	}

	int maxUpgradedSkills = hero->cb->getSettings().getInteger(EGameSettings::LEVEL_UP_UPGRADED_SKILLS_AMOUNT);
	int maxTotalSkills = hero->cb->getSettings().getInteger(EGameSettings::LEVEL_UP_TOTAL_SKILLS_AMOUNT);
	int newSkillsAvailable = none.size();
	int upgradedSkillsToSelect = std::max(maxUpgradedSkills, maxTotalSkills - newSkillsAvailable);

	while (skills.size() < upgradedSkillsToSelect && !basicAndAdv.empty())
	{
		skills.push_back(rollSecondarySkillForLevelup(hero, basicAndAdv));
		basicAndAdv.erase(skills.back());
	}

	while (skills.size() < maxTotalSkills && !none.empty())
	{
		skills.push_back(rollSecondarySkillForLevelup(hero, none));
		none.erase(skills.back());
	}
	return skills;
}
