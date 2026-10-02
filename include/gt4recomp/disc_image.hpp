#pragma once

// Host-side read-only access to the game disc image (the pinned ISO). The
// model's file services answer the game's loadfile requests from this data:
// the boot loads its IOP modules off the disc ("cdrom0:\IRX\SIO2MAN.IRX;1"
// and friends) and, without the bytes, those loads fail exactly as they do
// on a console without a disc. Nothing here is game-derived code: the image
// stays a local input and is never committed.

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gt4recomp {

// A random-access byte source for a disc image: file-backed in the tools,
// memory-backed in tests.
class DiscByteSource {
public:
    virtual ~DiscByteSource() = default;

    [[nodiscard]] virtual std::uint64_t size() const = 0;

    // Reads `destination.size()` bytes at `offset`. Throws std::runtime_error
    // when the range leaves the image.
    virtual void read(std::uint64_t offset,
                      std::span<std::uint8_t> destination) const = 0;
};

// Opens a disc image file read-only.
[[nodiscard]] std::unique_ptr<DiscByteSource> open_disc_file(
    const std::string& path);

// A disc image held in memory, for tests.
[[nodiscard]] std::unique_ptr<DiscByteSource> make_memory_disc_source(
    std::vector<std::uint8_t> bytes);

// What the model's file services ask of a disc image: a file's size and its
// bytes. Paths use the game's spelling (`cdrom0:\IRX\NAME.EXT;1`); the
// implementation normalizes them.
class DiscFiles {
public:
    virtual ~DiscFiles() = default;

    // The file's size in bytes, or 0 when the path names no file. (The
    // pinned disc's largest file, GT4.VOL, is 2,459,502,592 bytes, so a
    // 64-bit answer keeps room.)
    [[nodiscard]] virtual std::uint64_t file_size(std::string_view path) const = 0;

    // Reads part of a file at `offset`, zero-filling the tail when the file
    // ends before `destination`. Throws std::runtime_error when the path
    // names no file or the offset is past the file's end.
    virtual void read_file(std::string_view path, std::uint64_t offset,
                           std::span<std::uint8_t> destination) const = 0;
};

// ISO9660 image reader. The pinned disc is a plain ISO9660 volume ("GRANTU-
// RISMO4", 2048-byte blocks) whose root holds SYSTEM.CNF, SCUS_973.28,
// CORE.GT4, IOPRP300.IMG, IRX/, NET/, EPSON/ and GT4.VOL. Lookups walk the
// directory tree on demand and cache what they read; reads stream from the
// byte source, so a 2.4 GB file costs only the parts that are read.
class Iso9660Image final : public DiscFiles {
public:
    // Parses the primary volume descriptor and the root directory; throws
    // std::runtime_error when the image is not ISO9660.
    explicit Iso9660Image(std::unique_ptr<DiscByteSource> source);

    [[nodiscard]] std::uint64_t file_size(std::string_view path) const override;
    void read_file(std::string_view path, std::uint64_t offset,
                   std::span<std::uint8_t> destination) const override;

    // The names in a directory as stored on the disc (upper case, with the
    // version suffix), sorted; an empty list when the path names no
    // directory. For tools and tests.
    [[nodiscard]] std::vector<std::string> directory_names(
        std::string_view path) const;

private:
    struct Record {
        std::string name;      // as stored ("SIO2MAN.IRX;1")
        std::uint32_t extent = 0;   // first block
        std::uint32_t size = 0;     // bytes
        bool directory = false;
    };

    [[nodiscard]] std::vector<Record> read_directory(
        std::uint32_t extent, std::uint32_t size) const;
    // Resolves a normalized path ("IRX\SIO2MAN.IRX") to a record.
    [[nodiscard]] const Record* resolve(std::string_view normalized) const;

    static std::string normalize(std::string_view path);

    std::unique_ptr<DiscByteSource> source_;
    std::uint32_t block_size_ = 0;
    std::uint32_t root_extent_ = 0;
    std::uint32_t root_size_ = 0;
    // Caches keyed by normalized directory path ("" is the root) and by
    // normalized file path.
    mutable std::map<std::string, std::vector<Record>> directories_;
    mutable std::map<std::string, Record> files_;
};

} // namespace gt4recomp
