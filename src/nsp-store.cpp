#include "nsp-store.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nsp {

Store::~Store() { Close(); }

SlotMeta *Store::slotmeta_base() const {
    return reinterpret_cast<SlotMeta *>(
        static_cast<char *>(base_) + slotmeta_offset(header()->series_capacity));
}

Cell *Store::cells_base() const {
    return reinterpret_cast<Cell *>(
        static_cast<char *>(base_) +
        cells_offset(header()->series_capacity, header()->slot_count));
}

bool Store::MapFile(const std::string &path, size_t size, std::string &err, bool create) {
    int flags = O_RDWR | (create ? (O_CREAT | O_TRUNC) : 0);
    fd_ = ::open(path.c_str(), flags, 0644);
    if (fd_ < 0) { err = "open: " + std::string(strerror(errno)); return false; }
    if (create && ::ftruncate(fd_, (off_t)size) != 0) {
        err = "ftruncate: " + std::string(strerror(errno));
        ::close(fd_); fd_ = -1; return false;
    }
    void *m = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
        err = "mmap: " + std::string(strerror(errno));
        ::close(fd_); fd_ = -1; return false;
    }
    base_ = m;
    mapped_size_ = size;
    return true;
}

bool Store::Open(const std::string &path, uint32_t slot_duration,
                 uint32_t slot_count, uint32_t series_capacity, std::string &err) {
    Close();
    size_t want = file_size(series_capacity, slot_count);

    // Decide whether an existing file is reusable.
    bool recreate = true;
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && (size_t)st.st_size == want) {
        if (!MapFile(path, want, err, /*create=*/false)) return false;
        Header *h = header();
        recreate = !(h->magic == NSP_MAGIC && h->version == NSP_VERSION &&
                     h->slot_duration == slot_duration &&
                     h->slot_count == slot_count &&
                     h->series_capacity == series_capacity);
        if (recreate) { Close(); }
    }

    if (recreate) {
        if (!MapFile(path, want, err, /*create=*/true)) return false;
        std::memset(base_, 0, want);
        Header *h = header();
        h->magic = NSP_MAGIC;
        h->version = NSP_VERSION;
        h->slot_count = slot_count;
        h->slot_duration = slot_duration;
        h->series_capacity = series_capacity;
        h->series_count = 0;
        h->base_epoch = 0;
        h->write_cursor = -1;
    }
    return true;
}

