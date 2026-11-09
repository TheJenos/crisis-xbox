// Feeds hand-encoded PowerPC instructions through XenonRecomp's real code
// generator (Recompiler::Recompile) and writes the resulting C++ to a file.
// Each test is a tiny function: the instruction(s) under test followed by blr.
//
// usage: gen <out.inc>
#include "pch.h"
#include "recompiler.h"
#include <cstdio>

struct Test
{
    const char* name;
    std::vector<uint32_t> words;
    const char* mnemonic; // expected disassembly of the first word
};

static uint32_t D(uint32_t op, uint32_t rt, uint32_t ra, int32_t d) { return (op << 26) | (rt << 21) | (ra << 16) | (uint32_t(d) & 0xFFFF); }
static uint32_t X(uint32_t rt, uint32_t ra, uint32_t rb, uint32_t xo, uint32_t rc = 0) { return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc; }
static uint32_t VX(uint32_t vd, uint32_t va, uint32_t vb, uint32_t xo) { return (4u << 26) | (vd << 21) | (va << 16) | (vb << 11) | xo; }
static uint32_t BC(uint32_t bo, uint32_t bi, int32_t bd) { return (16u << 26) | (bo << 21) | (bi << 16) | (uint32_t(bd) & 0xFFFC); }
static constexpr uint32_t BLR = 0x4E800020;
static uint32_t LI(uint32_t rt, int32_t v) { return D(14, rt, 0, v); }

