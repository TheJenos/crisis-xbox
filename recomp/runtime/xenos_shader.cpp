// Xenos shader microcode decoding and disassembly. (WGSL translation is in
// xenos_wgsl.cpp.) Field positions follow the Xenos ISA as documented by
// Xenia (src/xenia/gpu/ucode.h) and Mesa freedreno (a2xx).
#include "xenos_shader.h"

#include <algorithm>
#include <cstdio>

namespace xenos
{
    bool ControlFlow::IsExec() const
    {
        switch (op)
        {
        case CfOp::Exec: case CfOp::ExecEnd: case CfOp::CondExec: case CfOp::CondExecEnd:
        case CfOp::CondExecPred: case CfOp::CondExecPredEnd: case CfOp::CondExecPredClean: case CfOp::CondExecPredCleanEnd:
            return true;
        default:
            return false;
        }
    }

    bool ControlFlow::IsEnd() const
    {
        return op == CfOp::ExecEnd || op == CfOp::CondExecEnd || op == CfOp::CondExecPredEnd || op == CfOp::CondExecPredCleanEnd;
    }

    // CondExecPredClean(End) are conditioned on a bool constant, like CondExec.
    bool ControlFlow::UsesBool() const
    {
        return op == CfOp::CondExec || op == CfOp::CondExecEnd || op == CfOp::CondExecPredClean || op == CfOp::CondExecPredCleanEnd;
    }

    bool ControlFlow::UsesPredicate() const
    {
        return op == CfOp::CondExecPred || op == CfOp::CondExecPredEnd;
    }

    uint32_t AluInstr::VectorResultMask() const
    {
        return exportData ? vectorWriteMask & ~scalarWriteMask : vectorWriteMask;
    }

    uint32_t AluInstr::ScalarResultMask() const
    {
        return exportData ? scalarWriteMask & ~vectorWriteMask : scalarWriteMask;
    }

    // Exports: components written by neither op are 0 when scalar_dest_rel is
    // set; components written by both are 1.
    uint32_t AluInstr::Constant0Mask() const
    {
        return exportData && scalarDestRelative ? 0xF & ~(vectorWriteMask | scalarWriteMask) : 0;
    }

    uint32_t AluInstr::Constant1Mask() const
    {
        return exportData ? vectorWriteMask & scalarWriteMask : 0;
    }

    namespace
    {
        uint32_t Bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
        int32_t SBits(uint32_t v, int lo, int n) { return int32_t(v << (32 - lo - n)) >> (32 - n); }

        ControlFlow DecodeCf(uint64_t w)
        {
            ControlFlow c{};
            c.op = CfOp((w >> 44) & 0xF);
            c.condition = (w >> 42) & 1;
            if (c.IsExec())
            {
                c.address = uint32_t(w & 0xFFF);
                c.count = uint32_t((w >> 12) & 7);
                c.sequence = uint32_t((w >> 16) & 0xFFF);
                c.boolIndex = uint32_t((w >> 34) & 0xFF);
            }
            else if (c.op == CfOp::LoopStart || c.op == CfOp::LoopEnd)
            {
                c.address = uint32_t(w & 0x1FFF);
                c.loopId = uint32_t((w >> 16) & 0x1F);
                c.predicated = c.op == CfOp::LoopEnd && ((w >> 21) & 1);
            }
            else if (c.op == CfOp::CondCall || c.op == CfOp::CondJmp)
            {
                c.address = uint32_t(w & 0x1FFF);
                c.unconditional = (w >> 13) & 1;
                c.predicated = (w >> 14) & 1;
                c.boolIndex = uint32_t((w >> 34) & 0xFF);
            }
            else if (c.op == CfOp::Alloc)
            {
                c.allocSize = uint32_t(w & 0xF);
                c.allocType = uint32_t((w >> 41) & 3);
            }
            return c;
        }

        Operand MakeOperand(uint32_t reg, bool temp, bool negate, bool absConstants, bool constAddressed, bool a0Relative)
        {
            Operand o;
            o.temp = temp;
            o.negate = negate;
            if (temp)
            {
                o.index = reg & 0x3F;
                o.addressing = (reg & 0x40) ? Addressing::Loop : Addressing::Absolute;
                o.abs = (reg >> 7) & 1;
            }
            else
            {
                o.index = reg;
                o.addressing = constAddressed ? (a0Relative ? Addressing::A0 : Addressing::Loop) : Addressing::Absolute;
                o.abs = absConstants;
            }
            return o;
        }

