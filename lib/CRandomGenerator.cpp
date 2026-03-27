/*
 * CRandomGenerator.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"
#include "CRandomGenerator.h"

VCMI_LIB_NAMESPACE_BEGIN

namespace
{
uint64_t drawDeterministicBelow(TGenerator & generator, uint64_t upperExclusive);

template<typename T>
T drawDeterministicIntegral(TGenerator & generator, T lower, T upper)
{
	static_assert(std::is_integral_v<T>);
	using U = std::make_unsigned_t<T>;
	const U lowerUnsigned = static_cast<U>(lower);
	const U upperUnsigned = static_cast<U>(upper);
	const uint64_t rangeMinusOne = static_cast<uint64_t>(upperUnsigned - lowerUnsigned);
	const uint64_t upperExclusive = rangeMinusOne == std::numeric_limits<uint64_t>::max() ? 0 : rangeMinusOne + 1ULL;
	const U offset = static_cast<U>(drawDeterministicBelow(generator, upperExclusive));
	return static_cast<T>(lowerUnsigned + offset);
}

uint64_t drawGeneratorRaw(TGenerator & generator)
{
	return static_cast<uint64_t>(generator()) - static_cast<uint64_t>(TGenerator::min());
}

uint64_t drawDeterministicBelow(TGenerator & generator, uint64_t upperExclusive)
{
	if(upperExclusive == 0)
	{
		// Generate full-width value for wraparound ranges.
		const uint64_t partA = drawGeneratorRaw(generator);
		const uint64_t partB = drawGeneratorRaw(generator);
		const uint64_t partC = drawGeneratorRaw(generator);
		return (partA << 33) ^ (partB << 2) ^ (partC & 0x3ULL);
	}

	constexpr uint64_t generatorRange = static_cast<uint64_t>(TGenerator::max()) - static_cast<uint64_t>(TGenerator::min()) + 1ULL;
	while(true)
	{
		unsigned __int128 value = 0;
		unsigned __int128 range = 1;
		while(range < upperExclusive)
		{
			value = value * generatorRange + drawGeneratorRaw(generator);
			range *= generatorRange;
		}

		const unsigned __int128 limit = range - (range % upperExclusive);
		if(value < limit)
			return static_cast<uint64_t>(value % upperExclusive);
	}
}

double drawDeterministicUnitDouble(TGenerator & generator)
{
	constexpr uint64_t unitRange = uint64_t{1} << 53; // exactly representable in double
	return static_cast<double>(drawDeterministicBelow(generator, unitRange)) / static_cast<double>(unitRange);
}
}

CRandomGenerator::CRandomGenerator()
{
	logRng->trace("CRandomGenerator constructed");
	resetSeed();
}

CRandomGenerator::CRandomGenerator(int seed)
{
	logRng->trace("CRandomGenerator constructed (%d)", seed);
	setSeed(seed);
}

void CRandomGenerator::setSeed(int seed)
{
	logRng->trace("CRandomGenerator::setSeed (%d)", seed);
	rand.seed(seed);
}

void CRandomGenerator::resetSeed()
{
	logRng->trace("CRandomGenerator::resetSeed");
	std::hash<std::thread::id> hasher;
	auto threadIdHash = hasher(std::this_thread::get_id());
	setSeed(static_cast<int>(threadIdHash * std::time(nullptr)));
}

int CRandomGenerator::nextInt(int upper)
{
	logRng->trace("CRandomGenerator::nextInt (%d)", upper);
	return nextInt(0, upper);
}

int64_t CRandomGenerator::nextInt64(int64_t upper)
{
	logRng->trace("CRandomGenerator::nextInt64 (%d)", upper);
	return nextInt64(0, upper);
}

int CRandomGenerator::nextInt(int lower, int upper)
{
	logRng->trace("CRandomGenerator::nextInt64 (%d, %d)", lower, upper);

	if (lower > upper)
		throw std::runtime_error("Invalid range provided: " + std::to_string(lower) + " ... " + std::to_string(upper));

	return drawDeterministicIntegral(rand, lower, upper);
}

int CRandomGenerator::nextInt()
{
	logRng->trace("CRandomGenerator::nextInt64");
	return drawDeterministicIntegral(rand, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
}

int CRandomGenerator::nextBinomialInt(int coinsCount, double coinChance)
{
	logRng->trace("CRandomGenerator::nextBinomialInt (%d, %f)", coinsCount, coinChance);
	std::binomial_distribution<> distribution(coinsCount, coinChance);
	return distribution(rand);
}

int64_t CRandomGenerator::nextInt64(int64_t lower, int64_t upper)
{
	logRng->trace("CRandomGenerator::nextInt64 (%d, %d)", lower, upper);
	if (lower > upper)
		throw std::runtime_error("Invalid range provided: " + std::to_string(lower) + " ... " + std::to_string(upper));

	return drawDeterministicIntegral(rand, lower, upper);
}

double CRandomGenerator::nextDouble(double upper)
{
	logRng->trace("CRandomGenerator::nextDouble (%f)", upper);
	return nextDouble(0, upper);
}

double CRandomGenerator::nextDouble(double lower, double upper)
{
	logRng->trace("CRandomGenerator::nextDouble (%f, %f)", lower, upper);
	if(lower > upper)
		throw std::runtime_error("Invalid range provided: " + std::to_string(lower) + " ... " + std::to_string(upper));

	if(lower == upper)
		return lower;

	const double fraction = drawDeterministicUnitDouble(rand);
	return lower + (upper - lower) * fraction;
}

CRandomGenerator & CRandomGenerator::getDefault()
{
	static thread_local CRandomGenerator defaultRand;
	return defaultRand;
}


VCMI_LIB_NAMESPACE_END
