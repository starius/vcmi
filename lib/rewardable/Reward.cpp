/*
 * Reward.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"
#include "Reward.h"

#include "../json/JsonBonus.h"
#include "../mapObjects/CGHeroInstance.h"
#include "../serializer/JsonSerializeFormat.h"
#include "../constants/StringConstants.h"
#include "../CSkillHandler.h"

namespace
{
constexpr std::array<std::string_view, 17> COMPONENT_TYPE_NAMES = {
	"none",
	"primarySkill",
	"secondarySkill",
	"resource",
	"resourcePerDay",
	"creature",
	"artifact",
	"spellScroll",
	"mana",
	"experience",
	"level",
	"spell",
	"morale",
	"luck",
	"building",
	"heroPortrait",
	"flag"
};

std::string componentTypeName(ComponentType type)
{
	const auto index = static_cast<int>(type) + 1;
	if(index < 0 || index >= static_cast<int>(COMPONENT_TYPE_NAMES.size()))
		return "none";

	return std::string(COMPONENT_TYPE_NAMES[index]);
}

ComponentType decodeComponentType(const std::string & type)
{
	for(size_t index = 0; index < COMPONENT_TYPE_NAMES.size(); ++index)
		if(COMPONENT_TYPE_NAMES[index] == type)
			return static_cast<ComponentType>(static_cast<int>(index) - 1);

	return ComponentType::NONE;
}

std::string componentSubtype(const Component & component)
{
	switch(component.type)
	{
		case ComponentType::PRIM_SKILL:
			return PrimarySkill::encode(component.subType.as<PrimarySkill>().getNum());
		case ComponentType::SEC_SKILL:
			return SecondarySkill::encode(component.subType.as<SecondarySkill>().getNum());
		case ComponentType::RESOURCE:
		case ComponentType::RESOURCE_PER_DAY:
			return GameResID::encode(component.subType.as<GameResID>().getNum());
		case ComponentType::CREATURE:
			return CreatureID::encode(component.subType.as<CreatureID>().getNum());
		case ComponentType::ARTIFACT:
			return ArtifactID::encode(component.subType.as<ArtifactID>().getNum());
		case ComponentType::SPELL_SCROLL:
		case ComponentType::SPELL:
			return SpellID::encode(component.subType.as<SpellID>().getNum());
		case ComponentType::HERO_PORTRAIT:
			return HeroTypeID::encode(component.subType.as<HeroTypeID>().getNum());
		case ComponentType::FLAG:
			return component.subType.as<PlayerColor>().toString();
		case ComponentType::NONE:
		case ComponentType::MANA:
		case ComponentType::EXPERIENCE:
		case ComponentType::LEVEL:
		case ComponentType::MORALE:
		case ComponentType::LUCK:
		case ComponentType::BUILDING:
			return "";
	}

	return "";
}

void decodeComponentSubtype(Component & component, const std::string & subtype)
{
	if(subtype.empty())
		return;

	switch(component.type)
	{
		case ComponentType::PRIM_SKILL:
			component.subType = PrimarySkill(PrimarySkill::decode(subtype));
			break;
		case ComponentType::SEC_SKILL:
			component.subType = SecondarySkill(SecondarySkill::decode(subtype));
			break;
		case ComponentType::RESOURCE:
		case ComponentType::RESOURCE_PER_DAY:
			component.subType = GameResID(GameResID::decode(subtype));
			break;
		case ComponentType::CREATURE:
			component.subType = CreatureID(CreatureID::decode(subtype));
			break;
		case ComponentType::ARTIFACT:
			component.subType = ArtifactID(ArtifactID::decode(subtype));
			break;
		case ComponentType::SPELL_SCROLL:
		case ComponentType::SPELL:
			component.subType = SpellID(SpellID::decode(subtype));
			break;
		case ComponentType::HERO_PORTRAIT:
			component.subType = HeroTypeID(HeroTypeID::decode(subtype));
			break;
		case ComponentType::FLAG:
			component.subType = PlayerColor(PlayerColor::decode(subtype));
			break;
		case ComponentType::NONE:
		case ComponentType::MANA:
		case ComponentType::EXPERIENCE:
		case ComponentType::LEVEL:
		case ComponentType::MORALE:
		case ComponentType::LUCK:
		case ComponentType::BUILDING:
			break;
	}
}

void serializeBuildingComponent(JsonSerializeFormat & handler, Component & component)
{
	auto building = handler.enterStruct("building");
	FactionID faction = component.subType.as<BuildingTypeUniqueID>().getFaction();
	BuildingID buildingID = component.subType.as<BuildingTypeUniqueID>().getBuilding();
	building->serializeId("faction", faction, FactionID::NONE);
	if(handler.saving)
	{
		std::string buildingName = BuildingID::encode(buildingID.getNum());
		building->serializeString("building", buildingName);
	}
	else
	{
		std::string buildingName;
		building->serializeString("building", buildingName);
		buildingID = BuildingID(BuildingID::decode(buildingName));
	}

	if(!handler.saving)
		component.subType = BuildingTypeUniqueID(faction, buildingID);
}

void serializeComponent(JsonSerializeFormat & handler, Component & component)
{
	if(handler.saving)
	{
		std::string type = componentTypeName(component.type);
		handler.serializeString("type", type);
		std::string subtype = componentSubtype(component);
		handler.serializeString("subtype", subtype);
	}
	else
	{
		std::string type;
		handler.serializeString("type", type);
		component.type = decodeComponentType(type);
		std::string subtype;
		handler.serializeString("subtype", subtype);
		decodeComponentSubtype(component, subtype);
	}

	if(component.type == ComponentType::BUILDING)
		serializeBuildingComponent(handler, component);

	handler.serializeInt("value", component.value);
}

void serializeBonusList(JsonSerializeFormat & handler, const std::string & fieldName, std::vector<std::shared_ptr<Bonus>> & bonuses)
{
	JsonNode node;
	if(handler.saving)
	{
		if(bonuses.empty())
			return;
		for(const auto & bonus : bonuses)
			node.Vector().push_back(bonus->toJsonNode());
	}

	handler.serializeRaw(fieldName, node, {});

	if(!handler.saving)
	{
		bonuses.clear();
		for(const auto & bonusNode : node.Vector())
			bonuses.push_back(JsonUtils::parseBonus(bonusNode));
	}
}
}

void Rewardable::RewardRevealTiles::serializeJson(JsonSerializeFormat & handler)
{
	handler.serializeBool("hide", hide);
	handler.serializeInt("scoreSurface", scoreSurface);
	handler.serializeInt("scoreSubterra", scoreSubterra);
	handler.serializeInt("scoreWater", scoreWater);
	handler.serializeInt("scoreRock", scoreRock);
	handler.serializeInt("radius", radius);
}

Rewardable::Reward::Reward()
	: heroExperience(0)
	, heroLevel(0)
	, manaDiff(0)
	, manaPercentage(-1)
	, manaOverflowFactor(0)
	, movePoints(0)
	, movePercentage(-1)
	, moveOverflowFactor(100)
	, primary(4, 0)
	, removeObject(false)
	, spellCast(SpellID::NONE, MasteryLevel::NONE)
{
}

Rewardable::Reward::~Reward() = default;

si32 Rewardable::Reward::calculateManaPoints(const CGHeroInstance * hero) const
{
	si32 manaScaled = hero->mana;
	if (manaPercentage >= 0)
		manaScaled = hero->manaLimit() * manaPercentage / 100;

	si32 manaMissing   = std::max(0, hero->manaLimit() - manaScaled);
	si32 manaGranted   = std::min(manaMissing, manaDiff);
	si32 manaOverflow  = manaDiff - manaGranted;
	si32 manaOverLimit = manaOverflow * manaOverflowFactor / 100;
	si32 manaOutput    = manaScaled + manaGranted + manaOverLimit;

	return manaOutput;
}

si32 Rewardable::Reward::calculateMovePoints(const CGHeroInstance * hero) const
{
	si32 moveScaled = hero->movementPointsRemaining();
	si32 moveLimit = hero->movementPointsLimit();

	if (movePercentage >= 0)
		moveScaled = moveLimit * movePercentage / 100;

	si32 moveMissing   = std::max(0, moveLimit - moveScaled);
	si32 moveGranted   = std::min(moveMissing, movePoints);
	si32 moveOverflow  = movePoints - moveGranted;
	si32 moveOverLimit = moveOverflow * moveOverflowFactor / 100;
	si32 moveOutput    = moveScaled + moveGranted + moveOverLimit;

	return std::max(0, moveOutput);
}

Component Rewardable::Reward::getDisplayedComponent(const CGHeroInstance * h) const
{
	std::vector<Component> comps;
	loadComponents(comps, h);

	if (!comps.empty())
		return comps.front();

	// Rewardable requested component that represent such rewards, to be used as button in UI selection dialog, e.g. Chest with its experience / money pick
	// However reward is either completely empty OR has no rewards that target hero can receive OR these rewards have no visible component (e.g. movement)
	// Such cases are unreachable in H3, however can be reached by mods
	logMod->warn("Failed to find displayed component for reward!");
	return Component(ComponentType::NONE, 0);
}

void Rewardable::Reward::loadComponents(std::vector<Component> & comps, const CGHeroInstance * h) const
{
	for (auto comp : extraComponents)
		comps.push_back(comp);
	
	for (auto & bonus : heroBonuses)
	{
		if (bonus->type == BonusType::MORALE)
			comps.emplace_back(ComponentType::MORALE, bonus->val);
		if (bonus->type == BonusType::LUCK)
			comps.emplace_back(ComponentType::LUCK, bonus->val);
	}
	
	if (heroExperience)
		comps.emplace_back(ComponentType::EXPERIENCE, static_cast<si32>(h ? h->calculateXp(heroExperience) : heroExperience));

	if (heroLevel)
		comps.emplace_back(ComponentType::LEVEL, heroLevel);

	if (manaDiff || manaPercentage >= 0)
		comps.emplace_back(ComponentType::MANA, h ? (calculateManaPoints(h) - h->mana) : manaDiff);

	for (size_t i=0; i<primary.size(); i++)
	{
		if (primary[i] != 0)
			comps.emplace_back(ComponentType::PRIM_SKILL, PrimarySkill(i), primary[i]);
	}

	for(const auto & entry : secondary)
	{
		auto skillID = entry.first;
		int levelsGained = entry.second;
		int currentLevel = h ? h->getSecSkillLevel(skillID) : 0;
		int finalLevel = std::clamp<int>(currentLevel + levelsGained, MasteryLevel::NONE, MasteryLevel::EXPERT);
		if (finalLevel == MasteryLevel::NONE)
			comps.emplace_back(ComponentType::SEC_SKILL, entry.first);
		else
			comps.emplace_back(ComponentType::SEC_SKILL, entry.first, finalLevel);
	}

	for(const auto & entry : grantedArtifacts)
		comps.emplace_back(ComponentType::ARTIFACT, entry);

	for(const auto & entry : takenArtifacts)
		comps.emplace_back(ComponentType::ARTIFACT, entry);

	for(const auto & entry : takenArtifactSlots)
	{
		if (h)
		{
			const auto & slotContent = h->getSlot(entry);
			if (slotContent->artifactID.hasValue())
				comps.emplace_back(ComponentType::ARTIFACT, slotContent->getArt()->getTypeId());
		}
	}

	for(const SpellID & spell : grantedScrolls)
		comps.emplace_back(ComponentType::SPELL, spell);

	for(const SpellID & spell : takenScrolls)
		comps.emplace_back(ComponentType::SPELL, spell);

	for(const auto & entry : spells)
	{
		bool learnable = !h || h->canLearnSpell(entry.toEntity(LIBRARY), true);
		comps.emplace_back(ComponentType::SPELL, entry, learnable ?	0 : -1);
	}

	for(const auto & entry : creatures)
		comps.emplace_back(ComponentType::CREATURE, entry.getId(), entry.getCount());

	for (size_t i=0; i<resources.size(); i++)
	{
		if (resources[i] !=0)
			comps.emplace_back(ComponentType::RESOURCE, GameResID(i), resources[i]);
	}
}

void Rewardable::Reward::serializeJson(JsonSerializeFormat & handler)
{
	resources.serializeJson(handler, "resources");
	handler.enterArray("extraComponents").serializeStruct<Component>(extraComponents, serializeComponent);
	handler.serializeBool("removeObject", removeObject);
	handler.serializeInt("manaPercentage", manaPercentage);
	handler.serializeInt("movePercentage", movePercentage);
	handler.serializeInt("heroExperience", heroExperience);
	handler.serializeInt("heroLevel", heroLevel);
	handler.serializeInt("manaDiff", manaDiff);
	handler.serializeInt("manaOverflowFactor", manaOverflowFactor);
	handler.serializeInt("movePoints", movePoints);
	handler.serializeInt("moveOverflowFactor", moveOverflowFactor);
	handler.enterArray("guards").serializeStruct(guards);
	serializeBonusList(handler, "bonuses", heroBonuses);
	serializeBonusList(handler, "commanderBonuses", commanderBonuses);
	serializeBonusList(handler, "playerBonuses", playerBonuses);
	handler.serializeIdArray("artifacts", grantedArtifacts);
	handler.serializeIdArray("takenArtifacts", takenArtifacts);
	handler.serializeIdArray("takenArtifactSlots", takenArtifactSlots);
	handler.serializeIdArray("scrolls", grantedScrolls);
	handler.serializeIdArray("takenScrolls", takenScrolls);
	handler.serializeIdArray("spells", spells);
	handler.enterArray("creatures").serializeStruct(creatures);
	handler.enterArray("takenCreatures").serializeStruct(takenCreatures);
	handler.enterArray("primary").serializeArray(primary);
	{
		auto a = handler.enterArray("secondary");
		std::vector<std::pair<SecondarySkill, si32>> fieldValue(secondary.begin(), secondary.end());
		a.serializeStruct<std::pair<SecondarySkill, si32>>(fieldValue, [](JsonSerializeFormat & h, std::pair<SecondarySkill, si32> & e)
		{
			h.serializeId("skill", e.first);
			h.serializeId("level", e.second, 0, [](const std::string & i){return vstd::find_pos(NSecondarySkill::levels, i);}, [](si32 i){return NSecondarySkill::levels.at(i);});
		});
		a.syncSize(fieldValue);
		secondary = std::map<SecondarySkill, si32>(fieldValue.begin(), fieldValue.end());
	}
	
	{
		auto a = handler.enterArray("creaturesChange");
		std::vector<std::pair<CreatureID, CreatureID>> fieldValue(creaturesChange.begin(), creaturesChange.end());
		a.serializeStruct<std::pair<CreatureID, CreatureID>>(fieldValue, [](JsonSerializeFormat & h, std::pair<CreatureID, CreatureID> & e)
		{
			h.serializeId("creature", e.first, CreatureID{});
			h.serializeId("amount", e.second, CreatureID{});
		});
		creaturesChange = std::map<CreatureID, CreatureID>(fieldValue.begin(), fieldValue.end());
	}
	if(handler.saving)
	{
		if(revealTiles)
			handler.serializeStruct("revealTiles", *revealTiles);
	}
	else
	{
		if(!handler.getCurrent()["revealTiles"].isNull())
		{
			revealTiles = RewardRevealTiles();
			handler.serializeStruct("revealTiles", *revealTiles);
		}
		else
		{
			revealTiles.reset();
		}
	}
	
	{
		auto a = handler.enterStruct("spellCast");
		a->serializeId("spell", spellCast.first, SpellID{});
		a->serializeInt("level", spellCast.second);
	}
}
