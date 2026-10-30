//===- X86AmxState.h - Architectural AMX bank operations ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Versioned operations on the complete architectural TILECFG/TILEDATA bank.
/// Native execution retains that bank in the CPU, including partial completion
/// at faults. Concrete execution owns the same bank in its register state.
/// These operations never pass tiles through an ordinary function value ABI.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86AMXSTATE_H
#define NEVERD_IR_INTRINSICS_X86_X86AMXSTATE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

enum class X86AmxOp : uint8_t {
  LoadConfig,
  StoreConfig,
  Release,
  Zero,
  Load,
  LoadT1,
  LoadRS,
  LoadRST1,
  Store,
  Int8SignedSigned,
  Int8SignedUnsigned,
  Int8UnsignedSigned,
  Int8UnsignedUnsigned,
  BF16,
  FP16,
  ComplexFP16Imaginary,
  ComplexFP16Real,
  BF8BF8,
  BF8HF8,
  HF8BF8,
  HF8HF8,
  TF32,
  MoveRow,
  Int32ToFP32,
  FP32ToBF16High,
  FP32ToBF16Low,
  FP32ToFP16High,
  FP32ToFP16Low,
  Count
};
static_assert(unsigned(X86AmxOp::TF32) - unsigned(X86AmxOp::Int8SignedSigned) ==
              unsigned(AMXTileComputeKind::TF32));
static_assert(unsigned(X86AmxOp::FP32ToFP16Low) - unsigned(X86AmxOp::MoveRow) ==
              unsigned(AMXTileRowKind::FP32ToFP16Low));
inline constexpr std::array<const char *, unsigned(X86AmxOp::Count)>
    X86AmxMnemonics = {
        "ldtilecfg",       "sttilecfg",       "tilerelease",   "tilezero",
        "tileloadd",       "tileloaddt1",     "tileloaddrs",   "tileloaddrst1",
        "tilestored",      "tdpbssd",         "tdpbsud",       "tdpbusd",
        "tdpbuud",         "tdpbf16ps",       "tdpfp16ps",     "tcmmimfp16ps",
        "tcmmrlfp16ps",    "tdpbf8ps",        "tdpbhf8ps",     "tdphbf8ps",
        "tdphf8ps",        "tmmultf32ps",     "tilemovrow",    "tcvtrowd2ps",
        "tcvtrowps2bf16h", "tcvtrowps2bf16l", "tcvtrowps2phh", "tcvtrowps2phl"};
// Version 2 separates the explicit SS prefix [18] from the original
// stack-default address [19]. Version 1 conflated them and also lost the
// stack default under a DS override; its unsegmented addr64 memory is
// ambiguous. op [4:0], tile [7:5], left [10:8], right [13:11], addr32 [14], row
// imm8 [15], APX memory [16], extended row GPR [17].
inline constexpr uint32_t X86AmxLegacyStateVersion = 1u << 24;
inline constexpr uint32_t X86AmxStateVersion = 2u << 24;
inline constexpr uint32_t X86AmxVersionMask = 0xffu << 24;
inline constexpr uint32_t X86AmxAddress32 = 1u << 14;
inline constexpr uint32_t X86AmxImmediateRow = 1u << 15;
inline constexpr uint32_t X86AmxEvexMemory = 1u << 16;
inline constexpr uint32_t X86AmxExtendedRow = 1u << 17;
inline constexpr uint32_t X86AmxStackSegment = 1u << 18;
inline constexpr uint32_t X86AmxStackReference = 1u << 19;
inline constexpr uint32_t X86AmxDescriptorMask = X86AmxVersionMask | 0xfffff;
inline constexpr uint16_t X86AmxRowBytes = 64;

