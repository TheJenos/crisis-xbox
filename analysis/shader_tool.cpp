// Offline tool for dumped Xenos shaders (vs_*.bin / ps_*.bin from SHADER_DUMP).
//
// usage: shader_tool dis <file.bin>...     disassemble
//        shader_tool wgsl <file.bin>...    translate to WGSL
//        shader_tool stats <dir>           opcode statistics over all dumps
//        shader_tool emit <outdir> <dir>   write <name>.wgsl and index.json for every dump
#include "xenos_shader.h"
#include "shader_synth.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

static bool Load(const std::string& path, std::vector<uint32_t>& words, bool& pixel)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    words.resize(bytes.size() / 4);
    memcpy(words.data(), bytes.data(), words.size() * 4);
    pixel = std::filesystem::path(path).filename().string().rfind("ps_", 0) == 0;
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: shader_tool dis|wgsl|stats|emit|synth <files or dir>\n"); return 1; }
    std::string mode = argv[1];
    if (mode == "synth")
    {
        // shader_tool synth <outdir>: synthetic coverage shaders -> <outdir>/*.wgsl + index.json
        std::filesystem::create_directories(argv[2]);
        std::string index = "[";
        int failed = 0;
        for (auto& [name, shader] : synth::All())
        {
            xenos::Translation t = xenos::TranslateToWgsl(shader);
            if (!t.error.empty()) { printf("%s: %s\n", name.c_str(), t.error.c_str()); failed++; continue; }
            std::ofstream(std::string(argv[2]) + "/" + name + ".wgsl", std::ios::binary) << t.wgsl;
            index += std::string(index.size() > 1 ? "," : "") + "\"" + name + "\"";
            printf("%s: %zu bytes of WGSL%s\n", name.c_str(), t.wgsl.size(), t.usesStateMachine ? " (state machine)" : "");
        }
        std::ofstream(std::string(argv[2]) + "/index.json", std::ios::binary) << index << "]";
        return failed ? 2 : 0;
    }
    std::vector<std::string> files;
    std::string outDir;
    if (mode == "emit")
    {
        if (argc < 4) return 1;
        outDir = argv[2];
        std::filesystem::create_directories(outDir);
        for (int i = 3; i < argc; i++)
            for (auto& e : std::filesystem::directory_iterator(argv[i]))
                if (e.path().extension() == ".bin") files.push_back(e.path().string());
    }
    else if (mode == "stats")
    {
        for (auto& e : std::filesystem::directory_iterator(argv[2]))
            if (e.path().extension() == ".bin") files.push_back(e.path().string());
    }
    else
        for (int i = 2; i < argc; i++) files.push_back(argv[i]);

    std::map<std::string, int> counts;
    std::string index = "[";
    int ok = 0, failed = 0;
    for (const auto& file : files)
    {
        std::vector<uint32_t> words;
        bool pixel;
        if (!Load(file, words, pixel)) { printf("cannot read %s\n", file.c_str()); continue; }
        xenos::DecodedShader s = xenos::Decode(words.data(), words.size(), pixel);
        // Shader caches hold vertex shaders before D3D binds a vertex
        // declaration (fetches have format 0); substitute float4 attributes
        // so the rest of the shader can still be tested.
        if (mode != "dis")
        {
            int n = 0;
            for (auto& in : s.slots)
                if (in.used && in.fetch && in.fetchInstr.op == 0 && in.fetchInstr.format == 0)
                {
                    in.fetchInstr.format = 38;
                    in.fetchInstr.stride = 16;
                    in.fetchInstr.offset = 4 * n++;
                }
        }
        if (mode == "dis")
            printf("== %s\n%s\n", file.c_str(), xenos::Disassemble(s).c_str());
        else if (mode == "wgsl")
        {
            xenos::Translation t = xenos::TranslateToWgsl(s);
            printf("== %s%s%s\n%s\n", file.c_str(), t.error.empty() ? "" : "  ERROR: ", t.error.c_str(), t.wgsl.c_str());
        }
        else if (mode == "emit")
        {
            xenos::Translation t = xenos::TranslateToWgsl(s);
            std::string name = std::filesystem::path(file).stem().string();
            if (!t.error.empty()) { printf("%s: %s\n", name.c_str(), t.error.c_str()); failed++; continue; }
            std::ofstream(outDir + "/" + name + ".wgsl", std::ios::binary) << t.wgsl;
            index += std::string(index.size() > 1 ? "," : "") + "\"" + name + "\"";
            ok++;
        }
        else
        {
            xenos::Translation t = xenos::TranslateToWgsl(s);
            (t.error.empty() ? ok : failed)++;
            if (!t.error.empty()) counts["untranslated: " + t.error]++;
            for (const auto& c : s.cf) counts["cf " + std::to_string(int(c.op))]++;
        }
    }
    if (mode == "emit")
    {
        std::ofstream(outDir + "/index.json", std::ios::binary) << index << "]";
        printf("%d translated, %d not\n", ok, failed);
    }
    if (mode == "stats")
    {
        printf("%zu shaders: %d translated, %d not\n", files.size(), ok, failed);
        for (auto& [k, v] : counts) printf("  %6d  %s\n", v, k.c_str());
    }
    return 0;
}
