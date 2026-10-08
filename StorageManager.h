#pragma once

#include <vector>

class StorageManager
{
private:
	std::vector<std::pair<uint8_t, std::string>> message_queue; // Pair of peer_id and message

public:
	StorageManager();

	//will add the message to the write queue and return true if the message was added successfully
	bool queueMessageWrite(uint8_t peer_id, const std::string& message);

	//flushes the write queue to the storage and returns true if the flush was successful
	bool messageWrite();

	//loads the messages for the given peer_id and returns them as a vector of strings
	std::vector<std::string> loadMessages(uint8_t peer_id);
	
};
