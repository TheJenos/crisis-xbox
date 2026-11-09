// Xenos shader microcode -> WGSL.
//
// Registers, the predicate, the previous scalar result and the address
// registers become private variables; every ALU/fetch instruction becomes a
// small block of WGSL. Linear control flow (exec / bool-conditioned exec /
// predicated exec) is emitted as straight code with `if`s; shaders with jumps,
// calls or loops are emitted as a `loop { switch (pc) { ... } }` state machine
// over the control-flow instruction index.
#include "xenos_shader.h"

#include <cstdio>
#include <set>
#include <algorithm>
#include <cmath>
#include <cstdarg>

namespace xenos
{
    namespace
    {
        const char kXyzw[] = "xyzw";

        std::string Fmt(const char* f, ...)
        {
            char buf[512];
            va_list ap;
            va_start(ap, f);
            vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }

        std::string FloatLit(float v)
        {
            std::string s = Fmt("%.9g", v);
            if (s.find_first_of(".eE") == std::string::npos && s.find("inf") == std::string::npos)
                s += ".0";
            return s;
        }

        struct Generator
        {
            const DecodedShader& s;
            Translation& t;
            std::string out;
            int depth = 1;
            bool pixel;
            uint32_t regCount = 1;
            bool relativeTemps = false;
            std::set<uint32_t> textureSlots;
            bool usesVertexData = false;

            Generator(const DecodedShader& shader, Translation& tr) : s(shader), t(tr), pixel(shader.pixel) {}

            void Line(const std::string& l)
            {
                out.append(size_t(depth) * 4, ' ');
                out += l;
                out += '\n';
            }

            void Fail(const std::string& why)
            {
                if (t.error.empty())
                    t.error = why;
            }

            // ---------------------------------------------------------------
            // Operands
            // ---------------------------------------------------------------
            std::string TempRef(uint32_t index, bool loopRelative)
            {
                if (!loopRelative)
                    return Fmt("r[%u]", index);
                return Fmt("r[clamp(%d + aL, 0, %u)]", int(index), regCount - 1);
            }

            std::string OperandBase(const Operand& o)
            {
                if (o.temp)
                    return TempRef(o.index, o.addressing == Addressing::Loop);
                uint32_t base = pixel ? 256 : 0;
                uint32_t idx = o.index + base;
                switch (o.addressing)
                {
                case Addressing::A0:
                    return Fmt("C.c[clamp(%u + a0, %u, %u)]", idx, base, base + 255);
                case Addressing::Loop:
                    return Fmt("C.c[clamp(%u + aL, %u, %u)]", idx, base, base + 255);
                default:
                    return Fmt("C.c[%u]", idx);
                }
            }

            std::string Modifiers(const Operand& o, std::string e)
            {
                if (o.abs)
                    e = "abs(" + e + ")";
                if (o.negate)
                    e = "-(" + e + ")";
                return e;
            }

            std::string Vec(const Operand& o)
            {
                std::string e = OperandBase(o);
                if (!(o.swizzle[0] == 0 && o.swizzle[1] == 1 && o.swizzle[2] == 2 && o.swizzle[3] == 3))
                {
                    e += '.';
                    for (int i = 0; i < 4; i++)
                        e += kXyzw[o.swizzle[i]];
                }
                return Modifiers(o, e);
            }

            std::string Scal(const Operand& o)
            {
                return Modifiers(o, OperandBase(o) + '.' + kXyzw[o.swizzle[0]]);
            }

            std::string BoolConst(uint32_t b)
            {
                return Fmt("((M.bools[%u][%u] >> %uu) & 1u) != 0u", b / 128, (b / 32) % 4, b % 32);
            }

            // ---------------------------------------------------------------
            // ALU
            // ---------------------------------------------------------------
            static int VectorOperandCount(uint32_t op)
            {
                switch (op)
                {
                case 8: case 9: case 10: case 19:
                    return 1;
                case 11: case 12: case 13: case 14: case 17:
                    return 3;
                default:
                    return 2;
                }
            }

            void Probe(uint32_t slot)
            {
                if (pixel && g_probe.active && slot == g_probe.instr)
                    Line(Fmt("oc[0] = vec4f(r[%u].%s * %s); return; // probe", g_probe.reg, g_probe.swizzle, FloatLit(g_probe.scale).c_str()));
            }

            std::string ExportTarget(uint32_t reg, int& components)
            {
                components = 4;
                if (reg >= 32 && reg <= 37)
                {
                    Fail("memory export");
                    return "";
                }
                if (!pixel)
                {
                    if (reg == 62)
                        return "opos";
                    if (reg == 63)
                    {
                        t.writesPointSize = true;
                        return "opts";
                    }
                    if (reg < kInterpolators)
                    {
                        t.interpolatorMask |= 1u << reg;
                        return Fmt("o[%u]", reg);
                    }
                }
                else
                {
                    if (reg < 4)
                    {
                        t.colorTargetMask |= 1u << reg;
                        return Fmt("oc[%u]", reg);
                    }
                    if (reg == 61)
                    {
                        t.writesDepth = true;
                        return "odepth";
                    }
                }
                Fail(Fmt("export to register %u", reg));
                return "";
            }

