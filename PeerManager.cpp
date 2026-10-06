#include "PeerManager.h"
#include "Interface.h"

#include <stdexcept>
#include <utility>
#include <mutex>

PeerManager::PeerManager(asio::io_context* context, uint8_t id, int port, Interface& ui)
    : acceptor(*context), peer_id(id), io(context), interface(ui)
{
    if (!context) throw std::invalid_argument("PeerManager requires an io_context");
    if (port < 1 || port > 65535) throw std::invalid_argument("Port must be between 1 and 65535");
    asio::error_code ec;
    const auto endpoint = asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1", ec),
        static_cast<unsigned short>(port));
    if (ec) throw std::runtime_error("Invalid listen address: " + ec.message());
    acceptor.open(endpoint.protocol(), ec);
    if (ec) throw std::runtime_error("Failed to open acceptor: " + ec.message());
    acceptor.bind(endpoint, ec);
    if (ec) throw std::runtime_error("Failed to bind port " + std::to_string(port) + ": " + ec.message());
    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) throw std::runtime_error("Failed to listen: " + ec.message());
    interface.printLineSuccess("Peer manager started on port = " + std::to_string(port) +
        " user id = " + std::to_string(peer_id));
    accept();
}

PeerManager::~PeerManager() { stop(); }

bool PeerManager::isRunning() const noexcept
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return !stopped && acceptor.is_open();
}

void PeerManager::fail(const std::string& reason) noexcept
{
    if (stopped) return;
    try { interface.printLineError("Peer manager stopped: " + reason); } catch (...) {}
    stop();
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
    cur_peer = -1;
}

void PeerManager::onPeerIdentified(uint8_t id, std::shared_ptr<Session> session)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (stopped || id == peer_id) {
        if (id == peer_id) interface.printLineError("Peer has the same user ID as this peer");
        session->close();
        return;
    }
    auto it = active_sessions.find(id);
    if (it == active_sessions.end()) {
        active_sessions.emplace(id, std::move(session));
        interface.printLineSuccess("Session identified as " + std::to_string(id));
        return;
    }

    // On simultaneous connects, the smaller ID keeps outgoing; the larger keeps incoming.
    const bool keepNew = peer_id < id
        ? session->getDirection() == ConnectionDirection::Outgoing
        : session->getDirection() == ConnectionDirection::Incoming;
    if (keepNew) {
        auto old = std::move(it->second);
        it->second = std::move(session);
        old->close();
        interface.printLine("Replaced duplicate connection to peer " + std::to_string(id));
    } else {
        session->close();
        interface.printLine("Rejected duplicate connection to peer " + std::to_string(id));
    }
}

void PeerManager::accept()
{
    if (!isRunning()) return;
    acceptor.async_accept([this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        if (stopped) return;
        if (ec) {
            if (ec != asio::error::operation_aborted) fail("Accept failed: " + ec.message());
            return;
        }
        try {
            auto session = std::make_shared<Session>(std::move(socket), peer_id, this,
                ConnectionDirection::Incoming, interface);
            session->initiateHandshake();
            session->start();
            accept();
        } catch (const std::exception& e) { fail(std::string("Could not create incoming session: ") + e.what()); }
        catch (...) { fail("Could not create incoming session"); }
    });
}

void PeerManager::connect(const std::string& address, unsigned short port)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (!isRunning()) { interface.printLineError("Peer manager is not running"); return; }
    try {
        asio::error_code ec;
        auto ip = asio::ip::make_address(address, ec);
        if (ec) { interface.printLineError("Invalid address: " + ec.message()); return; }
        auto socket = std::make_shared<asio::ip::tcp::socket>(*io);
        const asio::ip::tcp::endpoint endpoint(ip, port);
        interface.printLine("Connecting to " + address + ":" + std::to_string(port));
        socket->async_connect(endpoint, [this, socket](const asio::error_code& error) {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            if (stopped) return;
            if (error) { interface.printLineError("Connection failed: " + error.message()); return; }
            try {
                auto session = std::make_shared<Session>(std::move(*socket), peer_id, this,
                    ConnectionDirection::Outgoing, interface);
                session->initiateHandshake();
                session->start();
            } catch (const std::exception& e) {
                interface.printLineError(std::string("Could not create outgoing session: ") + e.what());
            } catch (...) { interface.printLineError("Could not create outgoing session"); }
        });
    } catch (const std::exception& e) { interface.printLineError(std::string("Connect failed: ") + e.what()); }
}

std::vector<uint8_t> PeerManager::showPeers()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    std::vector<uint8_t> peers;
    peers.reserve(active_sessions.size());
    for (const auto& [id, session] : active_sessions) peers.push_back(id);
    return peers;
}

void PeerManager::selectPeer(int user)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    if (user < 0 || user > 255) { interface.printLineError("Peer ID must be between 0 and 255"); return; }
    auto it = active_sessions.find(static_cast<uint8_t>(user));
    if (it == active_sessions.end()) { interface.printLineError("Peer is not connected"); return; }
    std::shared_ptr<Session> old_session;
    if (cur_peer >= 0) {
        auto old = active_sessions.find(static_cast<uint8_t>(cur_peer));
        if (old != active_sessions.end()) old_session = old->second;
    }
    cur_peer = user;
    auto selected = it->second;
    asio::post(*io, [old_session, selected] {
        if (old_session) old_session->deselectPeer();
        selected->selectPeer();
    });
}

void PeerManager::write(const std::string& msg)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    auto it = cur_peer < 0 ? active_sessions.end() : active_sessions.find(static_cast<uint8_t>(cur_peer));
    if (it == active_sessions.end()) {
        interface.printLineError("Select a connected peer before sending a message"); return;
    }
    auto session = it->second;
    try {
        asio::post(*io, [this, session, msg] {
            try {
                session->queueMessage({MessageType::Chat, std::vector<char>(msg.begin(), msg.end())});
                interface.printMessage(msg, peer_id);
            } catch (const std::exception& e) {
                interface.printLineError(std::string("Message send failed: ") + e.what());
            }
        });
    } catch (const std::exception& e) { interface.printLineError(std::string("Message send failed: ") + e.what()); }
}

void PeerManager::read(const std::string& msg)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    interface.printMessage(msg, cur_peer);
}

void PeerManager::onPeerDisconnected(uint8_t id, std::shared_ptr<Session> session)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    auto it = active_sessions.find(id);
    if (it == active_sessions.end() || it->second != session) return;
    active_sessions.erase(it);
    if (cur_peer == id) cur_peer = -1;
    interface.printLine("Peer " + std::to_string(id) + " disconnected");
}
