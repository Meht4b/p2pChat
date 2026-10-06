#include "ConsoleInterface.h"
#include "Interface.h"

#include <ftxui/ftxui.hpp>
#include <iostream>
#include <sstream>

using namespace ftxui;

// ---------------------------------------------------------------------------
// Borderless "floating" layout: no boxes, no separators. Structure comes from
// spacing, color and a left sidebar. Every glyph used is plain ASCII.
// ---------------------------------------------------------------------------
namespace
{
    const Color kAccent  = Color::Cyan;
    const Color kLabel   = Color::Yellow;

    bool startsWith(const std::string& s, const std::string& prefix)
    {
        return s.compare(0, prefix.size(), prefix) == 0;
    }

    // removes a leading ">>" or "[tag]" plus separators (" : ")
    std::string stripTag(const std::string& line)
    {
        size_t pos = 0;

        if (startsWith(line, ">>"))
            pos = 2;
        else if (!line.empty() && line[0] == '[')
        {
            pos = line.find(']');
            pos = (pos == std::string::npos) ? 0 : pos + 1;
        }

        while (pos < line.size() && (line[pos] == ' ' || line[pos] == ':'))
            ++pos;

        return line.substr(pos);
    }

    // reads the integer that follows `key` inside `s`
    bool intAfter(const std::string& s, const std::string& key, int& out)
    {
        size_t p = s.find(key);
        if (p == std::string::npos)
            return false;
        try {
            out = std::stoi(s.substr(p + key.size()));
            return true;
        }
        catch (...) {
            return false;
        }
    }

    // "tag   message" row used by the activity log
    Element logRow(const std::string& tag, Color tag_color, Element body)
    {
        std::string padded = tag;
        padded.resize(6, ' ');
        return hbox({
            text(padded) | bold | color(tag_color),
            std::move(body),
        });
    }

    Element styleLine(const std::string& line)
    {
        // echoed user command
        if (startsWith(line, "> "))
            return logRow(">", kAccent, text(line.substr(2)) | bold);

        if (startsWith(line, "[error]") || startsWith(line, "[connection error]"))
            return logRow("err", Color::Red,
                          paragraph(stripTag(line)) | color(Color::RedLight) | flex);

        if (startsWith(line, "[success]"))
            return logRow("ok", Color::Green,
                          paragraph(stripTag(line)) | color(Color::GreenLight) | flex);

        if (startsWith(line, ">>"))
            return logRow("..", Color::GrayLight,
                          paragraph(stripTag(line)) | flex);

        // indented lines (command list, peer list)
        if (startsWith(line, "  "))
            return logRow("", Color::White,
                          text(line.substr(2)) | color(kAccent));

        return logRow("", Color::White, paragraph(line) | flex);
    }

    // sidebar section: yellow label, indented content underneath
    Element section(const std::string& title, Elements items)
    {
        Elements rows;
        rows.push_back(text(title) | bold | color(kLabel));
        for (auto& it : items)
            rows.push_back(hbox({ text("  "), std::move(it) }));
        return vbox(std::move(rows));
    }

    Element kv(const std::string& key, const std::string& value)
    {
        return hbox({
            text(key) | dim,
            text(value) | bold,
        });
    }
}

ConsoleInterface::ConsoleInterface(Interface& interface)
    : interface(interface)
{
}

// thread-safe: may be called from the asio thread
void ConsoleInterface::printLine(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    output.push_back(msg);

    // the peer manager announces itself with this line -> update status
    if (startsWith(msg, "[success] : Peer manager started"))
    {
        int p = 0, u = 0;
        if (intAfter(msg, "port = ", p) && intAfter(msg, "user id = ", u))
        {
            online = true;
            port = p;
            user_id = u;
        }
    }

    if (request_redraw)
        request_redraw();   // wake the UI so it redraws now
}

