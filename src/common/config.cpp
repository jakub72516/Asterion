#include "common/config.hpp"

#include <fstream>
#include <sstream>

namespace ast {

namespace {

std::string read_file(const std::string& path, Error& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = Error(Err::IoError, "cannot open config file: " + path);
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        err = Error(Err::IoError, "error reading config file: " + path);
        return {};
    }
    return ss.str();
}

FlushMode parse_flush(const std::string& s, bool& ok) {
    ok = true;
    if (s == "per_write") return FlushMode::PerWrite;
    if (s == "batch") return FlushMode::Batch;
    if (s == "none") return FlushMode::None;
    ok = false;
    return FlushMode::PerWrite;
}

} // namespace

Result<Config> Config::load(const std::string& path) {
    Error err;
    std::string text = read_file(path, err);
    if (!err.ok()) return err;

    auto parsed = parse_json(text);
    if (!parsed.ok()) {
        return Error(Err::InvalidArgument,
                     "config " + path + ": " + parsed.error().to_string());
    }
    const Json& root = parsed.value();
    if (!root.is_object()) return Error(Err::InvalidArgument, "config root must be an object");

    Config c;

    c.node_id = (uint64_t)root.get_int("node_id").value_or(0);
    c.data_dir = root.get_string("data_dir").value_or("data");
    c.log_level = root.get_string("log_level").value_or("info");
    c.log_file = root.get_string("log_file").value_or("");

    if (const Json* s = root.get("storage")) {
        auto& sc = c.storage;
        sc.segment_size_bytes = (uint64_t)s->get_int("segment_size_bytes").value_or((int64_t)sc.segment_size_bytes);
        std::string fm = s->get_string("flush_mode").value_or("per_write");
        bool okf = true;
        sc.flush_mode = parse_flush(fm, okf);
        if (!okf) return Error(Err::InvalidArgument, "storage.flush_mode invalid: " + fm);
        sc.batch_flush_interval_ms = (uint64_t)s->get_int("batch_flush_interval_ms").value_or((int64_t)sc.batch_flush_interval_ms);
        sc.index_max_entries = (uint64_t)s->get_int("index_max_entries").value_or((int64_t)sc.index_max_entries);
        sc.compaction_enabled = s->get_bool("compaction_enabled").value_or(sc.compaction_enabled);
        sc.compaction_interval_s = (uint64_t)s->get_int("compaction_interval_s").value_or((int64_t)sc.compaction_interval_s);
        sc.compaction_threshold_ratio = s->get_double("compaction_threshold_ratio").value_or(sc.compaction_threshold_ratio);
    }

    if (const Json* n = root.get("network")) {
        auto& nc = c.network;
        nc.listen_host = n->get_string("listen_host").value_or(nc.listen_host);
        nc.listen_port = (uint16_t)n->get_int("listen_port").value_or((int64_t)nc.listen_port);
        nc.max_connections = (uint64_t)n->get_int("max_connections").value_or((int64_t)nc.max_connections);
        nc.max_outstanding_io = (uint64_t)n->get_int("max_outstanding_io").value_or((int64_t)nc.max_outstanding_io);
        nc.max_frame_size = (uint64_t)n->get_int("max_frame_size").value_or((int64_t)nc.max_frame_size);
        nc.recv_buffer_size = (uint64_t)n->get_int("recv_buffer_size").value_or((int64_t)nc.recv_buffer_size);
        nc.send_buffer_size = (uint64_t)n->get_int("send_buffer_size").value_or((int64_t)nc.send_buffer_size);
        nc.io_threads = (uint64_t)n->get_int("io_threads").value_or((int64_t)nc.io_threads);
        nc.worker_threads = (uint64_t)n->get_int("worker_threads").value_or((int64_t)nc.worker_threads);
        nc.connect_timeout_ms = (uint64_t)n->get_int("connect_timeout_ms").value_or((int64_t)nc.connect_timeout_ms);
    }

    if (const Json* r = root.get("replication")) {
        auto& rc = c.replication;
        rc.replication_factor = (uint64_t)r->get_int("replication_factor").value_or((int64_t)rc.replication_factor);
        rc.partition_count = (uint64_t)r->get_int("partition_count").value_or((int64_t)rc.partition_count);
        std::string cl = r->get_string("consistency_level").value_or("quorum");
        c.replication.consistency_level = c.parse_consistency(cl);
        rc.heartbeat_interval_ms = (uint64_t)r->get_int("heartbeat_interval_ms").value_or((int64_t)rc.heartbeat_interval_ms);
        rc.election_timeout_ms = (uint64_t)r->get_int("election_timeout_ms").value_or((int64_t)rc.election_timeout_ms);
        rc.catchup_batch_size = (uint64_t)r->get_int("catchup_batch_size").value_or((int64_t)rc.catchup_batch_size);
        rc.replication_queue_max_bytes = (uint64_t)r->get_int("replication_queue_max_bytes").value_or((int64_t)rc.replication_queue_max_bytes);

        if (const Json* peers = r->get("peers")) {
            if (!peers->is_array()) return Error(Err::InvalidArgument, "replication.peers must be an array");
            for (const Json& p : peers->as_array()) {
                PeerSpec ps;
                ps.node_id = (uint64_t)p.get_int("node_id").value_or(0);
                ps.host = p.get_string("host").value_or("127.0.0.1");
                ps.port = (uint16_t)p.get_int("port").value_or(0);
                rc.peers.push_back(std::move(ps));
            }
        }
    }

    if (const Json* l = root.get("limits")) {
        auto& lc = c.limits;
        lc.max_concurrent_scans = (uint64_t)l->get_int("max_concurrent_scans").value_or((int64_t)lc.max_concurrent_scans);
        lc.max_key_size = (uint64_t)l->get_int("max_key_size").value_or((int64_t)lc.max_key_size);
        lc.max_payload_size = (uint64_t)l->get_int("max_payload_size").value_or((int64_t)lc.max_payload_size);
        lc.max_scan_results = (uint64_t)l->get_int("max_scan_results").value_or((int64_t)lc.max_scan_results);
    }

    Error verr = c.validate();
    if (!verr.ok()) return verr;

    return c;
}

