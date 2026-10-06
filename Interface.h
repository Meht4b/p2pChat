#pragma once

#include <memory>
#include <string>
#include <asio.hpp>
#include <thread>
#include <cstdint>

class PeerManager;
class ConsoleInterface;

class Interface
{
private:
    asio::io_context io_context;
    std::unique_ptr<PeerManager> peer_manager;
    ConsoleInterface* console_interface = nullptr;
    std::thread network_thread;

public:
    Interface();
    ~Interface();

    void shutdown() noexcept;

    void setConsole(ConsoleInterface* console_interface);
    void start(int port, uint8_t user);
    void connect(const std::string& address, int port);
    bool isStarted() const;
    void printLine(const std::string& message);
    void printLineSuccess(const std::string& message);
    void printLineError(const std::string& message);
    void printLineIndent(const std::string& message);
	void printMessage(const std::string& message, int user_id);
    void showPeers();
    bool peerExists(int user);
    void selectPeer(int user);
    void write(const std::string& msg);

};
