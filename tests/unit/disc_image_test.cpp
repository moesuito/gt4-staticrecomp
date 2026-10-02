// Unit tests for the disc image readers: a synthetic ISO9660 image built in
// memory exercises the directory walk, the path normalization and the read
// tail; when the pinned ISO is available (argv[1]) the same reader must find
// the real files and return the bytes of an IOP module (the ELF magic).
#include "gt4recomp/disc_image.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gt4recomp;

namespace {

constexpr std::uint32_t block_size = 2048;

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint16_t value) {
    bytes[offset + 0] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint32_t value) {
    bytes[offset + 0] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

// Appends one ISO9660 directory record to a directory block.
void add_record(std::vector<std::uint8_t>& image, std::uint32_t block,
                const std::string& name, std::uint32_t extent,
                std::uint32_t size, bool directory) {
    const std::size_t offset = static_cast<std::size_t>(block) * block_size;
    std::size_t cursor = offset;
    while (image[cursor] != 0) {
        cursor += image[cursor];
    }
    const std::uint8_t length =
        static_cast<std::uint8_t>(33 + name.size() + (name.size() % 2 == 0 ? 1 : 0));
    image[cursor] = length;
    image[cursor + 1] = 0;
    put_u32(image, cursor + 2, extent);
    put_u32(image, cursor + 6, extent);
    put_u32(image, cursor + 10, size);
    put_u32(image, cursor + 14, size);
    image[cursor + 25] = directory ? 0x02 : 0x00;
    image[cursor + 32] = static_cast<std::uint8_t>(name.size());
    std::memcpy(image.data() + cursor + 33, name.data(), name.size());
}

std::vector<std::uint8_t> make_synthetic_iso() {
    // Blocks 0-15: the system area. 16: the primary descriptor. 20: the root
    // directory. 21: IRX. 22: TEST.TXT. 23: SIO2MAN.IRX.
    std::vector<std::uint8_t> image(24 * block_size, 0);
    const std::size_t pvd = 16 * block_size;
    image[pvd + 0] = 1;
    std::memcpy(image.data() + pvd + 1, "CD001", 5);
    image[pvd + 6] = 1;
    put_u16(image, pvd + 128, block_size);
    put_u32(image, pvd + 80, 24);
    // ISO9660 stores the volume space size twice; the sector reader checks
    // both copies.
    image[pvd + 84] = 0;
    image[pvd + 85] = 0;
    image[pvd + 86] = 0;
    image[pvd + 87] = 24;
    const std::size_t root = pvd + 156;
    image[root + 0] = 34;
    put_u32(image, root + 2, 20);
    put_u32(image, root + 10, block_size);
    image[root + 25] = 0x02;
    image[root + 32] = 1;
    image[root + 33] = 0;

    add_record(image, 20, "\0", 20, block_size, true);   // "."
    add_record(image, 20, "\1", 20, block_size, true);   // ".."
    add_record(image, 20, "IRX", 21, block_size, true);
    add_record(image, 20, "TEST.TXT;1", 22, 10, false);
    add_record(image, 21, "\0", 21, block_size, true);
    add_record(image, 21, "\1", 20, block_size, true);
    add_record(image, 21, "SIO2MAN.IRX;1", 23, 6, false);
    const char text[] = "0123456789";
    std::memcpy(image.data() + 22 * block_size, text, 10);
    const char module[] = "ABCDEF";
    std::memcpy(image.data() + 23 * block_size, module, 6);
    return image;
}

std::vector<std::uint8_t> make_synthetic_dual_layer_image() {
    // Two ISO9660 volumes: the first occupies the file's blocks 0..31 and
    // declares its size (32), so the second volume's logical block 0 is 32.
    // The file stores the second volume from its own block 16 on, sixteen
    // blocks early — its system area is left out — so its descriptor sits at
    // the file's block 32 where the logical block is 48. The file ends at
    // block 64 and the second volume declares 48 blocks, so the volumes tile
    // the logical image exactly (32 + 48 == 64 + 16).
    std::vector<std::uint8_t> image(64 * block_size, 0);
    const auto write_descriptor = [&image](std::size_t block, std::uint32_t size) {
        const std::size_t pvd = block * block_size;
        image[pvd + 0] = 1;
        std::memcpy(image.data() + pvd + 1, "CD001", 5);
        image[pvd + 6] = 1;
        put_u16(image, pvd + 128, block_size);
        put_u32(image, pvd + 80, size);
        image[pvd + 84] = static_cast<std::uint8_t>(size >> 24);
        image[pvd + 85] = static_cast<std::uint8_t>(size >> 16);
        image[pvd + 86] = static_cast<std::uint8_t>(size >> 8);
        image[pvd + 87] = static_cast<std::uint8_t>(size);
    };
    write_descriptor(16, 32);  // the first volume's descriptor
    write_descriptor(32, 48);  // the second volume's, sixteen blocks early
    // Markers the mapping tests look for.
    image[32 * block_size + 64] = 'S';  // the second volume's descriptor
    image[33 * block_size + 5] = 'T';   // the logical block 49
    image[40 * block_size + 0] = 'B';   // the logical block 56
    return image;
}

} // namespace

