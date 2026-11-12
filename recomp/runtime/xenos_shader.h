// Xenos (Xbox 360 GPU) shader microcode: decoding, disassembly and WGSL translation.
//
// The instruction set is the one also used by Adreno 200 (documented by Mesa's
// freedreno a2xx driver and by Xenia). A shader is an array of 96-bit slots:
// the first slots hold control-flow (CF) instructions, two 48-bit CFs per slot;
// EXEC CFs then run ALU or fetch instructions from later slots.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xenos
{
    // ---------------------------------------------------------------------
    // Decoded instructions
    // ---------------------------------------------------------------------
    enum class CfOp : uint8_t
    {
        Nop = 0, Exec = 1, ExecEnd = 2, CondExec = 3, CondExecEnd = 4, CondExecPred = 5, CondExecPredEnd = 6,
        LoopStart = 7, LoopEnd = 8, CondCall = 9, Return = 10, CondJmp = 11, Alloc = 12,
        CondExecPredClean = 13, CondExecPredCleanEnd = 14, MarkVsFetchDone = 15,
    };

    struct ControlFlow
    {
        CfOp op;
        uint32_t address = 0;   // EXEC: first instruction slot; jumps/loops: target CF index
        uint32_t count = 0;     // EXEC: number of instructions
        uint32_t sequence = 0;  // EXEC: 2 bits per instruction (bit 0 = fetch, bit 1 = serialize)
        uint32_t boolIndex = 0; // bool-conditioned CFs
        bool condition = false; // run when the bool/predicate equals this
        bool unconditional = false; // CondJmp / CondCall
        bool predicated = false;    // CondJmp / CondCall / LoopEnd (predicated break)
        uint32_t loopId = 0;    // LOOP_*
        uint32_t allocSize = 0, allocType = 0; // ALLOC
        bool IsExec() const;
        bool IsEnd() const;       // END variant (for conditional ones, only when taken)
        bool UsesBool() const;    // exec conditioned on a bool constant
        bool UsesPredicate() const; // exec conditioned on p0
    };

    enum class Addressing : uint8_t { Absolute, A0, Loop };

    struct Operand
    {
        bool temp = false;      // temporary register (else constant)
        uint32_t index = 0;
        Addressing addressing = Addressing::Absolute;
        bool negate = false, abs = false;
        uint8_t swizzle[4] = { 0, 1, 2, 3 }; // absolute component per lane
    };

    struct AluInstr
    {
        uint32_t vectorOp, scalarOp;
        uint32_t vectorDest, scalarDest;
        bool vectorDestRelative, scalarDestRelative;
        uint32_t vectorWriteMask, scalarWriteMask; // raw fields
        bool exportData;        // both results go to the export register vectorDest
        bool vectorClamp, scalarClamp;
        bool predicated, predCondition;
        Operand src[3];         // vector operands 1-3
        Operand scalarSrc[2];   // scalar operands: a, b (resolved per opcode)
        uint32_t scalarOperandCount; // 0, 1 (a), 2 (a, b)
        // Export write masks after the constant-0/1 rules.
        uint32_t VectorResultMask() const;
        uint32_t ScalarResultMask() const;
        uint32_t Constant0Mask() const;
        uint32_t Constant1Mask() const;
    };

    struct FetchInstr
    {
        uint32_t op;            // 0 vertex fetch, 1 texture fetch, 16+ texture special ops
        uint32_t srcReg, dstReg;
        bool srcRelative = false, dstRelative = false;
        uint8_t srcSwizzle[3] = {};  // texture: coordinate components; vertex: [0] = index component
        uint8_t dstSwizzle[4] = {};  // 0-3 xyzw, 4 = 0.0, 5 = 1.0, 7 = keep
        uint32_t constIndex = 0;     // vertex: 0-95 (index*3 + sel); texture: 0-31
        bool predicated = false, predCondition = false;
        // vertex fetch
        uint32_t format = 0, stride = 0;
        int32_t offset = 0;
        bool signedFormat = false, normalized = false, mini = false, roundIndex = false;
        int32_t expAdjust = 0;
        // texture fetch
        uint32_t dimension = 1; // 0 1D, 1 2D, 2 3D/stacked, 3 cube
        bool unnormalizedCoords = false, useRegisterLod = false, useComputedLod = true;
        float lodBias = 0, offsetX = 0, offsetY = 0, offsetZ = 0;
        // Filters: 0 point, 1 linear, 2 base map (mip only), 3 as the fetch constant says.
        uint8_t magFilter = 3, minFilter = 3, mipFilter = 3;
    };

    struct Instr
    {
        bool used = false;
        bool fetch = false;
        AluInstr alu{};
        FetchInstr fetchInstr{};
    };

    struct DecodedShader
    {
        bool pixel = false;
        std::vector<ControlFlow> cf;
        std::vector<Instr> slots; // indexed by instruction slot
        std::string error;        // non-empty if decoding failed
    };

    DecodedShader Decode(const uint32_t* words, size_t count, bool pixel);
    std::string Disassemble(const DecodedShader& shader);
    const char* VectorOpName(uint32_t op);
    const char* ScalarOpName(uint32_t op);

    // ---------------------------------------------------------------------
    // WGSL translation
    //
    // Resource interface (shared by both stages; bind with layout "auto"):
    //   @group(0) @binding(0) uniform  float constants, 512 vec4 (VS 0-255, PS 256-511)
    //   @group(0) @binding(1) uniform  Misc: bool constants, loop constants,
    //                                  vertex fetch bases, position transform
    //   @group(0) @binding(2) storage  vertex data (u32, already endian-swapped), VS only
    //   @group(1) @binding(2n), (2n+1) texture and sampler for fetch constant n
    // Vertex shader outputs: position + 16 vec4 interpolators at @location(0-15);
    // pixel shader inputs: the same 16 interpolators, loaded into r0-r15.
    // ---------------------------------------------------------------------
    constexpr uint32_t kInterpolators = 16;

    struct VertexFetchUse
    {
        uint32_t constIndex, format, stride;
        int32_t offset;
        bool signedFormat, normalized;
    };

    struct Translation
    {
        std::string wgsl;
        std::string error; // non-empty: shader can't be translated yet
        std::vector<VertexFetchUse> vertexFetches;
        uint32_t vertexFetchMask[3] = {};  // vertex fetch constants used (96 bits)
        uint32_t textureMask = 0;          // texture fetch constants used
        uint8_t textureDimension[32] = {}; // per texture fetch constant (as in FetchInstr)
        // Per texture fetch constant: filters its fetch instructions set
        // (mag, min, mip as in FetchInstr; 3 = the fetch constant's). Point
        // wins over linear when instructions disagree.
        uint8_t filterOverride[32][3] = {
            {3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},
            {3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3},{3,3,3} };
        uint32_t interpolatorMask = 0;     // VS: interpolators written
        bool writesPointSize = false, usesKill = false, writesDepth = false;
        uint32_t colorTargetMask = 0;      // PS: color exports
        bool usesStateMachine = false;     // non-linear control flow (jumps, loops, calls)
        std::vector<ControlFlow> controlFlow; // state-machine shaders: their CF list (runaway-loop checks)
    };

    Translation TranslateToWgsl(const DecodedShader& shader);

    // Debugging (SHADER_PROBE): the next translated pixel shader outputs
    // r[reg] * scale as color 0 at the start of instruction `instr`.
    struct ShaderProbe { bool active = false; uint32_t instr = 0, reg = 0; float scale = 1.0f; char swizzle[8] = "xyzw"; };
    extern ShaderProbe g_probe;

    // Layout of the Misc uniform block (binding 1), in bytes; must match the WGSL.
    struct MiscConstants
    {
        uint32_t bools[8];          // 256 bool constants
        uint32_t loops[32];         // loop constants: count | start << 8 | step << 16
        uint32_t vertexBase[96];    // dword offset of each vertex fetch constant's data in the storage buffer
        float positionScale[4];     // position.xyz = position.xyz * scale + offset * position.w
        float positionOffset[4];
        float alphaRef[4];          // alpha test reference, function; [2] = rect list corner offset
        uint32_t textureSwizzle[32]; // per fetch slot: component swizzle applied after sampling
        float textureScale[32][4];   // per fetch slot and output channel: number format and exponent adjust
        float paramGen[4];           // pixel position input: register (-1 none), pixel center offset
        // Per fetch slot: normalized 1D/2D coordinates are multiplied by this.
        // Resolved textures are sized by the copy's destination pitch (e.g.
        // 32x8), which can exceed the size the fetch constant gives (e.g. 1x1).
        float texcoordScale[32][2];
        // Per fetch slot: host texels per guest texel (resolved textures of
        // targets rendered at a reduced size, render::TargetScale), for
        // unnormalized coordinates.
        float texelScale[32];
    };
}
