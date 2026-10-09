#pragma once

#include <memory>
#include <string>
#include <asio.hpp>
#include <thread>
#include <cstdint>
#include "PeerManagerCallbackHandler.h"
#include "StorageManager.h"

class PeerManager;
class ConsoleInterface;

class Interface : public PeerManagerCallbackHandler
{
private:
    asio::io_context io_context;
    std::unique_ptr<PeerManager> peer_manager;
    ConsoleInterface* console_interface = nullptr;
    std::thread network_thread;
	std::unique_ptr<StorageManager> storage_manager;

public:
    Interface();
    ~Interface();
    
    //safely stops all the services
    void shutdown() noexcept;

	// Sets the console interface for output and user interaction.
    void setConsole(ConsoleInterface* console_interface);

	// Starts the peer manager on the specified port with the given user ID and the storagemanager.
    void start(int port, uint8_t user);

    void startPeerManager(int port, uint8_t user);

    void startStorageManager(const std::string& storage_path, std::size_t max_queue);

	//returns if peermanager has started and is running
    bool isStarted() const;

	//Thread - safe logging methods.These can be called from any thread.
    void printLine(const std::string& message);
    void printLineSuccess(const std::string& message);
    void printLineError(const std::string& message);
    void printLineIndent(const std::string& message);
	void printMessage(const std::string& message, int user_id);
    void printSentMessage(const std::string& message);

	//-------------------------------- PeerManager operations ------------------------
     
	// Connects to a peer at the specified address and port(delegates to the peer manager).
    void connect(const std::string& address, int port);

	// Shows the list of connected peers.
    void showPeers();

    //checks if peer exists
    bool peerExists(int user);

    //selects peer
    void selectPeer(int user);

    //deselects peer
    void deselectPeer();

    //writes the message to the socket
    void write(const std::string& msg);

	// ----------------- PeerManagerCallbackHandler overrides -----------------

    //returns the user id and port
    void onPeerManagerStarted(uint16_t port, uint8_t local_id) override;

    //handles errors
    void onPeerManagerError(const asio::error_code& error) override;
    void onPeerOperationError(const asio::error_code& error) override;
    void onPeerSessionError(bool identified, uint8_t peer_id,const asio::error_code& error) override;

	//when a connection is established , either incoming or outgoing, this callback is invoked
    void onPeerConnection(bool incoming, const asio::error_code& error) override;

	//when handshake is successful and peer is identified, this callback is invoked
    void onPeerIdentified(uint8_t peer_id) override;

    //when duplicate connection occurs
    void onDuplicatePeerConnection(uint8_t peer_id, bool new_connection_kept) override;

    //when peer disconnects
    void onPeerDisconnected(uint8_t peer_id) override;

	//when a message is received from a peer
    void onPeerMessage(uint8_t peer_id, const std::string& message) override;

	//when a message is sent to a peer
    void onLocalMessageSent(uint8_t local_id, const std::string& message) override;

};
