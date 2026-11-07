//===- X86StackFrameRules.h - Stack-frame operand rules --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared architectural nesting selection for ENTER lifting and execution.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86STACKFRAMERULES_H
#define NEVERD_IR_X86_X86STACKFRAMERULES_H

#include <cstdint>

namespace neverd {

constexpr unsigned x86EnterNestingLevel(uint64_t Immediate) {
  return Immediate & 31;
}

} // namespace neverd

#endif // NEVERD_IR_X86_X86STACKFRAMERULES_H
