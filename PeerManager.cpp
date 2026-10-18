#include "PeerManager.h"

#include <stdexcept>
#include <charconv>
#include <utility>

namespace {

asio::io_context& requireIoContext(asio::io_context* context)
{
    if (!context) throw std::invalid_argument("PeerManager requires an io_context");
    return *context;
}

struct QuicCryptoRuntime {
    QuicCryptoRuntime()
    {
        if (ngtcp2_crypto_ossl_init() != 0)
            throw std::runtime_error("Initializing ngtcp2 OpenSSL backend failed");
    }
    ~QuicCryptoRuntime() { ngtcp2_crypto_ossl_free(); }
};

void ensureQuicCryptoRuntime()
{
    static QuicCryptoRuntime runtime;
    (void)runtime;
}

std::pair<std::string, std::uint16_t> parseRelayEndpoint(const std::string& value)
{
    constexpr std::uint16_t default_port = 7443;
    std::string host;
    std::string port_text;
    if (!value.empty() && value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string::npos) throw std::invalid_argument("Invalid bracketed relay address");
        host = value.substr(1, close - 1);
        if (close + 1 < value.size()) {
            if (value[close + 1] != ':') throw std::invalid_argument("Invalid relay address suffix");
            port_text = value.substr(close + 2);
        }
    } else {
        const auto first = value.find(':');
        const auto last = value.rfind(':');
        if (first != std::string::npos && first == last) {
            host = value.substr(0, first);
            port_text = value.substr(first + 1);
        } else {
            host = value;
        }
    }
    if (host.empty()) throw std::invalid_argument("Relay server host cannot be empty");
    if (port_text.empty()) return {host, default_port};
    unsigned port = 0;
    const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
        port == 0 || port > 65535)
        throw std::invalid_argument("Relay signaling port must be between 1 and 65535");
    return {host, static_cast<std::uint16_t>(port)};
}

} // namespace

PeerManager::PeerManager(asio::io_context* context, PeerInfo local_info,
    const CryptoIdentity& identity, int port, PeerManagerCallbackHandler& event_handler)
    : socket(requireIoContext(context)), tls_context(identity.makeTlsContext()), local(std::move(local_info)),
      io(context), callbacks(event_handler)
{
    if (port < 1 || port > 65535)
        throw asio::system_error(asio::error::make_error_code(asio::error::invalid_argument));

    asio::error_code ec;
    const UdpEndpoint endpoint(asio::ip::udp::v4(), static_cast<unsigned short>(port));
    socket.open(endpoint.protocol(), ec);
    if (ec) throw asio::system_error(ec);
#ifndef _WIN32
    socket.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) throw asio::system_error(ec);
#endif
    socket.bind(endpoint, ec);
    if (ec) throw asio::system_error(ec);

    ensureQuicCryptoRuntime();
    callbacks.onPeerManagerStarted(static_cast<uint16_t>(port), local);
    receive();
}

PeerManager::~PeerManager()
{
    stop();
}

bool PeerManager::isRunning() const noexcept
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return !stopped && socket.is_open();
}

void PeerManager::fail(const asio::error_code& error) noexcept
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (stopped) return;
    stop();
    try { callbacks.onPeerManagerError(error); } catch (...) {}
}

void PeerManager::stop() noexcept
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (stopped) return;
    stopped = true;
    asio::error_code ignored;
    socket.cancel(ignored);
    socket.close(ignored);
    for (auto& entry : active_sessions) entry.second->close();
    for (auto& entry : pending_sessions) entry.second->close();
    for (auto& entry : room_transports) entry.second.transport->close();
    active_sessions.clear();
    pending_sessions.clear();
    room_transports.clear();
    session_rooms.clear();
    cid_sessions.clear();
    send_queue.clear();
    cur_peer.reset();
}