            void EmitAlu(const AluInstr& a, uint32_t slot)
            {
                Probe(slot);
                Line(Fmt("// %u: %s / %s", slot, VectorOpName(a.vectorOp), ScalarOpName(a.scalarOp)));
                if (a.predicated)
                {
                    Line(Fmt("if (p0 == %s) {", a.predCondition ? "true" : "false"));
                    depth++;
                }
                else
                {
                    Line("{");
                    depth++;
                }

                uint32_t vmask = a.VectorResultMask();
                uint32_t smask = a.ScalarResultMask();
                uint32_t op = a.vectorOp;
                bool vectorSideEffects = (op >= 20 && op <= 27) || op == 29;
                bool doVector = vmask || vectorSideEffects;
                bool doScalar = a.scalarOp != 50 || smask;

                // Read all sources before any write (vector and scalar are co-issued).
                if (doVector)
                {
                    int n = VectorOperandCount(op);
                    static const char* names[3] = { "va", "vb", "vc" };
                    for (int i = 0; i < n; i++)
                        Line(Fmt("let %s = %s;", names[i], Vec(a.src[i]).c_str()));
                }
                if (doScalar)
                {
                    if (a.scalarOperandCount >= 1)
                        Line("let sa = " + Scal(a.scalarSrc[0]) + ";");
                    if (a.scalarOperandCount >= 2)
                        Line("let sb = " + Scal(a.scalarSrc[1]) + ";");
                }

                if (doVector)
                {
                    std::string v;
                    switch (op)
                    {
                    case 0: v = "va + vb"; break;
                    case 1: v = "xe_mul4(va, vb)"; break;
                    case 2: v = "select(vb, va, va >= vb)"; break;
                    case 3: v = "select(vb, va, va < vb)"; break;
                    case 4: v = "select(vec4f(0.0), vec4f(1.0), va == vb)"; break;
                    case 5: v = "select(vec4f(0.0), vec4f(1.0), va > vb)"; break;
                    case 6: v = "select(vec4f(0.0), vec4f(1.0), va >= vb)"; break;
                    case 7: v = "select(vec4f(0.0), vec4f(1.0), va != vb)"; break;
                    case 8: v = "va - floor(va)"; break;
                    case 9: v = "trunc(va)"; break;
                    case 10: v = "floor(va)"; break;
                    case 11: v = "xe_mad4(va, vb, vc)"; break;
                    case 12: v = "select(vc, vb, va == vec4f(0.0))"; break;
                    case 13: v = "select(vc, vb, va >= vec4f(0.0))"; break;
                    case 14: v = "select(vc, vb, va > vec4f(0.0))"; break;
                    case 15: v = "vec4f(xe_dot4(va, vb))"; break;
                    case 16: v = "vec4f(xe_dot4(vec4f(va.xyz, 0.0), vec4f(vb.xyz, 0.0)))"; break;
                    case 17: v = "vec4f(xe_mul1(va.x, vb.x) + xe_mul1(va.y, vb.y) + vc.x)"; break;
                    case 18: v = "xe_cube(va)"; break;
                    case 19: v = "vec4f(max(max(va.x, va.y), max(va.z, va.w)))"; break;
                    case 20: case 21: case 22: case 23:
                    {
                        static const char* cmp[4] = { "==", "!=", ">", ">=" };
                        const char* c = cmp[op - 20];
                        Line(Fmt("let np0 = va.w == 0.0 && vb.w %s 0.0;", c));
                        v = Fmt("select(vec4f(va.x + 1.0), vec4f(0.0), va.x == 0.0 && vb.x %s 0.0)", c);
                        break;
                    }
                    case 24: case 25: case 26: case 27:
                    {
                        static const char* cmp[4] = { "==", ">", ">=", "!=" };
                        Line(Fmt("let vkill = any(va %s vb);", cmp[op - 24]));
                        v = "select(vec4f(0.0), vec4f(1.0), vkill)";
                        t.usesKill = true;
                        break;
                    }
                    case 28: v = "vec4f(1.0, va.y * vb.y, va.z, vb.w)"; break;
                    case 29:
                        Line("let na0 = i32(clamp(floor(va.w + 0.5), -256.0, 255.0));");
                        v = "select(vb, va, va >= vb)";
                        break;
                    default:
                        Fail(Fmt("vector opcode %u", op));
                        v = "vec4f(0.0)";
                    }
                    if (a.vectorClamp)
                        v = "saturate(" + v + ")";
                    Line("let v = " + v + ";");
                }

                if (doScalar)
                {
                    std::string r;
                    bool setsP0 = false, kills = false, setsA0 = false;
                    switch (a.scalarOp)
                    {
                    case 0: r = "sa + sb"; break;
                    case 1: r = "sa + ps"; break;
                    case 2: r = "xe_mul1(sa, sb)"; break;
                    case 3: r = "xe_mul1(sa, ps)"; break;
                    case 4: r = "select(sa * ps, -FLT_MAX, ps == -FLT_MAX || !(abs(ps) <= FLT_MAX) || !(abs(sb) <= FLT_MAX) || sb <= 0.0)"; break;
                    case 5: r = "select(sb, sa, sa >= sb)"; break;
                    case 6: r = "select(sb, sa, sa < sb)"; break;
                    case 7: r = "select(0.0, 1.0, sa == 0.0)"; break;
                    case 8: r = "select(0.0, 1.0, sa > 0.0)"; break;
                    case 9: r = "select(0.0, 1.0, sa >= 0.0)"; break;
                    case 10: r = "select(0.0, 1.0, sa != 0.0)"; break;
                    case 11: r = "sa - floor(sa)"; break;
                    case 12: r = "trunc(sa)"; break;
                    case 13: r = "floor(sa)"; break;
                    case 14: r = "exp2(sa)"; break;
                    case 15: r = "max(select(log2(abs(sa)), 0.0, sa == 1.0), -FLT_MAX)"; break;
                    case 16: r = "select(log2(abs(sa)), 0.0, sa == 1.0)"; break;
                    case 17: r = "select(clamp(1.0 / sa, -FLT_MAX, FLT_MAX), 1.0, sa == 1.0)"; break;
                    case 18: r = "xe_ff(select(1.0 / sa, 1.0, sa == 1.0))"; break;
                    case 19: r = "select(1.0 / sa, 1.0, sa == 1.0)"; break;
                    case 20: r = "select(clamp(inverseSqrt(abs(sa)), -FLT_MAX, FLT_MAX), 1.0, sa == 1.0)"; break;
                    case 21: r = "xe_ff(select(inverseSqrt(abs(sa)), 1.0, sa == 1.0))"; break;
                    case 22: r = "select(inverseSqrt(abs(sa)), 1.0, sa == 1.0)"; break;
                    case 23:
                        Line("let sa0 = i32(clamp(floor(sa + 0.5), -256.0, 255.0));");
                        setsA0 = true;
                        r = "select(sb, sa, sa >= sb)";
                        break;
                    case 24:
                        Line("let sa0 = i32(clamp(floor(sa), -256.0, 255.0));");
                        setsA0 = true;
                        r = "select(sb, sa, sa >= sb)";
                        break;
                    case 25: r = "sa - sb"; break;
                    case 26: r = "sa - ps"; break;
                    case 27: Line("let sp0 = sa == 0.0;"); setsP0 = true; r = "select(1.0, 0.0, sp0)"; break;
                    case 28: Line("let sp0 = sa != 0.0;"); setsP0 = true; r = "select(1.0, 0.0, sp0)"; break;
                    case 29: Line("let sp0 = sa > 0.0;"); setsP0 = true; r = "select(1.0, 0.0, sp0)"; break;
                    case 30: Line("let sp0 = sa >= 0.0;"); setsP0 = true; r = "select(1.0, 0.0, sp0)"; break;
                    case 31: Line("let sp0 = sa == 1.0;"); setsP0 = true; r = "select(select(sa, 1.0, sa == 0.0), 0.0, sp0)"; break;
                    case 32: Line("let sp0 = sa - 1.0 <= 0.0;"); setsP0 = true; r = "select(sa - 1.0, 0.0, sp0)"; break;
                    case 33: Line("let sp0 = false;"); setsP0 = true; r = "FLT_MAX"; break;
                    case 34: Line("let sp0 = sa == 0.0;"); setsP0 = true; r = "select(sa, 0.0, sp0)"; break;
                    case 35: Line("let skill = sa == 0.0;"); kills = true; break;
                    case 36: Line("let skill = sa > 0.0;"); kills = true; break;
                    case 37: Line("let skill = sa >= 0.0;"); kills = true; break;
                    case 38: Line("let skill = sa != 0.0;"); kills = true; break;
                    case 39: Line("let skill = sa == 1.0;"); kills = true; break;
                    case 40: r = "sqrt(sa)"; break;
                    case 42: case 43: r = "xe_mul1(sa, sb)"; break;
                    case 44: case 45: r = "sa + sb"; break;
                    case 46: case 47: r = "sa - sb"; break;
                    case 48: r = "sin(sa)"; break;
                    case 49: r = "cos(sa)"; break;
                    case 50: r = "ps"; break;
                    default:
                        Fail(Fmt("scalar opcode %u", a.scalarOp));
                        r = "0.0";
                    }
                    if (kills)
                    {
                        r = "select(0.0, 1.0, skill)";
                        t.usesKill = true;
                    }
                    if (a.scalarClamp)
                        r = "saturate(" + r + ")";
                    Line("let s = " + r + ";");
                    if (setsP0)
                        Line("p0 = sp0;");
                    if (setsA0)
                        Line("a0 = sa0;");
                    if (kills && pixel)
                        Line("if (skill) { discard; }");
                }

                if (doVector)
                {
                    if (op >= 20 && op <= 23)
                        Line("p0 = np0;");
                    if (op == 29)
                        Line("a0 = na0;");
                    if (op >= 24 && op <= 27 && pixel)
                        Line("if (vkill) { discard; }");
                }

                // Writes.
                if (a.exportData)
                {
                    int comps;
                    std::string target = ExportTarget(a.vectorDest, comps);
                    uint32_t c0 = a.Constant0Mask(), c1 = a.Constant1Mask();
                    if (!target.empty())
                        for (int i = 0; i < 4; i++)
                        {
                            std::string val;
                            if ((vmask >> i) & 1) val = std::string("v.") + kXyzw[i];
                            else if ((smask >> i) & 1) val = "s";
                            else if ((c0 >> i) & 1) val = "0.0";
                            else if ((c1 >> i) & 1) val = "1.0";
                            else continue;
                            Line(Fmt("%s.%c = %s;", target.c_str(), kXyzw[i], val.c_str()));
                        }
                }
                else
                {
                    if (vmask)
                    {
                        std::string d = TempRef(a.vectorDest, a.vectorDestRelative);
                        if (vmask == 0xF)
                            Line(d + " = v;");
                        else
                            for (int i = 0; i < 4; i++)
                                if ((vmask >> i) & 1)
                                    Line(Fmt("%s.%c = v.%c;", d.c_str(), kXyzw[i], kXyzw[i]));
                    }
                    if (smask)
                    {
                        std::string d = TempRef(a.scalarDest, a.scalarDestRelative);
                        if (smask == 0xF)
                            Line(d + " = vec4f(s);");
                        else
                            for (int i = 0; i < 4; i++)
                                if ((smask >> i) & 1)
                                    Line(Fmt("%s.%c = s;", d.c_str(), kXyzw[i]));
                    }
                }
                if (doScalar && a.scalarOp != 50)
                    Line("ps = s;");

                depth--;
                Line("}");
            }

