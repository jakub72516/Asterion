#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/result.hpp"
#include "protocol/protocol.hpp"

namespace ast {

// Blocking TCP client used by the CLI and by replication. Performs full-frame
// reads and enforces the same max_frame_size limit as the server.
class Client {
public:
    struct Config {
        std::string host = "127.0.0.1";
        uint16_t port = 7100;
        uint64_t max_frame_size = 1 * 1024 * 1024;
        uint64_t timeout_ms = 5000;
    };

    Client() = default;
    ~Client() { close(); }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&& o) noexcept;
    Client& operator=(Client&& o) noexcept;

    static Result<Client> connect(const Config& cfg);
    void close();
    bool is_open() const { return sock_ != kInvalidSocket; }

    // Send a request and read exactly one response frame.
    Result<proto::Frame> request(uint8_t type, const Buffer& body);
    Result<proto::Frame> request(uint8_t type, const std::vector<uint8_t>& body);

    // Send a request and read frames until a terminal frame of type `end_type`
    // arrives; invokes `on_frame` for every frame (including the terminal one).
    Result<size_t> request_stream(uint8_t type, const Buffer& body, uint8_t end_type,
                                  const std::function<bool(const proto::Frame&)>& on_frame);

private:
    Error send_all(const uint8_t* data, size_t len);
    Error read_exact(uint8_t* dst, size_t len);
    Result<proto::Frame> read_frame();

    static constexpr uintptr_t kInvalidSocket = ~(uintptr_t)0;
    uintptr_t sock_ = kInvalidSocket; // SOCKET (INVALID_SOCKET)
    uint64_t max_frame_size_ = 1 * 1024 * 1024;
    uint64_t timeout_ms_ = 5000;
};

} // namespace ast