        // Swizzles are relative: lane i reads component (i + field_i) & 3.
        uint8_t SwizzleComponent(uint32_t swiz, int lane) { return uint8_t((((swiz >> (lane * 2)) & 3) + lane) & 3); }

        // Scalar opcodes: 0 = no operand, 1 = a (W lane), 2 = a and b (W and X lanes), 3 = constant a + temporary b.
        int ScalarOperandKind(uint32_t op)
        {
            switch (op)
            {
            case 0: case 2: case 4: case 5: case 6: case 23: case 24: case 25:
                return 2;
            case 33: case 50:
                return 0;
            case 42: case 43: case 44: case 45: case 46: case 47:
                return 3;
            default:
                return 1;
            }
        }

        AluInstr DecodeAlu(const uint32_t* w)
        {
            AluInstr a{};
            a.vectorDest = Bits(w[0], 0, 6);
            a.vectorDestRelative = Bits(w[0], 6, 1);
            bool absConstants = Bits(w[0], 7, 1);
            a.scalarDest = Bits(w[0], 8, 6);
            a.scalarDestRelative = Bits(w[0], 14, 1);
            a.exportData = Bits(w[0], 15, 1);
            a.vectorWriteMask = Bits(w[0], 16, 4);
            a.scalarWriteMask = Bits(w[0], 20, 4);
            a.vectorClamp = Bits(w[0], 24, 1);
            a.scalarClamp = Bits(w[0], 25, 1);
            a.scalarOp = Bits(w[0], 26, 6);
            a.predCondition = Bits(w[1], 27, 1);
            a.predicated = Bits(w[1], 28, 1);
            bool a0Relative = Bits(w[1], 29, 1);
            bool const1Rel = Bits(w[1], 30, 1);
            bool const0Rel = Bits(w[1], 31, 1);
            a.vectorOp = Bits(w[2], 24, 5);

            uint32_t reg[3] = { Bits(w[2], 16, 8), Bits(w[2], 8, 8), Bits(w[2], 0, 8) };
            bool temp[3] = { (bool)Bits(w[2], 31, 1), (bool)Bits(w[2], 30, 1), (bool)Bits(w[2], 29, 1) };
            uint32_t swiz[3] = { Bits(w[1], 16, 8), Bits(w[1], 8, 8), Bits(w[1], 0, 8) };
            bool neg[3] = { (bool)Bits(w[1], 26, 1), (bool)Bits(w[1], 25, 1), (bool)Bits(w[1], 24, 1) };
            // Which of const_0_rel_abs / const_1_rel_abs applies depends on how
            // many earlier operands are constants.
            bool addressed[3] = { const0Rel, temp[0] ? const0Rel : const1Rel, (temp[0] && temp[1]) ? const0Rel : const1Rel };

            for (int i = 0; i < 3; i++)
            {
                a.src[i] = MakeOperand(reg[i], temp[i], neg[i], absConstants, addressed[i], a0Relative);
                for (int l = 0; l < 4; l++)
                    a.src[i].swizzle[l] = SwizzleComponent(swiz[i], l);
            }

            int kind = ScalarOperandKind(a.scalarOp);
            if (kind == 1 || kind == 2)
            {
                a.scalarOperandCount = kind;
                a.scalarSrc[0] = a.src[2];
                a.scalarSrc[1] = a.src[2];
                uint8_t ca = SwizzleComponent(swiz[2], 3), cb = SwizzleComponent(swiz[2], 0);
                for (int l = 0; l < 4; l++)
                {
                    a.scalarSrc[0].swizzle[l] = ca;
                    a.scalarSrc[1].swizzle[l] = cb;
                }
            }
            else if (kind == 3)
            {
                // Constant (src3 register, W lane) and a temporary whose index is
                // packed into the opcode's low bit, src3_sel and the swizzle.
                a.scalarOperandCount = 2;
                a.scalarSrc[0] = MakeOperand(reg[2], false, neg[2], absConstants, addressed[2], a0Relative);
                Operand t;
                t.temp = true;
                t.index = (a.scalarOp & 1) | (uint32_t(temp[2]) << 1) | (swiz[2] & 0x3C);
                t.negate = neg[2];
                t.abs = absConstants;
                a.scalarSrc[1] = t;
                uint8_t ca = SwizzleComponent(swiz[2], 3), cb = SwizzleComponent(swiz[2], 0);
                for (int l = 0; l < 4; l++)
                {
                    a.scalarSrc[0].swizzle[l] = ca;
                    a.scalarSrc[1].swizzle[l] = cb;
                }
            }
            return a;
        }

