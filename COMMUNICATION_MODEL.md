# P2P Chat Communication Model

## Purpose and scope

This document describes how the current chat application communicates after
the session transport moved to QUIC over UDP. It covers the components, packet
and message paths, connection setup, peer identity checks, idle behavior, and
the limits of the current design. It also identifies where ICE would attach
later. ICE, STUN candidate gathering, rendezvous signaling, hole punching, and
TURN relay are not implemented in the current application.

## The layers

Communication is split into four layers:

1. **PeerManager** owns the listening UDP socket, starts outgoing sessions,
   accepts incoming QUIC Initial packets, and dispatches packets to sessions.
2. **Session** owns one peer connection's ngtcp2 QUIC state and OpenSSL TLS
   state. It turns application messages into QUIC stream bytes and turns
   received stream bytes back into application messages.
3. **QUIC and TLS 1.3** provide encrypted and authenticated transport,
   retransmission, ordering, flow control, connection IDs, and keep-alive
   scheduling. The implementation uses ngtcp2 with its OpenSSL QUIC backend.
4. **The application protocol** identifies the peer and carries chat messages
   using the project's existing length-prefixed frame format.

The `Session` does not create or own its UDP socket. It is given the shared
socket, its current remote endpoint, and callbacks for sending datagrams and
registering connection IDs. That keeps the peer-session logic separate from
the code that delivers UDP packets.

## Startup and listening

At startup, the application loads or creates its cryptographic identity and
creates a TLS context from that identity. `PeerManager` binds one UDP socket
to the configured local port, begins an asynchronous receive, and retains the
TLS context for new sessions. The receive buffer is sized for a UDP datagram;
on each successful receive, the manager copies the bytes and source endpoint,
re-arms the receive, and then processes the copied packet. Re-arming allows
the socket to continue receiving while a session handles the current packet.

There is one bound socket per `PeerManager`, shared among that manager's peer
connections. QUIC connection IDs allow multiple sessions to use that socket
and local port without opening a separate listener for each peer.

## Outgoing connection setup

The current user-facing connection operation supplies a numeric IP address
and UDP port. `PeerManager::connect` validates the address, checks that its
address family matches the bound socket, and constructs a `Session` with the
destination endpoint. DNS-based discovery and exchange of peer candidates are
not part of this path.

The outgoing session then:

1. Creates an ngtcp2 client connection with a random source connection ID and
   random destination connection ID.
2. Registers its local connection ID with `PeerManager`, so future datagrams
   directed to that ID can be routed back to this session.
3. Creates an OpenSSL TLS session from the shared TLS context, configures the
   ngtcp2 OpenSSL QUIC integration, and advertises the ALPN protocol
   `p2pchat/1`.
4. Lets ngtcp2 produce the first QUIC Initial packet and sends that packet
   through the manager's UDP send callback.

The manager serializes UDP sends through a queue. This keeps the packet buffer
alive until the asynchronous send completes and avoids overlapping sends that
would invalidate the front queue entry.

## Incoming connection setup and packet routing

For each received datagram, `PeerManager` decodes enough of the QUIC header to
read the destination connection ID. If that ID is registered, the datagram
is passed to the matching `Session`. A session can register additional IDs
when ngtcp2 asks it to create them. The server session also registers the
client's original Initial destination ID so retransmitted Initial packets
reach the existing session instead of being mistaken for a second incoming
connection.

If no registered connection ID matches, the manager checks whether the packet
is a valid QUIC Initial. Non-QUIC and malformed packets are ignored. A valid
Initial creates an incoming `Session`, which initializes an ngtcp2 server
connection and consumes that first packet. Later packets are delivered by the
connection-ID map just like packets for outgoing sessions.

When a session terminates, its active and pending entries and all connection
ID routes are removed. Stopping the manager closes its socket and closes all
sessions it owns.

## TLS, encryption, and peer identity

Each installation has an Ed25519 private key and a self-signed X.509
certificate containing the corresponding public key. The stable peer ID is
the SHA-256 hash of the DER-encoded SubjectPublicKeyInfo for that key. TLS 1.3
uses the certificate during the QUIC handshake to prove that the other side
possesses the corresponding private key.

The application does not rely on a public certificate authority. It accepts
the self-signed certificate at the TLS verification-callback stage, then
performs its own identity check: it derives the peer ID from the TLS peer
certificate and compares that ID with the ID claimed by the application
handshake. A mismatch or missing certificate fails the session. This binds
the claimed chat identity to the key proven during TLS, but does not establish
that a first-contact key belongs to the person the user expects. First-contact
identity still needs an independent trusted comparison when impersonation
matters.

ALPN `p2pchat/1` identifies this application protocol during TLS negotiation.
TLS 1.3 and QUIC derive and use transport keys to encrypt and authenticate
QUIC packets. The chat payload is protected by that transport; the application
does not add a second message-encryption envelope. An on-path observer can
still see network addresses, packet timing, packet sizes, and traffic volume.

## Application handshake and message framing

After the QUIC/TLS handshake completes, each peer opens a bidirectional QUIC
stream and sends its application handshake on that stream. Each side uses its
own opened stream for its outgoing handshake and chat messages, while reading
the stream opened by the other side. This gives each direction an independent
reliable, ordered byte stream.

Each application frame has this layout:

