#include "gt4recomp/ee_disassemble.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

int main() {
    using gt4recomp::ee::format_instruction;
    using gt4recomp::ee::disassemble_region;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };
    struct Case { std::uint32_t word, pc; const char* expected; };
    const Case cases[] = {
        {0x3044000f, 0, "andi a0, v0, 0xf"},
        {0x33ffffff, 0, "andi ra, ra, 0xffff"},
        {0x3484fff0, 0, "ori a0, a0, 0xfff0"},
        {0x34028000, 0, "ori v0, zero, 0x8000"},
        {0x27bdfff0, 0, "addiu sp, sp, -0x10"},
        {0x24088000, 0, "addiu t0, zero, -0x8000"},
        {0x8603000c, 0, "lh v1, 0xc(s0)"},
        {0x012a4021, 0, "addu t0, t1, t2"},
        {0x012a4023, 0, "subu t0, t1, t2"},
        {0x012a4024, 0, "and t0, t1, t2"},
        {0x012a4025, 0, "or t0, t1, t2"},
        {0x012a4026, 0, "xor t0, t1, t2"},
        {0x012a402a, 0, "slt t0, t1, t2"},
        {0x012a402b, 0, "sltu t0, t1, t2"},
        {0x012a402d, 0, "daddu t0, t1, t2"},
        {0x3c1fffff, 0, "lui ra, 0xffff"},
        {0x8fa80010, 0, "lw t0, 0x10(sp)"},
        {0xafa8fffc, 0, "sw t0, -0x4(sp)"},
        {0xdfb00000, 0, "ld s0, 0x0(sp)"},
        {0xffbf0000, 0, "sd ra, 0x0(sp)"},
        {0xa0400000, 0, "sb zero, 0x0(v0)"},
        {0x80820003, 0, "lb v0, 0x3(a0)"},
        {0x90820003, 0, "lbu v0, 0x3(a0)"},
        {0x00022103, 0, "sra a0, v0, 0x4"},
        {0x2c820006, 0, "sltiu v0, a0, 0x6"},
        {0x28820006, 0, "slti v0, a0, 0x6"},
        {0x38420054, 0, "xori v0, v0, 0x54"},
        {0x11090003, 0x100090, "beq t0, t1, 0x001000a0"},
        {0x1509fffe, 0x1000a0, "bne t0, t1, 0x0010009c"},
        {0x1109fffe, 0, "beq t0, t1, 0xfffffffc"},
        {0x11090000, 0xfffffffc, "beq t0, t1, 0x00000000"},
        {0x50400004, 0x100100, "beql v0, zero, 0x00100114"},
        {0x54600005, 0x100100, "bnel v1, zero, 0x00100118"},
        {0x1840000c, 0x100100, "blez v0, 0x00100134"},
        {0x1d200004, 0x100100, "bgtz t1, 0x00100114"},
        {0x06000009, 0x100100, "bltz s0, 0x00100128"},
        {0x05210004, 0x100100, "bgez t1, 0x00100114"},
        {0x05220004, 0x100100, "bltzl t1, 0x00100114"},
        {0x0603fffc, 0x100100, "bgezl s0, 0x001000f4"},
        {0x05300004, 0x100100, "bltzal t1, 0x00100114"},
        {0x05310004, 0x100100, "bgezal t1, 0x00100114"},
        {0x05320004, 0x100100, "bltzall t1, 0x00100114"},
        {0x05330004, 0x100100, "bgezall t1, 0x00100114"},
        {0x08040000, 0x0ffffffc, "j 0x10100000"},
        {0x0c040000, 0xfffffffc, "jal 0x00100000"},
        {0x03e00008, 0, "jr ra"},
        {0x0040f809, 0, "jalr ra, v0"},
        {0x0000000c, 0, "syscall"},
        {0x000048cc, 0, "syscall 0x123"},
        {0x00094100, 0, "sll t0, t1, 0x4"},
        {0x001fffc2, 0, "srl ra, ra, 0x1f"},
        {0x0000040f, 0, "sync 0x10"},
        {0x70000c28, 0, "padduw at, zero, zero"},
        {0x44800000, 0, "mtc1 zero, f0"},
        {0x44c0f800, 0, "ctc1 zero, fcsr"},
        {0x46010018, 0, "adda.s f0, f0, f1"},
        {0x46010080, 0, "add.s f2, f0, f1"},
        {0x45010002, 0x100100, "bc1t 0x0010010c"},
        {0xc4800010, 0, "lwc1 f0, 0x10(a0)"},
        {0xe4800010, 0, "swc1 f0, 0x10(a0)"},
        {0x00001010, 0, "mfhi v0"},
        {0x04190000, 0, "mtsah zero, 0x0"},
        {0x71281c08, 0, "paddsw v1, t1, t0"},
        {0x7c400000, 0, "sq zero, 0x0(v0)"},
        {0x78220000, 0, "lq v0, 0x0(at)"},
        {0, 0, "sll zero, zero, 0x0"},
        {0x70000000, 0, "unsupported 0x70000000 ; opcode=0x1c function=0x00"},
    };
    for (const auto& item : cases) {
        const auto actual = format_instruction(item.word, item.pc);
        if (actual != item.expected) {
            std::cerr << "word 0x" << std::hex << item.word << std::dec << ": expected \""
                      << item.expected << "\", got \"" << actual << "\"\n";
            ++failures;
        }
    }
    gt4recomp::ImageRecord text{0x100000, {0xf0, 0xff, 0xbd, 0x27, 0, 0, 0, 0x70}};
    std::ostringstream listing, report;
    disassemble_region(text, 0x100000, 2, listing, report);
    check(listing.str() == "00100000: 27bdfff0  addiu sp, sp, -0x10\n"
                          "00100004: 70000000  unsupported 0x70000000 ; opcode=0x1c function=0x00\n",
          "complete sequential listing");
    check(report.str() == "region=0x00100000 words=2 supported=1 unsupported=1\n"
                         "unsupported opcode=0x1c function=0x00 count=1\n", "opcode report");
    for (const auto& [start, count] : {
        std::pair{0x100000u, 0u}, {0x100001u, 1u}, {0xffffcu, 1u},
        {0x100004u, 2u}, {0x100008u, 1u}, {0x100000u, 0xffffffffu}}) {
        std::ostringstream invalid_listing, invalid_report;
        bool rejected = false;
        try { disassemble_region(text, start, count, invalid_listing, invalid_report); }
        catch (const std::runtime_error&) { rejected = true; }
        check(rejected && invalid_listing.str().empty() && invalid_report.str().empty(),
              "bad range rejected before output");
    }
    gt4recomp::ImageRecord top{0xfffffffcu, {0, 0, 0, 0}};
    std::ostringstream top_listing, top_report;
    disassemble_region(top, 0xfffffffcu, 1, top_listing, top_report);
    check(top_listing.str().starts_with("fffffffc:"), "last address does not wrap traversal");
    std::ostringstream failed_listing, unused_report;
    failed_listing.setstate(std::ios::badbit);
    bool write_rejected = false;
    try { disassemble_region(text, 0x100000, 1, failed_listing, unused_report); }
    catch (const std::runtime_error&) { write_rejected = true; }
    check(write_rejected, "failed output stream rejected");
    return failures == 0 ? 0 : 1;
}
