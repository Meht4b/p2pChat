#pragma once

#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

class Interface;

class ConsoleInterface
{
public:
    explicit ConsoleInterface(Interface& interface);

    // starts the FTXUI event loop (blocks until the user quits)
    void run();

    // thread-safe: called from the network thread through Interface
    void printLine(const std::string& msg);

private:
    void handleInput(const std::string& command_line);
    void connect(std::istringstream& iss);
    void start(std::istringstream& iss);
    void help();
    void clear();
    void showPeers();

    Interface& interface;

    std::vector<std::string> output;
    std::string input;

    // shared between UI thread and network thread
    std::mutex state_mutex;
    std::function<void()> request_redraw;   // set while the UI loop is running

    // header status (filled in when the peer manager reports it started)
    bool online = false;
    int port = 0;
    int user_id = 0;
};