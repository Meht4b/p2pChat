// Tests for identity, handshake, storage and the network layer (no UI needed).
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

#include "Message.h"
#include "CryptoIdentity.h"
#include "PeerId.h"
#include "PeerManager.h"
#include "StorageManager.h"
#include "TextSanitizer.h"

namespace {

int g_failed = 0;
int g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failed;                                                               \
            std::cout << "  FAILED " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
        }                                                                             \
    } while (0)

void section(const char* name) { std::cout << "- " << name << "\n"; }

// ---- identity ---------------------------------------------------------------

void testPeerId()
{
    section("PeerId");
    const PeerId a = PeerId::generate();
    const PeerId b = PeerId::generate();
    CHECK(!a.isZero());
    CHECK(a != b);
    CHECK(PeerId::kSize == 32);

    const std::string hex = a.toHex();
    CHECK(hex.size() == 64);
    CHECK(PeerId::fromHex(hex) && *PeerId::fromHex(hex) == a);
    CHECK(PeerId::fromHex("ABCDEF" + hex.substr(6)).has_value());          // upper case accepted
    CHECK(!PeerId::fromHex(hex.substr(1)));                                // wrong length
    CHECK(!PeerId::fromHex(std::string(64, 'g')));                         // not hex
    CHECK(a.shortHex() == hex.substr(0, 8));
    CHECK(PeerId{}.isZero());

    std::set<PeerId> unique;
    for (int i = 0; i < 1000; ++i) unique.insert(PeerId::generate());
    CHECK(unique.size() == 1000);
}

// ---- handshake --------------------------------------------------------------

