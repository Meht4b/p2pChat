#include "ConsoleInterface.h"
#include "Interface.h"

#include <algorithm>
#include <exception>
#include <utility>

#include <ftxui/ftxui.hpp>

using namespace ftxui;

// ===========================================================================
// View: pure functions that turn data into FTXUI elements.
// Borderless "floating" layout, ASCII glyphs only.
// ===========================================================================
namespace
{
    const Color kAccent  = Color::Cyan;      // menu / prompt
    const Color kSection = Color::Yellow;    // section titles
    const Color kChat    = Color::Magenta;   // everything related to the active chat

    constexpr int    kSidebarWidth = 32;
    constexpr size_t kTagWidth     = 6;
    constexpr size_t kKeyWidth     = 9;

    std::string padRight(std::string s, size_t width)
    {
        if (s.size() < width)
            s.resize(width, ' ');
        return s;
    }

    std::string peerLabel(uint8_t peer_id)
    {
        return "peer " + std::to_string(static_cast<int>(peer_id));
    }

    // ---- log entries -----------------------------------------------------

    struct EntryStyle
    {
        std::string tag;
        Color tag_color;
        Color text_color;
        bool bold_text;
    };

    EntryStyle styleOf(const LogEntry& entry)
    {
        switch (entry.level)
        {
        case LogLevel::Success:
            return { "ok", Color::Green, Color::GreenLight, false };
        case LogLevel::Error:
            return { "err", Color::Red, Color::RedLight, false };
        case LogLevel::Detail:
            return { "", Color::Default, kAccent, false };
        case LogLevel::Command:
            return { ">", kAccent, Color::Default, true };
        case LogLevel::Incoming:
            return { "[" + std::to_string(static_cast<int>(entry.peer_id)) + "]",
                     kChat, Color::Default, false };
        case LogLevel::Outgoing:
            return { "you", Color::BlueLight, Color::Default, false };
        case LogLevel::Info:
        default:
            return { "..", Color::GrayLight, Color::Default, false };
        }
    }

    Element renderEntry(const LogEntry& entry)
    {
        const EntryStyle style = styleOf(entry);

        Element body = paragraph(entry.text) | color(style.text_color) | flex;
        if (style.bold_text)
            body = body | bold;

        return hbox({
            text(padRight(style.tag, kTagWidth)) | bold | color(style.tag_color),
            std::move(body),
        });
    }

    // "Activity" in the menu, a highlighted chat banner while chatting.
    Element renderLogHeader(const ConsoleStatus& status)
    {
        if (!status.selected_peer)
            return text("Activity") | bold | color(kSection);

        return hbox({
            text(" CHAT ") | bold | color(Color::Black) | bgcolor(kChat),
            text("  " + peerLabel(*status.selected_peer)) | bold | color(kChat),
            filler(),
            text("/leave to go back") | dim,
        });
    }

    Element renderLog(const std::vector<LogEntry>& log, const ConsoleStatus& status)
    {
        Elements rows;
        for (const auto& entry : log)
            rows.push_back(renderEntry(entry));

        if (rows.empty())
            rows.push_back(text("nothing yet") | dim);
        else
            rows.back() = rows.back() | focus;      // pin the view to the newest line

        return vbox({
            renderLogHeader(status),
            text(""),
            vbox(std::move(rows)) | yframe | flex,
        }) | flex;
    }

    // ---- title -----------------------------------------------------------

    Element renderTitle(const ConsoleStatus& status)
    {
        Elements row = {
            text("p2p") | bold | color(kAccent),
            text(" chat") | bold,
        };

        if (status.selected_peer)
        {
            row.push_back(text("  /  ") | dim);
            row.push_back(text(peerLabel(*status.selected_peer)) | bold | color(kChat));
        }

        row.push_back(filler());
        row.push_back(status.online
            ? text("* online") | bold | color(Color::Green)
            : text("* offline") | color(Color::GrayLight));

        return hbox(std::move(row));
    }

