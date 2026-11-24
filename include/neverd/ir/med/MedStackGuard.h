//===- MedStackGuard.h - Values of a stack protector check ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Which values of a function belong to its stack protector.  A protected
/// function copies the thread's stack guard into its frame at entry and
/// compares the two before it returns, leaving for __stack_chk_fail where
/// they differ.  GCC makes the comparison in the return register where that
/// holds no result (`mov rax, [rbp-8]; sub rax, fs:0x28; jne`), and Clang
/// loads the guard there to compare it.  Neither the guard nor anything the
/// check computes from it is a value of the program: no function returns
/// one.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDSTACKGUARD_H
#define NEVERD_IR_MED_MEDSTACKGUARD_H

#include "neverd/Common.h"
#include "neverd/ir/med/MedByteDemand.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// The values of one function that its stack protector check makes.
class MedStackGuard {
public:
  MedStackGuard() = default;
  /// Finds the loads of the thread's guard in \p Func and what follows from
  /// them.  The C libraries of x86 Linux and Android keep the guard in the
  /// thread control block, at %fs:0x28 (x86-64) and %gs:0x14 (i386); no
  /// value is found for another target.
  MedStackGuard(const MedFunc &Func, Arch Target);

  /// Whether the function reads the guard at all.
  bool empty() const { return Values.empty(); }

  /// Whether \p Value is the guard, a copy or a view of it, the difference
  /// or exclusive OR the check makes with it, a merge of nothing else, or a
  /// value only the check reads (the copy the frame kept).
  bool holds(const MedVar &Value) const {
    return !Value.isConst() && Values.count(medValueKey(Value)) != 0;
  }

private:
  MedValueMap<char> Values;
};

} // namespace neverd

#endif // NEVERD_IR_MED_MEDSTACKGUARD_H
