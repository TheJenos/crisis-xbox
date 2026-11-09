PPC_FUNC_IMPL(__imp__t_lhzu) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// lhzu r3,6(r4)
	ea = 6 + ctx.r4.u32;
	ctx.r3.u64 = PPC_LOAD_U16(ea);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lhzu) {
	__imp__t_lhzu(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_lhau) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// lhau r3,-2(r4)
	ea = -2 + ctx.r4.u32;
	ctx.r3.s64 = int16_t(PPC_LOAD_U16(ea));
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lhau) {
	__imp__t_lhau(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_lhzux) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// lhzux r3,r4,r5
	ea = ctx.r4.u32 + ctx.r5.u32;
	ctx.r3.u64 = PPC_LOAD_U16(ea);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lhzux) {
	__imp__t_lhzux(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_lbzux) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// lbzux r3,r4,r5
	ea = ctx.r4.u32 + ctx.r5.u32;
	ctx.r3.u64 = PPC_LOAD_U8(ea);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lbzux) {
	__imp__t_lbzux(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_lwzux) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// lwzux r3,r4,r5
	ea = ctx.r4.u32 + ctx.r5.u32;
	ctx.r3.u64 = PPC_LOAD_U32(ea);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lwzux) {
	__imp__t_lwzux(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_ldux) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// ldux r3,r4,r5
	ea = ctx.r4.u32 + ctx.r5.u32;
	ctx.r3.u64 = PPC_LOAD_U64(ea);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_ldux) {
	__imp__t_ldux(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_sthu) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// sthu r3,4(r4)
	ea = 4 + ctx.r4.u32;
	PPC_STORE_U16(ea, ctx.r3.u16);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_sthu) {
	__imp__t_sthu(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_sthux) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// sthux r3,r4,r5
	ea = ctx.r4.u32 + ctx.r5.u32;
	PPC_STORE_U16(ea, ctx.r3.u16);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_sthux) {
	__imp__t_sthux(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_stbux) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// stbux r3,r4,r5
	ea = ctx.r4.u32 + ctx.r5.u32;
	PPC_STORE_U8(ea, ctx.r3.u8);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_stbux) {
	__imp__t_stbux(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_stdux) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// stdux r3,r4,r5
	ea = ctx.r4.u32 + ctx.r5.u32;
	PPC_STORE_U64(ea, ctx.r3.u64);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_stdux) {
	__imp__t_stdux(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_lfsu) {
	PPC_FUNC_PROLOGUE();
	PPCRegister temp{};
	uint32_t ea{};
	// lfsu f1,8(r4)
	ctx.fpscr.disableFlushMode();
	ea = 8 + ctx.r4.u32;
	temp.u32 = PPC_LOAD_U32(ea);
	ctx.f1.f64 = double(temp.f32);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lfsu) {
	__imp__t_lfsu(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_lfsux) {
	PPC_FUNC_PROLOGUE();
	PPCRegister temp{};
	uint32_t ea{};
	// lfsux f1,r4,r5
	ctx.fpscr.disableFlushMode();
	ea = ctx.r4.u32 + ctx.r5.u32;
	temp.u32 = PPC_LOAD_U32(ea);
	ctx.f1.f64 = double(temp.f32);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lfsux) {
	__imp__t_lfsux(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_lfdu) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// lfdu f1,8(r4)
	ctx.fpscr.disableFlushMode();
	ea = 8 + ctx.r4.u32;
	ctx.f1.u64 = PPC_LOAD_U64(ea);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lfdu) {
	__imp__t_lfdu(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_lfdux) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// lfdux f1,r4,r5
	ctx.fpscr.disableFlushMode();
	ea = ctx.r4.u32 + ctx.r5.u32;
	ctx.f1.u64 = PPC_LOAD_U64(ea);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lfdux) {
	__imp__t_lfdux(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_stfsu) {
	PPC_FUNC_PROLOGUE();
	PPCRegister temp{};
	uint32_t ea{};
	// stfsu f1,8(r4)
	ctx.fpscr.disableFlushMode();
	ea = 8 + ctx.r4.u32;
	temp.f32 = float(ctx.f1.f64);
	PPC_STORE_U32(ea, temp.u32);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_stfsu) {
	__imp__t_stfsu(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_stfsux) {
	PPC_FUNC_PROLOGUE();
	PPCRegister temp{};
	uint32_t ea{};
	// stfsux f1,r4,r5
	ctx.fpscr.disableFlushMode();
	ea = ctx.r4.u32 + ctx.r5.u32;
	temp.f32 = float(ctx.f1.f64);
	PPC_STORE_U32(ea, temp.u32);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_stfsux) {
	__imp__t_stfsux(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_stfdu) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	// stfdu f1,8(r4)
	ctx.fpscr.disableFlushMode();
	ea = 8 + ctx.r4.u32;
	PPC_STORE_U64(ea, ctx.f1.u64);
	ctx.r4.u32 = ea;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_stfdu) {
	__imp__t_stfdu(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_lhbrx) {
	PPC_FUNC_PROLOGUE();
	// lhbrx r3,r4,r5
	ctx.r3.u64 = __builtin_bswap16(PPC_LOAD_U16(ctx.r4.u32 + ctx.r5.u32));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lhbrx) {
	__imp__t_lhbrx(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_eqv) {
	PPC_FUNC_PROLOGUE();
	// eqv r3,r4,r5
	ctx.r3.u64 = ~(ctx.r4.u64 ^ ctx.r5.u64);
	// blr 
	return;
}

PPC_WEAK_FUNC(t_eqv) {
	__imp__t_eqv(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_eqv_rc) {
	PPC_FUNC_PROLOGUE();
	// eqv. r3,r4,r5
	ctx.r3.u64 = ~(ctx.r4.u64 ^ ctx.r5.u64);
	ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
	// blr 
	return;
}

PPC_WEAK_FUNC(t_eqv_rc) {
	__imp__t_eqv_rc(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_addc) {
	PPC_FUNC_PROLOGUE();
	// addc r3,r4,r5
	ctx.xer.ca = ctx.r4.u32 + ctx.r5.u32 < ctx.r4.u32;
	ctx.r3.u64 = ctx.r4.u64 + ctx.r5.u64;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_addc) {
	__imp__t_addc(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_addme) {
	PPC_FUNC_PROLOGUE();
	PPCRegister temp{};
	// addme r3,r4
	temp.u8 = (ctx.r4.u32 != 0) | ctx.xer.ca;
	ctx.r3.u64 = ctx.r4.u64 + ctx.xer.ca - 1;
	ctx.xer.ca = temp.u8;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_addme) {
	__imp__t_addme(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_subfze) {
	PPC_FUNC_PROLOGUE();
	PPCRegister temp{};
	// subfze r3,r4
	temp.u8 = (ctx.r4.u32 == 0) & ctx.xer.ca;
	ctx.r3.u64 = ~ctx.r4.u64 + ctx.xer.ca;
	ctx.xer.ca = temp.u8;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_subfze) {
	__imp__t_subfze(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_subfze_rc) {
	PPC_FUNC_PROLOGUE();
	PPCRegister temp{};
	// subfze. r3,r4
	temp.u8 = (ctx.r4.u32 == 0) & ctx.xer.ca;
	ctx.r3.u64 = ~ctx.r4.u64 + ctx.xer.ca;
	ctx.xer.ca = temp.u8;
	ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
	// blr 
	return;
}

PPC_WEAK_FUNC(t_subfze_rc) {
	__imp__t_subfze_rc(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_mulhd) {
	PPC_FUNC_PROLOGUE();
	// mulhd r3,r4,r5
	ctx.r3.s64 = int64_t((__int128(ctx.r4.s64) * __int128(ctx.r5.s64)) >> 64);
	// blr 
	return;
}

PPC_WEAK_FUNC(t_mulhd) {
	__imp__t_mulhd(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_fctiw) {
	PPC_FUNC_PROLOGUE();
	// fctiw f1,f2
	ctx.fpscr.disableFlushMode();
	ctx.f1.s64 = (ctx.f2.f64 > double(INT_MAX)) ? INT_MAX : simde_mm_cvtsd_si32(simde_mm_load_sd(&ctx.f2.f64));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_fctiw) {
	__imp__t_fctiw(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_frsqrte) {
	PPC_FUNC_PROLOGUE();
	// frsqrte f1,f2
	ctx.fpscr.disableFlushMode();
	ctx.f1.f64 = 1.0 / sqrt(ctx.f2.f64);
	// blr 
	return;
}

PPC_WEAK_FUNC(t_frsqrte) {
	__imp__t_frsqrte(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_fnmadd) {
	PPC_FUNC_PROLOGUE();
	// fnmadd f1,f2,f3,f4
	ctx.fpscr.disableFlushMode();
	ctx.f1.f64 = -(ctx.f2.f64 * ctx.f3.f64 + ctx.f4.f64);
	// blr 
	return;
}

PPC_WEAK_FUNC(t_fnmadd) {
	__imp__t_fnmadd(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_rlwimi_rc) {
	PPC_FUNC_PROLOGUE();
	// rlwimi. r3,r4,8,16,23
	ctx.r3.u64 = (__builtin_rotateleft32(ctx.r4.u32, 8) & 0xFF00) | (ctx.r3.u64 & 0xFFFFFFFFFFFF00FF);
	ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
	// blr 
	return;
}

PPC_WEAK_FUNC(t_rlwimi_rc) {
	__imp__t_rlwimi_rc(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_bdzf_eq) {
	PPC_FUNC_PROLOGUE();
	// bdzf 4*cr6+eq,0x820000f4
	--ctx.ctr.u64;
	if (ctx.ctr.u32 == 0 && !ctx.cr6.eq) goto loc_820000F4;
	// li r3,1
	ctx.r3.s64 = 1;
	// blr 
	return;
loc_820000F4:
	// li r3,2
	ctx.r3.s64 = 2;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_bdzf_eq) {
	__imp__t_bdzf_eq(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_bdzf_gt) {
	PPC_FUNC_PROLOGUE();
	// bdzf 4*cr6+gt,0x82000108
	--ctx.ctr.u64;
	if (ctx.ctr.u32 == 0 && !ctx.cr6.gt) goto loc_82000108;
	// li r3,1
	ctx.r3.s64 = 1;
	// blr 
	return;
loc_82000108:
	// li r3,2
	ctx.r3.s64 = 2;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_bdzf_gt) {
	__imp__t_bdzf_gt(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_bdnzt_lt) {
	PPC_FUNC_PROLOGUE();
	// bdnzt 4*cr6+lt,0x8200011c
	--ctx.ctr.u64;
	if (ctx.ctr.u32 != 0 && ctx.cr6.lt) goto loc_8200011C;
	// li r3,1
	ctx.r3.s64 = 1;
	// blr 
	return;
loc_8200011C:
	// li r3,2
	ctx.r3.s64 = 2;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_bdnzt_lt) {
	__imp__t_bdnzt_lt(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_bdnzf_gt) {
	PPC_FUNC_PROLOGUE();
	// bdnzf 4*cr6+gt,0x82000130
	--ctx.ctr.u64;
	if (ctx.ctr.u32 != 0 && !ctx.cr6.gt) goto loc_82000130;
	// li r3,1
	ctx.r3.s64 = 1;
	// blr 
	return;
loc_82000130:
	// li r3,2
	ctx.r3.s64 = 2;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_bdnzf_gt) {
	__imp__t_bdnzf_gt(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vsubuhs) {
	PPC_FUNC_PROLOGUE();
	// vsubuhs v1,v2,v3
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_subs_epu16(simde_mm_load_si128((simde__m128i*)ctx.v2.u8), simde_mm_load_si128((simde__m128i*)ctx.v3.u8)));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vsubuhs) {
	__imp__t_vsubuhs(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vsubuwm) {
	PPC_FUNC_PROLOGUE();
	// vsubuwm v1,v2,v3
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_sub_epi32(simde_mm_load_si128((simde__m128i*)ctx.v2.u8), simde_mm_load_si128((simde__m128i*)ctx.v3.u8)));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vsubuwm) {
	__imp__t_vsubuwm(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vslh) {
	PPC_FUNC_PROLOGUE();
	PPCVRegister vTemp{};
	// vslh v1,v2,v3
	simde_mm_store_si128((simde__m128i*)vTemp.u8, simde_mm_load_si128((simde__m128i*)ctx.v3.u8));
	ctx.v1.u16[0] = uint16_t(ctx.v2.u16[0] << (vTemp.u16[0] & 0xF));
	ctx.v1.u16[1] = uint16_t(ctx.v2.u16[1] << (vTemp.u16[1] & 0xF));
	ctx.v1.u16[2] = uint16_t(ctx.v2.u16[2] << (vTemp.u16[2] & 0xF));
	ctx.v1.u16[3] = uint16_t(ctx.v2.u16[3] << (vTemp.u16[3] & 0xF));
	ctx.v1.u16[4] = uint16_t(ctx.v2.u16[4] << (vTemp.u16[4] & 0xF));
	ctx.v1.u16[5] = uint16_t(ctx.v2.u16[5] << (vTemp.u16[5] & 0xF));
	ctx.v1.u16[6] = uint16_t(ctx.v2.u16[6] << (vTemp.u16[6] & 0xF));
	ctx.v1.u16[7] = uint16_t(ctx.v2.u16[7] << (vTemp.u16[7] & 0xF));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vslh) {
	__imp__t_vslh(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vsrh) {
	PPC_FUNC_PROLOGUE();
	PPCVRegister vTemp{};
	// vsrh v1,v2,v3
	simde_mm_store_si128((simde__m128i*)vTemp.u8, simde_mm_load_si128((simde__m128i*)ctx.v3.u8));
	ctx.v1.u16[0] = uint16_t(ctx.v2.u16[0] >> (vTemp.u16[0] & 0xF));
	ctx.v1.u16[1] = uint16_t(ctx.v2.u16[1] >> (vTemp.u16[1] & 0xF));
	ctx.v1.u16[2] = uint16_t(ctx.v2.u16[2] >> (vTemp.u16[2] & 0xF));
	ctx.v1.u16[3] = uint16_t(ctx.v2.u16[3] >> (vTemp.u16[3] & 0xF));
	ctx.v1.u16[4] = uint16_t(ctx.v2.u16[4] >> (vTemp.u16[4] & 0xF));
	ctx.v1.u16[5] = uint16_t(ctx.v2.u16[5] >> (vTemp.u16[5] & 0xF));
	ctx.v1.u16[6] = uint16_t(ctx.v2.u16[6] >> (vTemp.u16[6] & 0xF));
	ctx.v1.u16[7] = uint16_t(ctx.v2.u16[7] >> (vTemp.u16[7] & 0xF));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vsrh) {
	__imp__t_vsrh(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vpkswss) {
	PPC_FUNC_PROLOGUE();
	// vpkswss v1,v2,v3
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*)ctx.v3.s32), simde_mm_load_si128((simde__m128i*)ctx.v2.s32)));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vpkswss) {
	__imp__t_vpkswss(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vctuxs) {
	PPC_FUNC_PROLOGUE();
	PPCVRegister vTemp{};
	// vctuxs v1,v3,1
	ctx.fpscr.enableFlushMode();
	for (int i = 0; i < 4; i++) {
		float x = ctx.v3.f32[i] * 2.0f;
		vTemp.u32[i] = (x != x || x <= 0.0f) ? 0u : (x >= 4294967296.0f) ? 0xFFFFFFFFu : uint32_t(x);
	}
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_load_si128((simde__m128i*)vTemp.u8));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vctuxs) {
	__imp__t_vctuxs(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vcmpgtsw_rc) {
	PPC_FUNC_PROLOGUE();
	// vcmpgtsw. v1,v2,v3
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_cmpgt_epi32(simde_mm_load_si128((simde__m128i*)ctx.v2.u8), simde_mm_load_si128((simde__m128i*)ctx.v3.u8)));
	ctx.cr6.setFromMask(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), 0xFFFF);
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vcmpgtsw_rc) {
	__imp__t_vcmpgtsw_rc(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vcmpgtuw_rc) {
	PPC_FUNC_PROLOGUE();
	// vcmpgtuw. v1,v2,v3
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_cmpgt_epi32(simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*)ctx.v2.u8), simde_mm_set1_epi32(INT_MIN)), simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*)ctx.v3.u8), simde_mm_set1_epi32(INT_MIN))));
	ctx.cr6.setFromMask(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), 0xFFFF);
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vcmpgtuw_rc) {
	__imp__t_vcmpgtuw_rc(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vcmpgtuh_rc) {
	PPC_FUNC_PROLOGUE();
	// vcmpgtuh. v1,v2,v3
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_cmpgt_epu16(simde_mm_load_si128((simde__m128i*)ctx.v2.u16), simde_mm_load_si128((simde__m128i*)ctx.v3.u16)));
	ctx.cr6.setFromMask(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), 0xFFFF);
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vcmpgtuh_rc) {
	__imp__t_vcmpgtuh_rc(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vsel128) {
	PPC_FUNC_PROLOGUE();
	// vsel128 v1,v2,v3,v1
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), simde_mm_load_si128((simde__m128i*)ctx.v2.u8)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), simde_mm_load_si128((simde__m128i*)ctx.v3.u8))));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vsel128) {
	__imp__t_vsel128(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vpkswss128) {
	PPC_FUNC_PROLOGUE();
	// vpkswss128 v1,v2,v3
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*)ctx.v3.s32), simde_mm_load_si128((simde__m128i*)ctx.v2.s32)));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vpkswss128) {
	__imp__t_vpkswss128(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vcfpuxws128) {
	PPC_FUNC_PROLOGUE();
	PPCVRegister vTemp{};
	// vcfpuxws128 v1,v3,2
	ctx.fpscr.enableFlushMode();
	for (int i = 0; i < 4; i++) {
		float x = ctx.v3.f32[i] * 4.0f;
		vTemp.u32[i] = (x != x || x <= 0.0f) ? 0u : (x >= 4294967296.0f) ? 0xFFFFFFFFu : uint32_t(x);
	}
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_load_si128((simde__m128i*)vTemp.u8));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vcfpuxws128) {
	__imp__t_vcfpuxws128(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_lvehx) {
	PPC_FUNC_PROLOGUE();
	// lvehx v1,r4,r5
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)(base + ((ctx.r4.u32 + ctx.r5.u32) & ~0xF))), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// blr 
	return;
}

PPC_WEAK_FUNC(t_lvehx) {
	__imp__t_lvehx(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vupkd3d_f16x2) {
	PPC_FUNC_PROLOGUE();
	PPCVRegister vTemp{};
	// vupkd3d128 v1,v3,12
	vTemp.f32[3] = ppc_half_to_float(ctx.v3.u16[1]);
	vTemp.f32[2] = ppc_half_to_float(ctx.v3.u16[0]);
	vTemp.f32[1] = 0.0f;
	vTemp.f32[0] = 1.0f;
	ctx.v1 = vTemp;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vupkd3d_f16x2) {
	__imp__t_vupkd3d_f16x2(ctx, base);
}

PPC_FUNC_IMPL(__imp__t_vupkd3d_f16x4) {
	PPC_FUNC_PROLOGUE();
	PPCVRegister vTemp{};
	// vupkd3d128 v1,v3,20
	vTemp.f32[0] = ppc_half_to_float(ctx.v3.u16[0]);
	vTemp.f32[1] = ppc_half_to_float(ctx.v3.u16[1]);
	vTemp.f32[2] = ppc_half_to_float(ctx.v3.u16[2]);
	vTemp.f32[3] = ppc_half_to_float(ctx.v3.u16[3]);
	ctx.v1 = vTemp;
	// blr 
	return;
}

PPC_WEAK_FUNC(t_vupkd3d_f16x4) {
	__imp__t_vupkd3d_f16x4(ctx, base);
}

