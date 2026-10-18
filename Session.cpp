#include "Session.h"

#include "CryptoIdentity.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

constexpr ngtcp2_duration kKeepAliveInterval = 10 * NGTCP2_SECONDS;

class Ngtcp2ErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "ngtcp2"; }
    std::string message(int error) const override { return ngtcp2_strerror(error); }
};

const Ngtcp2ErrorCategory ngtcp2_error_category;

ngtcp2_tstamp quicNow()
{
    using namespace std::chrono;
    return static_cast<ngtcp2_tstamp>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string cidKey(const ngtcp2_cid& cid)
{
    return std::string(reinterpret_cast<const char*>(cid.data), cid.datalen);
}

bool makeCid(ngtcp2_cid& cid)
{
    cid.datalen = Session::kConnectionIdLength;
    return RAND_bytes(cid.data, static_cast<int>(cid.datalen)) == 1;
}

} // namespace

ngtcp2_callbacks Session::makeCallbacks(bool server)
{
    ngtcp2_callbacks callbacks{};
    callbacks.client_initial = server ? nullptr : ngtcp2_crypto_client_initial_cb;
    callbacks.recv_client_initial = server ? ngtcp2_crypto_recv_client_initial_cb : nullptr;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.handshake_completed = &Session::onHandshakeCompleted;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.recv_stream_data = &Session::onRecvStreamData;
    callbacks.acked_stream_data_offset = &Session::onAckedStreamData;
    callbacks.recv_retry = server ? nullptr : ngtcp2_crypto_recv_retry_cb;
    callbacks.rand = &Session::randomBytes;
    callbacks.get_new_connection_id2 = &Session::onNewConnectionId;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    return callbacks;
}

Session::Session(asio::ip::udp::socket& socket, UdpEndpoint remote,
    std::shared_ptr<asio::ssl::context> tls_context, PeerInfo local,
    SessionCallbackHandler* callback, ConnectionDirection direction,
    DatagramSender send_datagram, CidRegistrar register_cid)
    : udp_socket_(socket), remote_endpoint_(std::move(remote)),
      tls_context_(std::move(tls_context)),
      expiry_timer_(std::make_shared<asio::steady_timer>(socket.get_executor())),
      local_(std::move(local)), session_callback_(callback), direction_(direction),
      send_datagram_(std::move(send_datagram)), register_cid_(std::move(register_cid))
{
    if (!tls_context_ || !session_callback_ || !send_datagram_ || !register_cid_)
        throw std::invalid_argument("Session requires a TLS context and callbacks");
}

Session::~Session()
{
    close();
}

void Session::initiateHandshake()
{
    if (closed_ || handshake_requested_) {
        fail(asio::error::make_error_code(asio::error::invalid_argument));
        return;
    }
    handshake_requested_ = true;
}

void Session::start()
{
    if (closed_) return;
    if (direction_ != ConnectionDirection::Outgoing || !handshake_requested_) {
        fail(asio::error::make_error_code(asio::error::invalid_argument));
        return;
    }
    try {
        initializeConnection(false);
        driveOutput();
    } catch (...) {
        fail(asio::error::make_error_code(asio::error::fault));
    }
}

void Session::startServer(const ngtcp2_pkt_hd& initial_header, const uint8_t* packet,
    std::size_t packet_size)
{
    if (closed_ || direction_ != ConnectionDirection::Incoming || !packet || packet_size == 0) {
        fail(asio::error::make_error_code(asio::error::invalid_argument));
        return;
    }
    handshake_requested_ = true;
    try {
        initializeConnection(true, &initial_header);
        consumeDatagram(packet, packet_size, remote_endpoint_);
    } catch (...) {
        fail(asio::error::make_error_code(asio::error::fault));
    }
}