        FetchInstr DecodeFetch(const uint32_t* w)
        {
            FetchInstr f{};
            f.op = Bits(w[0], 0, 5);
            f.srcReg = Bits(w[0], 5, 6);
            f.srcRelative = Bits(w[0], 11, 1);
            f.dstReg = Bits(w[0], 12, 6);
            f.dstRelative = Bits(w[0], 18, 1);
            for (int i = 0; i < 4; i++)
                f.dstSwizzle[i] = uint8_t(Bits(w[1], i * 3, 3));
            f.predicated = Bits(w[1], 31, 1);
            f.predCondition = Bits(w[2], 31, 1);
            if (f.op == 0) // vertex fetch
            {
                f.constIndex = Bits(w[0], 20, 5) * 3 + Bits(w[0], 25, 2);
                f.srcSwizzle[0] = uint8_t(Bits(w[0], 30, 2));
                f.signedFormat = Bits(w[1], 12, 1);
                f.normalized = !Bits(w[1], 13, 1);
                f.roundIndex = Bits(w[1], 15, 1);
                f.format = Bits(w[1], 16, 6);
                f.expAdjust = SBits(w[1], 24, 6);
                f.mini = Bits(w[1], 30, 1);
                f.stride = Bits(w[2], 0, 8);
                f.offset = SBits(w[2], 8, 23);
            }
            else
            {
                f.constIndex = Bits(w[0], 20, 5);
                f.unnormalizedCoords = Bits(w[0], 25, 1);
                for (int i = 0; i < 3; i++)
                    f.srcSwizzle[i] = uint8_t(Bits(w[0], 26 + i * 2, 2));
                f.magFilter = uint8_t(Bits(w[1], 12, 2));
                f.minFilter = uint8_t(Bits(w[1], 14, 2));
                f.mipFilter = uint8_t(Bits(w[1], 16, 2));
                f.useComputedLod = Bits(w[1], 28, 1);
                f.useRegisterLod = Bits(w[1], 29, 1);
                f.lodBias = SBits(w[2], 2, 7) / 16.0f;
                f.dimension = Bits(w[2], 14, 2);
                f.offsetX = SBits(w[2], 16, 5) * 0.5f;
                f.offsetY = SBits(w[2], 21, 5) * 0.5f;
                f.offsetZ = SBits(w[2], 26, 5) * 0.5f;
            }
            return f;
        }
    }

    DecodedShader Decode(const uint32_t* words, size_t count, bool pixel)
    {
        DecodedShader s;
        s.pixel = pixel;
        size_t slots = count / 3;
        s.slots.resize(slots);

        // CF pairs occupy the slots before the first instruction any EXEC refers
        // to. An unconditional EXEC_END also ends the CF program.
        size_t cfLimit = slots;
        bool ended = false;
        for (size_t i = 0; i < cfLimit && !ended; i++)
        {
            const uint32_t* w = words + i * 3;
            uint64_t cfWords[2] = { uint64_t(w[0]) | (uint64_t(w[1] & 0xFFFF) << 32),
                                    uint64_t(w[1] >> 16) | (uint64_t(w[2]) << 16) };
            for (uint64_t cw : cfWords)
            {
                ControlFlow c = DecodeCf(cw);
                if (c.IsExec() && c.count)
                    cfLimit = std::min<size_t>(cfLimit, c.address);
                s.cf.push_back(c);
                if (c.op == CfOp::ExecEnd)
                {
                    ended = true;
                    break;
                }
            }
        }
        if (!ended)
        {
            // Drop trailing NOP padding; the program must still end somewhere.
            while (!s.cf.empty() && s.cf.back().op == CfOp::Nop)
                s.cf.pop_back();
            bool anyEnd = false;
            for (const auto& c : s.cf)
                anyEnd |= c.IsEnd();
            if (!anyEnd)
                s.error = "no END control-flow instruction";
        }

        for (const auto& c : s.cf)
        {
            if (!c.IsExec())
                continue;
            for (uint32_t k = 0; k < c.count; k++)
            {
                uint32_t slot = c.address + k;
                if (slot >= slots)
                {
                    s.error = "EXEC refers past the end of the shader";
                    return s;
                }
                Instr& in = s.slots[slot];
                in.used = true;
                in.fetch = (c.sequence >> (k * 2)) & 1;
                if (in.fetch)
                    in.fetchInstr = DecodeFetch(words + slot * 3);
                else
                    in.alu = DecodeAlu(words + slot * 3);
            }
        }
        return s;
    }

