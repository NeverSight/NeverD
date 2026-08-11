//===- DirectBranch.h - Direct branch scanning for runtime edges ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decodes the direct call and jump forms a language runtime's helper calls
/// are spelled with, so a loader can find the sites that reach a known runtime
/// entry point before any disassembler has run.
///
/// A byte pattern alone would be far too weak on a variable-length encoding.
/// What makes the scan sound is that a decoded target is kept only when it
/// lands exactly on an address the caller already proved is a runtime entry,
/// which a misaligned match will not do.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_DIRECTBRANCH_H
#define NEVERD_LOADER_DIRECTBRANCH_H

#include "neverd/Common.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd {

/// Decode a direct call or unconditional direct jump at \p VA, reading at most
/// \p Available bytes from \p Code.  Both forms matter: a runtime helper that
/// returns is called, while one that does not return is reached by a tail jump
/// as often as by a call.
///
/// \p Mode selects between the two ARM instruction sets, whose branches share
/// no encoding.  It is ignored on every other architecture.
///
/// The returned address never carries the ARM Thumb interworking bit, so a
/// caller matching it against symbol-derived addresses must normalize those
/// the same way (see \ref normalizeCodeAddress).
///
/// On success \p Length receives the size of the decoded instruction.
std::optional<va_t> decodeDirectBranchTarget(Arch A, InstructionMode Mode,
                                             const uint8_t *Code,
                                             size_t Available, va_t VA,
                                             size_t &Length);

/// Step between branch candidates.  Fixed-width targets are scanned at
/// instruction granularity; x86 must be scanned byte by byte because a call
/// can begin at any offset, and Thumb at halfword granularity because its
/// 16- and 32-bit instructions interleave freely.
inline unsigned getBranchScanStride(Arch A, InstructionMode Mode) {
  if (A == Arch::X64 || A == Arch::X86)
    return 1;
  if (A == Arch::ARM && Mode == InstructionMode::Thumb)
    return 2;
  return 4;
}

/// True for the targets \ref decodeDirectBranchTarget models.
inline bool canScanDirectBranches(Arch A, InstructionMode Mode) {
  (void)Mode;
  return A == Arch::X64 || A == Arch::X86 || A == Arch::AArch64 ||
         A == Arch::ARM;
}

/// Call \p Visit(SiteVA, TargetVA) for every direct branch in
/// [\p BeginVA, \p BeginVA + \p Size) whose bytes start at \p Code.
template <typename Fn>
void forEachDirectBranch(Arch A, InstructionMode Mode, const uint8_t *Code,
                         size_t Size, va_t BeginVA, Fn Visit) {
  if (!canScanDirectBranches(A, Mode))
    return;
  const unsigned Stride = getBranchScanStride(A, Mode);
  for (size_t Offset = 0; Offset + 4 <= Size; Offset += Stride) {
    size_t Length = 0;
    std::optional<va_t> Target = decodeDirectBranchTarget(
        A, Mode, Code + Offset, Size - Offset, BeginVA + Offset, Length);
    if (Target)
      Visit(BeginVA + Offset, *Target);
  }
}

} // namespace neverd

#endif // NEVERD_LOADER_DIRECTBRANCH_H
