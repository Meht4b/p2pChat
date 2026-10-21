#pragma once

#include <asio/error_code.hpp>
#include <cstdint>
#include <string>

#include "PeerId.h"
#include "ConnectionPath.h"

// Application-facing events emitted by PeerManager. Implementations choose how
// to present or otherwise handle these events; PeerManager never formats UI text.
class PeerManagerCallbackHandler
{
public:
    virtual ~PeerManagerCallbackHandler() = default;

    virtual void onPeerManagerStarted(uint16_t port, const PeerInfo& local) = 0;
    virtual void onPeerManagerError(const asio::error_code& error) = 0;
    virtual void onPeerOperationError(const asio::error_code& error) = 0;
    virtual void onPeerRoomError(const std::string& message)
    {
        (void)message;
        onPeerOperationError(asio::error::make_error_code(asio::error::fault));
    }
    virtual void onPeerConnection(bool incoming, const asio::error_code& error) = 0;

    // handshake finished; `peer.username` is what the peer announced about itself
    virtual void onPeerIdentified(const PeerInfo& peer) = 0;
    virtual void onPeerIdentified(const PeerInfo& peer, ConnectionPath path)
    {
        (void)path;
        onPeerIdentified(peer);
    }
    virtual void onPeerConnectionPathChanged(const PeerId& peer_id, ConnectionPath path)
    {
        (void)peer_id;
        (void)path;
    }
    virtual void onDuplicatePeerConnection(const PeerId& peer_id, bool new_connection_kept) = 0;
    virtual void onPeerDisconnected(const PeerId& peer_id) = 0;

    // peer_id is all zeros when the session never identified itself
    virtual void onPeerSessionError(bool identified, const PeerId& peer_id,
        const asio::error_code& error) = 0;
    virtual void onPeerMessage(const PeerId& peer_id, const std::string& message) = 0;

    // `peer_id` is the peer the message was sent to
    virtual void onLocalMessageSent(const PeerId& peer_id, const std::string& message) = 0;
};
