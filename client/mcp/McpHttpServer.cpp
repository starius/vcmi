/*
 * McpHttpServer.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "McpHttpServer.h"

#include "../../lib/logging/CLogger.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

McpHttpServer::McpHttpServer(Mcp::Protocol & protocol, uint16_t port, std::string authToken)
	: protocol(protocol)
	, port(port)
	, authToken(std::move(authToken))
{
}

McpHttpServer::~McpHttpServer()
{
	stop();
}

void McpHttpServer::start()
{
	acceptor = std::make_unique<tcp::acceptor>(ioContext);
	tcp::endpoint endpoint{boost::asio::ip::address_v4::loopback(), port};
	acceptor->open(endpoint.protocol());
	acceptor->set_option(tcp::acceptor::reuse_address(true));
	acceptor->bind(endpoint);
	acceptor->listen();
	port = acceptor->local_endpoint().port();

	doAccept();
	ioThread = std::thread([this]()
	{
		ioContext.run();
	});
}

void McpHttpServer::stop()
{
	if(acceptor && acceptor->is_open())
	{
		boost::system::error_code ec;
		acceptor->close(ec);
	}

	ioContext.stop();
	if(ioThread.joinable())
		ioThread.join();
}

uint16_t McpHttpServer::getPort() const
{
	return port;
}

void McpHttpServer::doAccept()
{
	acceptor->async_accept([this](boost::system::error_code ec, tcp::socket socket)
	{
		if(!ec)
		{
			auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
			auto buffer = std::make_shared<beast::flat_buffer>();
			auto request = std::make_shared<Request>();

			http::async_read(*stream, *buffer, *request, [this, stream, buffer, request](boost::system::error_code readEc, std::size_t)
			{
				if(readEc)
				{
					if(readEc != http::error::end_of_stream)
						logGlobal->warn("MCP HTTP read error: %s", readEc.message());
					return;
				}

				auto response = std::make_shared<Response>(handleRequest(std::move(*request)));
				http::async_write(*stream, *response, [stream, response](boost::system::error_code, std::size_t)
				{
					beast::error_code shutdownEc;
					stream->socket().shutdown(tcp::socket::shutdown_send, shutdownEc);
				});
			});
		}
		else if(ec != boost::asio::error::operation_aborted)
		{
			logGlobal->warn("MCP HTTP accept error: %s", ec.message());
		}

		if(acceptor && acceptor->is_open())
			doAccept();
	});
}

McpHttpServer::Response McpHttpServer::handleRequest(Request && request)
{
	if(request.method() == http::verb::options)
	{
		auto response = makeResponse(request, http::status::no_content, "");
		response.set(http::field::access_control_allow_origin, "http://localhost");
		response.set(http::field::access_control_allow_methods, "POST, OPTIONS");
		response.set(http::field::access_control_allow_headers, "authorization, content-type, x-vcmi-mcp-token");
		return response;
	}

	if(request.target() == "/health")
		return makeResponse(request, http::status::ok, R"({"ok":true})");

	if(request.target() != "/mcp")
		return makeResponse(request, http::status::not_found, R"({"error":"not found"})");

	if(request.method() != http::verb::post)
		return makeResponse(request, http::status::method_not_allowed, R"({"error":"POST required"})");

	if(!isAuthorized(request))
		return makeResponse(request, http::status::unauthorized, R"({"error":"unauthorized"})");

	auto responsePayload = protocol.handleMessage(request.body());
	if(!responsePayload)
		return makeResponse(request, http::status::accepted, "");

	return makeResponse(request, http::status::ok, *responsePayload);
}

McpHttpServer::Response McpHttpServer::makeResponse(const Request & request, http::status status, std::string body, const std::string & contentType) const
{
	Response response{status, request.version()};
	response.set(http::field::server, "VCMI-MCP");
	if(!body.empty())
		response.set(http::field::content_type, contentType);
	response.keep_alive(false);
	response.body() = std::move(body);
	response.prepare_payload();
	return response;
}

bool McpHttpServer::isAuthorized(const Request & request) const
{
	if(authToken.empty())
		return true;

	const std::string expectedAuthorization = "Bearer " + authToken;
	if(std::string(request[http::field::authorization]) == expectedAuthorization)
		return true;

	return std::string(request["x-vcmi-mcp-token"]) == authToken;
}
