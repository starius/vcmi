/*
 * BattleSimulationFingerprint.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationFingerprint.h"

#include "BattleStartInfo.h"

#include "../../lib/bonuses/BonusEnum.h"
#include "../../lib/entities/artifact/CArtifactInstance.h"
#include "../../lib/entities/building/TownFortifications.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"
#include "../../lib/mapObjects/army/CStackInstance.h"

namespace BattleSimulation
{
namespace
{
enum class FingerprintSection : uint64_t
{
	LOCATION = 1,
	LAYOUT = 2,
	ARMY_SIDE = 3,
	ARMY = 4,
	STACK = 5,
	HERO_SIDE = 6,
	HERO = 7,
	ARTIFACT_SET = 8,
	ARTIFACT = 9,
	TOWN = 10,
	TOWN_FORTIFICATIONS = 11,
	TOWN_SPELLS = 12,
	BATTLE_START = 13,
};

uint64_t mix(uint64_t value)
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

void addSection(BattleSimulationFingerprintBuilder & builder, FingerprintSection section)
{
	builder.add(static_cast<uint64_t>(section));
}

template<typename Identifier>
void addIdentifier(BattleSimulationFingerprintBuilder & builder, const Identifier & identifier)
{
	builder.addSigned(identifier.getNum());
}

void addPosition(BattleSimulationFingerprintBuilder & builder, const int3 & position)
{
	builder.addSigned(position.x);
	builder.addSigned(position.y);
	builder.addSigned(position.z);
}

void addBattleHex(BattleSimulationFingerprintBuilder & builder, const BattleHex & hex)
{
	builder.addSigned(hex.toInt());
}

void addSpellList(BattleSimulationFingerprintBuilder & builder, const std::vector<SpellID> & spells)
{
	builder.add(spells.size());
	for(const auto & spell : spells)
		addIdentifier(builder, spell);
}

void addArtifact(BattleSimulationFingerprintBuilder & builder, const ArtSlotInfo & slotInfo)
{
	addSection(builder, FingerprintSection::ARTIFACT);
	builder.addBool(slotInfo.locked);
	addIdentifier(builder, slotInfo.getID());

	const auto * artifact = slotInfo.getArt();
	builder.addBool(artifact != nullptr);
	if(!artifact)
		return;

	addIdentifier(builder, artifact->getTypeId());
	addIdentifier(builder, artifact->getId());
	builder.addBool(artifact->isCombined());
	builder.addBool(artifact->isScroll());
	if(artifact->isScroll())
		addIdentifier(builder, artifact->getScrollSpellID());

	builder.add(artifact->getCharges());
	builder.addSigned(artifact->valOfBonuses(BonusType::ARTIFACT_GROWING));

	const auto & parts = artifact->getPartsInfo();
	builder.add(parts.size());
	for(const auto & part : parts)
	{
		addIdentifier(builder, part.getArtifactID());
		addIdentifier(builder, part.slot);
	}
}

void addArtifactSet(BattleSimulationFingerprintBuilder & builder, const CArtifactSet & artifacts)
{
	addSection(builder, FingerprintSection::ARTIFACT_SET);
	builder.add(artifacts.artifactsWorn.size());
	for(const auto & [slot, slotInfo] : artifacts.artifactsWorn)
	{
		addIdentifier(builder, slot);
		addArtifact(builder, slotInfo);
	}

	builder.add(artifacts.artifactsInBackpack.size());
	for(const auto & slotInfo : artifacts.artifactsInBackpack)
		addArtifact(builder, slotInfo);
}

void addStack(BattleSimulationFingerprintBuilder & builder, const CStackInstance & stack)
{
	addSection(builder, FingerprintSection::STACK);
	addIdentifier(builder, stack.getCreatureID());
	builder.addSigned(stack.getCount());
	builder.addSigned(stack.getTotalExperience());
	builder.addSigned(stack.getAverageExperience());
	builder.addSigned(stack.getLevel());
	builder.addSigned(stack.getExpRank());
	builder.addBool(stack.randomStack.has_value());
	if(stack.randomStack)
	{
		builder.add(stack.randomStack->level);
		builder.add(stack.randomStack->upgrade);
	}
	addArtifactSet(builder, stack);
}

void addArmy(BattleSimulationFingerprintBuilder & builder, const CArmedInstance * army)
{
	addSection(builder, FingerprintSection::ARMY);
	builder.addBool(army != nullptr);
	if(!army)
		return;

	addIdentifier(builder, army->id);
	addIdentifier(builder, army->ID);
	addIdentifier(builder, army->subID);
	addIdentifier(builder, army->getOwner());
	addPosition(builder, army->pos);
	addIdentifier(builder, army->getCurrentTerrain());
	builder.addSigned(static_cast<int64_t>(army->formation));

	const auto & slots = army->Slots();
	builder.add(slots.size());
	for(const auto & [slot, stack] : slots)
	{
		addIdentifier(builder, slot);
		builder.addBool(stack != nullptr);
		if(stack)
			addStack(builder, *stack);
	}
}

void addLayout(BattleSimulationFingerprintBuilder & builder, const BattleLayout & layout)
{
	addSection(builder, FingerprintSection::LAYOUT);
	for(const auto side : { BattleSide::ATTACKER, BattleSide::DEFENDER })
	{
		builder.addSigned(static_cast<int64_t>(side));
		for(const auto & hex : layout.units[side])
			addBattleHex(builder, hex);
		for(const auto & hex : layout.warMachines[side])
			addBattleHex(builder, hex);
		addBattleHex(builder, layout.commanders[side]);
	}
	builder.addBool(layout.tacticsAllowed);
	builder.addBool(layout.obstaclesAllowed);
}

void addHero(BattleSimulationFingerprintBuilder & builder, const CGHeroInstance * hero)
{
	addSection(builder, FingerprintSection::HERO);
	builder.addBool(hero != nullptr);
	if(!hero)
		return;

	addIdentifier(builder, hero->id);
	addIdentifier(builder, hero->getOwner());
	addIdentifier(builder, hero->ID);
	addIdentifier(builder, hero->subID);
	addPosition(builder, hero->pos);
	addIdentifier(builder, hero->getHeroTypeID());
	addIdentifier(builder, hero->getHeroClassID());
	builder.addSigned(hero->exp);
	builder.add(hero->level);
	builder.addSigned(hero->mana);
	builder.addSigned(hero->manaLimit());
	builder.addBool(hero->hasSpellbook());
	builder.addSigned(hero->maxSpellLevel());
	builder.addBool(hero->tacticFormationEnabled);
	builder.add(hero->moveDir);

	for(const auto & skill : PrimarySkill::ALL_SKILLS())
	{
		addIdentifier(builder, skill);
		builder.addSigned(hero->getPrimSkillLevel(skill));
		builder.addSigned(hero->getBasePrimarySkillValue(skill));
	}

	builder.add(hero->secSkills.size());
	for(const auto & [skill, level] : hero->secSkills)
	{
		addIdentifier(builder, skill);
		builder.add(level);
	}

	builder.add(hero->getSpellsInSpellbook().size());
	for(const auto & spell : hero->getSpellsInSpellbook())
		addIdentifier(builder, spell);

	addArtifactSet(builder, *hero);

	const auto * commander = hero->getCommander();
	builder.addBool(commander != nullptr);
	if(commander)
		addStack(builder, *commander);

	builder.add(hero->visitedObjects.size());
	for(const auto & objectID : hero->visitedObjects)
		addIdentifier(builder, objectID);
}

void addFortifications(BattleSimulationFingerprintBuilder & builder, const TownFortifications & fortifications)
{
	addSection(builder, FingerprintSection::TOWN_FORTIFICATIONS);
	addIdentifier(builder, fortifications.citadelShooter);
	addIdentifier(builder, fortifications.upperTowerShooter);
	addIdentifier(builder, fortifications.lowerTowerShooter);
	addIdentifier(builder, fortifications.moatSpell);
	builder.addSigned(fortifications.wallsHealth);
	builder.addSigned(fortifications.citadelHealth);
	builder.addSigned(fortifications.upperTowerHealth);
	builder.addSigned(fortifications.lowerTowerHealth);
	builder.addBool(fortifications.hasMoat);
}

void addTown(BattleSimulationFingerprintBuilder & builder, const CGTownInstance * town)
{
	addSection(builder, FingerprintSection::TOWN);
	builder.addBool(town != nullptr);
	if(!town)
		return;

	addIdentifier(builder, town->id);
	addIdentifier(builder, town->getOwner());
	addIdentifier(builder, town->ID);
	addIdentifier(builder, town->subID);
	addIdentifier(builder, town->getFactionID());
	addPosition(builder, town->pos);
	builder.addSigned(town->fortLevel());
	builder.addSigned(town->hallLevel());
	builder.addSigned(town->mageGuildLevel());
	builder.addSigned(town->getTownLevel());
	builder.addSigned(town->bonusValue.first);
	builder.addSigned(town->bonusValue.second);
	builder.addBool(town->spellResearchAllowed);
	builder.addSigned(town->spellResearchCounterDay);
	builder.addSigned(town->spellResearchAcceptedCounter);

	addFortifications(builder, town->fortificationsLevel());

	const auto buildings = town->getBuildings();
	builder.add(buildings.size());
	for(const auto & building : buildings)
		addIdentifier(builder, building);

	addSection(builder, FingerprintSection::TOWN_SPELLS);
	addSpellList(builder, town->possibleSpells);
	addSpellList(builder, town->obligatorySpells);
	builder.add(town->spells.size());
	for(const auto & spellsAtLevel : town->spells)
		addSpellList(builder, spellsAtLevel);

	const auto * garrisonHero = town->getGarrisonHero();
	const auto * visitingHero = town->getVisitingHero();
	builder.addBool(garrisonHero != nullptr);
	if(garrisonHero)
		addIdentifier(builder, garrisonHero->id);
	builder.addBool(visitingHero != nullptr);
	if(visitingHero)
		addIdentifier(builder, visitingHero->id);
}
}

bool isValidStateFingerprint(uint64_t fingerprint)
{
	return fingerprint != INVALID_STATE_FINGERPRINT;
}

uint64_t combineFingerprint(uint64_t fingerprint, uint64_t value)
{
	const auto result = fingerprint ^ mix(value + 0x9e3779b97f4a7c15ULL + (fingerprint << 6) + (fingerprint >> 2));
	return result == INVALID_STATE_FINGERPRINT ? 1 : result;
}

uint64_t fingerprintBattleStartInfo(const BattleStartInfo & setup)
{
	BattleSimulationFingerprintBuilder builder;
	addSection(builder, FingerprintSection::BATTLE_START);
	addSection(builder, FingerprintSection::LOCATION);
	addPosition(builder, setup.tile);
	addLayout(builder, setup.layout);
	addTown(builder, setup.town);

	for(const auto side : { BattleSide::ATTACKER, BattleSide::DEFENDER })
	{
		addSection(builder, FingerprintSection::ARMY_SIDE);
		builder.addSigned(static_cast<int64_t>(side));
		addArmy(builder, setup.armies[side]);

		addSection(builder, FingerprintSection::HERO_SIDE);
		builder.addSigned(static_cast<int64_t>(side));
		addHero(builder, setup.heroes[side]);
	}

	return builder.value();
}

void BattleSimulationFingerprintBuilder::add(uint64_t value)
{
	hasData = true;
	fingerprint = combineFingerprint(fingerprint, value);
}

void BattleSimulationFingerprintBuilder::addSigned(int64_t value)
{
	add(static_cast<uint64_t>(value));
}

void BattleSimulationFingerprintBuilder::addBool(bool value)
{
	add(value ? 1 : 0);
}

uint64_t BattleSimulationFingerprintBuilder::value() const
{
	return hasData ? fingerprint : INVALID_STATE_FINGERPRINT;
}
}
