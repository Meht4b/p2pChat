P2P Chat error-handling replacement files

Replace the matching project files with the 12 source/header files in this folder.

The updated code handles fatal session read/write/protocol errors by closing the socket and reporting the disconnect once; removes a disconnected session only if it is still the active session for that peer; keeps the direction-based duplicate-connection rule; stops the peer manager on fatal accept/network-loop errors; validates startup, connect, select, and send operations; and shuts the network thread down before UI objects are destroyed.

Validation: Message.cpp, Session.cpp, PeerManager.cpp, and Interface.cpp pass C++20 syntax checking with the project Asio headers. Full project compilation could not be checked here because the FTXUI headers/configuration are unavailable in this environment.
