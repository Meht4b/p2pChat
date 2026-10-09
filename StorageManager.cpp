#include "StorageManager.h"

#include <sqlite3.h>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

[[noreturn]] void throwSqliteError(sqlite3* db, const char* operation, int result)
{
    const char* detail = db ? sqlite3_errmsg(db) : sqlite3_errstr(result);
    throw std::runtime_error(std::string(operation) + ": " + detail);
}

void checkSqlite(sqlite3* db, const char* operation, int result)
{
    if (result != SQLITE_OK) {
        throwSqliteError(db, operation, result);
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db)
    {
        const int result = sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
            throwSqliteError(db_, "Preparing SQLite statement", result);
        }
    }

    ~Statement()
    {
        if (statement_) {
            sqlite3_finalize(statement_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

std::int64_t currentUnixMilliseconds()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

} // namespace

StorageManager::StorageManager(const std::string& database_path,
                               std::size_t max_queued_messages)
    : max_queued_messages_(max_queued_messages)
{
    if (max_queued_messages_ == 0) {
        throw std::invalid_argument("Storage queue capacity must be greater than zero");
    }

    const int result = sqlite3_open_v2(
        database_path.c_str(),
        &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);

    if (result != SQLITE_OK) {
        const std::string detail = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(result);
        if (db_) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("Opening SQLite database: " + detail);
    }

    try {
        checkSqlite(db_, "Setting SQLite busy timeout", sqlite3_busy_timeout(db_, 5000));
        createTables();
        writer_thread_ = std::thread(&StorageManager::writerLoop, this);
    } catch (...) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
        throw;
    }
}

StorageManager::~StorageManager()
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stopping_ = true;
    }
    queue_not_empty_.notify_all();
    queue_not_full_.notify_all();

    if (writer_thread_.joinable()) {
        writer_thread_.join(); // The worker drains queued messages before exiting.
    }

    if (db_) {
        sqlite3_close_v2(db_);
    }
}

void StorageManager::createTables()
{
    static constexpr char sql[] =
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "peer_id INTEGER NOT NULL CHECK(peer_id BETWEEN 0 AND 255),"
        "outgoing INTEGER NOT NULL CHECK(outgoing IN (0, 1)),"
        "timestamp INTEGER NOT NULL,"
        "content TEXT NOT NULL"
        ");";

    char* error_message = nullptr;
    const int result = sqlite3_exec(db_, sql, nullptr, nullptr, &error_message);
    if (result != SQLITE_OK) {
        const std::string detail = error_message ? error_message : sqlite3_errmsg(db_);
        sqlite3_free(error_message);
        throw std::runtime_error("Creating messages table: " + detail);
    }
}

void StorageManager::saveMessage(std::uint8_t peer_id, bool outgoing, const std::string& content)
{
    StoredMessage message;
    message.peer_id = peer_id;
    message.outgoing = outgoing;
    message.timestamp = currentUnixMilliseconds();
    message.content = content;

    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_not_full_.wait(lock, [this] {
        return queue_.size() < max_queued_messages_ || stopping_;
    });

    if (stopping_) {
        throw std::runtime_error("StorageManager is shutting down; message was not queued");
    }
    if (last_enqueued_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Storage message sequence number exhausted");
    }

    const std::uint64_t sequence = last_enqueued_sequence_ + 1;
    queue_.push_back(QueuedMessage{sequence, std::move(message)});
    // Advance only after push succeeds; otherwise a failed allocation would
    // leave a sequence gap and make flush wait forever for a message not queued.
    last_enqueued_sequence_ = sequence;
    lock.unlock();
    queue_not_empty_.notify_one();
}

void StorageManager::flush()
{
    std::unique_lock<std::mutex> lock(queue_mutex_);
    const std::uint64_t target_sequence = last_enqueued_sequence_;
    writes_completed_.wait(lock, [this, target_sequence] {
        return last_completed_sequence_ >= target_sequence;
    });

    if (first_writer_error_ && first_failed_sequence_ <= target_sequence) {
        const std::exception_ptr error = first_writer_error_;
        lock.unlock();
        std::rethrow_exception(error);
    }
}

std::vector<StoredMessage> StorageManager::getMessages(std::uint8_t peer_id)
{
    flush();

    static constexpr char sql[] =
        "SELECT id, peer_id, outgoing, timestamp, content "
        "FROM messages WHERE peer_id = ? ORDER BY timestamp ASC, id ASC;";

    std::lock_guard<std::mutex> database_lock(database_mutex_);
    Statement statement(db_, sql);
    sqlite3_stmt* stmt = statement.get();
    checkSqlite(db_, "Binding peer id", sqlite3_bind_int(stmt, 1, peer_id));

    std::vector<StoredMessage> messages;
    int result = SQLITE_OK;
    while ((result = sqlite3_step(stmt)) == SQLITE_ROW) {
        StoredMessage message;
        message.id = sqlite3_column_int64(stmt, 0);
        message.peer_id = static_cast<std::uint8_t>(sqlite3_column_int(stmt, 1));
        message.outgoing = sqlite3_column_int(stmt, 2) != 0;
        message.timestamp = sqlite3_column_int64(stmt, 3);

        const auto* content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const int content_size = sqlite3_column_bytes(stmt, 4);
        if (content && content_size > 0) {
            message.content.assign(content, static_cast<std::size_t>(content_size));
        }
        messages.push_back(std::move(message));
    }

    if (result != SQLITE_DONE) {
        throwSqliteError(db_, "Loading messages", result);
    }
    return messages;
}

void StorageManager::writerLoop() noexcept
{
    for (;;) {
        QueuedMessage queued;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_not_empty_.wait(lock, [this] {
                return !queue_.empty() || stopping_;
            });

            if (queue_.empty() && stopping_) {
                return;
            }

            queued = std::move(queue_.front());
            queue_.pop_front();
        }
        queue_not_full_.notify_one();

        std::exception_ptr write_error;
        try {
            writeMessage(queued.message);
        } catch (...) {
            write_error = std::current_exception();
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (write_error && !first_writer_error_) {
                first_failed_sequence_ = queued.sequence;
                first_writer_error_ = write_error;
            }
            // FIFO processing guarantees this advances contiguously, including
            // failed writes, so flush can wait for a precise enqueue snapshot.
            last_completed_sequence_ = queued.sequence;
        }
        writes_completed_.notify_all();
    }
}

void StorageManager::writeMessage(const StoredMessage& message)
{
    static constexpr char sql[] =
        "INSERT INTO messages (peer_id, outgoing, timestamp, content) "
        "VALUES (?, ?, ?, ?);";
    if (message.content.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("Message content is too large for SQLite");
    }

    std::lock_guard<std::mutex> database_lock(database_mutex_);
    Statement statement(db_, sql);
    sqlite3_stmt* stmt = statement.get();

    checkSqlite(db_, "Binding peer id", sqlite3_bind_int(stmt, 1, message.peer_id));
    checkSqlite(db_, "Binding direction", sqlite3_bind_int(stmt, 2, message.outgoing ? 1 : 0));
    checkSqlite(db_, "Binding timestamp", sqlite3_bind_int64(stmt, 3, message.timestamp));
    checkSqlite(db_, "Binding message content",
                sqlite3_bind_text(stmt, 4, message.content.data(),
                                  static_cast<int>(message.content.size()), SQLITE_TRANSIENT));

    const int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throwSqliteError(db_, "Saving message", result);
    }
}
