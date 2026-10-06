#include "Session.h"

#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>
#include "Interface.h"

Session::Session(asio::ip::tcp::socket socket, uint8_t local_id,
    SessionCallbackHandler* callback, ConnectionDirection direction, Interface& interface)
    : socket(std::move(socket)), local_id(local_id), remote_id(0),
      session_callback(callback), direction(direction), interface(interface)
{
    if (!session_callback)
        throw std::invalid_argument("Session requires a callback handler");
}

void Session::start()
{
    if (closed) return;
    try { readLength(); }
    catch (const std::exception& e) { fail(std::string("Could not start session: ") + e.what()); }
    catch (...) { fail("Could not start session: unknown error"); }
}

void Session::initiateHandshake()
{
    if (closed) return;
    try { queueMessage({MessageType::Handshake, {static_cast<char>(local_id)}}); }
    catch (const std::exception& e) { fail(std::string("Could not send handshake: ") + e.what()); }
    catch (...) { fail("Could not send handshake: unknown error"); }
}

void Session::readLength()
{
    if (closed) return;
    auto self = shared_from_this();
    asio::async_read(socket, asio::buffer(message_length),
        [self](const asio::error_code& ec, std::size_t) {
            if (ec) { self->fail("Read failed: " + ec.message()); return; }
            try { self->readBody(); }
            catch (const std::exception& e) { self->fail(std::string("Read failed: ") + e.what()); }
            catch (...) { self->fail("Read failed: unknown error"); }
        });
}

void Session::readBody()
{
    if (closed) return;
    const uint32_t length =
        (static_cast<uint32_t>(static_cast<unsigned char>(message_length[0])) << 24) |
        (static_cast<uint32_t>(static_cast<unsigned char>(message_length[1])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(message_length[2])) << 8) |
         static_cast<uint32_t>(static_cast<unsigned char>(message_length[3]));
    if (length < 1 || length > MAX_MESSAGE_SIZE) {
        fail("Invalid message length");
        return;
    }

    message_body.resize(length);
    auto self = shared_from_this();
    asio::async_read(socket, asio::buffer(message_body),
        [self, length](const asio::error_code& ec, std::size_t) {
            if (ec) { self->fail("Read failed: " + ec.message()); return; }
            try {
                const auto raw_type = static_cast<uint8_t>(self->message_body[0]);
                Message msg{static_cast<MessageType>(raw_type),
                    std::vector<char>(self->message_body.begin() + 1, self->message_body.end())};
                self->processMessages(std::move(msg));
                if (!self->closed) self->readLength();
            } catch (const std::exception& e) {
                self->fail(std::string("Invalid protocol message: ") + e.what());
            } catch (...) { self->fail("Invalid protocol message: unknown error"); }
        });
}

void Session::processMessages(Message msg)
{
    if (closed) return;
    if (msg.type == MessageType::Handshake) {
        if (handshake_complete || msg.body.size() != 1) {
            fail("Invalid or repeated handshake"); return;
        }
        handleHandshake(static_cast<uint8_t>(msg.body[0]));
        return;
    }
    if (msg.type != MessageType::Chat) { fail("Unknown message type"); return; }
    if (!handshake_complete) { fail("Chat received before handshake"); return; }
    if (is_selected) session_callback->read(std::string(msg.body.begin(), msg.body.end()));
    else read_queue.push_back(std::move(msg));
}

void Session::flushWriteQueue()
{
    if (closed || write_queue.empty()) return;
    auto self = shared_from_this();
    asio::async_write(socket, asio::buffer(write_queue.front()),
        [self](const asio::error_code& ec, std::size_t) {
            if (ec) { self->fail("Write failed: " + ec.message()); return; }
            self->write_queue.pop_front();
            self->flushWriteQueue();
        });
}

void Session::queueMessage(Message msg)
{
    if (closed) throw std::runtime_error("Session is closed");
    auto encoded = serialize(msg);
    const bool writing = !write_queue.empty();
    write_queue.push_back(std::move(encoded));
    if (!writing) flushWriteQueue();
}

void Session::handleHandshake(uint8_t id)
{
    remote_id = id;
    handshake_complete = true;
    try { session_callback->onPeerIdentified(id, shared_from_this()); }
    catch (const std::exception& e) { fail(std::string("Handshake callback failed: ") + e.what()); }
    catch (...) { fail("Handshake callback failed"); }
}

void Session::selectPeer()
{
    if (closed) return;
    is_selected = true;
    try {
        for (const auto& msg : read_queue)
            if (msg.type == MessageType::Chat)
                session_callback->read(std::string(msg.body.begin(), msg.body.end()));
        read_queue.clear();
    } catch (const std::exception& e) { fail(std::string("Message callback failed: ") + e.what()); }
    catch (...) { fail("Message callback failed"); }
}

void Session::deselectPeer() { is_selected = false; }
ConnectionDirection Session::getDirection() const { return direction; }

void Session::fail(const std::string& reason) noexcept
{
    if (closed) return;
    const bool identified = handshake_complete;
    closed = true;
    asio::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    try {
        interface.printLineError("Session with peer " +
            (identified ? std::to_string(remote_id) : std::string("(unidentified)")) +
            " terminated: " + reason);
    } catch (...) {}
    if (identified) {
        try { session_callback->onPeerDisconnected(remote_id, shared_from_this()); }
        catch (...) {}
    }
}

void Session::handleDisconnect() { fail("Peer disconnected"); }

void Session::close()
{
    if (closed) return;
    closed = true;
    asio::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}
