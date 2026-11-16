// Included inside class Emitter (emitter.h): the helpers and handlers of the mixins, one block per Python class, in
// the Emitter's MRO order (Sdk20Ops, ExtOps, VecLdSt, IntOps, FpOps, NeonOps, NeonWideOps). RC_H(name) declares a
// handler std::string name(const Function& f, const Insn& i, const Ops& ops).

// *p where Python reads an attribute of an operand that may be None (an AttributeError there, a C++ exception here)
static const Op& need(const Op* p) {
    if (!p) throw std::runtime_error("operand is None");
    return *p;
}

// `x, y, z = (self.regop(i, o) for o in ops)`: every register in order, then the unpacking's count check
std::vector<Reg> regops(const Insn& i, const Ops& ops, size_t count) {
    std::vector<Reg> rs;
    for (const Op& o : ops) rs.push_back(regop(i, o));
    if (rs.size() != count) throw std::runtime_error("unpacking: wrong number of registers");
    return rs;
}

// ---- Sdk20Ops (emit_sdk20.cpp) ----
RC_H(i_csdb); RC_H(i_stg); RC_H(i_bfc); RC_H(i_umnegl); RC_H(i_smnegl); RC_H(i_saddlv); RC_H(i_sqabs); RC_H(i_sqneg);
static std::string _sq_shift(int bits, const std::string& a, const std::string& sh, bool rounding);
std::string sq_shl(const Insn& i, const Ops& ops, bool rounding);
RC_H(i_sqshl); RC_H(i_sqrshl); RC_H(i_uqshl); RC_H(i_sli); RC_H(i_sri); RC_H(i_pmull); RC_H(i_tbx);

// ---- ExtOps (emit_ext.cpp) ----
// LSE.match(mn) (emit.py's insn() dispatches a match to lse())
bool lse_match(const std::string& mn) const;
std::string lse(const Insn& i, const Ops& ops);
static const std::map<std::string, std::string>& LSE_OPS();
RC_H(i_xpaclri); RC_H(i_ldaxp); RC_H(i_stlxp);
std::string adalp(const Insn& i, const Ops& ops, bool signed_);
RC_H(i_sadalp); RC_H(i_uadalp); RC_H(i_rshrn); RC_H(i_sminv); RC_H(i_uminv);
std::string pairwise(const Insn& i, const Ops& ops, const std::string& expr, const std::string& kind);
RC_H(i_fmaxp); RC_H(i_fminp); RC_H(i_fmaxnmp); RC_H(i_fminnmp); RC_H(i_smaxp); RC_H(i_sminp); RC_H(i_umaxp);
RC_H(i_uminp);
std::string fac(const Insn& i, const Ops& ops, const std::string& op);
RC_H(i_facge); RC_H(i_facgt); RC_H(i_sqrdmulh); RC_H(i_shsub); RC_H(i_uhsub); RC_H(i_srshl); RC_H(i_uabdl);
RC_H(i_sabdl); RC_H(i_umlsl); RC_H(i_smlsl);
RC_H(ExtOps_i_pmull);  // ExtOps.i_pmull (Sdk20Ops.i_pmull shadows it and calls it as super().i_pmull)
std::string crypto2(const Insn& i, const Ops& ops, const std::string& fn);
std::string crypto1(const Insn& i, const Ops& ops, const std::string& fn);
std::string crypto3(const Insn& i, const Ops& ops, const std::string& fn);
RC_H(i_aese); RC_H(i_aesd); RC_H(i_aesmc); RC_H(i_aesimc); RC_H(i_sha1su1); RC_H(i_sha256su0); RC_H(i_sha1su0);
RC_H(i_sha256h); RC_H(i_sha256h2); RC_H(i_sha256su1);
std::string sha1_rounds(const Insn& i, const Ops& ops, const std::string& fn);
RC_H(i_sha1c); RC_H(i_sha1p); RC_H(i_sha1m); RC_H(i_sha1h);

// ---- VecLdSt (emit_vldst.cpp) ----
struct VList {
    std::vector<Reg> rs;
    std::vector<Op> regs;
    const Op* mem = nullptr;
    const Op* after = nullptr;
};
VList vlist(const Insn& i, const Ops& ops);
std::string vldst(const Insn& i, const Ops& ops, int nstruct, bool store, bool replicate = false);
RC_H(i_ld1); RC_H(i_st1); RC_H(i_ld1r); RC_H(i_ld2); RC_H(i_st2); RC_H(i_ld3); RC_H(i_st3); RC_H(i_ld4); RC_H(i_st4);
RC_H(i_ld2r); RC_H(i_ld3r); RC_H(i_ld4r);