| Field | Size | Meaning |
|---|---:|---|
| Length | 4 bytes | Unsigned, big-endian length of the message type plus body |
| Type | 1 byte | `Handshake` or `Chat` |
| Body | Variable | Type-specific payload |

The type-plus-body length must be from 1 through 65,536 bytes. A chat body can
therefore be up to 65,535 bytes. The four-byte prefix is outside that limit.
QUIC stream boundaries do not replace this frame format: a frame can arrive
in several QUIC callbacks, and a callback can contain parts of more than one
frame. `Session` buffers stream data until a complete frame is available.

The handshake body is a 32-byte peer ID followed by a username of 1 to 32
bytes. The receiver validates the body size and nonzero peer ID, sanitizes the
username, and then compares the claimed peer ID against the identity derived
from the TLS certificate. Only after that check does the session notify
`PeerManager` that the peer is identified. The callback is posted onto the
event loop instead of being run inside ngtcp2's packet-processing callback;
this avoids re-entering manager/session state while ngtcp2 is still processing
a packet.

Before the application handshake is complete, a chat message is invalid. A
second handshake is invalid as well. Invalid frame lengths, unknown message
types, unexpected streams, or out-of-order stream offsets terminate the
session rather than being treated as chat data.

## Chat sending, acknowledgement, and receiving

When the user sends a chat message, `PeerManager` finds the currently selected
peer and posts the write to that session's event loop. `Session` serializes
the message into a frame and queues its bytes. It passes available bytes to
ngtcp2, which packetizes them into QUIC datagrams. The manager sends those
datagrams over UDP.

The session retains outgoing frame storage while ngtcp2 may still reference
it. QUIC can acknowledge a frame in ranges, including while a large frame is
still being submitted. The session tracks acknowledged byte ranges and only
releases a frame after every byte in it has been acknowledged. Loss recovery
and retransmission are handled by QUIC; the chat application does not
implement its own acknowledgement or retransmission protocol.

On receive, QUIC verifies packet authentication and reassembles ordered stream
bytes. The session checks offsets, appends bytes to its frame buffer, extracts
complete frames, and dispatches them. If this peer is selected in the UI,
chat data is delivered immediately. If it is not selected, messages are queued
in memory until selection changes. The application persists received chat
messages through its existing storage path.

## Idle connection behavior

The transport parameters advertise a 30-second QUIC idle timeout. The session
also configures ngtcp2 to send a QUIC keep-alive PING after 10 seconds with no
traffic. The existing expiry timer wakes at ngtcp2's next deadline; after
handling the expiry, the session asks ngtcp2 to produce output and sends the
resulting PING through the UDP sender callback. Any subsequent traffic resets
the idle period.

This is intended to keep a quiet live chat and its UDP mapping active. It is
not a reconnect system: if the peer exits, becomes unreachable, or the
network drops packets long enough for QUIC failure detection, the session can
still terminate. A firewall or NAT can also discard a mapping despite the
PINGs. On room connections, libjuice performs ICE connectivity checks and
refreshes TURN allocations independently; QUIC keep-alive does not replace
those ICE responsibilities.

## Connectivity modes and ICE boundary

`/connect <local-ip> <port>` retains a manually addressed direct QUIC path.
`/connect <room-token> <relay-host[:port]>` uses a separate rendezvous/TURN
service. Clients derive an opaque room key from their shared high-entropy token;
the signaling server pairs at most two clients in that room and forwards
authenticated-encrypted ICE descriptions. libjuice gathers candidates, checks
paths and selects direct UDP where possible, falling back to the server's TURN
packet relay when necessary. Only after ICE reports a selected path does the
app create a QUIC session.

`Session` remains transport-independent: it receives QUIC datagrams and emits
them through callbacks. The direct mode uses `PeerManager`'s UDP socket; room
mode sends and receives those datagrams through libjuice. QUIC TLS and the
key-bound peer identity remain the end-to-end authentication and encryption
layer in both modes. TURN forwards packets and does not terminate QUIC. The
ICE agent remains alive for the whole QUIC session and all room-path packets
use `juice_send`; that lets libjuice keep ICE checks/consent alive and change
the selected candidate pair when it finds a better path, including moving
from TURN to direct UDP after QUIC starts.

The signaling service sees connecting IP addresses, timing and room occupancy,
as well as TURN allocation and packet-routing metadata. It receives no app
PeerIds, usernames or QUIC plaintext. This is not anonymity: the shared room
token must be secret, and the server can deny service or delay/drop traffic.
There is no separate app-level QUIC migration controller: path selection and
later candidate-pair switches are owned by libjuice below the QUIC session.
Identity verification out of band is still recommended on first contact.

The implementation uses [libjuice](https://github.com/paullouisageneau/libjuice)
for ICE/STUN/TURN, [RFC 8445 (ICE)](https://www.rfc-editor.org/rfc/rfc8445),
and [RFC 8656 (TURN)](https://www.rfc-editor.org/rfc/rfc8656).

## Build and runtime constraints

The current QUIC backend uses ngtcp2's OpenSSL TLS backend and requires
OpenSSL 3.5 or newer for the QUIC TLS APIs. The backend is marked experimental
by ngtcp2, so dependency upgrades should be reviewed and tested. The portable
identity key is stored unencrypted in the SQLite settings database; someone
who can read that database can impersonate the installation. Local chat
history is also plaintext in SQLite.
