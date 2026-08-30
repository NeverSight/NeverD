// x86/x64 intrinsic enum entries — included by intrinsics.h.
// Range: 1000 – 5999.  New entries go at the bottom.

// Software interrupts / faults.
Int3 = 1000,                // int3  -> @llvm.debugtrap
Ud2,                        // ud2   -> @llvm.trap
Int1,                       // icebp / int1 -> debugtrap
IntN,                       // int imm8 (non-3) — extern stub

// Identification / timing.
Cpuid,                      // cpuid                       (clobbers RAX/RBX/RCX/RDX)
Rdtsc,                      // rdtsc       -> @llvm.readcyclecounter
Rdtscp,                     // rdtscp                      (clobbers RAX/RDX/RCX)
Xgetbv,                     // xgetbv
Rdmsr, Wrmsr, Rdpmc,

// Synchronization / scheduling.
Pause,                      // pause   (yield hint)
Mfence,                     // mfence  -> fence seq_cst
Lfence,                     // lfence  -> fence acquire
Sfence,                     // sfence  -> fence release

// Cache control.
Clflush, Clflushopt, Clwb,
Prefetch,                   // prefetcht0/t1/t2/nta/w

// Port I/O (privileged).
In, Out, Insb, Insw, Insd, Outsb, Outsw, Outsd,

// Privileged system control.
Cli, Sti, Hlt, Invd, Wbinvd, Invlpg, Swapgs, Wrpkru, Rdpkru,
Lgdt, Lidt, Lldt, Sgdt, Sidt, Sldt, Lmsw, Smsw, Ltr, Str,

// Virtualization.
Vmcall, Vmmcall,

// SMM.
Rsm,                        // return from SMM

// SSE/AVX control & status.
Ldmxcsr, Stmxcsr,

// String ops (rep-prefixable side-effect family).
Movsb, Movsw, Movsd, Movsq,
Stosb, Stosw, Stosd, Stosq,
Lodsb, Lodsw, Lodsd, Lodsq,
Cmpsb, Cmpsw, Cmpsd_str, Cmpsq,
Scasb, Scasw, Scasd, Scasq,
Xlatb,

// SSE string compare (set RCX/EFLAGS).  The *Flag variants pack the control
// imm (bits 0-7) and a flag selector (bits 8-9: 0=CF, 1=ZF, 2=SF, 3=OF) into a
// single operand so the comparison's status flags can be modelled alongside the
// index/mask result within the 6-input INTRINSIC limit.
Pcmpistri, Pcmpistrm, Pcmpestri, Pcmpestrm, PcmpistrFlag, PcmpestrFlag,

// x87 FPU.
X87Op,

// Intel TSX (transactional memory).
Xbegin, Xend, Xabort, Xtest, Xacquire, Xrelease,

// Intel MPX (deprecated bounds).
BndMk, BndMov, BndCl, BndCu, BndCn, BndLdx, BndStx,

// FXSAVE / state save.
Fxsave, Fxrstor,

// AES / SHA / PCLMUL — generic catch-all.
AesGeneric, ShaGeneric,

// Far/long control transfer.
Lcall, Ljmp, Retf, Iret,

// Flag stack.
Pushf, Popf,

// Misc.
Enter, Leave,
Ud0, Ud1,
Nop,

// x86 BCD legacy.
Aaa, Aas, Aad, Aam, Daa, Das,

// BOUND.
Bound,

// CET shadow stack.
CetIncSsp, CetRdSsp, CetSaveprevssp, CetRstorssp,
CetWrss, CetWruss, CetSetssbsy, CetClrssbsy,

// PT.
Ptwrite,

// VIA PadLock.
Xcryptecb, Xcryptcbc, Xcryptctr, Xcryptcfb, Xcryptofb,
Xsha1, Xsha256, Montmul, Xstore,

// xsave/xrstor 64-bit.
Xsave64, Xrstor64, Xsaves64, Xrstors64, Xsavec64, Xsaveopt64,

// LWP.
Llwpcb, Slwpcb, Lwpins, Lwpval,

// SMAP.
Clac, Stac,

// AMD misc.
Clzero, Invlpga, Cldemote, Pconfig, Sysexitq, Sysretq,
Wbnoinvd, Extrq, Insertq,

// BMI2.
Pdep, Pext,

// AVX Gather/Scatter.
VGather, VScatter,

// SIMD shuffle/permute/unpack.
Shufps, Shufpd,
Unpckhps, Unpckhpd, Unpcklps, Unpcklpd,
Punpckhbw, Punpckhwd, Punpckhdq, Punpckhqdq,
Punpcklbw, Punpcklwd, Punpckldq, Punpcklqdq,
Pshufd, Pshufb, Pshufw, Pshufhw, Pshuflw,
Palignr,

// Packed shift (legacy catch-all, kept for backward compat).
PackedShift,

// AVX-512 / SSE-era SIMD intrinsics.
Vpternlog,
Kunpck, Vpcompress, Vpexpand, Vpconflict, Vpscatter, Vpgather,
Pmovmskb,
Movddup, Movshdup, Movsldup,
Phminposuw, Vpmultishiftqb,
Vrange, Vgetexp, Vgetmant, Vfixupimm, Vfpclass,
Dpps, Mpsadbw, Vscalef, Vexp2, Vreduce, Vdbpsadbw,
Vpshufbitqmb,

