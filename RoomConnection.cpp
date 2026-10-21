#include "RoomConnection.h"

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

std::uint32_t readLength(const std::array<std::uint8_t, 4>& header)
{
    return (static_cast<std::uint32_t>(header[0]) << 24) |
        (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) << 8) | header[3];
}

std::optional<std::string> iceUfrag(const std::string& description)
{
    constexpr char prefix[] = "a=ice-ufrag:";
    std::size_t start = 0;
    while (start < description.size()) {
        const auto end = description.find('\n', start);
        const auto length = (end == std::string::npos ? description.size() : end) - start;
        if (length > sizeof(prefix) - 1 &&
            description.compare(start, sizeof(prefix) - 1, prefix) == 0) {
            std::string value = description.substr(start + sizeof(prefix) - 1,
                length - (sizeof(prefix) - 1));
            while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
                value.pop_back();
            if (!value.empty()) return value;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

RoomConnection::UdpEndpoint parseAddress(const char* text)
{
    const std::string input(text);
    const auto separator = input.rfind(':');
    if (separator == std::string::npos)
        throw std::runtime_error("ICE selected address has no port");
    std::string host = input.substr(0, separator);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    unsigned port = 0;
    const std::string port_text = input.substr(separator + 1);
    const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
        port == 0 || port > 65535)
        throw std::runtime_error("ICE selected address has an invalid port");
    asio::error_code error;
    const auto address = asio::ip::make_address(host, error);
    if (error) throw std::runtime_error("ICE selected address is not numeric");
    return {address, static_cast<std::uint16_t>(port)};
}

std::optional<std::string> environmentValue(const char* name)
{
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (!value) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

} // namespace

RoomConnection::RoomConnection(asio::io_context& io, std::string relay_host,
    std::uint16_t signal_port, std::string room_token, ReadyHandler ready,
    PacketHandler packet, ErrorHandler error)
    : io_(io), relay_host_(std::move(relay_host)),
      signal_port_text_(std::to_string(signal_port)), room_token_(std::move(room_token)),
      ready_handler_(std::move(ready)), packet_handler_(std::move(packet)),
      error_handler_(std::move(error)),
      tls_context_(std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client)),
      resolver_(io), stream_(io, *tls_context_), setup_timeout_(io)
{
    room_id_ = room_protocol::roomIdForToken(room_token_);
    tls_context_->set_default_verify_paths();
    if (const auto ca_file = environmentValue("P2P_RENDEZVOUS_CA_FILE"))
        tls_context_->load_verify_file(*ca_file);
    stream_.set_verify_mode(asio::ssl::verify_peer);

    asio::error_code address_error;
    (void)asio::ip::make_address(relay_host_, address_error);
    X509_VERIFY_PARAM* verify = SSL_get0_param(stream_.native_handle());
    const int verify_result = address_error
        ? X509_VERIFY_PARAM_set1_host(verify, relay_host_.c_str(), relay_host_.size())
        : X509_VERIFY_PARAM_set1_ip_asc(verify, relay_host_.c_str());
    if (verify_result != 1)
        throw std::runtime_error("Could not configure relay server certificate verification");
    if (address_error && SSL_set_tlsext_host_name(stream_.native_handle(), relay_host_.c_str()) != 1)
        throw std::runtime_error("Could not configure relay server TLS name");
}

RoomConnection::RoomConnection(asio::io_context& io, std::string relay_host,
    std::uint16_t signal_port, std::string room_token, LegacyReadyHandler ready,
    PacketHandler packet, ErrorHandler error)
    : RoomConnection(io, std::move(relay_host), signal_port, std::move(room_token),
        ReadyHandler([ready = std::move(ready)](std::shared_ptr<RoomConnection> room,
            UdpEndpoint local, UdpEndpoint remote, bool quic_client, ConnectionPath) {
            if (ready) ready(std::move(room), std::move(local), std::move(remote), quic_client);
        }), std::move(packet), std::move(error))
{}

RoomConnection::~RoomConnection()
{
    close();
}

void RoomConnection::start()
{
    setup_timeout_.expires_after(std::chrono::seconds(90));
    auto weak = weak_from_this();
    setup_timeout_.async_wait([weak](const asio::error_code& error) {
        if (!error) {
            if (auto self = weak.lock())
                self->reportError("Room setup timed out; check the room token and relay server");
        }
    });
    resolveAndConnect();
}

void RoomConnection::resolveAndConnect()
{
    auto self = shared_from_this();
    resolver_.async_resolve(relay_host_, signal_port_text_,
        [self](const asio::error_code& error, asio::ip::tcp::resolver::results_type results) {
            if (self->closed_) return;
            if (error) { self->reportError("Resolving rendezvous server failed: " + error.message()); return; }
            asio::async_connect(self->stream_.lowest_layer(), results,
                [self](const asio::error_code& connect_error, const asio::ip::tcp::endpoint&) {
                    if (self->closed_) return;
                    if (connect_error) { self->reportError("Connecting to rendezvous server failed: " + connect_error.message()); return; }
                    self->stream_.async_handshake(asio::ssl::stream_base::client,
                        [self](const asio::error_code& tls_error) {
                            if (self->closed_) return;
                            if (tls_error) { self->reportError("Rendezvous TLS verification failed: " + tls_error.message()); return; }
                            std::vector<std::uint8_t> payload(self->room_id_.begin(), self->room_id_.end());
                            self->enqueueFrame(room_protocol::Type::Join, payload);
                            self->readHeader();
                        });
                });
        });
}

void RoomConnection::readHeader()
{
    if (closed_) return;
    auto self = shared_from_this();
    asio::async_read(stream_, asio::buffer(read_header_),
        [self](const asio::error_code& error, std::size_t) {
            if (self->closed_) return;
            if (error) { self->reportError("Rendezvous connection closed: " + error.message()); return; }
            const auto size = readLength(self->read_header_);
            if (size == 0 || size > room_protocol::kMaximumFrameBytes) {
                self->reportError("Rendezvous sent an invalid frame length");
                return;
            }
            self->read_body_.assign(size, 0);
            self->readBody();
        });
}

void RoomConnection::readBody()
{
    auto self = shared_from_this();
    asio::async_read(stream_, asio::buffer(read_body_),
        [self](const asio::error_code& error, std::size_t) {
            if (self->closed_) return;
            if (error) { self->reportError("Reading rendezvous response failed: " + error.message()); return; }
            std::vector<std::uint8_t> encoded(self->read_header_.begin(), self->read_header_.end());
            encoded.insert(encoded.end(), self->read_body_.begin(), self->read_body_.end());
            self->handleFrame(encoded);
            if (!self->closed_) self->readHeader();
        });
}

void RoomConnection::handleFrame(const std::vector<std::uint8_t>& encoded)
{
    room_protocol::Frame frame;
    if (!room_protocol::decodeFrame(encoded, frame)) {
        reportError("Rendezvous frame is malformed");
        return;
    }
    if (frame.type == room_protocol::Type::Error) {
        reportError("Rendezvous rejected the connection: " +
            std::string(frame.payload.begin(), frame.payload.end()));
    } else if (frame.type == room_protocol::Type::Welcome) {
        handleWelcome(frame.payload);
    } else if (frame.type == room_protocol::Type::PeerDescription) {
        onRemoteDescription(frame.payload);
    } else {
        reportError("Unexpected rendezvous response");
    }
}

void RoomConnection::handleWelcome(const std::vector<std::uint8_t>& payload)
{
    if (agent_ || payload.size() < 4) {
        reportError("Rendezvous welcome is malformed");
        return;
    }
    const auto turn_port = static_cast<std::uint16_t>(
        (static_cast<unsigned>(payload[0]) << 8) | payload[1]);
    std::size_t position = 2;
    const std::size_t username_size = payload[position++];
    if (position + username_size + 1 > payload.size()) {
        reportError("Rendezvous TURN credentials are malformed");
        return;
    }
    turn_username_.assign(payload.begin() + static_cast<std::ptrdiff_t>(position),
        payload.begin() + static_cast<std::ptrdiff_t>(position + username_size));
    position += username_size;
    const std::size_t password_size = payload[position++];
    if (turn_port == 0 || turn_username_.empty() || password_size == 0 ||
        position + password_size != payload.size()) {
        reportError("Rendezvous TURN credentials are incomplete");
        return;
    }
    turn_password_.assign(payload.begin() + static_cast<std::ptrdiff_t>(position), payload.end());
    createIceAgent(turn_port);
}

void RoomConnection::createIceAgent(std::uint16_t turn_port)
{
    juice_turn_server_t turn{};
    turn.host = relay_host_.c_str();
    turn.port = turn_port;
    turn.username = turn_username_.c_str();
    turn.password = turn_password_.c_str();

    juice_config_t config{};
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
    config.stun_server_host = relay_host_.c_str();
    config.stun_server_port = turn_port;
    config.turn_servers = &turn;
    config.turn_servers_count = 1;
    config.cb_state_changed = &RoomConnection::stateChanged;
    config.cb_gathering_done = &RoomConnection::gatheringDone;
    config.cb_recv = &RoomConnection::received;
    config.user_ptr = this;
    agent_ = juice_create(&config);
    if (!agent_) {
        reportError("Creating ICE agent failed");
        return;
    }
    if (juice_gather_candidates(agent_) != JUICE_ERR_SUCCESS)
        reportError("Gathering ICE candidates failed");
}

void RoomConnection::enqueueFrame(room_protocol::Type type,
    const std::vector<std::uint8_t>& payload)
{
    if (closed_) return;
    try {
        write_queue_.push_back(room_protocol::encodeFrame(type, payload));
    } catch (const std::exception& error) {
        reportError(error.what());
        return;
    }
    if (write_queue_.size() == 1) writeNext();
}

void RoomConnection::writeNext()
{
    if (closed_ || write_queue_.empty()) return;
    auto self = shared_from_this();
    asio::async_write(stream_, asio::buffer(write_queue_.front()),
        [self](const asio::error_code& error, std::size_t) {
            if (self->closed_) return;
            if (error) { self->reportError("Writing rendezvous data failed: " + error.message()); return; }
            self->write_queue_.pop_front();
            if (!self->write_queue_.empty()) self->writeNext();
        });
}

void RoomConnection::onLocalGatheringDone()
{
    if (closed_ || local_description_sent_ || !agent_) return;
    std::array<char, JUICE_MAX_SDP_STRING_LEN> buffer{};
    if (juice_get_local_description(agent_, buffer.data(), buffer.size()) != JUICE_ERR_SUCCESS) {
        reportError("Reading local ICE description failed");
        return;
    }
    local_description_ = buffer.data();
    try {
        const auto encrypted = room_protocol::encryptDescription(room_token_, room_id_, local_description_);
        enqueueFrame(room_protocol::Type::Description, encrypted);
        local_description_sent_ = true;
        maybeReportReady();
    } catch (const std::exception& error) {
        reportError(std::string("Protecting ICE description failed: ") + error.what());
    }
}

void RoomConnection::onRemoteDescription(const std::vector<std::uint8_t>& encrypted)
{
    if (closed_ || remote_description_received_ || !agent_) {
        reportError("Unexpected ICE description from rendezvous");
        return;
    }
    if (!room_protocol::decryptDescription(room_token_, room_id_, encrypted, remote_description_)) {
        reportError("ICE description authentication failed; room token may be wrong");
        return;
    }
    if (juice_set_remote_description(agent_, remote_description_.c_str()) != JUICE_ERR_SUCCESS) {
        reportError("Applying remote ICE description failed");
        return;
    }
    remote_description_received_ = true;
    maybeReportReady();
}

void RoomConnection::onIceState(juice_state_t state)
{
    if (closed_) return;
    if (state == JUICE_STATE_FAILED) {
        reportError("ICE could not find a usable direct or TURN path");
        return;
    }
    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED)
        maybeReportReady();
}

