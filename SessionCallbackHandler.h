#pragma once

#include <cstdint>
#include <memory>
#include <string>

class Session;

//interface for managing the callback when session recieves the remote id
class SessionCallbackHandler
{
public:
	virtual ~SessionCallbackHandler() = default;

	virtual void onPeerIdentified(uint8_t id, std::shared_ptr<Session> session) = 0;

	virtual void read(const std::string& msg) = 0;

	virtual void onPeerDisconnected(uint8_t id, std::shared_ptr<Session> session) = 0;
};
