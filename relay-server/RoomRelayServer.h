#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <juice/juice.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../RoomProtocol.h"

class RoomRelayServer {
public:
    struct Config {
        std::string bind_address = "0.0.0.0";
        std::string external_address;
        std::string certificate_path;
        std::string private_key_path;
        std::uint16_t signal_port = 7443;
        std::uint16_t turn_port = 3478;
        std::uint16_t relay_port_begin = 50000;
        std::uint16_t relay_port_end = 50100;
    };

    RoomRelayServer(asio::io_context& io, Config config);
    ~RoomRelayServer();

    void start();

private:
    class Client;
    struct Room {
        std::array<std::weak_ptr<Client>, 2> clients;
        std::array<std::vector<std::uint8_t>, 2> descriptions;
    };

    asio::io_context& io_;
    Config config_;
    asio::ssl::context tls_context_{asio::ssl::context::tls_server};
    asio::ip::tcp::acceptor acceptor_;
    juice_server_t* turn_server_ = nullptr;
    std::unordered_map<std::string, Room> rooms_;

    void acceptNext();
    bool join(const std::shared_ptr<Client>& client, const room_protocol::RoomId& room_id,
        std::size_t& slot, room_protocol::TurnCredentials& credentials,
        std::vector<std::uint8_t>& waiting_description);
    void forwardDescription(const std::shared_ptr<Client>& client,
        const std::vector<std::uint8_t>& encrypted_description);
    void remove(const Client* client, const std::string& room_key, std::size_t slot);
    friend class Client;
};
