/*
 * McpHttpServer.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "McpProtocol.h"

class McpHttpServer
{
public:
	McpHttpServer(Mcp::Protocol & protocol, uint16_t port, std::string authToken);
	~McpHttpServer();

	void start();
	void stop();

	uint16_t getPort() const;

private:
	using Request = boost::beast::http::request<boost::beast::http::string_body>;
	using Response = boost::beast::http::response<boost::beast::http::string_body>;

	void doAccept();
	Response handleRequest(Request && request);

	Response makeResponse(const Request & request, boost::beast::http::status status, std::string body, const std::string & contentType = "application/json") const;
	bool isAuthorized(const Request & request) const;

	Mcp::Protocol & protocol;
	uint16_t port;
	std::string authToken;
	boost::asio::io_context ioContext;
	std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor;
	std::thread ioThread;
};
