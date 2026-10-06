#include "PeerManager.h"
#include <iostream>
#include "Message.h"

PeerManager::PeerManager(asio::io_context* io,uint8_t peer_id,int port,Interface& interface)
    : acceptor(*io),
      server(*io),
      peer_id(peer_id),
      io(io),
      interface(interface)
{
    asio::error_code ec;

    auto endpoint = asio::ip::tcp::endpoint(
        asio::ip::make_address("127.0.0.1"),
        port
    );

    acceptor.open(endpoint.protocol(), ec);

    if (ec)
    {
        interface.printLineError(
            "Failed to open acceptor: " + ec.message()
        );
        return;
    }

    acceptor.bind(endpoint, ec);

    if (ec)
    {
        interface.printLineError(
            "Failed to bind port " +
            std::to_string(port) +
            ": " +
            ec.message()
        );
        return;
    }

    acceptor.listen(
        asio::socket_base::max_listen_connections,
        ec
    );

    if (ec)
    {
        interface.printLine(
            "Failed to listen: " +
            ec.message()
        );
        return;
    }

    interface.printLineSuccess("Peer manager started on port = " +
		std::to_string(port)+ " user id = " + std::to_string(peer_id));

    accept();
}


void PeerManager::onPeerIdentified( uint8_t id, std::shared_ptr<Session> session)
{
    auto it = active_sessions.find(id);

    // First connection from this peer
    if (it == active_sessions.end()) {
        active_sessions[id] = session;

        interface.printLineSuccess(
            "session identified as " +
            std::to_string(id)
        );

        return;
    }

    auto existing = it->second;

    // Determine which connection should survive.
    bool keepNew;

    if (peer_id < id) {
        // Smaller ID keeps outgoing connection.
        keepNew =
            session->getDirection() ==
            ConnectionDirection::Outgoing;
    }
    else if (peer_id > id) {
        // Larger ID keeps incoming connection.
        keepNew =
            session->getDirection() ==
            ConnectionDirection::Incoming;
    }
    else {
        // Same ID as ourselves — invalid.
        interface.printLineError(
            "Peer has the same user ID as this peer"
        );

        session->close();
        return;
    }

    if (keepNew) {
        interface.printLine(
            "Replacing duplicate connection to peer " +
            std::to_string(id)
        );

        active_sessions[id] = session;

        existing->close();
    }
    else {
        interface.printLine(
            "Rejecting duplicate connection to peer " +
            std::to_string(id)
        );

        session->close();
    }
}

void PeerManager::accept() {
	//continously accpets new connections and creates a session object for each of them
	acceptor.async_accept(
		[&, this](asio::error_code ec, asio::ip::tcp::socket socket)
		{
			if (!ec)
			{
				interface.printLineSuccess("succesfully accepted");
				auto session = std::make_shared<Session>(
					std::move(socket),
					peer_id,
					this,
					ConnectionDirection::Incoming,
					interface
				);
				session->initiateHandshake();
				session->start();
			}
			//calls the function again to create async loop
			accept();
		});
		
}

void PeerManager::connect(const std::string& address, unsigned short port) {

	interface.printLine("Connecting to " + address + "::" + std::to_string(port));

	auto socket = std::make_shared<asio::ip::tcp::socket>(*io);

	asio::ip::tcp::endpoint endpoint(
		asio::ip::make_address(address),
		port
	);

	socket->async_connect(
		endpoint,
		[this, socket](const asio::error_code ec) {
			if (ec) {
				interface.printLineError("connection failed");
				return;
			}

			interface.printLineSuccess("connected");

			auto session = std::make_shared<Session>(
				std::move(*socket),
				peer_id,
				this,
				ConnectionDirection::Outgoing,
				interface
			);

			session->initiateHandshake();
			session->start();

		}
	);
}

std::vector<uint8_t> PeerManager::showPeers(){
	std::vector<uint8_t> peers;
	peers.reserve(active_sessions.size());

	for (const auto& [key, value] : active_sessions) {
		peers.push_back(key);
	}

	return peers;

}

void PeerManager::selectPeer(int user) {
	if (cur_peer != (uint8_t)0) {
		active_sessions[cur_peer]->deselectPeer();
	}
	cur_peer = (uint8_t)user;
	active_sessions[cur_peer]->selectPeer();
}


void PeerManager::write(const std::string& msg) {
	active_sessions[cur_peer]->queueMessage(Message(
		MessageType::Chat,
		std::vector<char>(msg.begin(), msg.end())
	));
	interface.printMessage(msg, peer_id);
}

void PeerManager::read(const std::string& msg) {
	interface.printMessage(msg, cur_peer);
}

void PeerManager::onPeerDisconnected(uint8_t id, std::shared_ptr<Session> session) {
	active_sessions.erase(id);
	if (cur_peer == id) {
		cur_peer = 0;
	}
}