            // ---------------------------------------------------------------
            // Fetch
            // ---------------------------------------------------------------
            uint32_t lastFetchConst = 0, lastFetchStride = 0;

            // Expression decoding `format` at dword address `addr` into a vec4f.
            std::string VertexDecode(const FetchInstr& f, const char* addr)
            {
                bool sg = f.signedFormat, norm = f.normalized;
                auto word = [&](int i) { return i ? Fmt("V[%s + %du]", addr, i) : Fmt("V[%s]", addr); };
                // Packed integer components: (bit offset, width) per component.
                auto packed = [&](std::initializer_list<std::pair<int, int>> fields, int words = 1) {
                    std::string comps[4] = { "0.0", "0.0", "0.0", "1.0" };
                    int i = 0;
                    for (auto [off, width] : fields)
                    {
                        int w = off / 32, o = off % 32;
                        std::string raw = sg ? Fmt("f32(extractBits(bitcast<i32>(%s), %du, %du))", word(w).c_str(), o, width)
                                             : Fmt("f32(extractBits(%s, %du, %du))", word(w).c_str(), o, width);
                        if (norm)
                        {
                            double maxv = sg ? double((1u << (width - 1)) - 1) : double((1ull << width) - 1);
                            raw = sg ? Fmt("max(%s / %s, -1.0)", raw.c_str(), FloatLit(float(maxv)).c_str())
                                     : Fmt("%s / %s", raw.c_str(), FloatLit(float(maxv)).c_str());
                        }
                        comps[i++] = raw;
                    }
                    (void)words;
                    return Fmt("vec4f(%s, %s, %s, %s)", comps[0].c_str(), comps[1].c_str(), comps[2].c_str(), comps[3].c_str());
                };
                auto int32s = [&](int n) {
                    std::string comps[4] = { "0.0", "0.0", "0.0", "1.0" };
                    for (int i = 0; i < n; i++)
                    {
                        std::string raw = sg ? "f32(bitcast<i32>(" + word(i) + "))" : "f32(" + word(i) + ")";
                        if (norm)
                            raw = sg ? "max(" + raw + " / 2147483647.0, -1.0)" : raw + " / 4294967295.0";
                        comps[i] = raw;
                    }
                    return Fmt("vec4f(%s, %s, %s, %s)", comps[0].c_str(), comps[1].c_str(), comps[2].c_str(), comps[3].c_str());
                };
                auto floats = [&](int n) {
                    std::string comps[4] = { "0.0", "0.0", "0.0", "1.0" };
                    for (int i = 0; i < n; i++)
                        comps[i] = "bitcast<f32>(" + word(i) + ")";
                    return Fmt("vec4f(%s, %s, %s, %s)", comps[0].c_str(), comps[1].c_str(), comps[2].c_str(), comps[3].c_str());
                };
                switch (f.format)
                {
                case 6: return packed({ { 0, 8 }, { 8, 8 }, { 16, 8 }, { 24, 8 } });
                case 7: return packed({ { 0, 10 }, { 10, 10 }, { 20, 10 }, { 30, 2 } });
                case 16: return packed({ { 0, 11 }, { 11, 11 }, { 22, 10 } });
                case 17: return packed({ { 0, 10 }, { 10, 11 }, { 21, 11 } });
                case 25: return packed({ { 0, 16 }, { 16, 16 } });
                case 26: return packed({ { 0, 16 }, { 16, 16 }, { 32, 16 }, { 48, 16 } }, 2);
                case 31: return Fmt("vec4f(unpack2x16float(%s), 0.0, 1.0)", word(0).c_str());
                case 32: return Fmt("vec4f(unpack2x16float(%s), unpack2x16float(%s))", word(0).c_str(), word(1).c_str());
                case 33: return int32s(1);
                case 34: return int32s(2);
                case 35: return int32s(4);
                case 36: return floats(1);
                case 37: return floats(2);
                case 38: return floats(4);
                case 57: return floats(3);
                default:
                    Fail(Fmt("vertex format %u", f.format));
                    return "vec4f(0.0, 0.0, 0.0, 1.0)";
                }
            }

