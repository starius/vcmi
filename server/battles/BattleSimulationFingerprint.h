/*
 * BattleSimulationFingerprint.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include <cstdint>

struct BattleStartInfo;

namespace BattleSimulation
{
constexpr uint64_t INVALID_STATE_FINGERPRINT = 0;

bool isValidStateFingerprint(uint64_t fingerprint);
uint64_t combineFingerprint(uint64_t fingerprint, uint64_t value);
uint64_t fingerprintBattleStartInfo(const BattleStartInfo & setup);

class BattleSimulationFingerprintBuilder
{
public:
	void add(uint64_t value);
	void addSigned(int64_t value);
	void addBool(bool value);
	uint64_t value() const;

private:
	uint64_t fingerprint = 0xcbf29ce484222325ULL;
	bool hasData = false;
};
}
