# Transport security

Peer connections use mutual TLS 1.3 through OpenSSL. The TLS connection protects
all existing length-prefixed frames, including the peer ID, username, and chat
messages. The framing and chat APIs stay independent of the transport; a relay
can later forward the TLS byte stream without terminating or reading it.

Each installation creates an Ed25519 signing key and a self-signed X.509
certificate. The stable 256-bit peer ID is SHA-256 of the key's DER
SubjectPublicKeyInfo. TLS proves possession of the signing key, and the
application handshake rejects any peer ID that does not match the TLS
certificate key. Thus a peer cannot claim another ID by changing the plaintext
application handshake. TLS 1.3 negotiates ephemeral key agreement and
authenticated encryption using OpenSSL's maintained implementation.

On Windows, the private key is protected with DPAPI for the current Windows
user before it is stored in the existing SQLite settings database. The
certificate is public and stored alongside it. Losing the Windows account's
DPAPI material means the identity cannot be recovered; restoring the database
on another Windows account will require a new identity. The previous random
peer ID setting is retained as unused legacy data. The new public-key-derived
ID differs from that old ID, so peers will see this installation as a new
identity after upgrading.

This encrypts traffic between the two chat endpoints, including through a
future transparent TCP relay. It does not add offline relay storage, message
queues, a double ratchet, post-compromise recovery, group encryption, or
application-level message signatures. A malicious relay can still observe
network addresses, timing, connection sizes, and traffic volume, and can drop
or delay traffic. TLS provides forward secrecy for captured traffic after a
later compromise of the long-term signing key, but does not protect messages
captured from a live compromised endpoint. The current SQLite chat history is
also stored as plaintext on the local device. Verify peer IDs through a separate
trusted channel when the first contact itself is security-sensitive. A relay
that stores messages for delivery after a peer disconnects will need an
application-level encrypted message envelope; this stream encryption alone
does not support offline store-and-forward.
