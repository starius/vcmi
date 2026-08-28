/*
 * ClassicAITargetRecord.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

/// Persistent scratch fields stored at army+0x538..army+0x544 by SoD's
/// find_AI_targets routine. They are decision input: enchantment callbacks
/// can read records left by an earlier simulation or action.
struct ClassicAITargetRecord
{
	bool hasTarget = false;
	uint32_t targetUnitId = std::numeric_limits<uint32_t>::max();
	int64_t targetValue = 0;
	int32_t targetDistance = 0;
	uint32_t possibleTargets = 0;
};

using ClassicAITargetRecords = std::map<uint32_t, ClassicAITargetRecord>;
