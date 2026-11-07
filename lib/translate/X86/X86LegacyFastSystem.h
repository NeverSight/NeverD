//===- X86LegacyFastSystem.h - Protected fast transfers --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original SYSENTER/SYSEXIT grammar and coupled protected state publication.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYFASTSYSTEM_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYFASTSYSTEM_H

#include "X86LegacyEncoding.h"

#include "neverd/ir/X86/X86FastSystemRules.h"
#include "neverd/translate/X86/X86LegacyExecution.h"

#include <cstdint>
#include <initializer_list>
#include <optional>

namespace neverd::translate::detail {

struct X86LegacyFastSystemInstruction {
  bool Enter;
  bool Locked;
};

inline std::optional<X86LegacyFastSystemInstruction>
decodeLegacyFastSystem(llvm::ArrayRef<uint8_t> Bytes, bool Code32) {
  X86LegacyEncoding R(Bytes, Code32);
  if (R.byte() != 0x0f)
    return std::nullopt;
  const auto Opcode = R.byte();
  if ((Opcode != 0x34 && Opcode != 0x35) || !R.done())
    return std::nullopt;
  return X86LegacyFastSystemInstruction{Opcode == 0x34, R.Group1Prefix == 0xf0};
}

/// No target fetch, stack access or descriptor-table lookup. The execution
/// owner authenticates the original bytes and delivers any returned fault.
inline std::optional<X86MemoryException>
executeLegacyFastSystem(X86LegacySegmentState &Next, uint32_t &Flags,
                        uint32_t &PC, const X86LegacySysenterState &MSRs,
                        const X86LegacyFastSystemInstruction &F) {
  if (F.Locked)
    return X86MemoryException{6, 0, {}};
  if (!(MSRs.CS & 0xfffc) || (!F.Enter && Next.CPL))
    return X86MemoryException{13, 0, {}};
  PC = F.Enter ? MSRs.EIP : Next.GPR[2];
  Next.GPR[4] = F.Enter ? MSRs.ESP : Next.GPR[1];
  Next.CPL = F.Enter ? 0 : 3;
  for (bool Code : {true, false}) {
    const auto Offset =
        neverd::detail::x86FastSystemSelectorOffset(F.Enter, false, Code);
    const uint16_t Selector = ((MSRs.CS & 0xfffc) + Offset) | Next.CPL;
    Next.Segments[Code ? 1 : 2] = {
        0, UINT32_MAX, Selector,
        neverd::detail::x86FlatSystemAttributes(Code, false, Next.CPL), false};
  }
  Flags &= ~neverd::detail::x86FastSystemClearedFlags(F.Enter);
  return std::nullopt;
}

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYFASTSYSTEM_H