int main(int argc, char** argv) {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };

    // The synthetic image: the directory walk, the game's path spelling and
    // the read behavior.
    {
        Iso9660Image image(make_memory_disc_source(make_synthetic_iso()));
        const std::vector<std::string> root = image.directory_names("");
        check(root.size() == 2 && root[0] == "IRX" && root[1] == "TEST.TXT;1",
              "the synthetic root lists its two entries");
        const std::vector<std::string> irx = image.directory_names("cdrom0:\\IRX");
        check(irx.size() == 1 && irx[0] == "SIO2MAN.IRX;1",
              "the synthetic IRX directory lists its module");
        check(image.file_size("cdrom0:\\TEST.TXT;1") == 10
                  && image.file_size("cdrom0:\\irx\\sio2man.irx;1") == 6
                  && image.file_size("cdrom0:\\IRX\\SIO2MAN.IRX") == 6,
              "file sizes resolve in the game's spellings");
        check(image.file_size("cdrom0:\\NOPE.TXT;1") == 0
                  && image.file_size("cdrom0:\\IRX") == 0,
              "unknown paths and directories answer zero");
        std::vector<std::uint8_t> buffer(4, 0xFF);
        image.read_file("cdrom0:\\TEST.TXT;1", 3, buffer);
        check(buffer[0] == '3' && buffer[1] == '4' && buffer[2] == '5'
                  && buffer[3] == '6',
              "a file read returns the file's bytes at the offset");
        std::vector<std::uint8_t> tail(4, 0xFF);
        image.read_file("cdrom0:\\TEST.TXT;1", 8, tail);
        check(tail[0] == '8' && tail[1] == '9' && tail[2] == 0 && tail[3] == 0,
              "a read past the end zero-fills the tail");
        bool threw = false;
        try {
            std::vector<std::uint8_t> past(4, 0);
            image.read_file("cdrom0:\\TEST.TXT;1", 11, past);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "an offset past the file's end is rejected");
        threw = false;
        try {
            std::vector<std::uint8_t> byte(1, 0);
            image.read_file("cdrom0:\\NOPE.TXT;1", 0, byte);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "reading an unknown file is rejected");
    }

    // A non-ISO image is rejected loudly.
    {
        std::vector<std::uint8_t> junk(32 * block_size, 0x5A);
        bool threw = false;
        try {
            Iso9660Image image(make_memory_disc_source(std::move(junk)));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "a non-ISO image is rejected");
    }

    // The logical sector reader: a single-volume image maps blocks straight
    // through.
    {
        DiscSectors sectors(make_memory_disc_source(make_synthetic_iso()));
        check(sectors.second_volume_lba() == 0
                  && sectors.second_volume_shift() == 0,
              "a single-volume image reports no second volume");
        check(sectors.size() == 24 * block_size,
              "a single-volume image keeps the file's logical size");
        std::vector<std::uint8_t> descriptor(6, 0);
        sectors.read(16 * block_size, descriptor);
        check(descriptor[0] == 1 && descriptor[1] == 'C' && descriptor[5] == '1',
              "a single-volume read lands on the file's own block");
    }

    // The dual-layer image: the second volume's logical blocks map sixteen
    // blocks back into the file and the logical size grows by the shift.
    {
        DiscSectors sectors(
            make_memory_disc_source(make_synthetic_dual_layer_image()));
        check(sectors.second_volume_lba() == 32
                  && sectors.second_volume_shift() == 16,
              "the dual-layer image derives the second volume and its shift");
        check(sectors.size() == 80 * block_size,
              "the dual-layer logical size includes the left-out blocks");
        std::vector<std::uint8_t> descriptor(6, 0);
        sectors.read(48 * block_size, descriptor);
        check(descriptor[0] == 1 && descriptor[1] == 'C' && descriptor[5] == '1',
              "the second volume's descriptor reads through the mapping");
        std::vector<std::uint8_t> marker(1, 0);
        sectors.read(49 * block_size + 5, marker);
        check(marker[0] == 'T',
              "a second-volume block one past the descriptor maps");
        sectors.read(56 * block_size, marker);
        check(marker[0] == 'B', "a second-volume data block maps sixteen back");
    }

    // An image whose tail is not a second volume stops loudly.
    {
        std::vector<std::uint8_t> image = make_synthetic_iso();
        image.resize(image.size() + 4 * block_size, 0);
        bool threw = false;
        try {
            DiscSectors sectors(make_memory_disc_source(std::move(image)));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "an unexplained tail is rejected");
    }

    // The pinned ISO, when the caller named it: the real root, the real IRX
    // modules and the ELF magic at the start of one of them.
    if (argc > 1) {
        try {
            Iso9660Image image(open_disc_file(argv[1]));
            const std::vector<std::string> root = image.directory_names("");
            bool has_irx = false;
            bool has_volume = false;
            for (const std::string& name : root) {
                has_irx = has_irx || name == "IRX";
                has_volume = has_volume || name == "GT4.VOL;1";
            }
            check(has_irx && has_volume,
                  "the pinned ISO's root holds IRX and GT4.VOL");
            const std::uint64_t module_size =
                image.file_size("cdrom0:\\IRX\\SIO2MAN.IRX;1");
            check(module_size > 0, "the pinned ISO holds SIO2MAN.IRX");
            std::vector<std::uint8_t> magic(4, 0);
            image.read_file("cdrom0:\\IRX\\SIO2MAN.IRX;1", 0, magic);
            check(magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L'
                      && magic[3] == 'F',
                  "SIO2MAN.IRX starts with the IOP ELF magic");
            const std::uint64_t volume_size =
                image.file_size("cdrom0:\\GT4.VOL;1");
            check(volume_size == 2459502592ull,
                  "GT4.VOL has the size the directory record declares");

            // The same image through the sector reader: the pinned disc is
            // dual-layer and its second volume is stored sixteen blocks early.
            {
                DiscSectors sectors(open_disc_file(argv[1]));
                const std::uint64_t raw_size = open_disc_file(argv[1])->size();
                check(sectors.second_volume_lba() == 0x1418C0u
                          && sectors.second_volume_shift() == 16,
                      "the pinned disc's second volume is derived at 0x1418C0");
                check(sectors.size() == raw_size + 16 * block_size,
                      "the pinned disc's logical size adds the left-out blocks");
                std::vector<std::uint8_t> descriptor(6, 0);
                sectors.read(0x1418D0ull * block_size, descriptor);
                check(descriptor[0] == 1 && descriptor[1] == 'C'
                          && descriptor[5] == '1',
                      "the pinned second descriptor reads at its logical block");
                std::vector<std::uint8_t> record(34, 0);
                sectors.read(0x1419C5ull * block_size, record);
                check(record[0] == 0x30 && record[32] == 1 && record[33] == 0,
                      "the pinned second root holds its self record");
                std::vector<std::uint8_t> magic(4, 0);
                sectors.read(0x1419CBull * block_size, magic);
                check(magic[0] == 0xAD && magic[1] == 0x90 && magic[2] == 0xB9
                          && magic[3] == 0xAC,
                      "GT4L1.VOL's archive header reads at its logical block");
            }
        } catch (const std::exception& error) {
            std::cerr << "pinned ISO check failed: " << error.what() << '\n';
            ++failures;
        }
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "the disc image readers resolve files and read their bytes\n";
    return 0;
}