            void WriteFetchResult(const FetchInstr& f, const char* value)
            {
                std::string d = TempRef(f.dstReg, f.dstRelative);
                for (int i = 0; i < 4; i++)
                {
                    uint8_t sw = f.dstSwizzle[i];
                    if (sw == 7)
                        continue;
                    std::string v = sw < 4 ? Fmt("%s.%c", value, kXyzw[sw]) : sw == 4 ? "0.0" : sw == 5 ? "1.0" : "0.0";
                    Line(Fmt("%s.%c = %s;", d.c_str(), kXyzw[i], v.c_str()));
                }
            }

            void EmitVertexFetch(const FetchInstr& f, uint32_t slot)
            {
                if (pixel)
                {
                    Fail("vertex fetch in a pixel shader");
                    return;
                }
                usesVertexData = true;
                uint32_t k = f.constIndex, stride = f.stride;
                if (f.mini)
                {
                    k = lastFetchConst;
                    stride = lastFetchStride;
                }
                else
                {
                    lastFetchConst = k;
                    lastFetchStride = stride;
                    t.vertexFetchMask[k / 32] |= 1u << (k % 32);
                }
                t.vertexFetches.push_back({ k, f.format, stride, f.offset, f.signedFormat, f.normalized });
                Probe(slot);
                Line(Fmt("// %u: vfetch%s vf%u fmt %u", slot, f.mini ? "_mini" : "", k, f.format));
                Line(f.predicated ? Fmt("if (p0 == %s) {", f.predCondition ? "true" : "false") : "{");
                depth++;
                if (!f.mini)
                    Line(Fmt("vfi = i32(floor(%s.%c%s));", TempRef(f.srcReg, f.srcRelative).c_str(), kXyzw[f.srcSwizzle[0]],
                             f.roundIndex ? " + 0.5" : ""));
                Line(Fmt("let addr = u32(i32(M.vbase[%u][%u]) + vfi * %d + %d);", k / 4, k % 4, int(stride), f.offset));
                std::string val = VertexDecode(f, "addr");
                if (f.expAdjust && f.format != 31 && f.format != 32 && !(f.format >= 36 && f.format <= 38) && f.format != 57)
                    val = Fmt("(%s * %s)", val.c_str(), FloatLit(std::ldexp(1.0f, f.expAdjust)).c_str());
                Line("let f = " + val + ";");
                WriteFetchResult(f, "f");
                depth--;
                Line("}");
            }

