#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <juice/juice.h>

#include "RoomProtocol.h"
#include "ConnectionPath.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RoomConnection : public std::enable_shared_from_this<RoomConnection> {
public:
    using UdpEndpoint = asio::ip::udp::endpoint;
    using ReadyHandler = std::function<void(std::shared_ptr<RoomConnection>,
        UdpEndpoint, UdpEndpoint, bool, ConnectionPath)>;
    using LegacyReadyHandler = std::function<void(std::shared_ptr<RoomConnection>,
        UdpEndpoint, UdpEndpoint, bool)>;
    using PacketHandler = std::function<void(std::shared_ptr<RoomConnection>,
        std::vector<std::uint8_t>)>;
    using ErrorHandler = std::function<void(std::shared_ptr<RoomConnection>, std::string)>;

    RoomConnection(asio::io_context& io, std::string relay_host,
        std::uint16_t signal_port, std::string room_token,
        ReadyHandler ready, PacketHandler packet, ErrorHandler error);
    // Compatibility overload for callers compiled against the earlier route-
    // agnostic readiness callback. New code should use ReadyHandler.
    RoomConnection(asio::io_context& io, std::string relay_host,
        std::uint16_t signal_port, std::string room_token,
        LegacyReadyHandler ready, PacketHandler packet, ErrorHandler error);
    ~RoomConnection();

    void start();
    void close() noexcept;
    int sendDatagram(const std::vector<std::uint8_t>& datagram) noexcept;

    UdpEndpoint localEndpoint() const { return local_endpoint_; }
    UdpEndpoint remoteEndpoint() const { return remote_endpoint_; }

private:
    asio::io_context& io_;
    std::string relay_host_;
    std::string signal_port_text_;
    std::string room_token_;
    ReadyHandler ready_handler_;
    PacketHandler packet_handler_;
    ErrorHandler error_handler_;
    std::shared_ptr<asio::ssl::context> tls_context_;
    asio::ip::tcp::resolver resolver_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    asio::steady_timer setup_timeout_;
    std::array<std::uint8_t, 4> read_header_{};
    std::vector<std::uint8_t> read_body_;
    std::deque<std::vector<std::uint8_t>> write_queue_;
    room_protocol::RoomId room_id_{};
    juice_agent_t* agent_ = nullptr;
    std::string turn_username_;
    std::string turn_password_;
    std::string local_description_;
    std::string remote_description_;
    UdpEndpoint local_endpoint_;
    UdpEndpoint remote_endpoint_;
    bool local_description_sent_ = false;
    bool remote_description_received_ = false;
    bool ready_reported_ = false;
    std::atomic_bool closed_{false};

    void resolveAndConnect();
    void handleWelcome(const std::vector<std::uint8_t>& payload);
    void createIceAgent(std::uint16_t turn_port);
    void readHeader();
    void readBody();
    void handleFrame(const std::vector<std::uint8_t>& encoded);
    void enqueueFrame(room_protocol::Type type, const std::vector<std::uint8_t>& payload);
    void writeNext();
    void onLocalGatheringDone();
    void onRemoteDescription(const std::vector<std::uint8_t>& encrypted);
    void onIceState(juice_state_t state);
    void maybeReportReady();
    void reportError(std::string message);
    static void stateChanged(juice_agent_t* agent, juice_state_t state, void* user_data);
    static void gatheringDone(juice_agent_t* agent, void* user_data);
    static void received(juice_agent_t* agent, const char* data, std::size_t size, void* user_data);
};