void Store::Close() {
    if (base_) { ::munmap(base_, mapped_size_); base_ = nullptr; mapped_size_ = 0; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool Store::SeriesIndex(const std::string &name, uint32_t &idx) {
    Header *h = header();
    char *nb = names_base();
    for (uint32_t i = 0; i < h->series_count; ++i) {
        const char *nm = nb + (size_t)i * NSP_NAME_MAX;
        if (std::strncmp(nm, name.c_str(), NSP_NAME_MAX) == 0) { idx = i; return true; }
    }
    if (h->series_count >= h->series_capacity) return false;
    idx = h->series_count++;
    char *slot = nb + (size_t)idx * NSP_NAME_MAX;
    std::memset(slot, 0, NSP_NAME_MAX);
    std::strncpy(slot, name.c_str(), NSP_NAME_MAX - 1);
    return true;
}

void Store::Append(int64_t epoch, const std::vector<Cell> &cells) {
    Header *h = header();
    SlotMeta *meta = slotmeta_base();
    Cell *grid = cells_base();

    int64_t cur = h->write_cursor;
    bool same_slot = false;
    if (cur >= 0) {
        int64_t cur_epoch = meta[cur].epoch;
        if (cur_epoch != 0 &&
            (epoch / (int64_t)h->slot_duration) == (cur_epoch / (int64_t)h->slot_duration))
            same_slot = true;
    }

    uint32_t slot;
    if (same_slot) {
        slot = (uint32_t)cur;
        // Consolidate: sum into the existing slot.
        for (uint32_t s = 0; s < h->series_count && s < (uint32_t)cells.size(); ++s) {
            Cell &dst = grid[cell_index(slot, s, h->series_capacity)];
            dst.rx_bytes += cells[s].rx_bytes;
            dst.tx_bytes += cells[s].tx_bytes;
            dst.rx_pkts  += cells[s].rx_pkts;
            dst.tx_pkts  += cells[s].tx_pkts;
            dst.flows    += cells[s].flows;
        }
    } else {
        slot = (uint32_t)((cur + 1) % (int64_t)h->slot_count);
        meta[slot].epoch = (epoch / (int64_t)h->slot_duration) * (int64_t)h->slot_duration;
        for (uint32_t s = 0; s < h->series_capacity; ++s) {
            Cell &dst = grid[cell_index(slot, s, h->series_capacity)];
            dst = (s < (uint32_t)cells.size()) ? cells[s] : Cell{};
        }
        h->write_cursor = slot;
        if (h->base_epoch == 0) h->base_epoch = epoch;
    }
}

void Store::ReadSeries(const std::string &name,
                       std::vector<int64_t> &epochs,
                       std::vector<Cell> &cells) const {
    Header *h = header();
    epochs.assign(h->slot_count, 0);
    cells.assign(h->slot_count, Cell{});

    char *nb = names_base();
    uint32_t col = 0; bool found = false;
    for (uint32_t i = 0; i < h->series_count; ++i) {
        const char *nm = nb + (size_t)i * NSP_NAME_MAX;
        if (std::strncmp(nm, name.c_str(), NSP_NAME_MAX) == 0) { col = i; found = true; break; }
    }

    SlotMeta *meta = slotmeta_base();
    Cell *grid = cells_base();
    int64_t cur = h->write_cursor;
    if (cur < 0) return;  // nothing written

    // Emit oldest->newest. Oldest is the slot after the cursor in ring order.
    for (uint32_t k = 0; k < h->slot_count; ++k) {
        uint32_t slot = (uint32_t)((cur + 1 + (int64_t)k) % (int64_t)h->slot_count);
        epochs[k] = meta[slot].epoch;
        if (found && meta[slot].epoch != 0)
            cells[k] = grid[cell_index(slot, col, h->series_capacity)];
    }
}

uint32_t Store::slot_duration() const { return header()->slot_duration; }
uint32_t Store::slot_count() const { return header()->slot_count; }

std::vector<std::string> Store::series_names() const {
    std::vector<std::string> out;
    Header *h = header();
    char *nb = names_base();
    for (uint32_t i = 0; i < h->series_count; ++i) {
        const char *nm = nb + (size_t)i * NSP_NAME_MAX;
        out.emplace_back(nm, ::strnlen(nm, NSP_NAME_MAX));
    }
    return out;
}

bool TierSet::Open(const std::string &dir, const std::string &dim,
                   const std::vector<TierSpec> &tiers, uint32_t series_capacity,
                   std::string &err) {
    Close();
    for (size_t i = 0; i < tiers.size(); ++i) {
        auto s = std::make_unique<Store>();
        std::string path = dir + "/" + dim + ".t" + std::to_string(i) + ".rrb";
        if (!s->Open(path, tiers[i].step, tiers[i].count, series_capacity, err))
            return false;
        stores_.push_back(std::move(s));
    }
    return true;
}

void TierSet::Close() { stores_.clear(); }

size_t TierSet::AppendSample(int64_t epoch, const std::vector<NamedMetrics> &series) {
    size_t total_dropped = 0;
    for (auto &sp : stores_) {
        // Assign columns + build a cell vector for this tier.
        // First pass: ensure all series have indices assigned and find max idx.
        uint32_t max_idx = 0;
        std::vector<std::pair<uint32_t, const Metrics *>> placed;
        for (auto &nm : series) {
            uint32_t idx;
            if (!sp->SeriesIndex(nm.name, idx)) {
                total_dropped++;  // capacity exhausted — series silently dropped
                continue;
            }
            if (idx + 1 > max_idx) max_idx = idx + 1;
            placed.emplace_back(idx, &nm.m);
        }
        std::vector<Cell> cells(max_idx, Cell{});
        for (auto &p : placed) {
            const Metrics &m = *p.second;
            cells[p.first] = Cell{ m.rx_bytes, m.tx_bytes, m.rx_pkts, m.tx_pkts, m.flows };
        }
        sp->Append(epoch, cells);
    }
    return total_dropped;
}

void TierSet::ReadSeries(size_t tier, const std::string &name,
                         std::vector<int64_t> &epochs, std::vector<Cell> &cells) const {
    stores_.at(tier)->ReadSeries(name, epochs, cells);
}

} // namespace nsp
