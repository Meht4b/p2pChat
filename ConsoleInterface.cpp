#include "ConsoleInterface.h"
#include "Interface.h"
#include "TextSanitizer.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

#include <ftxui/ftxui.hpp>

using namespace ftxui;

// ===========================================================================
// View: a terminal-native take on the Nullnet website visual system: near-black
// canvas, warm neutrals, coral highlights, fine panel rules, and small network
// labels. All content remains ordinary FTXUI elements so it works in terminals.
// ===========================================================================
namespace
{
    const Color kCanvas  = Color::RGB(8, 8, 8);
    const Color kPanel   = Color::RGB(16, 16, 15);
    const Color kRule    = Color::RGB(53, 51, 48);
    const Color kInk     = Color::RGB(240, 235, 226);
    const Color kWarm    = Color::RGB(190, 182, 169);
    const Color kMuted   = Color::RGB(133, 127, 117);
    const Color kCoral   = Color::RGB(255, 111, 82);
    const Color kOnline  = Color::RGB(132, 169, 146);

    constexpr int    kWideSidebarWidth = 34;
    constexpr int    kCompactSidebarWidth = 28;
    constexpr size_t kTagWidth = 12;
    constexpr size_t kKeyWidth = 9;
    constexpr uint16_t kDefaultPort = 5000;

    std::string padRight(std::string s, size_t width)
    {
        if (s.size() < width)
            s.resize(width, ' ');
        return s;
    }

    std::string truncate(const std::string& s, size_t width)
    {
        return s.size() <= width ? s : s.substr(0, width);
    }

