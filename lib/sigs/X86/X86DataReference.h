//===- X86DataReference.h - COFF external data fields ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Canonical RIP-relative data operands shared by generation and matching.
/// The trailing immediate determines the COFF REL32_N bias and RIP base.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_X86_X86DATAREFERENCE_H
#define NEVERD_SIGS_X86_X86DATAREFERENCE_H

#include "X86ImportReference.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::sigs {

struct X64DataReference {
  size_t End;
  unsigned Width;
  unsigned Bias;
};

/// MOV/LEA r,[RIP+d32], MOV [RIP+d32],r, CMP [RIP+d32],imm
/// MOV [RIP+d32],imm, AND [RIP+d32],imm and FF 15/25 indirect calls/jumps
/// through data. No address/segment overrides, extra REX bits or partial
/// displacements.
inline std::optional<X64DataReference>
x64DataReferenceField(llvm::ArrayRef<uint8_t> Code, size_t Offset) {
  if (Offset < 2 || Offset > Code.size() || Code.size() - Offset < 4)
    return std::nullopt;
  size_t Start = Offset - 2;
  unsigned Width = 4;
  if (Start && (Code[Start - 1] == 0x48 || Code[Start - 1] == 0x4c)) {
    --Start;
    Width = 8;
  }
  if (Start && isX86ReferencePrefix(Code[Start - 1], true))
    return std::nullopt;
  const uint8_t Op = Code[Offset - 2], ModRM = Code[Offset - 1];
  if ((ModRM & 0xc7) != 0x05)
    return std::nullopt;
  unsigned Bias = 0;
  if (Op == 0x8d) {
    if (Width != 8)
      return std::nullopt;
    // Taking an address proves mapping, not the object's extent.
    Width = 1;
  } else if (Op == 0xff && (ModRM == 0x15 || ModRM == 0x25)) {
    if (Start != Offset - 2 && Code[Start] != 0x48)
      return std::nullopt;
    Width = 8; // Near indirect calls/jumps read a full pointer in long mode.
  } else if (Op == 0x8b || Op == 0x89) {
    // Canonical register load/store, with the width selected above.
  } else if ((Op == 0x83 || Op == 0x81) && (ModRM == 0x3d || ModRM == 0x25)) {
    Bias = Op == 0x83 ? 1 : 4;
  } else if (Op == 0xc7 && ModRM == 0x05) {
    Bias = 4;
  } else {
    return std::nullopt;
  }
  if (Bias && Start != Offset - 2 && Code[Start] != 0x48)
    return std::nullopt; // REX.R is reserved for these opcode extensions.
  if (Code.size() - Offset - 4 < Bias)
    return std::nullopt;
  return X64DataReference{Offset + 4 + Bias, Width, Bias};
}

} // namespace neverd::sigs

#endif // NEVERD_SIGS_X86_X86DATAREFERENCE_H
