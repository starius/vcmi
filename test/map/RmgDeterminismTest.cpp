/*
 * RmgDeterminismTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "../../lib/json/JsonNode.h"
#include "../../lib/filesystem/CMemoryBuffer.h"
#include "../../lib/filesystem/CZipLoader.h"
#include "../../lib/mapping/CMap.h"
#include "../../lib/mapping/MapFormatJson.h"
#include "../../lib/modding/ModScope.h"
#include "../../lib/rmg/CMapGenOptions.h"
#include "../../lib/rmg/CMapGenerator.h"
#include "../../lib/rmg/RmgArea.h"
#include "../../lib/ScopeGuard.h"
#include "../../lib/serializer/JsonDeserializer.h"
#include "../mock/mock_IGameInfoCallback.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <tbb/global_control.h>

namespace
{
constexpr int TEST_RANDOM_SEED = 1337;
constexpr int TEST_SINGLE_THREAD_PARALLELISM = 1;
constexpr int TEST_PARALLEL_PARALLELISM = 8;
constexpr int TEST_MAX_PARALLELISM = 16;
constexpr std::time_t TEST_CREATION_TIME = 1'725'897'600;
const std::string TEST_TEMPLATE_DATA_PATH = "test/rmg/1.json";
const CMap * gMapForCallbackLookup = nullptr;

struct DeterminismScenario
{
	std::string id;
	std::string templateId;
	int width;
	int height;
	int levels;
	int humanOrCpuPlayers;
	int compOnlyPlayers;
	EWaterContent::EWaterContent waterContent;
	EMonsterStrength::EMonsterStrength monsterStrength;
};

const DeterminismScenario & defaultScenario()
{
	static const DeterminismScenario scenario = {
		"2SM2a-small-l1-default",
		"2SM2a",
		CMapHeader::MAP_SIZE_SMALL,
		CMapHeader::MAP_SIZE_SMALL,
		1,
		2,
		0,
		EWaterContent::RANDOM,
		EMonsterStrength::RANDOM,
	};
	return scenario;
}

const std::vector<DeterminismScenario> & coreScenarios()
{
	static const std::vector<DeterminismScenario> scenarios = {
		defaultScenario(),
		{
			"2SM2a-small-l2-no-water-weak",
			"2SM2a",
			CMapHeader::MAP_SIZE_SMALL,
			CMapHeader::MAP_SIZE_SMALL,
			2,
			2,
			0,
			EWaterContent::NONE,
			EMonsterStrength::GLOBAL_WEAK,
		},
		{
			"2SM2a-small-l2-islands-strong",
			"2SM2a",
			CMapHeader::MAP_SIZE_SMALL,
			CMapHeader::MAP_SIZE_SMALL,
			2,
			2,
			0,
			EWaterContent::ISLANDS,
			EMonsterStrength::GLOBAL_STRONG,
		},
		{
			"2LM2a-large-l1-no-water",
			"2LM2a",
			CMapHeader::MAP_SIZE_LARGE,
			CMapHeader::MAP_SIZE_LARGE,
			1,
			2,
			0,
			EWaterContent::NONE,
			EMonsterStrength::GLOBAL_WEAK,
		},
		{
			"2LM2a-large-l1-islands-strong",
			"2LM2a",
			CMapHeader::MAP_SIZE_LARGE,
			CMapHeader::MAP_SIZE_LARGE,
			1,
			2,
			0,
			EWaterContent::ISLANDS,
			EMonsterStrength::GLOBAL_STRONG,
		},
	};

	return scenarios;
}

std::shared_ptr<CRmgTemplate> loadTemplate(const std::string & templateId)
{
	static std::map<std::string, std::shared_ptr<CRmgTemplate>> templateCache;
	const auto cacheIt = templateCache.find(templateId);
	if(cacheIt != templateCache.end())
		return cacheIt->second;

	JsonNode testData(JsonPath::builtin(TEST_TEMPLATE_DATA_PATH));
	testData.setModScope(ModScope::scopeBuiltin(), true);
	if(testData[templateId].isNull())
		throw std::runtime_error("Missing determinism test template: " + templateId);

	auto result = std::make_shared<CRmgTemplate>();
	result->setId(templateId);

	JsonDeserializer handler(nullptr, testData[templateId]);
	result->serializeJson(handler);
	result->afterLoad();
	result->validate();

	templateCache.emplace(templateId, result);
	return result;
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

IGameInfoCallbackMock & getDummyCallback()
{
	static ::testing::NiceMock<IGameInfoCallbackMock> callback;
	static bool configured = false;

	if(!configured)
	{
		ON_CALL(callback, isAllowed(::testing::An<ArtifactID>()))
			.WillByDefault(::testing::Return(true));
		ON_CALL(callback, getArtInstance(::testing::_))
			.WillByDefault([](ArtifactInstanceID aid) -> const CArtifactInstance *
		{
			return gMapForCallbackLookup ? gMapForCallbackLookup->getArtifactInstance(aid) : nullptr;
		});
		configured = true;
	}

	return callback;
}

uint64_t stableHash64(const std::string & value)
{
	uint64_t hash = 14695981039346656037ULL;
	for(unsigned char ch : value)
	{
		hash ^= static_cast<uint64_t>(ch);
		hash *= 1099511628211ULL;
	}
	return hash;
}

int deterministicWorkerCount(const DeterminismScenario & scenario, int randomSeed)
{
	const uint64_t hash = stableHash64(scenario.id + "#" + std::to_string(randomSeed));
	const int workers = static_cast<int>((hash % TEST_MAX_PARALLELISM) + 1ULL);
	return workers == 1 ? TEST_MAX_PARALLELISM : workers;
}

std::string describeScenario(const DeterminismScenario & scenario)
{
	std::ostringstream output;
	output << scenario.id
		   << " template=" << scenario.templateId
		   << " size=" << scenario.width << "x" << scenario.height << "x" << scenario.levels
		   << " players=" << scenario.humanOrCpuPlayers << "+" << scenario.compOnlyPlayers
		   << " water=" << static_cast<int>(scenario.waterContent)
		   << " monsters=" << static_cast<int>(scenario.monsterStrength);
	return output.str();
}

std::unique_ptr<CMap> generateMap(
	const DeterminismScenario & scenario,
	int randomSeed,
	std::time_t creationDateTime,
	bool singleThread,
	int parallelism)
{
	const int effectiveParallelism = singleThread ? 1 : parallelism;
	tbb::global_control limitParallelism(tbb::global_control::max_allowed_parallelism, effectiveParallelism);

	auto mapTemplate = loadTemplate(scenario.templateId);
	const int standardPlayers = std::max(
		scenario.humanOrCpuPlayers, requiredStandardPlayersForTemplate(*mapTemplate));
	CMapGenOptions options;
	options.setMapTemplate(mapTemplate.get());
	options.setWidth(scenario.width);
	options.setHeight(scenario.height);
	options.setLevels(scenario.levels);
	options.setHumanOrCpuPlayerCount(static_cast<si8>(standardPlayers));
	options.setCompOnlyPlayerCount(static_cast<si8>(scenario.compOnlyPlayers));
	options.setWaterContent(scenario.waterContent);
	options.setMonsterStrength(scenario.monsterStrength);
	for(int playerIndex = 0; playerIndex < standardPlayers; ++playerIndex)
	{
		const auto playerType = playerIndex == 0 ? EPlayerType::HUMAN : EPlayerType::AI;
		options.setPlayerTypeForStandardPlayer(PlayerColor(playerIndex), playerType);
	}

	auto & callback = getDummyCallback();
	gMapForCallbackLookup = nullptr;
	CMapGenerator generator(options, &callback, randomSeed);
	generator.setSingleThread(singleThread);

	auto map = generator.generate(creationDateTime);
	gMapForCallbackLookup = map.get();
	return map;
}

std::vector<ui8> serializeMap(std::unique_ptr<CMap> map)
{
	gMapForCallbackLookup = map.get();

	CMemoryBuffer output;
	CMapSaverJson saver(&output);
	saver.saveMap(map);
	return output.getBuffer();
}

std::map<std::string, std::string> extractArchivePayload(const std::vector<ui8> & serializedMap)
{
	std::map<std::string, std::string> payloadByName;

	CMemoryBuffer input;
	const auto written = input.write(serializedMap.data(), static_cast<si64>(serializedMap.size()));
	if(written != static_cast<si64>(serializedMap.size()))
		throw std::runtime_error("Failed to stage serialized map for determinism test.");

	input.seek(0);
	std::shared_ptr<CIOApi> ioApi(new CProxyROIOApi(&input));
	CZipLoader archive("", "_", ioApi);

	const auto files = archive.getFilteredFiles([](const ResourcePath &)
	{
		return true;
	});

	for(const auto & file : files)
	{
		auto stream = archive.load(file);
		if(!stream)
			continue;

		auto data = stream->readAll();
		if(!data.first)
			throw std::runtime_error("Failed to read unpacked map payload in determinism test.");

		payloadByName.emplace(file.getOriginalName(), std::string(reinterpret_cast<const char *>(data.first.get()), data.second));
	}

	return payloadByName;
}

uint64_t fnv1a64(const std::vector<ui8> & data)
{
	uint64_t hash = 14695981039346656037ULL;
	for(ui8 byte : data)
	{
		hash ^= static_cast<uint64_t>(byte);
		hash *= 1099511628211ULL;
	}
	return hash;
}

std::string toHex64(uint64_t value)
{
	std::ostringstream stream;
	stream << std::hex << std::nouppercase << std::setw(16) << std::setfill('0') << value;
	return stream.str();
}

std::string mapHashHex(
	const DeterminismScenario & scenario,
	int randomSeed,
	std::time_t creationDateTime,
	bool singleThread,
	int parallelism)
{
	const auto serialized = serializeMap(generateMap(scenario, randomSeed, creationDateTime, singleThread, parallelism));
	return toHex64(fnv1a64(serialized));
}

::testing::AssertionResult archivePayloadEquals(
	const std::vector<ui8> & baselineSerialized,
	const std::vector<ui8> & candidateSerialized,
	const std::string & context)
{
	const auto baseline = extractArchivePayload(baselineSerialized);
	const auto candidate = extractArchivePayload(candidateSerialized);
	if(baseline == candidate)
		return ::testing::AssertionSuccess();

	std::ostringstream error;
	error << context << ": archive payload mismatch";
	if(baseline.size() != candidate.size())
		error << " (entry count " << baseline.size() << " vs " << candidate.size() << ")";

	for(const auto & [name, baselineData] : baseline)
	{
		const auto candidateIt = candidate.find(name);
		if(candidateIt == candidate.end())
		{
			error << "; missing entry in candidate: " << name;
			return ::testing::AssertionFailure() << error.str();
		}

		const auto & candidateData = candidateIt->second;
		if(baselineData == candidateData)
			continue;

		const auto firstMismatch = std::mismatch(
			baselineData.begin(), baselineData.end(),
			candidateData.begin(), candidateData.end());
		const size_t firstDifferent = static_cast<size_t>(
			std::distance(baselineData.begin(), firstMismatch.first));

		error << "; first difference in entry " << name
			  << " at byte " << firstDifferent
			  << " (sizes " << baselineData.size() << " vs " << candidateData.size() << ")";
		return ::testing::AssertionFailure() << error.str();
	}

	for(const auto & [name, candidateData] : candidate)
	{
		if(baseline.find(name) == baseline.end())
		{
			error << "; extra entry in candidate: " << name
				  << " (size " << candidateData.size() << ")";
			return ::testing::AssertionFailure() << error.str();
		}
	}

	return ::testing::AssertionFailure() << error.str();
}
}

TEST(RmgDeterminism, SameSeedProducesSameSerializedMap)
{
	const auto & scenario = defaultScenario();
	const auto first = serializeMap(generateMap(
		scenario, TEST_RANDOM_SEED, TEST_CREATION_TIME, true, TEST_SINGLE_THREAD_PARALLELISM));
	const auto second = serializeMap(generateMap(
		scenario, TEST_RANDOM_SEED, TEST_CREATION_TIME, true, TEST_SINGLE_THREAD_PARALLELISM));
	EXPECT_TRUE(archivePayloadEquals(first, second, "single-thread replay"));
}

TEST(RmgDeterminism, CreationTimestampCanBeOverridden)
{
	constexpr std::time_t timestampA = TEST_CREATION_TIME;
	constexpr std::time_t timestampB = TEST_CREATION_TIME + 86400;

	const auto & scenario = defaultScenario();
	auto mapA = generateMap(scenario, TEST_RANDOM_SEED, timestampA, true, TEST_SINGLE_THREAD_PARALLELISM);
	auto mapB = generateMap(scenario, TEST_RANDOM_SEED, timestampB, true, TEST_SINGLE_THREAD_PARALLELISM);
	EXPECT_EQ(mapA->creationDateTime, timestampA);
	EXPECT_EQ(mapB->creationDateTime, timestampB);

	const auto serializedA = serializeMap(std::move(mapA));
	const auto serializedB = serializeMap(std::move(mapB));
	EXPECT_NE(serializedA, serializedB);
}

TEST(RmgDeterminism, AreaTilesVectorIsSorted)
{
	rmg::Tileset tiles;
	tiles.insert(int3(5, 1, 0));
	tiles.insert(int3(2, 3, 0));
	tiles.insert(int3(4, 1, 0));
	tiles.insert(int3(2, 2, 0));

	rmg::Area area(std::move(tiles));
	const auto & vectorView = area.getTilesVector();
	EXPECT_TRUE(std::is_sorted(vectorView.begin(), vectorView.end()));
	EXPECT_EQ(vectorView.front(), int3(4, 1, 0));
	EXPECT_EQ(vectorView.back(), int3(2, 3, 0));
}

TEST(RmgDeterminism, DeterministicSeedDerivationIsStable)
{
	const auto & scenario = defaultScenario();
	auto mapTemplate = loadTemplate(scenario.templateId);
	CMapGenOptions options;
	options.setMapTemplate(mapTemplate.get());
	options.setWidth(scenario.width);
	options.setHeight(scenario.height);
	options.setLevels(scenario.levels);
	options.setHumanOrCpuPlayerCount(static_cast<si8>(scenario.humanOrCpuPlayers));
	options.setCompOnlyPlayerCount(static_cast<si8>(scenario.compOnlyPlayers));
	options.setPlayerTypeForStandardPlayer(PlayerColor(0), EPlayerType::HUMAN);
	options.setPlayerTypeForStandardPlayer(PlayerColor(1), EPlayerType::AI);
	CMapGenerator generator(options, nullptr, TEST_RANDOM_SEED);

	const int reference = generator.deriveDeterministicSeed(7, "MinePlacer", "objects");
	EXPECT_EQ(reference, generator.deriveDeterministicSeed(7, "MinePlacer", "objects"));
	EXPECT_NE(reference, generator.deriveDeterministicSeed(8, "MinePlacer", "objects"));
	EXPECT_NE(reference, generator.deriveDeterministicSeed(7, "MinePlacer", "extra"));
	EXPECT_NE(reference, 0);
}

TEST(RmgDeterminism, ParallelSameSeedProducesSameSerializedMap)
{
	const auto & scenario = defaultScenario();
	const auto first = serializeMap(generateMap(
		scenario, TEST_RANDOM_SEED, TEST_CREATION_TIME, false, TEST_PARALLEL_PARALLELISM));
	const auto second = serializeMap(generateMap(
		scenario, TEST_RANDOM_SEED, TEST_CREATION_TIME, false, TEST_PARALLEL_PARALLELISM));
	EXPECT_TRUE(archivePayloadEquals(first, second, "parallel replay"));
}

TEST(RmgDeterminism, ParallelResultIsThreadCountInvariant)
{
	const auto & scenario = defaultScenario();
	const auto baseline = serializeMap(generateMap(scenario, TEST_RANDOM_SEED, TEST_CREATION_TIME, false, 1));

	for(const int parallelism : {2, 4, 8})
	{
		const auto candidate = serializeMap(generateMap(
			scenario, TEST_RANDOM_SEED, TEST_CREATION_TIME, false, parallelism));
		EXPECT_TRUE(archivePayloadEquals(
			baseline, candidate,
			"worker-count invariance 1 vs " + std::to_string(parallelism)));
	}
}

TEST(RmgDeterminism, ReproSeedParallelReplay)
{
	constexpr int reproSeed = 16'711'681;
	constexpr std::time_t reproCreationTime = 1'742'174'175;
	constexpr int reproParallelism = 16;
	const auto & scenario = defaultScenario();

	const auto first = serializeMap(generateMap(
		scenario, reproSeed, reproCreationTime, false, reproParallelism));
	const auto second = serializeMap(generateMap(
		scenario, reproSeed, reproCreationTime, false, reproParallelism));
	EXPECT_TRUE(archivePayloadEquals(first, second, "repro parallel replay"));
}

TEST(RmgDeterminism, ReproSeedParallelReplaySecondCase)
{
	constexpr int reproSeed = 1'446'117'377;
	constexpr std::time_t reproCreationTime = 1'725'897'613;
	constexpr int reproParallelism = 16;
	const auto & scenario = defaultScenario();

	const auto first = serializeMap(generateMap(
		scenario, reproSeed, reproCreationTime, false, reproParallelism));
	const auto second = serializeMap(generateMap(
		scenario, reproSeed, reproCreationTime, false, reproParallelism));
	EXPECT_TRUE(archivePayloadEquals(first, second, "repro parallel replay second case"));
}

TEST(RmgDeterminism, ReproSeedWorkerCountInvariantThirdCase)
{
	constexpr int reproSeed = 20;
	constexpr std::time_t reproCreationTime = 1'742'174'175;
	const auto & scenario = defaultScenario();

	const auto onEightWorkers = serializeMap(generateMap(scenario, reproSeed, reproCreationTime, false, 8));
	const auto onSixteenWorkers = serializeMap(generateMap(scenario, reproSeed, reproCreationTime, false, 16));
	EXPECT_TRUE(archivePayloadEquals(onEightWorkers, onSixteenWorkers, "repro worker-count invariance third case"));
}

TEST(RmgDeterminism, FrozenHashesSingleScheduler)
{
	const auto & scenario = defaultScenario();
	constexpr std::time_t frozenTime = TEST_CREATION_TIME;
	constexpr int frozenParallelism = TEST_SINGLE_THREAD_PARALLELISM;
	const std::array<std::pair<int, const char *>, 4> frozenCases = {{
		{1337, "8fd05c2e50bb3a18"},
		{1338, "666f726e7246d671"},
		{1339, "011ae22711531a6e"},
		{1340, "8239f8e1326db9ae"},
	}};

	for(const auto & [seed, expectedHash] : frozenCases)
	{
		const auto actualHash = mapHashHex(scenario, seed, frozenTime, true, frozenParallelism);
		EXPECT_EQ(actualHash, expectedHash) << "single scheduler hash mismatch for seed " << seed;
	}
}

TEST(RmgDeterminism, FrozenHashesParallelSchedulerWorkerInvariant)
{
	const auto & scenario = defaultScenario();
	constexpr std::time_t frozenTime = TEST_CREATION_TIME;
	const std::array<std::pair<int, const char *>, 4> frozenCases = {{
		{1337, "5ee2e949699a274b"},
		{1338, "666f726e7246d671"},
		{1339, "011ae22711531a6e"},
		{1340, "8239f8e1326db9ae"},
	}};

	for(const auto & [seed, expectedHash] : frozenCases)
	{
		const auto hashOnOneWorker = mapHashHex(scenario, seed, frozenTime, false, 1);
		const auto hashOnEightWorkers = mapHashHex(scenario, seed, frozenTime, false, TEST_PARALLEL_PARALLELISM);
		EXPECT_EQ(hashOnOneWorker, expectedHash) << "parallel hash mismatch for seed " << seed;
		EXPECT_EQ(hashOnEightWorkers, expectedHash) << "parallel hash mismatch on 8 workers for seed " << seed;
	}
}

TEST(RmgDeterminism, FrozenHashesReproSeedSmallIslandsWorkerInvariant)
{
	const DeterminismScenario scenario = {
		"2SM2a-small-l2-islands-strong",
		"2SM2a",
		CMapHeader::MAP_SIZE_SMALL,
		CMapHeader::MAP_SIZE_SMALL,
		2,
		2,
		0,
		EWaterContent::ISLANDS,
		EMonsterStrength::GLOBAL_STRONG,
	};

	constexpr int frozenSeed = 98945;
	constexpr std::time_t frozenTime = 1'725'897'600;
	constexpr const char * expectedHash = "c8f5ab2a3f11a203";

	const auto hashOnOneWorker = mapHashHex(scenario, frozenSeed, frozenTime, false, 1);
	const auto hashOnSixteenWorkers = mapHashHex(scenario, frozenSeed, frozenTime, false, 16);

	EXPECT_EQ(hashOnOneWorker, expectedHash);
	EXPECT_EQ(hashOnSixteenWorkers, expectedHash);
}

TEST(RmgDeterminism, CoreScenarioMatrixThreadInvariant)
{
	const std::array<int, 3> seeds = {{11, 1337, 2026}};
	for(const auto & scenario : coreScenarios())
	{
		for(const int seed : seeds)
		{
			try
			{
				const int alternateWorkerCount = deterministicWorkerCount(scenario, seed);
				const auto baseline = serializeMap(generateMap(scenario, seed, TEST_CREATION_TIME, false, 1));
				const auto alternate = serializeMap(generateMap(
					scenario, seed, TEST_CREATION_TIME, false, alternateWorkerCount));
				EXPECT_TRUE(archivePayloadEquals(
					baseline, alternate,
					"core scenario worker invariance: " + describeScenario(scenario)
						+ " seed=" + std::to_string(seed)
						+ " workers=1 vs " + std::to_string(alternateWorkerCount)));
			}
			catch(const std::exception & ex)
			{
				FAIL() << "core scenario worker invariance threw exception for "
					   << describeScenario(scenario)
					   << " seed=" << seed
					   << " error=" << ex.what();
			}
		}
	}
}

TEST(RmgDeterminism, DISABLED_ExtendedScenarioSeedSweepThreadInvariant)
{
	for(const auto & scenario : coreScenarios())
	{
		for(int seed = 1; seed <= 48; ++seed)
		{
			try
			{
				const int alternateWorkerCount = deterministicWorkerCount(scenario, seed);
				const auto baseline = serializeMap(generateMap(scenario, seed, TEST_CREATION_TIME, false, 1));
				const auto alternate = serializeMap(generateMap(
					scenario, seed, TEST_CREATION_TIME, false, alternateWorkerCount));
				ASSERT_TRUE(archivePayloadEquals(
					baseline, alternate,
					"extended scenario sweep mismatch: " + describeScenario(scenario)
						+ " seed=" + std::to_string(seed)
						+ " workers=1 vs " + std::to_string(alternateWorkerCount)));
			}
			catch(const std::exception & ex)
			{
				FAIL() << "extended scenario sweep threw exception for "
					   << describeScenario(scenario)
					   << " seed=" << seed
					   << " error=" << ex.what();
			}
		}
	}
}
