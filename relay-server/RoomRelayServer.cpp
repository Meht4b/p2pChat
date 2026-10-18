#include "RoomRelayServer.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <deque>
#include <stdexcept>
#include <utility>

namespace {

constexpr unsigned long kTurnCredentialLifetimeMs = 60ul * 60ul * 1000ul;
constexpr char kHex[] = "0123456789abcdef";

std::string randomHex(std::size_t bytes)
{
    std::vector<unsigned char> random(bytes);
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        throw std::runtime_error("Could not generate temporary TURN credentials");
    std::string result;
    result.reserve(bytes * 2);
    for (unsigned char value : random) {
        result.push_back(kHex[value >> 4]);
        result.push_back(kHex[value & 15]);
    }
    return result;
}

std::string roomKey(const room_protocol::RoomId& id)
{
    return std::string(reinterpret_cast<const char*>(id.data()), id.size());
}

std::uint32_t readLength(const std::array<std::uint8_t, 4>& header)
{
    return (static_cast<std::uint32_t>(header[0]) << 24) |
        (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) << 8) | header[3];
}

std::vector<std::uint8_t> makeWelcome(const room_protocol::TurnCredentials& credentials)
{
    if (credentials.username.size() > 255 || credentials.password.size() > 255)
        throw std::length_error("TURN credential is too long");
    std::vector<std::uint8_t> payload{
        static_cast<std::uint8_t>(credentials.port >> 8),
        static_cast<std::uint8_t>(credentials.port),
        static_cast<std::uint8_t>(credentials.username.size())
    };
    payload.insert(payload.end(), credentials.username.begin(), credentials.username.end());
    payload.push_back(static_cast<std::uint8_t>(credentials.password.size()));
    payload.insert(payload.end(), credentials.password.begin(), credentials.password.end());
    return payload;
}

} // namespace

class RoomRelayServer::Client : public std::enable_shared_from_this<RoomRelayServer::Client> {
public:
    Client(RoomRelayServer& owner, asio::io_context& io, asio::ssl::context& tls)
        : owner_(owner), stream_(io, tls) {}

    auto& socket() { return stream_.lowest_layer(); }

    void start()
    {
        auto self = shared_from_this();
        stream_.async_handshake(asio::ssl::stream_base::server,
            [self](const asio::error_code& error) {
                if (error) { self->close(); return; }
                self->readHeader();
            });
    }

    void assignRoom(std::string key, std::size_t slot)
    {
        room_key_ = std::move(key);
        room_slot_ = slot;
        joined_ = true;
    }

    const std::string& roomKey() const { return room_key_; }
    std::size_t roomSlot() const { return room_slot_; }

    void send(room_protocol::Type type, const std::vector<std::uint8_t>& payload)
    {
        if (closed_) return;
        try {
            writes_.push_back(room_protocol::encodeFrame(type, payload));
        } catch (...) {
            close();
            return;
        }
        if (writes_.size() == 1) writeNext();
    }

private:
    RoomRelayServer& owner_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    std::array<std::uint8_t, 4> header_{};
    std::vector<std::uint8_t> body_;
    std::deque<std::vector<std::uint8_t>> writes_;
    std::string room_key_;
    std::size_t room_slot_ = 0;
    bool joined_ = false;
    bool closed_ = false;

    void readHeader()
    {
        if (closed_) return;
        auto self = shared_from_this();
        asio::async_read(stream_, asio::buffer(header_), [self](const asio::error_code& error, std::size_t) {
            if (error) { self->close(); return; }
            const auto length = readLength(self->header_);
            if (length == 0 || length > room_protocol::kMaximumFrameBytes) {
                self->sendError("invalid frame length");
                return;
            }
            self->body_.assign(length, 0);
            self->readBody();
        });
    }

    void readBody()
    {
        auto self = shared_from_this();
        asio::async_read(stream_, asio::buffer(body_), [self](const asio::error_code& error, std::size_t) {
            if (error) { self->close(); return; }
            std::vector<std::uint8_t> encoded(self->header_.begin(), self->header_.end());
            encoded.insert(encoded.end(), self->body_.begin(), self->body_.end());
            room_protocol::Frame frame;
            if (!room_protocol::decodeFrame(encoded, frame)) {
                self->sendError("invalid frame");
                return;
            }
            self->handleFrame(frame);
        });
    }

    void handleFrame(const room_protocol::Frame& frame)
    {
        if (!joined_) {
            if (frame.type != room_protocol::Type::Join || frame.payload.size() != room_protocol::kRoomIdBytes) {
                sendError("expected room join");
                return;
            }
            room_protocol::RoomId id{};
            std::copy(frame.payload.begin(), frame.payload.end(), id.begin());
            std::size_t slot = 0;
            room_protocol::TurnCredentials credentials;
            std::vector<std::uint8_t> waiting_description;
            try {
                if (!owner_.join(shared_from_this(), id, slot, credentials, waiting_description)) {
                    sendError("room is full");
                    return;
                }
            } catch (...) {
                sendError("relay service could not allocate the room");
                return;
            }
            send(room_protocol::Type::Welcome, makeWelcome(credentials));
            if (!waiting_description.empty())
                send(room_protocol::Type::PeerDescription, waiting_description);
        } else if (frame.type == room_protocol::Type::Description && !frame.payload.empty()) {
            owner_.forwardDescription(shared_from_this(), frame.payload);
        } else {
            sendError("unexpected room message");
            return;
        }
        readHeader();
    }

    void sendError(const std::string& text)
    {
        send(room_protocol::Type::Error,
            std::vector<std::uint8_t>(text.begin(), text.end()));
        close_after_write_ = true;
    }

