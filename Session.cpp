#include "Session.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include "Interface.h"

Session::Session(asio::ip::tcp::socket socket, uint8_t local_id, SessionCallbackHandler* session_callback, ConnectionDirection direction,Interface& interface ) :
		socket(std::move(socket)),
		local_id(local_id),
		session_callback(session_callback),
		direction(direction),
		interface(interface)
		
{

}

void Session::start() {
	readLength();
}

void Session::initiateHandshake() {
		
	std::vector<char> body(1);
	body[0	] = static_cast<char>(local_id);

	Message handshake_msg = {
		MessageType::Handshake,
		body
	};

	queueMessage(handshake_msg);
	
}

//reads the length of the incoming message and then calls the readBody function
void Session::readLength() {
	
	std::shared_ptr<Session> self = shared_from_this();
	
	//reads exactly the size of length header
	asio::async_read(
		socket,
		asio::buffer(message_length),
		[self](const asio::error_code ec, std::size_t bytes) {
			if (ec) {
				self->handleDisconnect();
				//close the connection
				return;
			}

			self->readBody();
		}
	);

	}

//reads the body and then creates an async loop by calling readLength function again also calls the processMessage function
void Session::readBody() {
	
	auto self = shared_from_this();


	//convert from big endian
	uint32_t length =
	  (static_cast<unsigned char>(message_length[0]) << 24)
	| (static_cast<unsigned char>(message_length[1]) << 16)
	| (static_cast<unsigned char>(message_length[2]) << 8)
	|  static_cast<unsigned char>(message_length[3]);


	if (length < 1) {
		interface.printLineError("Invalid message length");
		return;
	}

	if (length > MAX_MESSAGE_SIZE){
		interface.printLineError("Invalid message length");
		return;
	}

	message_body.resize(length);

	asio::async_read(
		socket,
		asio::buffer(message_body),
		[self,length](asio::error_code ec, std::size_t bytes) {

			if (ec) {
				self->handleDisconnect();
				return;
			}

			Message msg;
			msg.type = static_cast<MessageType>(static_cast<uint8_t>(self->message_body[0]));

			msg.body.resize(length - 1);
			memcpy(
				msg.body.data(),
				self->message_body.data() + 1,
				length - 1
			);

			self->processMessages(msg);
			self->readLength();

		}

	);
}

//process the messages and calls the 
void Session::processMessages(Message msg){

	switch (msg.type) {
	case MessageType::Handshake:
		handleHandshake(static_cast<uint8_t>(msg.body[0]));
		break;
	case MessageType::Chat:
		if (is_selected) {
			std::string message(msg.body.begin(), msg.body.end());
			session_callback->read(message);
		}
		else {
			read_queue.push_back(msg);
		}

		break;
	}

}

//writes the queue onto the socket
void Session::flushWriteQueue() {

	auto self = shared_from_this();


	asio::async_write(
		socket,
		asio::buffer(write_queue.front()),
		[self](const asio::error_code ec, std::size_t bytes) {
			if (ec) {
				self->handleDisconnect();
				return;
			}

			self->write_queue.pop_front();
			if (!self->write_queue.empty()) {
				self->flushWriteQueue();
			}
		}
	);
}

//adds the msg to the queue and calls flushWriteQueue
void Session::queueMessage(Message msg){
	//if queue not empty that means we're already writing
	bool writing = !write_queue.empty();

	write_queue.push_back(
		serialize(msg)
	);

	if (!writing) {
		flushWriteQueue();
	}

}

//handles the handshake and calls the SessionCallbackHandler to store the current session in the map
void Session::handleHandshake(uint8_t remote_id)
{
    this->remote_id = remote_id;
    handshake_complete = true;

    auto cur_session = shared_from_this();

    session_callback->onPeerIdentified(
        remote_id,
        cur_session
    );
}

void Session::selectPeer() {
	is_selected = true;
	for (const auto& msg : read_queue) {
		if (msg.type == MessageType::Chat) {
			std::string message(msg.body.begin(), msg.body.end());
			session_callback->read(message);
		}
	}
	read_queue.clear();
}

void Session::deselectPeer() {
	is_selected = false;
}


ConnectionDirection Session::getDirection() const
{
    return direction;
}

void Session::handleDisconnect()
{
    if (closed)
        return;

    closed = true;

    asio::error_code ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket.close(ec);

    if (remote_id != 0) {
        session_callback->onPeerDisconnected(
            remote_id,
            shared_from_this()
        );
    }
}

void Session::close()
{
    if (closed)
        return;

    closed = true;

    asio::error_code ec;
    socket.shutdown(
        asio::ip::tcp::socket::shutdown_both,
        ec
    );

    socket.close(ec);
}


