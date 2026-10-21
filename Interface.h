#pragma once

#include <memory>
#include <optional>
#include <string>
#include <asio.hpp>
#include <thread>
#include <cstdint>
#include "PeerManagerCallbackHandler.h"
#include "PeerId.h"
#include "CryptoIdentity.h"
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

    // who we are; loaded by initialize()
    PeerId local_id;
    CryptoIdentity crypto_identity;
    std::string username;                   // empty until the first-run setup is done
    std::optional<uint16_t> listen_port;    // empty until the first-run setup is done

    StorageManager& storage();

public:
    Interface();
    ~Interface();
    
    //safely stops all the services
    void shutdown() noexcept;

	// Sets the console interface for output and user interaction.
    void setConsole(ConsoleInterface* console_interface);

    //opens the local database, loads the identity (creating it on the very first run)
    //and the saved username / port. Throws on failure; must be called before anything else.
    void initialize(const std::string& storage_path = "storage.db");

    //false until a username and port have been saved (first run)
    bool needsSetup() const;

    //validates and saves the first-run settings. Throws std::invalid_argument on bad input.
    void saveProfile(const std::string& new_username, int port);

    //our own id and username
    PeerInfo localPeer() const;
    std::optional<uint16_t> savedPort() const;

	// Starts the peer manager on the saved port, or on `port` if given.
    void start();
    void start(int port);

    void startPeerManager(int port);

	//returns if peermanager has started and is running
    bool isStarted() const;

	//Thread - safe logging methods.These can be called from any thread.
    void printLine(const std::string& message);
    void printLineSuccess(const std::string& message);
    void printLineError(const std::string& message);
    void printLineIndent(const std::string& message);
	void printMessage(const std::string& message, const PeerId& peer_id);
    void printSentMessage(const std::string& message);

	//-------------------------------- PeerManager operations ------------------------
     
	// Connects to a peer at the specified address and port(delegates to the peer manager).
    void connect(const std::string& address, int port);
    void connectRoom(const std::string& room_token, const std::string& relay_server);

	// Shows the list of connected peers.
    void showPeers();

    // Shows every peer we have talked to before.
    void showKnownPeers();

    //checks if peer is connected
    bool peerExists(const PeerId& peer);

    //selects peer
    void selectPeer(const PeerId& peer);

    //deselects peer
    void deselectPeer();

    //writes the message to the socket
    void write(const std::string& msg);

	// ----------------- PeerManagerCallbackHandler overrides -----------------

    //called once the listening socket is up
    void onPeerManagerStarted(uint16_t port, const PeerInfo& local) override;

    //handles errors
    void onPeerManagerError(const asio::error_code& error) override;
    void onPeerOperationError(const asio::error_code& error) override;
    void onPeerRoomError(const std::string& message) override;
    void onPeerSessionError(bool identified, const PeerId& peer_id, const asio::error_code& error) override;

	//when a connection is established , either incoming or outgoing, this callback is invoked
    void onPeerConnection(bool incoming, const asio::error_code& error) override;

	//when handshake is successful and peer is identified, this callback is invoked
    void onPeerIdentified(const PeerInfo& peer) override;
    void onPeerIdentified(const PeerInfo& peer, ConnectionPath path) override;
    void onPeerConnectionPathChanged(const PeerId& peer_id, ConnectionPath path) override;

    //when duplicate connection occurs
    void onDuplicatePeerConnection(const PeerId& peer_id, bool new_connection_kept) override;

    //when peer disconnects
    void onPeerDisconnected(const PeerId& peer_id) override;

	//when a message is received from a peer
    void onPeerMessage(const PeerId& peer_id, const std::string& message) override;

	//when a message is sent to a peer
    void onLocalMessageSent(const PeerId& peer_id, const std::string& message) override;

};