void Session::initializeConnection(bool server, const ngtcp2_pkt_hd* initial_header)
{
    if (conn_) throw std::logic_error("QUIC connection already initialized");

    auto local_endpoint = udp_socket_.local_endpoint();
    ngtcp2_path_storage_init(&path_storage_,
        reinterpret_cast<const ngtcp2_sockaddr*>(local_endpoint.data()),
        static_cast<ngtcp2_socklen>(local_endpoint.size()),
        reinterpret_cast<const ngtcp2_sockaddr*>(remote_endpoint_.data()),
        static_cast<ngtcp2_socklen>(remote_endpoint_.size()), nullptr);

    ngtcp2_cid source_cid{};
    if (!makeCid(source_cid)) throw std::runtime_error("Generating QUIC connection ID failed");

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = quicNow();
    settings.max_tx_udp_payload_size = 1200;

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_data = 1024 * 1024;
    params.initial_max_streams_bidi = 4;
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;
    params.active_connection_id_limit = 2;
    if (server) {
        params.original_dcid = initial_header->dcid;
        params.original_dcid_present = 1;
    }

    const auto callbacks = Session::makeCallbacks(server);
    int rv;
    if (server) {
        if (!initial_header) throw std::invalid_argument("Server QUIC Initial header is missing");
        rv = ngtcp2_conn_server_new(&conn_, &initial_header->scid, &source_cid,
            &path_storage_.path, initial_header->version, &callbacks, &settings,
            &params, nullptr, this);
    } else {
        ngtcp2_cid destination_cid{};
        if (!makeCid(destination_cid)) throw std::runtime_error("Generating QUIC destination ID failed");
        rv = ngtcp2_conn_client_new(&conn_, &destination_cid, &source_cid,
            &path_storage_.path, NGTCP2_PROTO_VER_V1, &callbacks, &settings,
            &params, nullptr, this);
    }
    if (rv != 0) throw std::runtime_error(ngtcp2_strerror(rv));

    // ngtcp2 includes this deadline in its connection expiry. When the
    // connection has been quiet for this long, handleExpiry schedules a QUIC
    // PING, and driveOutput sends it through the shared UDP socket.
    ngtcp2_conn_set_keep_alive_timeout(conn_, kKeepAliveInterval);

    const auto self = shared_from_this();
    const std::string local_cid = cidKey(source_cid);
    register_cid_(local_cid, self);
    if (server) {
        // The client addresses its Initial packets to this original DCID until
        // the server's replacement source CID is in use. Route retransmitted
        // Initials to this session instead of constructing a duplicate one.
        register_cid_(cidKey(initial_header->dcid), self);
    }

    ssl_ = SSL_new(tls_context_->native_handle());
    if (!ssl_) throw std::runtime_error("Creating QUIC TLS session failed");
    conn_ref_.get_conn = &Session::getConn;
    conn_ref_.user_data = this;
    SSL_set_app_data(ssl_, &conn_ref_);

    const int tls_rv = server
        ? ngtcp2_crypto_ossl_configure_server_session(ssl_)
        : ngtcp2_crypto_ossl_configure_client_session(ssl_);
    if (tls_rv != 0) throw std::runtime_error("Configuring QUIC TLS session failed");
    if (!server) {
        CryptoIdentity::configureQuicClientAlpn(ssl_);
        SSL_set_connect_state(ssl_);
    }
    else SSL_set_accept_state(ssl_);

    rv = ngtcp2_crypto_ossl_ctx_new(&ossl_context_, ssl_);
    if (rv != 0) throw std::runtime_error("Creating ngtcp2 OpenSSL context failed");
    ngtcp2_conn_set_tls_native_handle(conn_, ossl_context_);
    updateExpiryTimer();
}

void Session::onDatagram(const uint8_t* packet, std::size_t packet_size,
    const UdpEndpoint& source)
{
    if (closed_ || !conn_ || !packet || packet_size == 0) return;
    try { consumeDatagram(packet, packet_size, source); }
    catch (...) { fail(asio::error::make_error_code(asio::error::fault)); }
}

void Session::consumeDatagram(const uint8_t* packet, std::size_t packet_size,
    const UdpEndpoint& source)
{
    auto local_endpoint = udp_socket_.local_endpoint();
    ngtcp2_path_storage_init(&path_storage_,
        reinterpret_cast<const ngtcp2_sockaddr*>(local_endpoint.data()),
        static_cast<ngtcp2_socklen>(local_endpoint.size()),
        reinterpret_cast<const ngtcp2_sockaddr*>(source.data()),
        static_cast<ngtcp2_socklen>(source.size()), nullptr);
    remote_endpoint_ = source;
    ngtcp2_pkt_info packet_info{};
    const int rv = ngtcp2_conn_read_pkt(conn_, &path_storage_.path, &packet_info,
        packet, packet_size, quicNow());
    if (rv != 0) { failQuic(rv); return; }
    driveOutput();
    updateExpiryTimer();
}