// AES — one code per instruction for correct LLVM intrinsic mapping.
AesEnc, AesEncLast,
AesDec, AesDecLast,
AesImc, AesKeyGenAssist,

// Per-lane packed shift — one code per direction+width.
PsllW, PsllD, PsllQ,
PsrlW, PsrlD, PsrlQ,
PsraW, PsraD,

// Broadcast — splat one element to all lanes.
BroadcastSS, BroadcastSD,
BroadcastB, BroadcastW, BroadcastD, BroadcastQ,

// Blend.
Blendd,                     // VPBLENDD — blend dwords by imm8

// 256-bit lane permute.
Perm2f128,                  // VPERM2F128 / VPERM2I128 — select 128-bit lanes
Permd,                      // VPERMD — variable dword permute

// Masked load/store.
MaskedLoadD,                // VPMASKMOVD load — masked load dwords
MaskedLoadQ,                // VPMASKMOVQ load — masked load qwords
MaskedStoreD,               // VPMASKMOVD store — masked store dwords
MaskedStoreQ,               // VPMASKMOVQ store — masked store qwords

// CRC32.
X86Crc32b,                  // CRC32 byte
X86Crc32w,                  // CRC32 word
X86Crc32d,                  // CRC32 dword
X86Crc32q,                  // CRC32 qword

// PCLMULQDQ.
Pclmulqdq,                  // PCLMULQDQ — carry-less multiplication

// SHA — per-instruction codes.
Sha1Rnds4,                  // SHA1RNDS4
Sha1Nexte,                  // SHA1NEXTE
Sha1Msg1,                   // SHA1MSG1
Sha1Msg2,                   // SHA1MSG2
Sha256Rnds2,                // SHA256RNDS2
Sha256Msg1,                 // SHA256MSG1
Sha256Msg2,                 // SHA256MSG2

// INTO — interrupt-on-overflow (distinct from `int imm8`, no operand).
Into,                       // into  -> inline asm "into"

// Per-lane saturating add/sub — mapped to @llvm.{s,u}{add,sub}.sat.iN.
// The lifter extracts each lane, calls the intrinsic, and reassembles.
// This bypasses LLVM's pattern-recognition of manual clamp chains, which
// has a constant-folding bug that mis-computes signed saturation results.
X86_SaddSat,   // signed saturating add   (@llvm.sadd.sat)
X86_UaddSat,   // unsigned saturating add (@llvm.uadd.sat)
X86_SsubSat,   // signed saturating sub   (@llvm.ssub.sat)
X86_UsubSat,   // unsigned saturating sub (@llvm.usub.sat)

// Per-lane signed/unsigned min/max — mapped to @llvm.{s,u}{min,max}.iN.
X86_Smin,      // signed minimum (@llvm.smin)
X86_Smax,      // signed maximum (@llvm.smax)
X86_Umin,      // unsigned minimum (@llvm.umin)
X86_Umax,      // unsigned maximum (@llvm.umax)

// x87 transcendental / special value-producing ops kept as the genuine
// instruction via inline asm (Unicorn runs them natively; lowering to libm
// would not roundtrip and would call functions absent under Unicorn).
X87Fsin, X87Fcos, X87F2xm1, X87Fscale, X87Fprem, X87Fprem1, X87Fpatan,
X87Fyl2x, X87Fyl2xp1, X87Fptan, X87Fxtractsig, X87Fxtractexp,

// x87 state operations. These have no integer result and must remain
// side-effecting instructions through MedIR/LLVM lowering.
X87Fninit, X87Fnclex,

// Capture the hardware x87 status word after an inline-asm value operation.
// Keep this at the end so existing intrinsic IDs remain stable.
X87ReadStatus,

// GFNI — per-byte operations over GF(2^8), for 128/256-bit vectors.
// Keep new entries append-only so existing serialized intrinsic IDs stay stable.
Gf2p8MulB,                  // (V)GF2P8MULB
Gf2p8AffineQb,              // (V)GF2P8AFFINEQB
Gf2p8AffineInvQb,           // (V)GF2P8AFFINEINVQB

// Append-only: implicit byte-masked store used by MASKMOVDQU/MASKMOVQ.
MaskedStoreB,

// Append-only: operand-carrying cache/state memory instructions.  Distinct
// prefetch IDs preserve the architectural hint instead of collapsing every
// encoding to PREFETCHT0.  State save/restore variants remain separate because
// their assembler mnemonic (including the 64-bit form) is semantically owned
// by the decoded instruction.
PrefetchT0, PrefetchT1, PrefetchT2, PrefetchNta, PrefetchW, PrefetchWT1,
Xsave, Xsavec, Xsaves, Xsaveopt, Xrstor, Xrstors,
Fxsave64Mem, Fxrstor64Mem,
X87Fldenv, X87Fnstenv, X87Frstor, X87Fnsave,