    std::string toLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool parseInt(const std::string& text, int min, int max, int& out)
    {
        try
        {
            size_t used = 0;
            const int value = std::stoi(text, &used);
            if (used != text.size() || value < min || value > max)
                return false;
            out = value;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::string routeLabel(const ConsoleStatus& status)
    {
        if (!status.selected_peer) return status.online ? "LISTENING" : "OFFLINE";
        const auto found = status.peer_paths.find(*status.selected_peer);
        return found == status.peer_paths.end()
            ? connectionPathLabel(ConnectionPath::Unknown)
            : connectionPathLabel(found->second);
    }

    // Display name of a peer: its username if we know it, else the short id.
    std::string peerLabel(const ConsoleStatus& status, const PeerId& peer_id)
    {
        const auto it = status.peers.find(peer_id);
        return it != status.peers.end() ? it->second : peer_id.shortHex();
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
            return { "OK", kOnline, kOnline, false };
        case LogLevel::Error:
            return { "ERR", kCoral, kCoral, false };
        case LogLevel::Detail:
            return { "", kMuted, kWarm, false };
        case LogLevel::Command:
            return { ">", kCoral, kInk, true };
        case LogLevel::Incoming:
            return { truncate(entry.sender, kTagWidth - 1), kCoral, kInk, false };
        case LogLevel::Outgoing:
            return { "YOU", kMuted, kWarm, false };
        case LogLevel::Info:
        default:
            return { "INFO", kMuted, kWarm, false };
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

    Element panelLabel(const std::string& title, const std::string& detail = "")
    {
        return hbox({
            text(title) | bold | color(kCoral),
            filler(),
            text(detail) | color(kMuted),
        });
    }

    Element renderLog(const std::vector<LogEntry>& log,
                      const ConsoleStatus& status,
                      std::size_t start,
                      std::size_t visible_count)
    {
        const std::size_t end = std::min(log.size(), start + visible_count);
        Elements rows;
        for (std::size_t i = start; i < end; ++i)
            rows.push_back(renderEntry(log[i]));

        if (rows.empty())
            rows.push_back(text("Your words. Your path. Start with /connect.") | color(kMuted));

        const bool more_above = start > 0;
        const bool more_below = end < log.size();
        Element messages = vbox(std::move(rows)) | flex;
        const std::string scroll_state = more_above && more_below ? "^v"
            : more_above ? "^" : more_below ? "v" : "";
        const std::string title = status.selected_peer
            ? "SESSION / " + peerLabel(status, *status.selected_peer)
            : "ACTIVITY / NODE";
        const std::string detail = routeLabel(status);

        return vbox({
            panelLabel(title, detail),
            separator() | color(kRule),
            std::move(messages),
            hbox({
                filler(),
                text(scroll_state.empty() ? " " : scroll_state) | color(kCoral),
            }),
        }) | borderStyled(kRule) | bgcolor(kPanel) | flex;
    }

    // ---- app chrome ------------------------------------------------------

    Element renderTitle(const ConsoleStatus& status)
    {
        const std::string user = status.self.username.empty()
            ? "UNCONFIGURED"
            : truncate(status.self.username, 18);
        const std::string network = status.online
            ? "QUIC / UDP " + std::to_string(status.port)
            : "NETWORK OFFLINE";

        return hbox({
            text(" o--o ") | color(kWarm),
            text("nullnet") | bold | color(kInk),
            text(".") | bold | color(kCoral),
            text("   /   PRIVATE PEER CHAT") | color(kMuted),
            filler(),
            text(status.online ? "* " : "o ") | color(status.online ? kOnline : kMuted),
            text(network) | color(status.online ? kInk : kMuted),
            text("   ") | color(kRule),
            text(user) | color(kWarm),
        });
    }

    // ---- identity, peers, and command panels ----------------------------

    Element keyValue(const std::string& key, const std::string& value)
    {
        return hbox({
            text(padRight(key, kKeyWidth)) | color(kMuted),
            text(value) | color(kInk),
        });
    }

    Element renderPeerRow(const std::string& name, const PeerId& peer_id, bool chatting,
                          int width)
    {
        const size_t id_width = width >= 32 ? 9 : 6;
        const size_t name_width = width >= 32 ? 13 : 8;
        return hbox({
            text(chatting ? "> " : "  ") | color(chatting ? kCoral : kMuted),
            text(truncate(name, name_width)) | bold | color(chatting ? kInk : kWarm),
            filler(),
            text(truncate(peer_id.shortHex(), id_width)) | color(kMuted),
        });
    }

    Element renderPeersSection(const ConsoleStatus& status, int width, bool compact)
    {
        Elements rows;
        const size_t limit = compact ? 1 : status.peers.size();
        size_t shown = 0;
        const auto selected = status.selected_peer
            ? status.peers.find(*status.selected_peer)
            : status.peers.end();
        if (compact && selected != status.peers.end())
        {
            rows.push_back(renderPeerRow(selected->second, selected->first, true, width));
            shown = 1;
        }
        for (const auto& [peer_id, name] : status.peers)
        {
            if (compact && selected != status.peers.end() && peer_id == selected->first)
                continue;
            if (shown++ >= limit)
                break;
            rows.push_back(renderPeerRow(name, peer_id,
                status.selected_peer == peer_id, width));
        }

        if (rows.empty())
            rows.push_back(text("No active peers") | color(kMuted));
        else if (compact && status.peers.size() > limit)
            rows.push_back(text("+" + std::to_string(status.peers.size() - limit) + " more /showpeers") | color(kMuted));

        return vbox({
            panelLabel("PEERS", std::to_string(status.peers.size()) + " ONLINE"),
            vbox(std::move(rows)),
        });
    }

    Element renderCommandsSection(const std::vector<std::string>& usages)
    {
        std::vector<std::string> names;
        for (const auto& usage : usages)
        {
            const auto end = usage.find_first_of(" \t|");
            const std::string command = usage.substr(0, end);
            names.push_back(command.empty() ? usage : command);
        }
        Elements items;
        for (std::size_t i = 0; i < names.size(); i += 2) {
            Element left = text(names[i]) | color(kWarm);
            Element right = i + 1 < names.size()
                ? text(names[i + 1]) | color(kWarm)
                : text("");
            items.push_back(hbox({std::move(left), filler(), std::move(right)}));
        }
        return vbox({
            panelLabel("COMMANDS", "/help for syntax"),
            vbox(std::move(items)),
        });
    }

    Element renderSidebar(const ConsoleStatus& status,
                          const std::vector<std::string>& usages,
                          int width,
                          int terminal_height)
    {
        const bool compact = terminal_height < 32;
        Elements identity;
        if (!status.self.username.empty())
        {
            if (compact)
            {
                identity.push_back(keyValue("id", status.self.id.shortHex()));
            }
            else
            {
                identity.push_back(keyValue("id", status.self.id.shortHex()));
            }
        }
        else
        {
            identity.push_back(text(status.online ? "Listening" : "Run /start") | color(kMuted));
        }

        return vbox({
            vbox({ panelLabel("IDENTITY / 01"), vbox(std::move(identity)) }),
            separator() | color(kRule),
            renderPeersSection(status, width, compact),
            separator() | color(kRule),
            renderCommandsSection(usages),
            separator() | color(kRule),
            vbox({
                panelLabel("KEYS"),
                hbox({ text("Ctrl+P/N") | color(kWarm), filler(), text("scroll") | color(kMuted) }),
                hbox({ text("PgUp/PgDn") | color(kWarm), filler(), text("page") | color(kMuted) }),
                hbox({ text("Up/Down") | color(kWarm), filler(), text("history") | color(kMuted) }),
            }),
        }) | borderStyled(kRule) | bgcolor(kPanel) | size(WIDTH, EQUAL, width);
    }

    // ---- page ------------------------------------------------------------

    Element renderPrompt(InputMode mode, const ConsoleStatus& status, Element input_field)
    {
        Element label;
        if (mode == InputMode::SetupUsername)
            label = text("USERNAME  > ") | bold | color(kCoral);
        else if (mode == InputMode::SetupPort)
            label = text("PORT  > ") | bold | color(kCoral);
        else if (status.selected_peer)
            label = text("YOU  > ") | bold | color(kCoral);
        else
            label = text("COMMAND  > ") | bold | color(kCoral);

        return hbox({
            std::move(label),
            std::move(input_field) | flex,
        }) | bgcolor(kPanel) | color(kInk);
    }

    Element renderInputPanel(InputMode mode, const ConsoleStatus& status, Element input_field)
    {
        Element prompt = renderPrompt(mode, status, std::move(input_field));
        if (mode == InputMode::Command)
            return vbox({ std::move(prompt) }) | borderStyled(kRule) | bgcolor(kPanel);

        return vbox({
            panelLabel("FIRST-RUN SETUP"),
            std::move(prompt),
        }) | borderStyled(kRule) | bgcolor(kPanel);
    }

    Element renderPage(const ConsoleSnapshot& snapshot,
                       const std::vector<std::string>& usages,
                       InputMode mode,
                       Element input_field,
                       int terminal_width,
                       int terminal_height)
    {
        Element conversation = renderLog(snapshot.log, snapshot.status,
            snapshot.log_start, snapshot.visible_log_count);
        Element workspace;
        if (terminal_width >= 74)
        {
            const int sidebar_width = terminal_width >= 106
                ? kWideSidebarWidth : kCompactSidebarWidth;
            workspace = hbox({
                std::move(conversation) | flex,
                text("  "),
                renderSidebar(snapshot.status, usages, sidebar_width, terminal_height),
            }) | size(WIDTH, EQUAL, terminal_width) | flex;
        }
        else
        {
            // At narrow widths keep the conversation readable. Identity and
            // peer details remain available through /known and /showpeers.
            workspace = std::move(conversation) | flex;
        }

        return vbox({
            renderTitle(snapshot.status),
            separator() | color(kRule),
            std::move(workspace) | flex,
            renderInputPanel(mode, snapshot.status, std::move(input_field)),
        }) | bgcolor(kCanvas) | color(kInk) |
            size(WIDTH, EQUAL, terminal_width) |
            size(HEIGHT, EQUAL, terminal_height) | flex;
    }

    // ---- input component -------------------------------------------------

    // Text field that calls `on_submit` with the line when Enter is pressed
    // (also for an empty line: the setup questions have defaults).
    Component makeInput(std::string* content, std::string* placeholder,
                        std::function<void(const std::string&)> on_submit)
    {
        InputOption option;
        option.transform = [](InputState state)
        {
            if (state.is_placeholder)
                return state.element | dim;
            return state.element;
        };

        auto field = Input(content, placeholder, option);

        return field | CatchEvent(
            [content, on_submit = std::move(on_submit)](Event event)
            {
                if (event != Event::Return)
                    return false;

                const std::string line = std::move(*content);
                content->clear();
                on_submit(line);
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
        { "start",      "/start [port]",
          [this](std::istringstream& args) { return start(args); } },

        { "connect",    "/connect <local-ip> <port> | /connect <room-token> <relay-server[:port]>",
          [this](std::istringstream& args) { return connect(args); } },

        { "showpeers",  "/showpeers",
          [this](std::istringstream&) { this->interface.showPeers(); return true; } },

        { "known",      "/known",
          [this](std::istringstream&) { this->interface.showKnownPeers(); return true; } },

        { "selectpeer", "/selectpeer <name|id>",
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

void ConsoleInterface::printMessage(const std::string& msg, const PeerId& sender)
{
    append({ LogLevel::Incoming, msg, peerName(sender) });
}

// ---- status ----------------------------------------------------------------

void ConsoleInterface::setOnline(uint16_t port, const PeerInfo& self)
{
    updateStatus([&](ConsoleStatus& s)
    {
        s.online = true;
        s.port = port;
        s.self = self;
        s.peers.clear();
        s.peer_paths.clear();
        s.selected_peer.reset();
    });
}

void ConsoleInterface::setOffline()
{
    updateStatus([](ConsoleStatus& s)
    {
        s.online = false;
        s.port = 0;
        s.peers.clear();
        s.peer_paths.clear();
        s.selected_peer.reset();
    });
}

void ConsoleInterface::onPeerConnected(const PeerInfo& peer, ConnectionPath path)
{
    updateStatus([&](ConsoleStatus& s) {
        s.peers[peer.id] = peer.username;
        s.peer_paths[peer.id] = path;
    });
}

void ConsoleInterface::onPeerDisconnected(const PeerId& peer_id)
{
    updateStatus([&](ConsoleStatus& s)
    {
        s.peers.erase(peer_id);
        s.peer_paths.erase(peer_id);
        if (s.selected_peer == peer_id)
            s.selected_peer.reset();
    });
}

void ConsoleInterface::setSelectedPeer(std::optional<PeerId> peer_id)
{
    updateStatus([&](ConsoleStatus& s) { s.selected_peer = peer_id; });
}

// ---- input handling --------------------------------------------------------

// "/command args" runs a command; anything else is sent to the selected peer.
void ConsoleInterface::handleInput(const std::string& line)
{
    if (mode != InputMode::Command)
    {
        handleSetupInput(line);
        return;
    }

    if (line.empty())
        return;

    if (line.front() == '/')
    {
        command_history.push_back(line);
        command_history_position = command_history.size();
        command_history_draft.clear();
        runCommand(line.substr(1));
    }
    else if (hasSelectedPeer())
        interface.write(line);
    else
        printLineError("No peer selected: use /selectpeer <name|id>");

    command_history_position = command_history.size();
    command_history_draft.clear();
}

bool ConsoleInterface::navigateCommandHistory(int direction)
{
    if (mode != InputMode::Command || command_history.empty())
        return false;

    if (direction < 0)
    {
        if (command_history_position == command_history.size())
            command_history_draft = input;
        if (command_history_position > 0)
            --command_history_position;
    }
    else if (command_history_position < command_history.size())
    {
        ++command_history_position;
    }
    else
    {
        return true;
    }

    input = command_history_position == command_history.size()
        ? command_history_draft
        : command_history[command_history_position];
    return true;
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

// /start        listens on the saved port
// /start <port> listens on that port for this run
bool ConsoleInterface::start(std::istringstream& args)
{
    std::string port_text;
    args >> port_text;

    if (port_text.empty())
    {
        interface.start();
        return true;
    }

    int port = 0;
    if (!parseInt(port_text, 1, 65535, port))
        return false;

    interface.start(port);
    return true;
}

bool ConsoleInterface::connect(std::istringstream& args)
{
    std::string first, second, extra;
    if (!(args >> first >> second) || (args >> extra)) return false;
    int port = 0;
    if (parseInt(second, 1, 65535, port)) interface.connect(first, port);
    else interface.connectRoom(first, second);
    return true;
}

bool ConsoleInterface::selectPeer(std::istringstream& args)
{
    std::string token;
    args >> token;
    if (token.empty())
        return false;

    PeerId peer;
    switch (resolvePeer(token, peer))
    {
    case PeerLookup::NotFound:
        printLineError("No connected peer matches '" + token + "' (see /showpeers)");
        return true;
    case PeerLookup::Ambiguous:
        printLineError("'" + token + "' matches several peers: type more of the id (see /showpeers)");
        return true;
    case PeerLookup::Found:
        break;
    }

    if (!interface.peerExists(peer))
    {
        printLineError("That peer is no longer connected");
        return true;
    }

    clearLog();
    setSelectedPeer(peer);
    printLineSuccess("Now chatting with " + peerName(peer));
    interface.selectPeer(peer);     // history and queued messages are delivered after this line
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
    log_start = 0;
    follow_log_tail = true;
    redrawLocked();
}

void ConsoleInterface::scrollLog(int amount)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    if (output.empty())
        return;

    const std::size_t max_start = output.size() > visible_log_count
        ? output.size() - visible_log_count
        : 0;

    if (amount < 0) {
        const std::size_t distance = static_cast<std::size_t>(-amount);
        log_start = log_start > distance ? log_start - distance : 0;
    } else {
        const std::size_t distance = static_cast<std::size_t>(amount);
        log_start = distance > max_start - std::min(log_start, max_start)
            ? max_start
            : std::min(log_start, max_start) + distance;
    }

    follow_log_tail = log_start >= max_start;
    redrawLocked();
}

// ---- thread-safe state access ----------------------------------------------

void ConsoleInterface::append(LogEntry entry)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    output.push_back(std::move(entry));
    if (follow_log_tail)
        log_start = output.size() > visible_log_count
            ? output.size() - visible_log_count
            : 0;
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

std::string ConsoleInterface::peerName(const PeerId& peer_id) const
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return peerLabel(status, peer_id);
}

// A peer is addressed by username (case-insensitive) or by the start of its id (4+ characters).
ConsoleInterface::PeerLookup ConsoleInterface::resolvePeer(const std::string& token, PeerId& out) const
{
    const std::string wanted = toLower(token);

    std::lock_guard<std::mutex> lock(state_mutex);

    std::vector<PeerId> matches;
    for (const auto& [id, name] : status.peers)
        if (toLower(name) == wanted)
            matches.push_back(id);

    if (matches.empty() && wanted.size() >= 4)
        for (const auto& entry : status.peers)
            if (entry.first.toHex().compare(0, wanted.size(), wanted) == 0)
                matches.push_back(entry.first);

    if (matches.empty())
        return PeerLookup::NotFound;
    if (matches.size() > 1)
        return PeerLookup::Ambiguous;

    out = matches.front();
    return PeerLookup::Found;
}

ConsoleSnapshot ConsoleInterface::snapshot(std::size_t terminal_height)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    // Reserve space for the single-line header, conversation panel chrome,
    // and message input panel. The remaining screen belongs to chat.
    visible_log_count = terminal_height > 10 ? terminal_height - 10 : 1;
    const std::size_t max_start = output.size() > visible_log_count
        ? output.size() - visible_log_count
        : 0;
    if (log_start > max_start)
        log_start = max_start;
    if (log_start >= max_start)
        follow_log_tail = true;
    return { output, status, log_start, visible_log_count };
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

    setMode(InputMode::Command);
    auto input_field = makeInput(&input, &placeholder,
        [this](const std::string& line) { handleInput(line); });

    input_field = CatchEvent(input_field, [this, &screen](Event event)
    {
        if (event == Event::CtrlP || event == Event::ArrowUpCtrl) {
            scrollLog(-1);
            return true;
        }
        if (event == Event::CtrlN || event == Event::ArrowDownCtrl) {
            scrollLog(1);
            return true;
        }
        if (event == Event::ArrowUp && navigateCommandHistory(-1))
            return true;
        if (event == Event::ArrowDown && navigateCommandHistory(1))
            return true;
        if (event == Event::PageUp) {
            scrollLog(-std::max(1, screen.dimy() - 10));
            return true;
        }
        if (event == Event::PageDown) {
            scrollLog(std::max(1, screen.dimy() - 10));
            return true;
        }
        return false;
    });

    auto page = Renderer(input_field, [&]
    {
        const auto current = snapshot(static_cast<std::size_t>(std::max(0, screen.dimy())));
        return renderPage(current, commandUsages(), mode, input_field->Render(),
            std::max(0, screen.dimx()), std::max(0, screen.dimy()));
    });

    setRedraw([&screen] { screen.PostEvent(Event::Custom); });
    ScopeExit unbind([this] { setRedraw(nullptr); });

    beginStartup();
    screen.Loop(page);
}

// ---- startup and first-run setup -------------------------------------------

// First launch: ask for a username and port. Later launches: listen with the saved settings.
void ConsoleInterface::beginStartup()
{
    refreshSelf();

    if (interface.needsSetup())
        beginSetup();
    else
        interface.start();
}

void ConsoleInterface::beginSetup()
{
    printLine("Welcome! Let's set up your profile (this happens only once).");
    printLine("Choose a username: letters, digits, '.', '_' or '-', up to 32 characters.");
    setMode(InputMode::SetupUsername);
}

void ConsoleInterface::handleSetupInput(const std::string& line)
{
    if (mode == InputMode::SetupUsername)
    {
        if (!isValidUsername(line))
        {
            printLineError("Invalid username. Use 1-32 letters, digits, '.', '_' or '-'.");
            return;
        }

        pending_username = line;
        printLine("Which port should we listen on? (1-65535, Enter for " + std::to_string(kDefaultPort) + ")");
        setMode(InputMode::SetupPort);
        return;
    }

    int port = kDefaultPort;
    if (!line.empty() && !parseInt(line, 1, 65535, port))
    {
        printLineError("Invalid port. Enter a number between 1 and 65535.");
        return;
    }

    try
    {
        interface.saveProfile(pending_username, port);
    }
    catch (const std::exception& e)
    {
        printLineError(std::string("Could not save settings: ") + e.what());
        return;
    }

    refreshSelf();
    setMode(InputMode::Command);
    printLineSuccess("Saved: " + pending_username + ", port " + std::to_string(port));
    interface.start();
}

void ConsoleInterface::setMode(InputMode new_mode)
{
    mode = new_mode;
    switch (mode)
    {
    case InputMode::SetupUsername: placeholder = "your username"; break;
    case InputMode::SetupPort:     placeholder = "port, e.g. 5000"; break;
    case InputMode::Command:       placeholder = "message or /command..."; break;
    }
}

// Copies our own id and username from the Interface into the sidebar.
void ConsoleInterface::refreshSelf()
{
    const PeerInfo self = interface.localPeer();
    updateStatus([&](ConsoleStatus& s) { s.self = self; });
}