void PeerManager::receive()
{
    if (!isRunning()) return;
    socket.async_receive_from(asio::buffer(receive_buffer), receive_endpoint,
        [this](const asio::error_code& ec, std::size_t bytes) {
            if (ec) {
                if (ec != asio::error::operation_aborted) fail(ec);
                return;
            }
            const UdpEndpoint source = receive_endpoint;
            const std::vector<uint8_t> received(receive_buffer.begin(),
                receive_buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
            receive();
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            if (stopped || bytes == 0) return;

            const uint8_t* packet = received.data();
            ngtcp2_version_cid version_cid{};
            const int decode_rv = ngtcp2_pkt_decode_version_cid(&version_cid,
                packet, bytes, Session::kConnectionIdLength);
            if (decode_rv == 0 && version_cid.dcid && version_cid.dcidlen > 0) {
                const std::string key(reinterpret_cast<const char*>(version_cid.dcid),
                    version_cid.dcidlen);
                const auto it = cid_sessions.find(key);
                if (it != cid_sessions.end()) {
                    it->second->onDatagram(packet, received.size(), source);
                    return;
                }
            }

            ngtcp2_pkt_hd initial_header{};
            if (ngtcp2_accept(&initial_header, packet, bytes) != 0) return;
            try {
                auto session = std::make_shared<Session>(socket, source,
                    tls_context, local, this, ConnectionDirection::Incoming,
                    [this](const UdpEndpoint& target, std::vector<uint8_t> data) {
                        sendDatagram(target, std::move(data));
                    },
                    [this](const std::string& cid, std::shared_ptr<Session> owner) {
                        registerCid(cid, std::move(owner));
                    });
                pending_sessions.emplace(session.get(), session);
                callbacks.onPeerConnection(true, {});
                session->startServer(initial_header, packet, received.size());
            } catch (...) {
                callbacks.onPeerSessionError(true, PeerId{},
                    asio::error::make_error_code(asio::error::fault));
            }
        });
}

void PeerManager::sendDatagram(const UdpEndpoint& endpoint, std::vector<uint8_t> packet)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (stopped || !socket.is_open() || packet.empty()) return;
    send_queue.push_back({endpoint, std::move(packet)});
    flushDatagrams();
}

void PeerManager::flushDatagrams()
{
    if (stopped || sending_datagram || send_queue.empty()) return;
    sending_datagram = true;
    auto& datagram = send_queue.front();
    socket.async_send_to(asio::buffer(datagram.bytes), datagram.endpoint,
        [this](const asio::error_code& ec, std::size_t) {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            sending_datagram = false;
            if (!send_queue.empty()) send_queue.pop_front();
            if (ec && ec != asio::error::operation_aborted) {
                fail(ec);
                return;
            }
            flushDatagrams();
        });
}

void PeerManager::registerCid(const std::string& cid, std::shared_ptr<Session> session)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (stopped) throw std::runtime_error("Peer manager is stopped");
    const auto [it, inserted] = cid_sessions.emplace(cid, session);
    if (!inserted && it->second != session)
        throw std::runtime_error("QUIC connection ID collision");
    pending_sessions.emplace(session.get(), std::move(session));
}

void PeerManager::removeRoutes(Session* session)
{
    pending_sessions.erase(session);
    for (auto it = cid_sessions.begin(); it != cid_sessions.end();) {
        if (it->second.get() == session) it = cid_sessions.erase(it);
        else ++it;
    }
}

void PeerManager::onPeerIdentified(const PeerInfo& peer, std::shared_ptr<Session> session)
{
    const PeerId& id = peer.id;
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    pending_sessions.erase(session.get());
    if (stopped || id == local.id) {
        if (id == local.id)
            callbacks.onPeerSessionError(true, id, asio::error::make_error_code(asio::error::fault));
        session->close();
        removeRoutes(session.get());
        removeRoomTransport(session.get());
        return;
    }
    auto it = active_sessions.find(id);
    if (it == active_sessions.end()) {
        active_sessions.emplace(id, std::move(session));
        callbacks.onPeerIdentified(peer);
        return;
    }

    // On simultaneous connects, the smaller ID keeps outgoing; the larger keeps incoming.
    const bool keepNew = local.id < id
        ? session->getDirection() == ConnectionDirection::Outgoing
        : session->getDirection() == ConnectionDirection::Incoming;
    if (keepNew) {
        auto old = std::move(it->second);
        it->second = std::move(session);
        removeRoutes(old.get());
        old->close();
        removeRoomTransport(old.get());
    } else {
        removeRoutes(session.get());
        session->close();
        removeRoomTransport(session.get());
    }
    callbacks.onDuplicatePeerConnection(id, keepNew);
}

