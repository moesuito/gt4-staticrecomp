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
        {0x012a4020, 0, "add t0, t1, t2"},
        {0x012a4022, 0, "sub t0, t1, t2"},
        {0x012a402c, 0, "dadd t0, t1, t2"},
        {0x012a402e, 0, "dsub t0, t1, t2"},
        {0x21280001, 0, "addi t0, t1, 0x1"},
        {0x61280001, 0, "daddi t0, t1, 0x1"},
        {0x71095409, 0, "pmaddh t2, t0, t1"},
        {0x704e0709, 0, "pmulth zero, v0, t6"},
        {0x71095329, 0, "pmultuw t2, t0, t1"},
        {0x012a4023, 0, "subu t0, t1, t2"},
        {0x012a4024, 0, "and t0, t1, t2"},
        {0x012a4025, 0, "or t0, t1, t2"},
        {0x012a4026, 0, "xor t0, t1, t2"},
        {0x012a402a, 0, "slt t0, t1, t2"},
        {0x012a402b, 0, "sltu t0, t1, t2"},
        {0x012a402d, 0, "daddu t0, t1, t2"},
        {0x012a402f, 0, "dsubu t0, t1, t2"},
        {0x012a400a, 0, "movz t0, t1, t2"},
        {0x012a400b, 0, "movn t0, t1, t2"},
        {0x012a4027, 0, "nor t0, t1, t2"},
        {0x58400004, 0x100100, "blezl v0, 0x00100114"},
        {0x5c400005, 0x100100, "bgtzl v0, 0x00100118"},
        {0x66100001, 0, "daddiu s0, s0, 0x1"},
        {0x3c1fffff, 0, "lui ra, 0xffff"},
        {0x8fa80010, 0, "lw t0, 0x10(sp)"},
        {0xafa8fffc, 0, "sw t0, -0x4(sp)"},
        {0xdfb00000, 0, "ld s0, 0x0(sp)"},
        {0xffbf0000, 0, "sd ra, 0x0(sp)"},
        {0xa0400000, 0, "sb zero, 0x0(v0)"},
        {0x80820003, 0, "lb v0, 0x3(a0)"},
        {0x90820003, 0, "lbu v0, 0x3(a0)"},
        {0x00022103, 0, "sra a0, v0, 0x4"},
        {0x00094138, 0, "dsll t0, t1, 0x4"},
        {0x0009413c, 0, "dsll32 t0, t1, 0x4"},
        {0x01494004, 0, "sllv t0, t1, t2"},
        {0x01494017, 0, "dsrav t0, t1, t2"},
        {0x0182c807, 0, "srav t9, v0, t4"},
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
        {0x9c820000, 0, "lwu v0, 0x0(a0)"},
        {0x94820000, 0, "lhu v0, 0x0(a0)"},
        {0xa4820000, 0, "sh v0, 0x0(a0)"},
        {0x88830003, 0, "lwl v1, 0x3(a0)"},
        {0x98830003, 0, "lwr v1, 0x3(a0)"},
        {0xa8830003, 0, "swl v1, 0x3(a0)"},
        {0xb8830003, 0, "swr v1, 0x3(a0)"},
        {0xbd180000, 0, "cache 0x18, 0x0(t0)"},
        {0xcca00040, 0, "pref 0x00, 0x40(a1)"},
        {0x68a30007, 0, "ldl v1, 0x7(a1)"},
        {0x6c830007, 0, "ldr v1, 0x7(a0)"},
        {0xb0830007, 0, "sdl v1, 0x7(a0)"},
        {0xb4830000, 0, "sdr v1, 0x0(a0)"},
        {0x40026000, 0, "mfc0 v0, Status"},
        {0x40826000, 0, "mtc0 v0, Status"},
        {0x40036000, 0, "mfc0 v1, Status"},
        {0x42000038, 0, "ei"},
        {0x42000039, 0, "di"},
        {0x42000018, 0, "eret"},
        {0x48a80800, 0, "qmtc2 t0, vf1"},
        {0x48290800, 0, "qmfc2 t1, vf1"},
        {0x484aa000, 0, "cfc2 t2, vi20"},
        {0x48cab000, 0, "ctc2 t2, vi22"},
        {0xd8420000, 0, "lqc2 vf2, 0x0(v0)"},
        {0xf8410010, 0, "sqc2 vf1, 0x10(v0)"},
        {0x4a0002ff, 0, "vnop"},
        // VU macro arithmetic: hand-assembled forms the interpreter test also
        // executes, plus observed words from the pinned text with their masks
        // and operand elements shown.
        {0x4BE20928, 0, "vadd vf4, vf1, vf2"},
        {0x4BE1112C, 0, "vsub vf4, vf2, vf1"},
        {0x4BE3094B, 0, "vmaddw vf5, vf1, vf3"},
        {0x4BE0F84B, 0, "vmaddw vf1, vf31, vf0"},
        {0x4BE311BC, 0, "vmulax acc, vf2, vf3"},
        {0x4BE1E1BC, 0, "vmulax acc, vf28, vf1"},
        {0x4B010841, 0, "vaddy.x vf1, vf1, vf1"},
        {0x4BE6297D, 0, "vftoi4 vf6, vf5"},
        {0x4BE7333C, 0, "vmove vf7, vf6"},
        {0x4BF8A33C, 0, "vmove vf24, vf20"},
        {0x4BE20BBC, 0, "vdiv Q, vf1.w, vf2.w"},
        {0x4A6103BE, 0, "vrsqrt Q, vf0.w, vf1.x"},
        {0x4BC532FE, 0, "vopmula acc, vf6, vf5"},
        {0x4BE110F0, 0, "viadd vi3, vi2, vi1"},
        {0x4BE31432, 0, "viaddi vi3, vi2, -0x10"},
        {0x4A0003BF, 0, "vwaitq"},
        {0x0000000d, 0, "break"},
        {0x000001cd, 0, "break 0x7"},
        {0x012a0018, 0, "mult t1, t2"},
        {0x012a0019, 0, "multu t1, t2"},
        {0x012a001a, 0, "div t1, t2"},
        {0x012a001b, 0, "divu t1, t2"},
        {0x70000000, 0, "madd zero, zero"},
        {0x70000004, 0, "plzcw zero, zero"},
        {0x70430018, 0, "mult1 v0, v1"},
        {0x7043001a, 0, "div1 v0, v1"},
        {0x70430020, 0, "madd1 v0, v1"},
        {0, 0, "sll zero, zero, 0x0"},
        {0x70000002, 0, "unsupported 0x70000002 ; opcode=0x1c function=0x02"},
    };
    for (const auto& item : cases) {
        const auto actual = format_instruction(item.word, item.pc);
        if (actual != item.expected) {
            std::cerr << "word 0x" << std::hex << item.word << std::dec << ": expected \""
                      << item.expected << "\", got \"" << actual << "\"\n";
            ++failures;
        }
    }
    gt4recomp::ImageRecord text{0x100000, {0xf0, 0xff, 0xbd, 0x27, 0x02, 0, 0, 0x70}};
    std::ostringstream listing, report;
    disassemble_region(text, 0x100000, 2, listing, report);
    check(listing.str() == "00100000: 27bdfff0  addiu sp, sp, -0x10\n"
                          "00100004: 70000002  unsupported 0x70000002 ; opcode=0x1c function=0x02\n",
          "complete sequential listing");
    check(report.str() == "region=0x00100000 words=2 supported=1 unsupported=1\n"
                         "unsupported opcode=0x1c function=0x02 count=1\n", "opcode report");
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
