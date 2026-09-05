#include <iostream>
#include <memory>
#include <asio.hpp>
#include <deque>
#include <optional>
#include <unordered_map>


constexpr std::size_t MAX_MESSAGE_SIZE = 64 * 1024;
class Session;

//enum class for the type of payload
enum class MessageType : uint8_t
{
	Handshake,
	Chat
};

//whether the session had initiated the connection or accepted the connection
enum class ConnectionDirection : bool
{
    Incoming,
    Outgoing
};

//structure for message 
typedef struct Message{
	MessageType type;
	std::vector<char> body;
};

//serializes the messages
std::vector<char> serialize(const Message& msg) {
	uint32_t length = 1 + msg.body.size();

	std::vector<char> data(4 + length);
	
	//converts to big endian
	data[0] = (length >> 24) & 0xFF;
	data[1] = (length >> 16) & 0xFF;
	data[2] = (length >> 8) & 0xFF;
	data[3] = length  & 0xFF;

	//stores the message type
	data[4] = static_cast<char>(msg.type);

	//actual body
	std::memcpy(
		data.data() + 5,
		msg.body.data(),
		length - 1
	);

	return data;

}

//deserialize the msg
Message deserialize(const std::vector<char>& data) {

	if (data.size() < 5) {
		throw std::runtime_error("Invalid message");
	}

	Message msg;

	//convert from big endian
	uint32_t length =
      (static_cast<unsigned char>(data[0]) << 24)
    | (static_cast<unsigned char>(data[1]) << 16)
    | (static_cast<unsigned char>(data[2]) << 8)
    |  static_cast<unsigned char>(data[3]);


	if (data.size() != 4 + length) {
		throw std::runtime_error("Invalid message length");
	}

	if (length < 1) {
		throw std::runtime_error("Invalid message length");
	}

	if (length > MAX_MESSAGE_SIZE){
		throw std::runtime_error("Invalid message length");
	}

	msg.type = static_cast<MessageType>(static_cast<uint8_t>(data[4]));

	msg.body.resize(length - 1);
	memcpy(
		msg.body.data(),
		data.data() + 5,
		length - 1
	);

	return msg;
	
}

//saves the actual data on to the device
//the actual interaction between the messages and the end user will happen through this
class StorageManager
{
public:
	StorageManager() {};

private:

};

//interface for managing the callback when session recieves the remote id
class SessionCallbackHandler
{
public:
	virtual ~SessionCallbackHandler() = default;

	virtual void onPeerIdentified(uint8_t id, std::shared_ptr<Session> session) = 0;

private:

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

public:
	Session(asio::ip::tcp::socket socket, uint8_t local_id, SessionCallbackHandler* session_callback, ConnectionDirection direction) :
		socket(std::move(socket)),
		local_id(local_id),
		session_callback(session_callback),
		direction(direction)
	{

	}

	void start() {
		readLength();
	}

	void initiateHandshake() {
		
		std::vector<char> body(1);
		body[0	] = static_cast<char>(local_id);

		Message handshake_msg = {
			MessageType::Handshake,
			body
		};

		queueMessage(handshake_msg);
		
	}
private:

	//reads the length of the incoming message and then calls the readBody function
	void readLength() {
		
		std::shared_ptr<Session> self = shared_from_this();
		
		//reads exactly the size of length header
		asio::async_read(
			socket,
			asio::buffer(message_length),
			[self](const asio::error_code ec, std::size_t bytes) {
				if (ec) {
					std::cout << ec << std::endl;
					std::cout << "disconnecting" << std::endl;
					//close the connection
					return;
				}

				self->readBody();
			}
		);

		}

	//reads the body and then creates an async loop by calling readLength function again also calls the processMessage function
	void readBody() {
		
		auto self = shared_from_this();


		//convert from big endian
		uint32_t length =
		  (static_cast<unsigned char>(message_length[0]) << 24)
		| (static_cast<unsigned char>(message_length[1]) << 16)
		| (static_cast<unsigned char>(message_length[2]) << 8)
		|  static_cast<unsigned char>(message_length[3]);


		if (length < 1) {
			throw std::runtime_error("Invalid message length");
		}

		if (length > MAX_MESSAGE_SIZE){
			throw std::runtime_error("Invalid message length");
		}

		message_body.resize(length);

		asio::async_read(
			socket,
			asio::buffer(message_body),
			[self,length](asio::error_code ec, std::size_t bytes) {

				if (ec) {
					std::cout << ec << std::endl << "disconnecting" << std::endl;
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
	void processMessages(Message msg){

		switch (msg.type) {
		case MessageType::Handshake:
			handleHandshake(static_cast<uint8_t>(msg.body[0]));
			break;
		case MessageType::Chat:
			read_queue.push_back(msg);
			break;
		}

	}

	//writes the queue onto the socket
	void flushWriteQueue() {

		auto self = shared_from_this();


		asio::async_write(
			socket,
			asio::buffer(write_queue.front()),
			[self](const asio::error_code ec, std::size_t bytes) {
				if (ec) {
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
	void queueMessage(Message msg){
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
	void handleHandshake(uint8_t remote_id) {
		auto cur_session = shared_from_this();

		session_callback->onPeerIdentified(remote_id, cur_session);
	}

};

//manages all the sessions
//keeps track of which session is active
//displays the messages of the active session
class PeerManager : public SessionCallbackHandler
{
private:
	std::unordered_map<uint8_t, std::shared_ptr<Session>> active_sessions;
	asio::ip::tcp::socket server;
	asio::ip::tcp::acceptor acceptor;
	uint8_t peer_id;
	asio::io_context* io;
	
public:
	PeerManager(asio::io_context* io, uint8_t peer_id, int port)
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

	void onPeerIdentified(uint8_t id, std::shared_ptr<Session> session) override {
		if (active_sessions.contains(id)) {
			std::cout << "session already exists" << std::endl;
			//delete the session
		}
		std::cout << "succecsfully identified " << id << std::endl;
		active_sessions[id] = session;
	
	}

	void publicConnect() {
		connect("127.0.0.1", 8080);
	}

private:
	void accept() {
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

	void connect(const std::string& address, unsigned short port) {
		
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
	
};

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
