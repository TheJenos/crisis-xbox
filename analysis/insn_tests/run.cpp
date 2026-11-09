// Runs the C++ that XenonRecomp generated for each test instruction (generated.inc)
// and checks results against values worked out by hand from the PowerPC spec.
#define PPC_CONFIG_H_INCLUDED
#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x10000ull
#define PPC_CODE_BASE 0x82000000ull
#define PPC_CODE_SIZE 0x10000ull
#include <ppc_context.h>
#include <cstdio>
#include <cmath>
#include <limits>
#include <initializer_list>

#include "generated.inc"

static int g_failures = 0, g_checks = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

alignas(64) static uint8_t g_mem[0x10000];
static uint8_t* const base = g_mem;

static void put16(uint32_t a, uint16_t v) { base[a] = v >> 8; base[a + 1] = uint8_t(v); }
static void put32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) base[a + i] = uint8_t(v >> (24 - 8 * i)); }
static void put64(uint32_t a, uint64_t v) { for (int i = 0; i < 8; i++) base[a + i] = uint8_t(v >> (56 - 8 * i)); }
static uint16_t get16(uint32_t a) { return uint16_t(base[a] << 8 | base[a + 1]); }
static uint32_t get32(uint32_t a) { uint32_t v = 0; for (int i = 0; i < 4; i++) v = v << 8 | base[a + i]; return v; }
static uint64_t get64(uint32_t a) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | base[a + i]; return v; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

