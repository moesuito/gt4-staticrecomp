// The disc image readers: a file-backed byte source for the tools and the
// ISO9660 directory/file lookup the model's file services answer from. See
// include/gt4recomp/disc_image.hpp for the contract and decision 0017 for
// why the model reads the pinned ISO instead of inventing file data.
#include "gt4recomp/disc_image.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace gt4recomp {
namespace {

// The ISO9660 primary volume descriptor sits at block 16 and starts with
// this identifier.
constexpr std::uint64_t volume_descriptor_block = 16;
constexpr char volume_identifier[] = "CD001";

// Directory records name "." and ".."; ISO9660 spells those two entries as
// the single bytes 0x00 and 0x01. Neither is a real entry.
bool is_placeholder_name(const std::string& name) {
    if (name.empty() || name == "." || name == "..") {
        return true;
    }
    return name.size() == 1 && (name[0] == '\0' || name[0] == '\1');
}

// ISO9660 stores file names as upper case with a ";version" suffix; the game
// may or may not spell the suffix. Comparing without it and without case
// matches both spellings.
std::string without_version_suffix(std::string name) {
    const std::size_t semicolon = name.find(';');
    if (semicolon != std::string::npos) {
        name.erase(semicolon);
    }
    return name;
}

class FileDiscSource final : public DiscByteSource {
public:
    explicit FileDiscSource(const std::string& path)
        : stream_(path, std::ios::binary) {
        if (!stream_) {
            throw std::runtime_error("The disc image could not be opened: " + path);
        }
        stream_.seekg(0, std::ios::end);
        size_ = static_cast<std::uint64_t>(stream_.tellg());
        stream_.seekg(0, std::ios::beg);
    }

    [[nodiscard]] std::uint64_t size() const override { return size_; }

    void read(std::uint64_t offset, std::span<std::uint8_t> destination) const override {
        if (offset + destination.size() > size_) {
            throw std::runtime_error(
                "A disc image read leaves the image (offset "
                + std::to_string(offset) + ", size "
                + std::to_string(destination.size()) + ")");
        }
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        stream_.read(reinterpret_cast<char*>(destination.data()),
                     static_cast<std::streamsize>(destination.size()));
        if (!stream_) {
            throw std::runtime_error("A disc image read failed at offset "
                                     + std::to_string(offset));
        }
    }

private:
    mutable std::ifstream stream_;
    std::uint64_t size_ = 0;
};

class MemoryDiscSource final : public DiscByteSource {
public:
    explicit MemoryDiscSource(std::vector<std::uint8_t> bytes)
        : bytes_(std::move(bytes)) {}

    [[nodiscard]] std::uint64_t size() const override { return bytes_.size(); }

    void read(std::uint64_t offset, std::span<std::uint8_t> destination) const override {
        if (offset + destination.size() > bytes_.size()) {
            throw std::runtime_error("A memory disc read leaves the image");
        }
        std::memcpy(destination.data(), bytes_.data() + offset,
                    destination.size());
    }

private:
    std::vector<std::uint8_t> bytes_;
};

// Reads a little-endian 32-bit value out of a directory record.
std::uint32_t read_u32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

} // namespace

std::unique_ptr<DiscByteSource> open_disc_file(const std::string& path) {
    return std::make_unique<FileDiscSource>(path);
}

std::unique_ptr<DiscByteSource> make_memory_disc_source(
    std::vector<std::uint8_t> bytes) {
    return std::make_unique<MemoryDiscSource>(std::move(bytes));
}

DiscFileSliceSource::DiscFileSliceSource(const DiscFiles* files,
                                         std::string path)
    : files_(files), path_(std::move(path)) {
    if (files_ == nullptr) {
        throw std::runtime_error("The disc file slice needs a disc image");
    }
    size_ = files_->file_size(path_);
    if (size_ == 0) {
        throw std::runtime_error("The disc image has no file named " + path_);
    }
}

std::uint64_t DiscFileSliceSource::size() const {
    return size_;
}

void DiscFileSliceSource::read(std::uint64_t offset,
                               std::span<std::uint8_t> destination) const {
    if (offset + destination.size() > size_) {
        throw std::runtime_error("A disc file read leaves the file");
    }
    files_->read_file(path_, offset, destination);
}

Iso9660Image::Iso9660Image(std::unique_ptr<DiscByteSource> source)
    : source_(std::move(source)) {
    if (source_ == nullptr) {
        throw std::runtime_error("The disc image reader needs a byte source");
    }
    std::vector<std::uint8_t> descriptor(2048, 0);
    source_->read(volume_descriptor_block * 2048, descriptor);
    if (std::memcmp(descriptor.data() + 1, volume_identifier,
                    sizeof(volume_identifier) - 1) != 0
        || descriptor[0] != 1) {
        throw std::runtime_error(
            "The disc image is not an ISO9660 volume (no primary descriptor)");
    }
    block_size_ = read_u16(descriptor.data() + 128);
    if (block_size_ == 0) {
        throw std::runtime_error("The disc image declares a zero block size");
    }
    // The root directory record is the last 34 bytes of the descriptor's
    // system area (offset 156).
    const std::uint8_t* root = descriptor.data() + 156;
    root_extent_ = read_u32(root + 2);
    root_size_ = read_u32(root + 10);
    if (root_extent_ == 0 || root_size_ == 0) {
        throw std::runtime_error("The disc image declares an empty root directory");
    }
}

