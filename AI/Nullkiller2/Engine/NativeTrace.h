/*
 * NativeTrace.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../../lib/json/JsonNode.h"

#include <mutex>
#include <string>
#include <vector>

namespace NK2AI
{

class NativeTrace
{
	std::string outputPath;
	JsonNode document;
	std::mutex mutex;

	void flushLocked() const;

public:
	NativeTrace();

	bool enabled() const;
	void recordDecision(
		const std::string & id,
		const std::string & functionName,
		JsonNode input,
		JsonNode nativeOutput,
		std::vector<std::string> compareFields = {});
};

}