void RoomConnection::maybeReportReady()
{
    if (closed_ || ready_reported_ || !agent_ || !local_description_sent_ ||
        !remote_description_received_) return;
    const auto state = juice_get_state(agent_);
    if (state != JUICE_STATE_CONNECTED && state != JUICE_STATE_COMPLETED) return;

    std::array<char, JUICE_MAX_ADDRESS_STRING_LEN> local{};
    std::array<char, JUICE_MAX_ADDRESS_STRING_LEN> remote{};
    if (juice_get_selected_addresses(agent_, local.data(), local.size(),
            remote.data(), remote.size()) != JUICE_ERR_SUCCESS) {
        reportError("ICE selected path is unavailable");
        return;
    }
    std::array<char, JUICE_MAX_SDP_STRING_LEN> local_candidate{};
    std::array<char, JUICE_MAX_SDP_STRING_LEN> remote_candidate{};
    if (juice_get_selected_candidates(agent_, local_candidate.data(), local_candidate.size(),
            remote_candidate.data(), remote_candidate.size()) != JUICE_ERR_SUCCESS) {
        reportError("ICE selected candidate type is unavailable");
        return;
    }
    const auto local_ufrag = iceUfrag(local_description_);
    const auto remote_ufrag = iceUfrag(remote_description_);
    if (!local_ufrag || !remote_ufrag || *local_ufrag == *remote_ufrag) {
        reportError("ICE descriptions do not provide distinct connection roles");
        return;
    }
    try {
        local_endpoint_ = parseAddress(local.data());
        remote_endpoint_ = parseAddress(remote.data());
    } catch (const std::exception& error) {
        reportError(error.what());
        return;
    }
    const std::string local_type(local_candidate.data());
    const std::string remote_type(remote_candidate.data());
    const bool uses_turn = local_type.find(" typ relay") != std::string::npos ||
        remote_type.find(" typ relay") != std::string::npos;
    const ConnectionPath path = uses_turn ? ConnectionPath::Relay
        : (isLanAddress(local_endpoint_.address()) &&
            isLanAddress(remote_endpoint_.address())
            ? ConnectionPath::Lan : ConnectionPath::NatTraversal);
    ready_reported_ = true;
    asio::error_code ignored;
    setup_timeout_.cancel();
    if (ready_handler_)
        ready_handler_(shared_from_this(), local_endpoint_, remote_endpoint_,
            *local_ufrag < *remote_ufrag, path);
}

