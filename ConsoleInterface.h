#pragma once

#include <string>
#include <sstream>
#include <vector>
#include <mutex>

class Interface;

class ConsoleInterface
{
private:
    Interface& interface;
    std::string input;
    std::vector<std::string> output;
    mutable std::mutex output_mutex;
    bool peerSelected = false;

    void handleInput(const std::string& command);
    void start(std::istringstream& iss);
    void help();
    void clear();
    void showPeers();
    void selectPeer(std::istringstream& iss);
    void connect(std::istringstream& iss);

public:
    explicit ConsoleInterface(Interface& interface);
    void run();
    void printLine(const std::string& msg);
    void printLineSuccess(const std::string& msg);
    void printLineError(const std::string& msg);
    void printLineIndent(const std::string& msg);
	void printMessage(const std::string& msg, int user_id);

};