void testHandshake()
{
    section("handshake format");
    const PeerInfo info{ PeerId::generate(), "alice" };

    const Message msg = makeHandshake(info);
    CHECK(msg.type == MessageType::Handshake);
    CHECK(msg.body.size() == PeerId::kSize + 5);                  // id + username, nothing else

    // survives the frame encoding too
    const Message decoded = deserialize(serialize(msg));
    PeerInfo parsed;
    CHECK(parseHandshake(decoded.body, parsed));
    CHECK(parsed.id == info.id);
    CHECK(parsed.username == "alice");

    // username length is simply body size - 32
    PeerInfo longest{ info.id, std::string(kMaxUsernameBytes, 'x') };
    CHECK(parseHandshake(makeHandshake(longest).body, parsed) && parsed.username.size() == kMaxUsernameBytes);

    bool threw = false;
    try { makeHandshake({ info.id, "" }); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { makeHandshake({ info.id, std::string(kMaxUsernameBytes + 1, 'x') }); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    // malformed bodies
    CHECK(!parseHandshake({}, parsed));
    CHECK(!parseHandshake(std::vector<char>{ 5 }, parsed));                          // old 1-byte format
    CHECK(!parseHandshake(std::vector<char>(PeerId::kSize, 'a'), parsed));           // id but no username
    CHECK(!parseHandshake(std::vector<char>(PeerId::kSize + kMaxUsernameBytes + 1, 'a'), parsed));
    std::vector<char> zero_id(PeerId::kSize + 3, 0);
    zero_id[PeerId::kSize] = 'b';
    CHECK(!parseHandshake(zero_id, parsed));                                         // all-zero id

    // untrusted usernames are cleaned
    auto withName = [&](const std::string& name) {
        std::vector<char> body(info.id.bytes().begin(), info.id.bytes().end());
        body.insert(body.end(), name.begin(), name.end());
        return body;
    };
    CHECK(parseHandshake(withName("bo\x1b[31mb"), parsed) && parsed.username == "bo[31mb");  // ESC removed
    CHECK(parseHandshake(withName("  Bob Smith "), parsed) && parsed.username == "Bob_Smith");
    CHECK(!parseHandshake(withName("   "), parsed));
    CHECK(!parseHandshake(withName("\x1b\x07"), parsed));                            // nothing left
}

void testSanitizer()
{
    section("text sanitizer / username validation");
    CHECK(isValidUsername("alice_01.x-y"));
    CHECK(!isValidUsername(""));
    CHECK(!isValidUsername("has space"));
    CHECK(!isValidUsername("slash/"));
    CHECK(!isValidUsername(std::string(33, 'a')));
    CHECK(isValidUsername(std::string(32, 'a')));

    CHECK(sanitizeText("plain") == "plain");
    CHECK(sanitizeText("a\x1b[2Jb") == "a[2Jb");
    CHECK(sanitizeText("t\tx") == "t x");
    CHECK(sanitizeText("h\xC3\xA9llo") == "h\xC3\xA9llo");          // valid UTF-8 kept
    CHECK(sanitizeText("bad\xFF" "x") == "bad?x");                    // invalid byte
    CHECK(sanitizeText("a\xC2\x9B" "b") == "ab");                     // C1 control (8-bit CSI)
    CHECK(sanitizeText("a\xE2\x80\xAE" "b") == "ab");                 // right-to-left override
}

// ---- storage ----------------------------------------------------------------

std::string tempDb(const char* name)
{
    const auto path = std::filesystem::temp_directory_path() / (std::string("p2p_test_") + name + ".db");
    std::filesystem::remove(path);
    return path.string();
}

void testStorage()
{
    section("storage: settings, known peers, history");
    const std::string path = tempDb("storage");
    const PeerId alice = PeerId::generate();
    const PeerId bob = PeerId::generate();

    {
        StorageManager storage(path, 10);
        CHECK(!storage.getSetting("username"));
        storage.setSetting("username", "me");
        storage.setSetting("username", "me2");              // overwrite
        CHECK(storage.getSetting("username") == "me2");

        CHECK(!storage.getKnownPeer(alice));
        storage.rememberPeer(alice, "alice");
        storage.rememberPeer(bob, "bob");

        storage.saveMessage(alice, false, "hi");
        storage.saveMessage(alice, true, "hello back");
        storage.saveMessage(bob, false, "other chat");
    }

    {   // everything is still there after reopening: this is what makes first-run config stick
        StorageManager storage(path, 10);
        CHECK(storage.getSetting("username") == "me2");

        const auto known = storage.getKnownPeer(alice);
        CHECK(known && known->username == "alice" && known->id == alice);
        const auto first_seen = known->first_seen;

        // a reconnect with another announced name keeps the saved one, and refreshes last_seen
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        storage.rememberPeer(alice, "mallory");
        const auto again = storage.getKnownPeer(alice);
        CHECK(again && again->username == "alice");
        CHECK(again->first_seen == first_seen && again->last_seen > first_seen);

        CHECK(storage.getKnownPeers().size() == 2);
        CHECK(storage.getKnownPeers().front().id == alice);     // most recently seen first

        const auto history = storage.getMessages(alice);
        CHECK(history.size() == 2);
        CHECK(history.size() == 2 && !history[0].outgoing && history[0].content == "hi");
        CHECK(history.size() == 2 && history[1].outgoing && history[1].peer_id == alice);
        CHECK(storage.getMessages(bob).size() == 1);
        CHECK(storage.getMessages(PeerId::generate()).empty());
    }
    std::filesystem::remove(path);
}

void testStorageMigration()
{
    section("storage: old 8-bit schema is preserved, not lost");
    const std::string path = tempDb("migrate");

    {   // what the previous version created
        sqlite3* db = nullptr;
        CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        const char* sql =
            "CREATE TABLE messages (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "peer_id INTEGER NOT NULL CHECK(peer_id BETWEEN 0 AND 255),"
            "outgoing INTEGER NOT NULL, timestamp INTEGER NOT NULL, content TEXT NOT NULL);"
            "INSERT INTO messages(peer_id, outgoing, timestamp, content) VALUES (7, 0, 1, 'legacy');";
        CHECK(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }

    {
        StorageManager storage(path, 10);
        const PeerId peer = PeerId::generate();
        storage.saveMessage(peer, true, "new schema works");
        CHECK(storage.getMessages(peer).size() == 1);
    }
    {
        StorageManager storage(path, 10);       // second open must not migrate again
        CHECK(!storage.getSetting("anything"));
    }

    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT content FROM messages_v1;", -1, &stmt, nullptr);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) == "legacy");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    std::filesystem::remove(path);
}

// ---- network ----------------------------------------------------------------

class Recorder : public PeerManagerCallbackHandler
{
public:
    std::mutex m;
    std::condition_variable cv;
    std::vector<PeerInfo> identified;
    std::vector<std::pair<PeerId, std::string>> received, sent;
    std::vector<std::pair<bool, PeerId>> session_errors;     // (identified, peer)
    int duplicates = 0;

    template <class Pred>
    bool waitFor(Pred pred, int ms = 3000)
    {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(ms), pred);
    }

    void onPeerManagerStarted(uint16_t, const PeerInfo&) override {}
    void onPeerManagerError(const asio::error_code&) override {}
    void onPeerOperationError(const asio::error_code&) override {}
    void onPeerConnection(bool, const asio::error_code&) override {}
    void onPeerIdentified(const PeerInfo& p) override { { std::lock_guard<std::mutex> l(m); identified.push_back(p); } cv.notify_all(); }
    void onDuplicatePeerConnection(const PeerId&, bool) override { { std::lock_guard<std::mutex> l(m); ++duplicates; } cv.notify_all(); }
    void onPeerDisconnected(const PeerId&) override {}
    void onPeerSessionError(bool id, const PeerId& p, const asio::error_code&) override { { std::lock_guard<std::mutex> l(m); session_errors.emplace_back(id, p); } cv.notify_all(); }
    void onPeerMessage(const PeerId& p, const std::string& s) override { { std::lock_guard<std::mutex> l(m); received.emplace_back(p, s); } cv.notify_all(); }
    void onLocalMessageSent(const PeerId& p, const std::string& s) override { { std::lock_guard<std::mutex> l(m); sent.emplace_back(p, s); } cv.notify_all(); }
};

uint16_t freePort()
{
    asio::io_context io;
    asio::ip::udp::socket socket(io, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    return socket.local_endpoint().port();
}

struct Node
{
    asio::io_context io;
    Recorder recorder;
    CryptoIdentity identity;
    PeerInfo self;
    uint16_t port;
    std::unique_ptr<PeerManager> manager;
    std::thread thread;

    Node(const std::string& name, bool mismatched_id = false) : identity(CryptoIdentity::generate()),
        self{ mismatched_id ? PeerId::generate() : identity.peerId(), name }, port(freePort())
    {
        manager = std::make_unique<PeerManager>(&io, self, identity, port, recorder);
        thread = std::thread([this] { io.run(); });
    }
    ~Node()
    {
        io.stop();
        thread.join();
        manager->stop();
    }
};

void testNetwork()
{
    section("network: QUIC over UDP, TLS 1.3 identity binding, reliable encrypted chat flow");
    Node alice("alice"), bob("bob");

    alice.manager->connect("127.0.0.1", bob.port);
    CHECK(alice.recorder.waitFor([&] { return !alice.recorder.identified.empty(); }));
    CHECK(bob.recorder.waitFor([&] { return !bob.recorder.identified.empty(); }));

    CHECK(alice.recorder.identified.size() == 1);
    CHECK(alice.recorder.identified[0].id == bob.self.id);
    CHECK(alice.recorder.identified[0].username == "bob");
    CHECK(alice.recorder.identified[0].id == bob.identity.peerId());
    CHECK(bob.recorder.identified[0].id == alice.self.id);
    CHECK(bob.recorder.identified[0].username == "alice");
    CHECK(bob.recorder.identified[0].id == alice.identity.peerId());
    CHECK(alice.manager->showPeers() == std::vector<PeerId>{ bob.self.id });

    alice.manager->selectPeer(bob.self.id);
    bob.manager->selectPeer(alice.self.id);
    CHECK(alice.manager->getSelectedPeer() == bob.self.id);
    alice.manager->write("hello bob");
    CHECK(bob.recorder.waitFor([&] { return !bob.recorder.received.empty(); }));
    CHECK(bob.recorder.received[0].first == alice.self.id);
    CHECK(bob.recorder.received[0].second == "hello bob");
    CHECK(alice.recorder.waitFor([&] { return !alice.recorder.sent.empty(); }));
    CHECK(alice.recorder.sent[0].first == bob.self.id);      // reports WHO it was sent to

    const std::string large_message(16 * 1024, 'z');
    alice.manager->write(large_message);
    CHECK(bob.recorder.waitFor([&] { return bob.recorder.received.size() == 2; }));
    CHECK(bob.recorder.received[1].second == large_message); // spans many UDP packets
    CHECK(alice.recorder.waitFor([&] { return alice.recorder.sent.size() == 2; }));

    bob.manager->write("reply from bob");
    CHECK(alice.recorder.waitFor([&] { return alice.recorder.received.size() == 1; }));
    CHECK(alice.recorder.received[0].second == "reply from bob");

    // leaving a chat buffers messages until the peer is selected again
    bob.manager->deselectPeer();
    CHECK(!bob.manager->getSelectedPeer().has_value());
    alice.manager->write("while you were away");
    CHECK(alice.recorder.waitFor([&] { return alice.recorder.sent.size() == 3; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(bob.recorder.received.size() == 2);
    bob.manager->selectPeer(alice.self.id);
    CHECK(bob.recorder.waitFor([&] { return bob.recorder.received.size() == 3; }));
    CHECK(bob.recorder.received[2].second == "while you were away");
}

void testCryptoIdentityPersistence()
{
    section("crypto identity: portable persistence and stable peer id");
    const std::string path = tempDb("crypto_identity");
    PeerId first;
    {
        StorageManager storage(path, 10);
        const CryptoIdentity identity = CryptoIdentity::loadOrCreate(storage);
        first = identity.peerId();
        CHECK(!first.isZero());
        CHECK(storage.getSetting("crypto.identity.v2").has_value());
    }
    {
        StorageManager storage(path, 10);
        const CryptoIdentity identity = CryptoIdentity::loadOrCreate(storage);
        CHECK(identity.peerId() == first);
    }
    std::filesystem::remove(path);
}

void sendRaw(uint16_t port, const std::vector<char>& bytes)
{
    asio::io_context io;
    asio::ip::udp::socket socket(io, asio::ip::udp::v4());
    socket.send_to(asio::buffer(bytes), { asio::ip::address_v4::loopback(), port });
}

void testNetworkRejectsBadHandshakes()
{
    section("network: non-QUIC UDP datagrams are ignored");
    Node node("server");

    sendRaw(node.port, { 7 });
    sendRaw(node.port, serialize({ MessageType::Handshake, { 7 } }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(node.manager->isRunning());
    CHECK(node.recorder.session_errors.empty());
    CHECK(node.recorder.identified.empty());
    CHECK(node.manager->showPeers().empty());
}

void testNetworkRejectsUnboundPeerId()
{
    section("network: rejects app identity not bound to QUIC TLS key");
    Node server("server");
    Node forged("forged", true);
    forged.manager->connect("127.0.0.1", server.port);
    CHECK(server.recorder.waitFor([&] { return !server.recorder.session_errors.empty(); }));
    CHECK(server.recorder.identified.empty());
    CHECK(server.manager->showPeers().empty());
}

} // namespace

int main()
{
    testPeerId();
    testHandshake();
    testSanitizer();
    testStorage();
    testStorageMigration();
    testCryptoIdentityPersistence();
    testNetwork();
    testNetworkRejectsBadHandshakes();
    testNetworkRejectsUnboundPeerId();

    std::cout << "\n" << (g_checks - g_failed) << "/" << g_checks << " checks passed\n";
    return g_failed == 0 ? 0 : 1;
}
