//===- CodeGenARM.cpp - ARM32 target feature detection ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 target CPU and feature-string detection for code generation.
/// Enables the VFP/NEON baseline plus VFPv4 (for fused multiply-add) and the
/// optional crypto extensions referenced by the recompiled module.
///
//===----------------------------------------------------------------------===//

#include "CodeGenDetail.h"

namespace neverd {

std::pair<std::string, std::string>
detectTargetFeaturesARM(const std::set<std::string> &Names) {
  auto Has = [&](const char *P) { return anyContains(Names, P); };

  std::string F = "+v7,+vfp2,+vfp3,+neon,+hwdiv-arm";
  // VFMA (fused multiply-add) needs VFPv4; without it @llvm.fma lowers to a
  // fmaf/fma library call which the recompiled binary cannot resolve.
  if (Has("llvm.fma"))
    F += ",+vfp4";
  // The integral-rounding VRINT family (VRINTA/N/M/P/R/X/Z) lowers from the FP
  // round-to-integral intrinsics @llvm.round/roundeven (matched by the
  // "llvm.round" substring), floor, ceil, trunc (round-toward-zero -> VRINTZ)
  // and rint/nearbyint (-> VRINTX/VRINTR).  ARMv8 AArch32 crypto (AES/SHA)
  // likewise needs an ARMv8 baseline (+v8 / +fp-armv8); on the plain VFPv3
  // baseline these rounding intrinsics fall back to round*/floor/ceil/trunc/
  // rint/nearbyint library calls (or a broken fallback whose constant pool the
  // integrated assembler rejects) and the crypto intrinsics fail to select.
  // trunc/rint/nearbyint were previously omitted from this gate.
  bool Aes = Has("arm.neon.aes"), Sha = Has("arm.neon.sha");
  // CRC32B/H/W (+ Castagnoli) are the ARMv8-A AArch32 CRC extension; the
  // @llvm.arm.crc32* intrinsics fail to select without the +crc feature on an
  // ARMv8 baseline.
  bool Crc = Has("arm.crc32");
  bool Rounding = Has("llvm.round") || Has("llvm.floor") || Has("llvm.ceil") ||
                  Has("llvm.trunc") || Has("llvm.rint") ||
                  Has("llvm.nearbyint");
  // VMINNM/VMAXNM (IEEE minNum/maxNum) are ARMv8 FP instructions; @llvm.minnum/
  // maxnum fall back to fmin/fmax library calls without an ARMv8 baseline.  The
  // horizontal @llvm.vector.reduce.fmax/fmin reductions (and the IEEE
  // .fmaximum/ .fminimum forms, matched by the same substrings) decompose into
  // those same pairwise minNum/maxNum operations, so they need the ARMv8 FP
  // baseline too — without +fp-armv8 the reduction lowers to a broken fallback
  // whose constant- pool reference the integrated assembler rejects ("cannot
  // perform a PC-relative fixup with a non-zero symbol offset").
  bool MinMax = Has("llvm.minnum") || Has("llvm.maxnum") ||
                Has("llvm.minimum") || Has("llvm.maximum") ||
                Has("reduce.fmax") || Has("reduce.fmin");
  // Half-precision (_Float16): without the FP16 conversion extension the
  // backend softens half<->float to __aeabi_h2f / __aeabi_f2h library calls the
  // rewrite image cannot resolve.  +fp16 (the VFPv4 half-precision *conversion*
  // instructions VCVTB.F16/VCVTT.F16) is enough — LLVM promotes half arithmetic
  // through float, so no native fp16 ALU (+fullfp16) is needed.  The Unicorn
  // ARMv8 "max" CPU implements these conversion instructions (MVFR1.FPHP=2).
  if (Has(kUsesHalfMarker))
    F += ",+fp16";
  if (Aes || Sha || Crc || Rounding || MinMax)
    F += ",+v8,+fp-armv8";
  if (Aes)
    F += ",+aes";
  if (Sha)
    F += ",+sha2";
  if (Crc)
    F += ",+crc";
  return {"cortex-a7", F};
}

} // namespace neverd
