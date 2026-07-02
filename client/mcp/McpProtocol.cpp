/*
 * McpProtocol.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "McpProtocol.h"

#include <stdexcept>

VCMI_LIB_NAMESPACE_BEGIN

namespace Mcp
{
namespace
{

enum class JsonRpcErrorCode : int32_t
{
	ParseError = -32700,
	InvalidRequest = -32600,
	MethodNotFound = -32601,
	InvalidParams = -32602,
	InternalError = -32603
};

bool hasField(const JsonNode & node, const std::string & field)
{
	return node.isStruct() && node.Struct().find(field) != node.Struct().end();
}

JsonNode makeEmptyObject()
{
	JsonNode node;
	node.Struct();
	return node;
}

JsonNode makeResponse(const JsonNode & id, const JsonNode & result)
{
	JsonNode response;
	response["jsonrpc"] = JsonNode("2.0");
	response["id"] = id;
	response["result"] = result;
	return response;
}

JsonNode makeErrorResponse(const JsonNode & id, JsonRpcErrorCode code, const std::string & message)
{
	JsonNode response;
	response["jsonrpc"] = JsonNode("2.0");
	response["id"] = id;
	response["error"]["code"] = JsonNode(static_cast<int32_t>(code));
	response["error"]["message"] = JsonNode(message);
	return response;
}

JsonNode makeContentText(const std::string & text)
{
	JsonNode content;
	content["type"] = JsonNode("text");
	content["text"] = JsonNode(text);
	return content;
}

JsonNode makeToolDescriptor(const Protocol::Tool & tool)
{
	JsonNode descriptor;
	descriptor["name"] = JsonNode(tool.name);
	descriptor["description"] = JsonNode(tool.description);
	descriptor["inputSchema"] = tool.inputSchema;
	return descriptor;
}

JsonNode makeResourceDescriptor(const Protocol::Resource & resource)
{
	JsonNode descriptor;
	descriptor["uri"] = JsonNode(resource.uri);
	descriptor["name"] = JsonNode(resource.name);
	descriptor["description"] = JsonNode(resource.description);
	descriptor["mimeType"] = JsonNode(resource.mimeType);
	return descriptor;
}

JsonNode makeInvalidParamsToolResult(const std::string & message)
{
	return Protocol::makeTextResult(message, true).result;
}

}

void Protocol::registerTool(Tool tool)
{
	if(tool.name.empty())
		throw std::invalid_argument("MCP tool name must not be empty");
	if(!tool.handler)
		throw std::invalid_argument("MCP tool handler must be set");
	if(tool.inputSchema.isNull())
		tool.inputSchema = makeEmptyObject();

	tools[tool.name] = std::move(tool);
}

void Protocol::registerResource(Resource resource)
{
	if(resource.uri.empty())
		throw std::invalid_argument("MCP resource URI must not be empty");
	if(!resource.reader)
		throw std::invalid_argument("MCP resource reader must be set");
	if(resource.mimeType.empty())
		resource.mimeType = "text/plain";

	resources[resource.uri] = std::move(resource);
}

std::optional<std::string> Protocol::handleMessage(std::string_view payload) const
{
	JsonParsingSettings settings;
	settings.mode = JsonParsingSettings::JsonFormatMode::JSON;
	settings.strict = true;

	try
	{
		JsonNode request(payload.data(), payload.size(), settings, "mcp-json-rpc");
		auto response = handleRequest(request);
		if(!response)
			return std::nullopt;
		return response->toCompactString();
	}
	catch(const std::exception & exception)
	{
		JsonNode nullId;
		return makeErrorResponse(nullId, JsonRpcErrorCode::ParseError, exception.what()).toCompactString();
	}
}

Protocol::ToolResult Protocol::makeTextResult(const std::string & text, bool isError)
{
	JsonNode result;
	result["content"].Vector().push_back(makeContentText(text));
	result["isError"] = JsonNode(isError);
	return { result };
}

std::optional<JsonNode> Protocol::handleRequest(const JsonNode & request) const
{
	JsonNode nullId;

	if(!request.isStruct())
		return makeErrorResponse(nullId, JsonRpcErrorCode::InvalidRequest, "JSON-RPC request must be an object");

	const bool requestHasId = hasField(request, "id");
	const JsonNode & responseId = requestHasId ? request["id"] : nullId;

	if(!request["jsonrpc"].isString() || request["jsonrpc"].String() != "2.0")
		return makeErrorResponse(responseId, JsonRpcErrorCode::InvalidRequest, "JSON-RPC version must be 2.0");

	if(!request["method"].isString())
		return makeErrorResponse(responseId, JsonRpcErrorCode::InvalidRequest, "JSON-RPC method must be a string");

	const std::string & method = request["method"].String();

	try
	{
		JsonNode result;
		if(method == "initialize")
			result = handleInitialize();
		else if(method == "notifications/initialized")
			return std::nullopt;
		else if(method == "ping")
			result = makeEmptyObject();
		else if(method == "tools/list")
			result = handleToolsList();
		else if(method == "tools/call")
			result = handleToolsCall(request["params"]);
		else if(method == "resources/list")
			result = handleResourcesList();
		else if(method == "resources/read")
			result = handleResourcesRead(request["params"]);
		else if(requestHasId)
			return makeErrorResponse(responseId, JsonRpcErrorCode::MethodNotFound, "Unsupported MCP method: " + method);
		else
			return std::nullopt;

		if(!requestHasId)
			return std::nullopt;

		return makeResponse(responseId, result);
	}
	catch(const std::invalid_argument & exception)
	{
		if(!requestHasId)
			return std::nullopt;
		return makeErrorResponse(responseId, JsonRpcErrorCode::InvalidParams, exception.what());
	}
	catch(const std::exception & exception)
	{
		if(!requestHasId)
			return std::nullopt;
		return makeErrorResponse(responseId, JsonRpcErrorCode::InternalError, exception.what());
	}
}

JsonNode Protocol::handleInitialize() const
{
	JsonNode result;
	result["protocolVersion"] = JsonNode("2025-06-18");
	result["serverInfo"]["name"] = JsonNode("vcmi");
	result["serverInfo"]["version"] = JsonNode("0.1.0");
	result["capabilities"]["tools"]["listChanged"] = JsonNode(false);
	result["capabilities"]["resources"]["listChanged"] = JsonNode(false);
	return result;
}

JsonNode Protocol::handleToolsList() const
{
	JsonNode result;
	for(const auto & entry : tools)
		result["tools"].Vector().push_back(makeToolDescriptor(entry.second));
	return result;
}

JsonNode Protocol::handleToolsCall(const JsonNode & params) const
{
	if(!params.isStruct())
		throw std::invalid_argument("tools/call params must be an object");
	if(!params["name"].isString())
		throw std::invalid_argument("tools/call params.name must be a string");

	const std::string & toolName = params["name"].String();
	auto tool = tools.find(toolName);
	if(tool == tools.end())
		return makeInvalidParamsToolResult("Unknown MCP tool: " + toolName);

	JsonNode arguments = makeEmptyObject();
	if(hasField(params, "arguments"))
	{
		if(!params["arguments"].isStruct())
			throw std::invalid_argument("tools/call params.arguments must be an object");
		arguments = params["arguments"];
	}

	return tool->second.handler(arguments).result;
}

JsonNode Protocol::handleResourcesList() const
{
	JsonNode result;
	for(const auto & entry : resources)
		result["resources"].Vector().push_back(makeResourceDescriptor(entry.second));
	return result;
}

JsonNode Protocol::handleResourcesRead(const JsonNode & params) const
{
	if(!params.isStruct())
		throw std::invalid_argument("resources/read params must be an object");
	if(!params["uri"].isString())
		throw std::invalid_argument("resources/read params.uri must be a string");

	const std::string & uri = params["uri"].String();
	auto resource = resources.find(uri);
	if(resource == resources.end())
		throw std::invalid_argument("Unknown MCP resource: " + uri);

	JsonNode content;
	content["uri"] = JsonNode(resource->second.uri);
	content["mimeType"] = JsonNode(resource->second.mimeType);
	content["text"] = JsonNode(resource->second.reader());

	JsonNode result;
	result["contents"].Vector().push_back(content);
	return result;
}

}

VCMI_LIB_NAMESPACE_END