    // ---- sidebar ---------------------------------------------------------

    Element section(const std::string& title, Elements items)
    {
        Elements rows;
        rows.push_back(text(title) | bold | color(kSection));
        for (auto& item : items)
            rows.push_back(hbox({ text("  "), std::move(item) }));
        return vbox(std::move(rows));
    }

    Element keyValue(const std::string& key, const std::string& value)
    {
        return hbox({
            text(padRight(key, kKeyWidth)) | dim,
            text(value) | bold,
        });
    }

    Element renderStatusSection(const ConsoleStatus& status)
    {
        if (!status.online)
        {
            return section("Status", {
                text("not started") | dim,
                text("run /start below") | dim,
            });
        }

        return section("Status", {
            keyValue("port",    std::to_string(status.port)),
            keyValue("user id", std::to_string(static_cast<int>(status.user_id))),
        });
    }

    // The peer being chatted with is shown as a highlighted block.
    Element renderPeerRow(uint8_t peer_id, bool chatting)
    {
        const std::string label = " " + peerLabel(peer_id) + " ";
        if (!chatting)
            return text(label);

        return hbox({
            text(label) | bold | color(Color::Black) | bgcolor(kChat),
            text(" chatting") | color(kChat),
        });
    }

    Element renderPeersSection(const ConsoleStatus& status)
    {
        Elements rows;
        for (uint8_t peer_id : status.peers)
            rows.push_back(renderPeerRow(peer_id, status.selected_peer == peer_id));

        if (rows.empty())
            rows.push_back(text("none yet") | dim);

        return section("Peers (" + std::to_string(status.peers.size()) + ")", std::move(rows));
    }

    Element renderCommandsSection(const std::vector<std::string>& usages)
    {
        Elements items;
        for (const auto& usage : usages)
            items.push_back(text(usage) | color(kAccent));
        return section("Commands", std::move(items));
    }

    Element renderSidebar(const ConsoleStatus& status,
                          const std::vector<std::string>& usages)
    {
        return vbox({
            renderStatusSection(status),
            text(""),
            renderPeersSection(status),
            text(""),
            renderCommandsSection(usages),
        }) | size(WIDTH, EQUAL, kSidebarWidth);
    }

    // ---- page ------------------------------------------------------------

    Element renderPrompt(const ConsoleStatus& status, Element input_field)
    {
        Element label = status.selected_peer
            ? text(peerLabel(*status.selected_peer) + " > ") | bold | color(kChat)
            : text("> ") | bold | color(kAccent);

        return hbox({
            std::move(label),
            std::move(input_field) | flex,
        });
    }

    Element renderPage(const ConsoleSnapshot& snapshot,
                       const std::vector<std::string>& usages,
                       Element input_field)
    {
        Element body = hbox({
            renderLog(snapshot.log, snapshot.status),
            text("    "),
            renderSidebar(snapshot.status, usages),
        }) | flex;

        return hbox({
            text("   "),
            vbox({
                text(""),
                renderTitle(snapshot.status),
                text(""),
                std::move(body),
                text(""),
                renderPrompt(snapshot.status, std::move(input_field)),
                text(""),
            }) | flex,
            text("   "),
        });
    }

    // ---- input component -------------------------------------------------

    // Text field that calls `on_submit` with the line when Enter is pressed.
    Component makeInput(std::string* content,
                        std::function<void(const std::string&)> on_submit)
    {
        InputOption option;
        option.transform = [](InputState state)
        {
            if (state.is_placeholder)
                return state.element | dim;
            return state.element;
        };

        auto field = Input(content, "message or /command...", option);

        return field | CatchEvent(
            [content, on_submit = std::move(on_submit)](Event event)
            {
                if (event != Event::Return)
                    return false;

                if (!content->empty())
                {
                    const std::string line = std::move(*content);
                    content->clear();
                    on_submit(line);
                }
                return true;
            });
    }