std::string Iso9660Image::normalize(std::string_view path) {
    std::string text(path);
    // Strip a device prefix ("cdrom0:", "cdrom:", "host:") case-insensitively.
    const std::size_t colon = text.find(':');
    if (colon != std::string::npos && colon <= 8) {
        text.erase(0, colon + 1);
    }
    for (char& character : text) {
        if (character == '/') {
            character = '\\';
        }
        character = static_cast<char>(std::toupper(
            static_cast<unsigned char>(character)));
    }
    while (!text.empty() && text.front() == '\\') {
        text.erase(text.begin());
    }
    while (!text.empty() && text.back() == '\\') {
        text.pop_back();
    }
    return text;
}

std::vector<Iso9660Image::Record> Iso9660Image::read_directory(
    std::uint32_t extent, std::uint32_t size) const {
    std::vector<std::uint8_t> bytes(size, 0);
    source_->read(static_cast<std::uint64_t>(extent) * block_size_, bytes);
    std::vector<Record> records;
    std::uint64_t offset = 0;
    while (offset + 33 <= bytes.size()) {
        const std::uint8_t length = bytes[offset];
        if (length == 0) {
            // A zero length byte ends the records in this block; the next
            // block continues (records never straddle blocks).
            offset = (offset / block_size_ + 1) * block_size_;
            continue;
        }
        if (offset + length > bytes.size() || length < 33) {
            break;
        }
        const std::uint8_t* record = bytes.data() + offset;
        const std::uint8_t name_length = record[32];
        if (offset + 33 + name_length <= bytes.size()) {
            Record entry;
            entry.name.assign(reinterpret_cast<const char*>(record + 33),
                              name_length);
            entry.extent = read_u32(record + 2);
            entry.size = read_u32(record + 10);
            entry.directory = (record[25] & 0x02) != 0;
            if (!is_placeholder_name(entry.name)) {
                records.push_back(std::move(entry));
            }
        }
        offset += length;
    }
    std::sort(records.begin(), records.end(),
              [](const Record& left, const Record& right) {
                  return left.name < right.name;
              });
    return records;
}

const Iso9660Image::Record* Iso9660Image::resolve(
    std::string_view normalized) const {
    // Walk the components; the root directory answers for the empty path.
    std::string directory_path;
    std::vector<Record> current;
    std::uint32_t extent = root_extent_;
    std::uint32_t size = root_size_;
    std::size_t start = 0;
    while (true) {
        const std::size_t separator = normalized.find('\\', start);
        const std::string component = without_version_suffix(
            std::string(normalized.substr(start,
                                           separator == std::string_view::npos
                                               ? std::string_view::npos
                                               : separator - start)));
        if (component.empty()) {
            return nullptr;
        }
        const auto cached = directories_.find(directory_path);
        if (cached != directories_.end()) {
            current = cached->second;
        } else {
            current = read_directory(extent, size);
            directories_.emplace(directory_path, current);
        }
        const auto found = std::find_if(
            current.begin(), current.end(), [&component](const Record& entry) {
                return without_version_suffix(entry.name) == component;
            });
        if (found == current.end()) {
            return nullptr;
        }
        if (separator == std::string_view::npos) {
            return &(files_.emplace(std::string(normalized), *found)
                         .first->second);
        }
        if (!found->directory) {
            return nullptr;
        }
        extent = found->extent;
        size = found->size;
        if (!directory_path.empty()) {
            directory_path += '\\';
        }
        directory_path += component;
        start = separator + 1;
    }
}

std::uint64_t Iso9660Image::file_size(std::string_view path) const {
    const Record* record = resolve(normalize(path));
    if (record == nullptr || record->directory) {
        return 0;
    }
    return record->size;
}

std::uint32_t Iso9660Image::file_extent(std::string_view path) const {
    const Record* record = resolve(normalize(path));
    if (record == nullptr || record->directory) {
        return 0;
    }
    return record->extent;
}

void Iso9660Image::read_file(std::string_view path, std::uint64_t offset,
                             std::span<std::uint8_t> destination) const {
    const Record* record = resolve(normalize(path));
    if (record == nullptr || record->directory) {
        throw std::runtime_error("The disc image has no file named "
                                 + std::string(path));
    }
    if (offset > record->size) {
        throw std::runtime_error("A disc file read starts past the file's end");
    }
    const std::uint64_t available =
        std::min<std::uint64_t>(destination.size(), record->size - offset);
    if (available > 0) {
        source_->read(static_cast<std::uint64_t>(record->extent) * block_size_
                          + offset,
                      destination.first(static_cast<std::size_t>(available)));
    }
    std::fill(destination.begin() + static_cast<std::ptrdiff_t>(available),
              destination.end(), 0);
}

std::vector<std::string> Iso9660Image::directory_names(
    std::string_view path) const {
    const std::string normalized = normalize(path);
    std::vector<std::string> names;
    if (normalized.empty()) {
        for (const Record& entry : read_directory(root_extent_, root_size_)) {
            names.push_back(entry.name);
        }
        return names;
    }
    const Record* record = resolve(normalized);
    if (record == nullptr || !record->directory) {
        return names;
    }
    for (const Record& entry : read_directory(record->extent, record->size)) {
        names.push_back(entry.name);
    }
    return names;
}

} // namespace gt4recomp
