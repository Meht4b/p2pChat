#pragma once

#include <stdio.h>
#include <memory>

class ConsoleInterface
{
private:
	PeerManager* peer_manager;
	

public:
	ConsoleInterface();

	~ConsoleInterface();

	int getPort();

	uint8_t getLocalID();

	void processCommand();

private:

};
