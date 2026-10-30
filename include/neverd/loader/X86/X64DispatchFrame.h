//===- X64DispatchFrame.h - Exact x64 physical dispatch bridges -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recognizes the complete byte contract of a native physical dispatch bridge.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_X86_X64DISPATCHFRAME_H
#define NEVERD_LOADER_X86_X64DISPATCHFRAME_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/RelocationProvenance.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace neverd {

/// A closed instruction sequence that builds a physical dispatch frame. Its
/// target receives the original register bank, not an inferred C argument list.
/// This identity grants a native source bridge, not a dispatcher implementation
/// or an ordinary C frame/return proof.
struct X64DispatchFrame {
  static constexpr size_t EncodedBytes = 30;
  va_t Entry = 0;
  va_t Linkage = 0;
  va_t Target = 0;
  uint32_t Selector = 0;
  int8_t Segment = 0;
  std::string LinkageSymbol;
  std::string TargetSymbol;
};

inline std::optional<X64DispatchFrame>
x64DispatchFrame(const BinaryImage &Image, va_t Address) {
  if (Image.Arch != Arch::X64 || Image.Bits != Bitness::Bits64 ||
      Image.Format != BinaryFormat::COFF || Image.IsRelocatable ||
      Address > InvalidVA - X64DispatchFrame::EncodedBytes ||
      !Image.isCodeRange(Address, X64DispatchFrame::EncodedBytes) ||
      // A PE fixup beginning before the sequence may still overlap its bytes.
      hasRelocationProvenanceIn(Image,
                                Address >= sizeof(uint64_t) - 1
                                    ? Address - (sizeof(uint64_t) - 1)
                                    : 0,
                                Address + X64DispatchFrame::EncodedBytes))
    return std::nullopt;
  const uint8_t *Bytes = Image.readVA(Address, X64DispatchFrame::EncodedBytes);
  // mov rax,rsp; cli; sub rsp,16; push rax; pushfq; push imm8;
  // lea rax,[rip+disp32]; push rax; mov eax,imm32; jmp rel32.
  constexpr std::array<uint8_t, 11> Prefix{0x48, 0x8b, 0xc4, 0xfa, 0x48, 0x83,
                                           0xec, 0x10, 0x50, 0x9c, 0x6a};
  if (!Bytes || !std::equal(Prefix.begin(), Prefix.end(), Bytes) ||
      Bytes[12] != 0x48 || Bytes[13] != 0x8d || Bytes[14] != 0x05 ||
      Bytes[19] != 0x50 || Bytes[20] != 0xb8 || Bytes[25] != 0xe9)
    return std::nullopt;
  const auto Relative = [&](size_t Displacement,
                            size_t Next) -> std::optional<va_t> {
    const int64_t Delta = static_cast<int32_t>(
        llvm::support::endian::read32le(Bytes + Displacement));
    const va_t PC = Address + Next;
    if ((Delta >= 0 && uint64_t(Delta) > InvalidVA - PC) ||
        (Delta < 0 && uint64_t(-Delta) > PC))
      return std::nullopt;
    const va_t Target =
        Delta < 0 ? PC - uint64_t(-Delta) : PC + uint64_t(Delta);
    if (!Image.isCodeRange(Target, 1) ||
        (Target >= Address &&
         Target - Address < X64DispatchFrame::EncodedBytes))
      return std::nullopt;
    return Target;
  };
  const auto Linkage = Relative(15, 19), Target = Relative(26, 30);
  if (!Linkage || !Target)
    return std::nullopt;
  X64DispatchFrame Result{Address,
                          *Linkage,
                          *Target,
                          llvm::support::endian::read32le(Bytes + 21),
                          static_cast<int8_t>(Bytes[11]),
                          Image.getFunctionNameAt(*Linkage),
                          Image.getFunctionNameAt(*Target)};
  // Assembly operands name code symbols. Reject non-identifiers instead of
  // letting a display name introduce assembler syntax or choose a new target.
  const auto Symbol = [](llvm::StringRef Name) {
    return !Name.empty() &&
           (llvm::isAlpha(Name.front()) || Name.front() == '_') &&
           llvm::all_of(Name,
                        [](char C) { return llvm::isAlnum(C) || C == '_'; });
  };
  if (!Symbol(Result.LinkageSymbol) || !Symbol(Result.TargetSymbol))
    return std::nullopt;
  return Result;
}

inline std::string x64DispatchFrameAssembly(const X64DispatchFrame &Frame,
                                            bool LLVM = false) {
  const std::string Immediate = LLVM ? "$$" : "$";
  return "movq %rsp, %rax\n\tcli\n\tsubq " + Immediate +
         "16, %rsp\n\tpushq %rax\n\tpushfq\n\tpushq " + Immediate +
         std::to_string(Frame.Segment) + "\n\tleaq " + Frame.LinkageSymbol +
         "(%rip), %rax\n\tpushq %rax\n\tmovl " + Immediate +
         std::to_string(Frame.Selector) + ", %eax\n\tjmp " + Frame.TargetSymbol;
}

} // namespace neverd

#endif // NEVERD_LOADER_X86_X64DISPATCHFRAME_H
