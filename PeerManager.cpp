#include "PeerManager.h"

#include <stdexcept>
#include <utility>
#include <mutex>

PeerManager::PeerManager(asio::io_context* context, PeerInfo local_info, const CryptoIdentity& identity, int port,
    PeerManagerCallbackHandler& event_handler)
    : acceptor(*context), tls_context(identity.makeTlsContext()), local(std::move(local_info)), io(context), callbacks(event_handler)
{
    if (!context) throw std::invalid_argument("PeerManager requires an io_context");
    if (port < 1 || port > 65535)
        throw asio::system_error(asio::error::make_error_code(asio::error::invalid_argument));

    asio::error_code ec;
    // Listen on the machine's network interfaces so other LAN peers can connect.
    const asio::ip::tcp::endpoint endpoint(
	asio::ip::address_v4::any(), static_cast<unsigned short>(port));


    acceptor.open(endpoint.protocol(), ec);
    if (ec) throw asio::system_error(ec);
#ifndef _WIN32
    // The app listens automatically on every launch, so a quick restart must not fail because
    // the previous run's connections are still in TIME_WAIT. (Not done on Windows, where
    // SO_REUSEADDR would let another process share the port.)
    acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) throw asio::system_error(ec);
#endif
    acceptor.bind(endpoint, ec);
    if (ec) throw asio::system_error(ec);
    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) throw asio::system_error(ec);

    callbacks.onPeerManagerStarted(static_cast<uint16_t>(port), local);
    accept();
}

PeerManager::~PeerManager() { stop(); }

bool PeerManager::isRunning() const noexcept
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return !stopped && acceptor.is_open();
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
    acceptor.cancel(ignored);
    acceptor.close(ignored);
    for (auto& entry : active_sessions) entry.second->close();
    active_sessions.clear();
    cur_peer.reset();
}

void PeerManager::onPeerIdentified(const PeerInfo& peer, std::shared_ptr<Session> session)
{
    const PeerId& id = peer.id;
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (stopped || id == local.id) {
        if (id == local.id)
            callbacks.onPeerSessionError(true, id, asio::error::make_error_code(asio::error::fault));
        session->close();
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
        old->close();
    } else {
        session->close();
    }
    callbacks.onDuplicatePeerConnection(id, keepNew);
}

void PeerManager::accept()
{
    if (!isRunning()) return;
    acceptor.async_accept([this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        if (stopped) return;
        if (ec) {
            if (ec != asio::error::operation_aborted) fail(ec);
            return;
        }
        callbacks.onPeerConnection(true, ec);
        try {
            asio::ssl::stream<asio::ip::tcp::socket> tls(std::move(socket), *tls_context);
            auto session = std::make_shared<Session>(std::move(tls), local, this,
                ConnectionDirection::Incoming);
            session->initiateHandshake();
            session->start();
            accept();
        } catch (const std::exception&) { fail(asio::error::make_error_code(asio::error::fault)); }
        catch (...) { fail(asio::error::make_error_code(asio::error::fault)); }
    });
}

void PeerManager::connect(const std::string& address, unsigned short port)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (!isRunning()) {
        callbacks.onPeerConnection(false, asio::error::make_error_code(asio::error::not_connected));
        return;
    }
    try {
        asio::error_code ec;
        auto ip = asio::ip::make_address(address, ec);
        if (ec) { callbacks.onPeerConnection(false, ec); return; }
        auto socket = std::make_shared<asio::ip::tcp::socket>(*io);
        const asio::ip::tcp::endpoint endpoint(ip, port);
        socket->async_connect(endpoint, [this, socket](const asio::error_code& error) {
            std::lock_guard<std::recursive_mutex> callback_lock(state_mutex);
            if (stopped) return;
            callbacks.onPeerConnection(false, error);
            if (error) return;
            try {
                asio::ssl::stream<asio::ip::tcp::socket> tls(std::move(*socket), *tls_context);
                auto session = std::make_shared<Session>(std::move(tls), local, this,
                    ConnectionDirection::Outgoing);
                session->initiateHandshake();
                session->start();
            } catch (const std::exception&) {
                callbacks.onPeerSessionError(false, PeerId{}, asio::error::make_error_code(asio::error::fault));
            } catch (...) {
                callbacks.onPeerSessionError(false, PeerId{}, asio::error::make_error_code(asio::error::fault));
            }
        });
    } catch (const asio::system_error& e) { callbacks.onPeerConnection(false, e.code()); }
    catch (...) { callbacks.onPeerConnection(false, asio::error::make_error_code(asio::error::fault)); }
}

std::vector<PeerId> PeerManager::showPeers()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    std::vector<PeerId> peers;
    peers.reserve(active_sessions.size());
    for (const auto& [id, session] : active_sessions) peers.push_back(id);
    return peers;
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

std::optional<PeerId> PeerManager::getSelectedPeer() {
	std::lock_guard<std::recursive_mutex> lock(state_mutex); return cur_peer;
}

void PeerManager::onPeerDisconnected(const PeerId& id, std::shared_ptr<Session> session)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    auto it = active_sessions.find(id);
    if (it == active_sessions.end() || it->second != session) return;
    active_sessions.erase(it);
    if (cur_peer == id) cur_peer.reset();
    callbacks.onPeerDisconnected(id);
}

void PeerManager::onSessionError(std::optional<PeerId> id, std::shared_ptr<Session> session,
    const asio::error_code& error)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
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
