#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace ast {

// A connection-scoped request handler. The server decodes frames and calls
// handle() on a worker thread (off the IOCP completion threads) so disk I/O
// never blocks completion processing.
//
// `cancelled` is shared with the connection; when the client disconnects it is
// set to true so a long-running scan can stop early. It is a shared_ptr so the
// reference stays valid even after the connection object is destroyed.
class RequestHandler {
public:
    virtual ~RequestHandler() = default;

    virtual void handle(uint8_t type, const uint8_t* body, size_t body_len,
                        std::vector<uint8_t>& out_frame_types,
                        std::vector<uint16_t>& out_frame_flags,
                        std::vector<std::vector<uint8_t>>& out_frame_bodies,
                        const std::shared_ptr<std::atomic<bool>>& cancelled) = 0;
};

} // namespace ast