    // ---------------------------------------------------------------------
    // Disassembly
    // ---------------------------------------------------------------------
    const char* VectorOpName(uint32_t op)
    {
        static const char* names[32] = {
            "add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad", "cndeq", "cndge",
            "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push", "setp_ne_push", "setp_gt_push",
            "setp_ge_push", "kill_eq", "kill_gt", "kill_ge", "kill_ne", "dst", "maxa", "op30", "op31" };
        return names[op & 31];
    }

    const char* ScalarOpName(uint32_t op)
    {
        static const char* names[64] = {
            "adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs", "sgts", "sges", "snes",
            "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc", "rcpf", "rcp", "rsqc", "rsqf", "rsq", "maxas",
            "maxasf", "subs", "subs_prev", "setp_eq", "setp_ne", "setp_gt", "setp_ge", "setp_inv", "setp_pop",
            "setp_clr", "setp_rstr", "kills_eq", "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt", "op41",
            "mulsc0", "mulsc1", "addsc0", "addsc1", "subsc0", "subsc1", "sin", "cos", "retain_prev", "op51", "op52",
            "op53", "op54", "op55", "op56", "op57", "op58", "op59", "op60", "op61", "op62", "op63" };
        return names[op & 63];
    }

    namespace
    {
        std::string OperandText(const Operand& o, bool pixel, int components = 4)
        {
            static const char xyzw[] = "xyzw";
            uint32_t idx = o.temp ? o.index : o.index + (pixel ? 256 : 0);
            std::string s = o.negate ? "-" : "";
            if (o.abs) s += "|";
            s += o.temp ? 'r' : 'c';
            if (o.addressing == Addressing::Absolute)
                s += std::to_string(idx);
            else
                s += "[" + std::to_string(idx) + (o.addressing == Addressing::A0 ? "+a0]" : "+aL]");
            s += ".";
            for (int i = 0; i < components; i++)
                s += xyzw[o.swizzle[i == 0 && components < 4 ? 3 : i]];
            if (o.abs) s += "|";
            return s;
        }

        std::string MaskText(uint32_t mask)
        {
            std::string s;
            for (int i = 0; i < 4; i++)
                s += (mask >> i) & 1 ? "xyzw"[i] : '_';
            return s;
        }

        std::string DestText(uint32_t reg, bool exportData, bool pixel)
        {
            char buf[24];
            if (!exportData)
                snprintf(buf, sizeof(buf), "r%u", reg);
            else if (!pixel && reg == 62)
                snprintf(buf, sizeof(buf), "oPos");
            else if (!pixel && reg == 63)
                snprintf(buf, sizeof(buf), "oPts");
            else if (pixel && reg == 61)
                snprintf(buf, sizeof(buf), "oDepth");
            else
                snprintf(buf, sizeof(buf), pixel ? "oC%u" : "o%u", reg);
            return buf;
        }
    }

