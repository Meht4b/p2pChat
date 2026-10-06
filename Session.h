#pragma once

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>
#include <array>

#include "Message.h"
#include "SessionCallbackHandler.h"

class Interface;

//whether the session had initiated the connection or accepted the connection
enum class ConnectionDirection : bool
{
    Incoming,
    Outgoing
};

//manages each session
//sends and recieves all the messages
//does not display anything
class Session : public std::enable_shared_from_this<Session>
{
private:
	//the socket
	asio::ip::tcp::socket socket;

	//read and write queues
	std::deque<std::vector<char>> write_queue;
	std::deque<Message> read_queue;
	
	//session ids
	uint8_t local_id,remote_id;
	
	//callback handler
	SessionCallbackHandler* session_callback;

	//read buffers
	std::array<char, 4> message_length;
	std::vector<char> message_body;

	//stores whether it was outgoing or incoming
	ConnectionDirection direction;

	Interface& interface;

	//stores whether the current peer is selected or not
	bool is_selected = false;

public:
	Session(asio::ip::tcp::socket socket, uint8_t local_id, SessionCallbackHandler* session_callback, ConnectionDirection direction, Interface& interface);

	void start();

	void initiateHandshake();

	//adds the msg to the queue and calls flushWriteQueue
	void queueMessage(Message msg);

	void selectPeer();

	void deselectPeer();

private:

	//reads the length of the incoming message and then calls the readBody function
	void readLength();

	//reads the body and then creates an async loop by calling readLength function again also calls the processMessage function
	void readBody();

	//process the messages and calls the 
	void processMessages(Message msg);

	//writes the queue onto the socket
	void flushWriteQueue();

	//handles the handshake and calls the SessionCallbackHandler to store the current session in the map
	void handleHandshake(uint8_t remote_id);

};