#pragma once

#include <memory>
#include <string>
#include <asio.hpp>
#include <thread>
#include <cstdint>
#include "PeerManagerCallbackHandler.h"

class PeerManager;
class ConsoleInterface;

class Interface : public PeerManagerCallbackHandler
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

    void onPeerManagerStarted(uint16_t port, uint8_t local_id) override;
    void onPeerManagerError(const asio::error_code& error) override;
    void onPeerOperationError(const asio::error_code& error) override;
    void onPeerConnection(bool incoming, const asio::error_code& error) override;
    void onPeerIdentified(uint8_t peer_id) override;
    void onDuplicatePeerConnection(uint8_t peer_id, bool new_connection_kept) override;
    void onPeerDisconnected(uint8_t peer_id) override;
    void onPeerSessionError(bool identified, uint8_t peer_id,
        const asio::error_code& error) override;
    void onPeerMessage(uint8_t peer_id, const std::string& message) override;
    void onLocalMessageSent(uint8_t local_id, const std::string& message) override;

};
