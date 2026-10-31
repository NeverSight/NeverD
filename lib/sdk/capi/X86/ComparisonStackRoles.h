//===- ComparisonStackRoles.h - Native stack-use candidates -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Byte-derived x64 fixed-frame address-use evidence. These hints retrieve
/// review alternatives; they do not normalize memory identity or prove
/// aliasing.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_X86_COMPARISONSTACKROLES_H
#define NEVERD_SDK_CAPI_X86_COMPARISONSTACKROLES_H

#include "neverd/diff/Control.h"
#include "neverd/diff/Program.h"

#include "llvm/ADT/ArrayRef.h"

#include <capstone/capstone.h>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::sdk::x86 {

enum class StackRoleEffect { None, Allocate, Release, Unsafe };

struct StackRoleUse {
  uint64_t Shape = 0;
  uint64_t Adjustment = 0;
  std::optional<int64_t> Offset;
  StackRoleEffect Effect = StackRoleEffect::None;
  uint32_t Operand = 0;
  uint8_t Width = 0;
  bool AddressTaken = false;
};

StackRoleUse readStackRoleUse(csh Handle, const cs_insn &I,
                              diff::InstructionFlow Flow);

/// EntryEnd excludes the first control transfer. The allocation must precede
/// it, all observed local uses must precede release, and slot ranges may not
/// overlap. Layout permutations remain candidate evidence, never equivalence.
uint64_t stackRoleCandidate(llvm::ArrayRef<StackRoleUse> Uses, size_t EntryEnd,
                            const diff::ComparisonControl &Control);

} // namespace neverd::sdk::x86

#endif // NEVERD_SDK_CAPI_X86_COMPARISONSTACKROLES_H
