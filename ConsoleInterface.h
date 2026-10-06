#pragma once

#include <string>
#include <vector>

class Interface;

class ConsoleInterface
{
private:
    Interface& interface;
    std::string input;
    std::vector<std::string> output;

    void handleInput(const std::string& command);
    void start(std::istringstream& iss);
    void help();
    void clear();
    void showPeers();
    void connect(std::istringstream& iss);

public:
    explicit ConsoleInterface(Interface& interface);
    void run();
    void printLine(const std::string& msg);

};