void Session::handleHandshakeComplete()
{
    if (!closed_) quic_handshake_complete_ = true;
}

void Session::handleStreamData(int64_t stream_id, uint64_t offset,
    const uint8_t* data, std::size_t length)
{
    if (closed_ || !data || length == 0) return;
    if (receive_stream_id_ == -1) receive_stream_id_ = stream_id;
    if (receive_stream_id_ != stream_id || offset != receive_stream_offset_) {
        fail(asio::error::make_error_code(asio::error::fault));
        return;
    }
    receive_buffer_.insert(receive_buffer_.end(), data, data + length);
    receive_stream_offset_ += length;
    processReceivedFrames();
}

void Session::processReceivedFrames()
{
    while (!closed_ && receive_buffer_.size() >= 4) {
        const uint32_t length =
            (static_cast<uint32_t>(static_cast<unsigned char>(receive_buffer_[0])) << 24) |
            (static_cast<uint32_t>(static_cast<unsigned char>(receive_buffer_[1])) << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(receive_buffer_[2])) << 8) |
             static_cast<uint32_t>(static_cast<unsigned char>(receive_buffer_[3]));
        if (length < 1 || length > MAX_MESSAGE_SIZE) {
            fail(asio::error::make_error_code(asio::error::message_size));
            return;
        }
        if (receive_buffer_.size() < 4u + length) return;
        const auto raw_type = static_cast<uint8_t>(receive_buffer_[4]);
        Message msg{static_cast<MessageType>(raw_type),
            std::vector<char>(receive_buffer_.begin() + 5,
                receive_buffer_.begin() + static_cast<std::ptrdiff_t>(4u + length))};
        receive_buffer_.erase(receive_buffer_.begin(),
            receive_buffer_.begin() + static_cast<std::ptrdiff_t>(4u + length));
        processMessage(std::move(msg));
    }
}

void Session::processMessage(Message msg)
{
    if (closed_) return;
    if (msg.type == MessageType::Handshake) {
        PeerInfo remote;
        if (app_handshake_complete_ || !parseHandshake(msg.body, remote)) {
            fail(asio::error::make_error_code(asio::error::invalid_argument));
            return;
        }
        try {
            const PeerId tls_peer_id = CryptoIdentity::peerIdFromTlsPeer(ssl_);
            if (remote.id != tls_peer_id) {
                fail(asio::error::make_error_code(asio::error::fault));
                return;
            }
        } catch (...) {
            fail(asio::error::make_error_code(asio::error::fault));
            return;
        }
        handleHandshake(remote);
        return;
    }
    if (msg.type != MessageType::Chat || !app_handshake_complete_) {
        fail(asio::error::make_error_code(asio::error::fault));
        return;
    }
    if (is_selected_ && peer_announced_)
        session_callback_->read(remote_id_, std::string(msg.body.begin(), msg.body.end()));
    else
        read_queue_.push_back(std::move(msg));
}

void Session::queueMessage(Message msg)
{
    if (closed_) throw std::runtime_error("Session is closed");
    auto encoded = serialize(msg);
    auto frame = std::make_shared<OutgoingFrame>();
    frame->bytes.assign(encoded.begin(), encoded.end());
    pending_frames_.push_back(std::move(frame));
    driveOutput();
}