void PeerManager::connect(const std::string& address, unsigned short port)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (!isRunning()) {
        callbacks.onPeerConnection(false, asio::error::make_error_code(asio::error::not_connected));
        return;
    }
    asio::error_code ec;
    auto ip = asio::ip::make_address(address, ec);
    if (ec) { callbacks.onPeerConnection(false, ec); return; }
    if (ip.is_v4() != socket.local_endpoint().address().is_v4()) {
        callbacks.onPeerConnection(false, asio::error::make_error_code(asio::error::address_family_not_supported));
        return;
    }
    const UdpEndpoint endpoint(ip, port);
    try {
        auto session = std::make_shared<Session>(socket, endpoint, tls_context, local,
            this, ConnectionDirection::Outgoing,
            [this](const UdpEndpoint& target, std::vector<uint8_t> data) {
                sendDatagram(target, std::move(data));
            },
            [this](const std::string& cid, std::shared_ptr<Session> owner) {
                registerCid(cid, std::move(owner));
            });
        pending_sessions.emplace(session.get(), session);
        session->initiateHandshake();
        session->start();
        callbacks.onPeerConnection(false, {});
    } catch (...) {
        callbacks.onPeerConnection(false, asio::error::make_error_code(asio::error::fault));
    }
}

void PeerManager::connectRoom(const std::string& room_token, const std::string& relay_server)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (!isRunning()) {
        callbacks.onPeerOperationError(asio::error::make_error_code(asio::error::not_connected));
        return;
    }
    try {
        auto [host, port] = parseRelayEndpoint(relay_server);
        auto room = std::make_shared<RoomConnection>(*io, std::move(host), port, room_token,
            [this](std::shared_ptr<RoomConnection> connection, UdpEndpoint local_path,
                UdpEndpoint remote_path, bool quic_client) {
                onRoomReady(std::move(connection), std::move(local_path),
                    std::move(remote_path), quic_client);
            },
            [this](std::shared_ptr<RoomConnection> connection, std::vector<std::uint8_t> packet) {
                onRoomDatagram(std::move(connection), std::move(packet));
            },
            [this](std::shared_ptr<RoomConnection> connection, std::string error) {
                onRoomError(std::move(connection), std::move(error));
            });
        room_transports.emplace(room.get(), RoomTransportState{room});
        room->start();
    } catch (const std::exception& error) {
        callbacks.onPeerRoomError(error.what());
    }
}

void PeerManager::onRoomReady(std::shared_ptr<RoomConnection> room,
    UdpEndpoint local_path, UdpEndpoint remote_path, bool quic_client)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    const auto found = room_transports.find(room.get());
    if (found == room_transports.end() || stopped) { room->close(); return; }
    auto& state = found->second;
    if (state.path_ready) return;
    state.local_endpoint = std::move(local_path);
    state.remote_endpoint = std::move(remote_path);
    state.quic_client = quic_client;
    state.path_ready = true;
    if (!quic_client) return;

    try {
        auto session = std::make_shared<Session>(socket, state.remote_endpoint,
            tls_context, local, this, ConnectionDirection::Outgoing,
            [room](const UdpEndpoint&, std::vector<std::uint8_t> packet) {
                (void)room->sendDatagram(packet);
            },
            [this](const std::string& cid, std::shared_ptr<Session> owner) {
                registerCid(cid, std::move(owner));
            }, state.local_endpoint);
        state.session = session;
        session_rooms.emplace(session.get(), room.get());
        pending_sessions.emplace(session.get(), session);
        session->initiateHandshake();
        session->start();
        callbacks.onPeerConnection(false, {});
    } catch (const std::exception& error) {
        onRoomError(std::move(room), error.what());
    }
}

void PeerManager::onRoomDatagram(std::shared_ptr<RoomConnection> room,
    std::vector<std::uint8_t> packet)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    const auto found = room_transports.find(room.get());
    if (found == room_transports.end() || stopped || packet.empty()) return;
    auto& state = found->second;
    if (!state.path_ready) return;
    if (state.session) {
        state.session->onDatagram(packet.data(), packet.size(), state.remote_endpoint);
        return;
    }
    if (state.quic_client) return;

    ngtcp2_pkt_hd initial_header{};
    if (ngtcp2_accept(&initial_header, packet.data(), packet.size()) != 0) return;
    try {
        auto session = std::make_shared<Session>(socket, state.remote_endpoint,
            tls_context, local, this, ConnectionDirection::Incoming,
            [room](const UdpEndpoint&, std::vector<std::uint8_t> outgoing) {
                (void)room->sendDatagram(outgoing);
            },
            [this](const std::string& cid, std::shared_ptr<Session> owner) {
                registerCid(cid, std::move(owner));
            }, state.local_endpoint);
        state.session = session;
        session_rooms.emplace(session.get(), room.get());
        pending_sessions.emplace(session.get(), session);
        callbacks.onPeerConnection(true, {});
        session->startServer(initial_header, packet.data(), packet.size());
    } catch (const std::exception& error) {
        onRoomError(std::move(room), error.what());
    }
}

