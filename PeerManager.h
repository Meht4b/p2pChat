#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <cstdint>
#include <deque>
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

// Owns the shared UDP socket and routes UDP payloads to QUIC sessions by
// connection ID. It does not know whether a payload arrived directly, through
// an ICE-selected path, or through a future relay adapter.
class PeerManager : public SessionCallbackHandler
{
private:
    using UdpEndpoint = asio::ip::udp::endpoint;
    struct PendingDatagram { UdpEndpoint endpoint; std::vector<uint8_t> bytes; };

    std::unordered_map<PeerId, std::shared_ptr<Session>> active_sessions;
    std::unordered_map<std::string, std::shared_ptr<Session>> cid_sessions;
    std::unordered_map<Session*, std::shared_ptr<Session>> pending_sessions;
    asio::ip::udp::socket socket;
    std::shared_ptr<asio::ssl::context> tls_context;
    PeerInfo local;
    asio::io_context* io;
    PeerManagerCallbackHandler& callbacks;
    std::optional<PeerId> cur_peer;
    std::array<uint8_t, 65536> receive_buffer{};
    UdpEndpoint receive_endpoint;
    std::deque<PendingDatagram> send_queue;
    bool sending_datagram = false;
    bool stopped = false;
    mutable std::recursive_mutex state_mutex;

public:
    PeerManager(asio::io_context* io, PeerInfo local, const CryptoIdentity& identity,
        int port, PeerManagerCallbackHandler& callbacks);
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
    void receive();
    void sendDatagram(const UdpEndpoint& endpoint, std::vector<uint8_t> packet);
    void flushDatagrams();
    void registerCid(const std::string& cid, std::shared_ptr<Session> session);
    void removeRoutes(Session* session);
    void fail(const asio::error_code& error) noexcept;
};
