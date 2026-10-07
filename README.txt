P2P Chat error-handling replacement files

Replace the matching project files with the 12 source/header files in this folder.

The updated code handles fatal session read/write/protocol errors by closing the socket and reporting the disconnect once; removes a disconnected session only if it is still the active session for that peer; keeps the direction-based duplicate-connection rule; stops the peer manager on fatal accept/network-loop errors; validates startup, connect, select, and send operations; and shuts the network thread down before UI objects are destroyed.

Validation: Message.cpp, Session.cpp, PeerManager.cpp, and Interface.cpp pass C++20 syntax checking with the project Asio headers. Full project compilation could not be checked here because the FTXUI headers/configuration are unavailable in this environment.

After a fatal PeerManager error the manager is marked stopped. A later /start joins the finished network worker, releases the stopped manager, restarts the io_context, and attempts startup again.

The console output vector is synchronized: all appends use one locked helper, clearing takes the same mutex, and rendering copies a locked snapshot before building UI elements.

Architecture update: PeerManager now depends on PeerManagerCallbackHandler rather than Interface. Interface inherits and implements that callback interface. The callback methods carry Asio error codes or structured peer/message data; the Interface decides how to display or handle them. Session now reports failures through SessionCallbackHandler and has no Interface dependency. Copy the new PeerManagerCallbackHandler.h into the project directory with the other replacement files.
