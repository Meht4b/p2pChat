#include "ConsoleInterface.h"
#include "Interface.h"

#include <ftxui/ftxui.hpp>
#include <sstream>
#include <iostream>
#include <exception>


using namespace ftxui;

ConsoleInterface::ConsoleInterface(Interface& interface)
    : interface(interface)
{
}

void ConsoleInterface::printLine(const std::string& message){
    std::lock_guard<std::mutex> lock(output_mutex);
    output.push_back(">> " + message);
}

void ConsoleInterface::printLineError(const std::string& message) {
    std::lock_guard<std::mutex> lock(output_mutex);
    output.push_back("[error] : " + message);
}

void ConsoleInterface::printLineSuccess(const std::string& message) {
    std::lock_guard<std::mutex> lock(output_mutex);
    output.push_back("[success] : " + message);
}

void ConsoleInterface::printLineIndent(const std::string& message) {
    std::lock_guard<std::mutex> lock(output_mutex);
    output.push_back("  " + message);
}

void ConsoleInterface::printMessage(const std::string& message, int user_id) {
    std::lock_guard<std::mutex> lock(output_mutex);
    output.push_back("[" + std::to_string(user_id) + "] : " + message);
}

void ConsoleInterface::connect(std::istringstream& iss) {
	std::string address;
	int port;

	if (!(iss >> address >> port) ||
		port < 1 || port > 65535)
	{
		printLineError("Usage: connect <address> <port>");
		return;
	}
    try {
		interface.connect(address, port);
    }
    catch (const std::exception& e) {
        printLineError(std::string("Connection error: ") + e.what());
    } catch (...) {
        printLineError("Connection error: unknown error");
    }

}

void ConsoleInterface::start(std::istringstream& iss) 
{
	int port;
	int user;

	if (!(iss >> port >> user) || port < 1 || port > 65535 || user<0 || user >=256)
	{
		printLineError("Usage: start <port> <user_id>");
		return;
	}




	interface.start(port, (uint8_t)user);
}

void ConsoleInterface::help()
{
	std::lock_guard<std::mutex> lock(output_mutex);
	output.push_back(">>Available commands:");
	output.push_back("  start <port> <user_id>");
	output.push_back("  connect <address> <port>");
	output.push_back("  showpeers");
	output.push_back("  help");
	output.push_back("  clear");
	output.push_back("  selectpeer <user_id>");
}

void ConsoleInterface::clear()
{
	std::lock_guard<std::mutex> lock(output_mutex);
	output.clear();
}

void ConsoleInterface::selectPeer(std::istringstream& iss) {
    int user = -1;
    if (!(iss >> user) || user < 0 || user > 255) {
        printLineError("Usage: selectpeer <user_id>");
        return;
    }
    if ( !interface.peerExists(user)) {
        printLineError("User has not been contacted/ user doesn't exist");
        return;
    }
	printLineSuccess("Selected peer " + std::to_string(user));
    clear();
    interface.selectPeer(user);
	peerSelected = true;

}

void ConsoleInterface::showPeers() {
    interface.showPeers();
}

// Handles all commands entered by the user
void ConsoleInterface::handleInput(const std::string& command_line)
{
    if (command_line.size() != 0 && command_line[0] == '/') {
		std::istringstream iss(command_line.substr(1));
		std::string command;
		iss >> command;

		try
		{
            if (command == "start") start(iss);
            else if (command == "connect") connect(iss);
            else if (command == "help") help();
            else if (command == "clear") clear();
            else if (command == "showpeers") showPeers();
            else if (command == "selectpeer") selectPeer(iss);
			else if (!command.empty())
			{
				printLineError("Unknown command " + command);
			}
		}
		catch (const std::exception& e)
		{
			printLineError(std::string("Command failed: ") + e.what());
		}
		catch (...) {
			printLineError("Command failed: unknown error");
		}

    }
    else if (peerSelected) {
        interface.write(command_line);
    }
    else {
        printLineError("illegal command/select peer first");
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

        {
            std::lock_guard<std::mutex> lock(output_mutex);
            for (const auto& line : output)
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

