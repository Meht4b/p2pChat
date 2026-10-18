#include "Interface.h"
#include "ConsoleInterface.h"
#include "PeerManager.h"
#include "StorageManager.h"
#include "TextSanitizer.h"
#include "CryptoIdentity.h"

#include <algorithm>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <system_error>

namespace
{
    constexpr const char* kIdentityKey = "identity";
    constexpr const char* kUsernameKey = "username";
    constexpr const char* kPortKey     = "port";

    std::string formatTime(std::int64_t unix_ms)
    {
        const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &seconds);
#else
        localtime_r(&seconds, &local);
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
        return buffer;
    }

    // "alice (1a2b3c4d)": the saved username if we know the peer, otherwise just the short id
    std::string describePeer(StorageManager& storage, const PeerId& id)
    {
        try {
            if (const auto known = storage.getKnownPeer(id))
                return known->username + " (" + id.shortHex() + ")";
        } catch (const std::exception&) {}
        return id.shortHex();
    }
}

Interface::Interface() = default;

Interface::~Interface()
{
    shutdown();
    peer_manager.reset();
}

void Interface::shutdown() noexcept
{
    io_context.stop();
    if (network_thread.joinable()) network_thread.join();
    if (peer_manager) peer_manager->stop();
}

void Interface::setConsole(ConsoleInterface* cs) { console_interface = cs; }

StorageManager& Interface::storage()
{
    if (!storage_manager) throw std::logic_error("Interface::initialize() has not been called");
    return *storage_manager;
}

// ---- identity and profile ---------------------------------------------------

void Interface::initialize(const std::string& storage_path)
{
    storage_manager = std::make_unique<StorageManager>(storage_path, 100);

    // The persistent signing key is stored portably; its public-key fingerprint
    // is the stable peer ID shared in the app handshake.
    crypto_identity = CryptoIdentity::loadOrCreate(*storage_manager);
    local_id = crypto_identity.peerId();

    // first-run settings: ignored (and asked for again) if missing or invalid
    const auto saved_name = storage_manager->getSetting(kUsernameKey);
    const auto saved_port = storage_manager->getSetting(kPortKey);
    if (saved_name && saved_port && isValidUsername(*saved_name)) {
        try {
            std::size_t used = 0;
            const int port = std::stoi(*saved_port, &used);
            if (used == saved_port->size() && port >= 1 && port <= 65535) {
                username = *saved_name;
                listen_port = static_cast<uint16_t>(port);
            }
        } catch (const std::exception&) {}
    }
}

bool Interface::needsSetup() const { return username.empty() || !listen_port; }

void Interface::saveProfile(const std::string& new_username, int port)
{
    if (!isValidUsername(new_username))
        throw std::invalid_argument("Username must be 1-32 characters: letters, digits, '.', '_' or '-'");
    if (port < 1 || port > 65535)
        throw std::invalid_argument("Port must be between 1 and 65535");

    storage().setSetting(kUsernameKey, new_username);
    storage().setSetting(kPortKey, std::to_string(port));
    username = new_username;
    listen_port = static_cast<uint16_t>(port);
}

PeerInfo Interface::localPeer() const { return { local_id, username }; }

std::optional<uint16_t> Interface::savedPort() const { return listen_port; }

// ---- starting ---------------------------------------------------------------

void Interface::start()
{
    if (needsSetup()) {
        printLineError("Finish the first-run setup before starting");
        return;
    }
    startPeerManager(*listen_port);
}

void Interface::start(int port)
{
    if (needsSetup()) {
        printLineError("Finish the first-run setup before starting");
        return;
    }
    startPeerManager(port);
}

