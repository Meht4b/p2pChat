#pragma once

#include <asio.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Session.h"
#include "SessionCallbackHandler.h"

class Interface;

//manages all the sessions
//keeps track of which session is active
//displays the messages of the active session
class PeerManager : public SessionCallbackHandler
{
private:
	std::unordered_map<uint8_t, std::shared_ptr<Session>> active_sessions;
	asio::ip::tcp::acceptor acceptor;
	uint8_t peer_id;
	asio::io_context* io;
	Interface& interface;
	int cur_peer = -1;
	bool stopped = false;
	mutable std::recursive_mutex state_mutex;
	
public:
	PeerManager(asio::io_context* io, uint8_t peer_id, int port, Interface& interface);
	~PeerManager() override;

	void onPeerIdentified(uint8_t id, std::shared_ptr<Session> session) override;

	void connect(const std::string& address, unsigned short port);

	void selectPeer(int user);

	void write(const std::string& msg);

	void read(const std::string& msg) override;

	std::vector<uint8_t> showPeers();

	void onPeerDisconnected(uint8_t id, std::shared_ptr<Session> session) override;
	void stop() noexcept;
	bool isRunning() const noexcept;

private:
	void accept();
	void fail(const std::string& reason) noexcept;

};
