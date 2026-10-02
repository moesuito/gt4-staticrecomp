// Unit tests for the GT4.VOL archive reader: a synthetic volume built in
// memory exercises the header, the name decoding (XOR 0xFF), the child
// classification and the path lookup; when the pinned ISO is available
// (argv[1]) the same reader must decode the real archive: the root's 23
// names and the movie entry the game's own loader asks for.
#include "gt4recomp/disc_image.hpp"
#include "gt4recomp/gt4_volume.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gt4recomp;

namespace {

void put_word(std::vector<std::uint8_t>& image, std::uint32_t offset,
              std::uint32_t value) {
    image[offset + 0] = static_cast<std::uint8_t>(value);
    image[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    image[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    image[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

// Names are the archive's text with every byte XOR 0xFF and an encoded NUL
// (0xFF) terminator.
void put_name(std::vector<std::uint8_t>& image, std::uint32_t offset,
              const std::string& text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        image[offset + index] =
            static_cast<std::uint8_t>(text[index]) ^ 0xFF;
    }
    image[offset + text.size()] = 0xFF;
}

std::vector<std::uint8_t> make_synthetic_volume() {
    std::vector<std::uint8_t> image(0x400, 0);
    // Names at 0x200: "dir1", "file1", "file2".
    put_name(image, 0x200, "dir1");
    put_name(image, 0x208, "file1");
    put_name(image, 0x210, "file2");
    // Header: magic, version, three values, the name table, two children.
    put_word(image, 0x00, 0xACB990ADu);
    put_word(image, 0x04, 0x00020002u);
    put_word(image, 0x08, 1);
    put_word(image, 0x0C, 2);
    put_word(image, 0x10, 3);
    put_word(image, 0x14, 0x01000200u);
    put_word(image, 0x18, 3);   // the header counts itself: two children
    put_word(image, 0x1C, 0);   // the header's last word before the list
    put_word(image, 0x20, 0x40);
    put_word(image, 0x24, 0x70);
    // Entry 0x40 "dir1": one child (0x60) and one name item that must not
    // become a child.
    put_word(image, 0x40, 0x01000200u);
    put_word(image, 0x44, 3);
    put_word(image, 0x48, 0x14);
    put_word(image, 0x4C, 0x60);
    put_word(image, 0x50, 0x01000208u);
    // Entry 0x60 "file2": a file with a size and no items.
    put_word(image, 0x60, 0x01000210u);
    put_word(image, 0x64, 1);
    put_word(image, 0x68, 0x1234);
    // Entry 0x70 "file1": one item that points at zeros and must not become
    // a child.
    put_word(image, 0x70, 0x01000208u);
    put_word(image, 0x74, 2);
    put_word(image, 0x78, 0x8000);
    put_word(image, 0x7C, 0x300);
    return image;
}

} // namespace

int main(int argc, char** argv) {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };

    // The synthetic volume: the header, the names, the child classification
    // and the path lookup.
    {
        auto source = make_memory_disc_source(make_synthetic_volume());
        Gt4Volume volume(source.get(), 0, 0x400);
        check(volume.version() == 0x00020002u, "the synthetic version reads");
        check(volume.header_value(0) == 1 && volume.header_value(1) == 2
                  && volume.header_value(2) == 3,
              "the synthetic header values read");
        const std::vector<Gt4Volume::Entry>& root = volume.root();
        check(root.size() == 2 && root[0].name == "dir1"
                  && root[1].name == "file1",
              "the synthetic root lists its two children");
        const std::vector<Gt4Volume::Entry>& dir1 = volume.children(root[0]);
        check(dir1.size() == 1 && dir1[0].name == "file2"
                  && dir1[0].value == 0x1234,
              "a directory keeps only real child entries");
        check(volume.children(root[1]).empty() && root[1].value == 0x8000,
              "an item that is not a child entry is not followed");
        const Gt4Volume::Entry* file = volume.find("DIR1/File2");
        check(file != nullptr && file->value == 0x1234,
              "the lookup finds a nested file case-insensitively");
        check(volume.find("dir1/nope") == nullptr
                  && volume.find("nope") == nullptr,
              "unknown paths answer nothing");
        check(volume.name_at(0x01000200u) == "dir1",
              "a name pointer decodes through XOR 0xFF");
    }

    // A non-volume image is rejected loudly.
    {
        std::vector<std::uint8_t> junk(0x100, 0x5A);
        auto source = make_memory_disc_source(std::move(junk));
        bool threw = false;
        try {
            Gt4Volume volume(source.get(), 0, 0x100);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "a bad magic is rejected");
    }

    // The pinned archive, when the caller named the ISO: the root's names
    // and the movie entry the game's loader asks for.
    if (argc > 1) {
        try {
            Iso9660Image disc(open_disc_file(argv[1]));
            DiscFileSliceSource volume_source(&disc, "cdrom0:\\GT4.VOL;1");
            check(volume_source.size() == 2459502592ull
                      && disc.file_extent("cdrom0:\\GT4.VOL;1") == 105879,
                  "the pinned GT4.VOL size and extent read");
            Gt4Volume volume(&volume_source, 0, volume_source.size());
            check(volume.version() == 0x00020002u, "the pinned version reads");
            const std::vector<Gt4Volume::Entry>& root = volume.root();
            bool has_advertise = false;
            bool has_mpeg = false;
            bool has_bgm = false;
            for (const Gt4Volume::Entry& entry : root) {
                has_advertise = has_advertise || entry.name == "advertise";
                has_mpeg = has_mpeg || entry.name == "mpeg";
                has_bgm = has_bgm || entry.name == "bgm";
            }
            check(root.size() == 22 && has_advertise && has_mpeg && has_bgm,
                  "the pinned root lists its 22 categories");
            const Gt4Volume::Entry* mpeg = volume.find("mpeg");
            check(mpeg != nullptr && volume.children(*mpeg).size() == 1
                      && volume.children(*mpeg)[0].name == "gt4",
                  "the pinned mpeg directory holds the gt4 subdirectory");
            const Gt4Volume::Entry* movie = volume.find("mpeg/gt4/mv0010");
            check(movie != nullptr && movie->value == 0x01200004u,
                  "the pinned movie entry resolves with its byte size");
            const Gt4Volume::Entry* gtloading = volume.find("advertise/gtloading.img");
            check(gtloading != nullptr && gtloading->value == 0x21A0u,
                  "the pinned module container resolves with its size");
        } catch (const std::exception& error) {
            std::cerr << "pinned volume check failed: " << error.what() << '\n';
            ++failures;
        }
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "the GT4.VOL reader decodes names, children and file sizes\n";
    return 0;
}
