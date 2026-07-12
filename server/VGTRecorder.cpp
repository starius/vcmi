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

#include "../Version.h"
#include "../lib/GameConstants.h"
#include "../lib/GameLibrary.h"
#include "../lib/ResourceSet.h"
#include "../lib/StartInfo.h"
#include "../lib/battle/BattleAction.h"
#include "../lib/callback/Calendar.h"
#include "../lib/constants/StringConstants.h"
#include "../lib/filesystem/CInputStream.h"
#include "../lib/filesystem/Filesystem.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/json/JsonNode.h"
#include "../lib/mapping/CMap.h"
#include "../lib/mapObjects/CGObjectInstance.h"
#include "../lib/networkPacks/NetPackVisitor.h"
#include "../lib/texts/MetaString.h"

#include <boost/algorithm/string.hpp>
#include <boost/core/demangle.hpp>
#include <boost/filesystem.hpp>

#include <array>
#include <cctype>
#include <cstdlib>
#include <iomanip>
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
	return "[" + std::to_string(value.x) + ", " + std::to_string(value.y) + ", " + std::to_string(value.z) + "]";
}

std::string path(const std::vector<int3> & value)
{
	std::vector<std::string> result;
	result.reserve(value.size());
	for(const auto & tile : value)
		result.push_back(pos(tile));
	return flowList(result);
}

std::string color(PlayerColor value)
{
	return value.toString();
}

std::string objectAlias(const CGameState & gameState, ObjectInstanceID id)
{
	if(id == ObjectInstanceID::NONE)
		return "object/none";

	const auto * object = gameState.getMap().getObject(id);
	if(!object)
		return "object/id-" + std::to_string(id.getNum());

	std::string type = MapObjectID::encode(object->ID.getNum());
	if(type.empty())
		type = "object";

	std::string owner;
	if(object->tempOwner.isValidPlayer())
		owner = "/" + object->tempOwner.toString();

	std::string name = object->instanceName;
	if(name.empty())
		name = object->getObjectName();
	boost::algorithm::to_lower(name);
	for(char & ch : name)
	{
		if(!std::isalnum(static_cast<unsigned char>(ch)))
			ch = '-';
	}
	boost::algorithm::trim_if(name, boost::is_any_of("-"));
	if(name.empty())
		name = "id-" + std::to_string(id.getNum());

	return "object/" + type + owner + "/" + name + "/at-" +
		std::to_string(object->visitablePos().x) + "-" +
		std::to_string(object->visitablePos().y) + "-" +
		std::to_string(object->visitablePos().z);
}

std::string heroAlias(const CGameState & gameState, ObjectInstanceID id)
{
	const auto * object = gameState.getMap().getObject(id);
	if(!object)
		return objectAlias(gameState, id);

	std::string owner = object->tempOwner.isValidPlayer() ? object->tempOwner.toString() : "neutral";
	std::string name = object->instanceName.empty() ? object->getObjectName() : object->instanceName;
	boost::algorithm::to_lower(name);
	for(char & ch : name)
	{
		if(!std::isalnum(static_cast<unsigned char>(ch)))
			ch = '-';
	}
	boost::algorithm::trim_if(name, boost::is_any_of("-"));
	if(name.empty())
		name = "id-" + std::to_string(id.getNum());

	return "hero/" + owner + "/" + name;
}

std::string actorForPlayer(PlayerColor player)
{
	return player.isValidPlayer() ? "player/" + player.toString() : "world";
}

std::string resource(GameResID id)
{
	if(id.getNum() < 0)
		return yamlString("core:none");
	return yamlString("core:" + GameResID::encode(id.getNum()));
}

std::string creature(CreatureID id)
{
	if(id.getNum() < 0)
		return yamlString("core:none");
	return yamlString("core:" + CreatureID::encode(id.getNum()));
}

std::string spell(SpellID id)
{
	if(id.getNum() < 0)
		return yamlString("core:none");
	return yamlString("core:" + SpellID::encode(id.getNum()));
}

