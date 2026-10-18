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

`PeerManager` owns one bound UDP socket for manually configured connections.
It routes incoming QUIC datagrams to sessions by destination connection ID. A
`Session` owns only QUIC and TLS state and sends packets through a callback.
For room connections, libjuice owns the ICE-selected UDP path and its callbacks
carry the QUIC packets; the same `Session` logic runs over that path. QUIC is
still the end-to-end security boundary.
TLS negotiates the application protocol identifier `p2pchat/1` so a client
cannot accidentally establish a chat session against a different QUIC service.

`/connect <local-ip> <port>` creates a manual direct QUIC connection.
`/connect <room-token> <relay-host[:port]>` uses TLS-protected rendezvous
signaling and libjuice ICE. The first and second participants exchange their
ICE descriptions through the service; libjuice attempts a direct UDP path and
uses TURN if direct connectivity fails. QUIC starts only after ICE reports a
selected path. Signaling defaults to TCP port 7443 and STUN/TURN to UDP port
3478; the server also needs its configured UDP relay port range reachable.
The relay host certificate must validate against OpenSSL's configured default
CA paths (or the PEM bundle selected by `P2P_RENDEZVOUS_CA_FILE`) and match the
relay hostname/IP entered by the client.

The room server receives only SHA-256 of the room token as its room key and
AES-256-GCM encrypted ICE descriptions. The token must be private and generated
with high entropy (at least 16 random characters; length alone does not make a
human-chosen phrase safe). TURN credentials are random, temporary, and limited
to one allocation per credential. For a public server behind NAT, configure
`--external-address` to the public address that TURN should advertise.

This does not make the relay anonymous. The rendezvous/TURN operator can see
client IP addresses, connection timing, room occupancy, packet sizes/volume,
and TURN allocation/peer routing metadata. The server does not receive app
usernames, PeerIds, or QUIC chat plaintext. A malicious rendezvous can deny
service or attempt traffic redirection; QUIC certificate identity validation
still prevents it from impersonating a peer, but first-contact PeerIds should
be verified out of band when identity assurance matters. TURN is a packet
forwarder, not an offline store-and-forward mailbox.

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

QUIC protects traffic between the two endpoints, including through a TURN
relay. A relay cannot read or change authenticated stream
contents, but it can observe endpoint addresses, connection IDs, timing, packet
sizes, and traffic volume, and can drop or delay packets. This transport does
not provide offline store-and-forward, message signatures, a double ratchet,
post-compromise recovery, group encryption, or anonymity. The SQLite chat
history is stored as plaintext on the local device. The current server does not
store chat messages beyond forwarding live ICE/QUIC packets. Durable offline
delivery would require a separate encrypted message envelope and delivery
protocol.