void RoomConnection::reportError(std::string message)
{
    if (closed_.exchange(true)) return;
    asio::error_code ignored;
    resolver_.cancel();
    setup_timeout_.cancel();
    stream_.lowest_layer().cancel(ignored);
    stream_.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    stream_.lowest_layer().close(ignored);
    if (agent_) {
        juice_agent_t* agent = agent_;
        agent_ = nullptr;
        juice_destroy(agent);
    }
    if (error_handler_) error_handler_(shared_from_this(), std::move(message));
}

void RoomConnection::close() noexcept
{
    if (closed_.exchange(true)) return;
    asio::error_code ignored;
    resolver_.cancel();
    try { setup_timeout_.cancel(); } catch (...) {}
    stream_.lowest_layer().cancel(ignored);
    stream_.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    stream_.lowest_layer().close(ignored);
    if (agent_) {
        juice_agent_t* agent = agent_;
        agent_ = nullptr;
        juice_destroy(agent);
    }
}

int RoomConnection::sendDatagram(const std::vector<std::uint8_t>& datagram) noexcept
{
    if (closed_ || !agent_ || datagram.empty()) return JUICE_ERR_FAILED;
    const int result = juice_send(agent_, reinterpret_cast<const char*>(datagram.data()), datagram.size());
    if (result != JUICE_ERR_SUCCESS) {
        auto weak = weak_from_this();
        asio::post(io_, [weak, result] {
            if (auto self = weak.lock())
                self->reportError("Sending QUIC datagram through ICE failed (code " +
                    std::to_string(result) + ")");
        });
    }
    return result;
}

