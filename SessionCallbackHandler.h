#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <asio/error_code.hpp>

#include "PeerId.h"

class Session;

//interface for managing the callback when session recieves the remote id
class SessionCallbackHandler
{
public:
	virtual ~SessionCallbackHandler() = default;

	//handshake finished: the remote id and username are known
	virtual void onPeerIdentified(const PeerInfo& peer, std::shared_ptr<Session> session) = 0;

	virtual void read(const PeerId& peer_id, const std::string& msg) = 0;

	virtual void onPeerDisconnected(const PeerId& id, std::shared_ptr<Session> session) = 0;
	virtual void onSessionError(std::optional<PeerId> peer_id,
		std::shared_ptr<Session> session, const asio::error_code& error) = 0;
};
