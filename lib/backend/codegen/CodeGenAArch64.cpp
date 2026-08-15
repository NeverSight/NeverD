//===- CodeGenAArch64.cpp - AArch64 target feature detection ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 target CPU and feature-string detection for code generation.
/// Enables NEON plus the optional extensions (crypto AES/SHA2, SVE, CRC,
/// pointer authentication, MTE) actually referenced by the recompiled module.
///
//===----------------------------------------------------------------------===//

#include "CodeGenDetail.h"

namespace neverd {

std::pair<std::string, std::string>
detectTargetFeaturesAArch64(const std::set<std::string> &Names) {
  auto Has = [&](const char *P) { return anyContains(Names, P); };

  std::string F = "+neon";
  // Half-precision (FEAT_FP16) arithmetic: without +fullfp16 the backend
  // softens `half` ops to float (or aborts on the vector forms), so fp16
  // fadd/fmul/... on .8h/.4h lanes would not match hardware.  (Same gating idea
  // as +sha3/+sm4.)
  if (Has(kUsesHalfMarker))
    F += ",+fullfp16";
  // PMULL64 (@llvm.aarch64.neon.pmull64) is gated behind the AES/PMULL feature
  // even though it lives under the neon namespace; without +aes the backend
  // cannot select AArch64ISD::PMULL and aborts.  (p8 pmull is base NEON.)
  if (Has("aarch64.crypto.aes") || Has("pmull64"))
    F += ",+aes";
  if (Has("aarch64.crypto.sha"))
    F += ",+sha2";
  // SHA512 (sha512h/h2/su0/su1) is gated behind the separate FeatureSHA3; +sha2
  // alone cannot select these and the backend aborts.  (Without this the lift
  // would be forced to a placeholder — see CodeGenARM's +fp-armv8 fix, #276.)
  if (Has("aarch64.crypto.sha512"))
    F += ",+sha3";
  // SM3 (sm3partw1/2/ss1/tt1a..2b) and SM4 (sm4e/sm4ekey) need FeatureSM4
  // (which implies SM3).
  if (Has("aarch64.crypto.sm3") || Has("aarch64.crypto.sm4"))
    F += ",+sm4";
  // FJCVTZS (FEAT_JSCVT, @llvm.aarch64.fjcvtzs) needs FeatureJS; without
  // +jsconv the backend cannot select the `fjcvtzs` instruction.
  if (Has("aarch64.fjcvtzs"))
    F += ",+jsconv";
  // BFMMLA is a FEAT_BF16 instruction.  Its LLVM intrinsic is otherwise
  // well-typed but cannot be selected by the generic AArch64 subtarget.
  if (Has("aarch64.neon.bfmmla"))
    F += ",+bf16";
  if (Has("aarch64.neon.famax") || Has("aarch64.neon.famin"))
    F += ",+faminmax";
  if (Has("aarch64.sve"))
    F += ",+sve";
  if (Has("aarch64.crc32"))
    F += ",+crc";
  if (Has("aarch64.pauth") || Has("pacia") || Has("autia"))
    F += ",+pauth";
  if (Has("aarch64.mte") || Has("irg") || Has("gmi"))
    F += ",+mte";
  // MOPS phase instructions currently survive lifting as target inline asm.
  // Seeing one proves the input binary requires FEAT_MOPS; enable the feature
  // so patch-mode code generation accepts and preserves the instruction.
  if (Has("setp") || Has("setm") || Has("sete") || Has("cpyf") || Has("cpyp") ||
      Has("cpym") || Has("cpye"))
    F += ",+mops";
  return {"generic", F};
}

} // namespace neverd
