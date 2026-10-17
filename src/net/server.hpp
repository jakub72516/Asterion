#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/result.hpp"
#include "net/handler.hpp"

namespace ast {

// IOCP-based asynchronous TCP server.
//
// Design notes (see docs/architecture.md):
//  * A single listener thread performs blocking accept() and hands accepted
//    sockets to the completion port.
//  * io_threads run the completion port loop (recv/send completions) and
//    never perform blocking disk I/O.
//  * worker_threads pull decoded requests from a bounded queue and run the
//    RequestHandler (which may do disk I/O), so slow handlers do not stall the
//    completion port.
//  * All queues are bounded; overload is signaled by closing the offending
//    connection (documented backpressure), never by unbounded growth.
class Server {
public:
    struct Config {
        std::string host = "127.0.0.1";
        uint16_t port = 7100;
        uint64_t max_connections = 128;
        uint64_t max_frame_size = 1 * 1024 * 1024;
        uint64_t recv_buffer_size = 64 * 1024;
        uint64_t send_queue_max_bytes = 8 * 1024 * 1024;
        uint64_t io_threads = 4;
        uint64_t worker_threads = 4;
        uint64_t max_queued_requests = 8192;
    };

    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    Error start(const Config& cfg, RequestHandler* handler);
    void stop();   // graceful: stop accepting, drain, close connections
    void join();   // wait for all threads to exit

    uint64_t connections() const;
    uint64_t accepted() const;
    uint64_t rejected() const;
    uint64_t bytes_in() const;
    uint64_t bytes_out() const;
    uint64_t frames_handled() const;
    uint64_t queue_depth() const;

    struct Impl; // defined in server.cpp

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace ast