// Vector registers are stored with the whole 16 bytes reversed, so PowerPC
// element i lives at the mirrored host index.
static void setW(PPCVRegister& v, int i, uint32_t x) { v.u32[3 - i] = x; }
static uint32_t getW(const PPCVRegister& v, int i) { return v.u32[3 - i]; }
static void setH(PPCVRegister& v, int i, uint16_t x) { v.u16[7 - i] = x; }
static uint16_t getH(const PPCVRegister& v, int i) { return v.u16[7 - i]; }
static void setF(PPCVRegister& v, int i, float x) { v.f32[3 - i] = x; }
static float getF(const PPCVRegister& v, int i) { return v.f32[3 - i]; }
static uint32_t fbitsOf(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// Zero the context, then take the FP control state from the host as the runtime does
// (an all-zero MXCSR would unmask every floating-point exception).
static PPCContext fresh() { static PPCContext c{}; memset(&c, 0, sizeof(c)); c.fpscr.loadFromHost(); memset(g_mem, 0, sizeof(g_mem)); return c; }

static void test_loads_with_update()
{
    { auto c = fresh(); c.r4.u64 = 0x100; put16(0x106, 0xBEEF); __imp__t_lhzu(c, base);
      CHECK(c.r3.u64 == 0xBEEF && c.r4.u64 == 0x106, "lhzu r3=%llx r4=%llx", c.r3.u64, c.r4.u64); }
    { auto c = fresh(); c.r4.u64 = 0x102; put16(0x100, 0x8001); __imp__t_lhau(c, base);
      CHECK(c.r3.s64 == -32767 && c.r4.u64 == 0x100, "lhau r3=%lld r4=%llx", c.r3.s64, c.r4.u64); }
    { auto c = fresh(); c.r4.u64 = 0x100; c.r5.u64 = 0x20; put16(0x120, 0x1234); __imp__t_lhzux(c, base);
      CHECK(c.r3.u64 == 0x1234 && c.r4.u64 == 0x120, "lhzux r3=%llx r4=%llx", c.r3.u64, c.r4.u64); }
    { auto c = fresh(); c.r4.u64 = 0x100; c.r5.u64 = 0x20; base[0x120] = 0xAB; __imp__t_lbzux(c, base);
      CHECK(c.r3.u64 == 0xAB && c.r4.u64 == 0x120, "lbzux r3=%llx r4=%llx", c.r3.u64, c.r4.u64); }
    { auto c = fresh(); c.r4.u64 = 0x100; c.r5.u64 = 0x20; put32(0x120, 0xDEADBEEF); __imp__t_lwzux(c, base);
      CHECK(c.r3.u64 == 0xDEADBEEF && c.r4.u64 == 0x120, "lwzux r3=%llx r4=%llx", c.r3.u64, c.r4.u64); }
    { auto c = fresh(); c.r4.u64 = 0x100; c.r5.u64 = 0x20; put64(0x120, 0x0123456789ABCDEFull); __imp__t_ldux(c, base);
      CHECK(c.r3.u64 == 0x0123456789ABCDEFull && c.r4.u64 == 0x120, "ldux r3=%llx r4=%llx", c.r3.u64, c.r4.u64); }
    { auto c = fresh(); c.r4.u64 = 0x300; put32(0x308, fbits(1.5f)); __imp__t_lfsu(c, base);
      CHECK(c.f1.f64 == 1.5 && c.r4.u64 == 0x308, "lfsu f1=%g r4=%llx", c.f1.f64, c.r4.u64); }
    { auto c = fresh(); c.r4.u64 = 0x300; c.r5.u64 = 0xC; put32(0x30C, fbits(-2.25f)); __imp__t_lfsux(c, base);
      CHECK(c.f1.f64 == -2.25 && c.r4.u64 == 0x30C, "lfsux f1=%g r4=%llx", c.f1.f64, c.r4.u64); }
    { auto c = fresh(); c.r4.u64 = 0x300; put64(0x308, dbits(3.141592653589793)); __imp__t_lfdu(c, base);
      CHECK(c.f1.f64 == 3.141592653589793 && c.r4.u64 == 0x308, "lfdu f1=%g r4=%llx", c.f1.f64, c.r4.u64); }
    { auto c = fresh(); c.r4.u64 = 0x300; c.r5.u64 = 0x10; put64(0x310, dbits(2.5e100)); __imp__t_lfdux(c, base);
      CHECK(c.f1.f64 == 2.5e100 && c.r4.u64 == 0x310, "lfdux f1=%g r4=%llx", c.f1.f64, c.r4.u64); }
    { auto c = fresh(); c.r4.u64 = 0x500; c.r5.u64 = 0; base[0x500] = 0x12; base[0x501] = 0x34; __imp__t_lhbrx(c, base);
      CHECK(c.r3.u64 == 0x3412, "lhbrx r3=%llx", c.r3.u64); }
}

static void test_stores_with_update()
{
    { auto c = fresh(); c.r3.u64 = 0x12345678; c.r4.u64 = 0x200; __imp__t_sthu(c, base);
      CHECK(get16(0x204) == 0x5678 && c.r4.u64 == 0x204, "sthu mem=%x r4=%llx", get16(0x204), c.r4.u64); }
    { auto c = fresh(); c.r3.u64 = 0x12345678; c.r4.u64 = 0x200; c.r5.u64 = 0x10; __imp__t_sthux(c, base);
      CHECK(get16(0x210) == 0x5678 && c.r4.u64 == 0x210, "sthux mem=%x r4=%llx", get16(0x210), c.r4.u64); }
    { auto c = fresh(); c.r3.u64 = 0x12345678; c.r4.u64 = 0x200; c.r5.u64 = 0x10; __imp__t_stbux(c, base);
      CHECK(base[0x210] == 0x78 && base[0x211] == 0 && c.r4.u64 == 0x210, "stbux mem=%x r4=%llx", base[0x210], c.r4.u64); }
    { auto c = fresh(); c.r3.u64 = 0x1122334455667788ull; c.r4.u64 = 0x200; c.r5.u64 = 0x10; __imp__t_stdux(c, base);
      CHECK(get64(0x210) == 0x1122334455667788ull && c.r4.u64 == 0x210, "stdux mem=%llx r4=%llx", get64(0x210), c.r4.u64); }
    { auto c = fresh(); c.f1.f64 = 0.75; c.r4.u64 = 0x400; __imp__t_stfsu(c, base);
      CHECK(get32(0x408) == 0x3F400000 && c.r4.u64 == 0x408, "stfsu mem=%x r4=%llx", get32(0x408), c.r4.u64); }
    { auto c = fresh(); c.f1.f64 = -8.0; c.r4.u64 = 0x400; c.r5.u64 = 0x10; __imp__t_stfsux(c, base);
      CHECK(get32(0x410) == 0xC1000000 && c.r4.u64 == 0x410, "stfsux mem=%x r4=%llx", get32(0x410), c.r4.u64); }
    { auto c = fresh(); c.f1.f64 = 1.0; c.r4.u64 = 0x400; __imp__t_stfdu(c, base);
      CHECK(get64(0x408) == 0x3FF0000000000000ull && c.r4.u64 == 0x408, "stfdu mem=%llx r4=%llx", get64(0x408), c.r4.u64); }
}

static void test_integer()
{
    { auto c = fresh(); c.r4.u64 = 0xF0F0F0F0; c.r5.u64 = 0xFF00FF00; __imp__t_eqv(c, base);
      CHECK(c.r3.u32 == 0xF00FF00F, "eqv r3=%x", c.r3.u32); }
    { auto c = fresh(); c.r4.u64 = 0x12345678; c.r5.u64 = 0x12345678; __imp__t_eqv_rc(c, base);
      CHECK(c.r3.s32 == -1 && c.cr0.lt && !c.cr0.gt && !c.cr0.eq, "eqv. r3=%x cr0=%d%d%d", c.r3.u32, c.cr0.lt, c.cr0.gt, c.cr0.eq); }

    { auto c = fresh(); c.r4.u64 = 0xFFFFFFFF; c.r5.u64 = 2; __imp__t_addc(c, base);
      CHECK(c.r3.u32 == 1 && c.xer.ca == 1, "addc overflow r3=%x ca=%d", c.r3.u32, c.xer.ca); }
    { auto c = fresh(); c.r4.u64 = 1; c.r5.u64 = 2; c.xer.ca = 1; __imp__t_addc(c, base);
      CHECK(c.r3.u32 == 3 && c.xer.ca == 0, "addc r3=%x ca=%d", c.r3.u32, c.xer.ca); }

    { auto c = fresh(); c.r4.u64 = 5; c.xer.ca = 0; __imp__t_addme(c, base);
      CHECK(c.r3.u32 == 4 && c.xer.ca == 1, "addme 5+0-1 r3=%x ca=%d", c.r3.u32, c.xer.ca); }
    { auto c = fresh(); c.r4.u64 = 0; c.xer.ca = 0; __imp__t_addme(c, base);
      CHECK(c.r3.u32 == 0xFFFFFFFF && c.xer.ca == 0, "addme 0+0-1 r3=%x ca=%d", c.r3.u32, c.xer.ca); }
    { auto c = fresh(); c.r4.u64 = 0; c.xer.ca = 1; __imp__t_addme(c, base);
      CHECK(c.r3.u32 == 0 && c.xer.ca == 1, "addme 0+1-1 r3=%x ca=%d", c.r3.u32, c.xer.ca); }

    { auto c = fresh(); c.r4.u64 = 0; c.xer.ca = 1; __imp__t_subfze(c, base);
      CHECK(c.r3.u32 == 0 && c.xer.ca == 1, "subfze ~0+1 r3=%x ca=%d", c.r3.u32, c.xer.ca); }
    { auto c = fresh(); c.r4.u64 = 5; c.xer.ca = 0; __imp__t_subfze(c, base);
      CHECK(c.r3.u32 == 0xFFFFFFFA && c.xer.ca == 0, "subfze ~5 r3=%x ca=%d", c.r3.u32, c.xer.ca); }
    { auto c = fresh(); c.r4.u64 = 5; c.xer.ca = 1; __imp__t_subfze(c, base);
      CHECK(c.r3.u32 == 0xFFFFFFFB && c.xer.ca == 0, "subfze ~5+1 r3=%x ca=%d", c.r3.u32, c.xer.ca); }
    { auto c = fresh(); c.r4.u64 = 0; c.xer.ca = 1; __imp__t_subfze_rc(c, base);
      CHECK(c.r3.u32 == 0 && c.cr0.eq && !c.cr0.lt && !c.cr0.gt, "subfze. r3=%x eq=%d", c.r3.u32, c.cr0.eq); }

    { auto c = fresh(); c.r4.s64 = 0x7FFFFFFFFFFFFFFFll; c.r5.s64 = 4; __imp__t_mulhd(c, base);
      CHECK(c.r3.s64 == 1, "mulhd big r3=%lld", c.r3.s64); }
    { auto c = fresh(); c.r4.s64 = -3; c.r5.s64 = 5; __imp__t_mulhd(c, base);
      CHECK(c.r3.s64 == -1, "mulhd negative r3=%lld", c.r3.s64); }

    // rlwimi. r3, r4, 8, 16, 23: insert rotl(r4, 8) under mask 0x0000FF00
    { auto c = fresh(); c.r3.u64 = 0xAAAAAAAA; c.r4.u64 = 0xCD; __imp__t_rlwimi_rc(c, base);
      CHECK(c.r3.u32 == 0xAAAACDAA && c.cr0.lt && !c.cr0.eq, "rlwimi. r3=%x lt=%d", c.r3.u32, c.cr0.lt); }
}

static void test_float()
{
    struct { double in; int32_t out; } cases[] = {
        { 2.5, 2 }, { 3.5, 4 }, { -2.5, -2 }, { 1.4, 1 }, { 1e10, INT_MAX }, { -1e10, INT_MIN },
        { std::numeric_limits<double>::quiet_NaN(), INT_MIN },
    };
    for (auto& t : cases)
    {
        auto c = fresh(); c.f2.f64 = t.in; __imp__t_fctiw(c, base);
        CHECK(c.f1.s32 == t.out, "fctiw(%g) = %d, expected %d", t.in, c.f1.s32, t.out);
    }
    { auto c = fresh(); c.f2.f64 = 4.0; __imp__t_frsqrte(c, base);
      CHECK(c.f1.f64 == 0.5, "frsqrte(4) = %g", c.f1.f64); }
    { auto c = fresh(); c.f2.f64 = 2; c.f3.f64 = 3; c.f4.f64 = 4; __imp__t_fnmadd(c, base);
      CHECK(c.f1.f64 == -10.0, "fnmadd -(2*3+4) = %g", c.f1.f64); }
}

static void test_branches()
{
    // Each test function returns r3=2 if the branch is taken, 1 if not.
    auto run = [](void (*fn)(PPCContext&, uint8_t*), uint64_t ctr, int lt, int gt, int eq, uint64_t& ctrOut) {
        auto c = fresh(); c.ctr.u64 = ctr; c.cr6.lt = lt; c.cr6.gt = gt; c.cr6.eq = eq; fn(c, base); ctrOut = c.ctr.u64; return c.r3.u64;
    };
    uint64_t ctr;
    CHECK(run(__imp__t_bdzf_eq, 1, 0, 0, 0, ctr) == 2 && ctr == 0, "bdzf eq: ctr->0, eq=0 should branch");
    CHECK(run(__imp__t_bdzf_eq, 1, 0, 0, 1, ctr) == 1, "bdzf eq: eq=1 should not branch");
    CHECK(run(__imp__t_bdzf_eq, 2, 0, 0, 0, ctr) == 1 && ctr == 1, "bdzf eq: ctr->1 should not branch");
    CHECK(run(__imp__t_bdzf_gt, 1, 0, 0, 1, ctr) == 2, "bdzf gt: gt=0 (eq=1) should branch");
    CHECK(run(__imp__t_bdzf_gt, 1, 0, 1, 0, ctr) == 1, "bdzf gt: gt=1 should not branch");
    CHECK(run(__imp__t_bdnzt_lt, 2, 1, 0, 0, ctr) == 2, "bdnzt lt: ctr->1, lt=1 should branch");
    CHECK(run(__imp__t_bdnzt_lt, 1, 1, 0, 0, ctr) == 1, "bdnzt lt: ctr->0 should not branch");
    CHECK(run(__imp__t_bdnzt_lt, 2, 0, 0, 0, ctr) == 1, "bdnzt lt: lt=0 should not branch");
    // Previously bdnzf always tested eq; this checks the decoded bit is used.
    CHECK(run(__imp__t_bdnzf_gt, 2, 0, 0, 1, ctr) == 2, "bdnzf gt: gt=0 (eq=1) should branch");
    CHECK(run(__imp__t_bdnzf_gt, 2, 0, 1, 0, ctr) == 1, "bdnzf gt: gt=1 (eq=0) should not branch");
}

static void test_vector()
{
    {   // vsubuhs: unsigned saturating halfword subtract
        auto c = fresh();
        uint16_t a[8] = { 5, 0, 65535, 100, 7, 1, 300, 0 }, b[8] = { 3, 1, 1, 200, 7, 0, 299, 65535 }, e[8] = { 2, 0, 65534, 0, 0, 1, 1, 0 };
        for (int i = 0; i < 8; i++) { setH(c.v2, i, a[i]); setH(c.v3, i, b[i]); }
        __imp__t_vsubuhs(c, base);
        for (int i = 0; i < 8; i++) CHECK(getH(c.v1, i) == e[i], "vsubuhs[%d] = %u, expected %u", i, getH(c.v1, i), e[i]);
    }
    {   // vsubuwm: modular word subtract
        auto c = fresh();
        uint32_t a[4] = { 5, 0, 10, 0x80000000 }, b[4] = { 3, 1, 20, 1 }, e[4] = { 2, 0xFFFFFFFF, 0xFFFFFFF6, 0x7FFFFFFF };
        for (int i = 0; i < 4; i++) { setW(c.v2, i, a[i]); setW(c.v3, i, b[i]); }
        __imp__t_vsubuwm(c, base);
        for (int i = 0; i < 4; i++) CHECK(getW(c.v1, i) == e[i], "vsubuwm[%d] = %x, expected %x", i, getW(c.v1, i), e[i]);
    }
    {   // vslh / vsrh: shift count is the low 4 bits of each halfword of vB
        auto c = fresh();
        uint16_t a[8] = { 1, 0x8001, 0xFFFF, 3, 0x00F0, 1, 2, 0x1234 }, b[8] = { 1, 1, 16, 15, 4, 0, 17, 0xFFF0 };
        uint16_t e[8] = { 2, 0x0002, 0xFFFF, 0x8000, 0x0F00, 1, 4, 0x1234 };
        for (int i = 0; i < 8; i++) { setH(c.v2, i, a[i]); setH(c.v3, i, b[i]); }
        __imp__t_vslh(c, base);
        for (int i = 0; i < 8; i++) CHECK(getH(c.v1, i) == e[i], "vslh[%d] = %x, expected %x", i, getH(c.v1, i), e[i]);
    }
    {
        auto c = fresh();
        uint16_t a[8] = { 0x8000, 0x8000, 0xFFFF, 4, 0x0F00, 9, 0xFFFF, 0x1234 }, b[8] = { 15, 1, 17, 2, 8, 0, 16, 0xFFF4 };
        uint16_t e[8] = { 1, 0x4000, 0x7FFF, 1, 0x000F, 9, 0xFFFF, 0x0123 };
        for (int i = 0; i < 8; i++) { setH(c.v2, i, a[i]); setH(c.v3, i, b[i]); }
        __imp__t_vsrh(c, base);
        for (int i = 0; i < 8; i++) CHECK(getH(c.v1, i) == e[i], "vsrh[%d] = %x, expected %x", i, getH(c.v1, i), e[i]);
    }
    // vpkswss / vpkswss128: vD = sat16(vA words) then sat16(vB words), in PowerPC element order
    for (auto fn : { __imp__t_vpkswss, __imp__t_vpkswss128 })
    {
        auto c = fresh();
        int32_t a[4] = { 1, -1, 70000, -70000 }, b[4] = { 32767, 32768, -32768, -32769 };
        int16_t e[8] = { 1, -1, 32767, -32768, 32767, 32767, -32768, -32768 };
        for (int i = 0; i < 4; i++) { setW(c.v2, i, uint32_t(a[i])); setW(c.v3, i, uint32_t(b[i])); }
        fn(c, base);
        for (int i = 0; i < 8; i++) CHECK(int16_t(getH(c.v1, i)) == e[i], "vpkswss%s[%d] = %d, expected %d", fn == __imp__t_vpkswss ? "" : "128", i, int16_t(getH(c.v1, i)), e[i]);
    }
    {   // vctuxs v1, v3, 1: unsigned saturate of v3 * 2
        auto c = fresh();
        float b[4] = { 1.5f, -1.0f, 3e9f, std::numeric_limits<float>::quiet_NaN() };
        uint32_t e[4] = { 3, 0, 0xFFFFFFFF, 0 };
        for (int i = 0; i < 4; i++) setF(c.v3, i, b[i]);
        __imp__t_vctuxs(c, base);
        for (int i = 0; i < 4; i++) CHECK(getW(c.v1, i) == e[i], "vctuxs[%d] = %x, expected %x", i, getW(c.v1, i), e[i]);
    }
    {   // vcfpuxws128 v1, v3, 2: unsigned saturate of v3 * 4
        auto c = fresh();
        float b[4] = { 1.25f, 0.1f, -5.0f, 1.1e9f };
        uint32_t e[4] = { 5, 0, 0, 0xFFFFFFFF };
        for (int i = 0; i < 4; i++) setF(c.v3, i, b[i]);
        __imp__t_vcfpuxws128(c, base);
        for (int i = 0; i < 4; i++) CHECK(getW(c.v1, i) == e[i], "vcfpuxws128[%d] = %x, expected %x", i, getW(c.v1, i), e[i]);
    }
    {   // vcmpgtsw.: mixed, all-true and all-false results for cr6
        struct { int32_t a[4], b[4]; uint32_t e[4]; int all, none; } cases[] = {
            { { 5, -1, 0, 7 }, { 3, 0, 0, -8 }, { ~0u, 0, 0, ~0u }, 0, 0 },
            { { 1, 1, 1, 1 }, { 0, -5, -9, 0 }, { ~0u, ~0u, ~0u, ~0u }, 1, 0 },
            { { 0, 0, 0, 0 }, { 0, 1, 2, 3 }, { 0, 0, 0, 0 }, 0, 1 },
        };
        for (auto& t : cases)
        {
            auto c = fresh();
            for (int i = 0; i < 4; i++) { setW(c.v2, i, uint32_t(t.a[i])); setW(c.v3, i, uint32_t(t.b[i])); }
            __imp__t_vcmpgtsw_rc(c, base);
            for (int i = 0; i < 4; i++) CHECK(getW(c.v1, i) == t.e[i], "vcmpgtsw.[%d] = %x", i, getW(c.v1, i));
            CHECK(c.cr6.lt == t.all && c.cr6.eq == t.none, "vcmpgtsw. cr6 lt=%d eq=%d, expected %d %d", c.cr6.lt, c.cr6.eq, t.all, t.none);
        }
    }
    {   // vcmpgtuw.: unsigned, so 0xFFFFFFFF > 1 and 0x80000000 > 0x7FFFFFFF
        auto c = fresh();
        uint32_t a[4] = { 0xFFFFFFFF, 1, 0x80000000, 0 }, b[4] = { 1, 0xFFFFFFFF, 0x7FFFFFFF, 0 }, e[4] = { ~0u, 0, ~0u, 0 };
        for (int i = 0; i < 4; i++) { setW(c.v2, i, a[i]); setW(c.v3, i, b[i]); }
        __imp__t_vcmpgtuw_rc(c, base);
        for (int i = 0; i < 4; i++) CHECK(getW(c.v1, i) == e[i], "vcmpgtuw.[%d] = %x, expected %x", i, getW(c.v1, i), e[i]);
        CHECK(!c.cr6.lt && !c.cr6.eq, "vcmpgtuw. cr6 lt=%d eq=%d", c.cr6.lt, c.cr6.eq);
    }
    {   // vcmpgtuh.: only halfword 1 is true. A 32-bit lane mask would miss it
        // (it doesn't hold the lane's sign bit), wrongly reporting "none true".
        auto c = fresh();
        setH(c.v2, 1, 2);
        __imp__t_vcmpgtuh_rc(c, base);
        for (int i = 0; i < 8; i++) CHECK(getH(c.v1, i) == (i == 1 ? 0xFFFF : 0), "vcmpgtuh.[%d] = %x", i, getH(c.v1, i));
        CHECK(!c.cr6.lt && !c.cr6.eq, "vcmpgtuh. cr6 lt=%d eq=%d, expected 0 0", c.cr6.lt, c.cr6.eq);
    }
    {   // vsel128 v1, v2, v3 with v1 as the mask: (v3 & mask) | (v2 & ~mask)
        auto c = fresh();
        uint32_t mask[4] = { 0xFFFF0000, 0, 0xFFFFFFFF, 0x0F0F0F0F }, e[4] = { 0x22221111, 0x11111111, 0x22222222, 0x12121212 };
        for (int i = 0; i < 4; i++) { setW(c.v1, i, mask[i]); setW(c.v2, i, 0x11111111); setW(c.v3, i, 0x22222222); }
        __imp__t_vsel128(c, base);
        for (int i = 0; i < 4; i++) CHECK(getW(c.v1, i) == e[i], "vsel128[%d] = %x, expected %x", i, getW(c.v1, i), e[i]);
    }
    {   // vupkd3d128 format 3: halfwords 6, 7 -> x, y; z = 0, w = 1
        auto c = fresh();
        setH(c.v3, 6, 0x3C00); // 1.0
        setH(c.v3, 7, 0xC000); // -2.0
        setH(c.v3, 0, 0x4000); // must be ignored
        __imp__t_vupkd3d_f16x2(c, base);
        float e[4] = { 1.0f, -2.0f, 0.0f, 1.0f };
        for (int i = 0; i < 4; i++) CHECK(getF(c.v1, i) == e[i], "vupkd3d128 f16x2[%d] = %g, expected %g", i, getF(c.v1, i), e[i]);
    }
    {   // vupkd3d128 format 5: halfwords 4..7 -> x, y, z, w (normal, infinity, smallest subnormal, -0)
        auto c = fresh();
        uint16_t h[4] = { 0x3800, 0x7C00, 0x0001, 0x8000 };
        for (int i = 0; i < 4; i++) setH(c.v3, 4 + i, h[i]);
        setH(c.v3, 0, 0x4000); // must be ignored
        __imp__t_vupkd3d_f16x4(c, base);
        uint32_t e[4] = { fbitsOf(0.5f), 0x7F800000, fbitsOf(5.9604644775390625e-8f), 0x80000000 };
        for (int i = 0; i < 4; i++) CHECK(fbitsOf(getF(c.v1, i)) == e[i], "vupkd3d128 f16x4[%d] = %08X, expected %08X", i, fbitsOf(getF(c.v1, i)), e[i]);
    }
    {   // lvehx: loads the aligned 16 bytes; the addressed halfword must be correct
        auto c = fresh();
        for (int i = 0; i < 16; i++) base[0x600 + i] = uint8_t(i);
        c.r4.u64 = 0x600; c.r5.u64 = 2;
        __imp__t_lvehx(c, base);
        CHECK(getH(c.v1, 1) == 0x0203, "lvehx element 1 = %x, expected 0203", getH(c.v1, 1));
    }
}

int main()
{
    test_loads_with_update();
    test_stores_with_update();
    test_integer();
    test_float();
    test_branches();
    test_vector();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures != 0;
}
