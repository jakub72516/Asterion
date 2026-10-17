#include "core/node.hpp"

#include <chrono>
#include <map>

#include "common/clock.hpp"
#include "common/platform/file.hpp"
#include "protocol/protocol.hpp"

namespace ast {

Result<std::unique_ptr<Node>> Node::create(const Config& cfg) {
    auto node = std::unique_ptr<Node>(new Node());
    auto e = node->init(cfg);
    if (!e.ok()) return e;
    return node;
}

Node::~Node() {
    stop();
    join();
}

Error Node::init(const Config& cfg) {
    cfg_ = cfg;

    Error e = make_dirs(cfg_.data_dir);
    if (!e.ok()) return e;

    Store::Options so;
    so.log.dir = cfg_.data_dir;
    so.log.segment_size = cfg_.storage.segment_size_bytes;
    so.log.flush_mode = cfg_.storage.flush_mode;
    so.log.batch_flush_interval_ms = cfg_.storage.batch_flush_interval_ms;
    so.log.max_key_size = cfg_.limits.max_key_size;
    so.log.max_payload_size = cfg_.limits.max_payload_size;
    so.index_max_entries = cfg_.storage.index_max_entries;
    so.max_key_size = cfg_.limits.max_key_size;
    so.max_payload_size = cfg_.limits.max_payload_size;

    RecoveryInfo ri;
    auto store = Store::open(so, &ri);
    if (!store.ok()) return store.error();

    if (!ri.issues.empty()) {
        for (auto& s : ri.issues) AST_LOG_WARN("recovery: " + s);
    }
    AST_LOG_INFO("recovered " + std::to_string(ri.records_recovered) + " record(s) in " +
                 std::to_string(ri.segments_opened) + " segment(s)");

    store_ = std::move(store.value());

    // Set up replication if a peer set is configured.
    if (!cfg_.replication.peers.empty()) {
        Cluster::Options co;
        co.node_id = cfg_.node_id;
        co.peers = cfg_.replication.peers;
        co.partition_count = cfg_.replication.partition_count;
        co.replication_factor = cfg_.replication.replication_factor;
        co.default_consistency = cfg_.replication.consistency_level;
        co.connect_timeout_ms = cfg_.network.connect_timeout_ms;
        co.max_frame_size = cfg_.network.max_frame_size;
        co.max_key_size = cfg_.limits.max_key_size;
        co.max_payload_size = cfg_.limits.max_payload_size;
        auto cluster = Cluster::create(co, *store_);
        if (!cluster.ok()) return cluster.error();
        cluster_ = std::move(cluster.value());
    }
    return ok();
}

Error Node::start() {
    Server::Config sc;
    sc.host = cfg_.network.listen_host;
    sc.port = cfg_.network.listen_port;
    sc.max_connections = cfg_.network.max_connections;
    sc.max_frame_size = cfg_.network.max_frame_size;
    sc.recv_buffer_size = cfg_.network.recv_buffer_size;
    sc.io_threads = cfg_.network.io_threads;
    sc.worker_threads = cfg_.network.worker_threads;
    sc.max_queued_requests = cfg_.network.max_outstanding_io;

    started_at_us_ = wall_time_us();
    auto e = server_.start(sc, this);
    if (!e.ok()) return e;

    if (cfg_.storage.compaction_enabled) {
        compactor_ = std::thread([this] {
            uint64_t interval_s = cfg_.storage.compaction_interval_s ? cfg_.storage.compaction_interval_s : 60;
            while (!shutdown_.load()) {
                for (uint64_t i = 0; i < interval_s && !shutdown_.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                if (shutdown_.load()) break;
                auto st = store_->compact(0);
                if (st.ok() && !st->nothing_to_do) {
                    AST_LOG_INFO("compaction merged " + std::to_string(st->segments_merged) +
                                 " segment(s), " + std::to_string(st->records_in) + " -> " +
                                 std::to_string(st->records_out) + " records");
                }
            }
        });
    }
    return ok();
}

void Node::request_shutdown() {
    if (shutdown_.exchange(true)) return;
    server_.stop();
}

void Node::stop() {
    server_.stop();
    shutdown_.store(true);
    if (compactor_.joinable()) compactor_.join();
    if (shutdown_deferrer_.joinable()) shutdown_deferrer_.join();
    if (store_) store_->close();
}

void Node::join() {
    server_.join();
}

namespace {
void append_event_body(std::vector<std::vector<uint8_t>>& out_bodies, const EventRecord& rec) {
    auto body = encode_record_body(rec);
    out_bodies.emplace_back(body->data(), body->data() + body->size());
}
} // namespace

void Node::handle(uint8_t type, const uint8_t* body, size_t body_len,
                  std::vector<uint8_t>& out_types, std::vector<uint16_t>& out_flags,
                  std::vector<std::vector<uint8_t>>& out_bodies,
                  const std::shared_ptr<std::atomic<bool>>& cancelled) {
    uint64_t start = SteadyClock::instance().now_ns();

    switch (type) {
    case proto::Ping: {
        out_types.push_back(proto::Pong);
        out_flags.push_back(0);
        out_bodies.emplace_back();
        break;
    }

    case proto::StatusReq: {
        out_types.push_back(proto::StatusResp);
        out_flags.push_back(0);
        std::string js = status_json();
        out_bodies.emplace_back(js.begin(), js.end());
        break;
    }

    case proto::AppendReq:
    case proto::DeleteReq: {
        auto rec = proto::decode_append_request(body, body_len, cfg_.limits.max_key_size,
                                                cfg_.limits.max_payload_size);
        if (!rec.ok()) {
            auto err = proto::encode_error_response((uint8_t)rec.code(), rec.error().message);
            out_types.push_back(proto::ErrorResp);
            out_flags.push_back(0);
            out_bodies.push_back(std::move(err.vec()));
            metrics_.errors++;
            break;
        }
        if (type == proto::DeleteReq) rec->deleted = true;
        if (rec->deleted) rec->payload.clear();

        Result<Store::WriteResult> wr = cluster_
            ? cluster_->write(*rec, cfg_.replication.consistency_level)
            : store_->write(*rec);
        metrics_.appends++;
        if (wr.ok()) metrics_.bytes_written += rec->payload.size();

        proto::AppendResponse resp;
        if (wr.ok()) {
            resp.code = wr->duplicate ? 1 : 0;
            resp.sequence = wr->sequence;
        } else {
            resp.code = (uint8_t)wr.code();
            resp.sequence = 0;
            metrics_.errors++;
        }
        auto resp_body = proto::encode_append_response(resp);
        out_types.push_back(proto::AppendResp);
        out_flags.push_back(0);
        out_bodies.push_back(std::move(resp_body.vec()));
        break;
    }

    case proto::ReplicateReq: {
        auto req = proto::decode_replicate_request(body, body_len, cfg_.limits.max_key_size,
                                                   cfg_.limits.max_payload_size);
        if (!req.ok() || !cluster_) {
            auto ack = proto::encode_replicate_ack(0, 1);
            out_types.push_back(proto::ReplicateAck);
            out_flags.push_back(0);
            out_bodies.push_back(std::move(ack.vec()));
            metrics_.errors++;
            break;
        }
        uint32_t partition = req->first;
        auto wr = cluster_->apply_replica(partition, req->second);
        uint8_t okflag = wr.ok() ? 0 : 1;
        auto ack = proto::encode_replicate_ack(partition, okflag);
        out_types.push_back(proto::ReplicateAck);
        out_flags.push_back(0);
        out_bodies.push_back(std::move(ack.vec()));
        break;
    }

    case proto::SyncReq: {
        auto req = proto::decode_sync_request(body, body_len);
        if (!req.ok() || !cluster_) {
            auto err = proto::encode_error_response((uint8_t)Err::InvalidArgument, "bad sync request");
            out_types.push_back(proto::ErrorResp);
            out_flags.push_back(0);
            out_bodies.push_back(std::move(err.vec()));
            metrics_.errors++;
            break;
        }
        uint32_t partition = *req;
        uint32_t count = (uint32_t)cluster_->sync_emit(partition, [&](const EventRecord& rec) {
            append_event_body(out_bodies, rec);
            out_types.push_back(proto::SyncResp);
            out_flags.push_back(0);
            return true;
        });
        auto end = proto::encode_sync_end(count);
        out_types.push_back(proto::SyncEnd);
        out_flags.push_back(0);
        out_bodies.push_back(std::move(end.vec()));
        break;
    }

    case proto::GetReq: {
        auto req = proto::decode_get_request(body, body_len, cfg_.limits.max_key_size);
        if (!req.ok()) {
            auto err = proto::encode_error_response((uint8_t)req.code(), req.error().message);
            out_types.push_back(proto::ErrorResp);
            out_flags.push_back(0);
            out_bodies.push_back(std::move(err.vec()));
            metrics_.errors++;
            break;
        }
        auto got = store_->get(req->tenant, req->key);
        metrics_.reads++;
        uint8_t code = 0;
        std::vector<uint8_t> resp_body;
        if (!got.ok()) {
            code = 2; // not found / error -> empty
            metrics_.errors++;
        } else if (!got->has_value()) {
            code = 1; // not found
        } else if (got->value().deleted) {
            code = 2; // tombstone
        } else {
            code = 0;
            auto ev_body = encode_record_body(got->value());
            resp_body.insert(resp_body.end(), ev_body->data(), ev_body->data() + ev_body->size());
        }
        Buffer r;
        r.append_byte(code);
        r.append(resp_body.data(), resp_body.size());
        out_types.push_back(proto::GetResp);
        out_flags.push_back(0);
        out_bodies.push_back(std::move(r.vec()));
        break;
    }

    case proto::ScanReq: {
        auto req = proto::decode_scan_request(body, body_len);
        if (!req.ok()) {
            auto err = proto::encode_error_response((uint8_t)req.code(), req.error().message);
            out_types.push_back(proto::ErrorResp);
            out_flags.push_back(0);
            out_bodies.push_back(std::move(err.vec()));
            metrics_.errors++;
            break;
        }
        metrics_.scans++;

        ScanControl ctrl;
        ctrl.max_results = req->max_results ? req->max_results : 1000;
        ctrl.cancelled = [&] { return cancelled && cancelled->load(); };

        auto e = store_->scan_tenant(req->tenant, req->from_ts, req->to_ts,
                                     [&](const EventRecord& rec) {
                                         append_event_body(out_bodies, rec);
                                         out_types.push_back(proto::ScanResp);
                                         out_flags.push_back(proto::kFlagMore);
                                         return true;
                                     },
                                     &ctrl);

        uint8_t reason = 0;
        if (e.ok()) {
            if (cancelled && cancelled->load()) reason = 2;
            else if (ctrl.emitted >= ctrl.max_results) reason = 1;
            else reason = 0;
        } else {
            reason = 2;
        }
        auto end_body = proto::encode_scan_end(reason);
        out_types.push_back(proto::ScanEnd);
        out_flags.push_back(0);
        out_bodies.push_back(std::move(end_body.vec()));
        break;
    }

    case proto::ShutdownReq: {
        auto body_buf = proto::encode_status_response("ok");
        out_types.push_back(proto::ShutdownResp);
        out_flags.push_back(0);
        out_bodies.push_back(std::move(body_buf.vec()));
        // Defer the actual stop so the ShutdownResp can be flushed to the client.
        if (!shutdown_deferrer_.joinable()) {
            shutdown_deferrer_ = std::thread([this] {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                request_shutdown();
            });
        }
        break;
    }

    default: {
        auto err = proto::encode_error_response((uint8_t)Err::Unsupported, "unknown message type");
        out_types.push_back(proto::ErrorResp);
        out_flags.push_back(0);
        out_bodies.push_back(std::move(err.vec()));
        metrics_.errors++;
        break;
    }
    }

    uint64_t end = SteadyClock::instance().now_ns();
    metrics_.record_latency((end - start) / 1000);
}

std::string Node::status_json() const {
    std::map<std::string, Json> o;
    o["node_id"] = Json((int64_t)cfg_.node_id);
    o["uptime_s"] = Json((int64_t)((wall_time_us() - started_at_us_) / 1000000));
    o["data_dir"] = Json(cfg_.data_dir);

    std::map<std::string, Json> store;
    store["total_records"] = Json((int64_t)store_->total_records());
    store["index_entries"] = Json((int64_t)store_->index_size());
    store["index_max"] = Json((int64_t)store_->index_max());

    std::map<std::string, Json> srv;
    srv["connections"] = Json((int64_t)server_.connections());
    srv["accepted"] = Json((int64_t)server_.accepted());
    srv["rejected"] = Json((int64_t)server_.rejected());
    srv["bytes_in"] = Json((int64_t)server_.bytes_in());
    srv["bytes_out"] = Json((int64_t)server_.bytes_out());
    srv["frames_handled"] = Json((int64_t)server_.frames_handled());
    srv["queue_depth"] = Json((int64_t)server_.queue_depth());

    std::map<std::string, Json> disk;
    disk["free_bytes"] = Json((int64_t)disk_free_bytes(cfg_.data_dir));
    disk["total_bytes"] = Json((int64_t)disk_total_bytes(cfg_.data_dir));

    o["store"] = Json(std::move(store));
    o["server"] = Json(std::move(srv));
    o["disk"] = Json(std::move(disk));
    o["metrics"] = metrics_.to_json();

    if (cluster_) {
        std::map<std::string, Json> cl;
        cl["replication_factor"] = Json((int64_t)cluster_->effective_replication_factor());
        std::vector<Json> peers;
        for (auto id : cluster_->peer_ids()) peers.push_back(Json((int64_t)id));
        cl["peers"] = Json(std::move(peers));
        o["cluster"] = Json(std::move(cl));
    }
    return Json(std::move(o)).dump();
}

} // namespace ast
