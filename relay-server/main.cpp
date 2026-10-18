#include "RoomRelayServer.h"

#include <asio/signal_set.hpp>

#include <charconv>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>

namespace {

std::uint16_t parsePort(const std::string& text, const char* name)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value == 0 || value > 65535)
        throw std::invalid_argument(std::string("Invalid ") + name + " port");
    return static_cast<std::uint16_t>(value);
}

void usage()
{
    std::cerr << "Usage: p2pChatRelayServer --cert <pem> --key <pem> "
        "[--bind <ip>] [--external-address <public-ip>] "
        "[--signal-port <port>] [--turn-port <port>] "
        "[--relay-ports <first>-<last>]\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        RoomRelayServer::Config config;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (i + 1 >= argc) { usage(); return 2; }
            const std::string value = argv[++i];
            if (option == "--cert") config.certificate_path = value;
            else if (option == "--key") config.private_key_path = value;
            else if (option == "--bind") config.bind_address = value;
            else if (option == "--external-address") config.external_address = value;
            else if (option == "--signal-port") config.signal_port = parsePort(value, "signal");
            else if (option == "--turn-port") config.turn_port = parsePort(value, "TURN");
            else if (option == "--relay-ports") {
                const auto dash = value.find('-');
                if (dash == std::string::npos) throw std::invalid_argument("Relay ports must be FIRST-LAST");
                config.relay_port_begin = parsePort(value.substr(0, dash), "relay range");
                config.relay_port_end = parsePort(value.substr(dash + 1), "relay range");
                if (config.relay_port_begin > config.relay_port_end)
                    throw std::invalid_argument("Relay port range is reversed");
            } else {
                usage();
                return 2;
            }
        }

        asio::io_context io;
        RoomRelayServer server(io, std::move(config));
        server.start();
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&io](const asio::error_code&, int) { io.stop(); });
        std::cout << "p2pChat rendezvous and STUN/TURN relay services are ready\n";
        io.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Relay server could not start: " << error.what() << '\n';
        usage();
        return 1;
    }
}
