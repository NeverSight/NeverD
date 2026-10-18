//===- IntrinsicInstructionText.h - The instruction behind one --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The instruction an intrinsic was lifted from, as a listing spells it,
/// where the intrinsic's C spelling leaves out what the instruction carries:
/// a trap's or a breakpoint's code (`brk #0x3E8`, which `__builtin_trap()`
/// does not say) and a barrier's option (`dmb ishld`).  The lifters keep
/// that operand as the intrinsic's first input; the C writers print this
/// text beside the call, or as the instruction an `asm` statement runs.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_INTRINSICINSTRUCTIONTEXT_H
#define NEVERD_IR_INTRINSICS_INTRINSICINSTRUCTIONTEXT_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd {

/// Whether \p Id keeps its instruction's operand as its first input: a trap
/// or breakpoint code, a hypervisor or secure-monitor call number, or a
/// barrier option.
bool intrinsicCarriesInstructionOperand(Intrinsic Id);

/// The instruction \p Id with first input \p Operand was lifted from
/// (`brk #0x3E8`, `dsb sy`, `ud1 eax, dword ptr [eax + 0x15]`); empty for an
/// intrinsic that carries no such operand, or without one.
std::string intrinsicInstructionText(Intrinsic Id,
                                     std::optional<uint64_t> Operand);

/// The option name of a DMB, DSB or ISB option field \p CRm (`ish`), or its
/// immediate (`#0x4`).
std::string armBarrierOptionText(uint64_t CRm);

/// The option field a barrier option \p Text spells (`ish` is 0xB, `#0x4`
/// is 4); none for any other text.
std::optional<uint64_t> armBarrierOptionValue(llvm::StringRef Text);

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_INTRINSICINSTRUCTIONTEXT_H
