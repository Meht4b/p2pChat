#include "Interface.h"
#include "ConsoleInterface.h"
#include "PeerManager.h"

#include <algorithm>
#include <exception>
#include <random>
#include <system_error>

uint8_t generateUserId()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    return static_cast<uint8_t>(std::uniform_int_distribution<int>(0, 255)(gen));
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

void Interface::start(int port, uint8_t user)
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
        auto manager = std::make_unique<PeerManager>(&io_context, user, port, *this);
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

void Interface::connect(const std::string& address, int port)
{
    if (!peer_manager || !peer_manager->isRunning()) { printLineError("Peer manager is not running; use /start <port> <user_id>"); return; }
    if (port < 1 || port > 65535) { printLineError("Port must be between 1 and 65535"); return; }
    peer_manager->connect(address, static_cast<unsigned short>(port));
}

void Interface::showPeers()
{
    if (!peer_manager || !peer_manager->isRunning()) { printLineError("Peer manager is not running"); return; }
    printLine("Connected peers:");
    for (uint8_t user : peer_manager->showPeers()) printLineIndent(std::to_string(user));
}

bool Interface::peerExists(int user)
{
    if (!peer_manager || !peer_manager->isRunning() || user < 0 || user > 255) return false;
    const auto peers = peer_manager->showPeers();
    return std::find(peers.begin(), peers.end(), static_cast<uint8_t>(user)) != peers.end();
}

void Interface::selectPeer(int user)
{
    if (!peer_manager || !peer_manager->isRunning()) { printLineError("Peer manager is not running"); return; }
    peer_manager->selectPeer(user);
}

bool Interface::isStarted() const { return peer_manager && peer_manager->isRunning(); }

void Interface::write(const std::string& msg)
{
    if (!peer_manager || !peer_manager->isRunning()) { printLineError("Peer manager is not running"); return; }
    peer_manager->write(msg);
}

void Interface::onPeerManagerStarted(uint16_t port, uint8_t local_id)
{
    printLineSuccess("Peer manager started on port " + std::to_string(port) +
        " with user ID " + std::to_string(local_id));
}

void Interface::onPeerManagerError(const asio::error_code& error)
{
    printLineError("Peer manager stopped: " + error.message());
}

void Interface::onPeerOperationError(const asio::error_code& error)
{
    printLineError("Peer operation failed: " + error.message());
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

void Interface::onPeerIdentified(uint8_t peer_id)
{
    printLineSuccess("Connected to peer " + std::to_string(peer_id));
}

void Interface::onDuplicatePeerConnection(uint8_t peer_id, bool new_connection_kept)
{
    printLine(std::string(new_connection_kept ? "Replaced" : "Rejected") +
        " duplicate connection to peer " + std::to_string(peer_id));
}

void Interface::onPeerDisconnected(uint8_t peer_id)
{
    printLine("Peer " + std::to_string(peer_id) + " disconnected");
}

void Interface::onPeerSessionError(bool identified, uint8_t peer_id,
    const asio::error_code& error)
{
    const std::string peer = identified ? " with peer " + std::to_string(peer_id) : " before peer identification";
    printLineError("Session terminated" + peer + ": " + error.message());
}

void Interface::onPeerMessage(uint8_t peer_id, const std::string& message)
{
    printMessage(message, peer_id);
}

void Interface::onLocalMessageSent(uint8_t local_id, const std::string& message)
{
    printMessage(message, local_id);
}

void Interface::printLine(const std::string& msg) { if (console_interface) console_interface->printLine(msg); }
void Interface::printLineError(const std::string& msg) { if (console_interface) console_interface->printLineError(msg); }
void Interface::printLineSuccess(const std::string& msg) { if (console_interface) console_interface->printLineSuccess(msg); }
void Interface::printLineIndent(const std::string& msg) { if (console_interface) console_interface->printLineIndent(msg); }
void Interface::printMessage(const std::string& msg, int id) { if (console_interface) console_interface->printMessage(msg, id); }
