/*
 * McpGameHelpers.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/json/JsonNode.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Mcp
{

struct StateSelection
{
	std::set<std::string> sections;
	std::optional<uint64_t> sinceRevision;
	size_t maxUpdates = 200;

	bool fullState() const;
	bool includes(const std::string & section) const;
};

StateSelection readStateSelection(const JsonNode & arguments);
JsonNode filterStateBySelection(const JsonNode & state, const StateSelection & selection);

std::string makeRouteId(
	int32_t heroId,
	int32_t startX,
	int32_t startY,
	int32_t startZ,
	int32_t destinationX,
	int32_t destinationY,
	int32_t destinationZ,
	int32_t layerId,
	int32_t movementRemaining);

JsonNode makeRevisionedUpdate(uint64_t revision, const std::string & type, JsonNode data);
JsonNode collectUpdatesSince(const std::deque<JsonNode> & journal, uint64_t sinceRevision, size_t maxUpdates);

}
