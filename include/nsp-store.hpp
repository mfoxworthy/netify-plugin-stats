// netify-plugin-stats — binary ring-buffer time-series store
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

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

} // namespace nsp