            void EmitTextureFetch(const FetchInstr& f, uint32_t slot)
            {
                uint32_t k = f.constIndex;
                std::string src = TempRef(f.srcReg, f.srcRelative);
                Probe(slot);
                Line(Fmt("// %u: texture op %u tf%u", slot, f.op, k));
                Line(f.predicated ? Fmt("if (p0 == %s) {", f.predCondition ? "true" : "false") : "{");
                depth++;
                switch (f.op)
                {
                case 1: // TEXTURE_FETCH
                {
                    uint32_t dim = f.dimension;
                    if (t.textureMask & (1u << k))
                    {
                        if (t.textureDimension[k] != dim)
                            Fail("texture fetch constant used with two dimensions");
                    }
                    t.textureMask |= 1u << k;
                    t.textureDimension[k] = uint8_t(dim);
                    // Filters the instruction sets instead of the fetch constant's.
                    const uint8_t want[3] = { f.magFilter, f.minFilter, f.mipFilter };
                    for (int i = 0; i < 3; i++)
                        if (want[i] != 3 && (t.filterOverride[k][i] == 3 || want[i] == 0))
                            t.filterOverride[k][i] = want[i];
                    textureSlots.insert(k);
                    std::string c = Fmt("%s.%c%c%c", src.c_str(), kXyzw[f.srcSwizzle[0]], kXyzw[f.srcSwizzle[1]], kXyzw[f.srcSwizzle[2]]);
                    Line("let tc = " + c + ";");
                    std::string coord;
                    if (dim == 0 || dim == 1)
                    {
                        coord = dim == 0 ? "vec2f(tc.x, 0.5)" : "tc.xy";
                        if (f.unnormalizedCoords)
                            coord = Fmt("(%s / vec2f(textureDimensions(tex%u)))", coord.c_str(), k);
                        else // fetch size / host size (resolved textures can be larger)
                            coord = Fmt("(%s * M.tcs[%u].%s)", coord.c_str(), k / 2, (k & 1) ? "zw" : "xy");
                    }
                    else if (dim == 2)
                    {
                        coord = "tc";
                        if (f.unnormalizedCoords)
                            coord = Fmt("(tc / vec3f(textureDimensions(tex%u)))", k);
                    }
                    else
                        coord = "xe_cube_dir(tc)";
                    std::string sample;
                    if (f.useRegisterLod)
                        sample = Fmt("textureSampleLevel(tex%u, smp%u, %s, tlod)", k, k, coord.c_str());
                    else if (!pixel)
                        sample = Fmt("textureSampleLevel(tex%u, smp%u, %s, 0.0)", k, k, coord.c_str());
                    else if (f.lodBias != 0.0f)
                        sample = Fmt("textureSampleBias(tex%u, smp%u, %s, %s)", k, k, coord.c_str(), FloatLit(f.lodBias).c_str());
                    else
                        sample = Fmt("textureSample(tex%u, smp%u, %s)", k, k, coord.c_str());
                    // Per-slot component swizzle from the recorder (resolved textures).
                    Line(Fmt("let f = xe_swz(%s, M.swz[%u][%u]) * M.tscale[%u];", sample.c_str(), k / 4, k % 4, k));
                    WriteFetchResult(f, "f");
                    break;
                }
                case 24: // SET_TEXTURE_LOD
                    Line(Fmt("tlod = %s.%c;", src.c_str(), kXyzw[f.srcSwizzle[0]]));
                    break;
                case 25: case 26: // gradients: ignored (implicit derivatives are used)
                    break;
                case 16: case 17: case 18: case 19: // border fraction, computed LOD, gradients, weights
                    Line("let f = vec4f(0.0);");
                    WriteFetchResult(f, "f");
                    break;
                default:
                    Fail(Fmt("fetch opcode %u", f.op));
                }
                depth--;
                Line("}");
            }

            void EmitExecBody(const ControlFlow& c)
            {
                for (uint32_t k = 0; k < c.count; k++)
                {
                    uint32_t slot = c.address + k;
                    const Instr& in = s.slots[slot];
                    if (!in.fetch)
                        EmitAlu(in.alu, slot);
                    else if (in.fetchInstr.op == 0)
                        EmitVertexFetch(in.fetchInstr, slot);
                    else
                        EmitTextureFetch(in.fetchInstr, slot);
                }
            }

            std::string ExecCondition(const ControlFlow& c)
            {
                if (c.UsesBool())
                    return Fmt("(%s) == %s", BoolConst(c.boolIndex).c_str(), c.condition ? "true" : "false");
                if (c.UsesPredicate())
                    return Fmt("p0 == %s", c.condition ? "true" : "false");
                return "";
            }

            // ---------------------------------------------------------------
            // Control flow
            // ---------------------------------------------------------------
            void EmitLinear()
            {
                for (const auto& c : s.cf)
                {
                    if (!c.IsExec())
                        continue;
                    std::string cond = ExecCondition(c);
                    if (!cond.empty())
                    {
                        Line("if (" + cond + ") {");
                        depth++;
                    }
                    EmitExecBody(c);
                    if (c.IsEnd())
                        Line("return;");
                    if (!cond.empty())
                    {
                        depth--;
                        Line("}");
                    }
                    if (c.op == CfOp::ExecEnd)
                        break;
                }
            }

            void EmitStateMachine()
            {
                t.usesStateMachine = true;
                t.controlFlow = s.cf;
                Line("var pc = 0u;");
                Line("var guard = 0u;");
                Line("var lcount: array<i32, 4>;");
                Line("var liter: array<i32, 4>;");
                Line("var lsaved: array<i32, 4>;");
                Line("var lstep: array<i32, 4>;");
                Line("var ld = -1;");
                Line("var cstack: array<u32, 4>;");
                Line("var cd = 0;");
                Line("loop {");
                depth++;
                Line("guard += 1u;");
                // The cap comes from a uniform (M.pgen.z), not a literal: with a
                // constant bound, the browser's D3D shader compiler tried to unroll
                // the whole loop, which took minutes and gigabytes in the GPU
                // process and froze the GPU (and the page) when such a shader
                // first appeared.
                Line("if (f32(guard) > M.pgen.z) { return; }");
                Line("switch (pc) {");
                depth++;
                for (uint32_t i = 0; i < s.cf.size(); i++)
                {
                    const ControlFlow& c = s.cf[i];
                    Line(Fmt("case %uu: {", i));
                    depth++;
                    uint32_t next = i + 1;
                    switch (c.op)
                    {
                    case CfOp::LoopStart:
                    {
                        uint32_t L = c.loopId;
                        Line(Fmt("let lc = M.loops[%u][%u];", L / 4, L % 4));
                        Line("ld = min(ld + 1, 3);");
                        Line("lcount[ld] = i32(lc & 0xFFu);");
                        Line("liter[ld] = 0;");
                        Line("lsaved[ld] = aL;");
                        Line("lstep[ld] = extractBits(bitcast<i32>(lc), 16u, 8u);");
                        Line("aL = i32((lc >> 8u) & 0xFFu);");
                        Line(Fmt("if (lcount[ld] <= 0) { aL = lsaved[ld]; ld -= 1; pc = %uu; } else { pc = %uu; }", c.address, next));
                        break;
                    }
                    case CfOp::LoopEnd:
                    {
                        Line("liter[ld] += 1;");
                        Line("aL += lstep[ld];");
                        std::string brk = c.predicated ? Fmt(" && p0 != %s", c.condition ? "true" : "false") : "";
                        Line(Fmt("if (liter[ld] < lcount[ld]%s) { pc = %uu; } else { aL = lsaved[max(ld, 0)]; ld = max(ld - 1, -1); pc = %uu; }",
                                 brk.c_str(), c.address, next));
                        break;
                    }
                    case CfOp::CondJmp:
                    case CfOp::CondCall:
                    {
                        std::string cond = c.unconditional ? "true"
                            : c.predicated ? Fmt("p0 == %s", c.condition ? "true" : "false")
                                           : Fmt("(%s) == %s", BoolConst(c.boolIndex).c_str(), c.condition ? "true" : "false");
                        if (c.op == CfOp::CondCall)
                            Line(Fmt("if (%s) { cstack[min(cd, 3)] = %uu; cd = min(cd + 1, 4); pc = %uu; } else { pc = %uu; }", cond.c_str(), next, c.address, next));
                        else
                            Line(Fmt("if (%s) { pc = %uu; } else { pc = %uu; }", cond.c_str(), c.address, next));
                        break;
                    }
                    case CfOp::Return:
                        Line(Fmt("if (cd > 0) { cd -= 1; pc = cstack[cd]; } else { pc = %uu; }", next));
                        break;
                    default:
                        if (c.IsExec())
                        {
                            std::string cond = ExecCondition(c);
                            if (!cond.empty())
                            {
                                Line("if (" + cond + ") {");
                                depth++;
                            }
                            EmitExecBody(c);
                            if (c.IsEnd())
                                Line("return;");
                            if (!cond.empty())
                            {
                                depth--;
                                Line("}");
                            }
                        }
                        Line(Fmt("pc = %uu;", next));
                    }
                    depth--;
                    Line("}");
                }
                Line("default: { return; }");
                depth--;
                Line("}");
                depth--;
                Line("}");
            }

