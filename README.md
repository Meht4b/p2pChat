# p2pChat room connectivity

The application supports two connection commands:

- `/connect <local-ip> <port>` creates a QUIC connection to a numeric peer address.
- `/connect <room-token> <relay-host[:signal-port]>` joins a two-person room, exchanges ICE descriptions over TLS, then starts QUIC over the selected ICE path.

Room setup expires after 90 seconds if the other participant does not join or ICE cannot select a usable path.

Use a random room token shared through a separate trusted channel. It must have at least 16 characters; for example, generate 24 random bytes with `openssl rand -hex 24`. Do not use a memorable phrase as the token. Both participants must use the same token and relay hostname. The signaling port defaults to 7443.

## Build

The root CMake project builds the chat app, tests, and relay service by default. Dependencies are declared in `vcpkg.json`: OpenSSL 3.5+, ngtcp2 with its OpenSSL backend, libjuice, SQLite, and FTXUI. Standalone Asio headers must be discoverable by CMake or fetched by FetchContent.

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

To build only the separate relay executable from the root build, build target `p2pChatRelayServer`. You can also configure `relay-server/` independently with the same vcpkg toolchain; provide `ASIO_INCLUDE_DIR` if CMake does not find Asio automatically.

## Run the rendezvous and TURN relay service

Install a TLS certificate whose name matches the hostname clients will enter. The certificate must chain to a CA trusted by the clients. On a public server, set the external address to its public IPv4 or IPv6 address:

```sh
./p2pChatRelayServer --cert fullchain.pem --key privkey.pem \
  --bind 0.0.0.0 --external-address 203.0.113.10 \
  --signal-port 7443 --turn-port 3478 --relay-ports 50000-50100
```

Allow inbound TCP 7443 for TLS signaling and UDP 3478 plus UDP 50000–50100 for STUN/TURN. Adjust ports to match deployment firewall rules. The process prints a ready message after both TLS rendezvous and TURN listeners bind. Keep the private key readable only by the service account. The public address must be reachable and correctly advertised for TURN allocations to work across NAT.

Clients validate the relay certificate with OpenSSL's configured default CA paths. Set `P2P_RENDEZVOUS_CA_FILE` to a PEM CA bundle when those paths do not include the desired trust roots; the relay hostname/IP still must match the certificate.

## What the room server handles

The app hashes the token with a domain separator to form the room identifier. It also derives a separate AES-256-GCM key from the token and encrypts each ICE description with a fresh random 96-bit nonce and 128-bit authentication tag. The room identifier is authenticated as associated data. The server sees the 32-byte room hash, encrypted descriptions, source IP addresses, timing, room occupancy, and TURN metadata; it does not receive usernames, peer IDs, or chat plaintext. TLS protects signaling in transit and validates the server certificate. QUIC TLS then authenticates peers using their key-bound peer identities and encrypts chat end to end, whether ICE selected a direct path or TURN.

This does not provide anonymity or offline message delivery. A relay operator can observe network metadata and can block, delay, replay signaling, or drop packets. QUIC peer authentication protects against silently impersonating a peer but cannot prevent denial of service. Verify a peer's stable ID through another trusted channel when first-contact identity matters. TURN credentials are random temporary credentials limited to one allocation each and expire after one hour. Long-lived chats may need reconnection when the ICE agent's allocation expires, depending on library refresh behavior.

The ICE agent stays alive for the QUIC session and owns candidate-pair changes. Since the app sends every room-path datagram through `juice_send`, libjuice can continue ICE checks and move an active session from TURN to a better direct path when available; the app does not implement a separate QUIC migration controller. See [SECURITY.md](SECURITY.md) for the complete threat model and [COMMUNICATION_MODEL.md](COMMUNICATION_MODEL.md) for packet/session architecture.
