#include "ConsoleInterface.h"

#include "PeerManager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

class ConsoleInterface::Impl
{
public:
    explicit Impl(PeerManager& peerManager)
        : peerManager_(peerManager)
        , input_(::GetStdHandle(STD_INPUT_HANDLE))
        , output_(::GetStdHandle(STD_OUTPUT_HANDLE))
    {
    }

    ~Impl()
    {
        stop();
        if (inputThread_.joinable())
            inputThread_.join();
    }

    void run()
    {
        enableVirtualTerminalOutput();
        postLine("Type 'help' for commands.");
        inputThread_ = std::thread([this] { readKeys(); });

        for (;;)
        {
            std::unique_lock lock(mutex_);
            redrawSignal_.wait(lock, [this] { return redrawRequested_ || stopped_; });

            if (stopped_)
                break;

            redrawRequested_ = false;
            lock.unlock();
            render();
        }
    }

    void stop()
    {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
            redrawRequested_ = true;
        }
        redrawSignal_.notify_one();

        // Wakes ReadConsoleInput if the caller is trying to shut down.
        if (inputThread_.joinable())
            ::CancelSynchronousIo(
                reinterpret_cast<HANDLE>(inputThread_.native_handle()));
    }

    void postLine(std::string line)
    {
        {
            std::lock_guard lock(mutex_);
            lines_.push_back(std::move(line));
            if (lines_.size() > maximumLines)
                lines_.pop_front();
            redrawRequested_ = true;
        }
        redrawSignal_.notify_one();
    }

