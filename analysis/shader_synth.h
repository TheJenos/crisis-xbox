// Synthetic decoded shaders that exercise every opcode, operand form, vertex
// format, texture dimension and control-flow construct of the WGSL generator
// (the game's shader cache doesn't use loops, calls, kills or relative
// addressing, so those paths need their own tests).
#pragma once
#include "xenos_shader.h"

#include <string>
#include <utility>
#include <vector>

namespace synth
{
    using namespace xenos;

    inline Operand T(uint32_t r, uint8_t sx = 0, uint8_t sy = 1, uint8_t sz = 2, uint8_t sw = 3, bool neg = false, bool abs = false)
    {
        Operand o;
        o.temp = true;
        o.index = r;
        o.negate = neg;
        o.abs = abs;
        o.swizzle[0] = sx; o.swizzle[1] = sy; o.swizzle[2] = sz; o.swizzle[3] = sw;
        return o;
    }

    inline Operand K(uint32_t c, Addressing a = Addressing::Absolute, bool neg = false, bool abs = false)
    {
        Operand o;
        o.temp = false;
        o.index = c;
        o.addressing = a;
        o.negate = neg;
        o.abs = abs;
        return o;
    }

    inline Operand Lane(Operand o, uint8_t c)
    {
        for (auto& s : o.swizzle) s = c;
        return o;
    }

    inline Instr Alu(uint32_t vop, uint32_t sop, uint32_t vdst, uint32_t vmask, uint32_t sdst, uint32_t smask,
                     Operand a, Operand b, Operand c, bool exportData = false)
    {
        Instr in;
        in.used = true;
        in.fetch = false;
        AluInstr& x = in.alu;
        x.vectorOp = vop;
        x.scalarOp = sop;
        x.vectorDest = vdst;
        x.vectorWriteMask = vmask;
        x.scalarDest = sdst;
        x.scalarWriteMask = smask;
        x.exportData = exportData;
        x.vectorDestRelative = x.scalarDestRelative = false;
        x.vectorClamp = x.scalarClamp = false;
        x.predicated = x.predCondition = false;
        x.src[0] = a; x.src[1] = b; x.src[2] = c;
        x.scalarSrc[0] = Lane(c, 3);
        x.scalarSrc[1] = Lane(c, 0);
        x.scalarOperandCount = 2;
        return in;
    }

    inline Instr VFetch(uint32_t dst, uint32_t format, uint32_t stride, int32_t offset, bool sg, bool norm, bool mini = false, uint32_t vf = 0)
    {
        Instr in;
        in.used = true;
        in.fetch = true;
        FetchInstr& f = in.fetchInstr;
        f.op = 0;
        f.srcReg = 0;
        f.dstReg = dst;
        for (int i = 0; i < 4; i++) f.dstSwizzle[i] = uint8_t(i);
        f.constIndex = vf;
        f.format = format;
        f.stride = stride;
        f.offset = offset;
        f.signedFormat = sg;
        f.normalized = norm;
        f.mini = mini;
        return in;
    }

    inline Instr TFetch(uint32_t op, uint32_t dst, uint32_t src, uint32_t slot, uint32_t dim)
    {
        Instr in;
        in.used = true;
        in.fetch = true;
        FetchInstr& f = in.fetchInstr;
        f.op = op;
        f.srcReg = src;
        f.dstReg = dst;
        f.srcSwizzle[0] = 0; f.srcSwizzle[1] = 1; f.srcSwizzle[2] = 2;
        f.dstSwizzle[0] = 0; f.dstSwizzle[1] = 1; f.dstSwizzle[2] = 2; f.dstSwizzle[3] = 5;
        f.constIndex = slot;
        f.dimension = dim;
        return in;
    }

    struct Builder
    {
        DecodedShader s;
        explicit Builder(bool pixel) { s.pixel = pixel; }

        // Adds an EXEC-type CF running `instrs` (split into groups of up to 7).
        void Exec(std::vector<Instr> instrs, CfOp op = CfOp::Exec, uint32_t boolIndex = 0, bool cond = true)
        {
            for (size_t i = 0; i < instrs.size(); i += 7)
            {
                size_t n = std::min<size_t>(7, instrs.size() - i);
                ControlFlow c{};
                c.op = (i + 7 >= instrs.size()) ? op : (op == CfOp::ExecEnd ? CfOp::Exec : op == CfOp::CondExecEnd ? CfOp::CondExec : op);
                c.address = uint32_t(s.slots.size());
                c.count = uint32_t(n);
                c.boolIndex = boolIndex;
                c.condition = cond;
                for (size_t k = 0; k < n; k++)
                {
                    if (instrs[i + k].fetch) c.sequence |= 1u << (k * 2);
                    s.slots.push_back(instrs[i + k]);
                }
                s.cf.push_back(c);
            }
        }

