# Transport security and identity

## Transport

Peer connections use IETF QUIC over UDP through ngtcp2 and OpenSSL's TLS 1.3
implementation. QUIC provides authenticated encryption, retransmission, and
ordered reliable streams. The chat still serializes each message as the existing
four-byte length, message type, and body; QUIC stream boundaries do not replace
or alter that application framing. Each peer opens one bidirectional QUIC
stream, so chat messages in each direction are delivered in order.
Sessions send a QUIC keep-alive PING after 10 seconds without traffic. This
prevents the configured 30-second QUIC idle timeout from closing an otherwise
quiet chat and helps maintain UDP/NAT state. It is a liveness aid, not a promise
that every router or network will retain a mapping indefinitely; a peer that
is offline or unreachable still disconnects after transport failure detection.

`PeerManager` owns one bound UDP socket. It routes incoming QUIC datagrams to
sessions by the destination connection ID. A `Session` owns only the QUIC and
TLS state; it receives datagrams and emits datagrams through callbacks. This
keeps message framing and session behavior independent from how UDP packets
reach the socket, which provides a place to add ICE-selected paths or relay
encapsulation later.
TLS negotiates the application protocol identifier `p2pchat/1` so a client
cannot accidentally establish a chat session against a different QUIC service.

The app currently supports direct connections to a numeric IP address. It does
not yet implement STUN, ICE candidate exchange, hole punching, a relay, or
address discovery. Both peers still need a reachable UDP path, such as the same
LAN or manually configured router forwarding. The shared socket is not yet
demultiplexing STUN or relay packets.

The ngtcp2 core implements QUIC. This project uses ngtcp2's OpenSSL TLS backend,
which is marked experimental by ngtcp2 and requires OpenSSL 3.5 or newer for its
QUIC TLS APIs. The CMake and vcpkg dependencies enforce that minimum. Review
ngtcp2 and OpenSSL release notes before shipping a production build.

## Peer identity

Each installation creates an Ed25519 key and a self-signed X.509 certificate.
The stable 256-bit peer ID is SHA-256 of the key's DER
SubjectPublicKeyInfo. TLS proves possession of the private key. After the QUIC
TLS handshake, the application handshake supplies the peer ID and username;
the session rejects a claimed ID that does not match the certificate key.
Certificates are self-signed and are not checked against a public certificate
authority, so the key-derived peer ID is the identity. Verify a new peer ID
through another trusted channel when first-contact identity matters.

The private key and public certificate are stored in the existing SQLite
settings database in a portable format. The private key is not encrypted at
rest; anyone who can read the database can impersonate this installation. A
Windows-only DPAPI identity from the previous format is migrated to the
portable format when opened on Windows. Linux and macOS cannot decrypt a
database that contains only that old DPAPI value; those platforms create a new
identity until the database is migrated on Windows. Existing random peer ID
settings remain unused legacy data. Since the current peer ID is derived from
the public key, peers may see an installation as a new identity after an
identity-format upgrade.

## Limits

QUIC protects traffic between the two endpoints, including through a future
transparent UDP relay. A relay cannot read or change authenticated stream
contents, but it can observe endpoint addresses, connection IDs, timing, packet
sizes, and traffic volume, and can drop or delay packets. This transport does
not provide offline store-and-forward, message signatures, a double ratchet,
post-compromise recovery, group encryption, or anonymity. The SQLite chat
history is stored as plaintext on the local device. A future relay that stores
messages beyond a live QUIC connection will need a separate encrypted message
envelope and delivery protocol.
