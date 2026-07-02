#include "StdInc.h"

#include "../../../client/mcp/McpProtocol.h"

VCMI_LIB_NAMESPACE_BEGIN

namespace
{

JsonNode parseResponse(const std::optional<std::string> & payload)
{
	EXPECT_TRUE(payload.has_value());

	JsonParsingSettings settings;
	settings.mode = JsonParsingSettings::JsonFormatMode::JSON;
	settings.strict = true;

	return JsonNode(payload->data(), payload->size(), settings, "mcp-test-response");
}

JsonNode objectSchema()
{
	JsonNode schema;
	schema["type"] = JsonNode("object");
	return schema;
}

}

TEST(McpProtocolTest, InitializeReturnsCapabilities)
{
	Mcp::Protocol protocol;

	auto response = parseResponse(protocol.handleMessage(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})"));

	EXPECT_EQ(response["jsonrpc"].String(), "2.0");
	EXPECT_EQ(response["id"].Integer(), 1);
	EXPECT_EQ(response["result"]["protocolVersion"].String(), "2025-06-18");
	EXPECT_EQ(response["result"]["serverInfo"]["name"].String(), "vcmi");
	EXPECT_TRUE(response["result"]["capabilities"]["tools"].isStruct());
	EXPECT_TRUE(response["result"]["capabilities"]["resources"].isStruct());
}

TEST(McpProtocolTest, InitializedNotificationHasNoResponse)
{
	Mcp::Protocol protocol;

	EXPECT_FALSE(protocol.handleMessage(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").has_value());
}

TEST(McpProtocolTest, ListsAndCallsRegisteredTool)
{
	Mcp::Protocol protocol;
	protocol.registerTool({
		"vcmi.echo",
		"Echoes the supplied text.",
		objectSchema(),
		[](const JsonNode & arguments)
		{
			return Mcp::Protocol::makeTextResult(arguments["text"].String());
		}
	});

	auto listResponse = parseResponse(protocol.handleMessage(R"({"jsonrpc":"2.0","id":"list","method":"tools/list"})"));
	ASSERT_EQ(listResponse["result"]["tools"].Vector().size(), 1);
	EXPECT_EQ(listResponse["result"]["tools"][0]["name"].String(), "vcmi.echo");
	EXPECT_EQ(listResponse["result"]["tools"][0]["inputSchema"]["type"].String(), "object");

	auto callResponse = parseResponse(protocol.handleMessage(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vcmi.echo","arguments":{"text":"hello"}}})"));
	EXPECT_EQ(callResponse["id"].Integer(), 2);
	EXPECT_EQ(callResponse["result"]["content"][0]["type"].String(), "text");
	EXPECT_EQ(callResponse["result"]["content"][0]["text"].String(), "hello");
	EXPECT_FALSE(callResponse["result"]["isError"].Bool());
}

TEST(McpProtocolTest, UnknownToolReturnsMcpToolError)
{
	Mcp::Protocol protocol;

	auto response = parseResponse(protocol.handleMessage(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vcmi.missing","arguments":{}}})"));

	EXPECT_EQ(response["id"].Integer(), 3);
	EXPECT_TRUE(response["result"]["isError"].Bool());
	EXPECT_EQ(response["result"]["content"][0]["type"].String(), "text");
	EXPECT_NE(response["result"]["content"][0]["text"].String().find("Unknown MCP tool"), std::string::npos);
}

TEST(McpProtocolTest, ListsAndReadsRegisteredResource)
{
	Mcp::Protocol protocol;
	protocol.registerResource({
		"vcmi://state",
		"Current VCMI state",
		"Visible game state for the MCP-controlled player.",
		"application/json",
		[] { return R"({"turnActive":true})"; }
	});

	auto listResponse = parseResponse(protocol.handleMessage(R"({"jsonrpc":"2.0","id":4,"method":"resources/list"})"));
	ASSERT_EQ(listResponse["result"]["resources"].Vector().size(), 1);
	EXPECT_EQ(listResponse["result"]["resources"][0]["uri"].String(), "vcmi://state");
	EXPECT_EQ(listResponse["result"]["resources"][0]["mimeType"].String(), "application/json");

	auto readResponse = parseResponse(protocol.handleMessage(R"({"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"vcmi://state"}})"));
	ASSERT_EQ(readResponse["result"]["contents"].Vector().size(), 1);
	EXPECT_EQ(readResponse["result"]["contents"][0]["uri"].String(), "vcmi://state");
	EXPECT_EQ(readResponse["result"]["contents"][0]["text"].String(), R"({"turnActive":true})");
}

TEST(McpProtocolTest, ReportsJsonRpcErrors)
{
	Mcp::Protocol protocol;
	protocol.registerTool({
		"vcmi.echo",
		"Echoes the supplied text.",
		objectSchema(),
		[](const JsonNode & arguments)
		{
			return Mcp::Protocol::makeTextResult(arguments["text"].String());
		}
	});

	auto parseError = parseResponse(protocol.handleMessage(R"({"jsonrpc":"2.0")"));
	EXPECT_EQ(parseError["id"].getType(), JsonNode::JsonType::DATA_NULL);
	EXPECT_EQ(parseError["error"]["code"].Integer(), -32700);

	auto methodError = parseResponse(protocol.handleMessage(R"({"jsonrpc":"2.0","id":6,"method":"missing/method"})"));
	EXPECT_EQ(methodError["id"].Integer(), 6);
	EXPECT_EQ(methodError["error"]["code"].Integer(), -32601);

	auto paramsError = parseResponse(protocol.handleMessage(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"vcmi.echo","arguments":[]}})"));
	EXPECT_EQ(paramsError["id"].Integer(), 7);
	EXPECT_EQ(paramsError["error"]["code"].Integer(), -32602);
}

VCMI_LIB_NAMESPACE_END