// ---- IntOps (emit_int.cpp) ----
RC_H(i_mov); RC_H(i_movz); RC_H(i_movn); RC_H(i_movk); RC_H(i_adrp);
std::string arith(const Insn& i, const Ops& ops, const std::string& op, bool setflags);
RC_H(i_add); RC_H(i_sub); RC_H(i_adds); RC_H(i_subs); RC_H(i_cmp); RC_H(i_cmn); RC_H(i_neg); RC_H(i_negs);
RC_H(i_adc); RC_H(i_adcs); RC_H(i_sbc); RC_H(i_sbcs);
std::string logic(const Insn& i, const Ops& ops, const std::string& expr, bool setflags = false, bool invert = false);
RC_H(i_and); RC_H(i_ands); RC_H(i_orr); RC_H(i_eor); RC_H(i_bic); RC_H(i_bics); RC_H(i_orn); RC_H(i_eon); RC_H(i_tst);
RC_H(i_mvn);
std::string shift_op(const Insn& i, const Ops& ops, const std::string& kind);
RC_H(i_lsl); RC_H(i_lsr); RC_H(i_asr); RC_H(i_ror); RC_H(i_extr);
// to_pos: None and 0 are the same there (pos = to_pos or 0)
std::string bitfield(const Insn& i, const Ops& ops, int64_t lsb, int64_t width, bool signed_, bool insert = false,
                     int64_t to_pos = 0);
RC_H(i_ubfx); RC_H(i_sbfx); RC_H(i_ubfiz); RC_H(i_sbfiz); RC_H(i_bfi); RC_H(i_bfxil); RC_H(i_ubfm);
std::string ext(const Insn& i, const Ops& ops, const std::string& t);
RC_H(i_sxtb); RC_H(i_sxth); RC_H(i_sxtw); RC_H(i_uxtb); RC_H(i_uxth); RC_H(i_csel); RC_H(i_csinc); RC_H(i_csinv);
RC_H(i_csneg); RC_H(i_cset); RC_H(i_csetm); RC_H(i_cinc); RC_H(i_cinv); RC_H(i_cneg);
std::string ccmp(const Insn& i, const Ops& ops, const std::string& fn);
RC_H(i_ccmp); RC_H(i_ccmn); RC_H(i_madd); RC_H(i_msub); RC_H(i_mul); RC_H(i_mneg);
// acc: nullptr for None
std::string long_mul(const Insn& i, const Ops& ops, bool signed_, const char* acc = nullptr);
RC_H(i_smull); RC_H(i_umull); RC_H(i_smaddl); RC_H(i_umaddl); RC_H(i_smsubl); RC_H(i_umsubl); RC_H(i_smulh);
RC_H(i_umulh); RC_H(i_udiv); RC_H(i_sdiv); RC_H(i_clz); RC_H(i_cls); RC_H(i_rbit); RC_H(i_rev); RC_H(i_rev16);
RC_H(i_rev32); RC_H(i_rev64);

// ---- FpOps (emit_fp.cpp) ----
std::string fr(const Reg& r);
std::string fw(const Reg& r, const std::string& expr);
bool is_vec(const Insn& i, const Op& o);
std::string fbin(const Insn& i, const Ops& ops, const std::string& expr);
// like: nullptr for None
std::string velem(const Insn& i, const Op& o, const std::string& kind, const Reg* like = nullptr);
RC_H(i_fadd); RC_H(i_fsub); RC_H(i_fmul); RC_H(i_fdiv); RC_H(i_fnmul); RC_H(i_fabd); RC_H(i_fmaxnm); RC_H(i_fminnm);
RC_H(i_fmax); RC_H(i_fmin);
std::string fma(const Insn& i, const Ops& ops, const std::string& kind);
RC_H(i_fmadd); RC_H(i_fmsub); RC_H(i_fnmadd); RC_H(i_fnmsub);
std::string fun(const Insn& i, const Ops& ops, const std::string& expr);
RC_H(i_fneg); RC_H(i_fabs); RC_H(i_fsqrt); RC_H(i_frintm); RC_H(i_frintp); RC_H(i_frintz); RC_H(i_frinta);
RC_H(i_frintn); RC_H(i_fcmp); RC_H(i_fccmp); RC_H(i_fcsel); RC_H(i_fmov); RC_H(i_fcvt);
std::string cvt_to_int(const Insn& i, const Ops& ops, char rnd, bool signed_);
RC_H(i_fcvtzs); RC_H(i_fcvtzu); RC_H(i_fcvtms); RC_H(i_fcvtmu); RC_H(i_fcvtps); RC_H(i_fcvtpu); RC_H(i_fcvtas);
RC_H(i_fcvtau); RC_H(i_fcvtns); RC_H(i_fcvtnu);
std::string cvt_from_int(const Insn& i, const Ops& ops, bool signed_);
RC_H(i_scvtf); RC_H(i_ucvtf);

// ---- NeonOps (emit_neon.cpp) ----
std::pair<int, int> vinfo(const Op& o);
std::string lane_t(int bits, const std::string& kind);
std::string v_src(const Insn& i, const Op& o, int bits, const std::string& kind, const std::string& k);
std::string v_loop(const Insn& i, const Op& dst, int lanes, int bits, const std::function<std::string()>& body_fn,
                   bool keep_dst = false);
