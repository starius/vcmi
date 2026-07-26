/*
* ResourceTrader.h, part of VCMI engine
*
* Authors: listed in file AUTHORS in main folder
*
* License: GNU General Public License v2.0 or later
* Full text of license available in license.txt file, in main folder
*/
#pragma once

#include "Nullkiller.h"

namespace NK2AI
{

class ResourceTrader
{
public:
	static constexpr float EXPENDABLE_BULK_RATIO = 0.5f;

	static bool trade(BuildAnalyzer & buildAnalyzer, CCallback & cc, const TResources & freeResources, float armyGoldRatio);
	static bool tradeHelper(
		float expendableBulkRatio,
		const IMarket & market,
		TResources missingNow,
		TResources income,
		TResources freeAfterMissingTotal,
		const BuildAnalyzer & buildAnalyzer,
		CCallback & cc
	);
};

}