std::string heroType(HeroTypeID id)
{
	if(id.getNum() < 0)
		return yamlString("core:none");
	return yamlString("core:" + HeroTypeID::encode(id.getNum()));
}

std::string faction(FactionID id)
{
	if(id.getNum() < 0)
		return yamlString("core:none");
	return yamlString("core:" + FactionID::encode(id.getNum()));
}

std::string primarySkill(PrimarySkill id)
{
	if(id.getNum() < 0)
		return yamlString("none");
	return yamlString(PrimarySkill::encode(id.getNum()));
}

std::string secondarySkill(SecondarySkill id)
{
	if(id.getNum() < 0)
		return yamlString("none");
	return yamlString(SecondarySkill::encode(id.getNum()));
}

std::string building(BuildingID id)
{
	if(id.getNum() >= 0 && id.getNum() < std::size(EBuildingType::names))
		return yamlString("core:" + EBuildingType::names[id.getNum()]);
	return yamlString("building:" + std::to_string(id.getNum()));
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

std::string resources(const ResourceSet & values)
{
	std::vector<std::string> entries;
	for(size_t i = 0; i < values.size(); ++i)
	{
		if(values[i] == 0)
			continue;
		entries.push_back("{ resource: " + resource(GameResID(static_cast<int>(i))) + ", amount: " + std::to_string(values[i]) + " }");
	}
	return flowList(entries);
}

std::string resourceValues(const ResourceSet & values)
{
	std::vector<std::string> entries;
	for(size_t i = 0; i < values.size(); ++i)
		entries.push_back("{ resource: " + resource(GameResID(static_cast<int>(i))) + ", amount: " + std::to_string(values[i]) + " }");
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

			runs.push_back("{ y: " + std::to_string(entry.first.first) +
				", z: " + std::to_string(entry.first.second) +
				", x: [" + std::to_string(xs[start]) + ", " + std::to_string(xs[end]) + "] }");
			start = end + 1;
		}
	}
	return flowList(runs);
}

std::string battleTarget(const BattleAction::DestinationInfo & target)
{
	std::vector<std::string> fields;
	if(target.unitValue >= 0)
		fields.push_back("unit: stack/" + std::to_string(target.unitValue));
	if(target.hexValue.isValid())
		fields.push_back("hex: " + std::to_string(target.hexValue.toInt()));
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string battleAction(const BattleAction & action)
{
	std::vector<std::string> targets;
	for(const auto & target : action.target)
		targets.push_back(battleTarget(target));

	return "{ side: " + battleSide(action.side) +
		", stack: stack/" + std::to_string(action.stackNumber) +
		", action: " + actionType(action.actionType) +
		", spell: " + spell(action.spell) +
		", target: " + flowList(targets) + " }";
}

std::string battleUnitChanges(const std::vector<UnitChanges> & changes)
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

		result.push_back("{ stack: stack/" + std::to_string(change.id) +
			", operation: " + operation +
			", healthDelta: " + std::to_string(change.healthDelta) +
			", data: " + change.data.toCompactString() + " }");
	}
	return flowList(result);
}

std::string battleStackAttacked(const BattleStackAttacked & attack)
{
	std::vector<std::string> fields;
	fields.push_back("target: stack/" + std::to_string(attack.stackAttacked));
	fields.push_back("attacker: stack/" + std::to_string(attack.attackerID));
	fields.push_back("damage: " + std::to_string(attack.damageAmount));
	fields.push_back("killed: " + std::to_string(attack.killedAmount));
	fields.push_back("flags: " + std::to_string(attack.flags));
	if(attack.spellID != SpellID::NONE)
		fields.push_back("spell: " + spell(attack.spellID));
	if(!attack.newState.data.isNull())
		fields.push_back("state: " + attack.newState.data.toCompactString());
	return "{ " + boost::algorithm::join(fields, ", ") + " }";
}

std::string battleStackAttacks(const std::vector<BattleStackAttacked> & attacks)
{
	std::vector<std::string> result;
	for(const auto & attack : attacks)
		result.push_back(battleStackAttacked(attack));
	return flowList(result);
}

