/*
* Settings.cpp, part of VCMI engine
*
* Authors: listed in file AUTHORS in main folder
*
* License: GNU General Public License v2.0 or later
* Full text of license available in license.txt file, in main folder
*
*/
#include "StdInc.h"
#include <algorithm>
#include <cmath>
#include <limits>

#include "Settings.h"

#include "../../../lib/constants/StringConstants.h"
#include "../../../lib/mapObjectConstructors/AObjectTypeHandler.h"
#include "../../../lib/mapObjectConstructors/CObjectClassesHandler.h"
#include "../../../lib/mapObjects/MapObjects.h"
#include "../../../lib/modding/CModHandler.h"
#include "../../../lib/GameLibrary.h"
#include "../../../lib/filesystem/Filesystem.h"
#include "../../../lib/json/JsonUtils.h"

namespace NK2AI
{
	namespace
	{
		constexpr float RATIO_MODEL_INTERCEPT = 1.01744632294f;
		constexpr float RATIO_MODEL_LOG_STRENGTH_RATIO_COEFFICIENT = 2.34077663709f;
		constexpr float RATIO_MODEL_LOG_STRENGTH_RATIO_MEAN = 0.437265671215f;
		constexpr float RATIO_MODEL_LOG_STRENGTH_RATIO_SCALE = 0.806169412392f;
		constexpr float MIN_RATIO_MODEL_SAFE_ATTACK_RATIO = 0.25f;
		constexpr float MAX_RATIO_MODEL_SAFE_ATTACK_RATIO = 4.0f;

		BattlePredictionModel parseBattlePredictionModel(const JsonNode & node)
		{
			if(node.isNull())
				return BattlePredictionModel::LEGACY;

			const auto value = node.String();
			if(value == "legacy")
				return BattlePredictionModel::LEGACY;
			if(value == "ratio")
				return BattlePredictionModel::RATIO;
			if(value == "v2")
				return BattlePredictionModel::V2;
			if(value == "v3")
				return BattlePredictionModel::V3;

			throw std::runtime_error("Unknown Nullkiller battle prediction model: " + value);
		}

		float probabilityToLogit(float probability)
		{
			probability = std::clamp(probability, 0.01f, 0.99f);
			return std::log(probability / (1.0f - probability));
		}

		float getRatioModelSafeAttackRatio(float safeProbability)
		{
			const float requiredLogRatio = RATIO_MODEL_LOG_STRENGTH_RATIO_MEAN
				+ RATIO_MODEL_LOG_STRENGTH_RATIO_SCALE
				* (probabilityToLogit(safeProbability) - RATIO_MODEL_INTERCEPT)
				/ RATIO_MODEL_LOG_STRENGTH_RATIO_COEFFICIENT;

			return std::clamp(
				std::exp(requiredLogRatio),
				MIN_RATIO_MODEL_SAFE_ATTACK_RATIO,
				MAX_RATIO_MODEL_SAFE_ATTACK_RATIO);
		}
	}

	Settings::Settings(int difficultyLevel):
		maxRoamingHeroes(8),
		maxRoamingHeroesPerTown(0),
		mainHeroTurnDistanceLimit(10),
		scoutHeroTurnDistanceLimit(5),
		threatTurnDistanceLimit(5),
		maxPass(10),
		maxPriorityPass(10),
		pathfinderBucketsCount(1),
		pathfinderBucketSize(32),
		maxGoldPressure(0.3f),
		retreatThresholdRelative(0.3),
		retreatThresholdAbsolute(10000),
		safeAttackRatio(1.1),
		battlePredictionSafeProbability(0.65f),
		maxArmyLossTarget(0.35f),
		battlePredictionModel(BattlePredictionModel::LEGACY),
		allowObjectGraph(true),
		useTroopsFromGarrisons(false),
		useOneWayMonoliths(false),
		updateHitmapOnTileReveal(false),
		openMap(true)
	{
		const std::string & difficultyName = GameConstants::DIFFICULTY_NAMES[difficultyLevel];
		const JsonNode & rootNode = JsonUtils::assembleFromFiles("config/ai/nk2ai/nk2ai-settings");
		const JsonNode & node = rootNode[difficultyName];

		maxRoamingHeroes = node["maxRoamingHeroes"].Integer();
		maxRoamingHeroesPerTown = node["maxRoamingHeroesPerTown"].Integer();
		mainHeroTurnDistanceLimit = node["mainHeroTurnDistanceLimit"].Integer();
		scoutHeroTurnDistanceLimit = node["scoutHeroTurnDistanceLimit"].Integer();
		maxPass = node["maxPass"].Integer();
		maxPriorityPass = node["maxPriorityPass"].Integer();
		pathfinderBucketsCount = node["pathfinderBucketsCount"].Integer();
		pathfinderBucketSize = node["pathfinderBucketSize"].Integer();
		maxGoldPressure = node["maxGoldPressure"].Float();
		retreatThresholdRelative = node["retreatThresholdRelative"].Float();
		retreatThresholdAbsolute = node["retreatThresholdAbsolute"].Float();
		maxArmyLossTarget = node["maxArmyLossTarget"].Float();
		safeAttackRatio = node["safeAttackRatio"].Float();
		battlePredictionModel = parseBattlePredictionModel(node["battlePredictionModel"]);
		if(!node["battlePredictionSafeProbability"].isNull())
			battlePredictionSafeProbability = std::clamp(static_cast<float>(node["battlePredictionSafeProbability"].Float()), 0.01f, 0.99f);
		allowObjectGraph = node["allowObjectGraph"].Bool();
		updateHitmapOnTileReveal = node["updateHitmapOnTileReveal"].Bool();
		openMap = node["openMap"].Bool();
		useTroopsFromGarrisons = node["useTroopsFromGarrisons"].Bool();
		useOneWayMonoliths = node["useOneWayMonoliths"].Bool();
	}

	float Settings::getSafeAttackRatio() const
	{
		if(battlePredictionModel == BattlePredictionModel::RATIO)
			return getRatioModelSafeAttackRatio(battlePredictionSafeProbability);

		return safeAttackRatio;
	}
}
