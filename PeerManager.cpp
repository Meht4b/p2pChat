#include "PeerManager.h"
#include <iostream>

PeerManager::PeerManager(asio::io_context* io, uint8_t peer_id, int port)
		: acceptor(
			*io,
			asio::ip::tcp::endpoint(
				asio::ip::make_address("127.0.0.1"),
				port
			)
		),
		server(*io),
		peer_id(peer_id),
		io(io)
			
	{
		accept();
	}

void PeerManager::onPeerIdentified(uint8_t id, std::shared_ptr<Session> session) {
		if (active_sessions.contains(id)) {
			std::cout << "session already exists" << std::endl;
			//delete the session
		}
		std::cout << "succecsfully identified " << id << std::endl;
		active_sessions[id] = session;
	
	}

void PeerManager::accept() {
	//continously accpets new connections and creates a session object for each of them
	acceptor.async_accept(
		[&, this](asio::error_code ec, asio::ip::tcp::socket socket)
		{
			if (!ec)
			{
				std::cout << "succesfully accepted" << std::endl;
				auto session = std::make_shared<Session>(
					std::move(socket),
					peer_id,
					this,
					ConnectionDirection::Incoming
				);
				session->initiateHandshake();
				session->start();
			}
			//calls the function again to create async loop
			accept();
		});
		
}

void PeerManager::publicConnect() {
		connect("127.0.0.1", 8080);
	}

void PeerManager::connect(const std::string& address, unsigned short port) {
		
	auto socket = std::make_shared<asio::ip::tcp::socket>(*io);

	asio::ip::tcp::endpoint endpoint(
		asio::ip::make_address(address),
		port
	);

	socket->async_connect(
		endpoint,
		[this, socket](const asio::error_code ec) {
			if (ec) {
				std::cout << "connection failed" << std::endl;
				std::cout << ec.message() << std::endl;
				return;
			}

			std::cout << "connected" << std::endl;

			auto session = std::make_shared<Session>(
				std::move(*socket),
				peer_id,
				this,
				ConnectionDirection::Outgoing
			);

			session->initiateHandshake();
			session->start();

		}
	);
}