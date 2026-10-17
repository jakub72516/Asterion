#include "core/cluster.hpp"

#include <algorithm>

#include "protocol/protocol.hpp"

namespace ast {

uint32_t partition_hash(uint64_t tenant, std::string_view key) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a 64
    auto mix = [&](uint8_t b) {
        h ^= b;
        h *= 1099511628211ULL;
    };
    for (int i = 0; i < 8; ++i) mix((uint8_t)((tenant >> (8 * i)) & 0xFF));
    for (char c : key) mix((uint8_t)c);
    h ^= h >> 32; // fold high bits into low for good low-bit distribution
    return (uint32_t)h;
}

namespace {
uint64_t required_acks(ConsistencyLevel cl, uint64_t rf) {
    switch (cl) {
    case ConsistencyLevel::One: return 1;
    case ConsistencyLevel::All: return rf;
    case ConsistencyLevel::Quorum: return rf / 2 + 1;
    }
    return 1;
}
} // namespace

Result<std::unique_ptr<Cluster>> Cluster::create(const Options& opts, Store& store) {
    auto c = std::unique_ptr<Cluster>(new Cluster());
    c->opts_ = opts;
    c->store_ = &store;

    for (auto& p : opts.peers) c->sorted_ids_.push_back(p.node_id);
    std::sort(c->sorted_ids_.begin(), c->sorted_ids_.end());
    c->sorted_ids_.erase(std::unique(c->sorted_ids_.begin(), c->sorted_ids_.end()),
                         c->sorted_ids_.end());
    c->eff_rf_ = std::min<uint64_t>(opts.replication_factor, c->sorted_ids_.size());
    if (c->eff_rf_ == 0) c->eff_rf_ = 1;
    return c;
}

Cluster::~Cluster() = default;

uint64_t Cluster::partition_for(uint64_t tenant, std::string_view key) const {
    if (opts_.partition_count == 0) return 0;
    return partition_hash(tenant, key) % opts_.partition_count;
}

std::vector<uint64_t> Cluster::replicas_for(uint64_t partition) const {
    std::vector<uint64_t> out;
    uint64_t n = sorted_ids_.size();
    if (n == 0) return out;
    uint64_t start = partition % n;
    for (uint64_t i = 0; i < eff_rf_; ++i) {
        out.push_back(sorted_ids_[(start + i) % n]);
    }
    return out;
}

uint64_t Cluster::leader_for(uint64_t partition) const {
    auto r = replicas_for(partition);
    return r.empty() ? 0 : r.front();
}

bool Cluster::is_leader(uint64_t partition) const {
    return leader_for(partition) == opts_.node_id;
}

bool Cluster::is_replica(uint64_t partition) const {
    for (uint64_t id : replicas_for(partition)) {
        if (id == opts_.node_id) return true;
    }
    return false;
}

Result<Client*> Cluster::peer_client(uint64_t node_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = clients_.find(node_id);
    if (it != clients_.end()) return it->second.get();

    PeerSpec spec;
    bool found = false;
    for (auto& p : opts_.peers) {
        if (p.node_id == node_id) { spec = p; found = true; break; }
    }
    if (!found) return Error(Err::NotFound, "unknown peer node " + std::to_string(node_id));

    Client::Config cc;
    cc.host = spec.host;
    cc.port = spec.port;
    cc.timeout_ms = opts_.connect_timeout_ms;
    cc.max_frame_size = opts_.max_frame_size;
    auto client = Client::connect(cc);
    if (!client.ok()) return client.error();

    auto up = std::make_unique<Client>(std::move(client.value()));
    Client* raw = up.get();
    clients_[node_id] = std::move(up);
    return raw;
}

Result<Store::WriteResult> Cluster::send_replicate(uint64_t node, uint64_t partition,
                                                   const EventRecord& rec) {
    auto c = peer_client(node);
    if (!c.ok()) return c.error();

    auto body = proto::encode_replicate_request((uint32_t)partition, rec);
    auto resp = c.value()->request(proto::ReplicateReq, body);
    if (!resp.ok()) return resp.error();
    if (resp->type != proto::ReplicateAck) return Error(Err::Internal, "unexpected replicate response");
    auto ack = proto::decode_replicate_ack(resp->body.data(), resp->body.size());
    if (!ack.ok()) return ack.error();
    if (*ack != 0) return Error(Err::Internal, "replica rejected event");

    Store::WriteResult wr;
    wr.duplicate = false;
    wr.sequence = rec.sequence;
    return wr;
}

Result<Store::WriteResult> Cluster::write(const EventRecord& rec, ConsistencyLevel cl) {
    uint64_t p = partition_for(rec.tenant_id, rec.key);
    if (!is_leader(p)) {
        return Error(Err::NotLeader,
                     "leader for partition " + std::to_string(p) + " is node " +
                         std::to_string(leader_for(p)));
    }

    auto wr = store_->write(rec);
    if (!wr.ok()) return wr;

    // Replicate with the leader-assigned sequence and timestamp so replicas
    // converge exactly.
    EventRecord repl = rec;
    repl.sequence = wr->sequence;
    repl.timestamp = wr->timestamp;

    uint64_t needed = required_acks(cl, eff_rf_);
    uint64_t acks = 1; // the leader itself

    auto replicas = replicas_for(p);
    for (uint64_t node : replicas) {
        if (node == opts_.node_id) continue;
        if (acks >= needed) break;
        auto r = send_replicate(node, p, repl);
        if (r.ok()) acks++;
    }

    if (acks < needed) {
        return Error(Err::QuorumUnavailable,
                     "replication acknowledged by " + std::to_string(acks) + "/" +
                         std::to_string(needed) + " replicas");
    }
    return wr;
}

Result<Store::WriteResult> Cluster::apply_replica(uint64_t partition, const EventRecord& rec) {
    if (!is_replica(partition)) {
        // Not our partition; ignore (safety net).
        Store::WriteResult wr;
        wr.duplicate = false;
        wr.sequence = rec.sequence;
        return wr;
    }
    return store_->write(rec);
}

uint64_t Cluster::sync_emit(uint64_t partition, const std::function<bool(const EventRecord&)>& emit) {
    uint64_t count = 0;
    store_->scan_all([&](const EventRecord& rec) {
        if (partition_for(rec.tenant_id, rec.key) != partition) return true;
        count++;
        return emit(rec);
    });
    return count;
}

Result<uint64_t> Cluster::sync_from(uint64_t partition) {
    uint64_t leader = leader_for(partition);
    if (leader == opts_.node_id) return Error(Err::InvalidArgument, "cannot sync from self");

    auto c = peer_client(leader);
    if (!c.ok()) return c.error();

    auto body = proto::encode_sync_request((uint32_t)partition);
    uint64_t applied = 0;
    auto r = c.value()->request_stream(proto::SyncReq, body, proto::SyncEnd,
                                       [&](const proto::Frame& f) {
        if (f.type == proto::SyncResp) {
            auto rec = proto::decode_sync_response(f.body.data(), f.body.size(),
                                                   opts_.max_key_size, opts_.max_payload_size);
            if (rec.ok()) {
                auto wr = apply_replica(partition, rec.value());
                if (wr.ok()) applied++;
            }
        }
        return true;
    });
    if (!r.ok()) return r.error();
    return applied;
}

} // namespace ast
