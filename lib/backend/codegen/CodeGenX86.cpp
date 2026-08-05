//===- CodeGenX86.cpp - x86 target feature detection --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86/x86-64 target CPU and feature-string detection for code generation.
/// Maps the intrinsics referenced by the recompiled module to the minimal
/// `-mattr` feature set (SSE/SSE2..SSE4.2, AVX/AVX2, AES, PCLMUL, BMI2,
/// POPCNT, LZCNT, SHA) needed for the LLVM x86 backend to lower them.
///
//===----------------------------------------------------------------------===//

#include "CodeGenDetail.h"

namespace neverd {

namespace {
// The default AMD64 baseline CPU model has no AVX scheduling/legalization
// support; forcing +avx/+avx2 features onto it leaves the x86 backend
// mis-legalizing 256-bit (YMM) values (e.g. an i256 sign-extend-and-pack is
// dropped to a single broadcast).  When 256-bit code is emitted, select a CPU
// model that actually understands YMM so legalization is correct.  kX86CpuAvx
// adds no extra ISA beyond the explicit feature string (it carries no implied
// instruction features), so the recompiled object stays within the detected
// instruction set.
constexpr const char *kX86CpuBaseline = "x86-64";
constexpr const char *kX86CpuAvx = "";
} // namespace

std::pair<std::string, std::string>
detectTargetFeaturesX86(const std::set<std::string> &Names) {
  auto Has = [&](const char *P) { return anyContains(Names, P); };

  bool AES = Has("aesni"), PCLMUL = Has("pclmulqdq"),
       SHA = Has("sha1") || Has("sha256");
  // The FP round-to-integral intrinsics (floor/ceil/trunc/round/roundeven via
  // substring "llvm.round", plus rint/nearbyint) all lower to SSE4.1
  // ROUNDSS/ROUNDSD (a single instruction whose immediate selects the rounding
  // mode); without +sse4.1 they fall back to floorf/ceilf/truncf/roundf/rintf/
  // nearbyintf library calls the rewrite image cannot resolve.  rint/nearbyint
  // were previously omitted from this gate.
  bool SSSE3 = Has("ssse3"),
       SSE41 = Has("sse41") || Has("llvm.round") || Has("llvm.floor") ||
               Has("llvm.ceil") || Has("llvm.trunc") || Has("llvm.rint") ||
               Has("llvm.nearbyint") || Has("sse42.crc32"),
       SSE42 = Has("sse42") || Has("sse42.crc32");
  bool SSE3 = Has("sse3") || SSSE3 || SSE41 || SSE42 || AES;
  SSSE3 = SSSE3 || SSE41 || SSE42;
  // llvm.fma lowers to a hardware VEX FMA (vfmadd*) only with +fma; without it
  // the backend falls back to an `fmaf`/`fma` libcall, which the bare-metal
  // round-trip cannot resolve.  FMA3 is VEX-encoded, so it also implies +avx.
  bool FMA = Has("llvm.fma");
  // Half-precision (_Float16): without +f16c the backend softens the
  // half<->float conversions (LLVM promotes half arithmetic through float) to
  // __extendhfsf2 / __truncsfhf2 library calls the rewrite image cannot
  // resolve.  F16C (VCVTPH2PS / VCVTPS2PH) is VEX-encoded, so it also implies
  // +avx.  Half arithmetic itself stays in float, so conversion support alone
  // (no native fp16 ALU) suffices.
  bool Half = Has(kUsesHalfMarker);
  bool AVX2 = Has("avx2"),
       AVX = Has("avx") || AVX2 || Has("avx512") || FMA || Half;

  std::string F = "+sse,+sse2,+cx16";
  if (SSE3)
    F += ",+sse3";
  if (SSSE3)
    F += ",+ssse3";
  if (SSE41)
    F += ",+sse4.1";
  if (SSE42)
    F += ",+sse4.2,+crc32";
  if (AES)
    F += ",+aes";
  if (AVX)
    F += ",+avx";
  if (Half)
    F += ",+f16c";
  if (FMA)
    F += ",+fma";
  if (AVX2)
    F += ",+avx2";
  // BMI1 BEXTR (register-variable bit-field extract) needs +bmi; BMI2 PDEP /
  // PEXT / BZHI need +bmi2.  Without the feature the backend cannot select the
  // intrinsic (or would emit a libcall the rewrite image cannot resolve).
  if (Has("bmi.bextr"))
    F += ",+bmi";
  if (Has("bmi.pdep") || Has("bmi.pext") || Has("bmi.bzhi"))
    F += ",+bmi2";
  if (PCLMUL)
    F += ",+pclmul";
  if (Has("ctpop"))
    F += ",+popcnt";
  if (Has("ctlz"))
    F += ",+lzcnt";
  if (SHA)
    F += ",+sha";
  return {AVX ? kX86CpuAvx : kX86CpuBaseline, F};
}

} // namespace neverd
