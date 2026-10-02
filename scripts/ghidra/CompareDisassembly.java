// Compare an M6 listing with independent decoding of the imported native ELF.
// Usage: -postScript CompareDisassembly.java <listing.txt> <comparison.tsv>
// Game words and assembly are written only to the caller's private report.
// @category GT4Recomp

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.PseudoDisassembler;
import ghidra.app.util.PseudoInstruction;
import ghidra.program.model.address.Address;

public class CompareDisassembly extends GhidraScript {
    // Mnemonics from the R5900's extensions that the base MIPS64 language
    // cannot express: MMI, the FPU accumulator forms and RSQRT, the shift
    // cache, the second HI/LO bank, and LQ/SQ (whose opcode the MIPS64r2
    // SPECIAL3 encoding reuses). Agreement is still counted when Ghidra
    // happens to decode the word the same way; disagreement is expected and
    // those instructions are verified against the reference implementation's
    // tables instead.
    private static final Set<String> R5900_ONLY = Set.of(
        "mfhi1", "mthi1", "mflo1", "mtlo1", "mtsa", "mtsab", "mtsah",
        "adda.s", "suba.s", "mula.s", "madda.s", "msuba.s", "madd.s", "msub.s", "rsqrt.s",
        "lq", "sq",
        "paddw", "psubw", "paddh", "psubh", "paddb", "psubb",
        "paddsw", "psubsw", "paddsh", "psubsh", "paddsb", "psubsb",
        "padduw", "psubuw", "padduh", "psubuh", "paddub", "psubub",
        "pcgtw", "pcgth", "pcgtb", "pceqw", "pceqh", "pceqb",
        "pmaxw", "pmaxh", "pminw", "pminh", "pabsw", "pabsh",
        "pand", "por", "pxor", "pnor",
        "psllh", "psrlh", "psrah", "psllw", "psrlw", "psraw",
        "psllvw", "psrlvw", "psravw",
        "pextlw", "pextlh", "pextlb", "pextuw", "pextuh", "pextub",
        "ppacw", "ppach", "ppacb", "pext5", "ppac5", "padsbh",
        "pinth", "pinteh", "pcpyld", "pcpyud", "pcpyh",
        "pexeh", "prevh", "pexew", "pexch", "pexcw", "prot3w",
        "pmfhi", "pmflo", "pmthi", "pmtlo", "pmfhl", "pmthl", "qfsrv",
        "pmaddh", "pmsubh", "pmulth", "pmultw", "pmadduw", "pmultuw", "pdivw", "pdivuw",
        "qmfc2", "qmtc2", "cfc2", "ctc2", "lqc2", "sqc2", "vnop",
        "vaddx", "vaddy", "vaddz", "vaddw", "vsubx", "vsuby", "vsubz", "vsubw",
        "vmaddx", "vmaddy", "vmaddz", "vmaddw", "vmsubx", "vmsuby", "vmsubz", "vmsubw",
        "vmaxx", "vmaxy", "vmaxz", "vmaxw", "vminix", "vminiy", "vminiz", "vminiw",
        "vmulx", "vmuly", "vmulz", "vmulw", "vmulq", "vmaxi", "vmuli", "vminii",
        "vaddq", "vmaddq", "vaddi", "vmaddi", "vsubq", "vmsubq", "vsubi", "vmsubi",
        "vadd", "vmadd", "vmul", "vmax", "vsub", "vmsub", "vopmsub", "vmini",
        "viadd", "visub", "viaddi", "viand", "vior",
        "vaddax", "vadday", "vaddaz", "vaddaw", "vsubax", "vsubay", "vsubaz", "vsubaw",
        "vmaddax", "vmadday", "vmaddaz", "vmaddaw", "vmsubax", "vmsubay", "vmsubaz", "vmsubaw",
        "vitof0", "vitof4", "vitof12", "vitof15",
        "vftoi0", "vftoi4", "vftoi12", "vftoi15",
        "vmulax", "vmulay", "vmulaz", "vmulaw", "vmulaq", "vabs", "vmulai", "vclipw",
        "vaddaq", "vmaddaq", "vaddai", "vmaddai", "vsubaq", "vmsubaq", "vsubai", "vmsubai",
        "vadda", "vmadda", "vmula", "vsuba", "vmsuba", "vopmula",
        "vmove", "vmr32", "vdiv", "vsqrt", "vrsqrt", "vwaitq", "vmtir", "vmfir",
        "vrnext", "vrget", "vrinit", "vrxor");

    private String normalize(String assembly) {
        String normalized = assembly.toLowerCase(Locale.ROOT).replaceAll("\\s+", "");
        Matcher hexadecimal = Pattern.compile("0x[0-9a-f]+").matcher(normalized);
        StringBuilder result = new StringBuilder();
        while (hexadecimal.find()) {
            String value = hexadecimal.group().substring(2);
            hexadecimal.appendReplacement(result, "0x" + Long.toHexString(Long.parseLong(value, 16)));
        }
        hexadecimal.appendTail(result);
        return result.toString();
    }

