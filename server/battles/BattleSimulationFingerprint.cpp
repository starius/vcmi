/*
 * BattleSimulationFingerprint.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationFingerprint.h"

namespace BattleSimulation
{
namespace
{
uint64_t mix(uint64_t value)
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}
}

bool isValidStateFingerprint(uint64_t fingerprint)
{
	return fingerprint != INVALID_STATE_FINGERPRINT;
}

uint64_t combineFingerprint(uint64_t fingerprint, uint64_t value)
{
	const auto result = fingerprint ^ mix(value + 0x9e3779b97f4a7c15ULL + (fingerprint << 6) + (fingerprint >> 2));
	return result == INVALID_STATE_FINGERPRINT ? 1 : result;
}

void BattleSimulationFingerprintBuilder::add(uint64_t value)
{
	hasData = true;
	fingerprint = combineFingerprint(fingerprint, value);
}

void BattleSimulationFingerprintBuilder::addSigned(int64_t value)
{
	add(static_cast<uint64_t>(value));
}

void BattleSimulationFingerprintBuilder::addBool(bool value)
{
	add(value ? 1 : 0);
}

uint64_t BattleSimulationFingerprintBuilder::value() const
{
	return hasData ? fingerprint : INVALID_STATE_FINGERPRINT;
}
}
