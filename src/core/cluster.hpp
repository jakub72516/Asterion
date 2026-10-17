#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/config.hpp"
#include "core/store.hpp"
#include "net/client.hpp"

namespace ast {

// Deterministic hash of (tenant, key) used for partition assignment.
uint32_t partition_hash(uint64_t tenant, std::string_view key);

// Primary-copy replication with deterministic partition assignment.
//
// Partitions shard *write ownership*: for partition p the primary (leader) is
// the node at index (p mod N) in the sorted node list; the next (RF-1) nodes
// are followers. A node stores the union of the partitions it is a replica of
// (as leader or follower), so each event has RF replicas. See docs for the
// consistency/failure model (this is primary-backup, not a consensus protocol).
class Cluster {
public:
    struct Options {
        uint64_t node_id = 0;
        std::vector<PeerSpec> peers; // full membership including self
        uint64_t partition_count = 4;
        uint64_t replication_factor = 3;
        ConsistencyLevel default_consistency = ConsistencyLevel::Quorum;
        uint64_t connect_timeout_ms = 2000;
        uint64_t max_frame_size = 1 << 20;
        uint64_t max_key_size = 1024;
        uint64_t max_payload_size = 1 << 20;
    };

    static Result<std::unique_ptr<Cluster>> create(const Options& opts, Store& store);
    ~Cluster();

    uint64_t partition_for(uint64_t tenant, std::string_view key) const;
    uint64_t leader_for(uint64_t partition) const;
    bool is_leader(uint64_t partition) const;
    std::vector<uint64_t> replicas_for(uint64_t partition) const; // leader first
    bool is_replica(uint64_t partition) const;
    uint64_t effective_replication_factor() const { return eff_rf_; }

    // Write with replication for the given consistency level. Returns
    // Err::NotLeader (with the leader id) or Err::QuorumUnavailable honestly.
    Result<Store::WriteResult> write(const EventRecord& rec, ConsistencyLevel cl);

    // Apply an incoming replicated event for `partition` (no re-replication).
    Result<Store::WriteResult> apply_replica(uint64_t partition, const EventRecord& rec);

    // Stream a full sync of one partition (used to serve SyncReq).
    uint64_t sync_emit(uint64_t partition, const std::function<bool(const EventRecord&)>& emit);

    // Pull a full sync of `partition` from its leader (catch-up after loss).
    Result<uint64_t> sync_from(uint64_t partition);

    std::vector<uint64_t> peer_ids() const { return sorted_ids_; }

private:
    Cluster() = default;
    Result<Client*> peer_client(uint64_t node_id);
    Result<Store::WriteResult> send_replicate(uint64_t node, uint64_t partition,
                                              const EventRecord& rec);

    Options opts_;
    Store* store_ = nullptr;
    std::vector<uint64_t> sorted_ids_;
    uint64_t eff_rf_ = 1;
    std::mutex mu_;
    std::unordered_map<uint64_t, std::unique_ptr<Client>> clients_;
};

} // namespace ast