    private String expandAlias(PseudoInstruction instruction, int word) {
        String name = instruction.getMnemonicString().toLowerCase(Locale.ROOT);
        List<String> operands = new ArrayList<>();
        for (int index = 0; index < instruction.getNumOperands(); index++) {
            operands.add(instruction.getDefaultOperandRepresentation(index));
        }
        if (name.equals("nop") && word == 0) {
            return "sll zero,zero,0x0";
        }
        if (name.equals("li") && operands.size() == 2) {
            int opcode = word >>> 26;
            if (opcode == 9 || opcode == 13) {
                return (opcode == 9 ? "addiu " : "ori ")
                    + operands.get(0) + ",zero," + operands.get(1);
            }
        }
        if ((name.equals("beqz") || name.equals("bnez")) && operands.size() == 2) {
            return (name.equals("beqz") ? "beq " : "bne ")
                + operands.get(0) + ",zero," + operands.get(1);
        }
        if (name.equals("b") && operands.size() == 1 && (word >>> 26) == 4) {
            return "beq zero,zero," + operands.get(0);
        }
        // clear rd is Ghidra's alias for daddu rd,zero,zero (funct 0x2d).
        if (name.equals("clear") && operands.size() == 1 && (word & 0x3f) == 0x2d) {
            return "daddu " + operands.get(0) + ",zero,zero";
        }
        // jalr rs omits the default link register; expand when the encoding has rd = ra.
        if (name.equals("jalr") && operands.size() == 1 && ((word >>> 11) & 0x1f) == 31) {
            return "jalr ra," + operands.get(0);
        }
        return instruction.toString();
    }

    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 2) {
            throw new IllegalArgumentException("Supply listing and private comparison output paths");
        }
        if (!currentProgram.getLanguageID().toString().equals("MIPS:LE:64:64-32addr")) {
            throw new IllegalStateException("Expected the documented little-endian MIPS language");
        }
        if (!currentProgram.getExecutableSHA256().equals(
                "10f82e2231a51404b95682ed3ea81171100a1af2fefdeed3391943016c7c935c")) {
            throw new IllegalStateException("Import the pinned M4 native analysis ELF");
        }
        PseudoDisassembler disassembler = new PseudoDisassembler(currentProgram);
        List<String> report = new ArrayList<>();
        report.add("address\tword\tours\tghidra\tresult");
        int matched = 0;
        int nonNopMatched = 0;
        int unsupported = 0;
        int mismatched = 0;
        int r5900Only = 0;
        Set<Long> addresses = new HashSet<>();
        for (String line : Files.readAllLines(Path.of(arguments[0]))) {
            String[] columns = line.split("\\s+", 3);
            long pc = Long.parseLong(columns[0].replace(":", ""), 16);
            if (!addresses.add(pc)) {
                throw new IllegalStateException("Duplicate sample address");
            }
            int word = (int) Long.parseLong(columns[1], 16);
            Address address = toAddr(pc);
            if (currentProgram.getMemory().getInt(address) != word) {
                throw new IllegalStateException("ELF/listing bytes differ at " + address);
            }
            if (columns[2].startsWith("unsupported ")) {
                unsupported++;
                report.add(String.format("%08x\t%08x\t%s\t\tunsupported", pc, word, columns[2]));
                continue;
            }
            // Decode one instruction without following flows through EE-only opcodes.
            PseudoInstruction reference = disassembler.disassemble(address);
            String referenceText = reference == null ? "<undecodable>" : expandAlias(reference, word);
            boolean agrees = normalize(columns[2]).equals(normalize(referenceText));
            String ourMnemonic = columns[2].split("\\s+", 2)[0].toLowerCase(Locale.ROOT);
            String result;
            if (agrees) {
                matched++;
                if (word != 0) { nonNopMatched++; }
                result = "match";
            } else if (R5900_ONLY.contains(ourMnemonic)) {
                // Expected: the base language cannot represent this R5900
                // instruction; the reference tables cover it instead.
                r5900Only++;
                result = "r5900-only";
            } else {
                mismatched++;
                result = "MISMATCH";
            }
            report.add(String.format("%08x\t%08x\t%s\t%s\t%s", pc, word, columns[2],
                reference == null ? "<undecodable>" : reference.toString(), result));
        }
        Files.write(Path.of(arguments[1]), report);
        println("M6_COMPARISON matched=" + matched + " non_nop=" + nonNopMatched
            + " unsupported=" + unsupported + " r5900_only=" + r5900Only
            + " mismatched=" + mismatched);
        if (mismatched != 0 || nonNopMatched < 100) {
            throw new IllegalStateException("M6 requires zero mismatches and at least 100 non-NOP matches");
        }
        println("M6_DISASSEMBLY_VERIFIED");
    }
}