int main(int argc, char** argv)
{
    // Conditional-branch test shape: [bc +12] [li r3,1] [blr] [li r3,2] [blr]
    auto branch = [](uint32_t bc) { return std::vector<uint32_t>{ bc, LI(3, 1), BLR, LI(3, 2), BLR }; };

    std::vector<Test> tests = {
        { "t_lhzu",    { D(41, 3, 4, 6) }, "lhzu" },
        { "t_lhau",    { D(43, 3, 4, -2) }, "lhau" },
        { "t_lhzux",   { X(3, 4, 5, 311) }, "lhzux" },
        { "t_lbzux",   { X(3, 4, 5, 119) }, "lbzux" },
        { "t_lwzux",   { X(3, 4, 5, 55) }, "lwzux" },
        { "t_ldux",    { X(3, 4, 5, 53) }, "ldux" },
        { "t_sthu",    { D(45, 3, 4, 4) }, "sthu" },
        { "t_sthux",   { X(3, 4, 5, 439) }, "sthux" },
        { "t_stbux",   { X(3, 4, 5, 247) }, "stbux" },
        { "t_stdux",   { X(3, 4, 5, 181) }, "stdux" },
        { "t_lfsu",    { D(49, 1, 4, 8) }, "lfsu" },
        { "t_lfsux",   { X(1, 4, 5, 567) }, "lfsux" },
        { "t_lfdu",    { D(51, 1, 4, 8) }, "lfdu" },
        { "t_lfdux",   { X(1, 4, 5, 631) }, "lfdux" },
        { "t_stfsu",   { D(53, 1, 4, 8) }, "stfsu" },
        { "t_stfsux",  { X(1, 4, 5, 695) }, "stfsux" },
        { "t_stfdu",   { D(55, 1, 4, 8) }, "stfdu" },
        { "t_lhbrx",   { X(3, 4, 5, 790) }, "lhbrx" },
        { "t_eqv",     { X(4, 3, 5, 284) }, "eqv" },       // eqv r3, r4, r5 (RS in the rt slot)
        { "t_eqv_rc",  { X(4, 3, 5, 284, 1) }, "eqv." },
        { "t_addc",    { X(3, 4, 5, 10) }, "addc" },
        { "t_addme",   { X(3, 4, 0, 234) }, "addme" },
        { "t_subfze",  { X(3, 4, 0, 200) }, "subfze" },
        { "t_subfze_rc", { X(3, 4, 0, 200, 1) }, "subfze." },
        { "t_mulhd",   { X(3, 4, 5, 73) }, "mulhd" },
        { "t_fctiw",   { (63u << 26) | (1 << 21) | (2 << 11) | (14 << 1) }, "fctiw" },
        { "t_frsqrte", { (63u << 26) | (1 << 21) | (2 << 11) | (26 << 1) }, "frsqrte" },
        { "t_fnmadd",  { (63u << 26) | (1 << 21) | (2 << 16) | (4 << 11) | (3 << 6) | (31 << 1) }, "fnmadd" }, // f1 = -(f2*f3 + f4)
        { "t_rlwimi_rc", { (20u << 26) | (4 << 21) | (3 << 16) | (8 << 11) | (16 << 6) | (23 << 1) | 1 }, "rlwimi." },
        // BO: bdnzf=0, bdzf=2, bdnzt=8. BI = 4*cr + bit (lt=0, gt=1, eq=2)
        { "t_bdzf_eq",  branch(BC(2, 26, 12)), "bdzf" },
        { "t_bdzf_gt",  branch(BC(2, 25, 12)), "bdzf" },
        { "t_bdnzt_lt", branch(BC(8, 24, 12)), "bdnzt" },
        { "t_bdnzf_gt", branch(BC(0, 25, 12)), "bdnzf" },
        { "t_vsubuhs", { VX(1, 2, 3, 1600) }, "vsubuhs" },
        { "t_vsubuwm", { VX(1, 2, 3, 1152) }, "vsubuwm" },
        { "t_vslh",    { VX(1, 2, 3, 324) }, "vslh" },
        { "t_vsrh",    { VX(1, 2, 3, 580) }, "vsrh" },
        { "t_vpkswss", { VX(1, 2, 3, 462) }, "vpkswss" },
        { "t_vctuxs",  { VX(1, 1, 3, 906) }, "vctuxs" },             // vctuxs v1, v3, 1
        { "t_vcmpgtsw_rc", { VX(1, 2, 3, 902 | 1024) }, "vcmpgtsw." },
        { "t_vcmpgtuw_rc", { VX(1, 2, 3, 646 | 1024) }, "vcmpgtuw." },
        { "t_vcmpgtuh_rc", { VX(1, 2, 3, 582 | 1024) }, "vcmpgtuh." },
        { "t_vsel128",    { (5u << 26) | (1 << 21) | (2 << 16) | (3 << 11) | (848 & 0x3D0) }, "vsel128" },
        { "t_vpkswss128", { (5u << 26) | (1 << 21) | (2 << 16) | (3 << 11) | (640 & 0x3D0) }, "vpkswss128" },
        { "t_vcfpuxws128", { (6u << 26) | (1 << 21) | (2 << 16) | (3 << 11) | (624 & 0x7F0) }, "vcfpuxws128" }, // v1 = v3 * 4 -> u32
        { "t_lvehx",   { X(1, 4, 5, 39) }, "lvehx" },
        // vupkd3d128 v1, v3, imm: format = imm >> 2 (3 = 2 half floats, 5 = 4 half floats)
        { "t_vupkd3d_f16x2", { (6u << 26) | (1 << 21) | (12 << 16) | (3 << 11) | 0x7F0 }, "vupkd3d128" },
        { "t_vupkd3d_f16x4", { (6u << 26) | (1 << 21) | (20 << 16) | (3 << 11) | 0x7F0 }, "vupkd3d128" },
    };

    constexpr uint32_t codeBase = 0x82000000;
    size_t total = 0;
    for (auto& t : tests)
        total += (t.words.size() + (t.words.back() == BLR ? 0 : 1)) * 4;

    Recompiler rec;
    rec.image.data = std::make_unique<uint8_t[]>(total);
    rec.image.base = codeBase;
    rec.image.size = uint32_t(total);
    rec.image.Map(".text", 0, uint32_t(total), SectionFlags_Code, rec.image.data.get());

    std::string output;
    uint32_t addr = codeBase;
    int failures = 0;
    for (auto& t : tests)
    {
        std::vector<uint32_t> words = t.words;
        if (words.back() != BLR)
            words.push_back(BLR);

        uint8_t* p = rec.image.data.get() + (addr - codeBase);
        for (size_t i = 0; i < words.size(); i++)
            ((uint32_t*)p)[i] = ByteSwap(words[i]);

        ppc_insn insn{};
        ppc::Disassemble(p, addr, insn);
        const char* got = insn.opcode ? insn.opcode->name : "(invalid)";
        if (strcmp(got, t.mnemonic) != 0)
        {
            fmt::println("ENCODING MISMATCH {}: expected {}, disassembled as {}", t.name, t.mnemonic, got);
            failures++;
        }

        uint32_t size = uint32_t(words.size() * 4);
        rec.image.symbols.emplace(Symbol{ t.name, addr, size, Symbol_Function });
        Function fn(addr, size);
        rec.out.clear();
        if (!rec.Recompile(fn))
        {
            fmt::println("RECOMPILE FAILED {}", t.name);
            failures++;
        }
        output += rec.out;
        addr += size;
    }

    std::ofstream(argv[1]) << output;
    fmt::println("{} tests generated, {} problems", tests.size(), failures);
    return failures != 0;
}