void Session::driveOutput()
{
    if (closed_ || !conn_) return;
    if (quic_handshake_complete_ && send_stream_id_ < 0) {
        if (ngtcp2_conn_open_bidi_stream(conn_, &send_stream_id_, nullptr) != 0) {
            fail(asio::error::make_error_code(asio::error::fault));
            return;
        }
        try {
            auto encoded = serialize(makeHandshake(local_));
            auto frame = std::make_shared<OutgoingFrame>();
            frame->bytes.assign(encoded.begin(), encoded.end());
            pending_frames_.push_back(std::move(frame));
        } catch (...) {
            fail(asio::error::make_error_code(asio::error::fault));
            return;
        }
    }
    std::array<uint8_t, 1400> packet{};
    for (std::size_t count = 0; count < 32 && !closed_; ++count) {
        ngtcp2_pkt_info packet_info{};
        ngtcp2_ssize written = 0;
        ngtcp2_ssize consumed = 0;

        while (!pending_frames_.empty() &&
            pending_frames_.front()->submitted == pending_frames_.front()->bytes.size()) {
            inflight_frames_.push_back(std::move(pending_frames_.front()));
            pending_frames_.pop_front();
        }

        if (send_stream_id_ >= 0 && !pending_frames_.empty()) {
            auto& frame = *pending_frames_.front();
            ngtcp2_vec data{};
            data.base = frame.bytes.data() + frame.submitted;
            data.len = frame.bytes.size() - frame.submitted;
            written = ngtcp2_conn_writev_stream(conn_, &path_storage_.path, &packet_info,
                packet.data(), packet.size(), &consumed, NGTCP2_WRITE_STREAM_FLAG_NONE,
                send_stream_id_, &data, 1, quicNow());
            if (consumed > 0) {
                if (frame.submitted == 0) {
                    frame.stream_offset = next_send_offset_;
                    frame.stream_offset_assigned = true;
                }
                frame.submitted += static_cast<std::size_t>(consumed);
                next_send_offset_ += static_cast<uint64_t>(consumed);
            }
            if (frame.submitted == frame.bytes.size()) {
                inflight_frames_.push_back(std::move(pending_frames_.front()));
                pending_frames_.pop_front();
            }
        } else {
            written = ngtcp2_conn_write_pkt(conn_, &path_storage_.path, &packet_info,
                packet.data(), packet.size(), quicNow());
        }

        if (written < 0) {
            failQuic(static_cast<int>(written));
            return;
        }
        if (written == 0) break;
        send_datagram_(remote_endpoint_,
            std::vector<uint8_t>(packet.begin(), packet.begin() + written));
    }
    updateExpiryTimer();
}

void Session::acknowledgeStreamBytes(uint64_t offset, std::size_t length)
{
    const uint64_t ack_end = offset + length;
    auto recordAcknowledgedRange = [&](const std::shared_ptr<OutgoingFrame>& frame) {
        if (!frame->stream_offset_assigned) return;
        const uint64_t start = std::max(offset, frame->stream_offset);
        const uint64_t end = std::min(ack_end, frame->stream_offset + frame->bytes.size());
        if (start >= end) return;
        frame->acknowledged.emplace_back(start, end);
        std::sort(frame->acknowledged.begin(), frame->acknowledged.end());
        std::vector<std::pair<uint64_t, uint64_t>> merged;
        for (const auto& range : frame->acknowledged) {
            if (merged.empty() || range.first > merged.back().second)
                merged.push_back(range);
            else
                merged.back().second = std::max(merged.back().second, range.second);
        }
        frame->acknowledged = std::move(merged);
    };
    // ngtcp2 can acknowledge early fragments while a larger application frame
    // is still being submitted. Keep those ACK ranges with the pending buffer.
    for (const auto& frame : pending_frames_) recordAcknowledgedRange(frame);
    for (const auto& frame : inflight_frames_) recordAcknowledgedRange(frame);
    while (!inflight_frames_.empty()) {
        const auto& frame = inflight_frames_.front();
        if (frame->acknowledged.size() != 1 ||
            frame->acknowledged.front().first != frame->stream_offset ||
            frame->acknowledged.front().second != frame->stream_offset + frame->bytes.size())
            break;
        inflight_frames_.pop_front();
    }
}

void Session::updateExpiryTimer()
{
    if (closed_ || !conn_ || !expiry_timer_) return;
    const ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry2(conn_);
    if (expiry == std::numeric_limits<ngtcp2_tstamp>::max()) {
        expiry_timer_->cancel();
        return;
    }
    const ngtcp2_tstamp now = quicNow();
    const auto delay = std::chrono::nanoseconds(expiry > now ? expiry - now : 0);
    expiry_timer_->expires_after(delay);
    auto self = shared_from_this();
    expiry_timer_->async_wait([self](const asio::error_code& ec) { self->onExpiry(ec); });
}

void Session::onExpiry(const asio::error_code& error)
{
    if (closed_ || error == asio::error::operation_aborted) return;
    if (error) { fail(error); return; }
    const int rv = ngtcp2_conn_handle_expiry(conn_, quicNow());
    if (rv != 0) { failQuic(rv); return; }
    driveOutput();
}