constexpr X86AmxOp x86AmxOperation(uint32_t D) { return X86AmxOp(D & 31); }
constexpr unsigned x86AmxTile(uint32_t D, unsigned Operand = 0) {
  return (D >> (5 + Operand * 3)) & 7;
}
constexpr bool x86AmxIsRow(X86AmxOp K) {
  return K >= X86AmxOp::MoveRow && K < X86AmxOp::Count;
}
constexpr bool x86AmxIsCompute(X86AmxOp K) {
  return K >= X86AmxOp::Int8SignedSigned && K <= X86AmxOp::TF32;
}
constexpr bool x86AmxIsTileMemory(X86AmxOp K) {
  return K >= X86AmxOp::Load && K <= X86AmxOp::Store;
}
constexpr bool x86AmxIsMemory(X86AmxOp K) {
  return K == X86AmxOp::LoadConfig || K == X86AmxOp::StoreConfig ||
         x86AmxIsTileMemory(K);
}
constexpr bool x86AmxDescriptorIsValid(uint32_t D) {
  const auto K = x86AmxOperation(D);
  const auto Version = D & X86AmxVersionMask;
  if ((D & ~X86AmxDescriptorMask) ||
      (Version != X86AmxStateVersion && Version != X86AmxLegacyStateVersion) ||
      ((D & X86AmxStackReference) &&
       (Version != X86AmxStateVersion || (D & X86AmxAddress32))) ||
      K >= X86AmxOp::Count ||
      (!x86AmxIsMemory(K) &&
       (D & (X86AmxAddress32 | X86AmxEvexMemory | X86AmxStackSegment |
             X86AmxStackReference))) ||
      (!x86AmxIsRow(K) && (D & (X86AmxImmediateRow | X86AmxExtendedRow))) ||
      ((D & X86AmxExtendedRow) && (D & X86AmxImmediateRow)))
    return false;
  if (x86AmxIsCompute(K))
    return x86AmxTile(D) != x86AmxTile(D, 1) &&
           x86AmxTile(D) != x86AmxTile(D, 2) &&
           x86AmxTile(D, 1) != x86AmxTile(D, 2);
  if (x86AmxTile(D, 1) || x86AmxTile(D, 2))
    return false;
  return (K != X86AmxOp::LoadConfig && K != X86AmxOp::StoreConfig &&
          K != X86AmxOp::Release) ||
         !x86AmxTile(D);
}

constexpr bool x86AmxAddressIsValid(uint32_t D, NdMemoryAddressSpace Space) {
  if (!isKnownMemoryAddressSpace(Space))
    return false;
  if (!x86AmxIsMemory(x86AmxOperation(D)))
    return Space == NdMemoryAddressSpace::Default;
  if ((D & (X86AmxStackSegment | X86AmxStackReference)) &&
      Space != NdMemoryAddressSpace::Default)
    return false;
  if ((D & X86AmxVersionMask) == X86AmxLegacyStateVersion &&
      !(D & X86AmxAddress32) && Space == NdMemoryAddressSpace::Default)
    return false;
  return true;
}

constexpr bool x86AmxNeedsStackLeaf(uint32_t D) {
  return (D & X86AmxStackReference) && x86AmxIsTileMemory(x86AmxOperation(D));
}

struct X86AmxShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  bool ValidId = false;
  bool WritableOutput = false;
  bool EmptyBase = false;
  uint16_t OutputSize = 0;
  unsigned Arguments = 0;
  std::optional<uint32_t> Descriptor;
  std::optional<uint64_t> RowImmediate;
  std::array<uint16_t, 3> InputSizes{};
  std::array<bool, 3> ScalarInputs{};
};
constexpr bool x86AmxShapeIsValid(const X86AmxShape &S) {
  if (!S.ValidId || !S.Descriptor || !x86AmxDescriptorIsValid(*S.Descriptor) ||
      (S.TargetArch != Arch::Unknown && S.TargetArch != Arch::X64) ||
      S.MemoryOrdering != NdMemoryOrdering::None ||
      !x86AmxAddressIsValid(*S.Descriptor, S.MemoryAddressSpace))
    return false;
  const auto K = x86AmxOperation(*S.Descriptor);
  const bool Row = x86AmxIsRow(K), Memory = x86AmxIsMemory(K);
  const auto Arguments = x86AmxIsTileMemory(K) || Row ? 3u : 2u;
  if (S.Arguments != Arguments || S.InputSizes[0] != 8 ||
      S.InputSizes[1] != 4 ||
      (!Memory && (!S.EmptyBase ||
                   S.MemoryAddressSpace != NdMemoryAddressSpace::Default)) ||
      S.OutputSize != (Row ? X86AmxRowBytes : 0) ||
      (Row && !S.WritableOutput) ||
      (Arguments == 3 && S.InputSizes[2] != (Row ? 4 : 8)) ||
      ((*S.Descriptor & X86AmxImmediateRow) &&
       (!S.RowImmediate || *S.RowImmediate > UINT8_MAX)))
    return false;
  for (unsigned I = 0; I < Arguments; ++I)
    if (!S.ScalarInputs[I])
      return false;
  return true;
}
} // namespace neverd
#endif // NEVERD_IR_INTRINSICS_X86_X86AMXSTATE_H
