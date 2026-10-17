#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "common/config.hpp"
#include "common/json.hpp"
#include "core/cluster.hpp"
#include "core/store.hpp"
#include "metrics/metrics.hpp"
#include "net/handler.hpp"
#include "net/server.hpp"

namespace ast {

// A database node: binds a Store (durability + index) to an IOCP Server via
// the request handler. Single-node in this release; replication plugs in at a
// higher layer (see core/cluster.hpp).
class Node : public RequestHandler {
public:
    static Result<std::unique_ptr<Node>> create(const Config& cfg);
    ~Node() override;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    Error start();
    void request_shutdown();
    void stop();
    void join();

    bool shutdown_requested() const { return shutdown_.load(); }
    const Config& config() const { return cfg_; }
    Store& store() { return *store_; }
    Server& server() { return server_; }
    Metrics& metrics() { return metrics_; }
    Cluster* cluster() { return cluster_.get(); }
    uint64_t started_at_us() const { return started_at_us_; }

    // RequestHandler implementation.
    void handle(uint8_t type, const uint8_t* body, size_t body_len,
                std::vector<uint8_t>& out_types, std::vector<uint16_t>& out_flags,
                std::vector<std::vector<uint8_t>>& out_bodies,
                const std::shared_ptr<std::atomic<bool>>& cancelled) override;

    // Status snapshot as JSON (for StatusReq and the CLI).
    std::string status_json() const;

private:
    Node() = default;
    Error init(const Config& cfg);

    Config cfg_;
    std::unique_ptr<Store> store_;
    std::unique_ptr<Cluster> cluster_;
    Server server_;
    Metrics metrics_;
    std::atomic<bool> shutdown_{false};
    uint64_t started_at_us_ = 0;
    std::thread compactor_;
    std::thread shutdown_deferrer_;
};

} // namespace ast