            void ScanRegisters()
            {
                uint32_t maxReg = 0;
                auto use = [&](uint32_t r) { maxReg = std::max(maxReg, r); };
                for (const auto& c : s.cf)
                {
                    if (!c.IsExec())
                        continue;
                    for (uint32_t k = 0; k < c.count; k++)
                    {
                        const Instr& in = s.slots[c.address + k];
                        if (in.fetch)
                        {
                            use(in.fetchInstr.srcReg);
                            use(in.fetchInstr.dstReg);
                            relativeTemps |= in.fetchInstr.srcRelative || in.fetchInstr.dstRelative;
                        }
                        else
                        {
                            const AluInstr& a = in.alu;
                            for (const Operand& o : a.src)
                                if (o.temp)
                                {
                                    use(o.index);
                                    relativeTemps |= o.addressing == Addressing::Loop;
                                }
                            for (const Operand& o : a.scalarSrc)
                                if (o.temp)
                                    use(o.index);
                            if (!a.exportData)
                            {
                                use(a.vectorDest);
                                use(a.scalarDest);
                                relativeTemps |= a.vectorDestRelative || a.scalarDestRelative;
                            }
                        }
                    }
                }
                regCount = relativeTemps ? 64 : maxReg + 1;
                if (pixel)
                    regCount = std::max<uint32_t>(regCount, 1);
            }

