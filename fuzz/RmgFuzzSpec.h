/*
 * RmgFuzzSpec.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#pragma once

#include "../lib/mapping/CMap.h"

#include <ctime>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fuzzing
{
struct RmgGenerationSpec
{
	int seed;
	bool singleThread;
	int parallelism;
	std::time_t creationDateTime;
};

RmgGenerationSpec decodeRmgGenerationSpec(const uint8_t * data, size_t size);
std::vector<uint8_t> encodeRmgGenerationSpec(const RmgGenerationSpec & spec);
std::string serializeRmgGenerationSpecText(const RmgGenerationSpec & spec);
RmgGenerationSpec parseRmgGenerationSpecText(const std::string & text);

std::unique_ptr<CMap> generateMap(const RmgGenerationSpec & spec);
std::unique_ptr<CMap> generateMapWithParallelism(const RmgGenerationSpec & spec, int parallelism);
}
