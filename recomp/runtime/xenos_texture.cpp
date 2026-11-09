// Xenos texture fetch constants, tiling and format decoding.
#include "xenos_texture.h"
#include "guest.h"

#include <algorithm>
#include <cstdlib>

namespace
{
    // Xenos TextureFormat values used here.
    enum : uint32_t
    {
        FMT_8 = 2, FMT_1_5_5_5 = 3, FMT_5_6_5 = 4, FMT_8_8_8_8 = 6, FMT_2_10_10_10 = 7, FMT_8_A = 8, FMT_8_B = 9,
        FMT_8_8 = 10, FMT_8_8_8_8_A = 14, FMT_4_4_4_4 = 15, FMT_DXT1 = 18, FMT_DXT2_3 = 19, FMT_DXT4_5 = 20,
        FMT_DXN = 49, FMT_8_8_8_8_AS_16_16_16_16 = 50, FMT_DXT1_AS_16_16_16_16 = 51,
        FMT_DXT2_3_AS_16_16_16_16 = 52, FMT_DXT4_5_AS_16_16_16_16 = 53, FMT_DXT5A = 59,
        FMT_CTX1 = 60,
        // Normalized 16 bits per component (unsigned, or signed by the fetch constant's sign modes).
        FMT_16 = 24, FMT_16_16 = 25, FMT_16_16_16_16 = 26,
        // _EXPAND: half floats too, filtered at full precision (XDK's *F_EXPAND formats).
        FMT_16_EXPAND = 27, FMT_16_16_EXPAND = 28, FMT_16_16_16_16_EXPAND = 29,
        FMT_16_FLOAT = 30, FMT_16_16_FLOAT = 31, FMT_16_16_16_16_FLOAT = 32,
        FMT_32_FLOAT = 36, FMT_32_32_FLOAT = 37, FMT_32_32_32_32_FLOAT = 38,
    };

    struct FormatInfo { uint32_t blockSize; uint32_t bytesPerBlock; };

    bool GetFormatInfo(uint32_t f, FormatInfo& info)
    {
        switch (f)
        {
        case FMT_8: case FMT_8_A: case FMT_8_B: info = { 1, 1 }; return true;
        case FMT_1_5_5_5: case FMT_5_6_5: case FMT_4_4_4_4: case FMT_8_8: info = { 1, 2 }; return true;
        case FMT_8_8_8_8: case FMT_8_8_8_8_A: case FMT_2_10_10_10: case FMT_8_8_8_8_AS_16_16_16_16: info = { 1, 4 }; return true;
        case FMT_DXT1: case FMT_DXT1_AS_16_16_16_16: case FMT_DXT5A: case FMT_CTX1: info = { 4, 8 }; return true;
        case FMT_DXT2_3: case FMT_DXT4_5: case FMT_DXN: case FMT_DXT2_3_AS_16_16_16_16: case FMT_DXT4_5_AS_16_16_16_16: info = { 4, 16 }; return true;
        case FMT_16_FLOAT: case FMT_16_EXPAND: case FMT_16: info = { 1, 2 }; return true;
        case FMT_16_16_FLOAT: case FMT_16_16_EXPAND: case FMT_32_FLOAT: case FMT_16_16: info = { 1, 4 }; return true;
        case FMT_16_16_16_16_FLOAT: case FMT_16_16_16_16_EXPAND: case FMT_32_32_FLOAT: case FMT_16_16_16_16: info = { 1, 8 }; return true;
        case FMT_32_32_32_32_FLOAT: info = { 1, 16 }; return true;
        default: return false;
        }
    }

    uint32_t Log2(uint32_t v) { uint32_t r = 0; while (v > 1) { v >>= 1; r++; } return r; }

