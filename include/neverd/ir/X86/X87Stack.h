//===- X87Stack.h - Typed x87 stack-management effects --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguish rotating TOP, freeing a named physical slot and popping the
/// previous top. A rotation never changes a tag or initializes a payload.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X87STACK_H
#define NEVERD_IR_X86_X87STACK_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <optional>

namespace neverd {

struct X87StackEffect {
  int TopDelta;
  bool FreesOperand;
  bool Pops;
};

constexpr std::optional<X87StackEffect> x87StackEffect(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::X87Ffree:
    return X87StackEffect{0, true, false};
  case Intrinsic::X87Ffreep:
    return X87StackEffect{1, true, true};
  case Intrinsic::X87Fincstp:
    return X87StackEffect{1, false, false};
  case Intrinsic::X87Fdecstp:
    return X87StackEffect{-1, false, false};
  default:
    return std::nullopt;
  }
}

} // namespace neverd

#endif // NEVERD_IR_X86_X87STACK_H