Error Config::validate() const {
    if (node_id == 0) return Error(Err::InvalidArgument, "node_id must be >= 1");
    if (data_dir.empty()) return Error(Err::InvalidArgument, "data_dir must not be empty");
    if (storage.segment_size_bytes < 4096)
        return Error(Err::InvalidArgument, "segment_size_bytes must be >= 4096");
    if (storage.index_max_entries == 0)
        return Error(Err::InvalidArgument, "index_max_entries must be >= 1");
    if (storage.compaction_threshold_ratio <= 0.0 || storage.compaction_threshold_ratio >= 1.0)
        return Error(Err::InvalidArgument, "compaction_threshold_ratio must be in (0,1)");
    if (network.listen_port == 0)
        return Error(Err::InvalidArgument, "listen_port must be 1..65535");
    if (network.max_frame_size < 64)
        return Error(Err::InvalidArgument, "max_frame_size must be >= 64");
    if (network.max_frame_size > (1u << 30))
        return Error(Err::InvalidArgument, "max_frame_size must be <= 1 GiB");
    if (network.io_threads == 0)
        return Error(Err::InvalidArgument, "network.io_threads must be >= 1");
    if (replication.replication_factor < 1 || replication.replication_factor > 15)
        return Error(Err::InvalidArgument, "replication_factor must be in 1..15");
    if (replication.partition_count == 0)
        return Error(Err::InvalidArgument, "partition_count must be >= 1");
    if (limits.max_payload_size == 0)
        return Error(Err::InvalidArgument, "limits.max_payload_size must be >= 1");
    if (limits.max_key_size == 0)
        return Error(Err::InvalidArgument, "limits.max_key_size must be >= 1");

    // Duplicate peer ids are a config error.
    for (size_t i = 0; i < replication.peers.size(); ++i) {
        if (replication.peers[i].node_id == 0 || replication.peers[i].port == 0)
            return Error(Err::InvalidArgument, "peer entries need node_id and port");
        for (size_t j = i + 1; j < replication.peers.size(); ++j) {
            if (replication.peers[i].node_id == replication.peers[j].node_id)
                return Error(Err::InvalidArgument, "duplicate peer node_id");
        }
    }
    return ok();
}

LogLevel Config::parse_log_level() const {
    if (log_level == "trace") return LogLevel::Trace;
    if (log_level == "debug") return LogLevel::Debug;
    if (log_level == "info") return LogLevel::Info;
    if (log_level == "warn") return LogLevel::Warn;
    if (log_level == "error") return LogLevel::Error;
    if (log_level == "off") return LogLevel::Off;
    return LogLevel::Info;
}

ConsistencyLevel Config::parse_consistency(const std::string& s) {
    if (s == "one") return ConsistencyLevel::One;
    if (s == "all") return ConsistencyLevel::All;
    return ConsistencyLevel::Quorum;
}

void Config::resolve_paths(const std::string& config_dir) {
    auto is_abs = [](const std::string& p) {
        if (p.empty()) return false;
        return p[0] == '\\' || p[0] == '/' || (p.size() > 2 && p[1] == ':');
    };
    auto join = [&](const std::string& p) {
        if (is_abs(p) || config_dir.empty()) return p;
        if (config_dir.back() == '\\' || config_dir.back() == '/')
            return config_dir + p;
        return config_dir + "\\" + p;
    };
    data_dir = join(data_dir);
    if (!log_file.empty()) log_file = join(log_file);
}

} // namespace ast