void Interface::startPeerManager(int port)
{
	if (peer_manager && peer_manager->isRunning()) {
        printLineError("PeerManager is already running");
        return;
    }
    if (port < 1 || port > 65535) { printLineError("Port must be between 1 and 65535"); return; }

    // A stopped manager can remain after a fatal accept/network error. Reap its
    // worker and manager before restarting the io_context and accepting /start.
    if (network_thread.joinable()) {
        io_context.stop();
        network_thread.join();
    }
    peer_manager.reset();
    io_context.restart();

    try {
        auto manager = std::make_unique<PeerManager>(&io_context, localPeer(), crypto_identity, port, *this);
        peer_manager = std::move(manager);
        network_thread = std::thread([this] {
            try { io_context.run(); }
            catch (const std::exception& e) {
                if (peer_manager) peer_manager->stop();
                printLineError(std::string("Network loop stopped: ") + e.what());
            } catch (...) {
                if (peer_manager) peer_manager->stop();
                printLineError("Network loop stopped by an unknown error");
            }
        });
    } catch (const asio::system_error& e) {
        peer_manager.reset();
        onPeerManagerError(e.code());
    } catch (const std::exception&) {
        peer_manager.reset();
        onPeerManagerError(asio::error::make_error_code(asio::error::fault));
    } catch (...) {
        peer_manager.reset();
        onPeerManagerError(asio::error::make_error_code(asio::error::fault));
    }
}

// ---- user actions -----------------------------------------------------------

void Interface::connect(const std::string& address, int port)
{
    if (!peer_manager || !peer_manager->isRunning()) { printLineError("Peer manager is not running; use /start"); return; }
    if (port < 1 || port > 65535) { printLineError("Port must be between 1 and 65535"); return; }
    peer_manager->connect(address, static_cast<unsigned short>(port));
}

void Interface::connectRoom(const std::string& room_token, const std::string& relay_server)
{
    if (!peer_manager || !peer_manager->isRunning()) { printLineError("Peer manager is not running; use /start"); return; }
    peer_manager->connectRoom(room_token, relay_server);
}

void Interface::showPeers()
{
    if (!peer_manager || !peer_manager->isRunning()) { printLineError("Peer manager is not running"); return; }
    printLine("Connected peers:");
    for (const PeerId& peer : peer_manager->showPeers())
        printLineIndent(describePeer(storage(), peer));
}

void Interface::showKnownPeers()
{
    const auto known = storage().getKnownPeers();
    if (known.empty()) {
        printLine("No known peers yet");
        return;
    }

    std::vector<PeerId> online;
    if (peer_manager && peer_manager->isRunning())
        online = peer_manager->showPeers();

    printLine("Known peers:");
    for (const auto& peer : known) {
        const bool is_online = std::find(online.begin(), online.end(), peer.id) != online.end();
        printLineIndent(peer.username + " (" + peer.id.shortHex() + ")  " +
            (is_online ? "online" : "last seen " + formatTime(peer.last_seen)));
    }
}

bool Interface::peerExists(const PeerId& peer)
{
    if (!peer_manager || !peer_manager->isRunning()) return false;
    const auto peers = peer_manager->showPeers();
    return std::find(peers.begin(), peers.end(), peer) != peers.end();
}

void Interface::selectPeer(const PeerId& peer)
{
    if (!peer_manager || !peer_manager->isRunning()) { printLineError("Peer manager is not running"); return; }
    peer_manager->selectPeer(peer);

    try {
        for (const auto& msg : storage().getMessages(peer)) {
            if (msg.outgoing) printSentMessage(msg.content);
            else printMessage(msg.content, msg.peer_id);
        }
    } catch (const std::exception& e) {
        printLineError(std::string("Could not load chat history: ") + e.what());
    }
}

void Interface::deselectPeer()
{
    if (!peer_manager || !peer_manager->isRunning()) { printLineError("Peer manager is not running"); return; }
    peer_manager->deselectPeer();
}

bool Interface::isStarted() const { return peer_manager && peer_manager->isRunning(); }

void Interface::write(const std::string& msg)
{
    if (!peer_manager || !peer_manager->isRunning()) { printLineError("Peer manager is not running"); return; }
    peer_manager->write(msg);       // history is saved in onLocalMessageSent, once it is really queued
}