    void writeNext()
    {
        if (writes_.empty() || closed_) return;
        auto self = shared_from_this();
        asio::async_write(stream_, asio::buffer(writes_.front()),
            [self](const asio::error_code& error, std::size_t) {
                if (error) { self->close(); return; }
                self->writes_.pop_front();
                if (self->writes_.empty() && self->close_after_write_) {
                    self->close();
                    return;
                }
                if (!self->writes_.empty()) self->writeNext();
            });
    }

    void close()
    {
        if (closed_) return;
        closed_ = true;
        asio::error_code ignored;
        stream_.lowest_layer().cancel(ignored);
        stream_.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        stream_.lowest_layer().close(ignored);
        if (joined_) owner_.remove(this, room_key_, room_slot_);
    }

    bool close_after_write_ = false;
};

RoomRelayServer::RoomRelayServer(asio::io_context& io, Config config)
    : io_(io), config_(std::move(config)), acceptor_(io)
{
    if (config_.certificate_path.empty() || config_.private_key_path.empty())
        throw std::invalid_argument("TLS certificate and private key paths are required");
    tls_context_.set_options(asio::ssl::context::default_workarounds |
        asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3);
    if (SSL_CTX_set_min_proto_version(tls_context_.native_handle(), TLS1_3_VERSION) != 1)
        throw std::runtime_error("Could not require TLS 1.3 for rendezvous");
    tls_context_.use_certificate_chain_file(config_.certificate_path);
    tls_context_.use_private_key_file(config_.private_key_path, asio::ssl::context::pem);

    juice_server_config_t turn_config{};
    turn_config.bind_address = config_.bind_address.c_str();
    turn_config.external_address = config_.external_address.empty()
        ? nullptr : config_.external_address.c_str();
    turn_config.port = config_.turn_port;
    turn_config.relay_port_range_begin = config_.relay_port_begin;
    turn_config.relay_port_range_end = config_.relay_port_end;
    turn_config.max_allocations = 1024;
    turn_config.max_peers = 4;
    turn_config.realm = "p2pChat";
    turn_server_ = juice_server_create(&turn_config);
    if (!turn_server_ || juice_server_get_port(turn_server_) == 0)
        throw std::runtime_error("Could not start the STUN/TURN service");

    asio::error_code error;
    const auto address = asio::ip::make_address(config_.bind_address, error);
    if (error) throw asio::system_error(error);
    const asio::ip::tcp::endpoint endpoint(address, config_.signal_port);
    acceptor_.open(endpoint.protocol(), error);
    if (error) throw asio::system_error(error);
    acceptor_.set_option(asio::socket_base::reuse_address(true), error);
    if (error) throw asio::system_error(error);
    acceptor_.bind(endpoint, error);
    if (error) throw asio::system_error(error);
    acceptor_.listen(asio::socket_base::max_listen_connections, error);
    if (error) throw asio::system_error(error);
}

RoomRelayServer::~RoomRelayServer()
{
    asio::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
    if (turn_server_) juice_server_destroy(turn_server_);
}

void RoomRelayServer::start()
{
    acceptNext();
}

void RoomRelayServer::acceptNext()
{
    auto client = std::make_shared<Client>(*this, io_, tls_context_);
    acceptor_.async_accept(client->socket(), [this, client](const asio::error_code& error) {
        if (!error) client->start();
        if (acceptor_.is_open()) acceptNext();
    });
}

bool RoomRelayServer::join(const std::shared_ptr<Client>& client,
    const room_protocol::RoomId& id, std::size_t& slot,
    room_protocol::TurnCredentials& credentials,
    std::vector<std::uint8_t>& waiting_description)
{
    const std::string key = roomKey(id);
    const auto existing = rooms_.find(key);
    if (existing != rooms_.end() && !existing->second.clients[0].expired() &&
        !existing->second.clients[1].expired()) return false;
    slot = existing == rooms_.end() || existing->second.clients[0].expired() ? 0 : 1;

    credentials.username = randomHex(16);
    credentials.password = randomHex(24);
    credentials.port = juice_server_get_port(turn_server_);
    juice_server_credentials_t turn_credentials{};
    turn_credentials.username = credentials.username.c_str();
    turn_credentials.password = credentials.password.c_str();
    turn_credentials.allocations_quota = 1;
    if (juice_server_add_credentials(turn_server_, &turn_credentials,
            kTurnCredentialLifetimeMs) != 0)
        throw std::runtime_error("Could not register temporary TURN credentials");

    Room& room = rooms_[key];
    client->assignRoom(key, slot);
    room.clients[slot] = client;
    const std::size_t other = 1 - slot;
    if (!room.clients[other].expired())
        waiting_description = room.descriptions[other];
    return true;
}

void RoomRelayServer::forwardDescription(const std::shared_ptr<Client>& client,
    const std::vector<std::uint8_t>& encrypted_description)
{
    const auto found = rooms_.find(client->roomKey());
    if (found == rooms_.end()) return;
    Room& room = found->second;
    const std::size_t slot = client->roomSlot();
    const auto current = room.clients[slot].lock();
    if (current != client) return;
    room.descriptions[slot] = encrypted_description;
    const auto other = room.clients[1 - slot].lock();
    if (other)
        other->send(room_protocol::Type::PeerDescription, encrypted_description);
}

void RoomRelayServer::remove(const Client* client, const std::string& key, std::size_t slot)
{
    const auto found = rooms_.find(key);
    if (found == rooms_.end()) return;
    Room& room = found->second;
    const auto current = room.clients[slot].lock();
    if (!current || current.get() == client) {
        room.clients[slot].reset();
        room.descriptions[slot].clear();
    }
    if (room.clients[0].expired() && room.clients[1].expired()) rooms_.erase(found);
}