// The keyword arguments of v_bin / v_un (lanes, fp_from_int: nullptr for None; v_bin's unused acc left out)
struct VOpt {
    bool fp = false;
    bool signed_ = false;
    const char* lanes = nullptr;
    const char* fp_from_int = nullptr;
};
std::string v_bin(const Insn& i, const Ops& ops, const std::string& expr, const VOpt& opt);
std::string v_bin(const Insn& i, const Ops& ops, const std::string& expr) { return v_bin(i, ops, expr, VOpt{}); }
std::string v_un(const Insn& i, const Ops& ops, const std::string& expr, const VOpt& opt);
std::string v_un(const Insn& i, const Ops& ops, const std::string& expr) { return v_un(i, ops, expr, VOpt{}); }
std::string v_mov(const Function& f, const Insn& i, const Ops& ops);
RC_H(i_umov); RC_H(i_smov); RC_H(i_ins); RC_H(i_dup); RC_H(i_movi); RC_H(i_mvni);
std::string v_imm_logic(const Insn& i, const Ops& ops, const std::string& op);
RC_H(i_fmla); RC_H(i_fmls); RC_H(i_mla); RC_H(i_mls); RC_H(i_bsl); RC_H(i_bit); RC_H(i_bif); RC_H(i_smax); RC_H(i_smin);
RC_H(i_umax); RC_H(i_umin); RC_H(i_urhadd); RC_H(i_srhadd); RC_H(i_uhadd); RC_H(i_shadd); RC_H(i_sabd); RC_H(i_uabd);
RC_H(i_sqdmulh); RC_H(i_sshl); RC_H(i_ushl);
std::string cmp_mask(const Insn& i, const Ops& ops, const std::string& op, const std::string& kind);
RC_H(i_cmeq); RC_H(i_cmgt); RC_H(i_cmge); RC_H(i_cmhi); RC_H(i_cmhs); RC_H(i_cmlt); RC_H(i_cmle); RC_H(i_cmtst);
RC_H(i_fcmeq); RC_H(i_fcmgt); RC_H(i_fcmge); RC_H(i_fcmlt); RC_H(i_fcmle);
std::string v_shift_imm(const Insn& i, const Ops& ops, const std::string& expr, bool signed_ = false, bool acc = false);
RC_H(i_shl); RC_H(i_ushr); RC_H(i_sshr); RC_H(i_srshr); RC_H(i_urshr); RC_H(i_ssra); RC_H(i_usra); RC_H(i_srsra);
RC_H(i_ursra);

// ---- NeonWideOps (emit_neon_wide.cpp) ----
std::string widen(const Insn& i, const Ops& ops, const std::string& expr, bool signed_, bool second_wide = false,
                  bool acc = false);
RC_H(i_ushll); RC_H(i_sshll); RC_H(i_uxtl); RC_H(i_sxtl); RC_H(i_shll); RC_H(i_uaddw); RC_H(i_saddw); RC_H(i_usubw);
RC_H(i_ssubw); RC_H(i_uaddl); RC_H(i_saddl); RC_H(i_usubl); RC_H(i_ssubl);
std::string v_long(const Insn& i, const Ops& ops, const std::string& expr, bool signed_, bool high);
RC_H(i_umull2); RC_H(i_smull2); RC_H(i_umlal); RC_H(i_smlal);
std::string pairwise_long(const Insn& i, const Ops& ops, bool signed_);
RC_H(i_saddlp); RC_H(i_uaddlp);
std::string narrow(const Insn& i, const Ops& ops, const std::string& expr, const std::string& src_kind = "u",
                   const std::string& dst_kind = "u");
RC_H(i_xtn); RC_H(i_shrn); RC_H(i_sqshrn); RC_H(i_sqrshrn); RC_H(i_sqadd); RC_H(i_sqsub); RC_H(i_uqadd); RC_H(i_uqsub);
RC_H(i_uaddlv); RC_H(i_smaxv); RC_H(i_frsqrts); RC_H(i_frecps); RC_H(i_sqxtun); RC_H(i_sqxtn); RC_H(i_uqxtn);
RC_H(i_sqshrun); RC_H(i_sqrshrun); RC_H(i_fcvtn); RC_H(i_fcvtl);
std::string v_cvt_to_int(const Insn& i, const Ops& ops, char rnd, bool signed_);
RC_H(i_ext);
std::string permute(const Insn& i, const Ops& ops, const std::string& body);
RC_H(i_zip1); RC_H(i_zip2); RC_H(i_uzp1); RC_H(i_uzp2); RC_H(i_trn1); RC_H(i_trn2);
std::string v_rev(const Insn& i, const Ops& ops, int container);
RC_H(i_tbl); RC_H(i_addv); RC_H(i_umaxv); RC_H(i_fmaxnmv); RC_H(i_fminnmv); RC_H(i_faddp); RC_H(i_addp); RC_H(i_cnt);
RC_H(i_not); RC_H(i_abs); RC_H(i_frecpe); RC_H(i_frsqrte);
