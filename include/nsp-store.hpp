// netify-plugin-stats — binary ring-buffer time-series store
#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "nsp-accum.hpp"

namespace nsp {

constexpr uint32_t NSP_MAGIC   = 0x4E535031;   // "NSP1"
constexpr uint32_t NSP_VERSION = 1;
constexpr uint32_t NSP_NAME_MAX = 48;          // fixed series-name field width

// One metric cell: all five metrics for one series at one slot.
#pragma pack(push, 1)
struct Cell {
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t rx_pkts;
    uint32_t tx_pkts;
    uint32_t flows;
};                                              // 28 bytes, packed

// Per-slot metadata (one per slot, precedes the cell grid).
struct SlotMeta {
    int64_t epoch;                              // slot start time; 0 == empty/unwritten
};

// File header, followed by series-name table, then SlotMeta[slot_count],
// then Cell[slot_count * series_capacity].
struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;        // ring length
    uint32_t slot_duration;     // seconds per slot (tier step)
    uint32_t series_capacity;   // max series the file can hold
    uint32_t series_count;      // series currently named
    int64_t  base_epoch;        // creation time (informational)
    int64_t  write_cursor;      // index of newest written slot; -1 == none yet
};
#pragma pack(pop)

// Byte offsets within the file.
inline size_t names_offset()  { return sizeof(Header); }
inline size_t names_bytes(uint32_t cap) { return (size_t)cap * NSP_NAME_MAX; }
inline size_t slotmeta_offset(uint32_t cap) { return names_offset() + names_bytes(cap); }
inline size_t slotmeta_bytes(uint32_t slots) { return (size_t)slots * sizeof(SlotMeta); }
inline size_t cells_offset(uint32_t cap, uint32_t slots) {
    return slotmeta_offset(cap) + slotmeta_bytes(slots);
}
inline size_t cell_index(uint32_t slot, uint32_t series, uint32_t cap) {
    return (size_t)slot * cap + series;
}
inline size_t file_size(uint32_t cap, uint32_t slots) {
    return cells_offset(cap, slots) + (size_t)slots * cap * sizeof(Cell);
}

// A memory-mapped fixed-size ring buffer for one (dimension, tier) file.
class Store {
public:
    Store() = default;
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    // Open `path`, (re)creating it if missing or if geometry/magic/version
    // mismatch the requested (slot_duration, slot_count, series_capacity).
    // On geometry mismatch the file is recreated and old data discarded.
    // Returns false on unrecoverable I/O error (err populated).
    bool Open(const std::string &path, uint32_t slot_duration,
              uint32_t slot_count, uint32_t series_capacity, std::string &err);

    void Close();

    bool ok() const { return base_ != nullptr; }

    // Resolve a series name to a stable column index, assigning a new column
    // on first use. Returns false if capacity is exhausted.
    bool SeriesIndex(const std::string &name, uint32_t &idx);

    // Write one slot's worth of cells at time `epoch`. `cells` is indexed by
    // series column; entries beyond series_count are ignored. Advances the
    // ring cursor (O(1)). If `epoch` falls in the same slot window as the
    // current newest slot, the cell is summed into it (consolidation helper).
    void Append(int64_t epoch, const std::vector<Cell> &cells);

    // Read all slots oldest->newest for the named series into aligned arrays.
    // `epochs[i]` is the slot start (0 if that slot was never written);
    // `cells[i]` is the metric cell (zeroed for empty slots).
    void ReadSeries(const std::string &name,
                    std::vector<int64_t> &epochs,
                    std::vector<Cell> &cells) const;

    // Accessors for the query layer.
    uint32_t slot_duration() const;
    uint32_t slot_count() const;
    std::vector<std::string> series_names() const;

private:
    Header *header() const { return reinterpret_cast<Header *>(base_); }
    char *names_base() const { return static_cast<char *>(base_) + names_offset(); }
    SlotMeta *slotmeta_base() const;
    Cell *cells_base() const;
    bool MapFile(const std::string &path, size_t size, std::string &err, bool create);

    void *base_ = nullptr;
    size_t mapped_size_ = 0;
    int fd_ = -1;
};

struct TierSpec { uint32_t step; uint32_t count; };

// One dimension ("apps"/"cats"), one Store per tier.
class TierSet {
public:
    // dir/<dim>.t<n>.rrb per tier. Recreates files on geometry mismatch.
    bool Open(const std::string &dir, const std::string &dim,
              const std::vector<TierSpec> &tiers, uint32_t series_capacity,
              std::string &err);
    void Close();

    // Append one interval sample (already top-N-reduced) at `epoch` to every
    // tier; each tier consolidates within its own slot window.
    void AppendSample(int64_t epoch, const std::vector<NamedMetrics> &series);

    // Read one tier (by index) for one series into the query arrays.
    void ReadSeries(size_t tier, const std::string &name,
                    std::vector<int64_t> &epochs, std::vector<Cell> &cells) const;

    Store &tier(size_t i) { return *stores_.at(i); }
    size_t tier_count() const { return stores_.size(); }

private:
    std::vector<std::unique_ptr<Store>> stores_;
};

} // namespace nsp
