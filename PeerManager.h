#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "PeerId.h"
#include "CryptoIdentity.h"
#include "Session.h"
#include "SessionCallbackHandler.h"
#include "PeerManagerCallbackHandler.h"

class Interface;

//manages all the sessions
//keeps track of which session is active
//displays the messages of the active session
class PeerManager : public SessionCallbackHandler
{
private:
	std::unordered_map<PeerId, std::shared_ptr<Session>> active_sessions;
	asio::ip::tcp::acceptor acceptor;
	std::shared_ptr<asio::ssl::context> tls_context;
	PeerInfo local;							//our own id and username
	asio::io_context* io;
	PeerManagerCallbackHandler& callbacks;
	std::optional<PeerId> cur_peer;			//the selected peer, if any
	bool stopped = false;
	mutable std::recursive_mutex state_mutex;
	
public:
	PeerManager(asio::io_context* io, PeerInfo local, const CryptoIdentity& identity, int port, PeerManagerCallbackHandler& callbacks);
	~PeerManager() override;

	void onPeerIdentified(const PeerInfo& peer, std::shared_ptr<Session> session) override;

	void connect(const std::string& address, unsigned short port);

	void selectPeer(const PeerId& peer);

	void deselectPeer();

	std::optional<PeerId> getSelectedPeer();

	void write(const std::string& msg);

	void read(const PeerId& peer_id, const std::string& msg) override;

	std::vector<PeerId> showPeers();

	void onPeerDisconnected(const PeerId& id, std::shared_ptr<Session> session) override;
	void onSessionError(std::optional<PeerId> peer_id, std::shared_ptr<Session> session,
		const asio::error_code& error) override;
	void stop() noexcept;
	bool isRunning() const noexcept;

private:
	void accept();
	void fail(const asio::error_code& error) noexcept;

};
