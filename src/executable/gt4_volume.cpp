// The GT4.VOL archive reader. The format is evidenced in
// docs/reverse-engineering/m30-slice17-archive-path.md and decision 0018;
// this file keeps the same sequence of steps the evidence describes and
// never guesses a field it has not seen. Parsing is lazy: only the entries
// a lookup walks are read.
#include "gt4recomp/gt4_volume.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace gt4recomp {
namespace {

constexpr std::uint32_t volume_magic = 0xACB990ADu;
// The header is seven words: magic, version, three values, the name table
// pointer and the child count; the child offsets follow at +0x20.
constexpr std::uint64_t header_size = 0x20;
// A name pointer's top byte is its table tag; the three tables the pinned
// volume uses are 0x00, 0x01 and 0x02.
constexpr std::uint32_t maximum_name_tag = 0x02u;

bool is_printable_name(const std::string& text) {
    if (text.empty() || text.size() > 200) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char character) {
        const unsigned char value = static_cast<unsigned char>(character);
        return value >= 0x20 && value < 0x7F;
    });
}

} // namespace

Gt4Volume::Gt4Volume(const DiscByteSource* source, std::uint64_t base,
                     std::uint64_t size)
    : source_(source), base_(base), size_(size) {
    if (source_ == nullptr) {
        throw std::runtime_error("The volume reader needs a byte source");
    }
    if (size_ < header_size) {
        throw std::runtime_error("The volume is smaller than its header");
    }
    // Read the metadata window once: the header, the entry tree and the
    // name tables live in the archive's first megabytes.
    constexpr std::uint64_t metadata_window = 64 * 1024 * 1024;
    const std::uint64_t window = std::min(size_, metadata_window);
    metadata_.resize(static_cast<std::size_t>(window));
    source_->read(base_, metadata_);
    if (word(0) != volume_magic) {
        throw std::runtime_error("The image is not a GT4 volume (bad magic)");
    }
    version_ = word(4);
    header_values_[0] = word(8);
    header_values_[1] = word(12);
    header_values_[2] = word(16);
    name_table_ = word(20);
    const std::uint32_t child_count = word(24);
    if (child_count < 2 || child_count > 4096) {
        throw std::runtime_error("The volume's root child count is out of range");
    }
    // The header's count follows the same rule as an entry's: it counts the
    // header itself as well, so the child list holds count - 1 offsets.
    for (std::uint32_t index = 0; index + 1 < child_count; ++index) {
        const std::uint32_t child = word(header_size + index * 4);
        root_.push_back(read_entry(child));
    }
}

std::uint32_t Gt4Volume::header_value(std::uint32_t index) const {
    if (index > 2) {
        throw std::runtime_error("The volume header has three values");
    }
    return header_values_[index];
}

std::uint32_t Gt4Volume::word(std::uint64_t offset) const {
    if (offset + 4 > size_) {
        throw std::runtime_error("A volume read leaves the archive");
    }
    if (offset + 4 <= metadata_.size()) {
        const std::uint8_t* bytes = metadata_.data() + offset;
        return static_cast<std::uint32_t>(bytes[0])
            | (static_cast<std::uint32_t>(bytes[1]) << 8)
            | (static_cast<std::uint32_t>(bytes[2]) << 16)
            | (static_cast<std::uint32_t>(bytes[3]) << 24);
    }
    std::uint8_t bytes[4] = {};
    source_->read(base_ + offset, bytes);
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string Gt4Volume::name_at(std::uint32_t pointer) const {
    const std::uint32_t table_offset = pointer & 0x00FFFFFFu;
    std::string text;
    for (std::uint32_t index = 0; index < 200; ++index) {
        const std::uint64_t offset = static_cast<std::uint64_t>(table_offset) + index;
        if (offset + 1 > size_) {
            break;
        }
        std::uint8_t byte = 0;
        if (offset < metadata_.size()) {
            byte = metadata_[offset];
        } else {
            source_->read(base_ + offset, {&byte, 1});
        }
        const char decoded = static_cast<char>(byte ^ 0xFFu);
        if (decoded == '\0') {
            break;
        }
        text.push_back(decoded);
    }
    return text;
}

Gt4Volume::Entry Gt4Volume::read_entry(std::uint32_t offset) const {
    const auto cached = entry_cache_.find(offset);
    if (cached != entry_cache_.end()) {
        return cached->second;
    }
    if (static_cast<std::uint64_t>(offset) + 12 > size_) {
        throw std::runtime_error("A volume entry starts outside the archive");
    }
    Entry entry;
    entry.offset = offset;
    const std::uint32_t name_pointer = word(offset);
    if ((name_pointer >> 24) > maximum_name_tag) {
        throw std::runtime_error("A volume entry's name pointer is out of range");
    }
    entry.name = name_at(name_pointer);
    if (!is_printable_name(entry.name)) {
        throw std::runtime_error("A volume entry's name is not printable text");
    }
    entry.count = word(offset + 4);
    entry.value = word(offset + 8);
    entry_cache_.emplace(offset, entry);
    return entry;
}

const std::vector<Gt4Volume::Entry>& Gt4Volume::children(
    const Entry& entry) const {
    const auto cached = children_cache_.find(static_cast<std::uint32_t>(entry.offset));
    if (cached != children_cache_.end()) {
        return cached->second;
    }
    std::vector<Entry> children;
    if (entry.count > 1) {
        const std::uint32_t item_count = entry.count - 1;
        if (entry.offset + 12 + static_cast<std::uint64_t>(item_count) * 4
            > size_) {
            throw std::runtime_error("A volume entry's item list leaves the archive");
        }
        for (std::uint32_t index = 0; index < item_count; ++index) {
            const std::uint32_t item =
                word(entry.offset + 12 + index * 4);
            if (item >= 0x01000000u || item + 12 > size_) {
                continue;
            }
            if (rejected_items_.count(item) != 0) {
                continue;
            }
            // Cheap rejection first: a child entry starts with a name
            // pointer whose tag must be one of the known tables, so a data
            // record's word never pays for a parse attempt.
            if ((word(item) >> 24) > maximum_name_tag) {
                rejected_items_.insert(item);
                continue;
            }
            try {
                children.push_back(read_entry(item));
            } catch (const std::runtime_error&) {
                // Not a child entry: the item is part of the entry's own
                // data records, which the reader does not model yet.
                rejected_items_.insert(item);
            }
        }
    }
    return children_cache_
        .emplace(static_cast<std::uint32_t>(entry.offset), std::move(children))
        .first->second;
}

const Gt4Volume::Entry* Gt4Volume::find(std::string_view path) const {
    std::vector<std::string> parts;
    std::string current;
    for (const char character : path) {
        if (character == '/' || character == '\\') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(static_cast<char>(
                std::toupper(static_cast<unsigned char>(character))));
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    const std::vector<Entry>* level = &root_;
    const Entry* found = nullptr;
    for (const std::string& part : parts) {
        found = nullptr;
        for (const Entry& candidate : *level) {
            std::string upper = candidate.name;
            for (char& character : upper) {
                character = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(character)));
            }
            if (upper == part) {
                found = &candidate;
                break;
            }
        }
        if (found == nullptr) {
            return nullptr;
        }
        level = &children(*found);
    }
    return found;
}

} // namespace gt4recomp