std::string query(QueryID queryID)
{
	if(queryID == QueryID::NONE)
		return "query/none";
	return "query/" + std::to_string(queryID.getNum());
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

std::string mapHash(const StartInfo & startInfo)
{
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
		boost::filesystem::path filePath(startInfo.fileURI);
		if(boost::filesystem::exists(filePath))
		{
			std::ifstream input(filePath.string(), std::ios::binary);
			std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			if(!data.empty())
				return sha256(data.data(), data.size());
		}
	}
	catch(...)
	{
	}

	return "unavailable";
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

	void visitMoveHero(MoveHero & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: moveHero, hero: " + heroAlias(gameState, pack.hid) +
			", path: " + path(pack.path) +
			", transit: " + std::string(pack.transit ? "true" : "false") + " }";
	}

	void visitBuildStructure(BuildStructure & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: buildStructure, town: " + objectAlias(gameState, pack.tid) +
			", building: " + building(pack.bid) + " }";
	}

	void visitRecruitCreatures(RecruitCreatures & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: recruitCreatures, source: " + objectAlias(gameState, pack.tid) +
			", destination: " + objectAlias(gameState, pack.dst) +
			", creature: " + creature(pack.crid) +
			", amount: " + std::to_string(pack.amount) +
			", level: " + std::to_string(pack.level) + " }";
	}

	void visitHireHero(HireHero & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: hireHero, town: " + objectAlias(gameState, pack.tid) +
			", hero: " + heroType(pack.hid) +
			", nextHero: " + heroType(pack.nhid) + " }";
	}

	void visitQueryReply(QueryReply & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: queryAnswer, query: " + query(pack.qid);
		if(pack.reply)
			line += ", answer: " + std::to_string(*pack.reply);
		else
			line += ", answer: null";
		line += " }";
	}

	void visitMakeAction(MakeAction & pack) override
	{
		line = "decision: { actor: " + actorForPlayer(pack.player) +
			", kind: battleAction, battle: battle/" + std::to_string(pack.battleID.getNum()) +
			", action: " + battleAction(pack.ba) + " }";
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

	void visitSaveGame(SaveGame &) override
	{
		line.clear();
	}

	void visitGamePause(GamePause &) override
	{
		line.clear();
	}

	void visitRequestStatistic(RequestStatistic &) override
	{
		line.clear();
	}

	void visitAdvInterfaceReady(AdvInterfaceReady &) override
	{
		line.clear();
	}
};

class EffectRecorder final : public ICPackVisitor
{
	const CGameState & gameState;
	std::string line;

	void unmodelled(CPackForClient & pack)
	{
		line = "unmodelled: { stream: effect, pack: " + yamlString(packetTypeName(pack)) + ", material: true }";
	}

public:
	explicit EffectRecorder(const CGameState & gameState)
		: gameState(gameState)
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

	void visitTurnTimeUpdate(TurnTimeUpdate &) override
	{
		line.clear();
	}

	void visitPlayerBlocked(PlayerBlocked & pack) override
	{
		line = "playerBlocked: { player: " + color(pack.player) + " }";
	}

	void visitPlayerStartsTurn(PlayerStartsTurn & pack) override
	{
		line = "turnStart: { player: " + color(pack.player) + ", query: " + query(pack.queryID) + " }";
	}

	void visitPlayerEndsTurn(PlayerEndsTurn & pack) override
	{
		line = "turnEnd: { player: " + color(pack.player) + " }";
	}

	void visitSetResources(SetResources & pack) override
	{
		line = "resources: { player: " + color(pack.player) +
			", mode: " + mode(pack.mode) +
			", values: " + resourceValues(pack.res) + " }";
	}

	void visitSetPrimarySkill(SetPrimarySkill & pack) override
	{
		line = "primarySkill: { hero: " + heroAlias(gameState, pack.id) +
			", skill: " + primarySkill(pack.which) +
			", mode: " + mode(pack.mode) +
			", value: " + std::to_string(pack.val) + " }";
	}

	void visitSetHeroExperience(SetHeroExperience & pack) override
	{
		line = "experience: { hero: " + heroAlias(gameState, pack.id) +
			", mode: " + mode(pack.mode) +
			", value: " + std::to_string(pack.val) + " }";
	}

	void visitSetSecSkill(SetSecSkill & pack) override
	{
		line = "secondarySkill: { hero: " + heroAlias(gameState, pack.id) +
			", skill: " + secondarySkill(pack.which) +
			", mode: " + mode(pack.mode) +
			", value: " + std::to_string(pack.val) + " }";
	}

	void visitSetMana(SetMana & pack) override
	{
		line = "mana: { hero: " + heroAlias(gameState, pack.hid) +
			", mode: " + mode(pack.mode) +
			", value: " + std::to_string(pack.val) + " }";
	}

	void visitSetMovePoints(SetMovePoints & pack) override
	{
		line = "movementPoints: { hero: " + heroAlias(gameState, pack.hid) +
			", value: " + std::to_string(pack.val) + " }";
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
			line += ", revealed: " + fowTiles(pack.fowRevealed);
		if(pack.attackedFrom.isValid())
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

	void visitSetHeroesInTown(SetHeroesInTown & pack) override
	{
		line = "townHeroes: { town: " + objectAlias(gameState, pack.tid) +
			", visiting: " + objectAlias(gameState, pack.visiting) +
			", garrison: " + objectAlias(gameState, pack.garrison) + " }";
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
			", player: " + color(pack.player) +
			", boat: " + objectAlias(gameState, pack.boatId) + " }";
	}

	void visitNewObject(NewObject & pack) override
	{
		const auto & object = pack.newObject;
		if(!object)
		{
			line = "newObject: { object: null }";
			return;
		}

		line = "newObject: { id: object/id-" + std::to_string(object->id.getNum()) +
			", type: " + yamlString(MapObjectID::encode(object->ID.getNum())) +
			", subtype: " + std::to_string(object->subID.getNum()) +
			", owner: " + color(object->tempOwner) +
			", position: " + pos(object->visitablePos()) +
			", initiator: " + color(pack.initiator) + " }";
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

	void visitHeroVisit(HeroVisit & pack) override
	{
		line = "visit: { hero: " + heroAlias(gameState, pack.heroId) +
			", object: " + objectAlias(gameState, pack.objId) +
			", start: " + std::string(pack.starting ? "true" : "false") + " }";
	}

	void visitNewTurn(NewTurn & pack) override
	{
		line = "newTurn: { day: " + std::to_string(pack.day) +
			", week: " + weekType(pack.specialWeek) +
			", creature: " + creature(pack.creatureid) +
			", income: [";

		bool first = true;
		for(const auto & entry : pack.playerIncome)
		{
			if(!first)
				line += ", ";
			first = false;
			line += "{ player: " + color(entry.first) + ", resources: " + resources(entry.second) + " }";
		}
		line += "] }";
	}

	void visitSetObjectProperty(SetObjectProperty & pack) override
	{
		line = "objectProperty: { object: " + objectAlias(gameState, pack.id) +
			", property: " + std::to_string(static_cast<int>(pack.what)) +
			", value: " + std::to_string(pack.identifier.getNum()) + " }";
	}

	void visitChangeObjectVisitors(ChangeObjectVisitors & pack) override
	{
		std::string visitMode = "clear";
		switch(pack.mode)
		{
			case ChangeObjectVisitors::VISITOR_ADD_HERO: visitMode = "addHero"; break;
			case ChangeObjectVisitors::VISITOR_ADD_PLAYER: visitMode = "addPlayer"; break;
			case ChangeObjectVisitors::VISITOR_SCOUTED: visitMode = "scouted"; break;
			case ChangeObjectVisitors::VISITOR_CLEAR: visitMode = "clear"; break;
		}
		line = "objectVisitors: { object: " + objectAlias(gameState, pack.object) +
			", hero: " + objectAlias(gameState, pack.hero) +
			", mode: " + visitMode + " }";
	}

	void visitHeroLevelUp(HeroLevelUp & pack) override
	{
		std::vector<std::string> skills;
		for(const auto & skill : pack.skills)
			skills.push_back(secondarySkill(skill));

		line = "levelUp: { player: " + color(pack.player) +
			", hero: " + heroAlias(gameState, pack.heroId) +
			", primary: " + primarySkill(pack.primskill) +
			", choices: " + flowList(skills) +
			", query: " + query(pack.queryID) + " }";
	}

	void visitInfoWindow(InfoWindow & pack) override
	{
		line = "info: { player: " + color(pack.player) +
			", text: " + yamlString(pack.text.toString()) +
			", components: " + std::to_string(pack.components.size()) + " }";
	}

	void visitBattleStart(BattleStart & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) + ", event: start }";
	}

	void visitBattleNextRound(BattleNextRound & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) + ", event: nextRound }";
	}

	void visitBattleSetActiveStack(BattleSetActiveStack & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) +
			", event: activeStack, stack: stack/" + std::to_string(pack.stack) + " }";
	}

	void visitBattleResult(BattleResult & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) +
			", event: result, result: " + battleResult(pack.result) +
			", winner: " + battleSide(pack.winner) +
			", attacker: " + color(pack.attacker) +
			", query: " + query(pack.queryID) + " }";
	}

	void visitBattleStackMoved(BattleStackMoved & pack) override
	{
		std::vector<std::string> tiles;
		for(const auto & tile : pack.tilesToMove)
			tiles.push_back(std::to_string(tile.toInt()));
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) +
			", event: move, stack: stack/" + std::to_string(pack.stack) +
			", path: " + flowList(tiles) +
			", distance: " + std::to_string(pack.distance) +
			", teleporting: " + std::string(pack.teleporting ? "true" : "false") + " }";
	}

	void visitBattleUnitsChanged(BattleUnitsChanged & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) +
			", event: unitsChanged, changes: " + battleUnitChanges(pack.changedStacks) + " }";
	}

	void visitBattleAttack(BattleAttack & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) +
			", event: attack, attacker: stack/" + std::to_string(pack.stackAttacking) +
			", to: " + std::to_string(pack.tile.toInt()) +
			", flags: " + std::to_string(pack.flags) +
			", attacks: " + battleStackAttacks(pack.bsa) + " }";
	}

	void visitStartAction(StartAction & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) +
			", event: startAction, action: " + battleAction(pack.ba) + " }";
	}

	void visitEndAction(EndAction & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) + ", event: endAction }";
	}

	void visitBattleSpellCast(BattleSpellCast & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) +
			", event: spellCast, side: " + battleSide(pack.side) +
			", spell: " + spell(pack.spellID) +
			", at: " + std::to_string(pack.tile.toInt()) +
			", casterStack: " + std::to_string(pack.casterStack) +
			", hero: " + std::string(pack.castByHero ? "true" : "false") + " }";
	}

	void visitStacksInjured(StacksInjured & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) +
			", event: injured, stacks: " + battleStackAttacks(pack.stacks) + " }";
	}

	void visitBattleEnded(BattleEnded & pack) override
	{
		line = "battle: { id: battle/" + std::to_string(pack.battleID.getNum()) +
			", event: ended, victor: " + color(pack.victor) +
			", loser: " + color(pack.loser) + " }";
	}

	void visitAdvmapSpellCast(AdvmapSpellCast & pack) override
	{
		line = "adventureSpell: { caster: " + heroAlias(gameState, pack.casterID) +
			", spell: " + spell(pack.spellID) + " }";
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

void VGTRecorder::initializeFromEnvironment()
{
	if(checkedEnvironment)
		return;
	checkedEnvironment = true;

	const char * path = std::getenv("VCMI_VGT_TEXT");
	if(!path || std::string(path).empty())
		return;

	outputPath = path;
	const boost::filesystem::path transcriptPath(outputPath);
	if(!transcriptPath.parent_path().empty())
		boost::filesystem::create_directories(transcriptPath.parent_path());

	output.open(outputPath, std::ios::out | std::ios::trunc);
	if(output)
		enabled = true;
	else
		logGlobal->error("Unable to open VGT transcript '%s'", outputPath);
}

void VGTRecorder::ensureHeader(const CGameState & gameState)
{
	if(headerWritten)
		return;

	const auto * startInfo = gameState.getStartInfo();
	output << "vgt: 3\n";
	output << "format: " << yamlString("VCMI readable event transcript") << "\n";
	output << "engine: { version: " << yamlString(GameConstants::VCMI_VERSION) << " }\n";
	if(startInfo)
	{
		output << "map:\n";
		output << "  uri: " << yamlString(startInfo->fileURI) << "\n";
		output << "  name: " << yamlString(startInfo->mapname) << "\n";
		output << "  hash: { algorithm: sha256, value: " << yamlString(mapHash(*startInfo)) << " }\n";
		output << "settings:\n";
		output << "  start: " << startMode(startInfo->mode) << "\n";
		output << "  difficulty: " << difficulty(startInfo->difficulty) << "\n";
		output << "  timer: " << (startInfo->turnTimerInfo.isEnabled() ? "enabled" : "none") << "\n";
		output << "players:\n";
		for(const auto & player : startInfo->playerInfos)
		{
			output << "  " << color(player.first) << ": { controller: "
				<< (player.second.isControlledByHuman() ? "human" : "ai")
				<< ", faction: " << faction(player.second.castle)
				<< ", hero: " << heroType(player.second.hero)
				<< ", name: " << yamlString(player.second.name)
				<< " }\n";
		}
	}
	else
	{
		output << "map: { uri: " << yamlString("") << ", hash: { algorithm: sha256, value: " << yamlString("unavailable") << " } }\n";
	}
	headerWritten = true;
	output.flush();
}

void VGTRecorder::startTurnDocument(const CGameState & gameState, PlayerColor player)
{
	ensureHeader(gameState);

	const auto calendar = gameState.getCalendar();
	output << "---\n";
	output << "turn: { month: " << calendar.getMonth()
		<< ", week: " << calendar.getWeek()
		<< ", day: " << calendar.getDayOfWeek()
		<< ", absoluteDay: " << calendar.getCurrentDay()
		<< ", player: " << color(player) << " }\n";
	output << "actions:\n";
	documentOpen = true;
	currentTurnPlayer = player;
}

void VGTRecorder::startWorldDocument(const CGameState & gameState, const std::string & phase)
{
	ensureHeader(gameState);

	const auto calendar = gameState.getCalendar();
	output << "---\n";
	output << "world: { month: " << calendar.getMonth()
		<< ", week: " << calendar.getWeek()
		<< ", day: " << calendar.getDayOfWeek()
		<< ", absoluteDay: " << calendar.getCurrentDay()
		<< ", phase: " << phase << " }\n";
	output << "events:\n";
	documentOpen = true;
	currentTurnPlayer.reset();
}

void VGTRecorder::writeActionLine(const CGameState & gameState, const std::string & line)
{
	if(line.empty())
		return;
	if(!documentOpen)
		startWorldDocument(gameState, "startup");
	output << "  - " << line << "\n";
	output.flush();
}

void VGTRecorder::recordDecision(const CGameState & gameState, CPackForServer & pack)
{
	std::scoped_lock lock(outputMutex);
	initializeFromEnvironment();
	if(!enabled)
		return;

	ensureHeader(gameState);
	DecisionRecorder recorder(gameState);
	pack.visit(recorder);
	writeActionLine(gameState, recorder.result());
}

void VGTRecorder::recordEffect(const CGameState & gameState, CPackForClient & pack)
{
	std::scoped_lock lock(outputMutex);
	initializeFromEnvironment();
	if(!enabled)
		return;

	ensureHeader(gameState);

	if(auto * start = dynamic_cast<PlayerStartsTurn *>(&pack))
		startTurnDocument(gameState, start->player);
	else if(dynamic_cast<NewTurn *>(&pack))
		startWorldDocument(gameState, "newTurn");

	EffectRecorder recorder(gameState);
	pack.visit(recorder);
	writeActionLine(gameState, recorder.result());
}
