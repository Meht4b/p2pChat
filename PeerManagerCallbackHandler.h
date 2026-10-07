#pragma once

#include <asio/error_code.hpp>
#include <cstdint>
#include <string>

// Application-facing events emitted by PeerManager. Implementations choose how
// to present or otherwise handle these events; PeerManager never formats UI text.
class PeerManagerCallbackHandler
{
public:
    virtual ~PeerManagerCallbackHandler() = default;

    virtual void onPeerManagerStarted(uint16_t port, uint8_t local_id) = 0;
    virtual void onPeerManagerError(const asio::error_code& error) = 0;
    virtual void onPeerOperationError(const asio::error_code& error) = 0;
    virtual void onPeerConnection(bool incoming, const asio::error_code& error) = 0;
    virtual void onPeerIdentified(uint8_t peer_id) = 0;
    virtual void onDuplicatePeerConnection(uint8_t peer_id, bool new_connection_kept) = 0;
    virtual void onPeerDisconnected(uint8_t peer_id) = 0;
    virtual void onPeerSessionError(bool identified, uint8_t peer_id,
        const asio::error_code& error) = 0;
    virtual void onPeerMessage(uint8_t peer_id, const std::string& message) = 0;
    virtual void onLocalMessageSent(uint8_t local_id, const std::string& message) = 0;
};
