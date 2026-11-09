// Diagnostic for a decrypted XEX image: sections, .pdata function count,
// and the instruction sequences that lead into indirect jumps (mtctr/bctr),
// to compare against the jump-table patterns XenonAnalyse searches for.
//
// usage: diag <default.xex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <image.h>
#include <xbox.h>
#include <byteswap.h>
#include <disasm.h>

static std::vector<uint8_t> LoadFile(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: diag <xex>\n");
        return 1;
    }

    auto file = LoadFile(argv[1]);
    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
    {
        printf("image failed to load\n");
        return 1;
    }

    // diag <xex> find <hex bytes>: find a byte pattern (4-byte aligned) in code sections
    if (argc >= 4 && strcmp(argv[2], "find") == 0)
    {
        std::vector<uint8_t> pat;
        for (const char* p = argv[3]; p[0] && p[1]; p += 2)
        {
            char byte[3] = { p[0], p[1], 0 };
            pat.push_back((uint8_t)strtoul(byte, nullptr, 16));
        }
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;
            for (size_t i = 0; i + pat.size() <= s.size; i += 4)
                if (memcmp(s.data + i, pat.data(), pat.size()) == 0)
                    printf("  %08zX\n", s.base + i);
        }
        return 0;
    }

    // diag <xex> fn <hex address>: the .pdata function containing the address, plus its neighbours
    if (argc >= 4 && strcmp(argv[2], "fn") == 0)
    {
        size_t addr = strtoull(argv[3], nullptr, 16);
        const Section* pdata = image.Find(".pdata");
        auto* pf = (const IMAGE_CE_RUNTIME_FUNCTION*)pdata->data;
        size_t count = pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
        for (size_t i = 0; i < count; i++)
        {
            IMAGE_CE_RUNTIME_FUNCTION fn = pf[i];
            fn.BeginAddress = ByteSwap(fn.BeginAddress);
            fn.Data = ByteSwap(fn.Data);
            size_t end = fn.BeginAddress + fn.FunctionLength * 4;
            if (fn.BeginAddress != 0 && addr + 0x400 >= fn.BeginAddress && addr < end + 0x400)
                printf("  %08X-%08zX  %5u insns%s\n", fn.BeginAddress, end, fn.FunctionLength,
                    addr >= fn.BeginAddress && addr < end ? "  <== contains address" : "");
        }
        return 0;
    }

    // diag <xex> leaf <file>: for each hex address in the file that isn't covered by
    // .pdata, print TOML `functions` entries for the leaf functions in its gap.
    // Function starts are the first non-padding word of the gap plus any bl targets.
    if (argc >= 4 && strcmp(argv[2], "leaf") == 0)
    {
        const Section* pdata = image.Find(".pdata");
        auto* pf = (const IMAGE_CE_RUNTIME_FUNCTION*)pdata->data;
        std::vector<std::pair<size_t, size_t>> fns;
        for (size_t i = 0; i < pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION); i++)
        {
            IMAGE_CE_RUNTIME_FUNCTION fn = pf[i];
            fn.BeginAddress = ByteSwap(fn.BeginAddress);
            fn.Data = ByteSwap(fn.Data);
            if (fn.BeginAddress != 0)
                fns.emplace_back(fn.BeginAddress, fn.BeginAddress + fn.FunctionLength * 4);
        }
        std::sort(fns.begin(), fns.end());

        // All bl targets in code.
        std::set<size_t> callTargets;
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;
            auto* code = (const uint32_t*)s.data;
            for (size_t i = 0; i < s.size / 4; i++)
            {
                uint32_t w = ByteSwap(code[i]);
                if ((w & 0xFC000003) != 0x48000001)
                    continue;
                int32_t disp = int32_t(w & 0x03FFFFFC);
                if (disp & 0x02000000)
                    disp -= 0x04000000;
                callTargets.insert(s.base + i * 4 + disp);
            }
        }

        std::set<std::pair<size_t, size_t>> out;
        std::ifstream in(argv[3]);
        std::string line;
        while (std::getline(in, line))
        {
            size_t a = strtoull(line.c_str(), nullptr, 16);
            auto it = std::upper_bound(fns.begin(), fns.end(), std::make_pair(a, SIZE_MAX));
            if (it != fns.begin() && a < std::prev(it)->second)
                continue; // covered by .pdata
            size_t gapStart = it == fns.begin() ? a : std::prev(it)->second;
            size_t gapEnd = it == fns.end() ? a + 4 : it->first;

            std::vector<size_t> starts;
            for (size_t p = gapStart; p < gapEnd; p += 4)
            {
                const uint32_t* w = (const uint32_t*)image.Find(p);
                if (w && *w != 0)
                {
                    starts.push_back(p);
                    break;
                }
            }
            for (auto t = callTargets.lower_bound(gapStart); t != callTargets.end() && *t < gapEnd; ++t)
                if (starts.empty() || *t > starts[0])
                    starts.push_back(*t);
            std::sort(starts.begin(), starts.end());

            for (size_t k = 0; k < starts.size(); k++)
            {
                size_t end = k + 1 < starts.size() ? starts[k + 1] : gapEnd;
                // trim trailing zero padding
                while (end > starts[k])
                {
                    const uint32_t* w = (const uint32_t*)image.Find(end - 4);
                    if (w && *w == 0) end -= 4; else break;
                }
                if (a >= starts[k] && a < end)
                    out.emplace(starts[k], end - starts[k]);
            }
        }
        for (auto& [s, n] : out)
            printf("    { address = 0x%zX, size = 0x%zX },\n", s, n);
        return 0;
    }

    // diag <xex> dumpimage <file>: write the decrypted, decompressed image
    if (argc >= 4 && strcmp(argv[2], "dumpimage") == 0)
    {
        FILE* f = fopen(argv[3], "wb");
        fwrite(image.data.get(), 1, image.size, f);
        fclose(f);
        printf("wrote %u bytes (base %08zX)\n", image.size, image.base);
        return 0;
    }

    // diag <xex> refs <hex address>: code that builds the address with lis + addi/ori
    if (argc >= 4 && strcmp(argv[2], "refs") == 0)
    {
        uint32_t target = uint32_t(strtoull(argv[3], nullptr, 16));
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;
            auto* code = (const uint32_t*)s.data;
            size_t n = s.size / 4;
            for (size_t i = 0; i < n; i++)
            {
                uint32_t w = ByteSwap(code[i]);
                if ((w >> 26) != 15 || ((w >> 16) & 31) != 0) // lis rD, hi (addis rD, 0, hi)
                    continue;
                uint32_t rd = (w >> 21) & 31, hi = (w & 0xFFFF) << 16;
                for (size_t k = i + 1; k < std::min(n, i + 12); k++)
                {
                    uint32_t x = ByteSwap(code[k]);
                    uint32_t op = x >> 26, ra = (x >> 16) & 31;
                    if (ra != rd) continue;
                    uint32_t value = op == 14 ? hi + uint32_t(int32_t(int16_t(x & 0xFFFF))) : op == 24 ? (hi | (x & 0xFFFF)) : 0;
                    if (value == target)
                        printf("  %08zX\n", s.base + k * 4);
                }
            }
        }
        return 0;
    }

    // diag <xex> str <hex address>: print the NUL-terminated string at the address
    if (argc >= 4 && strcmp(argv[2], "str") == 0)
    {
        size_t a = strtoull(argv[3], nullptr, 16);
        printf("%s\n", (const char*)image.Find(a));
        return 0;
    }

    // diag <xex> pdata: every .pdata function as "start end"
    if (argc >= 3 && strcmp(argv[2], "pdata") == 0)
    {
        const Section* pdata = image.Find(".pdata");
        auto* pf = (const IMAGE_CE_RUNTIME_FUNCTION*)pdata->data;
        for (size_t i = 0; i < pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION); i++)
        {
            IMAGE_CE_RUNTIME_FUNCTION fn = pf[i];
            fn.BeginAddress = ByteSwap(fn.BeginAddress);
            fn.Data = ByteSwap(fn.Data);
            if (fn.BeginAddress)
                printf("%08X %08X\n", fn.BeginAddress, fn.BeginAddress + fn.FunctionLength * 4);
        }
        return 0;
    }

    // diag <xex> codeptrs <known.txt>: words in data sections that point to the start
    // of code (after blr/b/bctr or padding) but aren't in the known function list
    if (argc >= 4 && strcmp(argv[2], "codeptrs") == 0)
    {
        std::set<size_t> known;
        {
            std::ifstream in(argv[3]);
            std::string line;
            while (std::getline(in, line))
                known.insert(strtoull(line.c_str(), nullptr, 16));
        }
        const Section* text = image.Find(".text");
        auto isCode = [&](size_t a) { return a >= text->base && a < text->base + text->size && (a & 3) == 0; };
        auto word = [&](size_t a) { return ByteSwap(*(const uint32_t*)image.Find(a)); };
        std::map<size_t, size_t> hits; // target -> number of references
        for (const auto& s : image.sections)
        {
            if (s.flags & SectionFlags_Code)
                continue;
            // Some sections (.reloc) extend past the loaded image; stay inside it.
            size_t limit = std::min<size_t>(s.size, image.base + image.size > s.base ? image.base + image.size - s.base : 0);
            for (size_t i = 0; i + 4 <= limit; i += 4)
            {
                size_t v = ByteSwap(*(const uint32_t*)(s.data + i));
                if (!isCode(v) || known.count(v) || v == text->base)
                    continue;
                uint32_t prev = word(v - 4), first = word(v);
                bool afterEnd = prev == 0x4E800020 || prev == 0x4E800420 || prev == 0 || (prev >> 26) == 18;
                if (afterEnd && first != 0)
                    hits[v]++;
            }
        }
        for (auto& [a, n] : hits)
            printf("%08zX %zu\n", a, n);
        return 0;
    }

    // diag <xex> syms [filter]: list symbols (import thunks) the loader created
    if (argc >= 3 && strcmp(argv[2], "syms") == 0)
    {
        for (const auto& sym : image.symbols)
            if (argc < 4 || sym.name.find(argv[3]) != std::string::npos)
                printf("  %08zX  %s\n", sym.address, sym.name.c_str());
        return 0;
    }

    // diag <xex> callers <hex address>: list bl instructions calling the address
    if (argc >= 4 && strcmp(argv[2], "callers") == 0)
    {
        size_t target = strtoull(argv[3], nullptr, 16);
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;
            auto* code = (const uint32_t*)s.data;
            for (size_t i = 0; i < s.size / 4; i++)
            {
                uint32_t w = ByteSwap(code[i]);
                if ((w & 0xFC000003) != 0x48000001)
                    continue;
                int32_t disp = int32_t(w & 0x03FFFFFC);
                if (disp & 0x02000000)
                    disp -= 0x04000000;
                if (s.base + i * 4 + disp == target)
                    printf("  %08zX\n", s.base + i * 4);
            }
        }
        return 0;
    }

    // diag <xex> check <file>: for each hex address in the file, report it if it
    // doesn't decode to a valid instruction
    if (argc >= 4 && strcmp(argv[2], "check") == 0)
    {
        std::ifstream in(argv[3]);
        std::string line;
        size_t total = 0, bad = 0;
        while (std::getline(in, line))
        {
            size_t a = strtoull(line.c_str(), nullptr, 16);
            const uint32_t* p = (const uint32_t*)image.Find(a);
            ppc_insn insn{};
            total++;
            if (p == nullptr || *p == 0 || (ppc::Disassemble(p, a, insn), insn.opcode == nullptr))
            {
                bad++;
                printf("  invalid target %08zX\n", a);
            }
        }
        printf("%zu labels checked, %zu invalid\n", total, bad);
        return 0;
    }

    // diag <xex> xref <hex address>: list branches targeting the address
    if (argc >= 4 && strcmp(argv[2], "xref") == 0)
    {
        size_t target = strtoull(argv[3], nullptr, 16);
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;
            auto* code = (const uint32_t*)s.data;
            for (size_t i = 0; i < s.size / 4; i++)
            {
                uint32_t w = ByteSwap(code[i]);
                uint32_t primary = w >> 26;
                if (primary != 16 && primary != 18)
                    continue;
                ppc_insn insn{};
                ppc::Disassemble(&code[i], s.base + i * 4, insn);
                if (!insn.opcode)
                    continue;
                for (int o = 0; o < 2; o++)
                    if (insn.operands[o] == target)
                    {
                        printf("  %08zX  %-8s %s\n", s.base + i * 4, insn.opcode->name, insn.op_str);
                        break;
                    }
            }
        }
        return 0;
    }

    // diag <xex> <hex address> [count]: disassemble `count` instructions ending at address
    if (argc >= 3)
    {
        size_t addr = strtoull(argv[2], nullptr, 16);
        size_t n = argc >= 4 ? strtoull(argv[3], nullptr, 10) : 24;
        for (size_t a = addr - (n - 1) * 4; a <= addr; a += 4)
        {
            const uint32_t* p = (const uint32_t*)image.Find(a);
            if (!p) continue;
            ppc_insn insn{};
            ppc::Disassemble(p, a, insn);
            printf("  %08zX  %-8s %s\n", a, insn.opcode ? insn.opcode->name : "?", insn.op_str);
        }
        return 0;
    }

    printf("image base 0x%zX size 0x%X entry 0x%zX\n", image.base, image.size, image.entry_point);
    printf("imports resolved as symbols: %zu\n\n", image.symbols.size());
    printf("sections:\n");
    for (const auto& s : image.sections)
        printf("  %-10s 0x%08zX  %10u bytes  %s\n", s.name.c_str(), s.base, s.size,
            (s.flags & SectionFlags_Code) ? "code" : "data");

    if (const Section* pdata = image.Find(".pdata"))
    {
        size_t count = pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
        auto* pf = (const IMAGE_CE_RUNTIME_FUNCTION*)pdata->data;
        size_t valid = 0, totalInsns = 0, eh = 0;
        for (size_t i = 0; i < count; i++)
        {
            IMAGE_CE_RUNTIME_FUNCTION fn = pf[i];
            fn.BeginAddress = ByteSwap(fn.BeginAddress);
            fn.Data = ByteSwap(fn.Data);
            if (fn.BeginAddress == 0)
                continue;
            valid++;
            totalInsns += fn.FunctionLength;
            eh += fn.ExceptionFlag;
        }
        printf("\n.pdata: %zu functions with unwind info, covering %zu instructions; %zu with exception handlers\n",
            valid, totalInsns, eh);
    }

    // Instruction statistics and indirect-jump idioms
    size_t insns = 0, blr = 0, bctr = 0, bctrl = 0, bl = 0, mtctr = 0, zero = 0;
    std::map<std::string, size_t> idioms;
    std::map<std::string, std::string> idiomExample;
    size_t tableJumps[4]{}; // absolute (lwzx), 16-bit offset (lhzx), 8-bit offset (lbzx), other

    for (const auto& s : image.sections)
    {
        if (!(s.flags & SectionFlags_Code))
            continue;
        auto* code = (const uint32_t*)s.data;
        size_t n = s.size / 4;
        for (size_t i = 0; i < n; i++)
        {
            uint32_t w = ByteSwap(code[i]);
            insns++;
            if (w == 0) zero++;
            else if (w == 0x4E800020) blr++;
            else if (w == 0x4E800420) bctr++;
            else if (w == 0x4E800421) bctrl++;
            else if ((w & 0xFC000003) == 0x48000001) bl++;
            else if ((w & 0xFC1FFFFF) == 0x7C0903A6) mtctr++;

            // An indirect jump that is not a call: mtctr followed shortly by bctr.
            if (w == 0x4E800420 && i >= 8)
            {
                std::string seq, text;
                for (size_t j = i - 8; j <= i; j++)
                {
                    ppc_insn insn{};
                    uint32_t raw = code[j];
                    ppc::Disassemble(&raw, s.base + j * 4, insn);
                    const char* name = insn.opcode ? insn.opcode->name : "?";
                    seq += name;
                    seq += ' ';
                    char line[256];
                    snprintf(line, sizeof(line), "    %08zX  %-8s %s\n", s.base + j * 4, name, insn.op_str);
                    text += line;
                }
                if (idioms[seq]++ == 0)
                    idiomExample[seq] = text;

                // Table-driven jump: an indexed load (lwzx/lhzx/lbzx) shortly before bctr
                if (seq.find("lwzx ") != std::string::npos) tableJumps[0]++;
                else if (seq.find("lhzx ") != std::string::npos) tableJumps[1]++;
                else if (seq.find("lbzx ") != std::string::npos) tableJumps[2]++;
                else tableJumps[3]++;
            }
        }
    }

    printf("\ncode stats: %zu words, %zu zero words, %zu blr, %zu bl, %zu mtctr, %zu bctr (indirect jumps), %zu bctrl (indirect calls)\n",
        insns, zero, blr, bl, mtctr, bctr, bctrl);

    printf("bctr breakdown: %zu absolute tables (lwzx), %zu 16-bit offset tables (lhzx), %zu 8-bit offset tables (lbzx), %zu other (tail calls / vtable jumps)\n",
        tableJumps[0], tableJumps[1], tableJumps[2], tableJumps[3]);

    std::vector<std::pair<size_t, std::string>> sorted;
    for (auto& [k, v] : idioms)
        sorted.emplace_back(v, k);
    std::sort(sorted.rbegin(), sorted.rend());

    printf("\nmost common 8-instruction sequences ending in bctr (%zu distinct):\n", sorted.size());
    for (size_t i = 0; i < sorted.size() && i < 8; i++)
    {
        printf("\n  [%zu occurrences] %s\n", sorted[i].first, sorted[i].second.c_str());
        printf("%s", idiomExample[sorted[i].second].c_str());
    }
    return 0;
}
