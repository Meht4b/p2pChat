#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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
    uint8_t peer_id = 0;    // sender, only used by LogLevel::Incoming
};

// Connection state shown in the title bar and sidebar.
struct ConsoleStatus
{
    bool online = false;
    uint16_t port = 0;
    uint8_t user_id = 0;
    std::optional<uint8_t> selected_peer;   // set while chatting
    std::set<uint8_t> peers;                // identified, connected peers
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
    void run();

    // Log output. Thread-safe, may be called from the network thread.
    void printLine(const std::string& msg);
    void printLineSuccess(const std::string& msg);
    void printLineError(const std::string& msg);
    void printLineIndent(const std::string& msg);
    void printMessage(const std::string& msg, int user_id);
    void printSentMessage(const std::string& msg);

    // Status updates. Thread-safe.
    void setOnline(uint16_t port, uint8_t user_id);
    void setOffline();
    void onPeerConnected(uint8_t peer_id);
    void onPeerDisconnected(uint8_t peer_id);

private:
    // A slash command. The usage string drives help, the sidebar and usage errors.
    // The handler returns false when its arguments are invalid.
    struct Command
    {
        std::string name;
        std::string usage;
        std::function<bool(std::istringstream&)> handler;
    };

    // input handling
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
    void setSelectedPeer(std::optional<uint8_t> peer_id);
    bool hasSelectedPeer() const;
    ConsoleSnapshot snapshot() const;
    void setRedraw(std::function<void()> redraw);
    void redrawLocked() const;      // caller must hold state_mutex

    Interface& interface;
    std::string input;
    std::vector<Command> commands;

    // guarded by state_mutex (shared with the network thread)
    mutable std::mutex state_mutex;
    std::vector<LogEntry> output;
    ConsoleStatus status;
    std::function<void()> request_redraw;
};