private:
    static constexpr std::size_t maximumLines = 200;

    void enableVirtualTerminalOutput()
    {
        DWORD mode = 0;
        if (::GetConsoleMode(output_, &mode))
            ::SetConsoleMode(output_, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    void readKeys()
    {
        while (!isStopped())
        {
            INPUT_RECORD record{};
            DWORD count = 0;
            if (!::ReadConsoleInput(input_, &record, 1, &count))
                return;

            if (count == 1 && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown)
                handleKey(record.Event.KeyEvent);
        }
    }

    bool isStopped()
    {
        std::lock_guard lock(mutex_);
        return stopped_;
    }

    void handleKey(const KEY_EVENT_RECORD& key)
    {
        if (key.wVirtualKeyCode == VK_ESCAPE)
        {
            std::lock_guard lock(mutex_);
            if (selectedPeer_)
            {
                lines_.push_back("Returned to command mode.");
                selectedPeer_.reset();
            }
            redrawRequested_ = true;
            redrawSignal_.notify_one();
            return;
        }

        if (key.wVirtualKeyCode == VK_BACK)
        {
            std::lock_guard lock(mutex_);
            if (!inputBuffer_.empty())
                inputBuffer_.pop_back();
            requestRedrawLocked();
            return;
        }

        if (key.wVirtualKeyCode == VK_RETURN)
        {
            std::string submitted;
            std::optional<std::uint8_t> peer;
            {
                std::lock_guard lock(mutex_);
                submitted = std::move(inputBuffer_);
                inputBuffer_.clear();
                peer = selectedPeer_;
                requestRedrawLocked();
            }

            if (!submitted.empty())
            {
                if (peer)
                    postLine("Messaging is not implemented yet (peer " + std::to_string(*peer) + ").");
                else
                    executeCommand(submitted);
            }
            return;
        }

        // This first version deliberately supports simple printable ASCII input.
        const char character = key.uChar.AsciiChar;
        if (character >= ' ' && character <= '~')
        {
            std::lock_guard lock(mutex_);
            inputBuffer_.push_back(character);
            requestRedrawLocked();
        }
    }

    void requestRedrawLocked()
    {
        redrawRequested_ = true;
        redrawSignal_.notify_one();
    }

    void executeCommand(const std::string& commandLine)
    {
        std::istringstream words(commandLine);
        std::string command;
        words >> command;

        if (command == "help")
        {
            postLine("help");
            postLine("clear");
            postLine("connect <address> <port>");
            postLine("showpeers");
            postLine("selectpeer <peer id>");
            postLine("Escape returns from a selected peer to command mode.");
            return;
        }

        if (command == "clear")
        {
            std::lock_guard lock(mutex_);
            lines_.clear();
            requestRedrawLocked();
            return;
        }

        if (command == "connect")
        {
            std::string address;
            unsigned int port = 0;
            std::string extra;
            if (!(words >> address >> port) || port == 0 || port > 65535 || (words >> extra))
            {
                postLine("Usage: connect <address> <port>");
                return;
            }

            peerManager_.connect(address, static_cast<unsigned short>(port));
            postLine("Connecting to " + address + ":" + std::to_string(port) + "...");
            return;
        }

        if (command == "showpeers")
        {
            const auto peers = peerManager_.peers();
            if (peers.empty())
            {
                postLine("No identified peers.");
                return;
            }

            for (const auto& peer : peers)
                postLine("Peer " + std::to_string(peer.id) + "  " + peer.endpoint);
            return;
        }

        if (command == "selectpeer")
        {
            unsigned int rawId = 0;
            std::string extra;
            if (!(words >> rawId) || rawId > 255 || (words >> extra))
            {
                postLine("Usage: selectpeer <peer id>");
                return;
            }

            const auto id = static_cast<std::uint8_t>(rawId);
            if (!peerManager_.hasPeer(id))
            {
                postLine("Peer " + std::to_string(id) + " is not connected.");
                return;
            }

            {
                std::lock_guard lock(mutex_);
                selectedPeer_ = id;
                requestRedrawLocked();
            }
            postLine("Selected peer " + std::to_string(id) + ". Press Escape to return.");
            return;
        }

        postLine("Unknown command: " + command + ". Type 'help'.");
    }

    void render()
    {
        std::deque<std::string> lines;
        std::string input;
        std::optional<std::uint8_t> peer;
        {
            std::lock_guard lock(mutex_);
            lines = lines_;
            input = inputBuffer_;
            peer = selectedPeer_;
        }

        CONSOLE_SCREEN_BUFFER_INFO info{};
        ::GetConsoleScreenBufferInfo(output_, &info);
        const int rowsForLog = std::max(1, static_cast<int>(info.dwSize.Y) - 1);
        const std::size_t first = lines.size() > static_cast<std::size_t>(rowsForLog)
            ? lines.size() - static_cast<std::size_t>(rowsForLog)
            : 0;

        // Only this thread writes to stdout.  Thus an ASIO handler can call
        // postLine() at any time without corrupting the prompt being typed.
        std::cout << "\x1b[?25l\x1b[H\x1b[2J";
        const std::size_t visibleLineCount = lines.size() - first;
        for (std::size_t i = first; i < lines.size(); ++i)
            std::cout << lines[i] << "\r\n";
        for (std::size_t i = visibleLineCount; i < static_cast<std::size_t>(rowsForLog); ++i)
            std::cout << "\r\n";

        const std::string prompt = peer
            ? "[" + std::to_string(*peer) + "] : "
            : ">> ";
        std::cout << prompt << input << "\x1b[?25h" << std::flush;
    }

    PeerManager& peerManager_;
    HANDLE input_;
    HANDLE output_;
    std::mutex mutex_;
    std::condition_variable redrawSignal_;
    std::thread inputThread_;
    std::deque<std::string> lines_;
    std::string inputBuffer_;
    std::optional<std::uint8_t> selectedPeer_;
    bool redrawRequested_ = false;
    bool stopped_ = false;
};

ConsoleInterface::ConsoleInterface(PeerManager& peerManager)
    : impl_(std::make_unique<Impl>(peerManager))
{
}

ConsoleInterface::~ConsoleInterface() = default;

void ConsoleInterface::run()
{
    impl_->run();
}

void ConsoleInterface::stop()
{
    impl_->stop();
}

void ConsoleInterface::postLine(std::string line)
{
    impl_->postLine(std::move(line));
}