    std::string Disassemble(const DecodedShader& s)
    {
        std::string out;
        char line[256];
        static const char* cfNames[16] = { "nop", "exec", "exec_end", "cexec", "cexec_end", "cexec_pred", "cexec_pred_end",
                                           "loop_start", "loop_end", "ccall", "return", "cjmp", "alloc",
                                           "cexec_pred_clean", "cexec_pred_clean_end", "vs_fetch_done" };
        snprintf(line, sizeof(line), "; %s shader, %zu slots%s%s\n", s.pixel ? "pixel" : "vertex", s.slots.size(),
                 s.error.empty() ? "" : ", error: ", s.error.c_str());
        out += line;
        for (const auto& c : s.cf)
        {
            snprintf(line, sizeof(line), "  %-16s", cfNames[int(c.op)]);
            out += line;
            if (c.IsExec())
            {
                snprintf(line, sizeof(line), " addr %u count %u seq %03X", c.address, c.count, c.sequence);
                out += line;
                if (c.UsesBool())
                {
                    snprintf(line, sizeof(line), " if b%u == %d", c.boolIndex, c.condition);
                    out += line;
                }
                else if (c.UsesPredicate())
                {
                    snprintf(line, sizeof(line), " if p0 == %d", c.condition);
                    out += line;
                }
                out += "\n";
                for (uint32_t k = 0; k < c.count; k++)
                {
                    if (c.address + k >= s.slots.size())
                    {
                        snprintf(line, sizeof(line), "      %3u  (past the end)\n", c.address + k);
                        out += line;
                        continue;
                    }
                    const Instr& in = s.slots[c.address + k];
                    if (in.fetch)
                    {
                        const FetchInstr& f = in.fetchInstr;
                        static const char comp[] = "xyzw01?_";
                        std::string dsw;
                        for (int i = 0; i < 4; i++) dsw += comp[f.dstSwizzle[i]];
                        if (f.op == 0)
                            snprintf(line, sizeof(line), "      %3u  vfetch%s r%u.%s, r%u.%c, vf%u (fmt %u%s%s, stride %u, offset %d%s)\n",
                                     c.address + k, f.mini ? "_mini" : "", f.dstReg, dsw.c_str(), f.srcReg, "xyzw"[f.srcSwizzle[0]],
                                     f.constIndex, f.format, f.signedFormat ? " signed" : "", f.normalized ? " norm" : " int",
                                     f.stride, f.offset, f.expAdjust ? " expadj" : "");
                        else
                            snprintf(line, sizeof(line), "      %3u  tfetch%s r%u.%s, r%u.%c%c%c, tf%u%s\n", c.address + k,
                                     f.op == 1 ? (f.dimension == 1 ? "2D" : f.dimension == 2 ? "3D" : f.dimension == 3 ? "Cube" : "1D") : "_op",
                                     f.dstReg, dsw.c_str(), f.srcReg, "xyzw"[f.srcSwizzle[0]], "xyzw"[f.srcSwizzle[1]],
                                     "xyzw"[f.srcSwizzle[2]], f.constIndex, f.op > 1 ? " (special)" : "");
                        out += line;
                    }
                    else
                    {
                        const AluInstr& a = in.alu;
                        std::string pred = a.predicated ? (a.predCondition ? "(p0) " : "(!p0) ") : "";
                        uint32_t c0 = a.Constant0Mask(), c1 = a.Constant1Mask();
                        if (a.VectorResultMask() || c0 || c1 || a.vectorOp >= 20)
                        {
                            std::string mask;
                            uint32_t vm = a.VectorResultMask();
                            for (int i = 0; i < 4; i++)
                                mask += (vm >> i) & 1 ? "xyzw"[i] : (c0 >> i) & 1 ? '0' : (c1 >> i) & 1 ? '1' : '_';
                            snprintf(line, sizeof(line), "      %3u  %s%s%s %s.%s, %s, %s, %s\n", c.address + k, pred.c_str(),
                                     VectorOpName(a.vectorOp), a.vectorClamp ? "_sat" : "",
                                     DestText(a.vectorDest, a.exportData, s.pixel).c_str(), mask.c_str(),
                                     OperandText(a.src[0], s.pixel).c_str(), OperandText(a.src[1], s.pixel).c_str(),
                                     OperandText(a.src[2], s.pixel).c_str());
                            out += line;
                        }
                        if (a.ScalarResultMask() || a.scalarOp != 50)
                        {
                            std::string ops;
                            if (a.scalarOperandCount >= 1) ops = ", " + OperandText(a.scalarSrc[0], s.pixel, 1);
                            if (a.scalarOperandCount == 2) ops += ", " + OperandText(a.scalarSrc[1], s.pixel, 1);
                            snprintf(line, sizeof(line), "      %3u  %s+ %s%s %s.%s%s\n", c.address + k, pred.c_str(),
                                     ScalarOpName(a.scalarOp), a.scalarClamp ? "_sat" : "",
                                     DestText(a.exportData ? a.vectorDest : a.scalarDest, a.exportData, s.pixel).c_str(),
                                     MaskText(a.ScalarResultMask()).c_str(), ops.c_str());
                            out += line;
                        }
                    }
                }
            }
            else
            {
                if (c.op == CfOp::Alloc)
                    snprintf(line, sizeof(line), " %s size %u\n", c.allocType == 1 ? "position" : c.allocType == 2 ? "interpolators/colors" : "memory", c.allocSize);
                else if (c.op != CfOp::Nop && c.op != CfOp::Return)
                    snprintf(line, sizeof(line), " target %u%s\n", c.address, c.op == CfOp::LoopStart || c.op == CfOp::LoopEnd ? " (loop)" : "");
                else
                    snprintf(line, sizeof(line), "\n");
                out += line;
            }
        }
        return out;
    }
}