void ConsoleInterface::connect(std::istringstream& iss) {
    std::string address;
    int port;

    if (!(iss >> address >> port) ||
        port < 1 || port > 65535)
    {
        printLine("[error] : [usage] connect <address> <port>");
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

    if (!(iss >> port >> user) || port < 1 || port > 65535 || user < 0 || user >= 256)
    {
        printLine("[error] : [usage] start <port> <user_id>");
        return;
    }

    interface.start(port, (uint8_t)user);
}

void ConsoleInterface::help()
{
    printLine(">>Available commands:");
    printLine("  start <port> <user_id>");
    printLine("  connect <address> <port>");
    printLine("  showpeers");
    printLine("  help");
    printLine("  clear");
}

void ConsoleInterface::clear()
{
    std::lock_guard<std::mutex> lock(state_mutex);
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

    // echo the command (not for "clear", so the log actually empties)
    if (!command.empty() && command != "clear")
        printLine("> " + command_line);

    try
    {
        if (command == "start") start(iss);
        else if (command == "connect") connect(iss);
        else if (command == "help") help();
        else if (command == "clear") clear();
        else if (command == "showpeers") showPeers();
        else if (!command.empty())
        {
            printLine("[error] : Unknown command " + command);
        }
    }
    catch (const std::exception& e)
    {
        printLine(std::string("[error] : ") + e.what());
    }
}

// Starts the FTXUI interface
void ConsoleInterface::run()
{
    auto screen_ui = ScreenInteractive::Fullscreen();

    // Input field: dim placeholder, no border
    InputOption input_option;
    input_option.transform = [](InputState state)
    {
        if (state.is_placeholder)
            return state.element | dim;
        return state.element;
    };

    auto input_component =
        Input(&input, "type a command...", input_option);

    // Handle keyboard events
    input_component |= CatchEvent([this](Event event)
    {
        if (event == Event::Return)
        {
            if (!input.empty())
            {
                std::string cmd = input;
                input.clear();
                handleInput(cmd);
            }
            return true;
        }
        return false;
    });

    // Render the interface
    auto component = Renderer(input_component, [this, &input_component]
    {
        // snapshot shared state so the network thread can't change it mid-render
        std::vector<std::string> lines;
        bool is_online;
        int cur_port, cur_user;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            lines = output;
            is_online = online;
            cur_port = port;
            cur_user = user_id;
        }

        // ---- title row ----
        auto title = hbox({
            text("p2p") | bold | color(kAccent),
            text(" chat") | bold,
            filler(),
            is_online
                ? (text("* online") | bold | color(Color::Green))
                : (text("* offline") | color(Color::GrayLight)),
        });

        // ---- sidebar ----
        Elements status_items;
        if (is_online)
        {
            status_items.push_back(kv("port     ", std::to_string(cur_port)));
            status_items.push_back(kv("user id  ", std::to_string(cur_user)));
        }
        else
        {
            status_items.push_back(text("not started") | dim);
            status_items.push_back(text("run 'start' below") | dim);
        }

        Elements command_items;
        command_items.push_back(text("start <port> <user_id>") | color(kAccent));
        command_items.push_back(text("connect <address> <port>") | color(kAccent));
        command_items.push_back(text("showpeers") | color(kAccent));
        command_items.push_back(text("help") | color(kAccent));
        command_items.push_back(text("clear") | color(kAccent));

        auto sidebar = vbox({
            section("Status", std::move(status_items)),
            text(""),
            section("Commands", std::move(command_items)),
        }) | size(WIDTH, EQUAL, 32);

        // ---- activity log ----
        Elements log;
        if (lines.empty())
        {
            log.push_back(text("nothing yet") | dim);
        }
        for (size_t i = 0; i < lines.size(); ++i)
        {
            Element e = styleLine(lines[i]);
            if (i + 1 == lines.size())
                e = e | focus;          // keep view pinned to the newest line
            log.push_back(std::move(e));
        }

        auto activity = vbox({
            text("Activity") | bold | color(kLabel),
            text(""),
            vbox(std::move(log)) | yframe | flex,
        }) | flex;

        // ---- prompt ----
        auto prompt = hbox({
            text("> ") | bold | color(kAccent),
            input_component->Render() | flex,
        });

        // ---- page: everything floats, padded with plain spaces ----
        auto body = hbox({
            activity,
            sidebar,
            text("    "),
        }) | flex;

        return hbox({
            text("   "),
            vbox({
                text(""),
                title,
                text(""),
                body,
                text(""),
                prompt,
                text(""),
            }) | flex,
            text("   "),
        });
    });

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        request_redraw = [&screen_ui] { screen_ui.PostEvent(Event::Custom); };
    }

    // Start the event loop
    screen_ui.Loop(component);

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        request_redraw = nullptr;
    }
}