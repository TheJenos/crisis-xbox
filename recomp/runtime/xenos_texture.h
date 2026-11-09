// Xenos (Xbox 360 GPU) texture fetch constants and texture decoding.
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

struct XenosTexture
{
    uint32_t address = 0;   // guest address of the base level
    uint32_t width = 0, height = 0;
    uint32_t pitch = 0;     // in texels (or blocks' worth of texels for compressed formats)
    uint32_t format = 0;    // Xenos TextureFormat
    uint32_t endian = 0;    // 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32
    uint32_t dimension = 0; // 0 1D, 1 2D, 2 3D, 3 cube
    uint32_t layers = 1;    // cube: 6 faces, stored one after another
    uint32_t depth = 1;     // 3D: slices (decoded one after another, like layers)
    uint32_t mipAddress = 0; // guest address of mip level 1 (0: none)
    uint32_t mipLevels = 1;  // levels to use, including the base
    uint32_t packedMips = 0; // small mips share one tile (the mip tail)
    uint32_t tiled = 0;
    uint32_t signs = 0;      // sign mode of X, Y, Z, W (2 bits each: 0 unsigned, 1 signed, 2 biased, 3 gamma)

    bool operator==(const XenosTexture& o) const
    {
        return address == o.address && width == o.width && height == o.height && format == o.format && layers == o.layers &&
               depth == o.depth;
    }
};

// Parse a 6-dword texture fetch constant (as stored in the register file,
// host-endian dwords). Returns false unless the slot holds a 2D, 3D or cube texture.
bool ParseTextureFetch(const uint32_t fetch[6], XenosTexture& out);

// True if DecodeTexture supports the format.
bool CanDecodeTexture(uint32_t format);
const char* TextureFormatName(uint32_t format);

// Decode the base level to tightly packed RGBA8 (R, G, B, A bytes per texel),
// cube faces or 3D slices one after another. False for DecodesToHalf formats.
bool DecodeTexture(const XenosTexture& tex, std::vector<uint8_t>& rgba);

// Decode mip level `level` (1 .. mipLevels-1) to RGBA8, max(1, width >> level)
// by max(1, height >> level) texels per layer. False if it can't be located.
bool DecodeTextureMip(const XenosTexture& tex, uint32_t level, std::vector<uint8_t>& rgba);

// Bytes of guest memory the base level occupies (0 if the format is unknown).
size_t TextureByteSize(const XenosTexture& tex);

// Write w x h 32-bit texels (as the GPU sees them: little-endian dwords) into
// a tiled 2D surface in guest memory, the way a resolve stores its output.
void WriteTiled32(uint32_t guestAddr, uint32_t pitchTexels, uint32_t w, uint32_t h, const uint32_t* texels, uint32_t endian);

// Float formats (16- and 32-bit float, 1 to 4 components) and the normalized
// 16-bit formats (16, 16_16, 16_16_16_16): decoded to RGBA16F (8 bytes per
// texel, components in X Y Z W order; missing ones 0, alpha 1). Normalized
// 16-bit data has the fetch constant's sign modes (tex.signs) applied here,
// as the shader's sign conversion assumes 8-bit data.
bool IsFloatTextureFormat(uint32_t format);
bool IsNorm16TextureFormat(uint32_t format);
bool DecodesToHalf(uint32_t format); // float or normalized 16-bit
bool DecodeTextureHalf(const XenosTexture& tex, std::vector<uint8_t>& rgbaHalf);
