#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nx::nvn {

// Registrations and a lazily rebuilt address index for each memory pool. The caller holds objects_lock for every
// operation, including visits. Entries contain guest keys, never object pointers: the current object must still be
// resolved by the caller. A guest can remain registered in several pools after it is re-initialized.
class TextureSpanIndex {
public:
    void insert(uint64_t pool, uint64_t guest) {
        auto& p = pools_[pool];
        if (p.guests.insert(guest).second) {
            guest_pools_[guest].insert(pool);
            p.dirty = true;
        }
    }

    // Only pools with registered guests retain a cached index.
    std::size_t pool_count() const { return pools_.size(); }

    // The current object at this key became a different Texture. Its registrations do not necessarily name the
    // Texture's current pool, so invalidate every pool containing the key.
    void invalidate_guest(uint64_t guest) {
        auto found = guest_pools_.find(guest);
        if (found == guest_pools_.end()) return;
        for (uint64_t pool : found->second) pools_.at(pool).dirty = true;
    }

    void erase_guest(uint64_t guest) {
        auto found = guest_pools_.find(guest);
        if (found == guest_pools_.end()) return;
        for (uint64_t pool : found->second) {
            auto p = pools_.find(pool);  // Reverse memberships only refer to existing pools.
            p->second.guests.erase(guest);
            if (p->second.guests.empty()) {
                // Erasing the pool also releases its cached spans and matches.
                pools_.erase(p);
            } else {
                p->second.dirty = true;
            }
        }
        guest_pools_.erase(found);
    }

    // Move every registration to a kept texture's new host key, merging any existing registrations at that key.
    void move_guest(uint64_t from, uint64_t to) {
        if (from == to) {
            invalidate_guest(to);
            return;
        }
        auto found = guest_pools_.find(from);
        if (found != guest_pools_.end()) {
            auto memberships = std::move(found->second);
            guest_pools_.erase(found);
            for (uint64_t pool : memberships) {
                auto& p = pools_.at(pool);
                // Moving always leaves the destination registered, including when that key already exists.
                p.guests.insert(to);
                p.guests.erase(from);
                guest_pools_[to].insert(pool);
                p.dirty = true;
            }
        }
        // The host key can already have registrations of its own, and now resolves to the moved object.
        invalidate_guest(to);
    }

    // For the legacy mode that publishes a Texture before its fields are filled in.
    template <typename Visit>
    void for_each(uint64_t pool, Visit&& visit) const {
        auto found = pools_.find(pool);
        if (found == pools_.end()) return;
        for (uint64_t guest : found->second.guests) visit(guest);
    }

    // read(guest, lo, hi) resolves a current Texture and returns its unsigned start and end; false excludes it.
    // visit(guest) resolves the key again and checks the live object's overlap. Do not mutate this index in either
    // callback. The comparisons intentionally match the old scan even for zero lengths and uint64_t wraparound.
    template <typename Read, typename Visit>
    void visit(uint64_t pool, uint64_t off, uint64_t size, Read&& read, Visit&& visit) {
        auto found = pools_.find(pool);
        if (found == pools_.end()) return;
        auto& p = found->second;
        if (p.dirty) {
            p.spans.clear();
            p.spans.reserve(p.guests.size());
            for (uint64_t guest : p.guests) {
                uint64_t lo, hi;
                if (read(guest, lo, hi)) p.spans.push_back({guest, lo, hi, 0});
            }
            std::sort(p.spans.begin(), p.spans.end(), [](const Span& a, const Span& b) {
                return a.lo < b.lo || (a.lo == b.lo && a.guest < b.guest);
            });
            uint64_t max_hi = 0;
            for (Span& span : p.spans) {
                max_hi = std::max(max_hi, span.hi);
                span.max_hi = max_hi;
            }
            p.dirty = false;
        }
        const uint64_t end = off + size;
        auto it = std::lower_bound(p.spans.begin(), p.spans.end(), end,
                                   [](const Span& span, uint64_t value) { return span.lo < value; });
        p.matches.clear();
        while (it != p.spans.begin()) {
            --it;
            if (it->max_hi <= off) break;
            if (it->hi > off) p.matches.push_back(it->guest);
        }
        // The old scan marked in ascending guest-key order. Preserve it even when spans overlap or share an address.
        std::sort(p.matches.begin(), p.matches.end());
        for (uint64_t guest : p.matches) visit(guest);
    }

private:
    struct Span {
        uint64_t guest, lo, hi, max_hi;
    };
    struct Pool {
        std::set<uint64_t> guests;
        std::vector<Span> spans;
        std::vector<uint64_t> matches;  // reused under objects_lock
        bool dirty = true;
    };
    std::map<uint64_t, Pool> pools_;
    std::unordered_map<uint64_t, std::set<uint64_t>> guest_pools_;
};

}  // namespace nx::nvn
