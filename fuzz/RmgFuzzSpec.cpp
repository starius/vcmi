/*
 * RmgFuzzSpec.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include "RmgFuzzSpec.h"

#include "../lib/callback/EditorCallback.h"
#include "../lib/filesystem/CBinaryReader.h"
#include "../lib/filesystem/CMemoryStream.h"
#include "../lib/mapping/CMapHeader.h"
#include "../lib/rmg/CMapGenerator.h"
#include "../lib/rmg/CRmgTemplate.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>

#include <tbb/global_control.h>

namespace
{
constexpr std::time_t BASE_CREATION_TIME = 1'725'897'600;
constexpr int CREATION_TIME_SPAN_SECONDS = 365 * 24 * 3600;
constexpr size_t FUZZ_SPEC_BINARY_SIZE = sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t)
	+ sizeof(uint32_t) + 8 * sizeof(uint8_t);
constexpr size_t FUZZ_SPEC_OFFSET_MAP_SIZE = sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t);
constexpr size_t FUZZ_SPEC_OFFSET_LEVELS = FUZZ_SPEC_OFFSET_MAP_SIZE + 1;
constexpr size_t FUZZ_SPEC_OFFSET_HUMAN_PLAYERS = FUZZ_SPEC_OFFSET_LEVELS + 1;
constexpr size_t FUZZ_SPEC_OFFSET_COMP_PLAYERS = FUZZ_SPEC_OFFSET_HUMAN_PLAYERS + 1;
constexpr size_t FUZZ_SPEC_OFFSET_WATER = FUZZ_SPEC_OFFSET_COMP_PLAYERS + 1;
constexpr size_t FUZZ_SPEC_OFFSET_MONSTERS = FUZZ_SPEC_OFFSET_WATER + 1;
constexpr size_t FUZZ_SPEC_OFFSET_TEMPLATE = FUZZ_SPEC_OFFSET_MONSTERS + 1;

constexpr std::array<int, 4> MAP_SIZE_CHOICES = {{
	CMapHeader::MAP_SIZE_SMALL,
	CMapHeader::MAP_SIZE_MIDDLE,
	CMapHeader::MAP_SIZE_LARGE,
	CMapHeader::MAP_SIZE_GIANT,
}};

constexpr std::array<EWaterContent::EWaterContent, 3> WATER_CHOICES = {{
	EWaterContent::NONE,
	EWaterContent::NORMAL,
	EWaterContent::ISLANDS,
}};

constexpr std::array<EMonsterStrength::EMonsterStrength, 3> MONSTER_CHOICES = {{
	EMonsterStrength::GLOBAL_WEAK,
	EMonsterStrength::GLOBAL_NORMAL,
	EMonsterStrength::GLOBAL_STRONG,
}};

int nearestMapSize(int value)
{
	int nearest = MAP_SIZE_CHOICES.front();
	int nearestDistance = std::abs(value - nearest);
	for(const int candidate : MAP_SIZE_CHOICES)
	{
		const int distance = std::abs(value - candidate);
		if(distance < nearestDistance)
		{
			nearestDistance = distance;
			nearest = candidate;
		}
	}
	return nearest;
}

template <typename T, size_t N>
bool containsChoice(const std::array<T, N> & choices, T value)
{
	return std::find(choices.begin(), choices.end(), value) != choices.end();
}

template <typename T, size_t N>
int choiceIndex(const std::array<T, N> & choices, T value)
{
	const auto it = std::find(choices.begin(), choices.end(), value);
	if(it == choices.end())
		return 0;
	return static_cast<int>(std::distance(choices.begin(), it));
}

const CRmgTemplate * pickDeterministicTemplate(const CMapGenOptions & options, int templateSelector)
{
	auto candidates = options.getPossibleTemplates();
	if(candidates.empty())
		throw std::runtime_error("No RMG templates available for fuzzing.");

	std::sort(candidates.begin(), candidates.end(), [](const CRmgTemplate * lhs, const CRmgTemplate * rhs)
	{
		return lhs->getId() < rhs->getId();
	});

	const size_t index = static_cast<size_t>(std::max(0, templateSelector)) % candidates.size();
	return candidates[index];
}

int requiredStandardPlayersForTemplate(const CRmgTemplate & mapTemplate)
{
	int requiredPlayers = 1;
	for(const auto & [zoneId, zone] : mapTemplate.getZones())
	{
		(void)zoneId;
		if(!zone)
			continue;

		const auto owner = zone->getOwner();
		if(owner)
			requiredPlayers = std::max(requiredPlayers, *owner);
	}

	return requiredPlayers;
}

EWaterContent::EWaterContent waterContentFromToken(uint8_t token)
{
	return WATER_CHOICES[token % WATER_CHOICES.size()];
}

EMonsterStrength::EMonsterStrength monsterStrengthFromToken(uint8_t token)
{
	return MONSTER_CHOICES[token % MONSTER_CHOICES.size()];
}

std::string toLowerCopy(const std::string & value)
{
	std::string lowered = value;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch)
	{
		return static_cast<char>(std::tolower(ch));
	});
	return lowered;
}

std::string waterContentToString(EWaterContent::EWaterContent value)
{
	switch(value)
	{
		case EWaterContent::NONE:
			return "none";
		case EWaterContent::NORMAL:
			return "normal";
		case EWaterContent::ISLANDS:
			return "islands";
		default:
			return "normal";
	}
}

std::string monsterStrengthToString(EMonsterStrength::EMonsterStrength value)
{
	switch(value)
	{
		case EMonsterStrength::GLOBAL_WEAK:
			return "weak";
		case EMonsterStrength::GLOBAL_NORMAL:
			return "normal";
		case EMonsterStrength::GLOBAL_STRONG:
			return "strong";
		default:
			return "normal";
	}
}

fuzzing::RmgGenerationSpec normalizeSpec(fuzzing::RmgGenerationSpec spec)
{
	if(spec.seed == 0)
		spec.seed = 1;

	spec.parallelism = std::max(1, spec.parallelism);
	if(spec.singleThread)
		spec.parallelism = 1;

	const std::time_t minCreationTime = BASE_CREATION_TIME;
	const std::time_t maxCreationTime = BASE_CREATION_TIME + CREATION_TIME_SPAN_SECONDS;
	spec.creationDateTime = std::clamp(spec.creationDateTime, minCreationTime, maxCreationTime);

	spec.width = nearestMapSize(std::max(1, spec.width));
	spec.height = spec.width;
	spec.levels = std::clamp(spec.levels, 1, 2);

	spec.humanOrCpuPlayers = std::clamp(spec.humanOrCpuPlayers, 1, 4);
	spec.compOnlyPlayers = std::clamp(spec.compOnlyPlayers, 0, 2);
	if(spec.humanOrCpuPlayers + spec.compOnlyPlayers < 2)
		spec.humanOrCpuPlayers = 2;

	if(!containsChoice(WATER_CHOICES, spec.waterContent))
		spec.waterContent = EWaterContent::NORMAL;
	if(!containsChoice(MONSTER_CHOICES, spec.monsterStrength))
		spec.monsterStrength = EMonsterStrength::GLOBAL_NORMAL;

	spec.templateSelector = std::max(0, spec.templateSelector);
	return spec;
}

void writeU32LE(uint8_t * out, uint32_t value)
{
	out[0] = static_cast<uint8_t>(value & 0xffU);
	out[1] = static_cast<uint8_t>((value >> 8) & 0xffU);
	out[2] = static_cast<uint8_t>((value >> 16) & 0xffU);
	out[3] = static_cast<uint8_t>((value >> 24) & 0xffU);
}

std::string trimCopy(const std::string & input)
{
	size_t begin = 0;
	while(begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0)
		++begin;

	size_t end = input.size();
	while(end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0)
		--end;

	return input.substr(begin, end - begin);
}

bool parseBoolValue(const std::string & value)
{
	const std::string lowered = toLowerCopy(value);

	if(lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on")
		return true;
	if(lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off")
		return false;

	throw std::runtime_error("Invalid boolean value in fuzz seed text: " + value);
}

int parseIntValue(const std::string & value, const char * key)
{
	size_t consumed = 0;
	const long long parsed = std::stoll(value, &consumed, 10);
	if(consumed != value.size())
		throw std::runtime_error(std::string("Invalid integer for ") + key + ": " + value);

	if(parsed < static_cast<long long>(std::numeric_limits<int>::min())
		|| parsed > static_cast<long long>(std::numeric_limits<int>::max()))
	{
		throw std::runtime_error(std::string("Out-of-range integer for ") + key + ": " + value);
	}

	return static_cast<int>(parsed);
}

std::time_t parseTimeValue(const std::string & value, const char * key)
{
	size_t consumed = 0;
	const long long parsed = std::stoll(value, &consumed, 10);
	if(consumed != value.size())
		throw std::runtime_error(std::string("Invalid integer for ") + key + ": " + value);

	if(parsed < static_cast<long long>(std::numeric_limits<std::time_t>::min())
		|| parsed > static_cast<long long>(std::numeric_limits<std::time_t>::max()))
	{
		throw std::runtime_error(std::string("Out-of-range integer for ") + key + ": " + value);
	}

	return static_cast<std::time_t>(parsed);
}

EWaterContent::EWaterContent parseWaterContentValue(const std::string & value)
{
	const std::string lowered = toLowerCopy(value);
	if(lowered == "none")
		return EWaterContent::NONE;
	if(lowered == "normal")
		return EWaterContent::NORMAL;
	if(lowered == "islands")
		return EWaterContent::ISLANDS;

	const int parsed = parseIntValue(value, "waterContent");
	if(parsed < EWaterContent::NONE || parsed > EWaterContent::ISLANDS)
		throw std::runtime_error("Out-of-range waterContent value: " + value);
	return static_cast<EWaterContent::EWaterContent>(parsed);
}

EMonsterStrength::EMonsterStrength parseMonsterStrengthValue(const std::string & value)
{
	const std::string lowered = toLowerCopy(value);
	if(lowered == "weak")
		return EMonsterStrength::GLOBAL_WEAK;
	if(lowered == "normal")
		return EMonsterStrength::GLOBAL_NORMAL;
	if(lowered == "strong")
		return EMonsterStrength::GLOBAL_STRONG;

	const int parsed = parseIntValue(value, "monsterStrength");
	if(parsed < EMonsterStrength::GLOBAL_WEAK || parsed > EMonsterStrength::GLOBAL_STRONG)
		throw std::runtime_error("Out-of-range monsterStrength value: " + value);
	return static_cast<EMonsterStrength::EMonsterStrength>(parsed);
}
}

namespace fuzzing
{
RmgGenerationSpec decodeRmgGenerationSpec(const uint8_t * data, size_t size)
{
	std::array<ui8, FUZZ_SPEC_BINARY_SIZE> paddedInput{};
	if(data != nullptr && size > 0)
	{
		const size_t copySize = std::min(size, paddedInput.size());
		std::memcpy(paddedInput.data(), data, copySize);
	}

	CMemoryStream stream(paddedInput.data(), static_cast<si64>(paddedInput.size()));
	CBinaryReader reader(&stream);

	RmgGenerationSpec spec;
	spec.seed = static_cast<int>(reader.readUInt32() & 0x7fffffffU);
	spec.singleThread = reader.readBool();
	spec.parallelism = static_cast<int>(reader.readUInt32() & 0x7fffffffU);
	const uint32_t creationOffset = reader.readUInt32() % static_cast<uint32_t>(CREATION_TIME_SPAN_SECONDS + 1);
	spec.creationDateTime = BASE_CREATION_TIME + static_cast<std::time_t>(creationOffset);
	spec.width = MAP_SIZE_CHOICES[paddedInput[FUZZ_SPEC_OFFSET_MAP_SIZE] % MAP_SIZE_CHOICES.size()];
	spec.height = spec.width;
	spec.levels = 1 + static_cast<int>(paddedInput[FUZZ_SPEC_OFFSET_LEVELS] % 2U);
	spec.humanOrCpuPlayers = 1 + static_cast<int>(paddedInput[FUZZ_SPEC_OFFSET_HUMAN_PLAYERS] % 4U);
	spec.compOnlyPlayers = static_cast<int>(paddedInput[FUZZ_SPEC_OFFSET_COMP_PLAYERS] % 3U);
	spec.waterContent = waterContentFromToken(paddedInput[FUZZ_SPEC_OFFSET_WATER]);
	spec.monsterStrength = monsterStrengthFromToken(paddedInput[FUZZ_SPEC_OFFSET_MONSTERS]);
	spec.templateSelector = static_cast<int>(paddedInput[FUZZ_SPEC_OFFSET_TEMPLATE]);
	return normalizeSpec(spec);
}

std::vector<uint8_t> encodeRmgGenerationSpec(const RmgGenerationSpec & specInput)
{
	const RmgGenerationSpec spec = normalizeSpec(specInput);
	std::vector<uint8_t> output(FUZZ_SPEC_BINARY_SIZE, 0);

	const uint32_t seed = static_cast<uint32_t>(spec.seed) & 0x7fffffffU;
	const uint32_t parallelism = static_cast<uint32_t>(spec.parallelism) & 0x7fffffffU;
	const uint32_t creationOffset = static_cast<uint32_t>(spec.creationDateTime - BASE_CREATION_TIME);

	writeU32LE(output.data(), seed);
	output[sizeof(uint32_t)] = spec.singleThread ? 1U : 0U;
	writeU32LE(output.data() + sizeof(uint32_t) + sizeof(uint8_t), parallelism);
	writeU32LE(output.data() + sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t), creationOffset);
	output[FUZZ_SPEC_OFFSET_MAP_SIZE] = static_cast<uint8_t>(choiceIndex(MAP_SIZE_CHOICES, spec.width));
	output[FUZZ_SPEC_OFFSET_LEVELS] = static_cast<uint8_t>(spec.levels > 1 ? 1 : 0);
	output[FUZZ_SPEC_OFFSET_HUMAN_PLAYERS] = static_cast<uint8_t>(std::max(0, spec.humanOrCpuPlayers - 1));
	output[FUZZ_SPEC_OFFSET_COMP_PLAYERS] = static_cast<uint8_t>(std::max(0, spec.compOnlyPlayers));
	output[FUZZ_SPEC_OFFSET_WATER] = static_cast<uint8_t>(choiceIndex(WATER_CHOICES, spec.waterContent));
	output[FUZZ_SPEC_OFFSET_MONSTERS] = static_cast<uint8_t>(choiceIndex(MONSTER_CHOICES, spec.monsterStrength));
	output[FUZZ_SPEC_OFFSET_TEMPLATE] = static_cast<uint8_t>(spec.templateSelector & 0xff);

	return output;
}

std::string serializeRmgGenerationSpecText(const RmgGenerationSpec & specInput)
{
	const RmgGenerationSpec spec = normalizeSpec(specInput);

	std::ostringstream output;
	output << "# vcmi-fuzz-rmg-repro seed format v2\n";
	output << "seed=" << spec.seed << '\n';
	output << "singleThread=" << (spec.singleThread ? 1 : 0) << '\n';
	output << "parallelism=" << spec.parallelism << '\n';
	output << "creationDateTime=" << spec.creationDateTime << '\n';
	output << "width=" << spec.width << '\n';
	output << "height=" << spec.height << '\n';
	output << "levels=" << spec.levels << '\n';
	output << "humanOrCpuPlayers=" << spec.humanOrCpuPlayers << '\n';
	output << "compOnlyPlayers=" << spec.compOnlyPlayers << '\n';
	output << "waterContent=" << waterContentToString(spec.waterContent) << '\n';
	output << "monsterStrength=" << monsterStrengthToString(spec.monsterStrength) << '\n';
	output << "templateSelector=" << spec.templateSelector << '\n';
	return output.str();
}

RmgGenerationSpec parseRmgGenerationSpecText(const std::string & text)
{
	RmgGenerationSpec spec{};
	spec.seed = 1;
	spec.singleThread = false;
	spec.parallelism = 1;
	spec.creationDateTime = BASE_CREATION_TIME;
	spec.width = CMapHeader::MAP_SIZE_SMALL;
	spec.height = CMapHeader::MAP_SIZE_SMALL;
	spec.levels = 1;
	spec.humanOrCpuPlayers = 2;
	spec.compOnlyPlayers = 0;
	spec.waterContent = EWaterContent::NORMAL;
	spec.monsterStrength = EMonsterStrength::GLOBAL_NORMAL;
	spec.templateSelector = 0;

	std::istringstream stream(text);
	std::string line;
	while(std::getline(stream, line))
	{
		const std::string trimmed = trimCopy(line);
		if(trimmed.empty() || trimmed[0] == '#')
			continue;

		const size_t separator = trimmed.find('=');
		if(separator == std::string::npos)
			throw std::runtime_error("Invalid fuzz seed line (expected key=value): " + trimmed);

		const std::string key = trimCopy(trimmed.substr(0, separator));
		const std::string value = trimCopy(trimmed.substr(separator + 1));
		if(key.empty())
			throw std::runtime_error("Invalid fuzz seed line with empty key: " + trimmed);

		if(key == "seed")
			spec.seed = parseIntValue(value, "seed");
		else if(key == "singleThread")
			spec.singleThread = parseBoolValue(value);
		else if(key == "parallelism")
			spec.parallelism = parseIntValue(value, "parallelism");
		else if(key == "creationDateTime")
			spec.creationDateTime = parseTimeValue(value, "creationDateTime");
		else if(key == "width")
			spec.width = parseIntValue(value, "width");
		else if(key == "height")
			spec.height = parseIntValue(value, "height");
		else if(key == "levels")
			spec.levels = parseIntValue(value, "levels");
		else if(key == "humanOrCpuPlayers")
			spec.humanOrCpuPlayers = parseIntValue(value, "humanOrCpuPlayers");
		else if(key == "compOnlyPlayers")
			spec.compOnlyPlayers = parseIntValue(value, "compOnlyPlayers");
		else if(key == "waterContent")
			spec.waterContent = parseWaterContentValue(value);
		else if(key == "monsterStrength")
			spec.monsterStrength = parseMonsterStrengthValue(value);
		else if(key == "templateSelector")
			spec.templateSelector = parseIntValue(value, "templateSelector");
		else
			throw std::runtime_error("Unknown fuzz seed key: " + key);
	}

	return normalizeSpec(spec);
}

std::unique_ptr<CMap> generateMapWithParallelism(const RmgGenerationSpec & specInput, int parallelism)
{
	const RmgGenerationSpec spec = normalizeSpec(specInput);

	const int maxParallelism = spec.singleThread ? 1 : std::max(1, parallelism);
	tbb::global_control parallelismLimit(tbb::global_control::max_allowed_parallelism, maxParallelism);

	// RMG object creation expects a non-null callback.
	CMap callbackMap(nullptr);
	EditorCallback callback(&callbackMap);

	CMapGenOptions options;
	options.setWidth(spec.width);
	options.setHeight(spec.height);
	options.setLevels(spec.levels);
	options.setWaterContent(spec.waterContent);
	options.setMonsterStrength(spec.monsterStrength);
	const CRmgTemplate * selectedTemplate = pickDeterministicTemplate(options, spec.templateSelector);
	const int standardPlayers = std::max(
		spec.humanOrCpuPlayers, requiredStandardPlayersForTemplate(*selectedTemplate));
	const int clampedStandardPlayers = std::min(standardPlayers, static_cast<int>(PlayerColor::PLAYER_LIMIT_I));
	const int templatePlayerLimit = selectedTemplate->getPlayers().maxValue();
	const int maxCompOnlyPlayers = std::max(0, templatePlayerLimit - clampedStandardPlayers);
	const int clampedCompOnlyPlayers = std::min(spec.compOnlyPlayers, maxCompOnlyPlayers);
	options.setHumanOrCpuPlayerCount(static_cast<si8>(clampedStandardPlayers));
	options.setCompOnlyPlayerCount(static_cast<si8>(clampedCompOnlyPlayers));
	for(int playerIndex = 0; playerIndex < clampedStandardPlayers; ++playerIndex)
	{
		const auto playerType = playerIndex == 0 ? EPlayerType::HUMAN : EPlayerType::AI;
		options.setPlayerTypeForStandardPlayer(PlayerColor(playerIndex), playerType);
	}
	options.setMapTemplate(selectedTemplate);
	CMapGenerator generator(options, &callback, spec.seed);

	generator.setSingleThread(spec.singleThread);

	return generator.generate(spec.creationDateTime);
}

std::unique_ptr<CMap> generateMap(const RmgGenerationSpec & spec)
{
	return generateMapWithParallelism(spec, spec.parallelism);
}
}
