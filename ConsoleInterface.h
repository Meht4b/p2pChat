#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "PeerId.h"

class Interface;

// What kind of line this is. The UI picks the styling from the level,
// so nothing ever has to inspect the text of a message.
enum class LogLevel
{
    Info,
    Success,
    Error,
    Detail,     // indented list items (command list, peer list)
    Command,    // echo of a command typed by the user
    Incoming,   // chat message from a peer
    Outgoing    // chat message sent by us
};

struct LogEntry
{
    LogLevel level;
    std::string text;
    std::string sender{};   // display name of the sender, only used by LogLevel::Incoming
};

// What the input line is currently used for.
enum class InputMode
{
    Command,            // chat text and /commands
    SetupUsername,      // first run: asking for a username
    SetupPort           // first run: asking for the listening port
};

// Connection state shown in the title bar and sidebar.
struct ConsoleStatus
{
    bool online = false;
    uint16_t port = 0;
    PeerInfo self;                                  // our own id and username
    std::optional<PeerId> selected_peer;            // set while chatting
    std::map<PeerId, std::string> peers;            // connected peers and their display names
};

// Consistent copy of everything the renderer needs for one frame.
struct ConsoleSnapshot
{
    std::vector<LogEntry> log;
    ConsoleStatus status;
};

class ConsoleInterface
{
public:
    explicit ConsoleInterface(Interface& interface);

    // Runs the UI loop; blocks until the user quits.
    // On the first launch it asks for a username and port, afterwards it starts listening
    // with the saved settings.
    void run();

    // Log output. Thread-safe, may be called from the network thread.
    void printLine(const std::string& msg);
    void printLineSuccess(const std::string& msg);
    void printLineError(const std::string& msg);
    void printLineIndent(const std::string& msg);
    void printMessage(const std::string& msg, const PeerId& sender);
    void printSentMessage(const std::string& msg);

    // Status updates. Thread-safe.
    void setOnline(uint16_t port, const PeerInfo& self);
    void setOffline();
    void onPeerConnected(const PeerInfo& peer);
    void onPeerDisconnected(const PeerId& peer_id);

private:
    // A slash command. The usage string drives help, the sidebar and usage errors.
    // The handler returns false when its arguments are invalid.
    struct Command
    {
        std::string name;
        std::string usage;
        std::function<bool(std::istringstream&)> handler;
    };

    enum class PeerLookup { Found, NotFound, Ambiguous };

    // startup and first-run setup (UI thread)
    void beginStartup();
    void beginSetup();
    void handleSetupInput(const std::string& line);
    void setMode(InputMode new_mode);
    void refreshSelf();

    // input handling (UI thread)
    void handleInput(const std::string& line);
    void runCommand(const std::string& command_line);
    const Command* findCommand(const std::string& name) const;
    std::vector<std::string> commandUsages() const;

    // commands
    bool start(std::istringstream& args);
    bool connect(std::istringstream& args);
    bool selectPeer(std::istringstream& args);
    bool leavePeer(std::istringstream& args);
    void showHelp();
    void clearLog();

    // state access
    void append(LogEntry entry);
    void updateStatus(const std::function<void(ConsoleStatus&)>& change);
    void setSelectedPeer(std::optional<PeerId> peer_id);
    bool hasSelectedPeer() const;
    std::string peerName(const PeerId& peer_id) const;
    PeerLookup resolvePeer(const std::string& token, PeerId& out) const;
    ConsoleSnapshot snapshot() const;
    void setRedraw(std::function<void()> redraw);
    void redrawLocked() const;      // caller must hold state_mutex

    Interface& interface;
    std::string input;
    std::string placeholder;                // input hint, depends on mode
    std::vector<Command> commands;

    // UI thread only
    InputMode mode = InputMode::Command;
    std::string pending_username;           // answer to the first setup question

    // guarded by state_mutex (shared with the network thread)
    mutable std::mutex state_mutex;
    std::vector<LogEntry> output;
    ConsoleStatus status;
    std::function<void()> request_redraw;
};
