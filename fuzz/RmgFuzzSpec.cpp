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
#include <limits>
#include <sstream>

#include <tbb/global_control.h>

namespace
{
constexpr std::time_t BASE_CREATION_TIME = 1'725'897'600;
constexpr int CREATION_TIME_SPAN_SECONDS = 365 * 24 * 3600;
constexpr size_t FUZZ_SPEC_BINARY_SIZE = sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t);

const CRmgTemplate * pickDeterministicTemplate(const CMapGenOptions & options)
{
	auto candidates = options.getPossibleTemplates();
	if(candidates.empty())
		throw std::runtime_error("No RMG templates available for fuzzing.");

	return *std::min_element(candidates.begin(), candidates.end(), [](const CRmgTemplate * lhs, const CRmgTemplate * rhs)
	{
		return lhs->getId() < rhs->getId();
	});
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
	std::string lowered = value;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch)
	{
		return static_cast<char>(std::tolower(ch));
	});

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

	return output;
}

std::string serializeRmgGenerationSpecText(const RmgGenerationSpec & specInput)
{
	const RmgGenerationSpec spec = normalizeSpec(specInput);

	std::ostringstream output;
	output << "# vcmi-fuzz-rmg-repro seed format v1\n";
	output << "seed=" << spec.seed << '\n';
	output << "singleThread=" << (spec.singleThread ? 1 : 0) << '\n';
	output << "parallelism=" << spec.parallelism << '\n';
	output << "creationDateTime=" << spec.creationDateTime << '\n';
	return output.str();
}

RmgGenerationSpec parseRmgGenerationSpecText(const std::string & text)
{
	RmgGenerationSpec spec{};
	spec.seed = 1;
	spec.singleThread = false;
	spec.parallelism = 1;
	spec.creationDateTime = BASE_CREATION_TIME;

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
	options.setWidth(CMapHeader::MAP_SIZE_SMALL);
	options.setHeight(CMapHeader::MAP_SIZE_SMALL);
	options.setLevels(1);
	options.setHumanOrCpuPlayerCount(2);
	options.setCompOnlyPlayerCount(0);
	options.setPlayerTypeForStandardPlayer(PlayerColor(0), EPlayerType::HUMAN);
	options.setPlayerTypeForStandardPlayer(PlayerColor(1), EPlayerType::AI);
	options.setMapTemplate(pickDeterministicTemplate(options));
	CMapGenerator generator(options, &callback, spec.seed);

	generator.setSingleThread(spec.singleThread);

	return generator.generate(spec.creationDateTime);
}

std::unique_ptr<CMap> generateMap(const RmgGenerationSpec & spec)
{
	return generateMapWithParallelism(spec, spec.parallelism);
}
}
