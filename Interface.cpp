#include "Interface.h"

#include <asio.hpp>
#include <stdexcept>

// will be moved another class called cryptography later
#include <random>
#include <cstdint>
uint8_t generateUserId()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());

    std::uniform_int_distribution<int> dist(0, 255);

    return static_cast<uint8_t>(dist(gen));
}
//cryptography class

Interface::Interface()   = default;

Interface::~Interface()
{
    io_context.stop();

    if (network_thread.joinable())
        network_thread.join();
}

void Interface::setConsole(ConsoleInterface* cs) {
    console_interface = cs;
}

void Interface::start(int port,uint8_t user)
{
    if (peer_manager)
        printLineError("PeerManager is already running");

    if (port < 1 || port > 65535)
        printLineError("Port must be between 1 and 65535");

    peer_manager = std::make_unique<PeerManager>(&io_context,user, port,*this);

    network_thread = std::thread([this]() {
        this->io_context.run();
        });
}

void Interface::connect(const std::string& address, int port)
{
    if (!peer_manager) {
        printLineError("Peer manager not started use : start <port> ");
        return;
    }
    peer_manager->connect(address, port);
}

void Interface::showPeers() {

    if (!isStarted()) {
        printLineError("peer manager not started use 'start <port> <user_id>'");
        return;
    }

    printLine("conncted peers :");
    for (uint8_t user : peer_manager->showPeers()) {
        printLineIndent(std::to_string(user));
    }
}

bool Interface::isStarted() const
{
    return peer_manager != nullptr;
}

void Interface::printLine(const std::string& message){
    console_interface->printLine(">> " + message);
}

void Interface::printLineError(const std::string& message) {
    console_interface->printLine("[error] : " + message);
}

void Interface::printLineSuccess(const std::string& message) {
    console_interface->printLine("[success] : " + message);
}

void Interface::printLineIndent(const std::string& message) {
    console_interface->printLine("  " + message);
}

