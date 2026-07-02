/*
 * McpProtocol.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../Global.h"
#include "../../lib/json/JsonNode.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

VCMI_LIB_NAMESPACE_BEGIN

namespace Mcp
{

class Protocol
{
public:
	struct ToolResult
	{
		JsonNode result;
	};

	using ToolHandler = std::function<ToolResult(const JsonNode & arguments)>;
	using ResourceReader = std::function<std::string()>;

	struct Tool
	{
		std::string name;
		std::string description;
		JsonNode inputSchema;
		ToolHandler handler;
	};

	struct Resource
	{
		std::string uri;
		std::string name;
		std::string description;
		std::string mimeType;
		ResourceReader reader;
	};

	void registerTool(Tool tool);
	void registerResource(Resource resource);

	/// Handles a single JSON-RPC message. Notifications return std::nullopt.
	std::optional<std::string> handleMessage(std::string_view payload) const;

	static ToolResult makeTextResult(const std::string & text, bool isError = false);

private:
	std::map<std::string, Tool> tools;
	std::map<std::string, Resource> resources;

	std::optional<JsonNode> handleRequest(const JsonNode & request) const;
	JsonNode handleInitialize() const;
	JsonNode handleToolsList() const;
	JsonNode handleToolsCall(const JsonNode & params) const;
	JsonNode handleResourcesList() const;
	JsonNode handleResourcesRead(const JsonNode & params) const;
};

}

VCMI_LIB_NAMESPACE_END
