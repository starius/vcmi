/*
 * VGTRecorder.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#include "StdInc.h"
#include "VGTRecorder.h"

#include "CGameHandler.h"
#include "VGTDiscovery.h"
#include "processors/HeroPoolProcessor.h"

#include "../Version.h"
#include "../lib/GameConstants.h"
#include "../lib/GameLibrary.h"
#include "../lib/IGameSettings.h"
#include "../lib/ResourceSet.h"
#include "../lib/StartInfo.h"
#include "../lib/VCMIDirs.h"
#include "../lib/CStack.h"
#include "../lib/CBonusTypeHandler.h"
#include "../lib/CPlayerState.h"
#include "../lib/battle/BattleAction.h"
#include "../lib/battle/Unit.h"
#include "../lib/bonuses/Bonus.h"
#include "../lib/callback/Calendar.h"
#include "../lib/callback/GameRandomizer.h"
#include "../lib/constants/StringConstants.h"
#include "../lib/constants/NumericConstants.h"
#include "../lib/filesystem/CInputStream.h"
#include "../lib/filesystem/Filesystem.h"
#include "../lib/gameState/GameStatistics.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/json/JsonNode.h"
#include "../lib/mapping/CMap.h"
#include "../lib/mapObjects/CGObjectInstance.h"
#include "../lib/mapObjects/CGCreature.h"
#include "../lib/mapObjects/CGDwelling.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapObjects/CGTownInstance.h"
#include "../lib/mapObjects/CQuest.h"
#include "../lib/mapObjects/IMarket.h"
#include "../lib/mapObjects/MiscObjects.h"
#include "../lib/mapObjects/army/CArmedInstance.h"
#include "../lib/mapObjects/army/CSimpleArmy.h"
#include "../lib/modding/CModHandler.h"
#include "../lib/networkPacks/NetPackVisitor.h"
#include "../lib/rmg/CMapGenOptions.h"
#include "../lib/serializer/CSaveFile.h"
#include "../lib/serializer/JsonSerializer.h"
#include "../lib/texts/MetaString.h"

#include <boost/algorithm/string.hpp>
#include <boost/core/demangle.hpp>
#include <boost/filesystem.hpp>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>

namespace
{
std::string yamlString(const std::string & value)
{
	std::string result;
	result.reserve(value.size() + 2);
	result.push_back('"');
	for(const char ch : value)
	{
		switch(ch)
		{
			case '\\': result += "\\\\"; break;
			case '"': result += "\\\""; break;
			case '\n': result += "\\n"; break;
			case '\r': result += "\\r"; break;
			case '\t': result += "\\t"; break;
			default: result.push_back(ch); break;
		}
	}
	result.push_back('"');
	return result;
}

bool isPlainYamlKey(const std::string & value)
{
	if(value.empty())
		return false;
	if(!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_')
		return false;

	for(const char ch : value)
	{
		const auto byte = static_cast<unsigned char>(ch);
		if(!std::isalnum(byte) && ch != '_' && ch != '-')
			return false;
	}
	return true;
}

std::string yamlKey(const std::string & value)
{
	return isPlainYamlKey(value) ? value : yamlString(value);
}

bool isPlainYamlIdentifier(const std::string & value)
{
	if(value.empty())
		return false;
	if(!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_')
		return false;

	for(const char ch : value)
	{
		const auto byte = static_cast<unsigned char>(ch);
		if(!std::isalnum(byte) && ch != '_' && ch != '-' && ch != '/')
			return false;
	}
	return true;
}

std::string yamlIdentifier(const std::string & value)
{
	return isPlainYamlIdentifier(value) ? value : yamlString(value);
}

std::string transcriptIdentifier(std::string identifier)
{
	boost::algorithm::replace_all(identifier, ":", "/");
	const std::string corePrefix = "core/";
	if(identifier.starts_with(corePrefix))
		identifier.erase(0, corePrefix.size());
	return yamlIdentifier(identifier);
}

struct BattleBlockRecord
{
	std::string battleID;
	std::string record;
};

std::string readableDecisionRecord(const std::string & line, const std::optional<PlayerColor> & defaultActor)
{
	const std::string prefix = "decision: { actor: ";
	const std::string kindMarker = ", kind: ";
	if(!line.starts_with(prefix))
		return line;

	const size_t actorEnd = line.find(kindMarker, prefix.size());
	if(actorEnd == std::string::npos || line.size() < 2 || !line.ends_with(" }"))
		return line;

	const size_t kindStart = actorEnd + kindMarker.size();
	const size_t fieldsStart = line.find(", ", kindStart);
	const size_t kindEnd = fieldsStart == std::string::npos ? line.size() - 2 : fieldsStart;
	const std::string actor = line.substr(prefix.size(), actorEnd - prefix.size());
	const std::string kind = line.substr(kindStart, kindEnd - kindStart);
	const std::string fields = fieldsStart == std::string::npos ? "" : line.substr(fieldsStart, line.size() - fieldsStart - 2);
	if(defaultActor && actor == defaultActor->toString() && kind != "battleAction")
	{
		if(fields.empty())
			return kind;
		return kind + ": { " + fields.substr(2) + " }";
	}
	return kind + ": { actor: " + actor + fields + " }";
}

std::string flattenBattleActionRecord(const std::string & line)
{
	const std::string prefix = "battleAction: { actor: ";
	const std::string outerActionMarker = ", action: { ";
	if(!line.starts_with(prefix) || !line.ends_with(" } }"))
		return line;

	const size_t actorEnd = line.find(outerActionMarker, prefix.size());
	if(actorEnd == std::string::npos)
		return line;
	const std::string actor = line.substr(prefix.size(), actorEnd - prefix.size());
	const size_t innerStart = actorEnd + outerActionMarker.size();
	const std::string inner = line.substr(innerStart, line.size() - innerStart - 4);
	const std::string actionMarker = ", action: ";
	const size_t actionStart = inner.find(actionMarker);
	if(actionStart == std::string::npos)
		return line;
	const size_t kindStart = actionStart + actionMarker.size();
	const size_t fieldsAfterKind = inner.find(", ", kindStart);
	const size_t kindEnd = fieldsAfterKind == std::string::npos ? inner.size() : fieldsAfterKind;
	const std::string kind = inner.substr(kindStart, kindEnd - kindStart);
	std::string fields = inner.substr(0, actionStart);
	if(fieldsAfterKind != std::string::npos)
		fields += inner.substr(fieldsAfterKind);
	return kind + ": { actor: " + actor + ", " + fields + " }";
}

std::optional<BattleBlockRecord> battleBlockRecord(const std::string & line)
{
	const std::string battlePrefix = "battle: { id: ";
	if(line.rfind(battlePrefix, 0) == 0)
	{
		const size_t battleIDStart = battlePrefix.size();
		const size_t battleIDEnd = line.find(", ", battleIDStart);
		if(battleIDEnd == std::string::npos || line.size() < 2 || line.substr(line.size() - 2) != " }")
			return std::nullopt;

		BattleBlockRecord result;
		result.battleID = line.substr(battleIDStart, battleIDEnd - battleIDStart);
		result.record = "{ " + line.substr(battleIDEnd + 2, line.size() - battleIDEnd - 4) + " }";
		return result;
	}

	const std::string battleDecisionMarker = ", battle: ";
	const size_t battleFieldStart = line.find(battleDecisionMarker);
	const bool readableDecision = line.starts_with("battleAction: { ");
	if(!readableDecision || battleFieldStart == std::string::npos)
		return std::nullopt;

	const size_t battleIDStart = battleFieldStart + battleDecisionMarker.size();
	const size_t battleIDEnd = line.find(", ", battleIDStart);
	if(battleIDEnd == std::string::npos)
		return std::nullopt;

	BattleBlockRecord result;
	result.battleID = line.substr(battleIDStart, battleIDEnd - battleIDStart);
	result.record = line;
	result.record.erase(battleFieldStart, battleIDEnd - battleFieldStart);
	if(readableDecision)
		result.record = flattenBattleActionRecord(result.record);
	return result;
}

std::string flowList(const std::vector<std::string> & values)
{
	std::ostringstream out;
	out << "[";
	for(size_t i = 0; i < values.size(); ++i)
	{
		if(i)
			out << ", ";
		out << values[i];
	}
	out << "]";
	return out.str();
}

std::string pos(const int3 & value)
{
	std::string result = "[" + std::to_string(value.x) + ", " + std::to_string(value.y);
	if(value.z != 0)
		result += ", " + std::to_string(value.z);
	return result + "]";
}

std::string pos(const std::array<int, 3> & value)
{
	return pos(int3(value[0], value[1], value[2]));
}

std::string directionName(int dx, int dy)
{
	if(dx == 0 && dy == -1) return "N";
	if(dx == 1 && dy == -1) return "NE";
	if(dx == 1 && dy == 0) return "E";
	if(dx == 1 && dy == 1) return "SE";
	if(dx == 0 && dy == 1) return "S";
	if(dx == -1 && dy == 1) return "SW";
	if(dx == -1 && dy == 0) return "W";
	if(dx == -1 && dy == -1) return "NW";
	return {};
}

std::optional<std::string> encodedDirections(
	const std::array<int, 3> & start,
	const std::vector<std::array<int, 3>> & route)
{
	std::vector<std::string> directions;
	std::array<int, 3> previous = start;
	for(const auto & destination : route)
	{
		if(destination[2] != previous[2])
			return std::nullopt;
		const auto direction = directionName(destination[0] - previous[0], destination[1] - previous[1]);
		if(direction.empty())
			return std::nullopt;
		directions.push_back(direction);
		previous = destination;
	}

	std::vector<std::string> runs;
	for(size_t index = 0; index < directions.size();)
	{
		size_t end = index + 1;
		while(end < directions.size() && directions[end] == directions[index])
			++end;
		const size_t count = end - index;
		runs.push_back(directions[index] + (count > 1 ? "*" + std::to_string(count) : ""));
		index = end;
	}
	return boost::algorithm::join(runs, " ");
}

std::string color(PlayerColor value)
{
	if(value == PlayerColor::CANNOT_DETERMINE)
		return "cannotDetermine";
	if(value == PlayerColor::UNFLAGGABLE)
		return "unflaggable";
	return value.toString();
}

std::string boolValue(bool value)
{
	return value ? "true" : "false";
}

std::string battleAlias(BattleID battleID)
{
	return std::to_string(battleID.getNum());
}

std::string heroAlias(const CGameState & gameState, ObjectInstanceID id);

std::string sanitizedAliasName(std::string name)
{
	boost::algorithm::to_lower(name);
	for(char & ch : name)
	{
		if(!std::isalnum(static_cast<unsigned char>(ch)))
			ch = '-';
	}
	name.erase(std::unique(name.begin(), name.end(), [](char left, char right)
	{
		return left == '-' && right == '-';
	}), name.end());
	boost::algorithm::trim_if(name, boost::is_any_of("-"));
	return name;
}

std::string objectLocation(const int3 & position)
{
	std::string result = "@" + std::to_string(position.x) + "." + std::to_string(position.y);
	if(position.z != 0)
		result += "." + std::to_string(position.z);
	return result;
}

bool genericObjectAliasType(const std::string & type)
{
	return type == "creatureGeneratorCommon" || type.starts_with("shrineOfMagicLevel");
}

bool equivalentAliasWords(const std::string & left, const std::string & right)
{
	auto compact = [](std::string value)
	{
		value = sanitizedAliasName(std::move(value));
		value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
		return value;
	};
	return compact(left) == compact(right);
}

std::string objectAlias(const CGameState & gameState, ObjectInstanceID id)
{
	if(id == ObjectInstanceID::NONE)
		return "none";

	const auto * object = gameState.getMap().getObject(id);
	if(!object)
		return "id-" + std::to_string(id.getNum());
	if(dynamic_cast<const CGHeroInstance *>(object))
		return heroAlias(gameState, id);

	std::string type = transcriptIdentifier(MapObjectID::encode(object->ID.getNum()));
	if(type.empty())
		type = "unknown";

	std::string owner;
	if(object->tempOwner.isValidPlayer())
		owner = "/" + object->tempOwner.toString();

	std::string name = sanitizedAliasName(object->getObjectName());
	if(name.empty())
		name = sanitizedAliasName(object->instanceName);
	if(name.empty())
		name = "id-" + std::to_string(id.getNum());
	if(dynamic_cast<const CGTownInstance *>(object))
		type = "town";

	const auto typeName = type.substr(type.rfind('/') == std::string::npos ? 0 : type.rfind('/') + 1);
	std::string result;
	if(equivalentAliasWords(name, typeName))
		result = owner.empty() ? name : owner.substr(1) + "/" + name;
	else if(genericObjectAliasType(type))
		result = owner.empty() ? name : owner.substr(1) + "/" + name;
	else
	{
		result = type + owner;
		result += "/" + name;
	}
	return result + objectLocation(object->visitablePos());
}

std::string heroAlias(const CGameState & gameState, ObjectInstanceID id)
{
	const auto * object = gameState.getMap().getObject(id);
	if(!object)
		return objectAlias(gameState, id);

	std::string owner = object->tempOwner.isValidPlayer() ? object->tempOwner.toString() : "neutral";
	if(const auto * hero = dynamic_cast<const CGHeroInstance *>(object); hero && hero->getHeroTypeID().hasValue())
		return owner + "/" + transcriptIdentifier(HeroTypeID::encode(hero->getHeroTypeID().getNum()));

	std::string name = sanitizedAliasName(object->getObjectName());
	if(name.empty())
		name = sanitizedAliasName(object->instanceName);
	if(name.empty())
		name = "id-" + std::to_string(id.getNum());

	return owner + "/" + name;
}

std::string actorForPlayer(PlayerColor player)
{
	return player.isValidPlayer() ? player.toString() : "world";
}

void unwrapPlayerMapping(std::string & line, const std::string & field, const std::string & player)
{
	const std::string marker = field + ": { " + player + ": ";
	bool quoted = false;
	bool escaped = false;
	for(size_t search = 0; search < line.size();)
	{
		if(quoted)
		{
			if(escaped)
				escaped = false;
			else if(line[search] == '\\')
				escaped = true;
			else if(line[search] == '"')
				quoted = false;
			++search;
			continue;
		}
		if(line[search] == '"')
		{
			quoted = true;
			++search;
			continue;
		}
		if(line.compare(search, marker.size(), marker) != 0)
		{
			++search;
			continue;
		}

		const size_t valueStart = search + marker.size();
		if(valueStart >= line.size() || line[valueStart] != '{')
		{
			search = valueStart;
			continue;
		}
		int depth = 0;
		bool valueQuoted = false;
		bool valueEscaped = false;
		size_t valueEnd = valueStart;
		for(; valueEnd < line.size(); ++valueEnd)
		{
			const char character = line[valueEnd];
			if(valueQuoted)
			{
				if(valueEscaped)
					valueEscaped = false;
				else if(character == '\\')
					valueEscaped = true;
				else if(character == '"')
					valueQuoted = false;
				continue;
			}
			if(character == '"')
				valueQuoted = true;
			else if(character == '{')
				++depth;
			else if(character == '}' && --depth == 0)
				break;
		}
		if(valueEnd >= line.size() || line.substr(valueEnd + 1, 2) != " }")
		{
			search = valueStart + 1;
			continue;
		}

		const std::string replacement = field + ": " + line.substr(valueStart, valueEnd - valueStart + 1);
		line.replace(search, valueEnd + 3 - search, replacement);
		search += replacement.size();
	}
}

void unwrapEntityValue(std::string & line, const std::string & field, const std::string & entity)
{
	const std::string marker = field + ": { " + entity + ": ";
	for(size_t search = line.find(marker); search != std::string::npos; search = line.find(marker, search))
	{
		const size_t valueStart = search + marker.size();
		int braces = 0;
		int brackets = 0;
		bool quoted = false;
		bool escaped = false;
		size_t valueEnd = valueStart;
		for(; valueEnd + 1 < line.size(); ++valueEnd)
		{
			const char character = line[valueEnd];
			if(quoted)
			{
				if(escaped)
					escaped = false;
				else if(character == '\\')
					escaped = true;
				else if(character == '"')
					quoted = false;
				continue;
			}
			if(character == '"')
				quoted = true;
			else if(character == '{')
				++braces;
			else if(character == '}')
				--braces;
			else if(character == '[')
				++brackets;
			else if(character == ']')
				--brackets;
			if(braces == 0 && brackets == 0 && line.substr(valueEnd, 2) == " }")
				break;
		}
		if(valueEnd + 1 >= line.size())
			return;
		const std::string replacement = field + ": " + line.substr(valueStart, valueEnd - valueStart);
		line.replace(search, valueEnd + 2 - search, replacement);
		search += replacement.size();
	}
}

std::string turnRelativeReferences(const std::string & line, const std::optional<PlayerColor> & player)
{
	if(!player)
		return line;

	const std::string name = player->toString();
	std::string result;
	result.reserve(line.size());
	bool quoted = false;
	bool escaped = false;
	for(size_t index = 0; index < line.size();)
	{
		const char character = line[index];
		if(quoted)
		{
			result.push_back(character);
			++index;
			if(escaped)
				escaped = false;
			else if(character == '\\')
				escaped = true;
			else if(character == '"')
				quoted = false;
			continue;
		}
		if(character == '"')
		{
			quoted = true;
			result.push_back(character);
			++index;
			continue;
		}

		const std::string ownedSegment = "/" + name + "/";
		if(line.compare(index, ownedSegment.size(), ownedSegment) == 0)
		{
			result.push_back('/');
			index += ownedSegment.size();
			continue;
		}
		const std::string heroPrefix = name + "/";
		const bool tokenStart = index == 0 || line[index - 1] == ' ' || line[index - 1] == '{' ||
			line[index - 1] == '[' || line[index - 1] == ',' || line[index - 1] == ':';
		if(tokenStart && line.compare(index, heroPrefix.size(), heroPrefix) == 0)
		{
			index += heroPrefix.size();
			continue;
		}

		result.push_back(character);
		++index;
	}

	for(const std::string field : {"actor", "owner", "player", "initiator"})
	{
		boost::algorithm::replace_all(result, ", " + field + ": " + name, "");
		boost::algorithm::replace_all(result, field + ": " + name + ", ", "");
	}

	for(const std::string field : {"resources", "setResources"})
		unwrapPlayerMapping(result, field, name);
	return result;
}

struct HeroActionLine
{
	std::string hero;
	std::string withoutHero;
};

std::optional<HeroActionLine> heroActionLine(const std::string & line)
{
	if(line.find('\n') != std::string::npos || !line.ends_with(" }"))
		return std::nullopt;
	const auto mapping = line.find(": { ");
	if(mapping == std::string::npos)
		return std::nullopt;
	const std::string kind = line.substr(0, mapping);
	static const std::set<std::string> heroKinds = {
		"assembleArtifacts", "buyArtifact", "capture", "castAdventureSpell", "chooseSkill", "dig",
		"dismissHero", "discovers", "encounter", "levelUp", "move", "setFormation", "setTactics", "teleport", "visit"
	};
	if(!heroKinds.contains(kind))
		return std::nullopt;

	const std::string marker = "hero: ";
	const auto field = line.find(marker, mapping + 4);
	if(field == std::string::npos)
		return std::nullopt;
	const auto valueStart = field + marker.size();
	const auto valueEnd = line.find_first_of(", }", valueStart);
	if(valueEnd == std::string::npos || valueEnd == valueStart)
		return std::nullopt;

	HeroActionLine result{line.substr(valueStart, valueEnd - valueStart), line};
	if(field >= 2 && line.substr(field - 2, 2) == ", ")
		result.withoutHero.erase(field - 2, valueEnd - field + 2);
	else if(line[valueEnd] == ',')
	{
		size_t eraseEnd = valueEnd + 1;
		if(eraseEnd < line.size() && line[eraseEnd] == ' ')
			++eraseEnd;
		result.withoutHero.erase(field, eraseEnd - field);
	}
	else
		result.withoutHero.erase(field, valueEnd - field);
	boost::algorithm::replace_all(result.withoutHero, "{  }", "{}");
	for(const std::string effect : {"experience", "setExperience", "mana", "setMana", "setMovement", "skills", "setSkills"})
		unwrapEntityValue(result.withoutHero, effect, result.hero);
	return result;
}

std::string resourceKey(GameResID id)
{
	if(id.getNum() < 0)
		return yamlKey("none");

	return yamlKey(GameResID::encode(id.getNum()));
}

std::string creature(CreatureID id)
{
	if(id.getNum() < 0)
		return "none";
	return transcriptIdentifier(CreatureID::encode(id.getNum()));
}

std::string spell(SpellID id)
{
	if(id.getNum() < 0)
		return "none";
	return transcriptIdentifier(SpellID::encode(id.getNum()));
}

std::string heroType(HeroTypeID id)
{
	if(id == HeroTypeID::NONE)
		return "none";
	if(id == HeroTypeID::RANDOM)
		return yamlString("random");
	if(id == HeroTypeID::CAMP_STRONGEST)
		return yamlString("campaignStrongest");
	if(id == HeroTypeID::CAMP_GENERATED)
		return yamlString("campaignGenerated");
	if(id == HeroTypeID::CAMP_RANDOM)
		return yamlString("campaignRandom");
	if(id.getNum() < 0)
		return transcriptIdentifier("hero:" + std::to_string(id.getNum()));
	return transcriptIdentifier(HeroTypeID::encode(id.getNum()));
}

std::string faction(FactionID id)
{
	if(id == FactionID::NONE)
		return "none";
	if(id == FactionID::RANDOM)
		return yamlString("random");
	if(id.getNum() < 0)
		return transcriptIdentifier("faction:" + std::to_string(id.getNum()));
	return transcriptIdentifier(FactionID::encode(id.getNum()));
}

std::string primarySkill(PrimarySkill id)
{
	if(id.getNum() < 0)
		return yamlString("none");
	return transcriptIdentifier(PrimarySkill::encode(id.getNum()));
}

std::string secondarySkill(SecondarySkill id)
{
	if(id.getNum() < 0)
		return yamlString("none");
	return transcriptIdentifier(SecondarySkill::encode(id.getNum()));
}

std::string building(BuildingID id)
{
	if(id.getNum() < 0)
		return "none";
	if(id.getNum() >= 0 && id.getNum() < std::size(EBuildingType::names))
		return transcriptIdentifier("core:" + EBuildingType::names[id.getNum()]);
	return transcriptIdentifier("building:" + std::to_string(id.getNum()));
}

std::string artifact(ArtifactID id)
{
	if(id.getNum() < 0)
		return "none";
	return transcriptIdentifier(ArtifactID::encode(id.getNum()));
}

std::string slot(SlotID id)
{
	return std::to_string(id.getNum());
}

std::string artifactPosition(ArtifactPosition id)
{
	return std::to_string(id.getNum());
}

std::string artifactPositions(const std::vector<ArtifactPosition> & positions)
{
	std::vector<std::string> entries;
	for(const auto & entry : positions)
		entries.push_back(artifactPosition(entry));
	return flowList(entries);
}

std::string team(TeamID id)
{
	if(id == TeamID::NO_TEAM)
		return "none";
	return std::to_string(id.getNum());
}

std::string randomMapPlayerType(EPlayerType value)
{
	switch(value)
	{
		case EPlayerType::HUMAN: return "human";
		case EPlayerType::AI: return "ai";
		case EPlayerType::COMP_ONLY: return "computerOnly";
	}
	return "unknown";
}

std::string randomMapWater(EWaterContent::EWaterContent value)
{
	switch(value)
	{
		case EWaterContent::RANDOM: return "random";
		case EWaterContent::NONE: return "none";
		case EWaterContent::NORMAL: return "normal";
		case EWaterContent::ISLANDS: return "islands";
	}
	return std::to_string(static_cast<int>(value));
}

std::string randomMapMonsterStrength(EMonsterStrength::EMonsterStrength value)
{
	switch(value)
	{
		case EMonsterStrength::RANDOM: return "random";
		case EMonsterStrength::GLOBAL_WEAK: return "weak";
		case EMonsterStrength::GLOBAL_NORMAL: return "normal";
		case EMonsterStrength::GLOBAL_STRONG: return "strong";
		default: break;
	}
	return std::to_string(static_cast<int>(value));
}

std::string road(RoadId id)
{
	if(id == RoadId::NO_ROAD)
		return "none";

	std::string identifier = RoadId::encode(id.getNum());
	if(identifier.empty())
		identifier = "road:" + std::to_string(id.getNum());
	return transcriptIdentifier(identifier);
}

std::string randomMapRoads(const CMapGenOptions & options)
{
	std::vector<std::string> entries;
	for(const auto roadId : { RoadId::DIRT_ROAD, RoadId::GRAVEL_ROAD, RoadId::COBBLESTONE_ROAD })
	{
		if(options.isRoadEnabled(roadId))
			entries.push_back(road(roadId));
	}
	return flowList(entries);
}

std::string randomMapPlayer(const CMapGenOptions::CPlayerSettings & player)
{
	return "{ type: " + randomMapPlayerType(player.getPlayerType()) +
		", faction: " + faction(player.getStartingTown()) +
		", hero: " + heroType(player.getStartingHero()) +
		", team: " + team(player.getTeam()) + " }";
}

std::string randomMapPlayers(const CMapGenOptions & options)
{
	std::vector<std::string> entries;
	for(const auto & player : options.getPlayersSettings())
	{
		const bool resolvedDefault = player.second.getPlayerType() == EPlayerType::AI &&
			player.second.getStartingTown() == FactionID::RANDOM &&
			player.second.getStartingHero() == HeroTypeID::RANDOM &&
			player.second.getTeam() == TeamID(player.first.getNum());
		if(!resolvedDefault)
			entries.push_back(color(player.first) + ": " + randomMapPlayer(player.second));
	}
	return "{ " + boost::algorithm::join(entries, ", ") + " }";
}

std::string randomMapSlots(const CMapGenOptions & options)
{
	const auto & settings = options.getPlayersSettings();
	const int inferredSlots = std::max<int>(0, options.getHumanOrCpuPlayerCount()) +
		std::max<int>(0, options.getCompOnlyPlayerCount());
	bool inferred = settings.size() == static_cast<size_t>(inferredSlots);
	for(int index = 0; inferred && index < inferredSlots; ++index)
		inferred = settings.contains(PlayerColor(index));
	if(inferred)
		return {};

	std::vector<std::string> entries;
	for(const auto & player : settings)
		entries.push_back(color(player.first));
	return flowList(entries);
}

std::string randomMapGenerator(const CMapGenOptions & options)
{
	std::vector<std::string> fields;
	std::string templateName;
	if(options.getMapTemplate())
		templateName = options.getMapTemplate()->getId();
	fields.push_back("width: " + std::to_string(options.getWidth()));
	fields.push_back("height: " + std::to_string(options.getHeight()));
	fields.push_back("humanOrComputerPlayers: " + std::to_string(options.getHumanOrCpuPlayerCount()));
	if(options.getLevels() != 1)
		fields.push_back("levels: " + std::to_string(options.getLevels()));
	if(options.getTeamCount() != 0)
		fields.push_back("teams: " + std::to_string(options.getTeamCount()));
	if(options.getCompOnlyPlayerCount() != 0)
		fields.push_back("computerOnlyPlayers: " + std::to_string(options.getCompOnlyPlayerCount()));
	if(options.getCompOnlyTeamCount() != 0)
		fields.push_back("computerOnlyTeams: " + std::to_string(options.getCompOnlyTeamCount()));
	if(options.getWaterContent() != EWaterContent::NONE)
		fields.push_back("water: " + randomMapWater(options.getWaterContent()));
	if(options.getMonsterStrength() != EMonsterStrength::GLOBAL_NORMAL)
		fields.push_back("monsters: " + randomMapMonsterStrength(options.getMonsterStrength()));
	if(!templateName.empty())
		fields.push_back("template: " + yamlString(templateName));
	if(options.isRoadEnabled())
		fields.push_back("roads: " + randomMapRoads(options));
	const auto slots = randomMapSlots(options);
	if(!slots.empty())
		fields.push_back("slots: " + slots);
	const auto players = randomMapPlayers(options);
	if(players != "{  }")
		fields.push_back("players: " + players);
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string mode(ChangeValueMode mode)
{
	switch(mode)
	{
		case ChangeValueMode::ABSOLUTE: return "absolute";
		case ChangeValueMode::RELATIVE: return "relative";
	}
	return "unknown";
}

std::string tavernSlot(TavernHeroSlot value)
{
	switch(value)
	{
		case TavernHeroSlot::NONE: return "none";
		case TavernHeroSlot::NATIVE: return "native";
		case TavernHeroSlot::RANDOM: return "random";
	}
	return "unknown";
}

std::string tavernRole(TavernSlotRole value)
{
	switch(value)
	{
		case TavernSlotRole::NONE: return "none";
		case TavernSlotRole::SINGLE_UNIT: return "singleUnit";
		case TavernSlotRole::FULL_ARMY: return "fullArmy";
		case TavernSlotRole::RETREATED: return "retreated";
		case TavernSlotRole::SURRENDERED: return "surrendered";
	}
	return "unknown";
}

std::string armyFormation(EArmyFormation value)
{
	switch(value)
	{
		case EArmyFormation::LOOSE: return "loose";
		case EArmyFormation::TIGHT: return "tight";
	}
	return "unknown";
}

std::string backpackManageCommand(ManageBackpackArtifacts::ManageCmd value)
{
	switch(value)
	{
		case ManageBackpackArtifacts::ManageCmd::SCROLL_LEFT: return "scrollLeft";
		case ManageBackpackArtifacts::ManageCmd::SCROLL_RIGHT: return "scrollRight";
		case ManageBackpackArtifacts::ManageCmd::SORT_BY_SLOT: return "sortBySlot";
		case ManageBackpackArtifacts::ManageCmd::SORT_BY_CLASS: return "sortByClass";
		case ManageBackpackArtifacts::ManageCmd::SORT_BY_COST: return "sortByCost";
	}
	return "unknown";
}

std::string openWindowMode(EOpenWindowMode value)
{
	switch(value)
	{
		case EOpenWindowMode::EXCHANGE_WINDOW: return "exchange";
		case EOpenWindowMode::RECRUITMENT_FIRST: return "recruitmentFirst";
		case EOpenWindowMode::RECRUITMENT_ALL: return "recruitmentAll";
		case EOpenWindowMode::SHIPYARD_WINDOW: return "shipyard";
		case EOpenWindowMode::THIEVES_GUILD: return "thievesGuild";
		case EOpenWindowMode::UNIVERSITY_WINDOW: return "university";
		case EOpenWindowMode::HILL_FORT_WINDOW: return "hillFort";
		case EOpenWindowMode::MARKET_WINDOW: return "market";
		case EOpenWindowMode::PUZZLE_MAP: return "puzzleMap";
		case EOpenWindowMode::TAVERN_WINDOW: return "tavern";
	}
	return "unknown";
}

std::string gateState(EGateState value)
{
	switch(value)
	{
		case EGateState::NONE: return "none";
		case EGateState::CLOSED: return "closed";
		case EGateState::BLOCKED: return "blocked";
		case EGateState::OPENED: return "opened";
		case EGateState::DESTROYED: return "destroyed";
	}
	return "unknown";
}

std::string battleStackProperty(BattleSetStackProperty::BattleStackProperty value)
{
	switch(value)
	{
		case BattleSetStackProperty::CASTS: return "casts";
		case BattleSetStackProperty::ENCHANTER_COUNTER: return "enchanterCounter";
		case BattleSetStackProperty::UNBIND: return "unbind";
		case BattleSetStackProperty::CLONED: return "cloned";
		case BattleSetStackProperty::HAS_CLONE: return "hasClone";
	}
	return "unknown";
}

std::string visibility(ETileVisibility mode)
{
	switch(mode)
	{
		case ETileVisibility::HIDDEN: return "hidden";
		case ETileVisibility::REVEALED: return "revealed";
	}
	return "unknown";
}

std::string weekType(EWeekType type)
{
	switch(type)
	{
		case EWeekType::FIRST_WEEK: return "firstWeek";
		case EWeekType::NORMAL: return "normal";
		case EWeekType::DOUBLE_GROWTH: return "doubleGrowth";
		case EWeekType::BONUS_GROWTH: return "bonusGrowth";
		case EWeekType::DEITYOFFIRE: return "deityOfFire";
		case EWeekType::PLAGUE: return "plague";
	}
	return "unknown";
}

std::string battleSide(BattleSide side)
{
	switch(side)
	{
		case BattleSide::ATTACKER: return "attacker";
		case BattleSide::DEFENDER: return "defender";
		case BattleSide::NONE: return "none";
		case BattleSide::INVALID: return "invalid";
		case BattleSide::ALL_KNOWING: return "allKnowing";
	}
	return "unknown";
}

std::string battleResult(EBattleResult result)
{
	switch(result)
	{
		case EBattleResult::NORMAL: return "normal";
		case EBattleResult::ESCAPE: return "escape";
		case EBattleResult::SURRENDER: return "surrender";
	}
	return "unknown";
}

std::string battleHeroResult(const CGameState & gameState, const BattleResultAccepted::HeroBattleResults & result)
{
	return "{ hero: " + objectAlias(gameState, result.heroID) +
		", army: " + objectAlias(gameState, result.armyID) +
		", experience: " + std::to_string(result.exp) + " }";
}

std::string actionType(EActionType action)
{
	switch(action)
	{
		case EActionType::NO_ACTION: return "none";
		case EActionType::END_TACTIC_PHASE: return "endTactics";
		case EActionType::RETREAT: return "retreat";
		case EActionType::SURRENDER: return "surrender";
		case EActionType::HERO_SPELL: return "heroSpell";
		case EActionType::WALK: return "walk";
		case EActionType::WAIT: return "wait";
		case EActionType::DEFEND: return "defend";
		case EActionType::WALK_AND_ATTACK: return "walkAndAttack";
		case EActionType::SHOOT: return "shoot";
		case EActionType::CATAPULT: return "catapult";
		case EActionType::MONSTER_SPELL: return "monsterSpell";
		case EActionType::BAD_MORALE: return "badMorale";
		case EActionType::STACK_HEAL: return "stackHeal";
		case EActionType::WALK_AND_CAST: return "walkAndCast";
	}
	return "unknown";
}

std::string movementResult(TryMoveHero::EResult result)
{
	switch(result)
	{
		case TryMoveHero::FAILED: return "failed";
		case TryMoveHero::SUCCESS: return "success";
		case TryMoveHero::TELEPORTATION: return "teleportation";
		case TryMoveHero::BLOCKING_VISIT: return "blockingVisit";
		case TryMoveHero::EMBARK: return "embark";
		case TryMoveHero::DISEMBARK: return "disembark";
	}
	return "unknown";
}

std::string packetTypeName(CPack & pack)
{
	std::string name = boost::core::demangle(typeid(pack).name());
	boost::algorithm::erase_first(name, "struct ");
	boost::algorithm::erase_first(name, "class ");
	return name;
}

std::string signedInteger(int64_t value)
{
	return value > 0 ? "+" + std::to_string(value) : std::to_string(value);
}

std::string resourceMap(const ResourceSet & values, bool showPositiveSign = false)
{
	std::vector<std::string> entries;
	for(size_t index = 0; index < values.size(); ++index)
	{
		if(values[index] != 0)
			entries.push_back(resourceKey(GameResID(static_cast<int>(index))) + ": " +
				(showPositiveSign ? signedInteger(values[index]) : std::to_string(values[index])));
	}
	return "{ " + boost::algorithm::join(entries, ", ") + " }";
}

std::string namedIntegerMap(const std::map<std::string, int64_t> & values)
{
	std::vector<std::string> entries;
	for(const auto & [name, value] : values)
		entries.push_back(yamlKey(name) + ": " + std::to_string(value));
	return "{ " + boost::algorithm::join(entries, ", ") + " }";
}

std::string heroRefreshMap(const std::map<std::string, int64_t> & values)
{
	std::map<std::string, std::vector<std::string>> players;
	for(const auto & [alias, value] : values)
	{
		const size_t separator = alias.find('/');
		if(separator == std::string::npos)
			throw std::runtime_error("VGT new-day hero alias has no player: " + alias);
		players[alias.substr(0, separator)].push_back(
			yamlIdentifier(alias.substr(separator + 1)) + ": " + std::to_string(value));
	}

	std::vector<std::string> entries;
	for(const auto & [player, heroes] : players)
		entries.push_back(yamlIdentifier(player) + ": { " + boost::algorithm::join(heroes, ", ") + " }");
	return "{ " + boost::algorithm::join(entries, ", ") + " }";
}

std::string startingBonus(PlayerStartingBonus value)
{
	switch(value)
	{
		case PlayerStartingBonus::RANDOM: return "random";
		case PlayerStartingBonus::ARTIFACT: return "artifact";
		case PlayerStartingBonus::GOLD: return "gold";
		case PlayerStartingBonus::RESOURCE: return "resource";
	}
	return "unknown";
}

std::string connectionList(const std::set<PlayerConnectionID> & values)
{
	std::vector<std::string> entries;
	for(const auto & entry : values)
		entries.push_back(std::to_string(static_cast<int>(entry)));
	return flowList(entries);
}

std::string handicap(const Handicap & value)
{
	std::vector<std::string> fields;
	if(value.startBonus.nonZero())
		fields.push_back("resources: " + resourceMap(value.startBonus));
	if(value.percentIncome != 100)
		fields.push_back("incomePercent: " + std::to_string(value.percentIncome));
	if(value.percentGrowth != 100)
		fields.push_back("growthPercent: " + std::to_string(value.percentGrowth));
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string simturnsInfo(const SimturnsInfo & value)
{
	return "{ requiredTurns: " + std::to_string(value.requiredTurns) +
		", optionalTurns: " + std::to_string(value.optionalTurns) +
		", allowHumanWithAI: " + boolValue(value.allowHumanWithAI) +
		", ignoreAlliedContacts: " + boolValue(value.ignoreAlliedContacts) + " }";
}

std::string timerInfo(const TurnTimerInfo & value)
{
	if(!value.isEnabled())
		return "none";

	return "{ enabled: " + boolValue(value.isEnabled()) +
		", turn: " + std::to_string(value.turnTimer) +
		", base: " + std::to_string(value.baseTimer) +
		", battle: " + std::to_string(value.battleTimer) +
		", unit: " + std::to_string(value.unitTimer) +
		", accumulatingTurn: " + boolValue(value.accumulatingTurnTimer) +
		", accumulatingUnit: " + boolValue(value.accumulatingUnitTimer) + " }";
}

std::string timerState(const TurnTimerInfo & value)
{
	std::vector<std::string> fields;
	if(value.turnTimer != 0)
		fields.push_back("turn: " + std::to_string(value.turnTimer));
	if(value.baseTimer != 0)
		fields.push_back("base: " + std::to_string(value.baseTimer));
	if(value.battleTimer != 0)
		fields.push_back("battle: " + std::to_string(value.battleTimer));
	if(value.unitTimer != 0)
		fields.push_back("unit: " + std::to_string(value.unitTimer));
	if(!value.isActive)
		fields.push_back("active: false");
	if(value.isBattle)
		fields.push_back("battleMode: true");
	if(value.remainingMovementPointsPercent != 0)
		fields.push_back("movement: " + std::to_string(value.remainingMovementPointsPercent));
	if(value.isTurnStart)
		fields.push_back("turnStart: true");
	if(value.isTurnEnded)
		fields.push_back("ended: true");
	if(fields.empty())
		return "none";
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string calendarDate(const Calendar & calendar)
{
	if(calendar.getCurrentDay() <= 0)
		return "0";

	return std::to_string(calendar.getMonth()) + "/" +
		std::to_string(calendar.getWeek()) + "/" +
		std::to_string(calendar.getDayOfWeek());
}

std::string victoryLossResult(const EVictoryLossCheckResult & result)
{
	if(result.victory())
		return "victory";
	if(result.loss())
		return "loss";
	return "ingame";
}

std::string extraOptions(const ExtraOptionsInfo & value)
{
	return "{ cheatsAllowed: " + boolValue(value.cheatsAllowed) +
		", unlimitedReplay: " + boolValue(value.unlimitedReplay) + " }";
}

std::string playerSettings(const PlayerSettings & value, const PlayerSettings * base = nullptr)
{
	std::vector<std::string> fields;
	auto changed = [base](const auto & valueField, const auto & baseField)
	{
		return !base || valueField != baseField;
	};
	const bool human = value.isControlledByHuman();
	if(!base || human != base->isControlledByHuman())
		fields.push_back("controller: " + std::string(human ? "human" : "ai"));
	if(!base || value.castle != base->castle)
		fields.push_back("faction: " + faction(value.castle));
	if(changed(value.hero, base ? base->hero : HeroTypeID::NONE) && (base || value.hero != HeroTypeID::NONE))
		fields.push_back("hero: " + heroType(value.hero));
	if(changed(value.heroPortrait, base ? base->heroPortrait : HeroTypeID::NONE) && (base || value.heroPortrait != HeroTypeID::NONE))
		fields.push_back("heroPortrait: " + heroType(value.heroPortrait));
	if(changed(value.heroNameTextId, base ? base->heroNameTextId : std::string()) && (base || !value.heroNameTextId.empty()))
		fields.push_back("heroNameTextId: " + yamlString(value.heroNameTextId));
	if(changed(value.bonus, base ? base->bonus : PlayerStartingBonus::RANDOM) && (base || value.bonus != PlayerStartingBonus::RANDOM))
		fields.push_back("startingBonus: " + startingBonus(value.bonus));
	const bool handicapChanged = !base || value.handicap.startBonus != base->handicap.startBonus ||
		value.handicap.percentIncome != base->handicap.percentIncome || value.handicap.percentGrowth != base->handicap.percentGrowth;
	if(handicapChanged && (base || value.handicap.startBonus.nonZero() ||
		value.handicap.percentIncome != 100 || value.handicap.percentGrowth != 100))
		fields.push_back("handicap: " + handicap(value.handicap));
	const std::string defaultName = human ? "" : "Computer";
	if(changed(value.name, base ? base->name : defaultName) && (base || value.name != defaultName))
		fields.push_back("name: " + yamlString(value.name));
	if(changed(value.connectedPlayerIDs, base ? base->connectedPlayerIDs : std::set<PlayerConnectionID>()) &&
		(base || !value.connectedPlayerIDs.empty()))
		fields.push_back("connections: " + connectionList(value.connectedPlayerIDs));
	if(changed(value.compOnly, base ? base->compOnly : false) && (base || value.compOnly))
		fields.push_back("computerOnly: " + boolValue(value.compOnly));
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string simpleArmy(const CSimpleArmy & army)
{
	std::vector<std::string> entries;
	for(const auto & entry : army.army)
	{
		entries.push_back("{ slot: " + slot(entry.first) +
			", creature: " + creature(entry.second.first) +
			", count: " + std::to_string(entry.second.second) + " }");
	}
	return flowList(entries);
}

std::string armyState(const CCreatureSet & army)
{
	std::vector<std::string> entries;
	for(const auto & [slotID, stack] : army.Slots())
	{
		if(!stack)
			continue;
		entries.push_back("{ slot: " + slot(slotID) +
			", creature: " + creature(stack->getCreatureID()) +
			", count: " + std::to_string(stack->getCount()) + " }");
	}
	return flowList(entries);
}

std::string strategicArmyState(const CCreatureSet & army)
{
	std::vector<std::string> entries;
	for(const auto & [slotID, stack] : army.Slots())
	{
		if(!stack)
			continue;
		std::string entry = "{ slot: " + slot(slotID) +
			", creature: " + creature(stack->getCreatureID()) +
			", count: " + std::to_string(stack->getCount());
		if(stack->getTotalExperience() != 0)
			entry += ", experience: " + std::to_string(stack->getTotalExperience());
		entries.push_back(entry + " }");
	}
	return flowList(entries);
}

void writeInitialState(std::ostream & output, const CGameState & gameState)
{
	output << "initialState:\n";
	output << "  heroes:\n";
	for(const auto & heroID : gameState.getMap().getHeroesOnMap())
	{
		if(const auto * hero = gameState.getHero(heroID))
		{
			output << "    " << heroAlias(gameState, heroID) << ":\n";
			output << "      position: " << pos(hero->visitablePos()) << "\n";
			output << "      experience: " << hero->exp << "\n";
			output << "      mana: " << hero->mana << "\n";
			output << "      movement: " << hero->movementPointsRemaining() << "\n";

			if(hero->artifactsWorn.empty())
				output << "      artifacts: []\n";
			else
			{
				output << "      artifacts:\n";
				for(const auto & [position, slotInfo] : hero->artifactsWorn)
				{
					const auto * artifactInstance = slotInfo.getArt();
					if(!artifactInstance)
						continue;
					std::vector<std::string> fields;
					fields.push_back("position: " + artifactPosition(position));
					fields.push_back("artifact: " + artifact(artifactInstance->getTypeId()));
					if(artifactInstance->getScrollSpellID() != SpellID::NONE)
						fields.push_back("spell: " + spell(artifactInstance->getScrollSpellID()));
					fields.push_back("instance: " + std::to_string(artifactInstance->getId().getNum()));
					if(slotInfo.locked)
						fields.push_back("locked: true");
					output << "        - { " << boost::algorithm::join(fields, ", ") << " }\n";
				}
			}

			if(hero->Slots().empty())
				output << "      army: []\n";
			else
			{
				output << "      army:\n";
				for(const auto & [slotID, stack] : hero->Slots())
				{
					if(stack)
						output << "        - { slot: " << slot(slotID) << ", creature: "
							<< creature(stack->getCreatureID()) << ", count: " << stack->getCount() << " }\n";
				}
			}
		}
	}
}

std::string spellsList(const std::set<SpellID> & spells)
{
	std::vector<std::string> entries;
	for(const auto & entry : spells)
		entries.push_back(spell(entry));
	return flowList(entries);
}

std::string creaturesList(const std::vector<CreatureID> & creatures)
{
	std::vector<std::string> entries;
	for(const auto & entry : creatures)
		entries.push_back(creature(entry));
	return flowList(entries);
}

std::string artifactsList(const std::vector<ArtifactID> & artifacts)
{
	std::vector<std::string> entries;
	for(const auto & entry : artifacts)
		entries.push_back(artifact(entry));
	return flowList(entries);
}

std::string sparseCreaturePools(const std::vector<std::pair<ui32, std::vector<CreatureID>>> & pools)
{
	std::vector<std::string> creatures;
	std::vector<std::string> shared;
	for(const auto & pool : pools)
	{
		if(pool.second.empty())
			continue;
		if(pool.second.size() == 1)
			creatures.push_back(creature(pool.second.front()) + ": " + std::to_string(pool.first));
		else
			shared.push_back("{ creatures: " + creaturesList(pool.second) + ", count: " + std::to_string(pool.first) + " }");
	}
	std::vector<std::string> fields;
	if(!creatures.empty())
		fields.insert(fields.end(), creatures.begin(), creatures.end());
	if(!shared.empty())
		fields.push_back("shared: " + flowList(shared));
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

bool isStandardDwellingRefresh(const CGameState & gameState, const SetAvailableCreatures & value)
{
	const auto * dwelling = dynamic_cast<const CGDwelling *>(gameState.getMap().getObject(value.tid));
	if(!dwelling || dynamic_cast<const CGTownInstance *>(dwelling) || dwelling->ID == Obj::REFUGEE_CAMP)
		return false;
	if(value.creatures.size() != dwelling->creatures.size())
		return false;

	const bool accumulate = gameState.getSettings().getBoolean(
		dwelling->tempOwner.isValidPlayer()
			? EGameSettings::DWELLINGS_ACCUMULATE_WHEN_OWNED
			: EGameSettings::DWELLINGS_ACCUMULATE_WHEN_NEUTRAL);
	for(size_t index = 0; index < value.creatures.size(); ++index)
	{
		const auto & current = dwelling->creatures[index];
		const auto & refreshed = value.creatures[index];
		if(current.second != refreshed.second)
			return false;
		if(current.second.empty())
			continue;

		const auto * creature = current.second.front().toCreature();
		const TQuantity growth = creature->getGrowth() *
			(1 + creature->valOfBonuses(BonusType::CREATURE_GROWTH_PERCENT) / 100) +
			creature->valOfBonuses(BonusType::CREATURE_GROWTH, BonusCustomSubtype::creatureLevel(creature->getLevel()));
		const TQuantity expected = accumulate ? current.first + growth : growth;
		if(refreshed.first != expected)
			return false;
	}
	return true;
}

std::string singleLineJson(const JsonNode & value)
{
	std::string result = value.toCompactString();
	boost::algorithm::erase_all(result, "\n");
	boost::algorithm::erase_all(result, "\r");
	boost::algorithm::erase_all(result, "\t");
	return result;
}

std::string stackExperienceValues(const std::map<SlotID, si64> & values)
{
	std::vector<std::string> entries;
	for(const auto & entry : values)
		entries.push_back("{ slot: " + slot(entry.first) + ", amount: " + std::to_string(entry.second) + " }");
	return flowList(entries);
}

std::string fowTiles(const FowTilesType & tiles)
{
	std::map<std::pair<int, int>, std::vector<int>> runsByLine;
	for(const auto & tile : tiles)
		runsByLine[{tile.y, tile.z}].push_back(tile.x);

	std::vector<std::string> runs;
	for(auto & entry : runsByLine)
	{
		auto & xs = entry.second;
		std::sort(xs.begin(), xs.end());

		size_t start = 0;
		while(start < xs.size())
		{
			size_t end = start;
			while(end + 1 < xs.size() && xs[end + 1] == xs[end] + 1)
				++end;

			std::string run = "{ y: " + std::to_string(entry.first.first);
			if(entry.first.second != 0)
				run += ", z: " + std::to_string(entry.first.second);
			run += ", x: [" + std::to_string(xs[start]) + ", " + std::to_string(xs[end]) + "] }";
			runs.push_back(std::move(run));
			start = end + 1;
		}
	}
	return flowList(runs);
}

std::string compactTile2D(const int3 & tile)
{
	return std::to_string(tile.x) + "x" + std::to_string(tile.y);
}

std::string fowCircle(const int3 & center, int radius)
{
	std::string result = "{ ";
	if(center.z != 0)
		result += "z: " + std::to_string(center.z) + ", ";
	return result + "radius: " + std::to_string(radius) +
		", centers: [" + compactTile2D(center) + "] }";
}

std::string battleStackCreature(const CGameState & gameState, BattleID battleID, int stackID)
{
	const auto * battle = gameState.getBattle(battleID);
	if(!battle)
		return {};

	const auto * stack = battle->battleGetStackByID(stackID, false);
	if(!stack)
		return {};

	return creature(stack->creatureId());
}

std::string pluralizedBattleCreature(CreatureID creatureID)
{
	std::string identifier = CreatureID::encode(creatureID.getNum());
	boost::algorithm::replace_all(identifier, ":", "/");
	const std::string corePrefix = "core/";
	if(identifier.starts_with(corePrefix))
		identifier.erase(0, corePrefix.size());

	std::string result;
	for(const char character : identifier)
	{
		if(std::isupper(static_cast<unsigned char>(character)))
		{
			result += '-';
			result += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}
		else
			result += character;
	}
	const auto slash = result.rfind('/');
	std::string prefix = slash == std::string::npos ? "" : result.substr(0, slash + 1);
	std::string name = slash == std::string::npos ? result : result.substr(slash + 1);
	if(name.ends_with("shaman"))
		name += 's';
	else if(name.ends_with("man"))
		name.replace(name.size() - 3, 3, "men");
	else if(name.ends_with("elf"))
		name.replace(name.size() - 3, 3, "elves");
	else if(name.ends_with("dwarf"))
		name.replace(name.size() - 5, 5, "dwarves");
	else if(name == "cyclops")
		name = "cyclopes";
	else if(name == "efreet")
		name = "efreeti";
	else if(name == "pegasus")
		name = "pegasi";
	else if(name.ends_with("y") && name.size() > 1 && std::string("aeiou").find(name[name.size() - 2]) == std::string::npos)
		name.replace(name.size() - 1, 1, "ies");
	else if(name.ends_with("s") || name.ends_with("x") || name.ends_with("z") || name.ends_with("ch") || name.ends_with("sh"))
		name += "es";
	else
		name += "s";
	return prefix + name;
}

using BattleUnitAliases = std::map<int, std::map<int, std::string>>;
BattleUnitAliases battleUnitAliases;

std::string battleUnitBase(BattleSide side, CreatureID creatureID)
{
	return battleSide(side) + "/" + pluralizedBattleCreature(creatureID);
}

void resetBattleUnitAliases(const BattleInfo & battle, BattleID battleID)
{
	auto & aliases = battleUnitAliases[battleID.getNum()];
	aliases.clear();
	std::map<std::string, std::vector<int>> groups;
	for(const auto * stack : battle.battleGetAllStacks(true))
	{
		if(stack)
			groups[battleUnitBase(stack->unitSide(), stack->creatureId())].push_back(static_cast<int>(stack->unitId()));
	}
	for(auto & [base, stackIDs] : groups)
	{
		std::sort(stackIDs.begin(), stackIDs.end());
		for(size_t index = 0; index < stackIDs.size(); ++index)
			aliases[stackIDs[index]] = base + (stackIDs.size() == 1 ? "" : "/" + std::to_string(index + 1));
	}
}

void resetBattleUnitAliases(const CGameState & gameState, BattleID battleID)
{
	const auto * battle = gameState.getBattle(battleID);
	if(battle)
		resetBattleUnitAliases(*battle, battleID);
	else
		battleUnitAliases[battleID.getNum()].clear();
}

std::string registerBattleUnitAlias(BattleID battleID, int stackID, BattleSide side, CreatureID creatureID)
{
	auto & aliases = battleUnitAliases[battleID.getNum()];
	if(const auto existing = aliases.find(stackID); existing != aliases.end())
		return existing->second;

	const std::string base = battleUnitBase(side, creatureID);
	int nextOrdinal = 1;
	bool baseInUse = false;
	for(const auto & [ignoredID, alias] : aliases)
	{
		if(alias == base)
		{
			baseInUse = true;
			nextOrdinal = std::max(nextOrdinal, 2);
		}
		else if(alias.starts_with(base + "/"))
		{
			baseInUse = true;
			try
			{
				nextOrdinal = std::max(nextOrdinal, std::stoi(alias.substr(base.size() + 1)) + 1);
			}
			catch(const std::exception &)
			{
				nextOrdinal = std::max(nextOrdinal, 2);
			}
		}
	}
	const std::string alias = baseInUse ? base + "/" + std::to_string(nextOrdinal) : base;
	aliases[stackID] = alias;
	return alias;
}

std::string battleUnitAlias(const CGameState & gameState, BattleID battleID, int stackID)
{
	if(stackID < 0)
		return "hero";
	if(!battleUnitAliases.contains(battleID.getNum()))
		resetBattleUnitAliases(gameState, battleID);
	if(const auto & aliases = battleUnitAliases[battleID.getNum()]; aliases.contains(stackID))
		return aliases.at(stackID);
	const auto * battle = gameState.getBattle(battleID);
	if(!battle)
		return "unit-" + std::to_string(stackID);
	const auto * stack = battle->battleGetStackByID(stackID, false);
	if(!stack)
		return "unit-" + std::to_string(stackID);
	return registerBattleUnitAlias(battleID, stackID, stack->unitSide(), stack->creatureId());
}

std::vector<std::string> battleRoster(const CGameState & gameState, BattleID battleID)
{
	std::vector<std::string> result;
	const auto * battle = gameState.getBattle(battleID);
	if(!battle)
		return result;
	resetBattleUnitAliases(gameState, battleID);
	for(const auto * stack : battle->battleGetAllStacks(true))
	{
		if(!stack)
			continue;
		result.push_back(yamlKey(battleUnitAlias(gameState, battleID, static_cast<int>(stack->unitId()))) +
			": { stack: " + std::to_string(stack->unitId()) +
			", owner: " + color(battle->battleGetOwner(stack)) +
			", count: " + std::to_string(stack->getCount()) + " }");
	}
	return result;
}

std::map<std::string, int> battlePositions(const CGameState & gameState, BattleID battleID)
{
	std::map<std::string, int> result;
	const auto * battle = gameState.getBattle(battleID);
	if(!battle)
		return result;
	for(const auto * stack : battle->battleGetAllStacks(true))
	{
		if(stack)
			result[battleUnitAlias(gameState, battleID, static_cast<int>(stack->unitId()))] = stack->getPosition().toInt();
	}
	return result;
}

std::vector<std::string> battleSurvivors(const CGameState & gameState, BattleID battleID)
{
	std::vector<std::string> result;
	const auto * battle = gameState.getBattle(battleID);
	if(!battle)
		return result;
	for(const auto * stack : battle->battleGetAllStacks(true))
	{
		if(!stack || stack->summoned || stack->isTurret())
			continue;
		const int count = std::max(0, stack->getCount() - stack->health.getResurrected());
		if(count > 0)
			result.push_back(yamlKey(battleUnitAlias(gameState, battleID, static_cast<int>(stack->unitId()))) +
				": " + std::to_string(count));
	}
	return result;
}

std::vector<std::string> battleCreatedUnits(const CGameState & gameState, BattleID battleID)
{
	std::vector<std::string> result;
	const auto * battle = gameState.getBattle(battleID);
	if(!battle)
		return result;
	for(const auto * stack : battle->battleGetAllStacks(true))
	{
		if(!stack || stack->summoned || stack->isTurret() ||
			stack->unitSlot() != SlotID::SUMMONED_SLOT_PLACEHOLDER || stack->getCount() <= 0)
			continue;
		result.push_back(yamlKey(battleUnitAlias(gameState, battleID, static_cast<int>(stack->unitId()))) +
			": { creature: " + creature(stack->creatureId()) +
			", count: " + std::to_string(stack->unitBaseAmount()) +
			", hex: " + std::to_string(stack->getPosition().toInt()) + " }");
	}
	return result;
}

std::set<ObjectInstanceID> battleRandomizerParticipants(const IBattleInfo & battle)
{
	std::set<ObjectInstanceID> result;
	for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		if(const auto * army = battle.getSideArmy(side))
			result.insert(army->id);
		if(const auto * hero = battle.getSideHero(side))
			result.insert(hero->id);
	}
	return result;
}

std::set<ObjectInstanceID> battleRandomizerParticipants(const CGameState & gameState, BattleID battleID)
{
	const auto * battle = gameState.getBattle(battleID);
	return battle ? battleRandomizerParticipants(*battle) : std::set<ObjectInstanceID>();
}

std::set<HeroTypeID> battleRandomizerHeroes(const IBattleInfo & battle)
{
	std::set<HeroTypeID> result;
	for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		const auto * hero = battle.getSideHero(side);
		if(hero && hero->getHeroTypeID().hasValue())
			result.insert(hero->getHeroTypeID());
	}
	return result;
}

std::set<HeroTypeID> battleRandomizerHeroes(const CGameState & gameState, BattleID battleID)
{
	const auto * battle = gameState.getBattle(battleID);
	return battle ? battleRandomizerHeroes(*battle) : std::set<HeroTypeID>();
}

std::string battleParticipant(const CGameState & gameState, BattleID battleID, BattleSide side)
{
	const auto * battle = gameState.getBattle(battleID);
	if(!battle)
		return battleSide(side);
	if(const auto * hero = battle->battleGetFightingHero(side))
		return heroAlias(gameState, hero->id);
	if(const auto * army = battle->battleGetArmyObject(side))
		return objectAlias(gameState, army->id);
	return battleSide(side);
}

std::string battleCasualties(const std::map<CreatureID, si32> & casualties)
{
	std::vector<std::string> entries;
	for(const auto & [creatureID, count] : casualties)
	{
		if(count > 0)
			entries.push_back(creature(creatureID) + ": " + std::to_string(count));
	}
	return "{ " + boost::algorithm::join(entries, ", ") + " }";
}

void addBattleStackCreature(std::vector<std::string> & fields, const std::string & label, const CGameState & gameState, BattleID battleID, int stackID)
{
	if(const auto value = battleStackCreature(gameState, battleID, stackID); !value.empty())
		fields.push_back(label + ": " + value);
}

std::string battleTarget(const CGameState & gameState, BattleID battleID, const BattleAction::DestinationInfo & target)
{
	std::vector<std::string> fields;
	if(target.unitValue >= 0)
		fields.push_back("unit: " + yamlIdentifier(battleUnitAlias(gameState, battleID, target.unitValue)));
	if(target.hexValue.isValid())
		fields.push_back("hex: " + std::to_string(target.hexValue.toInt()));
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string battleAction(const CGameState & gameState, BattleID battleID, const BattleAction & action)
{
	std::vector<std::string> targets;
	for(const auto & target : action.target)
		targets.push_back(battleTarget(gameState, battleID, target));

	std::vector<std::string> fields;
	fields.push_back("side: " + battleSide(action.side));
	fields.push_back("unit: " + yamlIdentifier(battleUnitAlias(gameState, battleID, static_cast<int>(action.stackNumber))));
	fields.push_back("action: " + actionType(action.actionType));
	if(action.spell != SpellID::NONE)
		fields.push_back("spell: " + spell(action.spell));
	if(!targets.empty())
		fields.push_back("target: " + flowList(targets));
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string battleUnitState(const UnitChanges & change);

std::string battleUnitChanges(const CGameState & gameState, BattleID battleID, const std::vector<UnitChanges> & changes)
{
	std::vector<std::string> result;
	for(const auto & change : changes)
	{
		std::string operation;
		switch(change.operation)
		{
			case BattleChanges::EOperation::ADD: operation = "add"; break;
			case BattleChanges::EOperation::UPDATE: operation = "update"; break;
			case BattleChanges::EOperation::REMOVE: operation = "remove"; break;
		}

		result.push_back("{ unit: " + yamlIdentifier(battleUnitAlias(gameState, battleID, change.id)) +
			", operation: " + operation +
			", healthDelta: " + std::to_string(change.healthDelta) +
			", state: " + battleUnitState(change) + " }");
	}
	return flowList(result);
}

std::string battleChangeOperation(BattleChanges::EOperation operation)
{
	switch(operation)
	{
		case BattleChanges::EOperation::ADD: return "add";
		case BattleChanges::EOperation::UPDATE: return "update";
		case BattleChanges::EOperation::REMOVE: return "remove";
	}
	return "unknown";
}

void appendJsonInteger(std::vector<std::string> & fields, const JsonNode & node, const std::string & key, const std::string & label)
{
	const auto & child = node[key];
	if(!child.isNull() && child.isNumber())
		fields.push_back(label + ": " + std::to_string(static_cast<si64>(child.Float())));
}

void appendJsonBool(std::vector<std::string> & fields, const JsonNode & node, const std::string & key, const std::string & label)
{
	const auto & child = node[key];
	if(!child.isNull() && child.isBool())
		fields.push_back(label + ": " + std::string(child.Bool() ? "true" : "false"));
}

std::string battleUnitState(const UnitChanges & change)
{
	std::vector<std::string> fields;
	fields.push_back("operation: " + battleChangeOperation(change.operation));
	if(change.healthDelta)
		fields.push_back("healthDelta: " + std::to_string(change.healthDelta));

	const JsonNode & state = change.data["state"];
	if(!state.isNull() && state.isStruct())
	{
		appendJsonInteger(fields, state, "position", "position");
		appendJsonBool(fields, state, "moved", "moved");
		appendJsonBool(fields, state, "defending", "defending");
		appendJsonBool(fields, state, "waiting", "waiting");
		appendJsonBool(fields, state, "waitedThisTurn", "waitedThisTurn");

		const JsonNode & health = state["health"];
		if(!health.isNull() && health.isStruct())
		{
			std::vector<std::string> healthFields;
			appendJsonInteger(healthFields, health, "fullUnits", "fullUnits");
			appendJsonInteger(healthFields, health, "firstHPleft", "firstHPleft");
			if(!healthFields.empty())
				fields.push_back("health: { " + boost::algorithm::join(healthFields, ", ") + " }");
		}
	}

	if(fields.empty())
		return "{}";
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string battleStackAttacked(const CGameState & gameState, BattleID battleID, const BattleStackAttacked & attack)
{
	std::vector<std::string> fields;
	fields.push_back("target: " + yamlIdentifier(battleUnitAlias(gameState, battleID, attack.stackAttacked)));
	addBattleStackCreature(fields, "targetCreature", gameState, battleID, attack.stackAttacked);
	fields.push_back("attacker: " + yamlIdentifier(battleUnitAlias(gameState, battleID, attack.attackerID)));
	addBattleStackCreature(fields, "attackerCreature", gameState, battleID, attack.attackerID);
	fields.push_back("damage: " + std::to_string(attack.damageAmount));
	fields.push_back("killed: " + std::to_string(attack.killedAmount));
	fields.push_back("flags: " + std::to_string(attack.flags));
	if(attack.spellID != SpellID::NONE)
		fields.push_back("spell: " + spell(attack.spellID));
	if(!attack.newState.data.isNull())
		fields.push_back("state: " + battleUnitState(attack.newState));
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string battleLeft(const UnitChanges & change)
{
	const JsonNode & state = change.data["state"];
	if(state.isNull() || !state.isStruct())
		return {};
	std::vector<std::string> fields;
	const JsonNode & health = state["health"];
	if(!health.isNull() && health.isStruct())
	{
		const JsonNode & fullUnits = health["fullUnits"];
		const JsonNode & firstHP = health["firstHPleft"];
		const int64_t full = fullUnits.isNull() ? 0 : static_cast<int64_t>(fullUnits.Float());
		const int64_t top = firstHP.isNull() ? 0 : static_cast<int64_t>(firstHP.Float());
		fields.push_back("count: " + std::to_string(full + (top > 0 ? 1 : 0)));
		if(top > 0)
			fields.push_back("topHp: " + std::to_string(top));
	}
	appendJsonInteger(fields, state, "position", "at");
	return fields.empty() ? "" : "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string battleStackAttacks(const CGameState & gameState, BattleID battleID, const std::vector<BattleStackAttacked> & attacks)
{
	std::vector<std::string> result;
	for(const auto & attack : attacks)
		result.push_back(battleStackAttacked(gameState, battleID, attack));
	return flowList(result);
}

std::string battleAttackHits(const CGameState & gameState, BattleID battleID, const std::vector<BattleStackAttacked> & attacks)
{
	std::vector<std::string> result;
	for(const auto & attack : attacks)
	{
		std::vector<std::string> fields;
		fields.push_back("target: " + yamlIdentifier(battleUnitAlias(gameState, battleID, attack.stackAttacked)));
		fields.push_back("damage: " + std::to_string(attack.damageAmount));
		if(attack.killedAmount != 0)
			fields.push_back("killed: " + std::to_string(attack.killedAmount));
		if(const auto left = battleLeft(attack.newState); !left.empty())
			fields.push_back("after: " + left);
		if(attack.willRebirth())
		{
			if(const auto left = battleLeft(attack.newState); !left.empty())
				fields.push_back("reborn: " + left);
		}
		if(attack.spellID != SpellID::NONE)
			fields.push_back("spell: " + spell(attack.spellID));
		result.push_back("{ " + boost::algorithm::join(fields, ", ") + " }");
	}
	return flowList(result);
}

std::string artifactLocation(const CGameState & gameState, const ArtifactLocation & location)
{
	std::vector<std::string> fields;
	fields.push_back("holder: " + objectAlias(gameState, location.artHolder));
	if(location.creature)
		fields.push_back("creatureSlot: " + slot(*location.creature));
	fields.push_back("slot: " + artifactPosition(location.slot));
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string marketTradePrefix(
	const CGameState & gameState,
	const TradeOnMarketplace & pack,
	const std::string & kind,
	bool needsHero)
{
	std::string result = kind + ": { actor: " + actorForPlayer(pack.player) +
		", at: " + objectAlias(gameState, pack.marketId);
	if(needsHero || pack.heroId != ObjectInstanceID::NONE)
		result += ", hero: " + heroAlias(gameState, pack.heroId);
	return result;
}

std::string marketResourceAmount(GameResID resource, int64_t amount)
{
	return "{ " + resourceKey(resource) + ": " + std::to_string(amount) + " }";
}

std::string marketSlotCreature(const CGameState & gameState, ObjectInstanceID heroID, SlotID slotID)
{
	const auto * hero = gameState.getHero(heroID);
	if(!hero || !hero->hasStackAtSlot(slotID))
		return "unknown";
	return creature(hero->getStack(slotID).getId());
}

std::string marketArtifact(const CGameState & gameState, ArtifactInstanceID instanceID)
{
	const auto * instance = gameState.getMap().getArtifactInstance(instanceID);
	return instance ? artifact(instance->getTypeId()) : "unknown";
}

std::optional<std::string> semanticMarketTrade(const CGameState & gameState, const TradeOnMarketplace & pack)
{
	if(pack.mode == EMarketMode::RESOURCE_RESOURCE)
		return std::nullopt;
	if(pack.r1.size() != pack.r2.size() || pack.r1.size() != pack.val.size())
		return std::nullopt;

	const auto * market = gameState.getMarket(pack.marketId);
	if(!market)
		return std::nullopt;

	switch(pack.mode)
	{
		case EMarketMode::RESOURCE_PLAYER:
		{
			std::map<PlayerColor, ResourceSet> transfers;
			for(size_t index = 0; index < pack.r1.size(); ++index)
				transfers[pack.r2[index].as<PlayerColor>()][pack.r1[index].as<GameResID>()] += pack.val[index];
			std::string line = marketTradePrefix(gameState, pack, "sendResources", false);
			if(transfers.size() == 1)
			{
				line += ", to: " + color(transfers.begin()->first) +
					", resources: " + resourceMap(transfers.begin()->second);
			}
			else
			{
				std::vector<std::string> entries;
				for(const auto & [player, resources] : transfers)
					entries.push_back("{ to: " + color(player) + ", resources: " + resourceMap(resources) + " }");
				line += ", transfers: " + flowList(entries);
			}
			return line + " }";
		}
		case EMarketMode::CREATURE_RESOURCE:
		{
			std::vector<std::string> sales;
			for(size_t index = 0; index < pack.r1.size(); ++index)
			{
				const auto slotID = pack.r1[index].as<SlotID>();
				const auto resource = pack.r2[index].as<GameResID>();
				int offeredUnits = 0;
				int offeredResources = 0;
				int64_t received = 0;
				if(const auto * hero = gameState.getHero(pack.heroId); hero && hero->hasStackAtSlot(slotID) &&
					market->getOffer(hero->getStack(slotID).getId(), resource, offeredUnits, offeredResources,
						EMarketMode::CREATURE_RESOURCE) && offeredUnits > 0)
					received = static_cast<int64_t>(pack.val[index] / offeredUnits) * offeredResources;
				sales.push_back("{ slot: " + slot(slotID) +
					", creature: " + marketSlotCreature(gameState, pack.heroId, slotID) +
					", count: " + std::to_string(pack.val[index]) +
					", received: " + marketResourceAmount(resource, received) + " }");
			}
			return marketTradePrefix(gameState, pack, "sellCreatures", true) +
				", sales: " + flowList(sales) + " }";
		}
		case EMarketMode::RESOURCE_ARTIFACT:
		{
			std::vector<std::string> purchases;
			for(size_t index = 0; index < pack.r1.size(); ++index)
			{
				const auto resource = pack.r1[index].as<GameResID>();
				const auto artifactID = pack.r2[index].as<ArtifactID>();
				int paid = 0;
				int received = 0;
				market->getOffer(resource, artifactID, paid, received, EMarketMode::RESOURCE_ARTIFACT);
				purchases.push_back("{ artifact: " + artifact(artifactID) +
					", paid: " + marketResourceAmount(resource, paid) + " }");
			}
			return marketTradePrefix(gameState, pack, "buyArtifacts", true) +
				", purchases: " + flowList(purchases) + " }";
		}
		case EMarketMode::ARTIFACT_RESOURCE:
		{
			std::vector<std::string> sales;
			for(size_t index = 0; index < pack.r1.size(); ++index)
			{
				const auto instanceID = pack.r1[index].as<ArtifactInstanceID>();
				const auto resource = pack.r2[index].as<GameResID>();
				int sold = 0;
				int received = 0;
				if(const auto * instance = gameState.getMap().getArtifactInstance(instanceID))
					market->getOffer(instance->getTypeId(), resource, sold, received, EMarketMode::ARTIFACT_RESOURCE);
				sales.push_back("{ artifact: " + marketArtifact(gameState, instanceID) +
					", instance: " + std::to_string(instanceID.getNum()) +
					", received: " + marketResourceAmount(resource, received) + " }");
			}
			return marketTradePrefix(gameState, pack, "sellArtifacts", true) +
				", sales: " + flowList(sales) + " }";
		}
		case EMarketMode::CREATURE_UNDEAD:
		{
			std::vector<std::string> stacks;
			for(const auto & item : pack.r1)
			{
				const auto slotID = item.as<SlotID>();
				stacks.push_back("{ slot: " + slot(slotID) +
					", creature: " + marketSlotCreature(gameState, pack.heroId, slotID) + " }");
			}
			return marketTradePrefix(gameState, pack, "transformUndead", false) +
				", stacks: " + flowList(stacks) + " }";
		}
		case EMarketMode::RESOURCE_SKILL:
		{
			std::vector<std::string> skills;
			for(const auto & item : pack.r2)
				skills.push_back(secondarySkill(item.as<SecondarySkill>()));
			return marketTradePrefix(gameState, pack, "learnSkills", true) +
				", skills: " + flowList(skills) + " }";
		}
		case EMarketMode::CREATURE_EXP:
		{
			std::vector<std::string> stacks;
			for(size_t index = 0; index < pack.r1.size(); ++index)
			{
				const auto slotID = pack.r1[index].as<SlotID>();
				stacks.push_back("{ slot: " + slot(slotID) +
					", creature: " + marketSlotCreature(gameState, pack.heroId, slotID) +
					", count: " + std::to_string(pack.val[index]) + " }");
			}
			return marketTradePrefix(gameState, pack, "sacrificeCreatures", true) +
				", stacks: " + flowList(stacks) + " }";
		}
		case EMarketMode::ARTIFACT_EXP:
		{
			std::vector<std::string> artifacts;
			for(const auto & item : pack.r1)
			{
				const auto instanceID = item.as<ArtifactInstanceID>();
				artifacts.push_back("{ artifact: " + marketArtifact(gameState, instanceID) +
					", instance: " + std::to_string(instanceID.getNum()) + " }");
			}
			return marketTradePrefix(gameState, pack, "sacrificeArtifacts", true) +
				", artifacts: " + flowList(artifacts) + " }";
		}
		case EMarketMode::RESOURCE_RESOURCE:
		case EMarketMode::MARKET_AFTER_LAST_PLACEHOLDER:
			break;
	}
	return std::nullopt;
}

std::string artifactMove(const MoveArtifactInfo & move)
{
	std::vector<std::string> fields;
	fields.push_back("from: " + artifactPosition(move.srcPos));
	fields.push_back("to: " + artifactPosition(move.dstPos));
	if(move.askAssemble)
		fields.push_back("askAssemble: true");
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string artifactMoves(const std::vector<MoveArtifactInfo> & moves)
{
	std::vector<std::string> entries;
	for(const auto & move : moves)
		entries.push_back(artifactMove(move));
	return flowList(entries);
}

std::string bulkArtifactMove(const CGameState & gameState, const BulkMoveArtifacts & pack)
{
	std::vector<std::string> fields;
	fields.push_back("owner: " + color(pack.interfaceOwner));
	fields.push_back("from: " + objectAlias(gameState, pack.srcArtHolder));
	fields.push_back("to: " + objectAlias(gameState, pack.dstArtHolder));
	if(pack.srcCreature)
		fields.push_back("fromCreatureSlot: " + slot(*pack.srcCreature));
	if(pack.dstCreature)
		fields.push_back("toCreatureSlot: " + slot(*pack.dstCreature));
	fields.push_back("movesFromSource: " + artifactMoves(pack.artsPack0));
	if(!pack.artsPack1.empty())
		fields.push_back("movesFromDestination: " + artifactMoves(pack.artsPack1));
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string bulkArtifactMoves(const CGameState & gameState, const std::vector<BulkMoveArtifacts> & moves)
{
	std::vector<std::string> entries;
	for(const auto & move : moves)
		entries.push_back(bulkArtifactMove(gameState, move));
	return flowList(entries);
}

void writeJsonCompact(std::ostream & out, const JsonNode & node)
{
	switch(node.getType())
	{
		case JsonNode::JsonType::DATA_NULL:
			out << "null";
			break;
		case JsonNode::JsonType::DATA_BOOL:
			out << (node.Bool() ? "true" : "false");
			break;
		case JsonNode::JsonType::DATA_FLOAT:
			out << node.Float();
			break;
		case JsonNode::JsonType::DATA_STRING:
			out << yamlString(node.String());
			break;
		case JsonNode::JsonType::DATA_VECTOR:
		{
			out << "[";
			bool first = true;
			for(const auto & entry : node.Vector())
			{
				if(!first)
					out << ", ";
				first = false;
				writeJsonCompact(out, entry);
			}
			out << "]";
			break;
		}
		case JsonNode::JsonType::DATA_STRUCT:
		{
			out << "{";
			bool first = true;
			for(const auto & entry : node.Struct())
			{
				if(!first)
					out << ", ";
				first = false;
				out << yamlString(entry.first) << ": ";
				writeJsonCompact(out, entry.second);
			}
			out << "}";
			break;
		}
		case JsonNode::JsonType::DATA_INTEGER:
			out << node.Integer();
			break;
	}
}

std::string jsonCompact(const JsonNode & node)
{
	std::ostringstream out;
	writeJsonCompact(out, node);
	return out.str();
}

bool isYamlScalarOrEmpty(const JsonNode & node)
{
	if(node.isVector())
		return node.Vector().empty();
	if(node.isStruct())
		return node.Struct().empty();
	return true;
}

void writeYamlIndent(std::ostream & out, size_t indent)
{
	for(size_t index = 0; index < indent; ++index)
		out << ' ';
}

void writeJsonYamlBlock(std::ostream & out, const JsonNode & node, size_t indent)
{
	if(isYamlScalarOrEmpty(node))
	{
		writeJsonCompact(out, node);
		out << "\n";
		return;
	}

	if(node.isStruct())
	{
		out << "\n";
		for(const auto & entry : node.Struct())
		{
			writeYamlIndent(out, indent);
			out << yamlKey(entry.first) << ": ";
			if(isYamlScalarOrEmpty(entry.second))
			{
				writeJsonCompact(out, entry.second);
				out << "\n";
			}
			else
				writeJsonYamlBlock(out, entry.second, indent + 2);
		}
		return;
	}

	if(node.isVector())
	{
		out << "\n";
		for(const auto & entry : node.Vector())
		{
			writeYamlIndent(out, indent);
			out << "- ";
			if(isYamlScalarOrEmpty(entry))
			{
				writeJsonCompact(out, entry);
				out << "\n";
			}
			else
				writeJsonYamlBlock(out, entry, indent + 2);
		}
		return;
	}

	writeJsonCompact(out, node);
	out << "\n";
}

std::string gameSettingsOverrides(const JsonNode & node)
{
	return node.isNull() ? "{}" : jsonCompact(node);
}

std::string lowerCamelEnum(std::string value)
{
	std::string result;
	bool capitalize = false;
	for(const char character : value)
	{
		if(character == '_')
		{
			capitalize = true;
			continue;
		}
		if(capitalize)
		{
			result += static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
			capitalize = false;
		}
		else
			result += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	return result;
}

std::string bonusTargetKind(GiveBonus::ETarget value)
{
	switch(value)
	{
		case GiveBonus::ETarget::OBJECT: return "object";
		case GiveBonus::ETarget::PLAYER: return "player";
		case GiveBonus::ETarget::BATTLE: return "battle";
		case GiveBonus::ETarget::HERO_COMMANDER: return "heroCommander";
	}
	return "unknown";
}

std::string bonusTarget(const CGameState & gameState, const GiveBonus & pack)
{
	switch(pack.who)
	{
		case GiveBonus::ETarget::OBJECT:
			return objectAlias(gameState, pack.id.as<ObjectInstanceID>());
		case GiveBonus::ETarget::PLAYER:
			return color(pack.id.as<PlayerColor>());
		case GiveBonus::ETarget::BATTLE:
			return battleAlias(pack.id.as<BattleID>());
		case GiveBonus::ETarget::HERO_COMMANDER:
			return heroAlias(gameState, pack.id.as<ObjectInstanceID>()) + "/commander";
	}
	return "unknown";
}

std::string semanticBonus(const CGameState & gameState, const GiveBonus & pack)
{
	const Bonus & bonus = pack.bonus;
	std::vector<std::string> fields;
	if(pack.who != GiveBonus::ETarget::BATTLE)
	{
		fields.push_back("targetKind: " + bonusTargetKind(pack.who));
		fields.push_back("target: " + bonusTarget(gameState, pack));
	}
	fields.push_back("type: " + lowerCamelEnum(LIBRARY->bth->bonusToString(bonus.type)));
	fields.push_back("value: " + signedInteger(bonus.val));

	std::vector<std::string> durations;
	for(const auto & [name, value] : bonusDurationMap)
	{
		if(name != "UNITL_BEING_ATTACKED" && (bonus.duration & value) != 0)
			durations.push_back(lowerCamelEnum(name));
	}
	if(durations.size() == 1)
		fields.push_back("duration: " + durations.front());
	else if(!durations.empty())
		fields.push_back("duration: " + flowList(durations));
	if(bonus.sid != BonusSourceID())
		fields.push_back("source: " + transcriptIdentifier(bonus.sid.toString()));
	if(bonus.source != BonusSource::OTHER)
		fields.push_back("sourceKind: " + lowerCamelEnum(vstd::findKey(bonusSourceMap, bonus.source)));
	if(bonus.subtype != BonusSubtypeID())
		fields.push_back("subtype: " + yamlString(bonus.subtype.toString()));
	if(bonus.turnsRemain)
		fields.push_back("turns: " + std::to_string(bonus.turnsRemain));
	if(bonus.valType != BonusValueType::ADDITIVE_VALUE)
		fields.push_back("valueKind: " + lowerCamelEnum(vstd::findKey(bonusValueMap, bonus.valType)));
	if(!bonus.stacking.empty())
		fields.push_back("stacking: " + yamlString(bonus.stacking));
	if(bonus.description.hasCustomText())
		fields.push_back("text: " + yamlString(bonus.description.toString()));
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string hexDigest(const std::array<uint32_t, 8> & words)
{
	std::ostringstream out;
	for(uint32_t word : words)
		out << std::hex << std::setfill('0') << std::setw(8) << word;
	return out.str();
}

uint32_t rotateRight(uint32_t value, uint32_t bits)
{
	return (value >> bits) | (value << (32 - bits));
}

std::string sha256(const uint8_t * data, size_t size)
{
	static constexpr std::array<uint32_t, 64> constants = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
	};

	std::array<uint32_t, 8> hash = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};

	std::vector<uint8_t> message(data, data + size);
	const uint64_t bitLength = static_cast<uint64_t>(size) * 8;
	message.push_back(0x80);
	while((message.size() % 64) != 56)
		message.push_back(0);
	for(int shift = 56; shift >= 0; shift -= 8)
		message.push_back(static_cast<uint8_t>((bitLength >> shift) & 0xff));

	for(size_t offset = 0; offset < message.size(); offset += 64)
	{
		std::array<uint32_t, 64> words = {};
		for(size_t i = 0; i < 16; ++i)
		{
			const size_t index = offset + i * 4;
			words[i] = (static_cast<uint32_t>(message[index]) << 24) |
				(static_cast<uint32_t>(message[index + 1]) << 16) |
				(static_cast<uint32_t>(message[index + 2]) << 8) |
				static_cast<uint32_t>(message[index + 3]);
		}
		for(size_t i = 16; i < 64; ++i)
		{
			const uint32_t s0 = rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
			const uint32_t s1 = rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
			words[i] = words[i - 16] + s0 + words[i - 7] + s1;
		}

		uint32_t a = hash[0];
		uint32_t b = hash[1];
		uint32_t c = hash[2];
		uint32_t d = hash[3];
		uint32_t e = hash[4];
		uint32_t f = hash[5];
		uint32_t g = hash[6];
		uint32_t h = hash[7];

		for(size_t i = 0; i < 64; ++i)
		{
			const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
			const uint32_t ch = (e & f) ^ ((~e) & g);
			const uint32_t temp1 = h + s1 + ch + constants[i] + words[i];
			const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
			const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t temp2 = s0 + maj;

			h = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
		}

		hash[0] += a;
		hash[1] += b;
		hash[2] += c;
		hash[3] += d;
		hash[4] += e;
		hash[5] += f;
		hash[6] += g;
		hash[7] += h;
	}

	return hexDigest(hash);
}

std::optional<std::string> mapHash(const StartInfo & startInfo)
{
	auto hashFile = [](const boost::filesystem::path & filePath) -> std::optional<std::string>
	{
		if(!boost::filesystem::exists(filePath))
			return std::nullopt;

		std::ifstream input(filePath.string(), std::ios::binary);
		std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if(data.empty())
			return std::nullopt;

		return sha256(data.data(), data.size());
	};

	try
	{
		ResourcePath mapPath(startInfo.fileURI, EResType::MAP);
		if(CResourceHandler::get()->existsResource(mapPath))
		{
			auto stream = CResourceHandler::get()->load(mapPath);
			auto data = stream->readAll();
			return sha256(data.first.get(), static_cast<size_t>(data.second));
		}
	}
	catch(...)
	{
	}

	try
	{
		if(const auto result = hashFile(boost::filesystem::path(startInfo.fileURI)))
			return result;
	}
	catch(...)
	{
	}

	try
	{
		if(const auto result = hashFile(VCMIDirs::get().userDataPath() / startInfo.fileURI))
			return result;
	}
	catch(...)
	{
	}

	return std::nullopt;
}

std::string mapTextEncoding(const StartInfo & startInfo)
{
	std::string uri = startInfo.fileURI;
	boost::algorithm::to_lower(uri);
	if(!uri.ends_with(".h3m"))
		return "{ source: utf-8, stored: utf-8 }";

	std::string fallback = "unknown";
	try
	{
		fallback = LIBRARY->modh->findResourceEncoding(ResourcePath(startInfo.fileURI, EResType::MAP));
	}
	catch(...)
	{
	}
	return "{ source: h3m-auto, fallback: " + yamlString(fallback) + ", stored: utf-8 }";
}

std::string startMode(EStartMode mode)
{
	switch(mode)
	{
		case EStartMode::NEW_GAME: return "newGame";
		case EStartMode::LOAD_GAME: return "loadGame";
		case EStartMode::CAMPAIGN: return "campaign";
		case EStartMode::INVALID: return "invalid";
	}
	return "unknown";
}

std::string difficulty(ui8 value)
{
	if(value < std::size(GameConstants::DIFFICULTY_NAMES))
		return GameConstants::DIFFICULTY_NAMES[value];
	return std::to_string(value);
}

class DecisionRecorder final : public ICPackVisitor
{
	const CGameState & gameState;
	std::string line;

	void unmodelled(CPackForServer & pack)
	{
		line = "unmodelled: { stream: decision, pack: " + yamlString(packetTypeName(pack)) + ", material: true }";
	}

public:
	explicit DecisionRecorder(const CGameState & gameState)
		: gameState(gameState)
	{
	}

	const std::string & result() const
	{
		return line;
	}

	void visitForServer(CPackForServer & pack) override
	{
		unmodelled(pack);
	}

	void visitEndTurn(EndTurn & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) + ", kind: endTurn }";
	}

	void visitDismissHero(DismissHero & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: dismissHero, hero: " + heroAlias(gameState, pack.hid) + " }";
	}

	void visitMoveHero(MoveHero & pack) override
	{
		unmodelled(pack);
	}

	void visitCastleTeleportHero(CastleTeleportHero & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: castleTeleportHero, hero: " + heroAlias(gameState, pack.hid) +
			", destination: " + objectAlias(gameState, pack.dest) +
			", source: " + std::to_string(pack.source) + " }";
	}

	void visitBuildStructure(BuildStructure & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: buildStructure, town: " + objectAlias(gameState, pack.tid) +
			", building: " + building(pack.bid) + " }";
	}

	void visitVisitTownBuilding(VisitTownBuilding & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: visitTownBuilding, town: " + objectAlias(gameState, pack.tid) +
			", building: " + building(pack.bid) + " }";
	}

	void visitRazeStructure(RazeStructure & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: razeStructure, town: " + objectAlias(gameState, pack.tid) +
			", building: " + building(pack.bid) + " }";
	}

	void visitSpellResearch(SpellResearch & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: spellResearch, town: " + objectAlias(gameState, pack.tid) +
			", spell: " + spell(pack.spellAtSlot) +
			", accepted: " + boolValue(pack.accepted) + " }";
	}

	void visitMakeAction(MakeAction & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: battleAction, battle: " + battleAlias(pack.battleID) +
			", action: " + battleAction(gameState, pack.battleID, pack.ba) + " }";
	}

	void visitDigWithHero(DigWithHero & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: dig, hero: " + heroAlias(gameState, pack.id) + " }";
	}

	void visitCastAdvSpell(CastAdvSpell & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: castAdventureSpell, hero: " + heroAlias(gameState, pack.hid) +
			", spell: " + spell(pack.sid) +
			", position: " + pos(pack.pos) + " }";
	}

	void visitArrangeStacks(ArrangeStacks & pack) override
	{
		const std::string kind = pack.what == 1 ? "swapStacks" : pack.what == 2 ? "mergeStacks" : "splitStack";
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: " + kind +
			", from: { army: " + objectAlias(gameState, pack.id1) +
			", slot: " + slot(pack.p1) + " }" +
			(pack.what == 2 ? ", into: { army: " : ", to: { army: ") + objectAlias(gameState, pack.id2) +
			", slot: " + slot(pack.p2) + " }";
		if(pack.what == 3)
			line += ", count: " + std::to_string(pack.val);
		line += " }";
	}

	void visitBulkMoveArmy(BulkMoveArmy & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: bulkMoveArmy, from: { army: " + objectAlias(gameState, pack.srcArmy) +
			", slot: " + slot(pack.srcSlot) + " }" +
			", to: " + objectAlias(gameState, pack.destArmy) + " }";
	}

	void visitBulkSplitStack(BulkSplitStack & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: bulkSplitStack, source: { army: " + objectAlias(gameState, pack.srcOwner) +
			", slot: " + slot(pack.src) + " }" +
			", amount: " + std::to_string(pack.amount) + " }";
	}

	void visitBulkMergeStacks(BulkMergeStacks & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: bulkMergeStacks, source: { army: " + objectAlias(gameState, pack.srcOwner) +
			", slot: " + slot(pack.src) + " } }";
	}

	void visitBulkSplitAndRebalanceStack(BulkSplitAndRebalanceStack & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: bulkSplitAndRebalanceStack, source: { army: " + objectAlias(gameState, pack.srcOwner) +
			", slot: " + slot(pack.src) + " } }";
	}

	void visitDisbandCreature(DisbandCreature & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: disbandCreature, army: " + objectAlias(gameState, pack.id) +
			", slot: " + slot(pack.pos) + " }";
	}

	void visitUpgradeCreature(UpgradeCreature & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: upgradeCreature, army: " + objectAlias(gameState, pack.id) +
			", slot: " + slot(pack.pos) +
			", creature: " + creature(pack.cid) + " }";
	}

	void visitGarrisonHeroSwap(GarrisonHeroSwap & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: swapTownHeroes, town: " + objectAlias(gameState, pack.tid) + " }";
	}

	void visitExchangeArtifacts(ExchangeArtifacts & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: exchangeArtifacts, from: " + artifactLocation(gameState, pack.src) +
			", to: " + artifactLocation(gameState, pack.dst) + " }";
	}

	void visitBulkExchangeArtifacts(BulkExchangeArtifacts & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: bulkExchangeArtifacts, from: " + objectAlias(gameState, pack.srcHero) +
			", to: " + objectAlias(gameState, pack.dstHero) +
			", swap: " + boolValue(pack.swap) +
			", equipped: " + boolValue(pack.equipped) +
			", backpack: " + boolValue(pack.backpack) + " }";
	}

	void visitManageBackpackArtifacts(ManageBackpackArtifacts & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: manageBackpackArtifacts, holder: " + objectAlias(gameState, pack.artHolder) +
			", command: " + backpackManageCommand(pack.cmd) + " }";
	}

	void visitManageEquippedArtifacts(ManageEquippedArtifacts & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: manageEquippedArtifacts, holder: " + objectAlias(gameState, pack.artHolder) +
			", costume: " + std::to_string(pack.costumeIdx);
		if(pack.saveCostume)
			line += ", save: true";
		line += " }";
	}

	void visitAssembleArtifacts(AssembleArtifacts & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: assembleArtifacts, hero: " + heroAlias(gameState, pack.heroID) +
			", slot: " + artifactPosition(pack.artifactSlot) +
			", assemble: " + boolValue(pack.assemble) +
			", artifact: " + artifact(pack.assembleTo) + " }";
	}

	void visitEraseArtifactByClient(EraseArtifactByClient & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: eraseArtifact, location: " + artifactLocation(gameState, pack.al) + " }";
	}

	void visitBuyArtifact(BuyArtifact & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: buyArtifact, hero: " + heroAlias(gameState, pack.hid) +
			", artifact: " + artifact(pack.aid) + " }";
	}

	void visitSetFormation(SetFormation & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: setFormation, hero: " + heroAlias(gameState, pack.hid) +
			", formation: " + armyFormation(pack.formation) + " }";
	}

	void visitSetTactics(SetTactics & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: setTactics, hero: " + heroAlias(gameState, pack.hid) +
			", enabled: " + boolValue(pack.enabled) + " }";
	}

	void visitSetTownName(SetTownName & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: setTownName, town: " + objectAlias(gameState, pack.tid) +
			", name: " + yamlString(pack.name) + " }";
	}

	void visitBuildBoat(BuildBoat & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: buildBoat, object: " + objectAlias(gameState, pack.objid) + " }";
	}

	void visitSaveGame(SaveGame &) override
	{
		line.clear();
	}

	void visitSaveLocalState(SaveLocalState & pack) override
	{
		line = "localState: { player: " + color(pack.player) + ", data: " + jsonCompact(pack.data) + " }";
	}

	void visitGamePause(GamePause & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) + ", kind: pauseTimer }";
	}

	void visitRequestStatistic(RequestStatistic &) override
	{
		line.clear();
	}

	void visitPlayerMessage(PlayerMessage & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: playerMessage, text: " + yamlString(pack.text) +
			", object: " + objectAlias(gameState, pack.currObj) + " }";
	}

	void visitAdvInterfaceReady(AdvInterfaceReady & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) + ", kind: ready }";
	}
};

class EffectRecorder final : public ICPackVisitor
{
	const CGameState & gameState;
	std::optional<std::string> turnStartTimer;
	std::optional<std::string> turnEndTimer;
	std::string line;

	void unmodelled(CPackForClient & pack)
	{
		line = "unmodelled: { stream: effect, pack: " + yamlString(packetTypeName(pack)) + ", material: true }";
	}

public:
	explicit EffectRecorder(
		const CGameState & gameState,
		std::optional<std::string> turnStartTimer = std::nullopt,
		std::optional<std::string> turnEndTimer = std::nullopt)
		: gameState(gameState)
		, turnStartTimer(std::move(turnStartTimer))
		, turnEndTimer(std::move(turnEndTimer))
	{
	}

	const std::string & result() const
	{
		return line;
	}

	void visitForClient(CPackForClient & pack) override
	{
		unmodelled(pack);
	}

	void visitPackageReceived(PackageReceived &) override
	{
		line.clear();
	}

	void visitPackageApplied(PackageApplied &) override
	{
		line.clear();
	}

	void visitTurnTimeUpdate(TurnTimeUpdate & pack) override
	{
		line.clear();
	}

	void visitPlayerBlocked(PlayerBlocked &) override
	{
		line.clear();
	}

	void visitSystemMessage(SystemMessage &) override
	{
		// Generated server/chat status belongs to the UI, not game history.
		line.clear();
	}

	void visitPlayerStartsTurn(PlayerStartsTurn &) override
	{
		line.clear();
	}

	void visitPlayerEndsTurn(PlayerEndsTurn & pack) override
	{
		if(!turnStartTimer && !turnEndTimer)
		{
			line.clear();
			return;
		}
		line = "turnEnd: { player: " + color(pack.player) +
			", timer: { start: " + turnStartTimer.value_or("none") +
			", end: " + turnEndTimer.value_or("none") + " } }";
	}

	void visitDaysWithoutTown(DaysWithoutTown & pack) override
	{
		line = "townless: { player: " + color(pack.player);
		if(pack.daysWithoutCastle)
			line += ", days: " + std::to_string(*pack.daysWithoutCastle);
		else
			line += ", cleared: true";
		line += " }";
	}

	void visitPlayerEndsGame(PlayerEndsGame & pack) override
	{
		line = "playerEnd: { player: " + color(pack.player) +
			", result: " + victoryLossResult(pack.victoryLossCheckResult);
		if(pack.silentEnd)
			line += ", silent: true";
		line += " }";
	}

	void visitSetResources(SetResources & pack) override
	{
		const std::string key = pack.mode == ChangeValueMode::ABSOLUTE ? "setResources" : "resources";
		line = key + ": { " + color(pack.player) + ": " +
			resourceMap(pack.res, pack.mode != ChangeValueMode::ABSOLUTE) + " }";
	}

	void visitSetPrimarySkill(SetPrimarySkill & pack) override
	{
		const std::string key = pack.mode == ChangeValueMode::ABSOLUTE ? "setSkills" : "skills";
		line = key + ": { " + heroAlias(gameState, pack.id) + ": { " + primarySkill(pack.which) + ": " +
			(pack.mode == ChangeValueMode::ABSOLUTE ? std::to_string(pack.val) : signedInteger(pack.val)) + " } }";
	}

	void visitSetHeroExperience(SetHeroExperience & pack) override
	{
		if(pack.mode == ChangeValueMode::RELATIVE && pack.val == 0)
		{
			line.clear();
			return;
		}
		const std::string key = pack.mode == ChangeValueMode::ABSOLUTE ? "setExperience" : "experience";
		line = key + ": { " + heroAlias(gameState, pack.id) + ": " +
			(pack.mode == ChangeValueMode::ABSOLUTE ? std::to_string(pack.val) : signedInteger(pack.val)) + " }";
	}

	void visitGiveStackExperience(GiveStackExperience & pack) override
	{
		line = "stackExperience: { army: " + objectAlias(gameState, pack.id) +
			", values: " + stackExperienceValues(pack.val) + " }";
	}

	void visitSetSecSkill(SetSecSkill & pack) override
	{
		const std::string key = pack.mode == ChangeValueMode::ABSOLUTE ? "setSkills" : "skills";
		line = key + ": { " + heroAlias(gameState, pack.id) + ": { " + secondarySkill(pack.which) + ": " +
			(pack.mode == ChangeValueMode::ABSOLUTE ? std::to_string(pack.val) : signedInteger(pack.val)) + " } }";
	}

	void visitHeroVisitCastle(HeroVisitCastle & pack) override
	{
		line.clear();
	}

	void visitChangeSpells(ChangeSpells & pack) override
	{
		line = "spells: { hero: " + heroAlias(gameState, pack.hid) +
			", mode: " + std::string(pack.learn ? "learn" : "forget") +
			", spells: " + spellsList(pack.spells) + " }";
	}

	void visitSetMana(SetMana & pack) override
	{
		const std::string key = pack.mode == ChangeValueMode::ABSOLUTE ? "setMana" : "mana";
		line = key + ": { " + heroAlias(gameState, pack.hid) + ": " +
			(pack.mode == ChangeValueMode::ABSOLUTE ? std::to_string(pack.val) : signedInteger(pack.val)) + " }";
	}

	void visitSetMovePoints(SetMovePoints & pack) override
	{
		line = "setMovement: { " + heroAlias(gameState, pack.hid) + ": " + std::to_string(pack.val) + " }";
	}

	void visitFoWChange(FoWChange & pack) override
	{
		line = "visibility: { player: " + color(pack.player) +
			", mode: " + visibility(pack.mode) +
			", runs: " + fowTiles(pack.tiles) + " }";
	}

	void visitChangeObjPos(ChangeObjPos & pack) override
	{
		line = "objectPosition: { object: " + objectAlias(gameState, pack.objid) +
			", to: " + pos(pack.nPos) +
			", initiator: " + color(pack.initiator) + " }";
	}

	void visitRemoveObject(RemoveObject & pack) override
	{
		line = "remove: { object: " + objectAlias(gameState, pack.objectID) +
			", initiator: " + color(pack.initiator) + " }";
	}

	void visitTryMoveHero(TryMoveHero & pack) override
	{
		line = "move: { hero: " + heroAlias(gameState, pack.id) +
			", from: " + pos(pack.start) +
			", to: " + pos(pack.end) +
			", result: " + movementResult(pack.result) +
			", movement: " + std::to_string(pack.movePoints);
		if(!pack.fowRevealed.empty())
		{
			if(const auto * hero = gameState.getHero(pack.id))
				line += ", revealed: " + fowCircle(hero->getSightCenter() + (pack.end - pack.start), hero->getSightRadius());
			else
				line += ", revealed: " + fowTiles(pack.fowRevealed);
		}
		if(pack.result == TryMoveHero::BLOCKING_VISIT && pack.attackedFrom.isValid() && pack.attackedFrom != int3())
			line += ", attackedFrom: " + pos(pack.attackedFrom);
		line += " }";
	}

	void visitNewStructures(NewStructures & pack) override
	{
		std::vector<std::string> buildings;
		for(const auto & entry : pack.bid)
			buildings.push_back(building(entry));
		line = "town: { id: " + objectAlias(gameState, pack.tid) +
			", build: " + flowList(buildings) +
			", builtThisTurn: " + std::to_string(pack.built) + " }";
	}

	void visitRazeStructures(RazeStructures & pack) override
	{
		std::vector<std::string> buildings;
		for(const auto & entry : pack.bid)
			buildings.push_back(building(entry));
		line = "town: { id: " + objectAlias(gameState, pack.tid) +
			", raze: " + flowList(buildings) +
			", destroyed: " + std::to_string(pack.destroyed) + " }";
	}

	void visitSetAvailableCreatures(SetAvailableCreatures & pack) override
	{
		line = "available: { " + objectAlias(gameState, pack.tid) + ": " + sparseCreaturePools(pack.creatures) + " }";
	}

	void visitSetHeroesInTown(SetHeroesInTown & pack) override
	{
		line = "townHeroes: { town: " + objectAlias(gameState, pack.tid) +
			", visiting: " + objectAlias(gameState, pack.visiting) +
			", garrison: " + objectAlias(gameState, pack.garrison) + " }";
	}

	void visitSetAvailableHero(SetAvailableHero & pack) override
	{
		line = "availableHero: { player: " + color(pack.player) +
			", slot: " + tavernSlot(pack.slotID) +
			", role: " + tavernRole(pack.roleID) +
			", hero: " + heroType(pack.hid) +
			", army: " + simpleArmy(pack.army) +
			", replenishMovement: " + std::string(pack.replenishPoints ? "true" : "false") + " }";
	}

	void visitHeroRecruited(HeroRecruited & pack) override
	{
		line = "heroRecruited: { player: " + color(pack.player) +
			", hero: " + heroType(pack.hid) +
			", town: " + objectAlias(gameState, pack.tid) +
			", tile: " + pos(pack.tile) +
			", boat: " + objectAlias(gameState, pack.boatId) + " }";
	}

	void visitGiveHero(GiveHero & pack) override
	{
		line = "heroOwner: { hero: " + heroAlias(gameState, pack.id) +
			", player: " + color(pack.player);
		if(pack.boatId != ObjectInstanceID::NONE)
			line += ", boat: " + objectAlias(gameState, pack.boatId);
		line += " }";
	}

	void visitAddQuest(AddQuest & pack) override
	{
		line = "quest: { player: " + color(pack.player) +
			", object: " + objectAlias(gameState, pack.quest.obj) + " }";
	}

	void visitGiveBonus(GiveBonus & pack) override
	{
		if(pack.who == GiveBonus::ETarget::OBJECT && pack.bonus.type == BonusType::NONE &&
			pack.bonus.val == 0 && transcriptIdentifier(pack.bonus.sid.toString()) == "magicWell")
		{
			line = "usedToday: { hero: " + objectAlias(gameState, pack.id.as<ObjectInstanceID>()) +
				", object: magicWell }";
			return;
		}
		line = "bonus: " + semanticBonus(gameState, pack);
	}

	void visitNewObject(NewObject & pack) override
	{
		const auto & object = pack.newObject;
		if(!object)
		{
			line = "appears: { object: null }";
			return;
		}

		const int objectID = object->id.hasValue()
			? object->id.getNum()
			: static_cast<int>(gameState.getMap().getObjects().size());
		std::string type = transcriptIdentifier(MapObjectID::encode(object->ID.getNum()));
		std::string name = sanitizedAliasName(object->getObjectName());
		if(name.empty())
			name = sanitizedAliasName(object->instanceName);
		if(name.empty())
			name = "id-" + std::to_string(objectID);
		const auto typeName = type.substr(type.rfind('/') == std::string::npos ? 0 : type.rfind('/') + 1);
		std::string alias = equivalentAliasWords(name, typeName) ? name : type + "/" + name;
		alias += objectLocation(object->visitablePos());

		line = "appears: { object: " + alias + ", at: " + pos(object->visitablePos());
		if(object->tempOwner.isValidPlayer())
			line += ", owner: " + color(object->tempOwner);
		if(const auto armed = std::dynamic_pointer_cast<CArmedInstance>(object))
			line += ", units: " + armyState(*armed);
		line += " }";
	}

	void visitSetAvailableArtifacts(SetAvailableArtifacts & pack) override
	{
		line = "availableArtifacts: { object: " + objectAlias(gameState, pack.id) +
			", artifacts: " + artifactsList(pack.arts) + " }";
	}

	void visitNewArtifact(NewArtifact & pack) override
	{
		std::vector<std::string> fields;
		fields.push_back("holder: " + objectAlias(gameState, pack.artHolder));
		fields.push_back("artifact: " + artifact(pack.artId));
		if(pack.spellId != SpellID::NONE)
			fields.push_back("spell: " + spell(pack.spellId));
		fields.push_back("position: " + artifactPosition(pack.pos));
		line = "artifact: { create: { " + boost::algorithm::join(fields, ", ") + " } }";
	}

	void visitPutArtifact(PutArtifact & pack) override
	{
		std::vector<std::string> fields;
		fields.push_back("artifactInstance: " + std::to_string(pack.id.getNum()));
		fields.push_back("to: " + artifactLocation(gameState, pack.al));
		if(pack.askAssemble)
			fields.push_back("askAssemble: true");
		line = "artifact: { put: { " + boost::algorithm::join(fields, ", ") + " } }";
	}

	void visitBulkEraseArtifacts(BulkEraseArtifacts & pack) override
	{
		std::vector<std::string> fields;
		fields.push_back("holder: " + objectAlias(gameState, pack.artHolder));
		if(pack.creature)
			fields.push_back("creatureSlot: " + slot(*pack.creature));
		fields.push_back("positions: " + artifactPositions(pack.posPack));
		line = "artifacts: { erase: { " + boost::algorithm::join(fields, ", ") + " } }";
	}

	void visitChangeStackCount(ChangeStackCount & pack) override
	{
		line = "army: { owner: " + objectAlias(gameState, pack.army) +
			", slot: " + std::to_string(pack.slot.getNum()) +
			", mode: " + mode(pack.mode) +
			", count: " + std::to_string(pack.count) + " }";
	}

	void visitSetStackType(SetStackType & pack) override
	{
		line = "army: { owner: " + objectAlias(gameState, pack.army) +
			", slot: " + std::to_string(pack.slot.getNum()) +
			", creature: " + creature(pack.type) + " }";
	}

	void visitEraseStack(EraseStack & pack) override
	{
		line = "army: { owner: " + objectAlias(gameState, pack.army) +
			", slot: " + std::to_string(pack.slot.getNum()) +
			", erase: true }";
	}

	void visitSwapStacks(SwapStacks & pack) override
	{
		line = "army: { swap: { from: { owner: " + objectAlias(gameState, pack.srcArmy) +
			", slot: " + slot(pack.srcSlot) +
			" }, to: { owner: " + objectAlias(gameState, pack.dstArmy) +
			", slot: " + slot(pack.dstSlot) + " } } }";
	}

	void visitInsertNewStack(InsertNewStack & pack) override
	{
		line = "army: { owner: " + objectAlias(gameState, pack.army) +
			", slot: " + slot(pack.slot) +
			", insert: { creature: " + creature(pack.type) +
			", count: " + std::to_string(pack.count) + " } }";
	}

	void visitRebalanceStacks(RebalanceStacks & pack) override
	{
		line = "army: { move: { from: { owner: " + objectAlias(gameState, pack.srcArmy) +
			", slot: " + slot(pack.srcSlot) +
			" }, to: { owner: " + objectAlias(gameState, pack.dstArmy) +
			", slot: " + slot(pack.dstSlot) +
			" }, count: " + std::to_string(pack.count) + " } }";
	}

	void visitBulkMoveArtifacts(BulkMoveArtifacts & pack) override
	{
		line = "artifacts: " + bulkArtifactMove(gameState, pack);
	}

	void visitHeroVisit(HeroVisit & pack) override
	{
		if(!pack.starting)
		{
			line.clear();
			return;
		}
		line = "visit: { hero: " + heroAlias(gameState, pack.heroId) +
			", object: " + objectAlias(gameState, pack.objId) + " }";
	}

	void visitNewTurn(NewTurn & pack) override
	{
		std::vector<std::string> records;
		std::map<std::string, int64_t> movementRefreshes;
		std::map<std::string, int64_t> manaRefreshes;
		for(const auto & movement : pack.heroesMovement)
			movementRefreshes[heroAlias(gameState, movement.hid)] = movement.val;
		for(const auto & mana : pack.heroesMana)
			manaRefreshes[heroAlias(gameState, mana.hid)] = mana.val;
		records.push_back("dayStart: { movement: " + heroRefreshMap(movementRefreshes) +
			", mana: " + heroRefreshMap(manaRefreshes) + " }");

		if(pack.specialWeek != EWeekType::NORMAL || pack.creatureid != CreatureID::NONE)
		{
			std::string week = "week: { type: " + weekType(pack.specialWeek);
			if(pack.creatureid != CreatureID::NONE)
				week += ", creature: " + creature(pack.creatureid);
			week += " }";
			records.push_back(week);
		}

		line = boost::algorithm::join(records, "\n  - ");
	}

	void visitSetObjectProperty(SetObjectProperty & pack) override
	{
		if(pack.what == ObjProperty::OWNER)
		{
			line = "capture: { object: " + objectAlias(gameState, pack.id) +
				", owner: " + color(pack.identifier.as<PlayerColor>()) + " }";
			return;
		}
		line.clear();
	}

	void visitSetRewardableConfiguration(SetRewardableConfiguration & pack) override
	{
		ResourceSet reward;
		std::optional<std::string> narrative;
		for(const auto & info : pack.configuration.info)
		{
			if(info.visitType != Rewardable::EEventType::EVENT_FIRST_VISIT)
				continue;
			reward += info.reward.resources;
			if(info.message.hasCustomText())
				narrative = info.message.toString();
		}
		line = "refresh: { object: " + objectAlias(gameState, pack.objectID);
		if(!reward.empty())
			line += ", reward: " + resourceMap(reward);
		if(narrative)
			line += ", text: " + yamlString(*narrative);
		line += " }";
	}

	void visitChangeObjectVisitors(ChangeObjectVisitors & pack) override
	{
		line.clear();
	}

	void visitHeroLevelUp(HeroLevelUp & pack) override
	{
		std::vector<std::string> skills;
		for(const auto & skill : pack.skills)
			skills.push_back(secondarySkill(skill));

		line = "levelUp: { player: " + color(pack.player) +
			", hero: " + heroAlias(gameState, pack.heroId) +
			", primary: " + primarySkill(pack.primskill) +
			", choices: " + flowList(skills) + " }";
	}

	void visitInfoWindow(InfoWindow & pack) override
	{
		line.clear();
	}

	void visitBattleStart(BattleStart & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) + ", event: start }";
	}

	void visitBattleNextRound(BattleNextRound & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) + ", event: nextRound }";
	}

	void visitBattleSetActiveStack(BattleSetActiveStack &) override
	{
		line.clear();
	}

	void visitBattleResult(BattleResult & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", event: result, result: " + battleResult(pack.result) +
			", winner: " + battleSide(pack.winner) +
			", attacker: " + color(pack.attacker) + " }";
	}

	void visitBattleResultAccepted(BattleResultAccepted & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", event: resultAccepted, winner: " + battleSide(pack.winnerSide) +
			", attacker: " + battleHeroResult(gameState, pack.heroResult[BattleSide::ATTACKER]) +
			", defender: " + battleHeroResult(gameState, pack.heroResult[BattleSide::DEFENDER]) + " }";
	}

	void visitBattleResultsApplied(BattleResultsApplied & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", event: resultsApplied, victor: " + color(pack.victor) +
			", loser: " + color(pack.loser) +
			", artifactMoves: " + bulkArtifactMoves(gameState, pack.movingArtifacts) +
			", grownArtifacts: " + std::to_string(pack.growingArtifacts.size()) +
			", dischargedArtifacts: " + std::to_string(pack.dischargingArtifacts.size()) + " }";
	}

	void visitBattleStackMoved(BattleStackMoved & pack) override
	{
		std::vector<std::string> tiles;
		for(const auto & tile : pack.tilesToMove)
			tiles.push_back(std::to_string(tile.toInt()));
		std::vector<std::string> fields;
		fields.push_back("id: " + battleAlias(pack.battleID));
		fields.push_back("event: move");
		fields.push_back("unit: " + yamlIdentifier(battleUnitAlias(gameState, pack.battleID, pack.stack)));
		fields.push_back("path: " + flowList(tiles));
		fields.push_back("distance: " + std::to_string(pack.distance));
		if(pack.teleporting)
			fields.push_back("teleporting: true");
		line = "battle: { " + boost::algorithm::join(fields, ", ") + " }";
	}

	void visitBattleUnitsChanged(BattleUnitsChanged & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", event: unitsChanged, changes: " + battleUnitChanges(gameState, pack.battleID, pack.changedStacks) + " }";
	}

	void visitBattleAttack(BattleAttack & pack) override
	{
		if(pack.bsa.size() == 1)
		{
			const auto & result = pack.bsa.front();
			std::vector<std::string> fields;
			fields.push_back("by: " + yamlIdentifier(battleUnitAlias(gameState, pack.battleID, pack.stackAttacking)));
			fields.push_back("target: " + yamlIdentifier(battleUnitAlias(gameState, pack.battleID, result.stackAttacked)));
			fields.push_back("damage: " + std::to_string(result.damageAmount));
			if(result.killedAmount != 0)
				fields.push_back("killed: " + std::to_string(result.killedAmount));
			if(const auto left = battleLeft(result.newState); !left.empty())
				fields.push_back("left: " + left);
			if(result.willRebirth())
			{
				if(const auto left = battleLeft(result.newState); !left.empty())
					fields.push_back("reborn: " + left);
			}
			if(pack.counter()) fields.push_back("retaliation: true");
			if(pack.shot()) fields.push_back("ranged: true");
			if(pack.lucky()) fields.push_back("luck: good");
			if(pack.unlucky()) fields.push_back("luck: bad");
			if(pack.deathBlow()) fields.push_back("deathBlow: true");
			if(pack.spellLike()) fields.push_back("spellLike: true");
			if(pack.lifeDrain()) fields.push_back("lifeDrain: true");
			line = "battle: { id: " + battleAlias(pack.battleID) +
				", attack: { " + boost::algorithm::join(fields, ", ") + " } }";
			return;
		}
		std::vector<std::string> fields;
		fields.push_back("by: " + yamlIdentifier(battleUnitAlias(gameState, pack.battleID, pack.stackAttacking)));
		fields.push_back("hits: " + battleAttackHits(gameState, pack.battleID, pack.bsa));
		if(pack.counter()) fields.push_back("retaliation: true");
		if(pack.shot()) fields.push_back("ranged: true");
		if(pack.lucky()) fields.push_back("luck: good");
		if(pack.unlucky()) fields.push_back("luck: bad");
		if(pack.deathBlow()) fields.push_back("deathBlow: true");
		if(pack.spellLike()) fields.push_back("spellLike: true");
		if(pack.lifeDrain()) fields.push_back("lifeDrain: true");
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", attack: { " + boost::algorithm::join(fields, ", ") + " } }";
	}

	void visitStartAction(StartAction & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", event: startAction, action: " + battleAction(gameState, pack.battleID, pack.ba) + " }";
	}

	void visitEndAction(EndAction &) override
	{
		line.clear();
	}

	void visitBattleSpellCast(BattleSpellCast & pack) override
	{
		std::string caster = battleUnitAlias(gameState, pack.battleID, pack.casterStack);
		if(caster.starts_with("unit-"))
			caster = "spell";
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", event: spellCast, side: " + battleSide(pack.side) +
			", spell: " + spell(pack.spellID) +
			", at: " + std::to_string(pack.tile.toInt()) +
			", caster: " + yamlIdentifier(caster) +
			", hero: " + std::string(pack.castByHero ? "true" : "false") + " }";
	}

	void visitSetStackEffect(SetStackEffect & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", event: stackEffects, add: " + std::to_string(pack.toAdd.size()) +
			", update: " + std::to_string(pack.toUpdate.size()) +
			", remove: " + std::to_string(pack.toRemove.size()) + " }";
	}

	void visitBattleObstaclesChanged(BattleObstaclesChanged & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", event: obstaclesChanged, changes: " + std::to_string(pack.changes.size()) + " }";
	}

	void visitCatapultAttack(CatapultAttack & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", catapult: { part: " + std::to_string(static_cast<int>(pack.attackedPart)) +
			", tile: " + std::to_string(pack.destinationTile) +
			", damage: " + std::to_string(pack.damageDealt);
		if(pack.killedTowerShooter >= 0)
			line += ", killedTowerShooter: " + yamlIdentifier(battleUnitAlias(gameState, pack.battleID, pack.killedTowerShooter));
		line += ", attacker: " + (pack.attacker < 0 ? "spell" : yamlIdentifier(battleUnitAlias(gameState, pack.battleID, pack.attacker))) + " } }";
	}

	void visitBattleTriggerEffect(BattleTriggerEffect & pack) override
	{
		std::string kind;
		switch(pack.effect)
		{
			case BonusType::HP_REGENERATION: kind = "regenerate"; break;
			case BonusType::MANA_DRAIN: kind = "drainMana"; break;
			case BonusType::POISON: kind = "poison"; break;
			case BonusType::ENCHANTER: kind = "enchant"; break;
			case BonusType::MORALE: kind = "morale"; break;
			case BonusType::FEARFUL: kind = "fear"; break;
			default: kind = "triggeredEffect"; break;
		}
		line = "battle: { id: " + battleAlias(pack.battleID) + ", " + kind + ": { unit: " +
			yamlIdentifier(battleUnitAlias(gameState, pack.battleID, pack.stackID));
		if(pack.val != 0)
			line += ", value: " + std::to_string(pack.val);
		if(pack.effect == BonusType::MANA_DRAIN && pack.additionalInfo >= 0)
			line += ", hero: " + heroAlias(gameState, ObjectInstanceID(pack.additionalInfo));
		else if(pack.additionalInfo != 0)
			line += ", info: " + std::to_string(pack.additionalInfo);
		if(kind == "triggeredEffect")
			line += ", type: " + std::to_string(static_cast<int>(pack.effect));
		line += " } }";
	}

	void visitBattleSetStackProperty(BattleSetStackProperty & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) + ", " + battleStackProperty(pack.which) +
			": { unit: " + yamlIdentifier(battleUnitAlias(gameState, pack.battleID, pack.stackID)) +
			", value: " + std::to_string(pack.val);
		if(pack.absolute)
			line += ", absolute: true";
		line += " } }";
	}

	void visitBattleUpdateGateState(BattleUpdateGateState & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", gate: { state: " + gateState(pack.state) + " } }";
	}

	void visitStacksInjured(StacksInjured & pack) override
	{
		if(pack.stacks.empty())
		{
			line.clear();
			return;
		}
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", event: injured, stacks: " + battleStackAttacks(gameState, pack.battleID, pack.stacks) + " }";
	}

	void visitBattleEnded(BattleEnded & pack) override
	{
		line = "battle: { id: " + battleAlias(pack.battleID) +
			", event: ended, victor: " + color(pack.victor) +
			", loser: " + color(pack.loser) + " }";
	}

	void visitAdvmapSpellCast(AdvmapSpellCast & pack) override
	{
		line = "adventureSpell: { caster: " + heroAlias(gameState, pack.casterID) +
			", spell: " + spell(pack.spellID) + " }";
	}

	void visitBlockingDialog(BlockingDialog &) override
	{
		line.clear();
	}

	void visitExchangeDialog(ExchangeDialog &) override
	{
		line.clear();
	}

	void visitOpenWindow(OpenWindow &) override
	{
		line.clear();
	}

	void visitGarrisonDialog(GarrisonDialog &) override
	{
		line.clear();
	}

	void visitTeleportDialog(TeleportDialog &) override
	{
		line.clear();
	}

	void visitBattleLogMessage(BattleLogMessage &) override
	{
		line.clear();
	}
};
}

VGTRecorder & VGTRecorder::get()
{
	static VGTRecorder recorder;
	return recorder;
}

bool VGTRecorder::isEnabled()
{
	std::scoped_lock lock(outputMutex);
	initializeFromEnvironment();
	return enabled;
}

void VGTRecorder::setRandomSeed(int seed)
{
	std::scoped_lock lock(outputMutex);
	randomSeed = seed;
}

void VGTRecorder::initializeFromEnvironment()
{
	if(checkedEnvironment)
		return;
	checkedEnvironment = true;

	const char * path = std::getenv("VCMI_VGT_TEXT");
	if(path && !std::string(path).empty())
	{
		outputPath = path;
		const boost::filesystem::path transcriptPath(outputPath);
		if(!transcriptPath.parent_path().empty())
			boost::filesystem::create_directories(transcriptPath.parent_path());

		output.open(outputPath, std::ios::out | std::ios::trunc);
		if(output)
		{
			battleOutputPath = outputPath + ".battles.yaml";
			battleOutput.open(battleOutputPath, std::ios::out | std::ios::trunc);
			if(battleOutput)
			{
				battleOutput << "vgtBattles: 4\n";
				battleOutput << "battles:\n";
				enabled = true;
			}
			else
				logGlobal->error("Unable to open VGT battle companion '%s'", battleOutputPath);
		}
		else
			logGlobal->error("Unable to open VGT transcript '%s'", outputPath);
	}

	const char * baselineSave = std::getenv("VCMI_VGT_BASELINE_SAVE");
	if(baselineSave && !std::string(baselineSave).empty())
	{
		baselineSavePath = baselineSave;
		const boost::filesystem::path savePath(baselineSavePath);
		if(!savePath.parent_path().empty())
			boost::filesystem::create_directories(savePath.parent_path());
		baselineSaveEnabled = true;
	}

	const char * baselineGameStateSave = std::getenv("VCMI_VGT_BASELINE_GAMESTATE_SAVE");
	if(baselineGameStateSave && !std::string(baselineGameStateSave).empty())
	{
		baselineGameStateSavePath = baselineGameStateSave;
		const boost::filesystem::path savePath(baselineGameStateSavePath);
		if(!savePath.parent_path().empty())
			boost::filesystem::create_directories(savePath.parent_path());
		baselineGameStateSaveEnabled = true;
	}

	const char * turnStateDirectoryValue = std::getenv("VCMI_VGT_TURN_STATE_DIR");
	if(turnStateDirectoryValue && !std::string(turnStateDirectoryValue).empty())
	{
		turnStateDirectory = turnStateDirectoryValue;
		boost::filesystem::create_directories(turnStateDirectory);
		turnStateArchiveEnabled = true;
	}

	const char * exitAfterTurnEndsValue = std::getenv("VCMI_VGT_EXIT_AFTER_TURN_ENDS");
	if(exitAfterTurnEndsValue && !std::string(exitAfterTurnEndsValue).empty())
	{
		char * parseEnd = nullptr;
		const long parsedValue = std::strtol(exitAfterTurnEndsValue, &parseEnd, 10);
		if(parseEnd != exitAfterTurnEndsValue && *parseEnd == '\0' && parsedValue > 0 && parsedValue <= std::numeric_limits<int>::max())
			exitAfterTurnEnds = static_cast<int>(parsedValue);
		else
			logGlobal->error("Ignoring invalid VGT turn-end exit limit '%s'", exitAfterTurnEndsValue);
	}
}

void VGTRecorder::ensureHeader(const CGameState & gameState)
{
	if(headerWritten)
		return;

	const auto * startInfo = gameState.getStartInfo();
	if(!startInfo)
	{
		logGlobal->error("Unable to write VGT transcript '%s': missing game start information", outputPath);
		enabled = false;
		return;
	}
	const auto * initialStartInfo = gameState.getInitialStartInfo();
	if(!initialStartInfo)
	{
		logGlobal->error("Unable to write VGT transcript '%s': missing initial game settings", outputPath);
		enabled = false;
		return;
	}
	if(!randomSeed)
	{
		logGlobal->error("Unable to write VGT transcript '%s': missing random seed", outputPath);
		enabled = false;
		return;
	}

	const auto hash = mapHash(*startInfo);
	if(!hash)
	{
		logGlobal->error("Unable to write VGT transcript '%s': unable to hash map '%s'", outputPath, startInfo->fileURI);
		enabled = false;
		return;
	}

	output << "vgt: 4\n";
	output << "format: " << yamlString("VCMI readable event transcript") << "\n";
	output << "engine: { version: " << yamlString(GameConstants::VCMI_VERSION) << " }\n";
	output << "companions: { battles: " << yamlString(boost::filesystem::path(battleOutputPath).filename().string()) << " }\n";
	output << "map:\n";
	output << "  uri: " << yamlString(startInfo->fileURI) << "\n";
	if(startInfo->mapname != startInfo->fileURI)
		output << "  name: " << yamlString(startInfo->mapname) << "\n";
	output << "  textEncoding: " << mapTextEncoding(*startInfo) << "\n";
	if(startInfo->mapGenOptions)
		output << "  source: generated-map-file\n";
	output << "  hash: { algorithm: sha256, value: " << yamlString(*hash) << " }\n";
	output << "  objectNameCounter: " << gameState.getMap().getUniqueInstanceNameCounter() << "\n";
	if(startInfo->mapGenOptions)
		output << "  generator: " << randomMapGenerator(*startInfo->mapGenOptions) << "\n";
	if(initialStartInfo->mapGenOptions)
		output << "  initialGenerator: " << randomMapGenerator(*initialStartInfo->mapGenOptions) << "\n";
	output << "settings:\n";
	output << "  start: " << startMode(startInfo->mode) << "\n";
	output << "  startTime: " << static_cast<int64_t>(startInfo->startTime) << "\n";
	output << "  difficulty: " << difficulty(startInfo->difficulty) << "\n";
	output << "  randomSeed: " << *randomSeed << "\n";
	if(startInfo->simturnsInfo != SimturnsInfo())
		output << "  simturns: " << simturnsInfo(startInfo->simturnsInfo) << "\n";
	if(startInfo->turnTimerInfo.isEnabled())
		output << "  timer: " << timerInfo(startInfo->turnTimerInfo) << "\n";
	if(!startInfo->extraOptionsInfo.cheatsAllowed || startInfo->extraOptionsInfo.unlimitedReplay)
		output << "  extraOptions: " << extraOptions(startInfo->extraOptionsInfo) << "\n";
	if(!gameState.getMap().getGameSettingsOverrides().isNull() &&
		!gameState.getMap().getGameSettingsOverrides().Struct().empty())
		output << "  gameSettingsOverrides: " << gameSettingsOverrides(gameState.getMap().getGameSettingsOverrides()) << "\n";
	output << "players:\n";
	for(const auto & player : startInfo->playerInfos)
		output << "  " << color(player.first) << ": " << playerSettings(player.second) << "\n";
	std::vector<std::string> initialPlayers;
	for(const auto & player : initialStartInfo->playerInfos)
	{
		const auto resolved = startInfo->playerInfos.find(player.first);
		const std::string delta = playerSettings(player.second,
			resolved == startInfo->playerInfos.end() ? nullptr : &resolved->second);
		if(delta != "{  }")
			initialPlayers.push_back(color(player.first) + ": " + delta);
	}
	if(initialPlayers.empty())
		output << "initialPlayers: {}\n";
	else
	{
		output << "initialPlayers:\n";
		for(const auto & player : initialPlayers)
			output << "  " << player << "\n";
	}
	writeInitialState(output, gameState);
	headerWritten = true;
	output.flush();
}

void VGTRecorder::startTurnDocument(const CGameState & gameState, PlayerColor player)
{
	ensureHeader(gameState);
	if(!enabled)
		return;

	flushPendingHeroScene();
	flushPendingArtifactTransfer(gameState);
	flushPendingArmyArrangement(gameState);
	flushPendingBattle(gameState);
	flushPendingWeeklyWorldEvents(gameState);
	const auto calendar = gameState.getCalendar();
	output << "---\n";
	output << "turn:\n";
	output << "  date: " << calendarDate(calendar) << "\n";
	output << "  player: " << color(player) << "\n";
	if(const auto * state = gameState.getPlayerState(player))
	{
		output << "  resources: " << resourceMap(state->resources) << "\n";
		std::vector<std::string> heroes;
		for(const auto * hero : state->getHeroes())
		{
			if(!hero)
				continue;
			std::string name = heroAlias(gameState, hero->id);
			const std::string ownerPrefix = color(player) + "/";
			if(name.starts_with(ownerPrefix))
				name.erase(0, ownerPrefix.size());
			heroes.push_back(yamlKey(name) + ": { movement: " +
				std::to_string(hero->movementPointsRemaining()) + ", mana: " +
				std::to_string(hero->mana) + " }");
		}
		if(!heroes.empty())
		{
			const std::string values = boost::algorithm::join(heroes, ", ");
			if(values.size() <= 120)
				output << "  heroes: { " << values << " }\n";
			else
			{
				output << "  heroes:\n";
				for(const auto & hero : heroes)
					output << "    " << hero << "\n";
			}
		}
	}
	output << "actions:\n";
	documentOpen = true;
	currentTurnPlayer = player;
}

void VGTRecorder::startWorldDocument(const CGameState & gameState, const std::string & phase)
{
	ensureHeader(gameState);
	if(!enabled)
		return;

	flushPendingHeroScene();
	flushPendingArtifactTransfer(gameState);
	flushPendingArmyArrangement(gameState);
	flushPendingBattle(gameState);
	const auto calendar = phase == "newDay" ? gameState.getCalendar().nextDay() : gameState.getCalendar();
	output << "---\n";
	output << "world: { date: " << calendarDate(calendar) << ", phase: " << phase << " }\n";
	output << "events:\n";
	documentOpen = true;
	currentTurnPlayer.reset();
}

namespace
{
std::vector<std::string> splitFlowFields(std::string_view value)
{
	std::vector<std::string> result;
	size_t start = 0;
	int braces = 0;
	int brackets = 0;
	bool quoted = false;
	bool escaped = false;
	for(size_t index = 0; index < value.size(); ++index)
	{
		const char character = value[index];
		if(quoted)
		{
			if(escaped)
				escaped = false;
			else if(character == '\\')
				escaped = true;
			else if(character == '"')
				quoted = false;
			continue;
		}
		if(character == '"')
		{
			quoted = true;
			continue;
		}
		if(character == '{')
			++braces;
		else if(character == '}')
			--braces;
		else if(character == '[')
			++brackets;
		else if(character == ']')
			--brackets;
		else if(character == ',' && braces == 0 && brackets == 0)
		{
			std::string field(value.substr(start, index - start));
			boost::algorithm::trim(field);
			if(!field.empty())
				result.push_back(std::move(field));
			start = index + 1;
		}
	}
	std::string field(value.substr(start));
	boost::algorithm::trim(field);
	if(!field.empty())
		result.push_back(std::move(field));
	return result;
}

std::optional<std::string_view> flowMappingBody(std::string_view value)
{
	if(value.size() < 4 || !value.starts_with("{ ") || !value.ends_with(" }"))
		return std::nullopt;
	return value.substr(2, value.size() - 4);
}

std::optional<std::vector<std::string>> keyedFlowFields(const std::string & record, const std::string & key)
{
	const std::string prefix = key + ": ";
	if(!record.starts_with(prefix))
		return std::nullopt;
	const auto body = flowMappingBody(std::string_view(record).substr(prefix.size()));
	if(!body)
		return std::nullopt;
	return splitFlowFields(*body);
}

std::optional<std::vector<std::string>> wrappedFlowFields(const std::string & record)
{
	const auto body = flowMappingBody(record);
	if(!body)
		return std::nullopt;
	return splitFlowFields(*body);
}

std::optional<std::string> flowField(const std::vector<std::string> & fields, const std::string & key)
{
	const std::string prefix = key + ": ";
	for(const auto & field : fields)
	{
		if(field.starts_with(prefix))
			return field.substr(prefix.size());
	}
	return std::nullopt;
}

bool flowBool(const std::vector<std::string> & fields, const std::string & key)
{
	return flowField(fields, key) == "true";
}

std::optional<std::vector<std::string>> nestedFlowFields(
	const std::vector<std::string> & fields,
	const std::string & key)
{
	const auto value = flowField(fields, key);
	if(!value)
		return std::nullopt;
	const auto body = flowMappingBody(*value);
	if(!body)
		return std::nullopt;
	return splitFlowFields(*body);
}

void writeYamlRecord(std::ostream & stream, const std::string & indent, const std::string & record)
{
	static constexpr size_t FLOW_LINE_LIMIT = 160;
	const std::string recordSeparator = "\n  - ";
	if(const auto separator = record.find(recordSeparator); separator != std::string::npos)
	{
		writeYamlRecord(stream, indent, record.substr(0, separator));
		writeYamlRecord(stream, indent, record.substr(separator + recordSeparator.size()));
		return;
	}
	if(record.size() <= FLOW_LINE_LIMIT)
	{
		stream << indent << "- " << record << "\n";
		return;
	}

	// Battle packets are stored as wrapped flow mappings until the complete
	// battle can be rendered.  A long singleton such as
	// `{ bonus: { ... } }` must lose that staging wrapper before it is expanded
	// into block YAML; otherwise the opening brace becomes part of the key and
	// the resulting document is invalid.
	if(const auto wrapped = flowMappingBody(record))
	{
		const auto fields = splitFlowFields(*wrapped);
		if(fields.size() == 1)
		{
			writeYamlRecord(stream, indent, fields.front());
			return;
		}

		// A multi-field staging wrapper cannot be removed without changing the
		// mapping's shape. Keep it as a valid (if long) flow mapping.
		stream << indent << "- " << record << "\n";
		return;
	}

	const auto separator = record.find(": ");
	if(separator == std::string::npos)
	{
		stream << indent << "- " << record << "\n";
		return;
	}
	const auto body = flowMappingBody(std::string_view(record).substr(separator + 2));
	if(!body)
	{
		stream << indent << "- " << record << "\n";
		return;
	}

	stream << indent << "- " << record.substr(0, separator) << ":\n";
	for(const auto & field : splitFlowFields(*body))
	{
		const auto fieldSeparator = field.find(": ");
		if(fieldSeparator != std::string::npos)
		{
			const std::string value = field.substr(fieldSeparator + 2);
			if(field.size() > FLOW_LINE_LIMIT)
			{
				if(const auto nested = flowMappingBody(value))
				{
					stream << indent << "    " << field.substr(0, fieldSeparator) << ":\n";
					for(const auto & nestedField : splitFlowFields(*nested))
						stream << indent << "      " << nestedField << "\n";
					continue;
				}
			}
			if(value.starts_with("[{ ") && value.ends_with(" }]"))
			{
				stream << indent << "    " << field.substr(0, fieldSeparator) << ":\n";
				for(const auto & item : splitFlowFields(std::string_view(value).substr(1, value.size() - 2)))
					stream << indent << "      - " << item << "\n";
				continue;
			}
		}
		stream << indent << "    " << field << "\n";
	}
}

bool isBattlePacketEvent(const std::string & record, const std::string & event)
{
	const auto fields = wrappedFlowFields(record);
	return fields && flowField(*fields, "event") == event;
}

std::vector<int> battleTargetHexes(const std::string & targets)
{
	std::vector<int> result;
	const std::string marker = "hex: ";
	for(size_t start = targets.find(marker); start != std::string::npos; start = targets.find(marker, start))
	{
		start += marker.size();
		size_t end = start;
		if(end < targets.size() && targets[end] == '-')
			++end;
		while(end < targets.size() && std::isdigit(static_cast<unsigned char>(targets[end])))
			++end;
		if(end > start)
			result.push_back(std::stoi(targets.substr(start, end - start)));
	}
	return result;
}

std::vector<std::vector<std::string>> battleActionTargets(const std::string & targets)
{
	std::string value = targets;
	boost::algorithm::trim(value);
	if(value.size() < 2 || !value.starts_with('[') || !value.ends_with(']'))
		return {};

	std::vector<std::vector<std::string>> result;
	for(const auto & item : splitFlowFields(std::string_view(value).substr(1, value.size() - 2)))
	{
		if(const auto fields = wrappedFlowFields(item))
			result.push_back(*fields);
	}
	return result;
}

std::vector<int> flowIntegerList(const std::string & values)
{
	std::vector<int> result;
	for(size_t start = 0; start < values.size();)
	{
		while(start < values.size() && values[start] != '-' && !std::isdigit(static_cast<unsigned char>(values[start])))
			++start;
		if(start >= values.size())
			break;
		size_t end = start + (values[start] == '-' ? 1 : 0);
		while(end < values.size() && std::isdigit(static_cast<unsigned char>(values[end])))
			++end;
		if(end > start + (values[start] == '-' ? 1 : 0))
			result.push_back(std::stoi(values.substr(start, end - start)));
		start = end;
	}
	return result;
}

std::optional<std::string> battleTargetUnit(const std::string & targets)
{
	const std::string marker = "unit: ";
	const auto start = targets.find(marker);
	if(start == std::string::npos)
		return std::nullopt;
	const auto valueStart = start + marker.size();
	const auto valueEnd = targets.find_first_of(", }", valueStart);
	if(valueEnd == std::string::npos || valueEnd == valueStart)
		return std::nullopt;
	return targets.substr(valueStart, valueEnd - valueStart);
}

std::optional<std::vector<std::string>> appliedBattleAttack(const std::string & record)
{
	const auto outer = wrappedFlowFields(record);
	if(!outer || outer->size() != 1)
		return std::nullopt;
	return nestedFlowFields(*outer, "attack");
}

void appendBattleAttackOutcome(
	std::vector<std::string> & outputFields,
	const std::vector<std::vector<std::string>> & strikes)
{
	if(strikes.empty())
		return;

	if(const auto hits = strikes.size() == 1 ? flowField(strikes.front(), "hits") : std::nullopt)
		outputFields.push_back("hits: " + *hits);
	else
	{
		int damage = 0;
		int killed = 0;
		for(const auto & strike : strikes)
		{
			if(const auto value = flowField(strike, "damage"))
				damage += std::stoi(*value);
			if(const auto value = flowField(strike, "killed"))
				killed += std::stoi(*value);
		}
		if(strikes.size() > 1)
			outputFields.push_back("strikes: " + std::to_string(strikes.size()));
		outputFields.push_back("damage: " + std::to_string(damage));
		if(killed > 0)
			outputFields.push_back("killed: " + std::to_string(killed));
	}
	for(auto strike = strikes.rbegin(); strike != strikes.rend(); ++strike)
	{
		if(const auto after = flowField(*strike, "left"))
		{
			outputFields.push_back("after: " + *after);
			break;
		}
	}
	for(auto strike = strikes.rbegin(); strike != strikes.rend(); ++strike)
	{
		if(const auto reborn = flowField(*strike, "reborn"))
		{
			outputFields.push_back("reborn: " + *reborn);
			break;
		}
	}

	for(const std::string flag : {"luck", "deathBlow", "spellLike", "lifeDrain"})
	{
		for(const auto & strike : strikes)
		{
			if(const auto value = flowField(strike, flag))
			{
				outputFields.push_back(flag + ": " + *value);
				break;
			}
		}
	}
}

void appendBattleRetaliation(
	std::vector<std::string> & outputFields,
	const std::vector<std::string> & applied)
{
	if(!flowBool(applied, "retaliation"))
		return;

	std::vector<std::string> fields;
	if(const auto hits = flowField(applied, "hits"))
		fields.push_back("hits: " + *hits);
	else if(const auto damage = flowField(applied, "damage"))
		fields.push_back("damage: " + *damage);
	if(const auto killed = flowField(applied, "killed"); killed && *killed != "0")
		fields.push_back("killed: " + *killed);
	if(const auto after = flowField(applied, "left"))
		fields.push_back("after: " + *after);
	if(const auto reborn = flowField(applied, "reborn"))
		fields.push_back("reborn: " + *reborn);
	for(const std::string flag : {"luck", "deathBlow", "spellLike", "lifeDrain"})
	{
		if(const auto value = flowField(applied, flag))
			fields.push_back(flag + ": " + *value);
	}
	if(!fields.empty())
		outputFields.push_back("retaliation: { " + boost::algorithm::join(fields, ", ") + " }");
}

std::optional<std::vector<std::string>> battleInjurySummary(const std::string & record)
{
	const auto fields = wrappedFlowFields(record);
	if(!fields || flowField(*fields, "event") != "injured")
		return std::nullopt;
	const auto stacks = flowField(*fields, "stacks");
	if(!stacks || *stacks == "[]")
		return std::vector<std::string>{};

	std::vector<std::string> summary;
	const auto entriesStart = stacks->find("[{ ");
	if(entriesStart != std::string::npos)
	{
		const auto entriesEnd = stacks->find(" }]", entriesStart);
		if(entriesEnd != std::string::npos)
		{
			const auto injury = splitFlowFields(std::string_view(*stacks).substr(entriesStart + 3, entriesEnd - entriesStart - 3));
			if(const auto target = flowField(injury, "target"))
				summary.push_back("target: " + *target);
			if(const auto damage = flowField(injury, "damage"))
				summary.push_back("damage: " + *damage);
			if(const auto killed = flowField(injury, "killed"); killed && *killed != "0")
				summary.push_back("killed: " + *killed);
		}
	}
	return summary;
}

std::optional<int> battleHealingSummary(const std::string & record)
{
	const auto fields = wrappedFlowFields(record);
	if(!fields || flowField(*fields, "event") != "unitsChanged")
		return std::nullopt;
	const auto changes = flowField(*fields, "changes");
	if(!changes)
		return std::nullopt;

	int total = 0;
	bool found = false;
	for(size_t start = changes->find("unit: "); start != std::string::npos; start = changes->find("unit: ", start))
	{
		const auto state = changes->find(", state: ", start);
		if(state == std::string::npos)
			return std::nullopt;
		const std::string entry = changes->substr(start, state - start);
		if(entry.find("operation: update") == std::string::npos)
			return std::nullopt;
		const std::string marker = "healthDelta: ";
		const auto amountStart = entry.find(marker);
		if(amountStart == std::string::npos)
			return std::nullopt;
		const auto valueStart = amountStart + marker.size();
		const auto valueEnd = entry.find_first_not_of("-0123456789", valueStart);
		const int amount = std::stoi(entry.substr(valueStart, valueEnd - valueStart));
		if(amount <= 0)
			return std::nullopt;
		total += amount;
		found = true;
		start = state + 1;
	}
	return found ? std::optional<int>(total) : std::nullopt;
}

struct BattleHealingChange
{
	std::string unit;
	int amount = 0;
	int count = 0;
	int topHP = 0;
	std::optional<int> position;
};

std::optional<std::vector<BattleHealingChange>> battleHealingChanges(const std::string & record)
{
	const auto fields = wrappedFlowFields(record);
	if(!fields || flowField(*fields, "event") != "unitsChanged")
		return std::nullopt;
	const auto changes = flowField(*fields, "changes");
	if(!changes || changes->size() < 4 || !changes->starts_with("[{") || !changes->ends_with("}]"))
		return std::nullopt;

	std::vector<BattleHealingChange> result;
	for(const auto & item : splitFlowFields(std::string_view(*changes).substr(1, changes->size() - 2)))
	{
		const auto change = wrappedFlowFields(item);
		if(!change || flowField(*change, "operation") != "update")
			return std::nullopt;
		const auto unit = flowField(*change, "unit");
		const auto amount = flowField(*change, "healthDelta");
		const auto state = nestedFlowFields(*change, "state");
		const auto health = state ? nestedFlowFields(*state, "health") : std::nullopt;
		const auto topHP = health ? flowField(*health, "firstHPleft") : std::nullopt;
		if(!unit || !amount || !state || !health || !topHP)
			return std::nullopt;

		BattleHealingChange value;
		value.unit = *unit;
		value.amount = std::stoi(*amount);
		value.topHP = std::stoi(*topHP);
		if(value.amount <= 0 || value.topHP <= 0)
			return std::nullopt;
		if(const auto fullUnits = flowField(*health, "fullUnits"))
			value.count = std::stoi(*fullUnits);
		++value.count; // firstHPleft describes the leading, partially damaged unit.
		if(const auto position = flowField(*state, "position"))
			value.position = std::stoi(*position);
		result.push_back(std::move(value));
	}
	return result.empty() ? std::nullopt : std::optional(std::move(result));
}

void updateBattlePositions(const std::string & record, std::map<std::string, int> & positions)
{
	const auto fields = wrappedFlowFields(record);
	if(!fields || flowField(*fields, "event") != "unitsChanged")
		return;
	const auto changes = flowField(*fields, "changes");
	if(!changes || changes->size() < 4 || !changes->starts_with("[{") || !changes->ends_with("}]"))
		return;

	for(const auto & item : splitFlowFields(std::string_view(*changes).substr(1, changes->size() - 2)))
	{
		const auto change = wrappedFlowFields(item);
		const auto unit = change ? flowField(*change, "unit") : std::nullopt;
		if(!change || !unit)
			continue;
		if(flowField(*change, "operation") == "remove")
		{
			positions.erase(*unit);
			continue;
		}
		const auto state = nestedFlowFields(*change, "state");
		const auto position = state ? flowField(*state, "position") : std::nullopt;
		if(position)
			positions[*unit] = std::stoi(*position);
	}
}

std::optional<std::string> battleTriggeredSpell(const std::string & record)
{
	const auto fields = wrappedFlowFields(record);
	if(!fields || flowField(*fields, "event") != "spellCast" || flowBool(*fields, "hero"))
		return std::nullopt;
	return flowField(*fields, "spell");
}

std::string appendFlowList(const std::string & first, const std::string & second)
{
	if(first == "[]")
		return second;
	if(second == "[]")
		return first;
	if(first.size() < 2 || second.size() < 2)
		return first;
	return first.substr(0, first.size() - 1) + ", " + second.substr(1);
}

std::string battleParticipantActor(const std::string & participant)
{
	static constexpr std::array<std::string_view, 8> colors = {
		"red", "blue", "tan", "green", "orange", "purple", "teal", "pink"};
	std::vector<std::string> parts;
	boost::algorithm::split(parts, participant, boost::is_any_of("/"));
	for(const auto & part : parts)
	{
		if(std::find(colors.begin(), colors.end(), part) != colors.end())
			return part;
	}
	return "world";
}

std::optional<int> battleManaSpent(const std::string & record)
{
	const auto outer = wrappedFlowFields(record);
	if(!outer)
		return std::nullopt;
	const auto mana = nestedFlowFields(*outer, "mana");
	if(!mana || mana->empty())
		return std::nullopt;
	const auto separator = mana->front().find(": ");
	if(separator == std::string::npos)
		return std::nullopt;
	const int change = std::stoi(mana->front().substr(separator + 2));
	return change < 0 ? -change : 0;
}

std::string battleFlowRecord(const std::string & kind, const std::vector<std::string> & fields)
{
	return kind + ": { " + boost::algorithm::join(fields, ", ") + " }";
}

void writeBattleArmyStates(
	std::ostream & stream,
	const std::string & indent,
	const std::map<std::string, std::string> & armies)
{
	if(armies.empty())
	{
		stream << indent << "armies: {}\n";
		return;
	}

	stream << indent << "armies:\n";
	for(const auto & [army, state] : armies)
	{
		if(state == "[]")
		{
			stream << indent << "  " << yamlString(army) << ": []\n";
			continue;
		}

		stream << indent << "  " << yamlString(army) << ":\n";
		if(state.size() < 2 || !state.starts_with('[') || !state.ends_with(']'))
		{
			stream << indent << "    - " << state << "\n";
			continue;
		}
		for(const auto & stack : splitFlowFields(std::string_view(state).substr(1, state.size() - 2)))
			stream << indent << "    - " << stack << "\n";
	}
}
}

void VGTRecorder::capturePendingBattleArmies(const CGameState & gameState)
{
	if(!pendingBattle)
		return;

	pendingBattle->armies.clear();
	for(const auto armyID : pendingBattle->armyIDs)
	{
		const auto * army = dynamic_cast<const CArmedInstance *>(gameState.getMap().getObject(armyID));
		if(army)
			pendingBattle->armies[objectAlias(gameState, armyID)] = strategicArmyState(*army);
	}
	pendingBattle->armiesCaptured = true;
}

void VGTRecorder::flushPendingBattle(const CGameState & gameState)
{
	if(!pendingBattle)
		return;
	flushPendingHeroScene();
	if(pendingBattle->ended && !pendingBattle->armiesCaptured)
		capturePendingBattleArmies(gameState);

	battleOutput << "  - battle:\n";
	battleOutput << "      id: " << pendingBattle->id << "\n";
	if(!pendingBattle->attacker.empty())
		battleOutput << "      attacker: " << pendingBattle->attacker << "\n";
	if(!pendingBattle->defender.empty())
		battleOutput << "      defender: " << pendingBattle->defender << "\n";
	if(!pendingBattle->units.empty())
	{
		battleOutput << "      units:\n";
		for(const auto & unit : pendingBattle->units)
			battleOutput << "        " << unit << "\n";
	}
	if(!pendingBattle->initialRandom.empty())
		battleOutput << "      randomBefore: " << pendingBattle->initialRandom << "\n";
	battleOutput << "      events:\n";
	int round = 0;
	std::string eventIndent = "        ";
	bool roundHeaderPending = false;
	auto writeBattleEvent = [&](const std::string & event)
	{
		if(roundHeaderPending)
		{
			battleOutput << "        - round: " << round << "\n";
			battleOutput << "          events:\n";
			roundHeaderPending = false;
		}
		writeYamlRecord(battleOutput, eventIndent, event);
	};
	auto positions = pendingBattle->positions;
	for(size_t index = 0; index < pendingBattle->events.size();)
	{
		updateBattlePositions(pendingBattle->events[index], positions);
		if(isBattlePacketEvent(pendingBattle->events[index], "start"))
		{
			++index;
			continue;
		}
		if(isBattlePacketEvent(pendingBattle->events[index], "nextRound"))
		{
			++round;
			eventIndent = "            ";
			roundHeaderPending = true;
			++index;
			continue;
		}
		if(const auto action = keyedFlowFields(pendingBattle->events[index], "walk"))
		{
			std::vector<std::string> fields;
			for(const std::string key : {"unit"})
			{
				if(const auto value = flowField(*action, key))
					fields.push_back(key + ": " + *value);
			}
			if(const auto target = flowField(*action, "target"))
			{
				const auto hexes = battleTargetHexes(*target);
				if(!hexes.empty())
					fields.push_back("to: " + std::to_string(hexes.back()));
			}

			std::optional<std::string> path;
			std::optional<std::string> gate;
			size_t end = index + 1;
			while(end < pendingBattle->events.size())
			{
				if(isBattlePacketEvent(pendingBattle->events[end], "move"))
				{
					const auto movement = wrappedFlowFields(pendingBattle->events[end]);
					if(!movement || flowField(*movement, "unit") != flowField(*action, "unit"))
						break;
					if(const auto segment = flowField(*movement, "path"))
						path = path ? appendFlowList(*path, *segment) : *segment;
					if(flowBool(*movement, "teleporting"))
						fields.push_back("teleport: true");
					++end;
					continue;
				}
				const auto effect = wrappedFlowFields(pendingBattle->events[end]);
				const auto gateEffect = effect ? nestedFlowFields(*effect, "gate") : std::nullopt;
				if(!gateEffect)
					break;
				gate = flowField(*gateEffect, "state");
				++end;
			}
			if(!path)
			{
				index = end;
				continue;
			}
			fields.push_back("path: " + *path);
			if(const auto unit = flowField(*action, "unit"))
			{
				const auto hexes = flowIntegerList(*path);
				if(!hexes.empty())
					positions[*unit] = hexes.back();
			}
			if(gate)
				fields.push_back("gate: " + *gate);
			writeBattleEvent(battleFlowRecord("move", fields));
			index = end;
			continue;
		}

		std::optional<std::vector<std::string>> attackAction;
		bool rangedAttack = false;
		if(auto action = keyedFlowFields(pendingBattle->events[index], "walkAndAttack"))
			attackAction = std::move(action);
		else if(auto action = keyedFlowFields(pendingBattle->events[index], "shoot"))
		{
			attackAction = std::move(action);
			rangedAttack = true;
		}
		else if(const auto accepted = wrappedFlowFields(pendingBattle->events[index]);
			accepted && flowField(*accepted, "event") == "acceptedAs")
		{
			const auto action = nestedFlowFields(*accepted, "action");
			if(!action || flowField(*action, "action") != "shoot")
			{
				++index;
				continue;
			}
			attackAction = *action;
			const auto side = flowField(*action, "side");
			const auto & participant = side && *side == "defender"
				? pendingBattle->defender
				: pendingBattle->attacker;
			attackAction->insert(
				attackAction->begin(), "actor: " + battleParticipantActor(participant));
			attackAction->push_back("automatic: true");
			rangedAttack = true;
		}
		if(attackAction)
		{
			std::vector<std::string> fields;
			const auto attackingUnit = flowField(*attackAction, "unit");
			if(attackingUnit)
				fields.push_back("by: " + *attackingUnit);
			const auto aim = flowField(*attackAction, "target");
			const auto actionTargets = aim ? battleActionTargets(*aim) : std::vector<std::vector<std::string>>{};

			size_t end = index + 1;
			std::optional<std::string> approach;
			std::vector<std::string> concurrentEffects;
			while(end < pendingBattle->events.size())
			{
				if(isBattlePacketEvent(pendingBattle->events[end], "move"))
				{
					const auto movement = wrappedFlowFields(pendingBattle->events[end]);
					if(!movement || flowField(*movement, "unit") != flowField(*attackAction, "unit"))
						break;
					if(const auto path = flowField(*movement, "path"))
						approach = approach ? appendFlowList(*approach, *path) : *path;
					++end;
					continue;
				}
				const auto wrapped = wrappedFlowFields(pendingBattle->events[end]);
				if(wrapped && nestedFlowFields(*wrapped, "gate"))
				{
					concurrentEffects.push_back(pendingBattle->events[end]);
					++end;
					continue;
				}
				break;
			}
			std::optional<int> attackFrom;
			if(!rangedAttack && !actionTargets.empty())
			{
				if(const auto value = flowField(actionTargets.front(), "hex"))
					attackFrom = std::stoi(*value);
			}
			if(approach)
			{
				const auto hexes = flowIntegerList(*approach);
				if(!hexes.empty())
				{
					if(!attackFrom)
						attackFrom = hexes.back();
					if(hexes.size() > 1)
					{
						std::vector<std::string> via;
						for(size_t pathIndex = 0; pathIndex + 1 < hexes.size(); ++pathIndex)
							via.push_back(std::to_string(hexes[pathIndex]));
						fields.push_back("via: " + flowList(via));
					}
				}
			}
			if(!attackFrom && attackingUnit)
			{
				const auto position = positions.find(*attackingUnit);
				if(position != positions.end())
					attackFrom = position->second;
			}
			if(attackFrom)
			{
				fields.push_back("from: " + std::to_string(*attackFrom));
				if(attackingUnit)
					positions[*attackingUnit] = *attackFrom;
			}

			std::vector<std::vector<std::string>> strikes;
			if(end < pendingBattle->events.size())
			{
				if(auto applied = appliedBattleAttack(pendingBattle->events[end]))
					strikes.push_back(std::move(*applied));
			}
			if(!strikes.empty())
			{
				if(const auto target = flowField(strikes.front(), "target"))
					fields.push_back("target: " + *target);
				++end;
			}
			else if(aim)
			{
				if(const auto target = battleTargetUnit(*aim))
					fields.push_back("target: " + *target);
			}
			std::optional<int> targetAt;
			const size_t targetIndex = rangedAttack ? 0 : 1;
			if(targetIndex < actionTargets.size())
			{
				if(const auto value = flowField(actionTargets[targetIndex], "hex"))
					targetAt = std::stoi(*value);
			}
			if(!targetAt)
			{
				const auto target = flowField(fields, "target");
				const auto position = target ? positions.find(*target) : positions.end();
				if(position != positions.end())
					targetAt = position->second;
			}
			if(targetAt)
				fields.push_back("targetAt: " + std::to_string(*targetAt));
			if(rangedAttack)
				fields.push_back("ranged: true");
			if(flowBool(*attackAction, "automatic"))
				fields.push_back("automatic: true");

			std::optional<std::vector<std::string>> retaliation;
			std::vector<std::string> appliedSpells;
			std::vector<std::string> attackEffects;
			while(end < pendingBattle->events.size())
			{
				if(isBattlePacketEvent(pendingBattle->events[end], "obstaclesChanged"))
				{
					concurrentEffects.push_back(pendingBattle->events[end]);
					++end;
					continue;
				}
				if(isBattlePacketEvent(pendingBattle->events[end], "stackEffects"))
				{
					++end;
					continue;
				}
				if(const auto spell = battleTriggeredSpell(pendingBattle->events[end]))
				{
					if(std::find(appliedSpells.begin(), appliedSpells.end(), *spell) == appliedSpells.end())
						appliedSpells.push_back(*spell);
					++end;
					continue;
				}
				if(const auto injury = battleInjurySummary(pendingBattle->events[end]); injury && !injury->empty())
				{
					const auto target = flowField(*injury, "target");
					const auto attacker = flowField(*attackAction, "unit");
					std::optional<std::string> effect;
					if(!appliedSpells.empty())
						effect = appliedSpells.back();
					else if(target && target == attacker)
						effect = "reflection";
					if(effect)
					{
						std::vector<std::string> effectFields = {"effect: " + *effect};
						effectFields.insert(effectFields.end(), injury->begin(), injury->end());
						attackEffects.push_back("{ " + boost::algorithm::join(effectFields, ", ") + " }");
						++end;
						continue;
					}
				}
				const auto nextAttack = appliedBattleAttack(pendingBattle->events[end]);
				if(!nextAttack)
					break;
				if(flowBool(*nextAttack, "retaliation"))
				{
					retaliation = *nextAttack;
					++end;
					continue;
				}
				if(strikes.empty())
				{
					if(flowField(*nextAttack, "by") != flowField(*attackAction, "unit"))
						break;
					strikes.push_back(*nextAttack);
					++end;
					continue;
				}
				if(flowField(*nextAttack, "by") != flowField(strikes.front(), "by") ||
					flowField(*nextAttack, "target") != flowField(strikes.front(), "target"))
					break;
				strikes.push_back(*nextAttack);
				++end;
			}
			appendBattleAttackOutcome(fields, strikes);
			if(!appliedSpells.empty())
				fields.push_back("applies: " + flowList(appliedSpells));
			if(!attackEffects.empty())
				fields.push_back("effects: " + flowList(attackEffects));
			if(retaliation)
				appendBattleRetaliation(fields, *retaliation);

			if(end < pendingBattle->events.size() && isBattlePacketEvent(pendingBattle->events[end], "move"))
			{
				const auto movement = wrappedFlowFields(pendingBattle->events[end]);
				if(movement && flowField(*movement, "unit") == flowField(*attackAction, "unit"))
				{
					if(const auto path = flowField(*movement, "path"))
					{
						const auto hexes = flowIntegerList(*path);
						if(hexes.size() == 1)
							fields.push_back("returnsTo: " + std::to_string(hexes.front()));
						else if(!hexes.empty())
							fields.push_back("returnPath: " + *path);
						if(!hexes.empty() && attackingUnit)
							positions[*attackingUnit] = hexes.back();
					}
					++end;
				}
			}

			for(const auto & effect : concurrentEffects)
				writeBattleEvent(effect);
			writeBattleEvent(battleFlowRecord("attack", fields));
			index = end;
			continue;
		}

		std::optional<std::vector<std::string>> spellAction;
		bool heroCast = false;
		if(auto action = keyedFlowFields(pendingBattle->events[index], "heroSpell"))
		{
			spellAction = std::move(action);
			heroCast = true;
		}
		else if(auto action = keyedFlowFields(pendingBattle->events[index], "monsterSpell"))
			spellAction = std::move(action);
		if(spellAction)
		{
			std::vector<std::string> fields;
			const auto side = flowField(*spellAction, "side");
			if(heroCast)
			{
				if(side)
					fields.push_back("side: " + *side);
				fields.push_back("caster: " + (side == "defender" ? pendingBattle->defender : pendingBattle->attacker));
			}
			else if(const auto unit = flowField(*spellAction, "unit"))
				fields.push_back("caster: " + *unit);
			if(const auto value = flowField(*spellAction, "spell"))
				fields.push_back("spell: " + *value);
			const auto aim = flowField(*spellAction, "target");
			fields.push_back("aim: " + aim.value_or("[]"));

			std::optional<int> mana;
			std::optional<int> healed;
			std::vector<std::string> injury;
			size_t end = index + 1;
			while(end < pendingBattle->events.size())
			{
				if(isBattlePacketEvent(pendingBattle->events[end], "spellCast") ||
					isBattlePacketEvent(pendingBattle->events[end], "stackEffects"))
				{
					++end;
					continue;
				}
				if(const auto spent = battleManaSpent(pendingBattle->events[end]))
				{
					mana = *spent;
					++end;
					continue;
				}
				if(const auto amount = battleHealingSummary(pendingBattle->events[end]))
				{
					healed = healed.value_or(0) + *amount;
					++end;
					continue;
				}
				if(const auto summary = battleInjurySummary(pendingBattle->events[end]))
				{
					if(!summary->empty())
						injury = *summary;
					++end;
					continue;
				}
				break;
			}
			if(!injury.empty())
				fields.insert(fields.end(), injury.begin(), injury.end());
			else if(aim)
			{
				if(const auto target = battleTargetUnit(*aim))
					fields.push_back("target: " + *target);
			}
			if(mana && *mana != 0)
				fields.push_back("mana: " + std::to_string(*mana));
			if(healed)
				fields.push_back("healed: " + std::to_string(*healed));

			writeBattleEvent(battleFlowRecord("cast", fields));
			index = end;
			continue;
		}

		const std::string attackPrefix = "{ attack: { ";
		if(pendingBattle->events[index].starts_with(attackPrefix) && pendingBattle->events[index].ends_with(" } }"))
		{
			auto attack = pendingBattle->events[index].substr(attackPrefix.size(), pendingBattle->events[index].size() - attackPrefix.size() - 4);
			if(index + 1 < pendingBattle->events.size() && pendingBattle->events[index + 1].starts_with(attackPrefix) &&
				pendingBattle->events[index + 1].find(", retaliation: true") != std::string::npos)
			{
				auto retaliation = pendingBattle->events[index + 1].substr(
					attackPrefix.size(), pendingBattle->events[index + 1].size() - attackPrefix.size() - 4);
				boost::algorithm::erase_first(retaliation, ", retaliation: true");
				if(const auto damage = retaliation.find("damage: "); damage != std::string::npos)
					retaliation.erase(0, damage);
				attack += ", retaliation: { " + retaliation + " }";
				++index;
			}
			else
				boost::algorithm::replace_all(attack, ", retaliation: true", ", counterattack: true");
			writeBattleEvent("attack: { " + attack + " }");
			++index;
			continue;
		}

		if(const auto automaticSpell = battleTriggeredSpell(pendingBattle->events[index]);
			automaticSpell && *automaticSpell == "catapultShot" && index + 1 < pendingBattle->events.size())
		{
			const auto next = wrappedFlowFields(pendingBattle->events[index + 1]);
			if(next && nestedFlowFields(*next, "catapult"))
			{
				++index;
				continue;
			}
		}
		if(const auto automaticSpell = battleTriggeredSpell(pendingBattle->events[index]);
			automaticSpell && index + 1 < pendingBattle->events.size())
		{
			const auto spell = wrappedFlowFields(pendingBattle->events[index]);
			const auto healing = battleHealingChanges(pendingBattle->events[index + 1]);
			const auto caster = spell ? flowField(*spell, "caster") : std::nullopt;
			if(healing && caster)
			{
				for(const auto & change : *healing)
				{
					std::vector<std::string> after = {
						"count: " + std::to_string(change.count),
						"topHp: " + std::to_string(change.topHP)};
					if(change.position)
						after.push_back("at: " + std::to_string(*change.position));
					writeBattleEvent(battleFlowRecord("heal", {
						"by: " + *caster,
						"target: " + change.unit,
						"amount: " + std::to_string(change.amount),
						"after: { " + boost::algorithm::join(after, ", ") + " }"}));
				}
				index += 2;
				continue;
			}
		}
		if(pendingBattle->events[index].starts_with("wait: { "))
		{
			std::vector<std::string> units;
			size_t end = index;
			while(end < pendingBattle->events.size() && pendingBattle->events[end].starts_with("wait: { "))
			{
				const auto marker = pendingBattle->events[end].find("unit: ");
				if(marker == std::string::npos)
					break;
				const auto valueStart = marker + std::string("unit: ").size();
				const auto valueEnd = pendingBattle->events[end].find_first_of(", }", valueStart);
				units.push_back(pendingBattle->events[end].substr(valueStart, valueEnd - valueStart));
				++end;
			}
			if(!units.empty())
			{
				writeBattleEvent("wait: " + (units.size() == 1 ? units.front() : flowList(units)));
				index = end;
				continue;
			}
		}
		if(pendingBattle->events[index].starts_with("defend: { "))
		{
			std::vector<std::string> units;
			size_t end = index;
			while(end < pendingBattle->events.size())
			{
				const auto action = keyedFlowFields(pendingBattle->events[end], "defend");
				if(!action)
					break;
				if(const auto unit = flowField(*action, "unit"))
					units.push_back(*unit);
				else
					break;
				++end;
				while(end < pendingBattle->events.size() && isBattlePacketEvent(pendingBattle->events[end], "stackEffects"))
					++end;
			}
			if(!units.empty())
			{
				writeBattleEvent("defend: " + (units.size() == 1 ? units.front() : flowList(units)));
				index = end;
				continue;
			}
		}
		bool compactedDecision = false;
		for(const std::string kind : {
			"badMorale", "catapult", "none", "retreat", "stackHeal", "surrender", "walkAndCast"})
		{
			const auto action = keyedFlowFields(pendingBattle->events[index], kind);
			const auto unit = action ? flowField(*action, "unit") : std::nullopt;
			if(!action || !unit || (!unit->starts_with("attacker/") && !unit->starts_with("defender/")))
				continue;
			std::vector<std::string> compact;
			for(const auto & field : *action)
			{
				if(!field.starts_with("actor: ") && !field.starts_with("side: "))
					compact.push_back(field);
			}
			writeBattleEvent(battleFlowRecord(kind, compact));
			++index;
			compactedDecision = true;
			break;
		}
		if(compactedDecision)
			continue;
		writeBattleEvent(pendingBattle->events[index]);
		++index;
	}
	if(!pendingBattle->outcome.empty() || !pendingBattle->manaChanges.empty() ||
		!pendingBattle->armies.empty() || !pendingBattle->aftermath.empty())
	{
		battleOutput << "      outcome:\n";
		for(const auto & field : pendingBattle->outcome)
			battleOutput << "        " << field << "\n";
		if(pendingBattle->survivors.empty())
			battleOutput << "        survivors: {}\n";
		else
		{
			battleOutput << "        survivors:\n";
			for(const auto & survivor : pendingBattle->survivors)
				battleOutput << "          " << survivor << "\n";
		}
		if(!pendingBattle->createdUnits.empty())
			battleOutput << "        createdUnits: { " + boost::algorithm::join(pendingBattle->createdUnits, ", ") + " }\n";
		if(!pendingBattle->randomBeforeAftermath.empty())
			battleOutput << "        randomBeforeAftermath: " << pendingBattle->randomBeforeAftermath << "\n";
		if(!pendingBattle->continuation.empty())
			battleOutput << "        continuation: " << pendingBattle->continuation << "\n";
		if(!pendingBattle->manaChanges.empty())
			battleOutput << "        mana: " << namedIntegerMap(pendingBattle->manaChanges) << "\n";
		if(pendingBattle->ended)
			writeBattleArmyStates(battleOutput, "        ", pendingBattle->armies);
		if(!pendingBattle->aftermath.empty())
		{
			battleOutput << "        aftermath:\n";
			for(const auto & record : pendingBattle->aftermath)
				battleOutput << "          - { " << record << " }\n";
		}
	}
	battleOutput.flush();

	auto outcomeValue = [&](const std::string & key) -> std::optional<std::string>
	{
		const std::string prefix = key + ": ";
		for(const auto & field : pendingBattle->outcome)
		{
			if(field.starts_with(prefix))
				return field.substr(prefix.size());
		}
		return std::nullopt;
	};

	output << "  - battle:\n";
	output << "      tactics: " << pendingBattle->id << "\n";
	if(!pendingBattle->attacker.empty())
		output << "      attacker: " << pendingBattle->attacker << "\n";
	if(!pendingBattle->defender.empty())
		output << "      defender: " << pendingBattle->defender << "\n";
	if(!pendingBattle->units.empty())
	{
		output << "      forces:\n";
		for(const auto & unit : pendingBattle->units)
		{
			const auto mapping = unit.find(": { ");
			const auto count = unit.find("count: ");
			if(mapping == std::string::npos || count == std::string::npos)
				continue;
			const auto end = unit.find_first_of(" }", count + 7);
			output << "        " << unit.substr(0, mapping) << ": "
				<< unit.substr(count + 7, end - count - 7) << "\n";
		}
	}
	output << "      outcome:\n";
	const std::string result = outcomeValue("result").value_or("unknown");
	const std::string winner = outcomeValue("winner").value_or("unknown");
	const std::string loser = outcomeValue("loser").value_or("unknown");
	if(result == "normal")
		output << "        victory: " << winner << "\n";
	else if(result == "escape")
	{
		output << "        victory: " << winner << "\n";
		output << "        escaped: " << loser << "\n";
	}
	else if(result == "surrender")
	{
		output << "        victory: " << winner << "\n";
		output << "        surrendered: " << loser << "\n";
	}
	else
	{
		output << "        result: " << result << "\n";
		output << "        winner: " << winner << "\n";
	}
	for(const auto & field : pendingBattle->outcome)
	{
		if(!field.starts_with("result: ") && !field.starts_with("winnerSide: ") &&
			!field.starts_with("winner: ") && !field.starts_with("loser: "))
			output << "        " << field << "\n";
	}
	if(!pendingBattle->manaChanges.empty())
		output << "        mana: " << namedIntegerMap(pendingBattle->manaChanges) << "\n";
	if(pendingBattle->ended)
		writeBattleArmyStates(output, "        ", pendingBattle->armies);
	if(!pendingBattle->aftermath.empty())
	{
		output << "        aftermath:\n";
		for(const auto & record : pendingBattle->aftermath)
			output << "          - { " << record << " }\n";
	}
	output.flush();
	battleUnitAliases.erase(std::stoi(pendingBattle->id));
	pendingBattle.reset();
}

void VGTRecorder::flushPendingHeroScene()
{
	if(!pendingHeroScene)
		return;

	if(pendingHeroScene->actions.size() == 1)
		writeYamlRecord(output, "  ", pendingHeroScene->actions.front().original);
	else
	{
		output << "  - with: " << pendingHeroScene->hero << "\n";
		output << "    actions:\n";
		for(const auto & action : pendingHeroScene->actions)
			writeYamlRecord(output, "      ", action.nested);
	}
	output.flush();
	pendingHeroScene.reset();
}

void VGTRecorder::collectInitialAvailability(const CGameState & gameState, const SetAvailableCreatures & availability)
{
	const std::string pools = sparseCreaturePools(availability.creatures);
	if(pools == "{  }")
		return;

	const std::string object = objectAlias(gameState, availability.tid);
	if(dynamic_cast<const CGTownInstance *>(gameState.getMap().getObject(availability.tid)))
		pendingInitialTownAvailability[object] = pools;
	else
		pendingInitialDwellingAvailability[object] = pools;
}

void VGTRecorder::flushPendingInitialAvailability(const CGameState & gameState)
{
	if(pendingInitialTownAvailability.empty() && pendingInitialDwellingAvailability.empty())
		return;

	std::vector<std::string> scopes;
	for(const auto & [scopeName, availability] : {
		std::pair{"towns", &pendingInitialTownAvailability},
		std::pair{"dwellings", &pendingInitialDwellingAvailability}})
	{
		if(availability->empty())
			continue;
		std::vector<std::string> entries;
		for(const auto & [object, pools] : *availability)
			entries.push_back(object + ": " + pools);
		scopes.push_back(std::string(scopeName) + ": { " + boost::algorithm::join(entries, ", ") + " }");
	}

	pendingInitialTownAvailability.clear();
	pendingInitialDwellingAvailability.clear();
	writeActionLine(gameState, "availability: { " + boost::algorithm::join(scopes, ", ") + " }");
}

void VGTRecorder::collectWeeklyAvailability(const CGameState & gameState, const SetAvailableCreatures & availability)
{
	const bool isTown = dynamic_cast<const CGTownInstance *>(gameState.getMap().getObject(availability.tid));
	if(!isTown && isStandardDwellingRefresh(gameState, availability))
		return;

	const std::string pools = sparseCreaturePools(availability.creatures);
	if(pools == "{  }")
		return;

	auto & scope = isTown ? pendingWeeklyTownAvailability : pendingWeeklyDwellingAvailability;
	scope[objectAlias(gameState, availability.tid)] = pools;
}

void VGTRecorder::flushPendingWeeklyWorldEvents(const CGameState & gameState)
{
	if(!pendingWeeklyTownAvailability.empty() || !pendingWeeklyDwellingAvailability.empty())
	{
		std::vector<std::string> scopes;
		for(const auto & [scopeName, availability] : {
			std::pair{"towns", &pendingWeeklyTownAvailability},
			std::pair{"dwellings", &pendingWeeklyDwellingAvailability}})
		{
			if(availability->empty())
				continue;
			std::vector<std::string> entries;
			for(const auto & [object, pools] : *availability)
				entries.push_back(yamlKey(object) + ": " + pools);
			scopes.push_back(std::string(scopeName) + ": { " + boost::algorithm::join(entries, ", ") + " }");
		}
		writeActionLine(gameState, "weeklyAvailability: { " + boost::algorithm::join(scopes, ", ") + " }");
	}

	if(!pendingWeeklyRewards.empty())
	{
		using RewardKey = std::pair<std::string, std::string>;
		std::map<RewardKey, std::vector<std::string>> objectsByReward;
		for(const auto & entry : pendingWeeklyRewards)
			objectsByReward[{entry.reward, entry.text}].push_back(entry.object);

		output << "  - weeklyRewards:\n";
		for(const auto & [reward, objects] : objectsByReward)
		{
			output << "      - at:\n";
			for(const auto & object : objects)
				output << "          - " << object << "\n";
			if(!reward.first.empty())
				output << "        reward: " << reward.first << "\n";
			if(!reward.second.empty())
				output << "        text: " << reward.second << "\n";
		}
		output.flush();
	}
	if(!pendingWeeklySpawns.empty())
	{
		std::vector<std::string> groups;
		for(const auto & [creatureName, spawns] : pendingWeeklySpawns)
			groups.push_back(yamlKey(creatureName) + ": " + flowList(spawns));
		writeActionLine(gameState, "spawns: { " + boost::algorithm::join(groups, ", ") + " }");
	}

	pendingWeeklyTownAvailability.clear();
	pendingWeeklyDwellingAvailability.clear();
	pendingWeeklyRewards.clear();
	pendingWeeklySpawns.clear();
}

void VGTRecorder::collectDiscoveries(const CGameState & gameState, const TryMoveHero & move)
{
	if(move.fowRevealed.empty())
		return;

	const auto * hero = gameState.getHero(move.id);
	if(!hero || !hero->tempOwner.isValidPlayer())
		return;
	const PlayerColor player = hero->tempOwner;

	discoveryTracker.observeVisible(gameState, player);
	std::set<std::string> discoveredAliases;
	for(const ObjectInstanceID objectID : discoveryTracker.discoverInTiles(gameState, player, move.fowRevealed))
	{
		if(objectID != move.id)
			discoveredAliases.insert(objectAlias(gameState, objectID));
	}
	if(discoveredAliases.empty())
		return;

	const std::vector<std::string> discoveries(discoveredAliases.begin(), discoveredAliases.end());
	const std::string movingHero = heroAlias(gameState, move.id);
	if(pendingMove && pendingMove->hero == movingHero)
	{
		for(const auto & discovery : discoveries)
		{
			if(!vstd::contains(pendingMove->discoveries, discovery))
				pendingMove->discoveries.push_back(discovery);
		}
		return;
	}
	if(pendingEncounter && pendingEncounter->hero == movingHero)
	{
		for(const auto & discovery : discoveries)
		{
			if(!vstd::contains(pendingEncounter->discoveries, discovery))
				pendingEncounter->discoveries.push_back(discovery);
		}
		return;
	}

	writeActionLine(gameState, "discovers: { hero: " + movingHero +
		", objects: " + flowList(discoveries) + " }");
}

void VGTRecorder::writeActionLine(const CGameState & gameState, const std::string & line)
{
	if(line.empty())
		return;
	if(!documentOpen)
		startWorldDocument(gameState, "startup");
	if(!enabled || !documentOpen)
		return;

	if(const auto battleRecord = battleBlockRecord(line))
	{
		flushPendingHeroScene();
		if(!pendingBattle || pendingBattle->id != battleRecord->battleID)
		{
			flushPendingBattle(gameState);
			pendingBattle.emplace();
			pendingBattle->id = battleRecord->battleID;
			pendingBattle->attacker = battleParticipant(
				gameState, BattleID(std::stoi(battleRecord->battleID)), BattleSide::ATTACKER);
			pendingBattle->defender = battleParticipant(
				gameState, BattleID(std::stoi(battleRecord->battleID)), BattleSide::DEFENDER);
			pendingBattle->units = battleRoster(gameState, BattleID(std::stoi(battleRecord->battleID)));
			pendingBattle->positions = battlePositions(gameState, BattleID(std::stoi(battleRecord->battleID)));
		}
		if(pendingBattle->units.empty())
		{
			pendingBattle->units = battleRoster(gameState, BattleID(std::stoi(battleRecord->battleID)));
			pendingBattle->positions = battlePositions(gameState, BattleID(std::stoi(battleRecord->battleID)));
			pendingBattle->attacker = battleParticipant(
				gameState, BattleID(std::stoi(battleRecord->battleID)), BattleSide::ATTACKER);
			pendingBattle->defender = battleParticipant(
				gameState, BattleID(std::stoi(battleRecord->battleID)), BattleSide::DEFENDER);
		}
		if(const auto * battle = gameState.getBattle(BattleID(std::stoi(battleRecord->battleID))))
		{
			pendingBattle->randomizerParticipants = battleRandomizerParticipants(
				gameState, BattleID(std::stoi(battleRecord->battleID)));
			pendingBattle->randomizerHeroes = battleRandomizerHeroes(
				gameState, BattleID(std::stoi(battleRecord->battleID)));
			for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
			{
				const auto * army = battle->battleGetArmyObject(side);
				if(army && !vstd::contains(pendingBattle->armyIDs, army->id))
					pendingBattle->armyIDs.push_back(army->id);
			}
		}
		pendingBattle->events.push_back(battleRecord->record);
		return;
	}

	if(pendingBattle)
	{
		if(pendingBattle->ended)
			pendingBattle->aftermath.push_back(line);
		else
			pendingBattle->events.push_back("{ " + line + " }");
		return;
	}
	const std::string relativeLine = turnRelativeReferences(line, currentTurnPlayer);
	if(const auto heroAction = heroActionLine(relativeLine))
	{
		if(pendingHeroScene && pendingHeroScene->hero != heroAction->hero)
			flushPendingHeroScene();
		if(!pendingHeroScene)
			pendingHeroScene = PendingHeroScene{heroAction->hero, {}};
		pendingHeroScene->actions.push_back({relativeLine, heroAction->withoutHero});
		return;
	}
	flushPendingHeroScene();
	writeYamlRecord(output, "  ", relativeLine);
	output.flush();
}

void VGTRecorder::flushPendingMove(const CGameState & gameState)
{
	if(!pendingMove)
		return;

	std::string line = "move: { ";
	if(!currentTurnPlayer || pendingMove->actor != currentTurnPlayer->toString())
		line += "actor: " + pendingMove->actor + ", ";
	line += "hero: " + pendingMove->hero;
	if(pendingMove->route.size() == 1)
	{
		line += ", to: " + pos(pendingMove->route.front());
	}
	else
	{
		const auto directions = encodedDirections(pendingMove->start, pendingMove->route);
		if(!directions)
		{
			logGlobal->error("Unable to encode non-adjacent VGT move for %s", pendingMove->hero);
			pendingMove.reset();
			writeActionLine(gameState, "unmodelled: { stream: decision, pack: MoveHero, material: true }");
			return;
		}
		else
		{
			line += ", to: " + pos(pendingMove->route.back());
			line += ", steps: " + yamlString(*directions);
		}
	}
	if(pendingMove->transit)
		line += ", transit: true";
	if(!pendingMove->discoveries.empty())
		line += ", discovers: " + flowList(pendingMove->discoveries);
	line += " }";
	pendingMove.reset();
	writeActionLine(gameState, line);
}

void VGTRecorder::flushPendingRecruit(const CGameState & gameState)
{
	if(!pendingRecruit)
		return;

	std::string line = "recruit: { ";
	if(!currentTurnPlayer || pendingRecruit->actor != currentTurnPlayer->toString())
		line += "actor: " + pendingRecruit->actor + ", ";
	line += "at: " + pendingRecruit->source;
	if(pendingRecruit->destination != pendingRecruit->source)
		line += ", to: " + pendingRecruit->destination;
	if(pendingRecruit->units.size() == 1 && pendingRecruit->units.front().slot >= 0)
	{
		const auto & unit = pendingRecruit->units.front();
		line += ", units: { " + yamlKey(unit.creature) + ": " + std::to_string(unit.count) + " }";
		if(unit.pool)
			line += ", pool: " + *unit.pool;
	}
	else
	{
		std::vector<std::string> units;
		for(const auto & unit : pendingRecruit->units)
		{
			std::string entry = "{ creature: " + unit.creature + ", count: " + std::to_string(unit.count);
			if(unit.slot >= 0)
				entry += ", slot: " + std::to_string(unit.slot);
			if(unit.pool)
				entry += ", pool: " + *unit.pool;
			entry += " }";
			units.push_back(entry);
		}
		line += ", units: " + flowList(units);
	}
	if(!pendingRecruit->paid.empty())
		line += ", paid: " + namedIntegerMap(pendingRecruit->paid);
	line += " }";

	pendingRecruit.reset();
	suppressDerivedEffects = false;
	writeActionLine(gameState, line);
}

void VGTRecorder::flushPendingTrade(const CGameState & gameState)
{
	if(!pendingTrade)
		return;

	std::vector<PendingTrade::Exchange> aggregated;
	for(const auto & exchange : pendingTrade->exchanges)
	{
		if(aggregated.empty() || aggregated.back().sold != exchange.sold || aggregated.back().bought != exchange.bought ||
			static_cast<uint64_t>(aggregated.back().boughtAmount) * exchange.soldAmount !=
			static_cast<uint64_t>(exchange.boughtAmount) * aggregated.back().soldAmount)
			aggregated.push_back(exchange);
		else
		{
			auto & total = aggregated.back();
			total.soldAmount += exchange.soldAmount;
			total.boughtAmount += exchange.boughtAmount;
		}
	}

	std::vector<std::string> exchanges;
	for(const auto & exchange : aggregated)
	{
		exchanges.push_back(yamlString(std::to_string(exchange.soldAmount) + " " + exchange.sold +
			" for " + std::to_string(exchange.boughtAmount) + " " + exchange.bought));
	}

	std::string line = "trade: { ";
	if(!currentTurnPlayer || pendingTrade->actor != currentTurnPlayer->toString())
		line += "actor: " + pendingTrade->actor + ", ";
	line += "at: " + pendingTrade->market + ", exchanges: " + flowList(exchanges) + " }";
	pendingTrade.reset();
	suppressDerivedEffects = false;
	writeActionLine(gameState, line);
}

void VGTRecorder::flushPendingArtifactTransfer(const CGameState & gameState)
{
	if(!pendingArtifactTransfer)
		return;

	std::vector<std::string> moves;
	for(const auto & move : pendingArtifactTransfer->moves)
	{
		moves.push_back("{ artifact: " + move.artifact +
			", from: " + move.from + ", to: " + move.to + " }");
	}

	std::string line = "moveArtifacts: { ";
	if(!currentTurnPlayer || pendingArtifactTransfer->actor != currentTurnPlayer->toString())
		line += "actor: " + pendingArtifactTransfer->actor + ", ";
	line += "from: " + pendingArtifactTransfer->from +
		", to: " + pendingArtifactTransfer->to +
		", artifacts: " + flowList(moves) + " }";
	pendingArtifactTransfer.reset();
	writeActionLine(gameState, line);
}

std::shared_ptr<const Bonus> undeadMoraleBonus(const CGameState & gameState, ObjectInstanceID armyID)
{
	const auto * army = dynamic_cast<const CArmedInstance *>(gameState.getMap().getObject(armyID));
	if(!army)
		return {};
	return army->getExportedBonusList().getFirst(
		Selector::source(BonusSource::ARMY, BonusCustomSource::undeadMoraleDebuff)
			.And(Selector::type()(BonusType::MORALE)));
}

void VGTRecorder::flushPendingArmyArrangement(const CGameState & gameState)
{
	if(!pendingArmyArrangement)
		return;

	std::vector<std::string> armies;
	for(const auto armyID : pendingArmyArrangement->armies)
	{
		const auto * army = dynamic_cast<const CArmedInstance *>(gameState.getMap().getObject(armyID));
		if(army)
			armies.push_back(yamlKey(objectAlias(gameState, armyID)) + ": " + strategicArmyState(*army));
	}
	std::vector<std::string> refreshedUndeadMorale;
	for(const auto & [armyID, initialBonus] : pendingArmyArrangement->initialUndeadMoraleBonuses)
	{
		const auto finalBonus = undeadMoraleBonus(gameState, armyID);
		if(finalBonus && finalBonus != initialBonus)
			refreshedUndeadMorale.push_back(objectAlias(gameState, armyID));
	}
	std::string line = "arrangeArmies: { ";
	if(!currentTurnPlayer || pendingArmyArrangement->actor != currentTurnPlayer->toString())
		line += "actor: " + pendingArmyArrangement->actor + ", ";
	line += "armies: { " + boost::algorithm::join(armies, ", ") + " }";
	if(!refreshedUndeadMorale.empty())
		line += ", refreshUndeadMorale: " + flowList(refreshedUndeadMorale);
	line += " }";
	pendingArmyArrangement.reset();
	writeActionLine(gameState, line);
}

void VGTRecorder::flushPendingEncounter(const CGameState & gameState)
{
	if(!pendingEncounter)
		return;
	// Collapse exact pickup/join packet sequences into one story outcome. These
	// patterns are deliberately strict so pre-battle neutral stack splitting is
	// never mistaken for creatures joining the hero.
	if(pendingEncounter->outcomes.size() == 2)
	{
		const auto removed = keyedFlowFields(pendingEncounter->outcomes[1], "remove");
		const bool removesEncounter = removed && flowField(*removed, "object") == pendingEncounter->object;
		if(removesEncounter)
		{
			if(const auto resources = keyedFlowFields(pendingEncounter->outcomes[0], "resources"))
			{
				pendingEncounter->outcomes = {
					"collects: { resources: { " + boost::algorithm::join(*resources, ", ") + " } }"};
			}
			else if(const auto army = keyedFlowFields(pendingEncounter->outcomes[0], "army"))
			{
				const auto movement = nestedFlowFields(*army, "move");
				const auto from = movement ? nestedFlowFields(*movement, "from") : std::nullopt;
				const auto to = movement ? nestedFlowFields(*movement, "to") : std::nullopt;
				const auto source = from ? flowField(*from, "owner") : std::nullopt;
				const auto destination = to ? flowField(*to, "owner") : std::nullopt;
				const auto count = movement ? flowField(*movement, "count") : std::nullopt;
				if(source == pendingEncounter->object && destination && count &&
					pendingEncounter->object.starts_with("monster/"))
				{
					const auto creatureStart = std::string("monster/").size();
					const auto creatureEnd = pendingEncounter->object.find('@', creatureStart);
					const std::string joinedCreature = pendingEncounter->object.substr(
						creatureStart, creatureEnd - creatureStart);
					pendingEncounter->outcomes = {"joins: { hero: " + *destination +
						", units: { " + joinedCreature + ": " + *count + " } }"};
				}
			}
			else if(const auto artifactChange = keyedFlowFields(pendingEncounter->outcomes[0], "artifact"))
			{
				std::vector<std::string> fields;
				if(const auto created = nestedFlowFields(*artifactChange, "create"))
				{
					if(const auto value = flowField(*created, "artifact"))
						fields.push_back("artifact: " + *value);
					if(const auto value = flowField(*created, "position"))
						fields.push_back("slot: " + *value);
				}
				else if(const auto put = nestedFlowFields(*artifactChange, "put"))
				{
					std::string found = pendingEncounter->object.substr(0, pendingEncounter->object.find('@'));
					if(found.starts_with("artifact/"))
						found.erase(0, std::string("artifact/").size());
					fields.push_back("artifact: " + found);
					if(const auto value = flowField(*put, "artifactInstance"))
						fields.push_back("instance: " + *value);
					if(const auto target = nestedFlowFields(*put, "to"))
					{
						if(const auto value = flowField(*target, "slot"))
							fields.push_back("slot: " + *value);
					}
				}
				if(!fields.empty())
					pendingEncounter->outcomes = {
						"finds: { " + boost::algorithm::join(fields, ", ") + " }"};
			}
		}
	}
	if(pendingEncounter->object.starts_with("monster/") &&
		std::none_of(pendingEncounter->outcomes.begin(), pendingEncounter->outcomes.end(), [](const std::string & outcome)
		{
			return outcome.starts_with("joins: ");
		}))
	{
		std::erase_if(pendingEncounter->outcomes, [](const std::string & outcome)
		{
			return outcome.starts_with("army: ");
		});
	}

	auto appendApproach = [this](std::string & line)
	{
		if(!pendingEncounter->approach)
			return;
		const auto & move = *pendingEncounter->approach;
		line += ", approach: { to: " + pos(move.route.back());
		if(move.route.size() > 1)
		{
			const auto steps = encodedDirections(move.start, move.route);
			if(steps)
				line += ", steps: " + yamlString(*steps);
		}
		if(move.transit)
			line += ", transit: true";
		if(!move.discoveries.empty())
			line += ", discovers: " + flowList(move.discoveries);
		line += " }";
	};
	auto appendDiscoveries = [this](std::string & line)
	{
		if(!pendingEncounter->discoveries.empty())
			line += ", discovers: " + flowList(pendingEncounter->discoveries);
	};

	if(pendingEncounter->teleport && pendingEncounter->answered)
	{
		std::string line = "teleport: { ";
		if(!currentTurnPlayer || pendingEncounter->actor != currentTurnPlayer->toString())
			line += "actor: " + pendingEncounter->actor + ", ";
		line += "hero: " + pendingEncounter->hero + ", via: " + pendingEncounter->object;
		appendApproach(line);
		appendDiscoveries(line);

		std::array<int, 3> destination = pendingEncounter->teleportStart;
		if(const auto * hero = gameState.getHero(pendingEncounter->heroID))
		{
			const auto position = hero->visitablePos();
			destination = {position.x, position.y, position.z};
		}
		const int32_t answer = pendingEncounter->answer.value_or(-1);
		if(answer < 0)
			line += ", random: true";

		std::string exit;
		if(answer >= 0 && static_cast<size_t>(answer) < pendingEncounter->teleportExits.size())
			exit = pendingEncounter->teleportExits[answer].object;
		if(exit.empty())
		{
			for(const auto & candidate : pendingEncounter->teleportExits)
			{
				if(candidate.position == destination)
				{
					exit = candidate.object;
					break;
				}
			}
		}
		if(exit.empty() && destination != pendingEncounter->teleportStart)
		{
			for(const auto & object : gameState.getMap().getObjects())
			{
				if(object && dynamic_cast<const CGTeleport *>(object) &&
					object->visitablePos() == int3(destination[0], destination[1], destination[2]))
				{
					exit = objectAlias(gameState, object->id);
					break;
				}
			}
		}
		if(!exit.empty())
			line += ", exit: " + exit;
		if(destination != pendingEncounter->teleportStart)
			line += ", to: " + pos(destination);
		else
			line += ", blocked: true";
		if(!pendingEncounter->outcomes.empty())
		{
			std::vector<std::string> outcomes;
			for(const auto & outcome : pendingEncounter->outcomes)
				outcomes.push_back("{ " + outcome + " }");
			line += ", outcome: " + flowList(outcomes);
		}
		line += " }";
		pendingEncounter.reset();
		writeActionLine(gameState, line);
		return;
	}

	const auto capture = std::find_if(pendingEncounter->outcomes.begin(), pendingEncounter->outcomes.end(), [](const std::string & outcome)
	{
		return outcome.starts_with("capture: { ");
	});
	const bool captureOnly = capture != pendingEncounter->outcomes.end() && !pendingEncounter->answered && !pendingEncounter->text &&
		std::all_of(pendingEncounter->outcomes.begin(), pendingEncounter->outcomes.end(), [](const std::string & outcome)
		{
			return outcome.starts_with("capture: { ") || outcome.starts_with("opened: ");
		});
	if(captureOnly)
	{
		std::string line = "capture: { ";
		if(!currentTurnPlayer || pendingEncounter->actor != currentTurnPlayer->toString())
			line += "actor: " + pendingEncounter->actor + ", ";
		line += "hero: " + pendingEncounter->hero + ", object: " + pendingEncounter->object +
			", owner: " + pendingEncounter->actor;
		appendApproach(line);
		appendDiscoveries(line);
		for(const auto & outcome : pendingEncounter->outcomes)
		{
			if(outcome.starts_with("opened: "))
				line += ", " + outcome;
		}
		line += " }";
		pendingEncounter.reset();
		writeActionLine(gameState, line);
		return;
	}

	if(!pendingEncounter->answered && pendingEncounter->outcomes.empty() && !pendingEncounter->text && !pendingEncounter->quest)
	{
		std::string line = "visit: { hero: " + pendingEncounter->hero + ", object: " + pendingEncounter->object;
		appendApproach(line);
		appendDiscoveries(line);
		line += " }";
		writeActionLine(gameState, line);
		pendingEncounter.reset();
		return;
	}

	std::string line = "encounter: { ";
	if(!currentTurnPlayer || pendingEncounter->actor != currentTurnPlayer->toString())
		line += "actor: " + pendingEncounter->actor + ", ";
	line += "hero: " + pendingEncounter->hero + ", with: " + pendingEncounter->object;
	appendApproach(line);
	appendDiscoveries(line);
	if(pendingEncounter->quest)
		line += ", quest: " + *pendingEncounter->quest;
	if(pendingEncounter->answered)
	{
		const int32_t answer = pendingEncounter->answer.value_or(0);
		std::string choiceName;
		if(pendingEncounter->battleFollows && pendingEncounter->object.starts_with("monster/"))
			choiceName = "fight";
		else if(pendingEncounter->selection)
			choiceName = "option" + std::to_string(answer);
		else if(answer == 0)
			choiceName = pendingEncounter->cancel ? "cancel" : "decline";
		else if(answer == 1)
			choiceName = "accept";
		else
			choiceName = "answer" + std::to_string(answer);
		line += ", choice: { name: " + choiceName + ", value: " +
			(pendingEncounter->answer ? std::to_string(answer) : "null") + " }";
	}
	if(pendingEncounter->text)
		line += ", text: " + yamlString(*pendingEncounter->text);
	if(!pendingEncounter->outcomes.empty())
	{
		std::vector<std::string> outcomes;
		for(const auto & outcome : pendingEncounter->outcomes)
			outcomes.push_back("{ " + outcome + " }");
		line += ", outcome: " + flowList(outcomes);
	}
	line += " }";
	pendingEncounter.reset();
	writeActionLine(gameState, line);
}

void VGTRecorder::writeBaselineSave(CGameHandler & gameHandler)
{
	if(!baselineSaveEnabled && !baselineGameStateSaveEnabled)
		return;

	if(baselineSaveEnabled)
	{
		try
		{
			const boost::filesystem::path targetPath(baselineSavePath);
			boost::filesystem::path temporaryPath = targetPath;
			temporaryPath += ".tmp";

			gameHandler.saveToFile(temporaryPath.string());
			if(boost::filesystem::exists(targetPath))
				boost::filesystem::remove(targetPath);
			boost::filesystem::rename(temporaryPath, targetPath);
			baselineSaveEnabled = false;
		}
		catch(const std::exception & e)
		{
			logGlobal->error("Unable to write VGT baseline save '%s': %s", baselineSavePath, e.what());
		}
	}

	if(baselineGameStateSaveEnabled)
	{
		try
		{
			const boost::filesystem::path targetPath(baselineGameStateSavePath);
			boost::filesystem::path temporaryPath = targetPath;
			temporaryPath += ".tmp";

			CSaveFile save;
			gameHandler.gameState().saveGame(save);
			save.write(temporaryPath);
			if(boost::filesystem::exists(targetPath))
				boost::filesystem::remove(targetPath);
			boost::filesystem::rename(temporaryPath, targetPath);
			baselineGameStateSaveEnabled = false;
		}
		catch(const std::exception & e)
		{
			logGlobal->error("Unable to write VGT baseline game state save '%s': %s", baselineGameStateSavePath, e.what());
		}
	}
}

void VGTRecorder::writeTurnState(CGameHandler & gameHandler)
{
	if(!turnStateArchiveEnabled || !pendingTurnStatePlayer)
		return;

	try
	{
		++archivedTurnStates;
		std::ostringstream fileName;
		fileName << "turn-" << std::setfill('0') << std::setw(6) << archivedTurnStates
			<< "-day-" << std::setw(4) << gameHandler.gameState().getCalendar().getCurrentDay()
			<< "-" << pendingTurnStatePlayer->toString() << ".vsgm1";

		const boost::filesystem::path targetPath = boost::filesystem::path(turnStateDirectory) / fileName.str();
		boost::filesystem::path temporaryPath = targetPath;
		temporaryPath += ".tmp";

		gameHandler.saveToFile(temporaryPath.string());
		if(boost::filesystem::exists(targetPath))
			boost::filesystem::remove(targetPath);
		boost::filesystem::rename(temporaryPath, targetPath);
		logGlobal->info("Wrote VGT turn state '%s'", targetPath.string());
	}
	catch(const std::exception & e)
	{
		logGlobal->error("Unable to write VGT turn state: %s", e.what());
	}

	pendingTurnStatePlayer.reset();
}

void VGTRecorder::recordDecision(CGameHandler & gameHandler, CPackForServer & pack)
{
	std::scoped_lock lock(outputMutex);
	const CGameState & gameState = gameHandler.gameState();
	initializeFromEnvironment();
	if(!enabled)
		return;
	if(!dynamic_cast<ExchangeArtifacts *>(&pack))
		flushPendingArtifactTransfer(gameState);
	const bool arrangesArmy = dynamic_cast<ArrangeStacks *>(&pack) || dynamic_cast<BulkMoveArmy *>(&pack) ||
		dynamic_cast<BulkSplitStack *>(&pack) || dynamic_cast<BulkMergeStacks *>(&pack) ||
		dynamic_cast<BulkSplitAndRebalanceStack *>(&pack);
	if(!arrangesArmy)
		flushPendingArmyArrangement(gameState);

	ensureHeader(gameState);
	if(!enabled)
		return;
	if(pendingBattle && pendingBattle->ended && !pendingBattle->aftermathDecisionStarted && gameHandler.randomizer)
		pendingBattle->continuation = singleLineJson(gameHandler.randomizer->toVGTBattleJson(
			pendingBattle->randomizerParticipants, pendingBattle->randomizerHeroes));
	if(dynamic_cast<QueryReply *>(&pack))
	{
		// Buffered transactions happened before the query was closed. Emit them now
		// so recruitment/trading never appears after the finish/answer that ended
		// its visit or marketplace context.
		flushPendingRecruit(gameState);
		flushPendingTrade(gameState);
	}
	if(auto * reply = dynamic_cast<QueryReply *>(&pack);
		reply && pendingEncounter && pendingEncounter->query == reply->qid.getNum())
	{
		if(pendingBattle && pendingBattle->ended)
		{
			pendingBattle->aftermathDecisionStarted = true;
			pendingBattle->armyDecisionStarted = true;
		}
		pendingEncounter->answered = true;
		pendingEncounter->answer = reply->reply;
		return;
	}
	if(auto * reply = dynamic_cast<QueryReply *>(&pack); reply)
	{
		const auto pending = pendingQueries.find(reply->qid.getNum());
		if(pending != pendingQueries.end())
		{
			if(pending->second.kind == "battleChoice" && pendingBattle)
			{
				pendingBattle->aftermathDecisionStarted = true;
				pendingBattle->armyDecisionStarted = true;
				const int answer = reply->reply.value_or(0);
				std::string name;
				if(pending->second.selection)
					name = "option" + std::to_string(answer);
				else if(answer == 0)
					name = pending->second.cancel ? "cancel" : "decline";
				else if(answer == 1)
					name = "accept";
				else
					name = "answer" + std::to_string(answer);
				pendingBattle->aftermath.push_back("answer: { name: " + name + ", value: " +
					(reply->reply ? std::to_string(answer) : "null") + " }");
				pendingQueries.erase(pending);
				return;
			}
			if(pending->second.kind == "levelUp" && pendingBattle)
			{
				pendingBattle->aftermathDecisionStarted = true;
				if(reply->reply && *reply->reply >= 0 &&
					static_cast<size_t>(*reply->reply) < pending->second.choices.size())
				{
					pendingBattle->aftermath.push_back("chooseSkill: { hero: " + pending->second.subject +
						", skill: " + pending->second.choices[*reply->reply] + " }");
					suppressDerivedEffects = true;
				}
				pendingQueries.erase(pending);
				return;
			}
			flushPendingEncounter(gameState);
			if(pendingBattle)
				flushPendingBattle(gameState);
			if(pending->second.kind == "levelUp" && reply->reply && *reply->reply >= 0 &&
				static_cast<size_t>(*reply->reply) < pending->second.choices.size())
			{
				writeActionLine(gameState, "chooseSkill: { hero: " + pending->second.subject +
					", skill: " + pending->second.choices[*reply->reply] + " }");
				suppressDerivedEffects = true;
			}
			else if(pending->second.kind == "window")
				writeActionLine(gameState, "finish: { activity: " + pending->second.subject + " }");
			else
			{
				std::string line = "answer: { value: ";
				line += reply->reply ? std::to_string(*reply->reply) : "null";
				line += " }";
				writeActionLine(gameState, line);
			}
			pendingQueries.erase(pending);
			return;
		}
	}
	flushPendingEncounter(gameState);
	if(auto * reply = dynamic_cast<QueryReply *>(&pack); reply)
	{
		std::string line = "answer: { value: ";
		line += reply->reply ? std::to_string(*reply->reply) : "null";
		line += " }";
		writeActionLine(gameState, line);
		return;
	}
	if(!dynamic_cast<MakeAction *>(&pack))
		flushPendingBattle(gameState);
	suppressDerivedEffects = false;
	if(!dynamic_cast<TradeOnMarketplace *>(&pack))
		flushPendingTrade(gameState);
	if(auto * move = dynamic_cast<MoveHero *>(&pack); move && !move->path.empty())
	{
		flushPendingRecruit(gameState);
		const std::string actor = actorForPlayer(move->player);
		const std::string hero = heroAlias(gameState, move->hid);
		const int routeZ = move->path.front().z;
		const bool sameLevel = std::all_of(move->path.begin(), move->path.end(), [routeZ](const int3 & destination)
		{
			return destination.z == routeZ;
		});
		if(!sameLevel)
		{
			flushPendingMove(gameState);
			writeActionLine(gameState, "unmodelled: { stream: decision, pack: MoveHero, material: true }");
			return;
		}
		if(pendingMove && (pendingMove->actor != actor || pendingMove->hero != hero || pendingMove->z != routeZ || pendingMove->transit != move->transit))
			flushPendingMove(gameState);
		for(const auto & destination : move->path)
		{
			if(!pendingMove)
			{
				std::array<int, 3> start = {destination.x, destination.y, destination.z};
				if(const auto * heroObject = gameState.getMap().getObject(move->hid))
				{
					const auto position = heroObject->anchorPos();
					start = {position.x, position.y, position.z};
				}
				pendingMove = PendingMove{actor, hero, start, {}, {}, routeZ, move->transit};
			}
			pendingMove->route.push_back({destination.x, destination.y, destination.z});
		}
		return;
	}
	flushPendingMove(gameState);

	if(auto * recruitPack = dynamic_cast<RecruitCreatures *>(&pack))
	{
		const std::string actor = actorForPlayer(recruitPack->player);
		const std::string source = objectAlias(gameState, recruitPack->tid);
		const std::string destination = objectAlias(gameState, recruitPack->dst);
		if(pendingRecruit && (pendingRecruit->actor != actor || pendingRecruit->source != source || pendingRecruit->destination != destination))
			flushPendingRecruit(gameState);
		if(!pendingRecruit)
			pendingRecruit = PendingRecruit{actor, source, destination, {}, {}, {}};

		int destinationSlot = -1;
		if(const auto * army = dynamic_cast<const CArmedInstance *>(gameState.getMap().getObject(recruitPack->dst)))
			destinationSlot = army->getSlotFor(recruitPack->crid).getNum();
		std::optional<std::string> poolName;
		if(const auto * dwelling = dynamic_cast<const CGDwelling *>(gameState.getMap().getObject(recruitPack->tid));
			dwelling && recruitPack->level >= 0 && static_cast<size_t>(recruitPack->level) < dwelling->creatures.size())
		{
			const auto matchingPools = std::count_if(dwelling->creatures.begin(), dwelling->creatures.end(), [&](const auto & pool)
			{
				return pool.first >= recruitPack->amount &&
					std::find(pool.second.begin(), pool.second.end(), recruitPack->crid) != pool.second.end();
			});
			if(matchingPools > 1)
			{
				if(const auto * town = dynamic_cast<const CGTownInstance *>(dwelling);
					static_cast<size_t>(recruitPack->level) == town->getTown()->creatures.size())
					poolName = "portalOfSummoning";
				else
					poolName = "level-" + std::to_string(recruitPack->level + 1);
			}
		}
		pendingRecruit->units.push_back({creature(recruitPack->crid), recruitPack->amount, destinationSlot, poolName});

		if(const auto * recruited = recruitPack->crid.toCreature())
		{
			const auto cost = recruited->getFullRecruitCost() * static_cast<TResource>(recruitPack->amount);
			for(size_t index = 0; index < cost.size(); ++index)
			{
				if(cost[index] != 0)
					pendingRecruit->paid[GameResID::encode(static_cast<int>(index))] += cost[index];
			}
		}
		if(const auto * dwelling = dynamic_cast<const CGDwelling *>(gameState.getMap().getObject(recruitPack->tid));
			dwelling && recruitPack->level >= 0 && static_cast<size_t>(recruitPack->level) < dwelling->creatures.size())
		{
			const auto & pool = dwelling->creatures[recruitPack->level];
			for(const auto creatureID : pool.second)
			{
				const auto remaining = std::max<int64_t>(0, static_cast<int64_t>(pool.first) - recruitPack->amount);
				if(remaining > 0)
					pendingRecruit->remaining[transcriptIdentifier(CreatureID::encode(creatureID.getNum()))] = remaining;
			}
		}
		suppressDerivedEffects = true;
		return;
	}

	flushPendingRecruit(gameState);
	if(auto * trade = dynamic_cast<TradeOnMarketplace *>(&pack);
		trade && trade->mode == EMarketMode::RESOURCE_RESOURCE)
	{
		const std::string actor = actorForPlayer(trade->player);
		const std::string marketAlias = objectAlias(gameState, trade->marketId);
		if(pendingTrade && (pendingTrade->actor != actor || pendingTrade->market != marketAlias))
			flushPendingTrade(gameState);
		if(!pendingTrade)
			pendingTrade = PendingTrade{actor, marketAlias, {}};

		const auto * market = gameState.getMarket(trade->marketId);
		if(!market || trade->r1.size() != trade->r2.size() || trade->r1.size() != trade->val.size())
		{
			flushPendingTrade(gameState);
			writeActionLine(gameState, "unmodelled: { stream: decision, pack: TradeOnMarketplace, material: true }");
			return;
		}
		for(size_t index = 0; index < trade->val.size(); ++index)
		{
			const auto sold = trade->r1[index].as<GameResID>();
			const auto bought = trade->r2[index].as<GameResID>();
			int offerSold = 0;
			int offerBought = 0;
			if(!market->getOffer(sold, bought, offerSold, offerBought, EMarketMode::RESOURCE_RESOURCE) || offerSold <= 0)
			{
				flushPendingTrade(gameState);
				writeActionLine(gameState, "unmodelled: { stream: decision, pack: TradeOnMarketplace, material: true }");
				return;
			}
			pendingTrade->exchanges.push_back({
				GameResID::encode(sold.getNum()),
				trade->val[index],
				GameResID::encode(bought.getNum()),
				static_cast<uint32_t>(trade->val[index] / offerSold * offerBought)});
		}
		suppressDerivedEffects = true;
		return;
	}
	if(auto * trade = dynamic_cast<TradeOnMarketplace *>(&pack))
	{
		flushPendingTrade(gameState);
		if(const auto line = semanticMarketTrade(gameState, *trade))
		{
			writeActionLine(gameState, *line);
			suppressDerivedEffects = true;
		}
		else
			writeActionLine(gameState, "unmodelled: { stream: decision, pack: TradeOnMarketplace, material: true }");
		return;
	}

	if(auto * hire = dynamic_cast<HireHero *>(&pack))
	{
		std::string line = "hire: { ";
		const auto actor = actorForPlayer(hire->player);
		if(!currentTurnPlayer || actor != currentTurnPlayer->toString())
			line += "actor: " + actor + ", ";
		line += "at: " + objectAlias(gameState, hire->tid) +
			", hero: " + heroType(hire->hid) +
			", paid: { gold: " + std::to_string(GameConstants::HERO_GOLD_COST) + " }";
		if(hire->nhid != HeroTypeID::NONE)
			line += ", replacement: " + heroType(hire->nhid);
		line += " }";
		writeActionLine(gameState, line);
		suppressDerivedEffects = true;
		return;
	}

	if(auto * buildPack = dynamic_cast<BuildStructure *>(&pack))
	{
		std::string line = "build: { ";
		const auto actor = actorForPlayer(buildPack->player);
		if(!currentTurnPlayer || actor != currentTurnPlayer->toString())
			line += "actor: " + actor + ", ";
		line += "town: " + objectAlias(gameState, buildPack->tid) +
			", building: " + building(buildPack->bid);
		if(const auto * town = dynamic_cast<const CGTownInstance *>(gameState.getMap().getObject(buildPack->tid)))
			line += ", cost: " + resourceMap(town->getBuildingCost(buildPack->bid));
		line += " }";
		writeActionLine(gameState, line);
		suppressDerivedEffects = true;
		return;
	}

	if(auto * moveArtifact = dynamic_cast<ExchangeArtifacts *>(&pack);
		moveArtifact && !moveArtifact->src.creature && !moveArtifact->dst.creature)
	{
		const auto * source = gameState.getArtSet(moveArtifact->src);
		const auto * instance = source ? source->getArt(moveArtifact->src.slot) : nullptr;
		if(instance)
		{
			const std::string actor = actorForPlayer(moveArtifact->player);
			const std::string from = objectAlias(gameState, moveArtifact->src.artHolder);
			const std::string to = objectAlias(gameState, moveArtifact->dst.artHolder);
			if(pendingArtifactTransfer && (pendingArtifactTransfer->actor != actor ||
				pendingArtifactTransfer->from != from || pendingArtifactTransfer->to != to))
				flushPendingArtifactTransfer(gameState);
			if(!pendingArtifactTransfer)
				pendingArtifactTransfer = PendingArtifactTransfer{actor, from, to, {}};
			pendingArtifactTransfer->moves.push_back({
				artifact(instance->getTypeId()),
				artifactPosition(moveArtifact->src.slot),
				artifactPosition(moveArtifact->dst.slot)});
			suppressDerivedEffects = true;
			return;
		}
	}

	if(arrangesArmy)
	{
		const std::string actor = actorForPlayer(pack.player);
		if(pendingArmyArrangement && pendingArmyArrangement->actor != actor)
			flushPendingArmyArrangement(gameState);
		if(!pendingArmyArrangement)
			pendingArmyArrangement = PendingArmyArrangement{actor, {}, {}};
		auto trackArmy = [&](ObjectInstanceID armyID)
		{
			pendingArmyArrangement->initialUndeadMoraleBonuses.try_emplace(
				armyID, undeadMoraleBonus(gameState, armyID));
			pendingArmyArrangement->armies.insert(armyID);
		};
		if(const auto * arrangement = dynamic_cast<ArrangeStacks *>(&pack))
		{
			trackArmy(arrangement->id1);
			trackArmy(arrangement->id2);
		}
		else if(const auto * movement = dynamic_cast<BulkMoveArmy *>(&pack))
		{
			trackArmy(movement->srcArmy);
			trackArmy(movement->destArmy);
		}
		else if(const auto * split = dynamic_cast<BulkSplitStack *>(&pack))
			trackArmy(split->srcOwner);
		else if(const auto * merge = dynamic_cast<BulkMergeStacks *>(&pack))
			trackArmy(merge->srcOwner);
		else if(const auto * rebalance = dynamic_cast<BulkSplitAndRebalanceStack *>(&pack))
			trackArmy(rebalance->srcOwner);
		return;
	}

	DecisionRecorder recorder(gameState);
	pack.visit(recorder);
	auto * battleActionPack = dynamic_cast<MakeAction *>(&pack);
	if(battleActionPack)
		lastBattleDecisions[battleAlias(battleActionPack->battleID)] = battleAction(
			gameState, battleActionPack->battleID, battleActionPack->ba);
	writeActionLine(gameState, readableDecisionRecord(recorder.result(), currentTurnPlayer));
	if(battleActionPack && pendingBattle && pendingBattle->initialRandom.empty() && gameHandler.randomizer)
		pendingBattle->initialRandom = singleLineJson(gameHandler.randomizer->toVGTBattleJson(
			battleRandomizerParticipants(gameState, battleActionPack->battleID),
			battleRandomizerHeroes(gameState, battleActionPack->battleID)));
	if(dynamic_cast<ExchangeArtifacts *>(&pack) || dynamic_cast<BulkExchangeArtifacts *>(&pack) ||
		dynamic_cast<ManageBackpackArtifacts *>(&pack) || dynamic_cast<ManageEquippedArtifacts *>(&pack) ||
		dynamic_cast<AssembleArtifacts *>(&pack) || dynamic_cast<EraseArtifactByClient *>(&pack) ||
		dynamic_cast<BuyArtifact *>(&pack))
		suppressDerivedEffects = true;
}

void VGTRecorder::recordTimerEndTurn(const CGameState & gameState, PlayerColor player)
{
	std::scoped_lock lock(outputMutex);
	initializeFromEnvironment();
	if(!enabled)
		return;

	ensureHeader(gameState);
	if(!enabled)
		return;

	flushPendingMove(gameState);
	flushPendingRecruit(gameState);
	flushPendingTrade(gameState);
	flushPendingBattle(gameState);
	writeActionLine(gameState, "endTurn: { actor: timer/" + color(player) + " }");
}

void VGTRecorder::recordTimerBattleAction(const CGameState & gameState, PlayerColor player, BattleID battleID, const BattleAction & action)
{
	std::scoped_lock lock(outputMutex);
	initializeFromEnvironment();
	if(!enabled)
		return;

	ensureHeader(gameState);
	if(!enabled)
		return;

	flushPendingMove(gameState);
	flushPendingRecruit(gameState);
	flushPendingTrade(gameState);
	lastBattleDecisions[battleAlias(battleID)] = battleAction(gameState, battleID, action);
	writeActionLine(
		gameState,
		"battleAction: { actor: timer/" + color(player) +
			", battle: " + battleAlias(battleID) +
			", action: " + battleAction(gameState, battleID, action) + " }");
}

void VGTRecorder::recordEffect(CGameHandler & gameHandler, CPackForClient & pack)
{
	std::scoped_lock lock(outputMutex);
	const CGameState & gameState = gameHandler.gameState();
	initializeFromEnvironment();
	if(auto * end = dynamic_cast<PlayerEndsTurn *>(&pack); end && turnStateArchiveEnabled)
		pendingTurnStatePlayer = end->player;
	if(!enabled)
		return;

	ensureHeader(gameState);
	if(!enabled)
		return;
	if(auto * fog = dynamic_cast<FoWChange *>(&pack);
		fog && fog->mode == ETileVisibility::REVEALED && fog->player.isValidPlayer())
	{
		discoveryTracker.discoverInTiles(gameState, fog->player, fog->tiles);
	}
	if(pendingBattle && pendingBattle->ended && !pendingBattle->aftermathDecisionStarted &&
		!dynamic_cast<BattleEnded *>(&pack) && gameHandler.randomizer)
		pendingBattle->continuation = singleLineJson(gameHandler.randomizer->toVGTBattleJson(
			pendingBattle->randomizerParticipants, pendingBattle->randomizerHeroes));
	if(dynamic_cast<PlayerStartsTurn *>(&pack))
		betweenPlayerTurns = false;
	if(auto * visit = dynamic_cast<HeroVisit *>(&pack))
	{
		const auto * town = dynamic_cast<const CGTownInstance *>(gameState.getMap().getObject(visit->objId));
		const auto * hero = gameState.getHero(visit->heroId);
		const bool belongsToAnotherTurn = currentTurnPlayer && hero && hero->tempOwner != *currentTurnPlayer;
		if(town && (betweenPlayerTurns || belongsToAnotherTurn))
			return;
	}
	auto * battleStart = dynamic_cast<BattleStart *>(&pack);
	if(battleStart)
	{
		if(battleStart->info)
			resetBattleUnitAliases(*battleStart->info, battleStart->battleID);
		else
			resetBattleUnitAliases(gameState, battleStart->battleID);
	}
	if(auto * changed = dynamic_cast<BattleUnitsChanged *>(&pack))
	{
		for(const auto & change : changed->changedStacks)
		{
			if(change.operation != BattleChanges::EOperation::ADD)
				continue;
			battle::UnitInfo info;
			info.load(change.id, change.data);
			const auto alias = registerBattleUnitAlias(changed->battleID, static_cast<int>(change.id), info.side, info.type);
			if(pendingBattle && pendingBattle->id == battleAlias(changed->battleID))
			{
				const auto * battle = gameState.getBattle(changed->battleID);
				const auto owner = battle ? battle->getSidePlayer(info.side) : PlayerColor::NEUTRAL;
				pendingBattle->units.push_back(yamlKey(alias) + ": { stack: " + std::to_string(change.id) +
					", owner: " + color(owner) + ", count: " + std::to_string(info.count) + " }");
			}
		}
	}
	if(auto * startAction = dynamic_cast<StartAction *>(&pack))
	{
		const std::string id = battleAlias(startAction->battleID);
		const std::string accepted = battleAction(gameState, startAction->battleID, startAction->ba);
		const auto requested = lastBattleDecisions.find(id);
		if(requested != lastBattleDecisions.end() && requested->second == accepted)
		{
			lastBattleDecisions.erase(requested);
			return;
		}
		lastBattleDecisions.erase(id);
		writeActionLine(gameState, "battle: { id: " + id + ", event: acceptedAs, action: " + accepted + " }");
		return;
	}
	if(auto * mana = dynamic_cast<SetMana *>(&pack); mana && pendingBattle)
	{
		int64_t change = mana->val;
		if(mana->mode == ChangeValueMode::ABSOLUTE)
		{
			const auto * hero = gameState.getHero(mana->hid);
			change -= hero ? hero->mana : 0;
		}
		const std::string hero = heroAlias(gameState, mana->hid);
		auto & total = pendingBattle->manaChanges[hero];
		total += change;
		if(total == 0)
			pendingBattle->manaChanges.erase(hero);
		if(pendingBattle->ended)
			return;
	}
	if(auto * result = dynamic_cast<BattleResult *>(&pack); result && pendingBattle)
	{
		pendingBattle->manaChanges.clear();
		if(const auto * battle = gameState.getBattle(result->battleID))
		{
			for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
			{
				const auto & battleSide = battle->getSide(side);
				const auto * hero = battle->getSideHero(side);
				if(!hero)
					continue;
				const int64_t change = std::min(hero->mana, battleSide.initialMana) - battleSide.initialMana;
				if(change != 0)
					pendingBattle->manaChanges[heroAlias(gameState, hero->id)] = change;
			}
		}
		pendingBattle->survivors = battleSurvivors(gameState, result->battleID);
		pendingBattle->createdUnits = battleCreatedUnits(gameState, result->battleID);
		if(gameHandler.randomizer)
			pendingBattle->randomBeforeAftermath = singleLineJson(gameHandler.randomizer->toVGTBattleJson(
				pendingBattle->randomizerParticipants, pendingBattle->randomizerHeroes));
		pendingBattle->outcome.push_back("result: " + battleResult(result->result));
		pendingBattle->outcome.push_back("winnerSide: " + battleSide(result->winner));
		std::vector<std::string> casualties;
		for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
		{
			const auto & values = result->casualties[side];
			if(!values.empty())
				casualties.push_back(battleSide(side) + ": " + battleCasualties(values));
		}
		pendingBattle->outcome.push_back("casualties: { " + boost::algorithm::join(casualties, ", ") + " }");
		return;
	}
	if(auto * accepted = dynamic_cast<BattleResultAccepted *>(&pack); accepted && pendingBattle)
	{
		std::vector<std::string> experience;
		for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
		{
			const auto & heroResult = accepted->heroResult[side];
			if(heroResult.exp != 0 && heroResult.heroID != ObjectInstanceID::NONE)
				experience.push_back(heroAlias(gameState, heroResult.heroID) + ": " + std::to_string(heroResult.exp));
		}
		if(!experience.empty())
			pendingBattle->outcome.push_back("experience: { " + boost::algorithm::join(experience, ", ") + " }");
		return;
	}
	if(auto * applied = dynamic_cast<BattleResultsApplied *>(&pack); applied && pendingBattle)
	{
		pendingBattle->outcome.push_back("winner: " + color(applied->victor));
		pendingBattle->outcome.push_back("loser: " + color(applied->loser));
		if(!applied->movingArtifacts.empty())
			pendingBattle->outcome.push_back("artifactMoves: " + bulkArtifactMoves(gameState, applied->movingArtifacts));
		if(!applied->learnedSpells.spells.empty())
			pendingBattle->outcome.push_back("learnedSpells: { hero: " +
				heroAlias(gameState, applied->learnedSpells.hid) + ", spells: " +
				spellsList(applied->learnedSpells.spells) + " }");
		if(!applied->growingArtifacts.empty())
			pendingBattle->outcome.push_back("grownArtifacts: " + std::to_string(applied->growingArtifacts.size()));
		if(!applied->dischargingArtifacts.empty())
			pendingBattle->outcome.push_back("dischargedArtifacts: " + std::to_string(applied->dischargingArtifacts.size()));
		if(applied->raisedStack.getId() != CreatureID::NONE && applied->raisedStack.getCount() > 0)
			pendingBattle->outcome.push_back("raised: { creature: " + creature(applied->raisedStack.getId()) +
				", count: " + std::to_string(applied->raisedStack.getCount()) + " }");
		return;
	}
	if(auto * ended = dynamic_cast<BattleEnded *>(&pack); ended && pendingBattle)
	{
		if(gameHandler.randomizer)
		{
			if(pendingBattle->randomizerParticipants.empty())
				pendingBattle->randomizerParticipants = battleRandomizerParticipants(gameState, ended->battleID);
			if(pendingBattle->randomizerHeroes.empty())
				pendingBattle->randomizerHeroes = battleRandomizerHeroes(gameState, ended->battleID);
			pendingBattle->continuation = singleLineJson(gameHandler.randomizer->toVGTBattleJson(
				pendingBattle->randomizerParticipants, pendingBattle->randomizerHeroes));
		}
		if(std::none_of(pendingBattle->outcome.begin(), pendingBattle->outcome.end(), [](const std::string & field)
		{
			return field.starts_with("winner: ");
		}))
			pendingBattle->outcome.push_back("winner: " + color(ended->victor));
		if(std::none_of(pendingBattle->outcome.begin(), pendingBattle->outcome.end(), [](const std::string & field)
		{
			return field.starts_with("loser: ");
		}))
			pendingBattle->outcome.push_back("loser: " + color(ended->loser));
		capturePendingBattleArmies(gameState);
		pendingBattle->ended = true;
		return;
	}

	if(dynamic_cast<BattleStart *>(&pack) && pendingEncounter)
	{
		pendingEncounter->battleFollows = true;
		flushPendingEncounter(gameState);
	}
	if(auto * visit = dynamic_cast<HeroVisit *>(&pack); visit && visit->starting)
	{
		std::optional<PendingMove> approach;
		const std::string visitingHero = heroAlias(gameState, visit->heroId);
		if(pendingMove && pendingMove->hero == visitingHero && !pendingMove->transit)
		{
			const bool encodable = pendingMove->route.size() == 1 || encodedDirections(pendingMove->start, pendingMove->route).has_value();
			if(encodable)
			{
				approach = std::move(pendingMove);
				pendingMove.reset();
			}
			else
				flushPendingMove(gameState);
		}
		else
			flushPendingMove(gameState);
		flushPendingEncounter(gameState);
		PendingEncounter encounter;
		encounter.hero = visitingHero;
		encounter.heroID = visit->heroId;
		encounter.object = objectAlias(gameState, visit->objId);
		if(const auto * hero = gameState.getHero(visit->heroId))
			encounter.actor = actorForPlayer(hero->tempOwner);
		else
			encounter.actor = currentTurnPlayer ? currentTurnPlayer->toString() : "world";
		encounter.approach = std::move(approach);
		pendingEncounter = std::move(encounter);
		return;
	}
	if(auto * teleport = dynamic_cast<TeleportDialog *>(&pack); teleport && pendingEncounter)
	{
		pendingEncounter->query = teleport->queryID.getNum();
		pendingEncounter->teleport = true;
		pendingEncounter->impassable = teleport->impassable;
		if(const auto * hero = gameState.getHero(teleport->hero))
		{
			const auto position = hero->visitablePos();
			pendingEncounter->teleportStart = {position.x, position.y, position.z};
		}
		for(const auto & [object, position] : teleport->exits)
		{
			pendingEncounter->teleportExits.push_back({
				objectAlias(gameState, object), {position.x, position.y, position.z}});
		}
		return;
	}
	if(auto * window = dynamic_cast<OpenWindow *>(&pack))
	{
		const std::string activity = openWindowMode(window->window);
		if(window->queryID != QueryID::NONE)
			pendingQueries[window->queryID.getNum()] = PendingQuery{"window", activity, {}};
		const bool generatedRecruitment = activity == "recruitmentFirst" || activity == "recruitmentAll";
		if(pendingEncounter && !generatedRecruitment)
			pendingEncounter->outcomes.push_back("opened: { activity: " + activity + " }");
		else if(!pendingEncounter && !generatedRecruitment)
			writeActionLine(gameState, "opened: { activity: " + activity +
				", object: " + objectAlias(gameState, window->object) +
				", hero: " + heroAlias(gameState, window->visitor) + " }");
		return;
	}
	if(auto * exchange = dynamic_cast<ExchangeDialog *>(&pack))
	{
		if(exchange->queryID != QueryID::NONE)
			pendingQueries[exchange->queryID.getNum()] = PendingQuery{"window", "exchange", {}};
		const std::string opened = "opened: { activity: exchange, with: " + heroAlias(gameState, exchange->hero2) + " }";
		if(pendingEncounter)
			pendingEncounter->outcomes.push_back(opened);
		else
			writeActionLine(gameState, opened);
		return;
	}
	if(auto * garrison = dynamic_cast<GarrisonDialog *>(&pack))
	{
		if(garrison->queryID != QueryID::NONE)
			pendingQueries[garrison->queryID.getNum()] = PendingQuery{"window", "garrison", {}};
		const std::string opened = "opened: { activity: garrison, at: " + objectAlias(gameState, garrison->objid) + " }";
		if(pendingEncounter)
			pendingEncounter->outcomes.push_back(opened);
		else
			writeActionLine(gameState, opened);
		return;
	}
	if(auto * level = dynamic_cast<HeroLevelUp *>(&pack))
	{
		std::vector<std::string> choices;
		for(const auto & skill : level->skills)
			choices.push_back(secondarySkill(skill));
		if(level->queryID != QueryID::NONE)
			pendingQueries[level->queryID.getNum()] = PendingQuery{
				"levelUp", heroAlias(gameState, level->heroId), choices};
		std::string line = "levelUp: { hero: " + heroAlias(gameState, level->heroId) +
			", primary: " + primarySkill(level->primskill);
		if(!choices.empty())
			line += ", choices: " + flowList(choices);
		line += " }";
		if(pendingEncounter)
			pendingEncounter->outcomes.push_back(line);
		else if(pendingBattle)
			pendingBattle->aftermath.push_back(line);
		else
			writeActionLine(gameState, line);
		return;
	}
	if(auto * addedQuest = dynamic_cast<AddQuest *>(&pack); addedQuest && pendingEncounter &&
		objectAlias(gameState, addedQuest->quest.obj) == pendingEncounter->object)
	{
		const auto * questObject = dynamic_cast<const IQuestObject *>(gameState.getObjInstance(addedQuest->quest.obj));
		if(questObject && !questObject->getQuest().mission.artifacts.empty())
		{
			std::vector<std::string> requestedArtifacts;
			for(const auto artifactID : questObject->getQuest().mission.artifacts)
				requestedArtifacts.push_back(artifact(artifactID));
			const std::string request = requestedArtifacts.size() == 1
				? requestedArtifacts.front()
				: flowList(requestedArtifacts);
			pendingEncounter->quest = "{ bring: " + request + " }";
			pendingEncounter->standardQuestText = !questObject->getQuest().isCustomFirst;
			return;
		}
	}
	if(auto * dialog = dynamic_cast<BlockingDialog *>(&pack); dialog && pendingEncounter)
	{
		pendingEncounter->query = dialog->queryID.getNum();
		pendingEncounter->selection = dialog->selection();
		pendingEncounter->cancel = dialog->cancel();
		const bool standardMonsterPrompt = pendingEncounter->object.starts_with("monster/");
		if(dialog->text.hasCustomText() && !standardMonsterPrompt)
			pendingEncounter->text = dialog->text.toString();
		return;
	}
	if(auto * dialog = dynamic_cast<BlockingDialog *>(&pack); dialog && pendingBattle && pendingBattle->ended)
	{
		if(dialog->queryID != QueryID::NONE)
			pendingQueries[dialog->queryID.getNum()] = PendingQuery{
				"battleChoice", {}, {}, dialog->selection(), dialog->cancel()};
		return;
	}
	if(auto * info = dynamic_cast<InfoWindow *>(&pack))
	{
		const bool standardShrineText = pendingEncounter &&
			pendingEncounter->object.starts_with("shrine-of-magic-");
		const bool standardQuestText = pendingEncounter && pendingEncounter->standardQuestText;
		if(info->text.hasCustomText() && !standardShrineText && !standardQuestText)
		{
			if(pendingEncounter)
				pendingEncounter->text = info->text.toString();
			else
				writeActionLine(gameState, "story: { text: " + yamlString(info->text.toString()) + " }");
		}
		return;
	}
	if(auto * fog = dynamic_cast<FoWChange *>(&pack); fog && pendingEncounter && visibility(fog->mode) == "revealed")
	{
		const bool alreadyRecorded = std::any_of(
			pendingEncounter->outcomes.begin(), pendingEncounter->outcomes.end(), [](const std::string & outcome)
			{
				return outcome.starts_with("reveal: ");
			});
		if(!alreadyRecorded)
		{
			std::string reveal = "reveal: { by: " + pendingEncounter->object;
			if(color(fog->player) != pendingEncounter->actor)
				reveal += ", player: " + color(fog->player);
			pendingEncounter->outcomes.push_back(reveal + " }");
		}
		return;
	}
	if(auto * move = dynamic_cast<TryMoveHero *>(&pack))
	{
		collectDiscoveries(gameState, *move);
		return;
	}
	if(auto * removed = dynamic_cast<RemoveObject *>(&pack); pendingBattle && pendingBattle->ended &&
		removed && objectAlias(gameState, removed->objectID) == pendingBattle->defender)
	{
		return;
	}

	const bool documentBoundary = dynamic_cast<PlayerStartsTurn *>(&pack) ||
		dynamic_cast<PlayerEndsTurn *>(&pack) || dynamic_cast<NewTurn *>(&pack);
	if(documentBoundary)
	{
		flushPendingInitialAvailability(gameState);
		flushPendingEncounter(gameState);
		flushPendingTrade(gameState);
		flushPendingArtifactTransfer(gameState);
		flushPendingArmyArrangement(gameState);
		flushPendingBattle(gameState);
	}
	if(auto * availability = dynamic_cast<SetAvailableCreatures *>(&pack); availability && !currentTurnPlayer)
	{
		if(gameState.getCalendar().getCurrentDay() == 1)
		{
			collectInitialAvailability(gameState, *availability);
			return;
		}
		if(isStandardDwellingRefresh(gameState, *availability))
			return;
		collectWeeklyAvailability(gameState, *availability);
		return;
	}
	const bool battleArmyChange =
		dynamic_cast<ChangeStackCount *>(&pack) ||
		dynamic_cast<SetStackType *>(&pack) ||
		dynamic_cast<EraseStack *>(&pack) ||
		dynamic_cast<SwapStacks *>(&pack) ||
		dynamic_cast<InsertNewStack *>(&pack) ||
		dynamic_cast<RebalanceStacks *>(&pack) ||
		dynamic_cast<GiveStackExperience *>(&pack) ||
		dynamic_cast<SetHeroExperience *>(&pack);
	if(pendingBattle && battleArmyChange)
	{
		if(pendingBattle->ended && !pendingBattle->armyDecisionStarted)
			pendingBattle->refreshArmiesAfterApply = true;
		return;
	}
	if(suppressDerivedEffects && !documentBoundary)
		return;
	if(!pendingEncounter && !pendingBattle && !documentBoundary && (
		dynamic_cast<FoWChange *>(&pack) ||
		dynamic_cast<NewStructures *>(&pack) ||
		dynamic_cast<RazeStructures *>(&pack) ||
		dynamic_cast<SetHeroesInTown *>(&pack) ||
		dynamic_cast<SetAvailableHero *>(&pack) ||
		dynamic_cast<SetAvailableArtifacts *>(&pack) ||
		dynamic_cast<ChangeStackCount *>(&pack) ||
		dynamic_cast<SetStackType *>(&pack) ||
		dynamic_cast<EraseStack *>(&pack) ||
		dynamic_cast<SwapStacks *>(&pack) ||
		dynamic_cast<InsertNewStack *>(&pack) ||
		dynamic_cast<RebalanceStacks *>(&pack)))
		return;

	if(auto * timer = dynamic_cast<TurnTimeUpdate *>(&pack))
	{
		latestTimerStates[timer->player] = timerState(timer->turnTimer);
		return;
	}
	std::optional<std::string> turnStartTimer;
	std::optional<std::string> turnEndTimer;
	const auto * startInfo = gameState.getStartInfo();
	const bool timersEnabled = startInfo && startInfo->turnTimerInfo.isEnabled();

	if(auto * start = dynamic_cast<PlayerStartsTurn *>(&pack))
	{
		flushPendingMove(gameState);
		flushPendingRecruit(gameState);
		flushPendingTrade(gameState);
		if(timersEnabled)
		{
			if(auto it = latestTimerStates.find(start->player); it != latestTimerStates.end())
				turnStartTimerStates[start->player] = it->second;
		}
		startTurnDocument(gameState, start->player);
	}
	else if(auto * newTurn = dynamic_cast<NewTurn *>(&pack))
	{
		if(newTurn->day <= 1)
		{
			pendingInitialTownAvailability.clear();
			pendingInitialDwellingAvailability.clear();
			for(const auto & availability : newTurn->availableCreatures)
				collectInitialAvailability(gameState, availability);
		}
		else
		{
			pendingWeeklyTownAvailability.clear();
			pendingWeeklyDwellingAvailability.clear();
			pendingWeeklyRewards.clear();
			pendingWeeklySpawns.clear();
			for(const auto & availability : newTurn->availableCreatures)
				collectWeeklyAvailability(gameState, availability);
		}
		flushPendingMove(gameState);
		flushPendingRecruit(gameState);
		flushPendingTrade(gameState);
		startWorldDocument(gameState, "newDay");
	}
	else if(auto * end = dynamic_cast<PlayerEndsTurn *>(&pack))
	{
		flushPendingMove(gameState);
		flushPendingRecruit(gameState);
		flushPendingTrade(gameState);
		if(timersEnabled)
		{
			if(auto it = turnStartTimerStates.find(end->player); it != turnStartTimerStates.end() && it->second != "none")
				turnStartTimer = it->second;
			if(auto it = latestTimerStates.find(end->player); it != latestTimerStates.end() && it->second != "none")
				turnEndTimer = it->second;
		}
	}

	if(auto * created = dynamic_cast<NewObject *>(&pack); created && !currentTurnPlayer &&
		created->newObject && created->newObject->ID == Obj::MONSTER)
	{
		if(const auto armed = std::dynamic_pointer_cast<CArmedInstance>(created->newObject);
			armed && armed->Slots().size() == 1)
		{
			const auto & [ignoredSlot, stack] = *armed->Slots().begin();
			if(stack)
			{
				pendingWeeklySpawns[creature(stack->getCreatureID())].push_back(
					"{ at: " + pos(created->newObject->visitablePos()) +
					", count: " + std::to_string(stack->getCount()) + " }");
				return;
			}
		}
	}

	EffectRecorder recorder(gameState, turnStartTimer, turnEndTimer);
	pack.visit(recorder);
	if(!recorder.result().empty())
	{
		if(!currentTurnPlayer && dynamic_cast<SetRewardableConfiguration *>(&pack))
		{
			const auto fields = keyedFlowFields(recorder.result(), "refresh");
			if(fields)
			{
				const auto object = flowField(*fields, "object");
				const auto reward = flowField(*fields, "reward");
				const auto text = flowField(*fields, "text");
				if(object && (reward || text))
					pendingWeeklyRewards.push_back({*object, reward.value_or(""), text.value_or("")});
			}
			return;
		}
		if(pendingEncounter)
		{
			std::string outcome = recorder.result();
			if(outcome.starts_with("usedToday: { "))
				boost::algorithm::replace_first(outcome, "object: magicWell", "object: " + pendingEncounter->object);
			pendingEncounter->outcomes.push_back(std::move(outcome));
			return;
		}
		if(pendingBattle && dynamic_cast<SetAvailableCreatures *>(&pack))
		{
			pendingBattle->aftermath.push_back(recorder.result());
			return;
		}
		flushPendingMove(gameState);
		writeActionLine(gameState, recorder.result());
		if(battleStart && pendingBattle && gameHandler.randomizer)
		{
			if(battleStart->info)
			{
				pendingBattle->randomizerParticipants = battleRandomizerParticipants(*battleStart->info);
				pendingBattle->randomizerHeroes = battleRandomizerHeroes(*battleStart->info);
			}
			else
			{
				pendingBattle->randomizerParticipants = battleRandomizerParticipants(gameState, battleStart->battleID);
				pendingBattle->randomizerHeroes = battleRandomizerHeroes(gameState, battleStart->battleID);
			}
			pendingBattle->initialRandom = singleLineJson(gameHandler.randomizer->toVGTBattleJson(
				pendingBattle->randomizerParticipants, pendingBattle->randomizerHeroes));
		}
	}
	if(auto * end = dynamic_cast<PlayerEndsTurn *>(&pack))
	{
		turnStartTimerStates.erase(end->player);
		betweenPlayerTurns = true;
	}
	if(exitAfterTurnEnds && dynamic_cast<PlayerEndsTurn *>(&pack))
	{
		++observedTurnEnds;
		if(observedTurnEnds >= *exitAfterTurnEnds)
			exitAfterAppliedState = true;
	}
}

void VGTRecorder::recordAppliedState(CGameHandler & gameHandler)
{
	std::scoped_lock lock(outputMutex);
	initializeFromEnvironment();
	if(pendingBattle && pendingBattle->refreshArmiesAfterApply)
	{
		capturePendingBattleArmies(gameHandler.gameState());
		pendingBattle->refreshArmiesAfterApply = false;
	}
	writeBaselineSave(gameHandler);
	writeTurnState(gameHandler);
	if(exitAfterAppliedState)
	{
		if(output)
			output.flush();
		logGlobal->info("VGT capture reached requested turn-end limit");
		std::fflush(nullptr);
		std::_Exit(EXIT_SUCCESS);
	}
}
