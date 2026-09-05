#pragma once

#include <asio.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "Session.h"
#include "SessionCallbackHandler.h"

//manages all the sessions
//keeps track of which session is active
//displays the messages of the active session
class PeerManager : public SessionCallbackHandler
{
private:
	std::unordered_map<uint8_t, std::shared_ptr<Session>> active_sessions;
	asio::ip::tcp::socket server;
	asio::ip::tcp::acceptor acceptor;
	uint8_t peer_id;
	asio::io_context* io;
	
public:
	PeerManager(asio::io_context* io, uint8_t peer_id, int port);

	void onPeerIdentified(uint8_t id, std::shared_ptr<Session> session) override;

	void publicConnect();

private:
	void accept();

	void connect(const std::string& address, unsigned short port);
	
};