    // Runs a callback when it goes out of scope (also on exceptions).
    class ScopeExit
    {
    public:
        explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
        ~ScopeExit() { if (fn_) fn_(); }
        ScopeExit(const ScopeExit&) = delete;
        ScopeExit& operator=(const ScopeExit&) = delete;
    private:
        std::function<void()> fn_;
    };
}

// ===========================================================================
// Controller: commands, input handling, thread-safe state.
// ===========================================================================

ConsoleInterface::ConsoleInterface(Interface& interface)
    : interface(interface)
{
    commands = {
        { "start",      "/start <port> <user_id>",
          [this](std::istringstream& args) { return start(args); } },

        { "connect",    "/connect <address> <port>",
          [this](std::istringstream& args) { return connect(args); } },

        { "showpeers",  "/showpeers",
          [this](std::istringstream&) { this->interface.showPeers(); return true; } },

        { "selectpeer", "/selectpeer <user_id>",
          [this](std::istringstream& args) { return selectPeer(args); } },

        { "leave",      "/leave",
          [this](std::istringstream& args) { return leavePeer(args); } },

        { "help",       "/help",
          [this](std::istringstream&) { showHelp(); return true; } },

        { "clear",      "/clear",
          [this](std::istringstream&) { clearLog(); return true; } },
    };
}

// ---- log output ------------------------------------------------------------

void ConsoleInterface::printLine(const std::string& msg)        { append({ LogLevel::Info,     msg }); }
void ConsoleInterface::printLineSuccess(const std::string& msg) { append({ LogLevel::Success,  msg }); }
void ConsoleInterface::printLineError(const std::string& msg)   { append({ LogLevel::Error,    msg }); }
void ConsoleInterface::printLineIndent(const std::string& msg)  { append({ LogLevel::Detail,   msg }); }
void ConsoleInterface::printSentMessage(const std::string& msg) { append({ LogLevel::Outgoing, msg }); }

void ConsoleInterface::printMessage(const std::string& msg, int user_id)
{
    append({ LogLevel::Incoming, msg, static_cast<uint8_t>(user_id) });
}

// ---- status ----------------------------------------------------------------

void ConsoleInterface::setOnline(uint16_t port, uint8_t user_id)
{
    updateStatus([&](ConsoleStatus& s)
    {
        s = ConsoleStatus{};
        s.online = true;
        s.port = port;
        s.user_id = user_id;
    });
}

void ConsoleInterface::setOffline()
{
    updateStatus([](ConsoleStatus& s) { s = ConsoleStatus{}; });
}

void ConsoleInterface::onPeerConnected(uint8_t peer_id)
{
    updateStatus([&](ConsoleStatus& s) { s.peers.insert(peer_id); });
}

void ConsoleInterface::onPeerDisconnected(uint8_t peer_id)
{
    updateStatus([&](ConsoleStatus& s)
    {
        s.peers.erase(peer_id);
        if (s.selected_peer == peer_id)
            s.selected_peer.reset();
    });
}

void ConsoleInterface::setSelectedPeer(std::optional<uint8_t> peer_id)
{
    updateStatus([&](ConsoleStatus& s) { s.selected_peer = peer_id; });
}

// ---- input handling --------------------------------------------------------

// "/command args" runs a command; anything else is sent to the selected peer.
void ConsoleInterface::handleInput(const std::string& line)
{
    if (line.empty())
        return;

    if (line.front() == '/')
        runCommand(line.substr(1));
    else if (hasSelectedPeer())
        interface.write(line);
    else
        printLineError("No peer selected: use /selectpeer <user_id>");
}

