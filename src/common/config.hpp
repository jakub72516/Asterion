#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/json.hpp"
#include "common/logging.hpp"
#include "common/result.hpp"

namespace ast {

enum class FlushMode { PerWrite, Batch, None };

enum class ConsistencyLevel { One, Quorum, All };

struct PeerSpec {
    uint64_t node_id = 0;
    std::string host;
    uint16_t port = 0;
};

struct StorageConfig {
    uint64_t segment_size_bytes = 64 * 1024 * 1024;
    FlushMode flush_mode = FlushMode::PerWrite;
    uint64_t batch_flush_interval_ms = 10;
    uint64_t index_max_entries = 1'000'000;
    bool compaction_enabled = true;
    uint64_t compaction_interval_s = 60;
    double compaction_threshold_ratio = 0.5;
};

struct NetworkConfig {
    std::string listen_host = "127.0.0.1";
    uint16_t listen_port = 7100;
    uint64_t max_connections = 128;
    uint64_t max_outstanding_io = 4096;
    uint64_t max_frame_size = 1 * 1024 * 1024;
    uint64_t recv_buffer_size = 64 * 1024;
    uint64_t send_buffer_size = 64 * 1024;
    uint64_t io_threads = 4;
    uint64_t worker_threads = 4;
    uint64_t connect_timeout_ms = 5000;
};

struct ReplicationConfig {
    uint64_t replication_factor = 3;
    uint64_t partition_count = 4;
    std::vector<PeerSpec> peers;
    ConsistencyLevel consistency_level = ConsistencyLevel::Quorum;
    uint64_t replication_queue_max_bytes = 64 * 1024 * 1024;
    uint64_t heartbeat_interval_ms = 500;
    uint64_t election_timeout_ms = 2000;
    uint64_t catchup_batch_size = 1024;
};

struct LimitsConfig {
    uint64_t max_concurrent_scans = 32;
    uint64_t max_key_size = 1024;
    uint64_t max_payload_size = 1 * 1024 * 1024;
    uint64_t max_scan_results = 100000;
};

struct Config {
    uint64_t node_id = 0;
    std::string data_dir;
    std::string log_level = "info";
    std::string log_file;

    StorageConfig storage;
    NetworkConfig network;
    ReplicationConfig replication;
    LimitsConfig limits;

    static Result<Config> load(const std::string& path);
    Error validate() const;

    // Resolve any relative paths against the config file directory.
    void resolve_paths(const std::string& config_dir);

    LogLevel parse_log_level() const;
    ConsistencyLevel parse_consistency(const std::string& s);
};

} // namespace ast
