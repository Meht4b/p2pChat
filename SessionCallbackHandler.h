#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <asio/error_code.hpp>

class Session;

//interface for managing the callback when session recieves the remote id
class SessionCallbackHandler
{
public:
	virtual ~SessionCallbackHandler() = default;

	virtual void onPeerIdentified(uint8_t id, std::shared_ptr<Session> session) = 0;

	virtual void read(uint8_t peer_id, const std::string& msg) = 0;

	virtual void onPeerDisconnected(uint8_t id, std::shared_ptr<Session> session) = 0;
	virtual void onSessionError(std::optional<uint8_t> peer_id,
		std::shared_ptr<Session> session, const asio::error_code& error) = 0;
};
