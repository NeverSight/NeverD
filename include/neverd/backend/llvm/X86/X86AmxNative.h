//===- X86AmxNative.h - Physical AMX native API plan ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// One mapping from the architectural bank descriptor to native API names,
/// constant controls, and instruction feature requirements.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86AMXNATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86AMXNATIVE_H

#include "neverd/backend/llvm/X86/X86NativeMemoryAddress.h"
#include "neverd/ir/intrinsics/X86/X86AmxState.h"

#if __has_include("llvm/IR/X86AMXNative.h")
#include "llvm/IR/X86AMXNative.h"
#endif

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace neverd {
inline constexpr unsigned X86AmxNativeStackReference = X86NativeStackReference;
constexpr bool x86AmxNativeStackMemoryIsAvailable() {
#if defined(LLVM_X86_AMX_STACK_MEMORY) && LLVM_X86_AMX_STACK_MEMORY >= 1
  return true;
#else
  return false;
#endif
}
inline std::string x86AmxNativeSuffix(uint32_t D) {
  constexpr std::array<const char *, unsigned(X86AmxOp::Count)> Names = {
      "ldtilecfg",       "sttilecfg",       "tilerelease",   "tilezero",
      "tileloadd64",     "tileloaddt164",   "tileloaddrs64", "tileloaddrst164",
      "tilestored64",    "tdpbssd",         "tdpbsud",       "tdpbusd",
      "tdpbuud",         "tdpbf16ps",       "tdpfp16ps",     "tcmmimfp16ps",
      "tcmmrlfp16ps",    "tdpbf8ps",        "tdpbhf8ps",     "tdphbf8ps",
      "tdphf8ps",        "tmmultf32ps",     "tilemovrow",    "tcvtrowd2ps",
      "tcvtrowps2bf16h", "tcvtrowps2bf16l", "tcvtrowps2phh", "tcvtrowps2phl"};
  if (!x86AmxDescriptorIsValid(D))
    return {};
  const auto K = x86AmxOperation(D);
  return std::string(Names[unsigned(K)]) +
         (x86AmxIsMemory(K) ? ".address"
          : x86AmxIsRow(K)  ? D & X86AmxImmediateRow ? "i" : ".reg"
                            : "");
}
inline std::optional<uint32_t> x86AmxNativeDescriptor(std::string_view Name) {
  for (unsigned K = 0; K != unsigned(X86AmxOp::Count); ++K)
    for (const uint32_t Flag : {0u, X86AmxImmediateRow}) {
      if (Flag && !x86AmxIsRow(X86AmxOp(K)))
        continue;
      const uint32_t D =
          X86AmxStateVersion | K | Flag |
          (x86AmxIsCompute(X86AmxOp(K)) ? (1u << 8) | (2u << 11) : 0);
      if (Name == "llvm.x86." + x86AmxNativeSuffix(D))
        return D;
    }
  return {};
}
inline unsigned x86AmxNativePrefix(uint32_t D, NdMemoryAddressSpace Space) {
  const unsigned Prefix = Space == NdMemoryAddressSpace::X86FS   ? 0x64
                          : Space == NdMemoryAddressSpace::X86GS ? 0x65
                          : D & X86AmxStackSegment               ? 0x36
                                                                 : 0x3e;
  return Prefix | (D & X86AmxStackReference ? X86AmxNativeStackReference : 0);
}
inline std::string x86AmxNativeFeatures(uint32_t D) {
  const auto K = x86AmxOperation(D);
  std::string F = "amx-tile";
  if (K >= X86AmxOp::Int8SignedSigned && K <= X86AmxOp::Int8UnsignedUnsigned)
    F += ",amx-int8";
  if (K == X86AmxOp::BF16)
    F += ",amx-bf16";
  if (K == X86AmxOp::FP16)
    F += ",amx-fp16";
  if (K == X86AmxOp::ComplexFP16Imaginary || K == X86AmxOp::ComplexFP16Real)
    F += ",amx-complex";
  if (K >= X86AmxOp::BF8BF8 && K <= X86AmxOp::HF8HF8)
    F += ",amx-fp8";
  if (K == X86AmxOp::TF32)
    F += ",amx-tf32";
  if (K == X86AmxOp::LoadRS || K == X86AmxOp::LoadRST1)
    F += ",amx-movrs";
  if (x86AmxIsRow(K))
    F += ",amx-avx512,avx10.2";
  if (D & (X86AmxEvexMemory | X86AmxExtendedRow))
    F += ",egpr";
  return F;
}
} // namespace neverd
#endif // NEVERD_BACKEND_LLVM_X86_X86AMXNATIVE_H