// ---- PeerManager events -----------------------------------------------------

void Interface::onPeerManagerStarted(uint16_t port, const PeerInfo& local)
{
    printLineSuccess("Peer manager started on port " + std::to_string(port) +
        " as " + local.username + " (" + local.id.shortHex() + ")");
    if (console_interface) console_interface->setOnline(port, local);
}

void Interface::onPeerManagerError(const asio::error_code& error)
{
    printLineError("Peer manager stopped: " + error.message());
    if (console_interface) console_interface->setOffline();
}

void Interface::onPeerOperationError(const asio::error_code& error)
{
    printLineError("Peer operation failed: " + error.message());
}

void Interface::onPeerRoomError(const std::string& message)
{
    printLineError("Room connection failed: " + message);
}

void Interface::onPeerConnection(bool incoming, const asio::error_code& error)
{
    if (error) {
        printLineError(std::string(incoming ? "Incoming connection failed: " : "Connection failed: ") +
            error.message());
    } else {
        printLineSuccess(incoming ? "Incoming connection accepted" : "Connection established");
    }
}

// A peer we know keeps the username we saved the first time; a new peer is remembered.
void Interface::onPeerIdentified(const PeerInfo& peer)
{
    PeerInfo shown = peer;
    try {
        if (const auto known = storage().getKnownPeer(peer.id)) {
            shown.username = known->username;
            if (known->username != peer.username)
                printLine(known->username + " now announces itself as '" + peer.username +
                    "'; keeping the saved name");
        }
        storage().rememberPeer(peer.id, shown.username);    // new peer: saved, known peer: last seen updated
    } catch (const std::exception& e) {
        printLineError(std::string("Could not save peer: ") + e.what());
    }

    printLineSuccess("Connected to " + shown.username + " (" + shown.id.shortHex() + ")");
    if (console_interface) console_interface->onPeerConnected(shown);
}

void Interface::onDuplicatePeerConnection(const PeerId& peer_id, bool new_connection_kept)
{
    printLine(std::string(new_connection_kept ? "Replaced" : "Rejected") +
        " duplicate connection to " + describePeer(storage(), peer_id));
}

void Interface::onPeerDisconnected(const PeerId& peer_id)
{
    printLine(describePeer(storage(), peer_id) + " disconnected");
    if (console_interface) console_interface->onPeerDisconnected(peer_id);
}

void Interface::onPeerSessionError(bool identified, const PeerId& peer_id,
    const asio::error_code& error)
{
    const std::string peer = identified
        ? " with " + describePeer(storage(), peer_id)
        : " before peer identification";
    printLineError("Session terminated" + peer + ": " + error.message());
}

void Interface::onPeerMessage(const PeerId& peer_id, const std::string& message)
{
    printMessage(message, peer_id);
    try {
        storage().saveMessage(peer_id, false, message);
    } catch (const std::exception& e) {
        printLineError(std::string("Could not save message: ") + e.what());
    }
}

void Interface::onLocalMessageSent(const PeerId& peer_id, const std::string& message)
{
    printSentMessage(message);
    try {
        storage().saveMessage(peer_id, true, message);
    } catch (const std::exception& e) {
        printLineError(std::string("Could not save message: ") + e.what());
    }
}

// ---- console output ---------------------------------------------------------

void Interface::printLine(const std::string& msg) { if (console_interface) console_interface->printLine(msg); }
void Interface::printLineError(const std::string& msg) { if (console_interface) console_interface->printLineError(msg); }
void Interface::printLineSuccess(const std::string& msg) { if (console_interface) console_interface->printLineSuccess(msg); }
void Interface::printLineIndent(const std::string& msg) { if (console_interface) console_interface->printLineIndent(msg); }
void Interface::printMessage(const std::string& msg, const PeerId& id) { if (console_interface) console_interface->printMessage(msg, id); }
void Interface::printSentMessage(const std::string& msg) { if (console_interface) console_interface->printSentMessage(msg); }
