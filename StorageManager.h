#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>

struct sqlite3;

struct StoredMessage {
    std::int64_t id = 0;
    std::uint8_t peer_id = 0;
    bool outgoing = false;
    // Unix time in milliseconds (UTC).
    std::int64_t timestamp = 0;
    std::string content;
};

class StorageManager {
public:
    explicit StorageManager(const std::string& database_path,
                            std::size_t max_queued_messages = 1024);
    ~StorageManager();

    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;
    StorageManager(StorageManager&&) = delete;
    StorageManager& operator=(StorageManager&&) = delete;

    // Enqueues a message for the SQLite writer thread. Blocks when the bounded
    // queue is full, applying backpressure instead of dropping chat messages.
    // A successful return means queued, not yet durable on disk.
    void saveMessage(std::uint8_t peer_id, bool outgoing, const std::string& content);

    // Waits for all messages queued before this call to finish writing.
    // Throws std::runtime_error if the writer has encountered a SQLite error.
    void flush();

    // Flushes pending writes before loading the requested peer's history.
    std::vector<StoredMessage> getMessages(std::uint8_t peer_id);

private:
    struct QueuedMessage {
        std::uint64_t sequence;
        StoredMessage message;
    };

    sqlite3* db_ = nullptr;
    std::deque<QueuedMessage> queue_;
    const std::size_t max_queued_messages_;
    std::thread writer_thread_;

    // Protects the queue and all lifecycle/sequence/error state below.
    std::mutex queue_mutex_;
    std::condition_variable queue_not_empty_;
    std::condition_variable queue_not_full_;
    std::condition_variable writes_completed_;
    bool stopping_ = false;
    std::uint64_t last_enqueued_sequence_ = 0;
    std::uint64_t last_completed_sequence_ = 0;
    std::uint64_t first_failed_sequence_ = 0;
    std::exception_ptr first_writer_error_;

    // Serializes SQLite access between the writer and history-loading caller.
    std::mutex database_mutex_;

    void createTables();
    void writerLoop() noexcept;
    void writeMessage(const StoredMessage& message);
};