void RoomConnection::stateChanged(juice_agent_t* agent, juice_state_t state, void* user_data)
{
    (void)agent;
    auto* self = static_cast<RoomConnection*>(user_data);
    if (!self || self->closed_) return;
    auto weak = self->weak_from_this();
    asio::post(self->io_, [weak, state] {
        if (auto owner = weak.lock(); owner && !owner->closed_) owner->onIceState(state);
    });
}

void RoomConnection::gatheringDone(juice_agent_t* agent, void* user_data)
{
    (void)agent;
    auto* self = static_cast<RoomConnection*>(user_data);
    if (!self || self->closed_) return;
    auto weak = self->weak_from_this();
    asio::post(self->io_, [weak] {
        if (auto owner = weak.lock(); owner && !owner->closed_) owner->onLocalGatheringDone();
    });
}

void RoomConnection::received(juice_agent_t* agent, const char* data,
    std::size_t size, void* user_data)
{
    (void)agent;
    auto* self = static_cast<RoomConnection*>(user_data);
    if (!self || self->closed_ || !data || size == 0) return;
    std::vector<std::uint8_t> packet(reinterpret_cast<const std::uint8_t*>(data),
        reinterpret_cast<const std::uint8_t*>(data) + size);
    auto weak = self->weak_from_this();
    asio::post(self->io_, [weak, packet = std::move(packet)]() mutable {
        if (auto owner = weak.lock(); owner && !owner->closed_ && owner->packet_handler_)
            owner->packet_handler_(std::move(owner), std::move(packet));
    });
}
