#include "storage/key_index.hpp"

namespace ast {

void KeyIndex::put(const IndexKey& key, const KeyIndexEntry& entry) {
    auto it = map_.find(key);
    if (it != map_.end()) {
        // Update existing entry and move to front (most-recently-used).
        it->second->second = entry;
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    if (map_.size() >= max_entries_) {
        evict_one();
    }
    lru_.emplace_front(key, entry);
    map_[key] = lru_.begin();
}

std::optional<KeyIndexEntry> KeyIndex::get(const IndexKey& key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
        misses_++;
        return std::nullopt;
    }
    hits_++;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->second;
}

bool KeyIndex::contains(const IndexKey& key) const {
    return map_.find(key) != map_.end();
}

void KeyIndex::remove(const IndexKey& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return;
    lru_.erase(it->second);
    map_.erase(it);
}

void KeyIndex::clear() {
    lru_.clear();
    map_.clear();
}

void KeyIndex::evict_one() {
    if (lru_.empty()) return;
    auto& back = lru_.back();
    map_.erase(back.first);
    lru_.pop_back();
    evictions_++;
}

} // namespace ast
