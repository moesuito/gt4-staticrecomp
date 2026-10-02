#pragma once

// The game's own data volume, GT4.VOL (2,459,502,592 bytes at ISO extent
// 105879). Its format is evidenced in
// docs/reverse-engineering/m30-slice17-archive-path.md and decision 0018:
//
//  - header: magic 0xACB990AD, version 0x00020002, three values, the name
//    table's offset, a child count and that many child offsets (the count
//    includes the header itself, like an entry's count includes the entry);
//  - entry: {name pointer, count, value} followed by (count - 1) item
//    words; directories list their children by entry offset, files carry
//    their byte size in `value` and their items describe the data;
//  - names are the archive's text with every byte XOR 0xFF, NUL
//    terminated, in name tables whose pointer tags (0x00, 0x01, 0x02) sit
//    in the top byte.
//
// The reader resolves names, directory children and file sizes. It parses
// lazily — the pinned tree is far larger than one walk should pay for — and
// it does not invent data offsets: where the format is not yet pinned (the
// file item records), it reports what it read and stops with an error
// instead of guessing.

#include "gt4recomp/disc_image.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace gt4recomp {

class Gt4Volume {
public:
    // One archive entry's header. `offset` is the entry's position in the
    // volume; `value` is the entry's third header word (a file's byte size
    // for the file entries the game loads); `count` its item count plus one.
    struct Entry {
        std::string name;
        std::uint64_t offset = 0;
        std::uint32_t count = 0;
        std::uint32_t value = 0;
    };

    // Reads the volume's bytes through `source` starting at `base` (the
    // GT4.VOL file inside the ISO), limited to `size` bytes. Throws
    // std::runtime_error when the header is not a GT4 volume. Only the
    // header and the root entries are parsed here.
    Gt4Volume(const DiscByteSource* source, std::uint64_t base,
              std::uint64_t size);

    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    // The header's three values, for tools.
    [[nodiscard]] std::uint32_t header_value(std::uint32_t index) const;
    [[nodiscard]] const std::vector<Entry>& root() const noexcept { return root_; }

    // The child entries an entry's item list names (directories); an item
    // that is not a child entry (a file's data record) is skipped. Parsed
    // once per entry.
    [[nodiscard]] const std::vector<Entry>& children(const Entry& entry) const;

    // The entry a path names ("mpeg/gt4/mv0010", case-insensitive, '/'
    // separators), or nullptr when the path names nothing.
    [[nodiscard]] const Entry* find(std::string_view path) const;

    // Decodes a name pointer's text (XOR 0xFF); exposed for tests.
    [[nodiscard]] std::string name_at(std::uint32_t pointer) const;

private:
    [[nodiscard]] Entry read_entry(std::uint32_t offset) const;
    [[nodiscard]] std::uint32_t word(std::uint64_t offset) const;

    const DiscByteSource* source_ = nullptr;
    std::uint64_t base_ = 0;
    std::uint64_t size_ = 0;
    // The volume's metadata region (the header, the tree and the name
    // tables) read once, so the tree walk does not issue one disc read per
    // word; offsets beyond it are read directly.
    std::vector<std::uint8_t> metadata_;
    // Parsed entry headers and children lists by offset; items that failed
    // to parse as entries (data records) are remembered so they are never
    // retried.
    mutable std::map<std::uint32_t, Entry> entry_cache_;
    mutable std::map<std::uint32_t, std::vector<Entry>> children_cache_;
    mutable std::set<std::uint32_t> rejected_items_;
    std::uint32_t version_ = 0;
    std::uint32_t header_values_[3] = {};
    std::uint32_t name_table_ = 0;
    std::vector<Entry> root_;
};

} // namespace gt4recomp
