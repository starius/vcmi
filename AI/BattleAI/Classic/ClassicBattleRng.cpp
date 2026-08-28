/*
 * ClassicBattleRng.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "../StdInc.h"
#include "ClassicBattleRng.h"

#include "../../../lib/CRandomGenerator.h"

int32_t ClassicBattleAIRng::nextIntInclusive(int32_t lower, int32_t upper)
{
	return CRandomGenerator::getDefault().nextInt(lower, upper);
}

ClassicVstdRngAdapter::ClassicVstdRngAdapter(IClassicBattleAIRng & source) : source(source) {}

int ClassicVstdRngAdapter::nextInt(int lower, int upper)
{
	return source.nextIntInclusive(lower, upper);
}

int64_t ClassicVstdRngAdapter::nextInt64(int64_t lower, int64_t upper)
{
	if(lower < std::numeric_limits<int32_t>::min() || upper > std::numeric_limits<int32_t>::max())
		throw std::out_of_range("Classic BattleAI RNG request exceeds 32-bit oracle range");
	return source.nextIntInclusive(static_cast<int32_t>(lower), static_cast<int32_t>(upper));
}

double ClassicVstdRngAdapter::nextDouble(double lower, double upper)
{
	const int32_t scaled = source.nextIntInclusive(0, 1000000);
	return lower + (upper - lower) * scaled / 1000000.0;
}

int ClassicVstdRngAdapter::nextInt(int upper)
{
	return nextInt(0, upper);
}

int64_t ClassicVstdRngAdapter::nextInt64(int64_t upper)
{
	return nextInt64(0, upper);
}

double ClassicVstdRngAdapter::nextDouble(double upper)
{
	return nextDouble(0.0, upper);
}

int ClassicVstdRngAdapter::nextInt()
{
	return source.nextIntInclusive(0, std::numeric_limits<int32_t>::max());
}

int ClassicVstdRngAdapter::nextBinomialInt(int coinsCount, double coinChance)
{
	int result = 0;
	const int32_t limit = static_cast<int32_t>(std::clamp(coinChance, 0.0, 1.0) * 1000000.0);
	for(int coin = 0; coin < coinsCount; ++coin)
		result += source.nextIntInclusive(1, 1000000) <= limit;
	return result;
}

ReplayClassicBattleAIRng::ReplayClassicBattleAIRng(std::vector<int32_t> values) : values(std::move(values)) {}

int32_t ReplayClassicBattleAIRng::nextIntInclusive(int32_t lower, int32_t upper)
{
	if(cursor >= values.size())
		throw std::runtime_error(
			"Classic BattleAI random tape exhausted at request " + std::to_string(cursor)
			+ " [" + std::to_string(lower) + "," + std::to_string(upper) + "]");

	const int32_t result = values[cursor++];
	if(result < lower || result > upper)
	{
		throw std::runtime_error(
			"Classic BattleAI random value " + std::to_string(result)
			+ " is outside requested range [" + std::to_string(lower) + ", " + std::to_string(upper)
			+ "]"
		);
	}
	requests.push_back({lower, upper, result});
	return result;
}

size_t ReplayClassicBattleAIRng::consumed() const
{
	return cursor;
}

bool ReplayClassicBattleAIRng::exhausted() const
{
	return cursor == values.size();
}

const std::vector<ClassicRngRequest> & ReplayClassicBattleAIRng::getRequests() const
{
	return requests;
}
