/*
 * BattleSimulationEvaluatorTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"
#include "../../../server/battles/BattleSimulationEvaluator.h"

#include "../../../lib/mapObjects/army/CArmedInstance.h"

namespace
{
class BattleSimulationEvaluatorTest : public testing::Test
{
protected:
	CArmedInstance attacker{nullptr};
	CArmedInstance defender{nullptr};

	BattleSimulation::BattleSimulationRequest makeRequest(int32_t sampleIndex, int32_t sampleCount)
	{
		BattleSimulation::BattleSimulationRequest request;
		request.setup.armies[BattleSide::ATTACKER] = &attacker;
		request.setup.armies[BattleSide::DEFENDER] = &defender;
		request.seed.gameSeed = 12345;
		request.seed.player = PlayerColor(0);
		request.seed.heroId = ObjectInstanceID(17);
		request.seed.targetObjectId = ObjectInstanceID(42);
		request.seed.battleType = 3;
		request.seed.turn = 11;
		request.seed.sampleIndex = sampleIndex;
		request.seed.evaluatorVersion = 1;
		request.stateFingerprint = 0x123456789abcdefULL;
		request.sampleCount = sampleCount;
		return request;
	}

	BattleSimulation::BattleSimulationSummary oneSample(BattleSide winner)
	{
		BattleSimulation::BattleSimulationSummary summary;
		summary.recordWinner(winner);
		return summary;
	}
};
}

TEST_F(BattleSimulationEvaluatorTest, returnsExactCachedSummary)
{
	BattleSimulation::BattleSimulationEvaluator evaluator;
	auto request = makeRequest(0, 3);
	BattleSimulation::BattleSimulationSummary summary;
	summary.recordWinner(BattleSide::ATTACKER);
	summary.recordWinner(BattleSide::ATTACKER);
	summary.recordWinner(BattleSide::DEFENDER);

	evaluator.storeCachedSummary(request, summary);
	const auto response = evaluator.evaluate(request);

	EXPECT_EQ(response.status, BattleSimulation::BattleSimulationResponseStatus::COMPLETE);
	EXPECT_EQ(response.summary.rows, 3);
	EXPECT_EQ(response.summary.attackerWins, 2);
	EXPECT_EQ(response.summary.defenderWins, 1);
	EXPECT_DOUBLE_EQ(response.evaluation.attackerWinProbability, 2.0 / 3.0);
	EXPECT_EQ(evaluator.cacheSize(), 1);
}

TEST_F(BattleSimulationEvaluatorTest, composesCachedSingleSampleSummaries)
{
	BattleSimulation::BattleSimulationEvaluator evaluator;
	evaluator.storeCachedSummary(makeRequest(0, 1), oneSample(BattleSide::ATTACKER));
	evaluator.storeCachedSummary(makeRequest(1, 1), oneSample(BattleSide::DEFENDER));
	evaluator.storeCachedSummary(makeRequest(2, 1), oneSample(BattleSide::NONE));

	const auto response = evaluator.evaluate(makeRequest(0, 3));

	EXPECT_EQ(response.status, BattleSimulation::BattleSimulationResponseStatus::COMPLETE);
	EXPECT_EQ(response.summary.rows, 3);
	EXPECT_EQ(response.summary.attackerWins, 1);
	EXPECT_EQ(response.summary.defenderWins, 1);
	EXPECT_EQ(response.summary.noWinner, 1);
	EXPECT_DOUBLE_EQ(response.evaluation.attackerWinProbability, 1.0 / 3.0);
	EXPECT_EQ(evaluator.cacheSize(), 4);
}

TEST_F(BattleSimulationEvaluatorTest, returnsNotAvailableWhenSingleSampleIsMissing)
{
	BattleSimulation::BattleSimulationEvaluator evaluator;
	evaluator.storeCachedSummary(makeRequest(0, 1), oneSample(BattleSide::ATTACKER));
	evaluator.storeCachedSummary(makeRequest(2, 1), oneSample(BattleSide::ATTACKER));

	const auto response = evaluator.evaluate(makeRequest(0, 3));

	EXPECT_EQ(response.status, BattleSimulation::BattleSimulationResponseStatus::NOT_AVAILABLE);
	EXPECT_EQ(response.summary.rows, 0);
	EXPECT_EQ(evaluator.cacheSize(), 2);
}

TEST_F(BattleSimulationEvaluatorTest, ignoresIncompleteCachedSummary)
{
	BattleSimulation::BattleSimulationEvaluator evaluator;
	auto request = makeRequest(0, 3);
	BattleSimulation::BattleSimulationSummary incomplete;
	incomplete.recordWinner(BattleSide::ATTACKER);

	evaluator.storeCachedSummary(request, incomplete);
	const auto response = evaluator.evaluate(request);

	EXPECT_EQ(response.status, BattleSimulation::BattleSimulationResponseStatus::NOT_AVAILABLE);
	EXPECT_EQ(evaluator.cacheSize(), 0);
}