            std::string Translate()
            {
                ScanRegisters();
                bool linear = true;
                for (const auto& c : s.cf)
                    if (c.op == CfOp::LoopStart || c.op == CfOp::LoopEnd || c.op == CfOp::CondCall || c.op == CfOp::CondJmp || c.op == CfOp::Return)
                        linear = false;

                // Body first: it determines which resources and outputs are used.
                std::string body;
                std::swap(out, body);
                depth = 1;
                Line("var vfi = 0;");
                if (linear)
                    EmitLinear();
                else
                    EmitStateMachine();
                std::swap(out, body);

                std::string m;
                m += "// Generated from Xenos microcode.\n";
                m += "diagnostic(off, derivative_uniformity);\n\n";
                m += "const FLT_MAX = 0x1.fffffep+127f;\n";
                m += "struct FloatConstants { c: array<vec4f, 512>, }\n";
                m += "struct Misc {\n    bools: array<vec4u, 2>,\n    loops: array<vec4u, 8>,\n    vbase: array<vec4u, 24>,\n"
                     "    pos_scale: vec4f,\n    pos_offset: vec4f,\n    alpha_ref: vec4f,\n    swz: array<vec4u, 8>,\n    tscale: array<vec4f, 32>,\n    pgen: vec4f,\n"
                     "    tcs: array<vec4f, 16>,\n}\n";
                m += "@group(0) @binding(0) var<uniform> C: FloatConstants;\n";
                m += "@group(0) @binding(1) var<uniform> M: Misc;\n";
                if (!pixel)
                    m += "@group(0) @binding(2) var<storage, read> V: array<u32>;\n";
                for (uint32_t k : textureSlots)
                {
                    const char* type = t.textureDimension[k] == 2 ? "texture_3d<f32>" : t.textureDimension[k] == 3 ? "texture_cube<f32>" : "texture_2d<f32>";
                    m += Fmt("@group(1) @binding(%u) var tex%u: %s;\n", k * 2, k, type);
                    m += Fmt("@group(1) @binding(%u) var smp%u: sampler;\n", k * 2 + 1, k);
                }
                m += "\n";
                m += Fmt("var<private> r: array<vec4f, %u>;\n", regCount);
                m += "var<private> p0: bool = false;\nvar<private> ps: f32 = 0.0;\nvar<private> a0: i32 = 0;\nvar<private> aL: i32 = 0;\n"
                     "var<private> tlod: f32 = 0.0;\n";
                if (!pixel)
                    m += Fmt("var<private> o: array<vec4f, %u>;\nvar<private> opos: vec4f;\nvar<private> opts: vec4f;\n", kInterpolators);
                else
                    m += "var<private> oc: array<vec4f, 4>;\nvar<private> odepth: vec4f;\n";
                m += "\n";
                m += "fn xe_ff(t: f32) -> f32 { return select(t, 0.0, !(abs(t) <= FLT_MAX)); }\n";
                // Shader Model 3 multiplication: 0 * anything (even inf or NaN) = 0.
                m += "fn xe_mul1(a: f32, b: f32) -> f32 { return select(a * b, 0.0, a == 0.0 || b == 0.0); }\n";
                m += "fn xe_mul4(a: vec4f, b: vec4f) -> vec4f { return select(a * b, vec4f(0.0), (a == vec4f(0.0)) | (b == vec4f(0.0))); }\n";
                // Not fma(): Xenos MAD rounds the product before the add, so
                // shaders computing the same sum in another order (multi-pass
                // vertex shaders for one mesh) agree bit for bit; a fused
                // multiply-add breaks that and the passes z-fight. (Positions
                // are @invariant, so this isn't re-fused there.)
                m += "fn xe_mad4(a: vec4f, b: vec4f, c: vec4f) -> vec4f { return select(a * b + c, c, (a == vec4f(0.0)) | (b == vec4f(0.0))); }\n";
                m += "fn xe_dot4(a: vec4f, b: vec4f) -> f32 { let p = xe_mul4(a, b); return (p.x + p.y) + (p.z + p.w); }\n";
                // Texture component swizzle: 3 bits per output, 0-3 = XYZW, 4 = 0, 5 = 1.
                // Selects, not v[c]: indexing a vector with a runtime value makes
                // D3D compilers (browsers on Windows) move it to slow local memory,
                // and this runs for every texture fetch.
                m += "fn xe_sel(v: vec4f, c: u32) -> f32 {\n"
                     "    return select(select(select(select(select(0.0, 1.0, c == 5u), v.w, c == 3u), v.z, c == 2u), v.y, c == 1u), v.x, c == 0u);\n}\n";
                // Texture sign per output channel, bits 12-19 of s (2 bits each):
                // 1 signed (8-bit two's complement), 2 unsigned biased (2x - 1),
                // 3 gamma (inverse of the Xenos piecewise-linear gamma ramp).
                m += "fn xe_degamma(g: f32) -> f32 {\n    let x = clamp(g, 0.0, 1.0);\n"
                     "    if (x < 0.25) { return x * 0.25; }\n    if (x < 0.375) { return (x - 0.125) * 0.5; }\n"
                     "    if (x < 0.75) { return x - 0.25; }\n    return (x - 0.5) * 2.0;\n}\n";
                m += "fn xe_sign(x: f32, m: u32) -> f32 {\n"
                     "    if (m == 1u) { let b = round(x * 255.0); return max(select(b, b - 256.0, b >= 128.0) / 127.0, -1.0); }\n"
                     "    if (m == 2u) { return x * 2.0 - 1.0; }\n"
                     "    if (m == 3u) { return xe_degamma(x); }\n"
                     "    return x;\n}\n";
                m += "fn xe_swz(v: vec4f, s: u32) -> vec4f {\n    if (s == 0x688u) { return v; }\n"
                     "    return vec4f(xe_sign(xe_sel(v, s & 7u), (s >> 12u) & 3u), xe_sign(xe_sel(v, (s >> 3u) & 7u), (s >> 14u) & 3u),\n"
                     "                 xe_sign(xe_sel(v, (s >> 6u) & 7u), (s >> 16u) & 3u), xe_sign(xe_sel(v, (s >> 9u) & 7u), (s >> 18u) & 3u));\n}\n";
                // CUBE: direction from src0 = (z, z, x, y) -> (x, y, z) = (A.z, A.w, A.x).
                m += "fn xe_cube(a: vec4f) -> vec4f {\n"
                     "    let d = vec3f(a.z, a.w, a.x);\n"
                     "    let ad = abs(d);\n"
                     "    if (ad.z >= ad.x && ad.z >= ad.y) {\n"
                     "        return vec4f(-d.y, select(d.x, -d.x, d.z < 0.0), 2.0 * d.z, select(4.0, 5.0, d.z < 0.0));\n"
                     "    } else if (ad.y >= ad.x) {\n"
                     "        return vec4f(select(d.z, -d.z, d.y < 0.0), d.x, 2.0 * d.y, select(2.0, 3.0, d.y < 0.0));\n"
                     "    }\n"
                     "    return vec4f(-d.y, select(-d.z, d.z, d.x < 0.0), 2.0 * d.x, select(0.0, 1.0, d.x < 0.0));\n"
                     "}\n";
                // tfetchCube coordinates: (S, T) in [1, 2] and the face index.
                m += "fn xe_cube_dir(c: vec3f) -> vec3f {\n"
                     "    let sc = c.x * 2.0 - 3.0;\n"
                     "    let tc = c.y * 2.0 - 3.0;\n"
                     "    switch (u32(clamp(c.z, 0.0, 5.0))) {\n"
                     "        case 0u: { return vec3f(1.0, -tc, -sc); }\n"
                     "        case 1u: { return vec3f(-1.0, -tc, sc); }\n"
                     "        case 2u: { return vec3f(sc, 1.0, tc); }\n"
                     "        case 3u: { return vec3f(sc, -1.0, -tc); }\n"
                     "        case 4u: { return vec3f(sc, -tc, 1.0); }\n"
                     "        default: { return vec3f(-sc, -tc, -1.0); }\n"
                     "    }\n"
                     "}\n\n";
                m += "fn run() {\n" + body + "}\n\n";

                if (!pixel)
                {
                    // @invariant: games draw the same mesh in several passes with
                    // different vertex shaders and depth test LEQUAL/EQUAL; the
                    // hardware gives identical positions, but separately compiled
                    // host shaders may fuse or reorder the math differently and
                    // z-fight (Crysis 2's skin showed the earlier pass in patches).
                    m += "struct VsOut {\n    @invariant @builtin(position) pos: vec4f,\n";
                    for (uint32_t i = 0; i < kInterpolators; i++)
                        m += Fmt("    @location(%u) i%u: vec4f,\n", i, i);
                    m += "}\n\n";
                    // One vertex: the shader run for vertex (index) vid, from clean state.
                    m += "fn vs_one(vid: u32) -> VsOut {\n";
                    m += Fmt("    r = array<vec4f, %u>();\n", regCount);
                    m += Fmt("    o = array<vec4f, %u>();\n", kInterpolators);
                    m += "    opos = vec4f(0.0);\n    opts = vec4f(0.0);\n    p0 = false;\n    a0 = 0;\n    aL = 0;\n";
                    m += "    r[0] = vec4f(f32(vid), 0.0, 0.0, 0.0);\n";
                    m += "    run();\n";
                    m += "    var out: VsOut;\n";
                    m += "    out.pos = vec4f(opos.xyz * M.pos_scale.xyz + M.pos_offset.xyz * opos.w, opos.w);\n";
                    for (uint32_t i = 0; i < kInterpolators; i++)
                        m += Fmt("    out.i%u = o[%u];\n", i, i);
                    m += "    _ = C.c[0];\n    _ = V[0];\n";
                    m += "    return out;\n}\n\n";
                    m += "@vertex fn main(@builtin(vertex_index) vid: u32) -> VsOut {\n    return vs_one(vid);\n}\n\n";
                    // Rectangle lists: each rectangle is three corners; the fourth is
                    // the corner opposite the right angle, P + Q - R, for every output.
                    // Drawn as 6 vertices per rectangle; the corner indices are in V
                    // starting at the dword offset in M.alpha_ref.z (unused by vertex shaders).
                    m += "fn vs_fourth(p: VsOut, q: VsOut, rr: VsOut) -> VsOut {\n    var f: VsOut;\n    f.pos = p.pos + q.pos - rr.pos;\n";
                    for (uint32_t i = 0; i < kInterpolators; i++)
                        m += Fmt("    f.i%u = p.i%u + q.i%u - rr.i%u;\n", i, i, i, i);
                    m += "    return f;\n}\n\n";
                    m += "@vertex fn main_rect(@builtin(vertex_index) v: u32) -> VsOut {\n"
                         "    let base = bitcast<u32>(M.alpha_ref.z) + (v / 6u) * 3u;\n"
                         "    let k = v % 6u;\n"
                         "    let v0 = vs_one(V[base]);\n"
                         "    let v1 = vs_one(V[base + 1u]);\n"
                         "    let v2 = vs_one(V[base + 2u]);\n"
                         "    let q0 = v0.pos.xy / v0.pos.w;\n"
                         "    let q1 = v1.pos.xy / v1.pos.w;\n"
                         "    let q2 = v2.pos.xy / v2.pos.w;\n"
                         "    let d01 = dot(q1 - q0, q1 - q0);\n"
                         "    let d02 = dot(q2 - q0, q2 - q0);\n"
                         "    let d12 = dot(q2 - q1, q2 - q1);\n"
                         "    var R = v0;\n    var P = v1;\n    var Q = v2;\n"
                         "    if (d02 > d01 && d02 > d12) { R = v1; P = v2; Q = v0; }\n"
                         "    else if (d01 > d02 && d01 > d12) { R = v2; P = v0; Q = v1; }\n"
                         "    switch (k) {\n"
                         "        case 0u: { return R; }\n"
                         "        case 1u: { return P; }\n"
                         "        case 2u: { return Q; }\n"
                         "        case 3u: { return vs_fourth(P, Q, R); }\n"
                         "        case 4u: { return Q; }\n"
                         "        default: { return P; }\n"
                         "    }\n"
                         "}\n";
                }
                else
                {
                    m += "struct PsIn {\n    @invariant @builtin(position) frag_coord: vec4f,\n";
                    for (uint32_t i = 0; i < kInterpolators; i++)
                        m += Fmt("    @location(%u) i%u: vec4f,\n", i, i);
                    m += "}\n\n";
                    bool hasOut = t.colorTargetMask || t.writesDepth;
                    if (hasOut)
                    {
                        m += "struct PsOut {\n";
                        for (uint32_t i = 0; i < 4; i++)
                            if (t.colorTargetMask & (1u << i))
                                m += Fmt("    @location(%u) c%u: vec4f,\n", i, i);
                        if (t.writesDepth)
                            m += "    @builtin(frag_depth) depth: f32,\n";
                        m += "}\n\n";
                    }
                    m += std::string("@fragment fn main(input: PsIn)") + (hasOut ? " -> PsOut" : "") + " {\n";
                    for (uint32_t i = 0; i < std::min(regCount, kInterpolators); i++)
                        m += Fmt("    r[%u] = input.i%u;\n", i, i);
                    m += "    odepth = vec4f(input.frag_coord.z);\n";
                    // Pixel position (param gen): XY = position (D3D centers at integers),
                    // reported as front facing (positive X; facing would need another inter-stage slot).
                    m += Fmt("    if (M.pgen.x >= 0.0) {\n"
                             "        let pos = input.frag_coord.xy - vec2f(M.pgen.y);\n"
                             "        r[min(u32(M.pgen.x), %uu)] = vec4f(pos, 0.0, 0.0);\n"
                             "    }\n", regCount - 1);
                    m += "    run();\n";
                    m += "    _ = C.c[0];\n";
                    // Alpha test (fixed function on Xenos): alpha_ref = (reference, compare function; 7 = always).
                    if (t.colorTargetMask & 1)
                        m += "    let af = u32(M.alpha_ref.y);\n"
                             "    if (af != 7u) {\n"
                             "        let a = oc[0].w;\n"
                             "        let ar = M.alpha_ref.x;\n"
                             "        let ok = (af == 1u && a < ar) || (af == 2u && a == ar) || (af == 3u && a <= ar) ||\n"
                             "                   (af == 4u && a > ar) || (af == 5u && a != ar) || (af == 6u && a >= ar);\n"
                             "        if (!ok) { discard; }\n"
                             "    }\n"
                             // Alpha to mask without MSAA: an ordered 2x2 dither of coverage.
                             "    if (M.alpha_ref.w != 0.0) {\n"
                             "        let q = vec2u(input.frag_coord.xy) & vec2u(1u);\n"
                             "        let thresholds = array<f32, 4>(0.125, 0.625, 0.875, 0.375);\n"
                             "        if (oc[0].w < thresholds[q.y * 2u + q.x]) { discard; }\n"
                             "    }\n";
                    if (hasOut)
                    {
                        m += "    var out: PsOut;\n";
                        for (uint32_t i = 0; i < 4; i++)
                            if (t.colorTargetMask & (1u << i))
                                m += Fmt("    out.c%u = oc[%u];\n", i, i);
                        if (t.writesDepth)
                            m += "    out.depth = odepth.x;\n";
                        m += "    return out;\n";
                    }
                    m += "}\n";
                }
                // Keep the Misc block in every stage's layout.
                size_t mainPos = m.rfind("    run();\n");
                m.insert(mainPos, "    _ = M.bools[0];\n");
                return m;
            }
        };
    }

    ShaderProbe g_probe;

    Translation TranslateToWgsl(const DecodedShader& shader)
    {
        Translation t;
        if (!shader.error.empty())
        {
            t.error = "decode: " + shader.error;
            return t;
        }
        Generator g(shader, t);
        t.wgsl = g.Translate();
        return t;
    }
}
