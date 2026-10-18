#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Message.h"
#include "PeerId.h"
#include "SessionCallbackHandler.h"

enum class ConnectionDirection : bool { Incoming, Outgoing };

// A Session is a reliable, ordered QUIC stream pair driven by datagrams from
// PeerManager's shared UDP socket. It never owns a socket or assumes how a
// datagram reaches that socket (direct, ICE-selected, or relay-encapsulated).
class Session : public std::enable_shared_from_this<Session>
{
public:
    using UdpEndpoint = asio::ip::udp::endpoint;
    using DatagramSender = std::function<void(const UdpEndpoint&, std::vector<uint8_t>)>;
    using CidRegistrar = std::function<void(const std::string&, std::shared_ptr<Session>)>;
    static constexpr std::size_t kConnectionIdLength = 16;

    Session(asio::ip::udp::socket& socket, UdpEndpoint remote,
        std::shared_ptr<asio::ssl::context> tls_context, PeerInfo local,
        SessionCallbackHandler* session_callback, ConnectionDirection direction,
        DatagramSender send_datagram, CidRegistrar register_cid);
    ~Session();

    // Outgoing sessions call initiateHandshake() then start(). Incoming
    // sessions are started with the first validated QUIC Initial datagram.
    void start();
    void startServer(const ngtcp2_pkt_hd& initial_header, const uint8_t* packet,
        std::size_t packet_size);
    void initiateHandshake();
    void onDatagram(const uint8_t* packet, std::size_t packet_size,
        const UdpEndpoint& source);

    void queueMessage(Message msg);
    void selectPeer();
    void deselectPeer();
    ConnectionDirection getDirection() const;
    void close();

private:
    struct OutgoingFrame {
        std::vector<uint8_t> bytes;
        uint64_t stream_offset = 0;
        std::size_t submitted = 0;
        bool stream_offset_assigned = false;
        std::vector<std::pair<uint64_t, uint64_t>> acknowledged;
    };

    asio::ip::udp::socket& udp_socket_;
    UdpEndpoint remote_endpoint_;
    std::shared_ptr<asio::ssl::context> tls_context_;
    std::shared_ptr<asio::steady_timer> expiry_timer_;
    PeerInfo local_;
    PeerId remote_id_;
    SessionCallbackHandler* session_callback_ = nullptr;
    ConnectionDirection direction_;
    DatagramSender send_datagram_;
    CidRegistrar register_cid_;

    ngtcp2_conn* conn_ = nullptr;
    ngtcp2_crypto_ossl_ctx* ossl_context_ = nullptr;
    SSL* ssl_ = nullptr;
    ngtcp2_crypto_conn_ref conn_ref_{};
    ngtcp2_path_storage path_storage_{};
    int64_t send_stream_id_ = -1;
    int64_t receive_stream_id_ = -1;
    uint64_t receive_stream_offset_ = 0;
    uint64_t next_send_offset_ = 0;
    std::deque<std::shared_ptr<OutgoingFrame>> pending_frames_;
    std::deque<std::shared_ptr<OutgoingFrame>> inflight_frames_;
    std::vector<char> receive_buffer_;
    std::deque<Message> read_queue_;

    bool is_selected_ = false;
    bool closed_ = false;
    bool handshake_requested_ = false;
    bool quic_handshake_complete_ = false;
    bool app_handshake_complete_ = false;
    bool peer_announced_ = false;
    std::optional<asio::error_code> pending_failure_;

    void initializeConnection(bool server, const ngtcp2_pkt_hd* initial_header = nullptr);
    void consumeDatagram(const uint8_t* packet, std::size_t packet_size,
        const UdpEndpoint& source);
    void handleHandshakeComplete();
    void handleStreamData(int64_t stream_id, uint64_t offset,
        const uint8_t* data, std::size_t length);
    void processReceivedFrames();
    void processMessage(Message msg);
    void handleHandshake(const PeerInfo& remote);
    void acknowledgeStreamBytes(uint64_t offset, std::size_t length);
    void driveOutput();
    void updateExpiryTimer();
    void onExpiry(const asio::error_code& error);
    void fail(const asio::error_code& error) noexcept;
    void finishFailure() noexcept;
    void releaseResources() noexcept;
    void failQuic(int error) noexcept;
    static ngtcp2_conn* getConn(ngtcp2_crypto_conn_ref* conn_ref);
    static ngtcp2_callbacks makeCallbacks(bool server);
    static void randomBytes(uint8_t* destination, size_t length,
        const ngtcp2_rand_ctx* random_context);
    static int onHandshakeCompleted(ngtcp2_conn* conn, void* user_data);
    static int onRecvStreamData(ngtcp2_conn* conn, uint32_t flags,
        int64_t stream_id, uint64_t offset, const uint8_t* data,
        size_t datalen, void* user_data, void* stream_user_data);
    static int onAckedStreamData(ngtcp2_conn* conn, int64_t stream_id,
        uint64_t offset, uint64_t datalen, void* user_data,
        void* stream_user_data);
    static int onNewConnectionId(ngtcp2_conn* conn, ngtcp2_cid* cid,
        ngtcp2_stateless_reset_token* token, size_t cid_length, void* user_data);
};