        uint32_t Cf(CfOp op, uint32_t address = 0, uint32_t loopId = 0, bool predicated = false, bool cond = true, bool unconditional = false, uint32_t boolIndex = 0)
        {
            ControlFlow c{};
            c.op = op;
            c.address = address;
            c.loopId = loopId;
            c.predicated = predicated;
            c.condition = cond;
            c.unconditional = unconditional;
            c.boolIndex = boolIndex;
            s.cf.push_back(c);
            return uint32_t(s.cf.size() - 1);
        }
    };

    inline std::vector<std::pair<std::string, DecodedShader>> All()
    {
        std::vector<std::pair<std::string, DecodedShader>> out;

        // Every vector op paired with every scalar op, all operand forms.
        for (int pixel = 0; pixel < 2; pixel++)
        {
            Builder b(pixel);
            std::vector<Instr> v;
            for (uint32_t i = 0; i <= 50; i++)
            {
                uint32_t vop = i % 30, sop = i == 41 ? 50 : i;
                Operand a = (i % 3 == 0) ? K(i, Addressing::Absolute, i % 2, i % 4 == 0) : T(i % 6, 3, 2, 1, 0, i % 5 == 0, i % 7 == 0);
                Operand bb = (i % 4 == 1) ? K(i + 1, i % 8 == 1 ? Addressing::A0 : Addressing::Loop) : T((i + 1) % 6);
                Operand c = T((i + 2) % 6, 0, 0, 1, 1);
                if (sop >= 42 && sop <= 47)
                {
                    c = K(7);
                }
                Instr in = Alu(vop, sop, i % 8, 0xF >> (i % 3), (i + 3) % 8, 1u << (i % 4), a, bb, c);
                if (sop >= 42 && sop <= 47)
                {
                    in.alu.scalarSrc[0] = Lane(K(7), 3);
                    in.alu.scalarSrc[1] = Lane(T(2), 0);
                }
                in.alu.vectorClamp = i % 5 == 1;
                in.alu.scalarClamp = i % 6 == 1;
                in.alu.predicated = i % 9 == 4;
                in.alu.predCondition = i % 2;
                v.push_back(in);
            }
            if (!pixel)
            {
                v.push_back(VFetch(6, 38, 4, 0, false, false));
                for (uint32_t o = 0; o < 16; o++)
                    v.push_back(Alu(2, 50, o, 0xF, 0, 0, T(o % 8), T(o % 8), T(0), true));
                Instr pos = Alu(15, 2, 62, 0x7, 0, 0x8, T(1), K(4, Addressing::A0), T(2), true);
                v.push_back(pos);
                Instr pts = Alu(2, 0, 63, 0x1, 0, 0x2, T(1), T(1), T(1), true);
                pts.alu.scalarDestRelative = true; // constant 0 in .zw
                v.push_back(pts);
            }
            else
            {
                v.push_back(TFetch(1, 1, 0, 0, 0));
                v.push_back(TFetch(1, 2, 0, 1, 1));
                Instr t3 = TFetch(1, 3, 0, 2, 2);
                t3.fetchInstr.unnormalizedCoords = true;
                v.push_back(t3);
                v.push_back(TFetch(1, 4, 0, 3, 3));
                v.push_back(TFetch(24, 0, 1, 0, 1));
                Instr lod = TFetch(1, 5, 0, 4, 1);
                lod.fetchInstr.useRegisterLod = true;
                v.push_back(lod);
                Instr bias = TFetch(1, 5, 0, 5, 1);
                bias.fetchInstr.lodBias = -1.5f;
                bias.fetchInstr.predicated = true;
                v.push_back(bias);
                v.push_back(TFetch(19, 6, 0, 1, 1));
                v.push_back(TFetch(17, 6, 0, 1, 1));
                for (uint32_t o = 0; o < 4; o++)
                    v.push_back(Alu(2, 50, o, 0xF, 0, 0, T(o), T(o), T(0), true));
                v.push_back(Alu(2, 50, 61, 0x0, 0, 0x1, T(1), T(1), T(1), true)); // oDepth from scalar
            }
            b.Exec(v, CfOp::ExecEnd);
            out.push_back({ pixel ? "ps_synth_ops" : "vs_synth_ops", b.s });
        }

        // Every vertex format, signed/unsigned x normalized/integer, mini fetches, exp adjust.
        {
            Builder b(false);
            std::vector<Instr> v;
            uint32_t formats[] = { 6, 7, 16, 17, 25, 26, 31, 32, 33, 34, 35, 36, 37, 38, 57 };
            uint32_t vf = 0;
            for (uint32_t fmt : formats)
                for (int variant = 0; variant < 4; variant++)
                {
                    Instr f = VFetch(1 + variant, fmt, 9, variant, variant & 1, variant & 2, variant == 3, vf % 96);
                    f.fetchInstr.expAdjust = variant == 2 ? -3 : 0;
                    f.fetchInstr.roundIndex = variant == 1;
                    f.fetchInstr.dstSwizzle[3] = uint8_t(variant == 0 ? 5 : variant == 1 ? 4 : variant == 2 ? 7 : 3);
                    v.push_back(f);
                    vf += 7;
                }
            v.push_back(Alu(2, 50, 62, 0xF, 0, 0, T(1), T(2), T(0), true));
            b.Exec(v, CfOp::ExecEnd);
            out.push_back({ "vs_synth_formats", b.s });
        }

        // Control flow: loops (with predicated break and aL-relative access),
        // calls/returns, jumps of every kind, conditional ends.
        for (int pixel = 0; pixel < 2; pixel++)
        {
            Builder b(pixel);
            b.Exec({ Alu(0, 27, 1, 0xF, 2, 0x1, T(0), K(1), T(0)) }, CfOp::CondExec, 3, true);   // 0
            uint32_t loopStart = b.Cf(CfOp::LoopStart, 0, 1);                                     // 1
            Instr rel = Alu(11, 29, 3, 0xF, 4, 0x2, T(0), K(10, Addressing::Loop), T(1));
            rel.alu.src[0].addressing = Addressing::Loop; // r[0 + aL]
            rel.alu.vectorDestRelative = true;
            b.Exec({ rel, pixel ? Alu(24, 50, 0, 0, 0, 0, T(1), T(2), T(0)) : Alu(29, 23, 5, 0xF, 6, 0x1, T(1), T(2), T(3)) }); // 2
            b.Exec({ pixel ? TFetch(1, 7, 1, 0, 1) : VFetch(7, 38, 4, 0, false, false) }, CfOp::CondExecPred, 0, true); // 3
            uint32_t loopEnd = b.Cf(CfOp::LoopEnd, loopStart + 1, 1, true, true);                 // 4
            b.s.cf[loopStart].address = loopEnd + 1;
            uint32_t call = b.Cf(CfOp::CondCall, 0, 0, false, true, false, 7);                     // 5
            uint32_t jmp = b.Cf(CfOp::CondJmp, 0, 0, true, false);                                 // 6
            b.Exec({ Alu(0, 35, 1, 0xF, 2, 0, T(1), T(2), T(3)) }, CfOp::CondExecPredEnd, 0, false); // 7
            uint32_t uj = b.Cf(CfOp::CondJmp, 0, 0, false, true, true);                            // 8
            uint32_t fn = uint32_t(b.s.cf.size());                                                 // 9: function
            b.Exec({ Alu(1, 22, 2, 0xF, 3, 0x4, T(2), K(3, Addressing::A0), T(2)) });
            b.Cf(CfOp::Return);
            uint32_t tail = uint32_t(b.s.cf.size());
            if (!pixel)
                b.Exec({ Alu(2, 50, 62, 0xF, 0, 0, T(1), T(1), T(0), true), Alu(2, 50, 0, 0xF, 0, 0, T(2), T(2), T(0), true) }, CfOp::ExecEnd);
            else
                b.Exec({ Alu(2, 50, 0, 0xF, 0, 0, T(1), T(1), T(0), true) }, CfOp::ExecEnd);
            b.s.cf[call].address = fn;
            b.s.cf[jmp].address = tail;
            b.s.cf[uj].address = tail;
            out.push_back({ pixel ? "ps_synth_flow" : "vs_synth_flow", b.s });
        }
        return out;
    }
}