void Session::handleHandshake(const PeerInfo& remote)
{
    remote_id_ = remote.id;
    app_handshake_complete_ = true;
    auto self = shared_from_this();
    asio::post(udp_socket_.get_executor(), [self, remote] {
        if (self->closed_) return;
        try {
            self->session_callback_->onPeerIdentified(remote, self);
            if (!self->closed_) self->peer_announced_ = true;
        } catch (...) {
            self->fail(asio::error::make_error_code(asio::error::fault));
        }
    });
}

void Session::selectPeer()
{
    if (closed_) return;
    is_selected_ = true;
    try {
        for (const auto& msg : read_queue_)
            if (msg.type == MessageType::Chat)
                session_callback_->read(remote_id_, std::string(msg.body.begin(), msg.body.end()));
        read_queue_.clear();
    } catch (...) { fail(asio::error::make_error_code(asio::error::fault)); }
}

void Session::deselectPeer() { is_selected_ = false; }
ConnectionDirection Session::getDirection() const { return direction_; }

void Session::fail(const asio::error_code& error) noexcept
{
    if (closed_ || pending_failure_) return;
    closed_ = true;
    pending_failure_ = error;
    auto self = shared_from_this();
    try { asio::post(udp_socket_.get_executor(), [self] { self->finishFailure(); }); }
    catch (...) { releaseResources(); }
}

void Session::finishFailure() noexcept
{
    if (!pending_failure_) return;
    const auto error = *pending_failure_;
    pending_failure_.reset();
    const bool identified = app_handshake_complete_;
    releaseResources();
    try {
        session_callback_->onSessionError(
            identified ? std::optional<PeerId>(remote_id_) : std::nullopt,
            shared_from_this(), error);
    } catch (...) {}
}

void Session::failQuic(int error) noexcept
{
    fail(asio::error_code(error, ngtcp2_error_category));
}

void Session::close()
{
    if (closed_ && !pending_failure_) return;
    closed_ = true;
    pending_failure_.reset();
    releaseResources();
}

void Session::releaseResources() noexcept
{
    if (expiry_timer_) {
        try { expiry_timer_->cancel(); } catch (...) {}
    }
    if (conn_) { ngtcp2_conn_del(conn_); conn_ = nullptr; }
    if (ssl_) {
        SSL_set_app_data(ssl_, nullptr);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ossl_context_) { ngtcp2_crypto_ossl_ctx_del(ossl_context_); ossl_context_ = nullptr; }
}

ngtcp2_conn* Session::getConn(ngtcp2_crypto_conn_ref* conn_ref)
{
    auto* self = static_cast<Session*>(conn_ref->user_data);
    return self ? self->conn_ : nullptr;
}

void Session::randomBytes(uint8_t* destination, size_t length,
    const ngtcp2_rand_ctx* random_context)
{
    (void)random_context;
    if (RAND_bytes(destination, static_cast<int>(length)) != 1)
        std::terminate();
}

int Session::onHandshakeCompleted(ngtcp2_conn* conn, void* user_data)
{
    (void)conn;
    static_cast<Session*>(user_data)->handleHandshakeComplete();
    return 0;
}

int Session::onRecvStreamData(ngtcp2_conn* conn, uint32_t flags,
    int64_t stream_id, uint64_t offset, const uint8_t* data, size_t datalen,
    void* user_data, void* stream_user_data)
{
    (void)conn; (void)flags; (void)stream_user_data;
    static_cast<Session*>(user_data)->handleStreamData(stream_id, offset, data, datalen);
    return 0;
}

int Session::onAckedStreamData(ngtcp2_conn* conn, int64_t stream_id,
    uint64_t offset, uint64_t datalen, void* user_data, void* stream_user_data)
{
    (void)conn; (void)stream_id; (void)stream_user_data;
    static_cast<Session*>(user_data)->acknowledgeStreamBytes(offset,
        static_cast<std::size_t>(datalen));
    return 0;
}

int Session::onNewConnectionId(ngtcp2_conn* conn, ngtcp2_cid* cid,
    ngtcp2_stateless_reset_token* token, size_t cid_length, void* user_data)
{
    (void)conn;
    auto* self = static_cast<Session*>(user_data);
    cid->datalen = cid_length;
    if (RAND_bytes(cid->data, static_cast<int>(cid_length)) != 1 ||
        RAND_bytes(token->data, sizeof(token->data)) != 1)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    try {
        self->register_cid_(cidKey(*cid), self->shared_from_this());
    } catch (...) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