    // Byte offset of block (x, y) in a 2D tiled surface whose row pitch is
    // `pitchBlocks` (aligned to 32 blocks). Tiles are 32x32 blocks made of
    // interleaved 8x8 micro tiles; this is the standard Xenos 2D tiling.
    uint32_t TiledRowOffset(uint32_t y, uint32_t pitchBlocks, uint32_t log2Bpb)
    {
        uint32_t macro = ((y >> 5) * (pitchBlocks >> 5)) << (log2Bpb + 7);
        uint32_t micro = ((y & 6) << 2) << log2Bpb;
        return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2Bpb)) + ((y & 1) << 4);
    }
    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t log2Bpb, uint32_t rowOffset)
    {
        uint32_t macro = (x >> 5) << (log2Bpb + 7);
        uint32_t micro = (x & 7) << log2Bpb;
        uint32_t offset = rowOffset + macro + ((micro & ~0xFu) << 1) + (micro & 0xF);
        return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) +
               ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
    }

    // Byte offset of block (x, y, z) in a tiled 3D texture, as in Xenia
    // (texture_address.h, Tiled3D). Macro tiles are 32x16x4 blocks, laid out
    // X fastest, then Y, then Z; inside one, 8x8x4 micro tiles hold (x, y, z)
    // linearly but for y's lowest bit, and x bits 3-4, y bit 3 and z bit 2
    // select the pipe and bank. pitchAligned and heightAligned: in blocks,
    // multiples of 32.
    uint64_t Tiled3D(uint32_t x, uint32_t y, uint32_t z, uint32_t pitchAligned, uint32_t heightAligned, uint32_t log2Bpb)
    {
        uint32_t outer = (((z >> 2) * (heightAligned >> 4) + (y >> 4)) * (pitchAligned >> 5) + (x >> 5)) << 7;
        uint32_t inner = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
        uint64_t bytes = uint64_t(outer | inner) << log2Bpb;
        uint32_t bank = ((y >> 3) ^ (z >> 2)) & 1;
        uint32_t pipe = ((x >> 3) & 3) ^ (bank << 1);
        return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (bytes & 0xF) | (((bytes >> 4) & 1) << 5) |
               (((bytes >> 5) & 7) << 8) | ((bytes >> 8) << 12);
    }

    // Undo the texture's endian swap so data reads as the GPU sees it (little-endian).
    void Unswap(uint8_t* p, uint32_t n, uint32_t endian)
    {
        switch (endian)
        {
        case 1: for (uint32_t i = 0; i + 1 < n; i += 2) std::swap(p[i], p[i + 1]); break;
        case 2: for (uint32_t i = 0; i + 3 < n; i += 4) { std::swap(p[i], p[i + 3]); std::swap(p[i + 1], p[i + 2]); } break;
        case 3: for (uint32_t i = 0; i + 3 < n; i += 4) { std::swap(p[i], p[i + 2]); std::swap(p[i + 1], p[i + 3]); } break;
        default: break;
        }
    }

    void Rgb565(uint16_t c, uint8_t out[3])
    {
        out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
        out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
        out[2] = uint8_t((c & 31) * 255 / 31);
    }

    // Decode one BC1 color block into a 4x4 RGBA tile (16 * 4 bytes).
    void DecodeBc1Color(const uint8_t* b, uint8_t out[64], bool allowAlpha)
    {
        uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
        uint8_t pal[4][4];
        Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
        pal[0][3] = pal[1][3] = 255;
        if (c0 > c1 || !allowAlpha)
        {
            for (int k = 0; k < 3; k++)
            {
                pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
                pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
            }
            pal[2][3] = pal[3][3] = 255;
        }
        else
        {
            for (int k = 0; k < 3; k++) { pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
            pal[2][3] = 255;
            pal[3][3] = 0;
        }
        uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24;
        for (int i = 0; i < 16; i++)
            memcpy(out + i * 4, pal[(idx >> (2 * i)) & 3], 4);
    }

    // Interpolated 8-bit channel block (BC3 alpha / BC4 / BC5 channel).
    void DecodeBc4Channel(const uint8_t* b, uint8_t out[16])
    {
        uint8_t a0 = b[0], a1 = b[1], pal[8] = { a0, a1 };
        if (a0 > a1)
            for (int i = 1; i < 7; i++) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
        else
        {
            for (int i = 1; i < 5; i++) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
            pal[6] = 0; pal[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; i++) out[i] = pal[(bits >> (3 * i)) & 7];
    }

    // Decode one block (blockSize x blockSize texels) to RGBA.
    void DecodeBlock(uint32_t format, const uint8_t* b, uint8_t* tile /* blockSize^2 * 4 */)
    {
        switch (format)
        {
        case FMT_8: case FMT_8_A: case FMT_8_B:
            tile[0] = tile[1] = tile[2] = b[0]; tile[3] = 255;
            break;
        case FMT_8_8:
            tile[0] = b[0]; tile[1] = b[1]; tile[2] = 0; tile[3] = 255;
            break;
        case FMT_CTX1:
        {
            // Two 8:8 (X, Y) endpoints, then 2-bit indices: endpoint 0, 1, and
            // the 2/3 : 1/3 and 1/3 : 2/3 blends (as in Xenia).
            static const bool swapCtx = getenv("CTX1_SWAP") != nullptr; // experiment
            uint8_t x0 = swapCtx ? b[1] : b[0], y0 = swapCtx ? b[0] : b[1], x1 = swapCtx ? b[3] : b[2], y1 = swapCtx ? b[2] : b[3];
            uint8_t px[4] = { x0, x1, uint8_t((2 * x0 + x1) / 3), uint8_t((x0 + 2 * x1) / 3) };
            uint8_t py[4] = { y0, y1, uint8_t((2 * y0 + y1) / 3), uint8_t((y0 + 2 * y1) / 3) };
            uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24;
            for (int i = 0; i < 16; i++)
            {
                uint32_t k = (idx >> (2 * i)) & 3;
                tile[i * 4 + 0] = px[k]; tile[i * 4 + 1] = py[k]; tile[i * 4 + 2] = 0; tile[i * 4 + 3] = 255;
            }
            break;
        }
        case FMT_5_6_5:
            Rgb565(uint16_t(b[0] | b[1] << 8), tile); tile[3] = 255;
            break;
        case FMT_1_5_5_5:
        {
            uint16_t c = uint16_t(b[0] | b[1] << 8);
            tile[0] = uint8_t(((c >> 10) & 31) * 255 / 31); tile[1] = uint8_t(((c >> 5) & 31) * 255 / 31);
            tile[2] = uint8_t((c & 31) * 255 / 31); tile[3] = (c & 0x8000) ? 255 : 0;
            break;
        }
        case FMT_4_4_4_4:
        {
            uint16_t c = uint16_t(b[0] | b[1] << 8);
            tile[0] = uint8_t(((c >> 8) & 15) * 17); tile[1] = uint8_t(((c >> 4) & 15) * 17);
            tile[2] = uint8_t((c & 15) * 17); tile[3] = uint8_t(((c >> 12) & 15) * 17);
            break;
        }
        case FMT_8_8_8_8: case FMT_8_8_8_8_A: case FMT_8_8_8_8_AS_16_16_16_16:
            // Little-endian A8R8G8B8: bytes B, G, R, A
            tile[0] = b[2]; tile[1] = b[1]; tile[2] = b[0]; tile[3] = b[3];
            break;
        case FMT_2_10_10_10:
        {
            uint32_t c = b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24;
            tile[0] = uint8_t(((c >> 20) & 1023) >> 2); tile[1] = uint8_t(((c >> 10) & 1023) >> 2);
            tile[2] = uint8_t((c & 1023) >> 2); tile[3] = uint8_t((c >> 30) * 85);
            break;
        }
        case FMT_DXT1: case FMT_DXT1_AS_16_16_16_16:
            DecodeBc1Color(b, tile, true);
            break;
        case FMT_DXT2_3: case FMT_DXT2_3_AS_16_16_16_16:
            DecodeBc1Color(b + 8, tile, false);
            for (int i = 0; i < 16; i++) tile[i * 4 + 3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            break;
        case FMT_DXT4_5: case FMT_DXT4_5_AS_16_16_16_16:
        {
            uint8_t alpha[16];
            DecodeBc4Channel(b, alpha);
            DecodeBc1Color(b + 8, tile, false);
            for (int i = 0; i < 16; i++) tile[i * 4 + 3] = alpha[i];
            break;
        }
        case FMT_DXT5A:
        {
            uint8_t v[16];
            DecodeBc4Channel(b, v);
            for (int i = 0; i < 16; i++) { tile[i * 4] = tile[i * 4 + 1] = tile[i * 4 + 2] = v[i]; tile[i * 4 + 3] = 255; }
            break;
        }
        case FMT_DXN: // two BC4 channels: red, green
        {
            uint8_t r[16], g[16];
            static const bool swapDxn = getenv("DXN_SWAP") != nullptr; // experiment
            DecodeBc4Channel(b + (swapDxn ? 8 : 0), r);
            DecodeBc4Channel(b + (swapDxn ? 0 : 8), g);
            for (int i = 0; i < 16; i++) { tile[i * 4] = r[i]; tile[i * 4 + 1] = g[i]; tile[i * 4 + 2] = 0; tile[i * 4 + 3] = 255; }
            break;
        }
        }
    }

    // Where the base level's blocks are: one 2D surface per cube face, or one
    // tiled (or linear) volume for 3D textures.
    struct BaseLayout
    {
        uint32_t blocksW, blocksH, slices; // slices: cube faces / layers, or 3D depth
        uint32_t pitchBlocks, heightBlocks; // storage row pitch and slice height (strides)
        uint32_t log2Bpb;
        size_t sliceBytes;                  // stride between slices (not used by tiled 3D)
        bool is3d;
    };

    bool GetBaseLayout(const XenosTexture& tex, const FormatInfo& fi, BaseLayout& l)
    {
        if (tex.width == 0 || tex.height == 0 || tex.width > 8192 || tex.height > 8192 || tex.depth == 0 || tex.depth > 1024)
            return false;
        l.is3d = tex.dimension == 2;
        l.blocksW = (tex.width + fi.blockSize - 1) / fi.blockSize;
        l.blocksH = (tex.height + fi.blockSize - 1) / fi.blockSize;
        l.slices = l.is3d ? tex.depth : tex.layers;
        // Pitch is in texels; tiled surfaces are laid out in 32-block-aligned rows.
        l.pitchBlocks = (std::max<uint32_t>(tex.pitch / fi.blockSize, l.blocksW) + 31) & ~31u;
        // Cube faces follow each other, each a whole (32-block-aligned when
        // tiled) surface. 3D slices are always 32 rows apart (as in Xenia's
        // GetGuestTextureLayout, linear ones too).
        l.heightBlocks = (tex.tiled || l.is3d) ? (l.blocksH + 31) & ~31u : l.blocksH;
        l.log2Bpb = Log2(fi.bytesPerBlock);
        l.sliceBytes = size_t(l.pitchBlocks) * l.heightBlocks * fi.bytesPerBlock;
        return true;
    }

    // Byte offset (from the base address) of block (x, y) of slice s.
    // rowOffset: TiledRowOffset(y) for tiled 2D surfaces (computed once per row).
    uint32_t BlockOffset(const XenosTexture& tex, const BaseLayout& l, uint32_t x, uint32_t y, uint32_t s, uint32_t rowOffset)
    {
        if (l.is3d && tex.tiled)
            return uint32_t(Tiled3D(x, y, s, l.pitchBlocks, l.heightBlocks, l.log2Bpb));
        uint32_t offset = tex.tiled ? TiledOffset(x, y, l.log2Bpb, rowOffset) : (y * l.pitchBlocks + x) << l.log2Bpb;
        return uint32_t(s * l.sliceBytes) + offset;
    }
    uint32_t RowOffset(const XenosTexture& tex, const BaseLayout& l, uint32_t y)
    {
        return tex.tiled && !l.is3d ? TiledRowOffset(y, l.pitchBlocks, l.log2Bpb) : 0;
    }

    // Inverse of the Xenos piecewise-linear gamma ramp (as xe_degamma in the shaders).
    float Degamma(float g)
    {
        float x = std::clamp(g, 0.0f, 1.0f);
        if (x < 0.25f) return x * 0.25f;
        if (x < 0.375f) return (x - 0.125f) * 0.5f;
        if (x < 0.75f) return x - 0.25f;
        return (x - 0.5f) * 2.0f;
    }

    // One normalized 16-bit component with its sign mode applied.
    float Norm16(uint16_t raw, uint32_t sign)
    {
        switch (sign & 3)
        {
        case 1: return std::max(float(int16_t(raw)) / 32767.0f, -1.0f); // signed (two's complement)
        case 2: return float(raw) / 65535.0f * 2.0f - 1.0f;             // unsigned, biased
        case 3: return Degamma(float(raw) / 65535.0f);                  // gamma
        default: return float(raw) / 65535.0f;
        }
    }
}

bool ParseTextureFetch(const uint32_t f[6], XenosTexture& out)
{
    // dword0: type (bits 0-1, 2 = texture), sign X Y Z W (bits 2-9, 2 bits each),
    //         pitch / 32 (bits 22-30), tiled (bit 31)
    // dword1: format (bits 0-5), endianness (bits 6-7), base address (bits 12-31)
    // dword2: 2D size: width-1 (bits 0-12), height-1 (bits 13-25);
    //         3D size: width-1 (bits 0-10), height-1 (bits 11-21), depth-1 (bits 22-31)
    // dword5: dimension (bits 9-10)
    // Type 2 is "texture". Crysis 2 builds some render-target textures' fetch
    // constants with type 0 ("invalid texture"; the rest is a valid texture:
    // e.g. its 576x360 screen-droplet buffer and the 1152x720 normals), and
    // the hardware samples them all the same (Xenia accepts them too, see
    // gpu_allow_invalid_fetch_constants). Rejecting them left those fetches
    // unbound, and the effects built on them washed the screen out to white
    // or black around the underwater escape.
    if ((f[0] & 3) != 2 && (f[0] & 3) != 0)
        return false;
    out.pitch = ((f[0] >> 22) & 0x1FF) * 32;
    out.tiled = (f[0] >> 31) & 1;
    out.format = f[1] & 0x3F;
    out.endian = (f[1] >> 6) & 3;
    out.address = 0xA0000000 | (f[1] & 0x1FFFF000);
    out.signs = (f[0] >> 2) & 0xFF;
    out.dimension = (f[5] >> 9) & 3;
    if (out.dimension == 0) // 1D: not handled
        return false;
    if (out.dimension == 2)
    {
        out.width = (f[2] & 0x7FF) + 1;
        out.height = ((f[2] >> 11) & 0x7FF) + 1;
        out.depth = ((f[2] >> 22) & 0x3FF) + 1;
    }
    else
    {
        out.width = (f[2] & 0x1FFF) + 1;
        out.height = ((f[2] >> 13) & 0x1FFF) + 1;
        out.depth = 1;
    }
    out.layers = out.dimension == 3 ? 6 : 1;
    // dword4: mip_max_level (bits 6-9); dword5: packed_mips (bit 11), mip_address (bits 12-31)
    out.packedMips = (f[5] >> 11) & 1;
    out.mipAddress = (f[5] & 0xFFFFF000) ? 0xA0000000 | (f[5] & 0x1FFFF000) : 0;
    uint32_t maxLevel = (f[4] >> 6) & 0xF, longest = std::max({ out.width, out.height, out.depth }), full = 1;
    while ((longest >> full) > 0) full++; // levels down to 1x1
    out.mipLevels = out.mipAddress ? std::min(maxLevel + 1, full) : 1;
    return (f[1] & 0x1FFFF000) != 0;
}

bool CanDecodeTexture(uint32_t format)
{
    FormatInfo info;
    return GetFormatInfo(format, info);
}

const char* TextureFormatName(uint32_t f)
{
    switch (f)
    {
    case FMT_8: return "8"; case FMT_1_5_5_5: return "1_5_5_5"; case FMT_5_6_5: return "5_6_5";
    case FMT_8_8_8_8: return "8_8_8_8"; case FMT_2_10_10_10: return "2_10_10_10"; case FMT_8_A: return "8_A";
    case FMT_8_B: return "8_B"; case FMT_8_8: return "8_8"; case FMT_8_8_8_8_A: return "8_8_8_8_A";
    case FMT_4_4_4_4: return "4_4_4_4"; case FMT_DXT1: return "DXT1"; case FMT_DXT2_3: return "DXT2_3";
    case FMT_DXT4_5: return "DXT4_5"; case FMT_DXN: return "DXN"; case FMT_DXT5A: return "DXT5A"; case FMT_CTX1: return "CTX1";
    case FMT_8_8_8_8_AS_16_16_16_16: return "8_8_8_8_AS_16"; case FMT_DXT1_AS_16_16_16_16: return "DXT1_AS_16";
    case FMT_DXT2_3_AS_16_16_16_16: return "DXT2_3_AS_16"; case FMT_DXT4_5_AS_16_16_16_16: return "DXT4_5_AS_16";
    case FMT_16: return "16"; case FMT_16_16: return "16_16"; case FMT_16_16_16_16: return "16_16_16_16";
    default: return "?";
    }
}

bool IsFloatTextureFormat(uint32_t f)
{
    return (f >= FMT_16_EXPAND && f <= FMT_16_16_16_16_FLOAT) || (f >= FMT_32_FLOAT && f <= FMT_32_32_32_32_FLOAT);
}

bool IsNorm16TextureFormat(uint32_t f)
{
    return f >= FMT_16 && f <= FMT_16_16_16_16;
}

bool DecodesToHalf(uint32_t f)
{
    return IsFloatTextureFormat(f) || IsNorm16TextureFormat(f);
}

namespace
{
    uint16_t FloatToHalf(float f)
    {
        uint32_t x;
        memcpy(&x, &f, 4);
        uint32_t sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
        if (e == 0xFF) return uint16_t(sign | 0x7C00 | (m ? 0x200 : 0));
        int32_t he = int32_t(e) - 127 + 15;
        if (he >= 31) return uint16_t(sign | 0x7BFF); // clamp to the largest half
        if (he <= 0)
        {
            if (he < -10) return uint16_t(sign);
            m |= 0x800000;
            return uint16_t(sign | (m >> (14 - he)));
        }
        return uint16_t(sign | (he << 10) | (m >> 13));
    }
}

void WriteTiled32(uint32_t guestAddr, uint32_t pitchTexels, uint32_t w, uint32_t h, const uint32_t* texels, uint32_t endian)
{
    uint32_t pitchBlocks = (std::max(pitchTexels, w) + 31) & ~31u;
    for (uint32_t y = 0; y < h; y++)
    {
        uint32_t rowOffset = TiledRowOffset(y, pitchBlocks, 2);
        for (uint32_t x = 0; x < w; x++)
        {
            uint8_t b[4];
            memcpy(b, &texels[size_t(y) * w + x], 4);
            Unswap(b, 4, endian); // the swaps are their own inverses
            memcpy(GuestPtr(guestAddr + TiledOffset(x, y, 2, rowOffset)), b, 4);
        }
    }
}

bool DecodeTextureHalf(const XenosTexture& tex, std::vector<uint8_t>& rgbaHalf)
{
    FormatInfo fi;
    BaseLayout l;
    if (!DecodesToHalf(tex.format) || !GetFormatInfo(tex.format, fi) || !GetBaseLayout(tex, fi, l))
        return false;
    size_t sliceTexels = size_t(tex.width) * tex.height;
    rgbaHalf.assign(sliceTexels * l.slices * 8, 0);
    // Components: 16-bit (half floats or normalized integers) or 32-bit floats.
    bool norm16 = IsNorm16TextureFormat(tex.format);
    bool half = tex.format >= FMT_16_EXPAND && tex.format <= FMT_16_16_16_16_FLOAT;
    uint32_t comps = fi.bytesPerBlock / (half || norm16 ? 2 : 4);
    uint8_t texel[16];
    for (uint32_t s = 0; s < l.slices; s++)
        for (uint32_t y = 0; y < tex.height; y++)
        {
            uint32_t rowOffset = RowOffset(tex, l, y);
            for (uint32_t x = 0; x < tex.width; x++)
            {
                memcpy(texel, GuestPtr(tex.address + BlockOffset(tex, l, x, y, s, rowOffset)), fi.bytesPerBlock);
                Unswap(texel, fi.bytesPerBlock, tex.endian);
                uint16_t out[4] = { 0, 0, 0, 0x3C00 }; // missing components: 0, alpha 1
                for (uint32_t c = 0; c < comps; c++)
                {
                    if (norm16)
                    {
                        uint16_t raw;
                        memcpy(&raw, texel + c * 2, 2);
                        out[c] = FloatToHalf(Norm16(raw, tex.signs >> (c * 2)));
                    }
                    else if (half)
                        memcpy(&out[c], texel + c * 2, 2);
                    else
                    {
                        float v;
                        memcpy(&v, texel + c * 4, 4);
                        out[c] = FloatToHalf(v);
                    }
                }
                memcpy(&rgbaHalf[(s * sliceTexels + size_t(y) * tex.width + x) * 8], out, 8);
            }
        }
    return true;
}

namespace
{
    uint32_t Log2Ceil(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }
    uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

    // Where a surface's texels live: its tiled/linear grid and where in it the
    // wanted region starts (mip tail levels sit inside one shared tile).
    struct Placement { uint32_t address, pitchBlocks, offsetX, offsetY; size_t sliceBytes; };

    // Block offset of a mip inside the packed mip tail (Xenos layout, as in Xenia).
    void PackedMipOffset(uint32_t width, uint32_t height, uint32_t level, uint32_t blockSize, uint32_t& x, uint32_t& y)
    {
        uint32_t lw = Log2Ceil(width), lh = Log2Ceil(height), size = std::min(lw, lh);
        uint32_t base = size > 4 ? size - 4 : 0, packed = level - base;
        x = y = 0;
        if (packed < 3)
        {
            if (lw > lh) y = 16 >> packed; // wider than tall: stacked vertically
            else x = 16 >> packed;
        }
        else
        {
            if (lw > lh) x = (1u << (lw - base)) >> (packed - 2);
            else y = (1u << (lh - base)) >> (packed - 2);
        }
        x /= blockSize;
        y /= blockSize;
    }

    bool MipPlacement(const XenosTexture& tex, const FormatInfo& fi, uint32_t level, Placement& out)
    {
        uint32_t w2 = 1u << Log2Ceil(tex.width), h2 = 1u << Log2Ceil(tex.height);
        uint32_t size = std::min(Log2Ceil(tex.width), Log2Ceil(tex.height));
        uint32_t tailStart = tex.packedMips ? (size > 4 ? size - 4 : 0) : 99;
        if (!tex.mipAddress || tailStart == 0)
            return false; // whole chain packed with the base: not handled
        size_t offset = 0;
        for (uint32_t l = 1; l < 16; l++)
        {
            uint32_t wl = std::max(1u, w2 >> l), hl = std::max(1u, h2 >> l);
            if (l >= tailStart)
            {
                out = { tex.mipAddress + uint32_t(offset), 32, 0, 0, AlignUp(32 * 32 * fi.bytesPerBlock, 4096) };
                PackedMipOffset(tex.width, tex.height, level, fi.blockSize, out.offsetX, out.offsetY);
                return true;
            }
            uint32_t bw = (wl + fi.blockSize - 1) / fi.blockSize, bh = (hl + fi.blockSize - 1) / fi.blockSize;
            uint32_t pitch = AlignUp(bw, 32), rows = tex.tiled ? AlignUp(bh, 32) : bh;
            size_t slice = AlignUp(pitch * rows * fi.bytesPerBlock, 4096);
            if (l == level)
            {
                out = { tex.mipAddress + uint32_t(offset), pitch, 0, 0, slice };
                return true;
            }
            offset += slice * tex.layers;
        }
        return false;
    }

    // Decode a w x h region (per layer) of a placed surface to RGBA8.
    void DecodeRegion(const XenosTexture& tex, const FormatInfo& fi, const Placement& pl, uint32_t w, uint32_t h,
                      std::vector<uint8_t>& rgba);
}

bool DecodeTextureMip(const XenosTexture& tex, uint32_t level, std::vector<uint8_t>& rgba)
{
    FormatInfo fi;
    // (3D mip chains are laid out differently: not handled, base level only.)
    if (level == 0 || level >= tex.mipLevels || DecodesToHalf(tex.format) || tex.dimension == 2 || !GetFormatInfo(tex.format, fi))
        return false;
    Placement pl;
    if (!MipPlacement(tex, fi, level, pl))
        return false;
    DecodeRegion(tex, fi, pl, std::max(1u, tex.width >> level), std::max(1u, tex.height >> level), rgba);
    return true;
}

namespace
{
    void DecodeRegion(const XenosTexture& tex, const FormatInfo& fi, const Placement& pl, uint32_t w, uint32_t h,
                      std::vector<uint8_t>& rgba)
    {
        uint32_t log2Bpb = Log2(fi.bytesPerBlock);
        uint32_t blocksW = (w + fi.blockSize - 1) / fi.blockSize, blocksH = (h + fi.blockSize - 1) / fi.blockSize;
        size_t layerTexels = size_t(w) * h;
        rgba.assign(layerTexels * tex.layers * 4, 0);
        uint8_t block[16], tile[16 * 4];
        for (uint32_t layer = 0; layer < tex.layers; layer++)
            for (uint32_t by = 0; by < blocksH; by++)
            {
                uint32_t gy = by + pl.offsetY;
                uint32_t rowOffset = tex.tiled ? TiledRowOffset(gy, pl.pitchBlocks, log2Bpb) : 0;
                for (uint32_t bx = 0; bx < blocksW; bx++)
                {
                    uint32_t gx = bx + pl.offsetX;
                    uint32_t offset = tex.tiled ? TiledOffset(gx, gy, log2Bpb, rowOffset) : (gy * pl.pitchBlocks + gx) * fi.bytesPerBlock;
                    memcpy(block, GuestPtr(pl.address + uint32_t(layer * pl.sliceBytes) + offset), fi.bytesPerBlock);
                    Unswap(block, fi.bytesPerBlock, tex.endian);
                    DecodeBlock(tex.format, block, tile);
                    for (uint32_t ty = 0; ty < fi.blockSize; ty++)
                        for (uint32_t tx = 0; tx < fi.blockSize; tx++)
                        {
                            uint32_t x = bx * fi.blockSize + tx, y = by * fi.blockSize + ty;
                            if (x < w && y < h)
                                memcpy(&rgba[(layer * layerTexels + size_t(y) * w + x) * 4], tile + (ty * fi.blockSize + tx) * 4, 4);
                        }
                }
            }
    }
}

bool DecodeTexture(const XenosTexture& tex, std::vector<uint8_t>& rgba)
{
    FormatInfo fi;
    BaseLayout l;
    if (DecodesToHalf(tex.format) || !GetFormatInfo(tex.format, fi) || !GetBaseLayout(tex, fi, l))
        return false;

    size_t sliceTexels = size_t(tex.width) * tex.height;
    rgba.assign(sliceTexels * l.slices * 4, 0);
    uint8_t block[16], tile[16 * 4];
    for (uint32_t s = 0; s < l.slices; s++)
    for (uint32_t by = 0; by < l.blocksH; by++)
    {
        uint32_t rowOffset = RowOffset(tex, l, by);
        for (uint32_t bx = 0; bx < l.blocksW; bx++)
        {
            memcpy(block, GuestPtr(tex.address + BlockOffset(tex, l, bx, by, s, rowOffset)), fi.bytesPerBlock);
            Unswap(block, fi.bytesPerBlock, tex.endian);
            DecodeBlock(tex.format, block, tile);
            for (uint32_t ty = 0; ty < fi.blockSize; ty++)
                for (uint32_t tx = 0; tx < fi.blockSize; tx++)
                {
                    uint32_t x = bx * fi.blockSize + tx, y = by * fi.blockSize + ty;
                    if (x < tex.width && y < tex.height)
                        memcpy(&rgba[(s * sliceTexels + size_t(y) * tex.width + x) * 4], tile + (ty * fi.blockSize + tx) * 4, 4);
                }
        }
    }
    return true;
}

size_t TextureByteSize(const XenosTexture& tex)
{
    FormatInfo fi;
    BaseLayout l;
    if (!GetFormatInfo(tex.format, fi) || !GetBaseLayout(tex, fi, l))
        return 0;
    // Tiled 3D textures are made of 32x16x4-block macro tiles: whole groups of 4 slices.
    if (l.is3d && tex.tiled)
        return l.sliceBytes * ((l.slices + 3) & ~3u);
    return l.sliceBytes * l.slices;
}
