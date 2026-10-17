#pragma once

#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

#include "storage/log.hpp"

namespace ast {

// Composite key for the index: (tenant_id, key).
struct IndexKey {
    uint64_t tenant = 0;
    std::string key;

    bool operator==(const IndexKey& o) const {
        return tenant == o.tenant && key == o.key;
    }
};

struct IndexKeyHash {
    size_t operator()(const IndexKey& k) const {
        size_t h = std::hash<uint64_t>{}(k.tenant);
        h ^= std::hash<std::string>{}(k.key) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// Per-key index entry: the latest event and where to find it.
struct KeyIndexEntry {
    uint64_t latest_seq = 0;
    LogPos pos;
    bool deleted = false;     // whether the latest event is a tombstone
    uint64_t timestamp = 0;
};

// A bounded-memory LRU index from (tenant, key) -> latest event location.
// Memory is O(index_max_entries), independent of total stored data. Evicted
// keys are re-resolved by a backward log scan at query time (correct, slower).
class KeyIndex {
public:
    KeyIndex() = default;
    explicit KeyIndex(uint64_t max_entries) : max_entries_(max_entries) {}

    void put(const IndexKey& key, const KeyIndexEntry& entry);
    std::optional<KeyIndexEntry> get(const IndexKey& key); // touches LRU on hit
    bool contains(const IndexKey& key) const;
    void remove(const IndexKey& key);
    void clear();

    uint64_t size() const { return map_.size(); }
    uint64_t max_entries() const { return max_entries_; }

    // Stats (reset by reset_stats()).
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    uint64_t evictions() const { return evictions_; }
    void reset_stats() { hits_ = misses_ = evictions_ = 0; }

private:
    using List = std::list<std::pair<IndexKey, KeyIndexEntry>>;
    using Map = std::unordered_map<IndexKey, List::iterator, IndexKeyHash>;

    void evict_one();

    uint64_t max_entries_ = 0;
    List lru_;
    Map map_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    uint64_t evictions_ = 0;
};

} // namespace ast
