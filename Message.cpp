#include "Message.h"

#include <cstring>
#include <stdexcept>

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