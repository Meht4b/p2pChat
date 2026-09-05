#include <iostream>
#include <asio.hpp>

#include "PeerManager.h"

int main() {

	asio::io_context io;

	int peer_id_in, port;
	std::cin >> peer_id_in >> port;
	uint8_t peer_id = static_cast<uint8_t>(peer_id_in);

	PeerManager peer_manager(&io, peer_id,port);

	if (peer_id == 1) {
		peer_manager.publicConnect();
	}
	io.run();

}
