#pragma once

#include <memory>
#include <string>

class PeerManager;

// Owns terminal input and drawing.  It deliberately knows nothing about ASIO.
// ASIO callbacks call postLine(); run() is normally called on main().
class ConsoleInterface
{
public:
    explicit ConsoleInterface(PeerManager& peerManager);
    ~ConsoleInterface();

    ConsoleInterface(const ConsoleInterface&) = delete;
    ConsoleInterface& operator=(const ConsoleInterface&) = delete;

    // Starts the persistent terminal interface. This blocks until stop() is called.
    void run();
    void stop();

    // Safe to call from an ASIO completion handler or any other thread.
    void postLine(std::string line);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