void PeerManager::onRoomError(std::shared_ptr<RoomConnection> room, std::string error)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    const auto found = room_transports.find(room.get());
    if (found != room_transports.end()) {
        auto session = found->second.session;
        if (session) {
            pending_sessions.erase(session.get());
            session_rooms.erase(session.get());
            removeRoutes(session.get());
            for (auto it = active_sessions.begin(); it != active_sessions.end();) {
                if (it->second == session) {
                    const PeerId id = it->first;
                    it = active_sessions.erase(it);
                    if (cur_peer == id) cur_peer.reset();
                    callbacks.onPeerDisconnected(id);
                } else ++it;
            }
            session->close();
        }
        room_transports.erase(found);
    }
    room->close();
    callbacks.onPeerRoomError(error);
}

void PeerManager::removeRoomTransport(Session* session)
{
    const auto association = session_rooms.find(session);
    if (association == session_rooms.end()) return;
    const auto found = room_transports.find(association->second);
    session_rooms.erase(association);
    if (found != room_transports.end()) {
        found->second.transport->close();
        room_transports.erase(found);
    }
}

void PeerManager::selectPeer(const PeerId& peer)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    auto it = active_sessions.find(peer);
    if (it == active_sessions.end()) {
        callbacks.onPeerOperationError(asio::error::make_error_code(asio::error::not_connected));
        return;
    }
    std::shared_ptr<Session> old_session;
    if (cur_peer) {
        auto old = active_sessions.find(*cur_peer);
        if (old != active_sessions.end()) old_session = old->second;
    }
    cur_peer = peer;
    auto selected = it->second;
    asio::post(*io, [old_session, selected] {
        if (old_session) old_session->deselectPeer();
        selected->selectPeer();
    });
}

void PeerManager::deselectPeer()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (!cur_peer) return;
    auto it = active_sessions.find(*cur_peer);
    cur_peer.reset();
    if (it == active_sessions.end()) return;
    auto session = it->second;
    asio::post(*io, [session] { session->deselectPeer(); });
}

void PeerManager::write(const std::string& msg)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    auto it = cur_peer ? active_sessions.find(*cur_peer) : active_sessions.end();
    if (it == active_sessions.end()) {
        callbacks.onPeerOperationError(asio::error::make_error_code(asio::error::not_connected));
        return;
    }
    auto session = it->second;
    const PeerId target = *cur_peer;
    try {
        asio::post(*io, [this, session, target, msg] {
            try {
                session->queueMessage({MessageType::Chat, std::vector<char>(msg.begin(), msg.end())});
                callbacks.onLocalMessageSent(target, msg);
            } catch (const std::exception&) {
                callbacks.onPeerOperationError(asio::error::make_error_code(asio::error::message_size));
            }
        });
    } catch (...) {
        callbacks.onPeerOperationError(asio::error::make_error_code(asio::error::fault));
    }
}

void PeerManager::read(const PeerId& id, const std::string& msg)
{
    callbacks.onPeerMessage(id, msg);
}

std::optional<PeerId> PeerManager::getSelectedPeer()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return cur_peer;
}

std::vector<PeerId> PeerManager::showPeers()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    std::vector<PeerId> peers;
    peers.reserve(active_sessions.size());
    for (const auto& [id, session] : active_sessions) { (void)session; peers.push_back(id); }
    return peers;
}

void PeerManager::onPeerDisconnected(const PeerId& id, std::shared_ptr<Session> session)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    auto it = active_sessions.find(id);
    if (it == active_sessions.end() || it->second != session) return;
    active_sessions.erase(it);
    if (cur_peer == id) cur_peer.reset();
    removeRoutes(session.get());
    removeRoomTransport(session.get());
    callbacks.onPeerDisconnected(id);
}

void PeerManager::onSessionError(std::optional<PeerId> id, std::shared_ptr<Session> session,
    const asio::error_code& error)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    removeRoutes(session.get());
    removeRoomTransport(session.get());
    if (id) {
        auto it = active_sessions.find(*id);
        if (it != active_sessions.end() && it->second == session) {
            active_sessions.erase(it);
            if (cur_peer == *id) cur_peer.reset();
            callbacks.onPeerDisconnected(*id);
        }
    }
    callbacks.onPeerSessionError(id.has_value(), id.value_or(PeerId{}), error);
}
