#include "ConsoleInterface.h"
#include "Interface.h"

#include <ftxui/ftxui.hpp>
#include <sstream>
#include <iostream>


using namespace ftxui;

ConsoleInterface::ConsoleInterface(Interface& interface)
    : interface(interface)
{
}

void ConsoleInterface::printLine(const std::string& msg) {
    output.push_back(msg);
}

void ConsoleInterface::connect(std::istringstream& iss) {
	std::string address;
	int port;

	if (!(iss >> address >> port) ||
		port < 1 || port > 65535)
	{
		output.push_back("[error] : [usage] connect <address> <port>");
		return;
	}
    try {
		interface.connect(address, port);
    }
    catch (const std::exception& e) {
        printLine(std::string("[connection error] error ") + e.what());
    } catch (...) {
        printLine("[connection error] uknown");
    }

}

void ConsoleInterface::start(std::istringstream& iss) 
{
	int port;
	int user;

	if (!(iss >> port >> user) || port < 1 || port > 65535 || user<0 || user >=256)
	{
		output.push_back("[error] : [usage] start <port> <user_id>");
		return;
	}




	interface.start(port, (uint8_t)user);
}

void ConsoleInterface::help()
{
	output.push_back(">>Available commands:");
	output.push_back("  start <port> <user_id>");
	output.push_back("  connect <address> <port>");
	output.push_back("  showpeers");
	output.push_back("  help");
	output.push_back("  clear");
}

void ConsoleInterface::clear()
{
	output.clear();
}


void ConsoleInterface::showPeers() {
    interface.showPeers();
}

// Handles all commands entered by the user
void ConsoleInterface::handleInput(const std::string& command_line)
{
    std::istringstream iss(command_line);
    std::string command;
    iss >> command;

    try
    {
        if (command == "start") start(iss);
        else if (command == "connect") connect(iss);
        else if (command == "help") help();
        else if (command == "clear") clear();
        else if (command == "showpeers") showPeers();
        else if (!command.empty())
        {
            output.push_back("[error] : Unknown command " + command);
        }
    }
    catch (const std::exception& e)
    {
        output.push_back(std::string("Error: ") + e.what());
    }
}
// Starts the FTXUI interface
void ConsoleInterface::run()
{
    auto screen = ScreenInteractive::Fullscreen();

    // Configure the input field
    InputOption input_option;

    input_option.transform = [](InputState state)
    {
        return state.element;
    };

    auto input_component =
        Input(&input, "Enter command...", input_option);


    // Handle keyboard events
    input_component |= CatchEvent([this](Event event)
    {
        if (event == Event::Return)
        {
            if (!input.empty())
            {

                handleInput(input);

                input.clear();
            }

            return true;
        }

        return false;
    });


    // Render the interface
    auto component = Renderer(input_component, [this, &input_component]
    {
        Elements output_elements;

        for (const auto& line : output)
        {
            output_elements.push_back(text(line));
        }

        return vbox({

            text("P2P CHAT") | bold,

            separator(),

            vbox(std::move(output_elements)) | flex,

            separator(),

            hbox({
                text(">> "),
                input_component->Render() | flex
            })

        });
    });


    // Start the event loop
    screen.Loop(component);
}

