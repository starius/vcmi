/*
 * IBonusBearer.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include "IBonusBearer.h"
#include "BonusList.h"

#include <charconv>

namespace
{

template<size_t Size>
std::string makeCacheKey(const char (&category)[Size], int32_t value)
{
	std::array<char, 32> result;
	static_assert(Size <= 8);
	auto position = std::copy_n(category, Size - 1, result.begin());
	*position++ = '_';
	const auto conversion = std::to_chars(position, result.end(), value);
	assert(conversion.ec == std::errc());
	return std::string(result.begin(), conversion.ptr);
}

template<size_t Size>
std::string makeCacheKey(const char (&category)[Size], int32_t first, int32_t second)
{
	std::array<char, 32> result;
	static_assert(Size <= 8);
	auto position = std::copy_n(category, Size - 1, result.begin());
	*position++ = '_';
	const auto firstConversion = std::to_chars(position, result.end() - 12, first);
	assert(firstConversion.ec == std::errc());
	position = firstConversion.ptr;
	*position++ = '_';
	const auto secondConversion = std::to_chars(position, result.end(), second);
	assert(secondConversion.ec == std::errc());
	return std::string(result.begin(), secondConversion.ptr);
}

}

int IBonusBearer::valOfBonuses(const CSelector &selector, const std::string &cachingStr, int baseValue) const
{
	TConstBonusListPtr hlp = getAllBonuses(selector, cachingStr);
	return hlp->totalValue(baseValue);
}

bool IBonusBearer::hasBonus(const CSelector &selector, const std::string &cachingStr) const
{
	//TODO: We don't need to count all bonuses and could break on first matching
	return !getBonuses(selector, cachingStr)->empty();
}

TConstBonusListPtr IBonusBearer::getBonuses(const CSelector &selector, const std::string &cachingStr) const
{
	return getAllBonuses(selector, cachingStr);
}

TConstBonusListPtr IBonusBearer::getBonusesFrom(BonusSource source) const
{
	const auto cachingStr = makeCacheKey("source", static_cast<int32_t>(source));
	CSelector s = Selector::sourceTypeSel(source);
	return getBonuses(s, cachingStr);
}

TConstBonusListPtr IBonusBearer::getBonusesOfType(BonusType type) const
{
	const auto cachingStr = makeCacheKey("type", static_cast<int32_t>(type));
	CSelector s = Selector::type()(type);
	return getBonuses(s, cachingStr);
}

TConstBonusListPtr IBonusBearer::getBonusesOfType(BonusType type, BonusSubtypeID subtype) const
{
	const auto cachingStr = makeCacheKey("type", static_cast<int32_t>(type), subtype.getNum());
	CSelector s = Selector::typeSubtype(type, subtype);
	return getBonuses(s, cachingStr);
}

int IBonusBearer::applyBonuses(BonusType type, int baseValue) const
{
	//This part is performance-critical
	const auto cachingStr = makeCacheKey("type", static_cast<int32_t>(type));
	CSelector s = Selector::type()(type);
	return valOfBonuses(s, cachingStr, baseValue);
}

int IBonusBearer::valOfBonuses(BonusType type) const
{
	return applyBonuses(type, 0);
}

bool IBonusBearer::hasBonusOfType(BonusType type) const
{
	//This part is performance-critical
	const auto cachingStr = makeCacheKey("type", static_cast<int32_t>(type));

	CSelector s = Selector::type()(type);

	return hasBonus(s, cachingStr);
}

int IBonusBearer::valOfBonuses(BonusType type, BonusSubtypeID subtype) const
{
	//This part is performance-critical
	const auto cachingStr = makeCacheKey("type", static_cast<int32_t>(type), subtype.getNum());

	CSelector s = Selector::typeSubtype(type, subtype);

	return valOfBonuses(s, cachingStr);
}

bool IBonusBearer::hasBonusOfType(BonusType type, BonusSubtypeID subtype) const
{
	//This part is performance-critical
	const auto cachingStr = makeCacheKey("type", static_cast<int32_t>(type), subtype.getNum());

	CSelector s = Selector::typeSubtype(type, subtype);

	return hasBonus(s, cachingStr);
}

bool IBonusBearer::hasBonusFrom(BonusSource source, BonusSourceID sourceID) const
{
	const auto cachingStr = makeCacheKey("source", static_cast<int32_t>(source), sourceID.getNum());
	return hasBonus(Selector::source(source,sourceID), cachingStr);
}

bool IBonusBearer::hasBonusFrom(BonusSource source) const
{
	const auto cachingStr = makeCacheKey("source", static_cast<int32_t>(source));
	return hasBonus((Selector::sourceTypeSel(source)), cachingStr);
}

std::shared_ptr<const Bonus> IBonusBearer::getBonus(const CSelector &selector) const
{
	auto bonuses = getAllBonuses(selector);
	return bonuses->getFirst(Selector::all);
}
