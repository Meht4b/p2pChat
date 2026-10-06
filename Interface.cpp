#include "Interface.h"
#include "ConsoleInterface.h"
#include "PeerManager.h"

#include <algorithm>
#include <exception>
#include <random>

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
    if (peer_manager) { printLineError("PeerManager is already running"); return; }
    if (port < 1 || port > 65535) { printLineError("Port must be between 1 and 65535"); return; }
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
    } catch (const std::exception& e) {
        peer_manager.reset();
        printLineError(std::string("Could not start peer manager: ") + e.what());
    } catch (...) {
        peer_manager.reset();
        printLineError("Could not start peer manager: unknown error");
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

void Interface::printLine(const std::string& msg) { if (console_interface) console_interface->printLine(msg); }
void Interface::printLineError(const std::string& msg) { if (console_interface) console_interface->printLineError(msg); }
void Interface::printLineSuccess(const std::string& msg) { if (console_interface) console_interface->printLineSuccess(msg); }
void Interface::printLineIndent(const std::string& msg) { if (console_interface) console_interface->printLineIndent(msg); }
void Interface::printMessage(const std::string& msg, int id) { if (console_interface) console_interface->printMessage(msg, id); }