void ConsoleInterface::runCommand(const std::string& command_line)
{
    append({ LogLevel::Command, "/" + command_line });

    std::istringstream args(command_line);
    std::string name;
    args >> name;
    if (name.empty())
        return;

    const Command* command = findCommand(name);
    if (!command)
    {
        printLineError("Unknown command " + name);
        return;
    }

    try
    {
        if (!command->handler(args))
            printLineError("Usage: " + command->usage);
    }
    catch (const std::exception& e)
    {
        printLineError(std::string("Command failed: ") + e.what());
    }
    catch (...)
    {
        printLineError("Command failed: unknown error");
    }
}

const ConsoleInterface::Command* ConsoleInterface::findCommand(const std::string& name) const
{
    auto it = std::find_if(commands.begin(), commands.end(),
        [&](const Command& c) { return c.name == name; });
    return it == commands.end() ? nullptr : &*it;
}

std::vector<std::string> ConsoleInterface::commandUsages() const
{
    std::vector<std::string> usages;
    usages.reserve(commands.size());
    for (const auto& command : commands)
        usages.push_back(command.usage);
    return usages;
}

// ---- commands --------------------------------------------------------------

bool ConsoleInterface::start(std::istringstream& args)
{
    int port, user;
    if (!(args >> port >> user) || port < 1 || port > 65535 || user < 0 || user > 255)
        return false;

    interface.start(port, static_cast<uint8_t>(user));
    return true;
}

bool ConsoleInterface::connect(std::istringstream& args)
{
    std::string address;
    int port;
    if (!(args >> address >> port) || port < 1 || port > 65535)
        return false;

    interface.connect(address, port);
    return true;
}

bool ConsoleInterface::selectPeer(std::istringstream& args)
{
    int user;
    if (!(args >> user) || user < 0 || user > 255)
        return false;

    if (!interface.peerExists(user))
    {
        printLineError("User has not been contacted / doesn't exist");
        return true;
    }

    clearLog();
    setSelectedPeer(static_cast<uint8_t>(user));
    printLineSuccess("Now chatting with peer " + std::to_string(user));
    interface.selectPeer(user);     // queued messages are delivered after this line
    return true;
}

// Leaves the active chat and returns to the main menu.
bool ConsoleInterface::leavePeer(std::istringstream&)
{
    if (!hasSelectedPeer())
    {
        printLineError("No peer selected");
        return true;
    }

    interface.deselectPeer();
    setSelectedPeer(std::nullopt);
    clearLog();
    printLine("Back to main menu");
    return true;
}

void ConsoleInterface::showHelp()
{
    printLine("Available commands:");
    for (const auto& command : commands)
        printLineIndent(command.usage);
}

void ConsoleInterface::clearLog()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    output.clear();
    redrawLocked();
}

// ---- thread-safe state access ----------------------------------------------

void ConsoleInterface::append(LogEntry entry)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    output.push_back(std::move(entry));
    redrawLocked();
}

void ConsoleInterface::updateStatus(const std::function<void(ConsoleStatus&)>& change)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    change(status);
    redrawLocked();
}

bool ConsoleInterface::hasSelectedPeer() const
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return status.selected_peer.has_value();
}

ConsoleSnapshot ConsoleInterface::snapshot() const
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return { output, status };
}

void ConsoleInterface::setRedraw(std::function<void()> redraw)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    request_redraw = std::move(redraw);
}

// Wakes the UI thread so new output shows up without waiting for a keypress.
void ConsoleInterface::redrawLocked() const
{
    if (request_redraw)
        request_redraw();
}

// ---- UI loop ---------------------------------------------------------------

void ConsoleInterface::run()
{
    auto screen = ScreenInteractive::Fullscreen();

    auto input_field = makeInput(&input,
        [this](const std::string& line) { handleInput(line); });

    auto page = Renderer(input_field, [&]
    {
        return renderPage(snapshot(), commandUsages(), input_field->Render());
    });

    setRedraw([&screen] { screen.PostEvent(Event::Custom); });
    ScopeExit unbind([this] { setRedraw(nullptr); });

    screen.Loop(page);
}
