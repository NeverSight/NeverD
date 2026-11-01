//===- X86MpxSemantics.h - Shared bounds arithmetic ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Value recipes shared by concrete MPX evaluation and explicit-state source.
/// Address recipes do not probe memory or establish access permissions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86MPXSEMANTICS_H
#define NEVERD_IR_X86_X86MPXSEMANTICS_H

#include "neverd/ir/X86/X86MpxContext.h"

#include <cstdint>

namespace neverd {

/// Ops supplies unsigned word operations for either integers or emitted IR.
/// Mode and address-width adjustment are authenticated before these recipes.
template <class Ops> class X86MpxSemantics {
  Ops &O;
  using Value = typename Ops::Value;

public:
  explicit X86MpxSemantics(Ops &O) : O(O) {}

  Value word(Value V, X86MpxMode Mode) {
    return Mode == X86MpxMode::Bits64 ? V : O.bitAnd(V, O.constant(UINT32_MAX));
  }

  auto failed(X86MpxMode Mode, X86MpxCheck Kind, Value V, Value Bound) {
    if (Kind == X86MpxCheck::UpperComplement)
      Bound = O.bitXor(Bound, O.constant(UINT64_MAX));
    V = word(V, Mode);
    Bound = word(Bound, Mode);
    return Kind == X86MpxCheck::Lower ? O.less(V, Bound) : O.less(Bound, V);
  }

  Value directory(X86MpxMode Mode, Value Config, Value Pointer,
                  unsigned Adjust) {
    const bool Wide = Mode == X86MpxMode::Bits64;
    const uint64_t IndexMask = (uint64_t{1} << (Wide ? 28 + Adjust : 20)) - 1;
    auto Index = O.bitAnd(O.right(Pointer, O.constant(Wide ? 20 : 12)),
                          O.constant(IndexMask));
    auto Offset = O.left(Index, O.constant(Wide ? 3 : 2));
    return word(O.add(O.bitAnd(Config, O.constant(~uint64_t{0xfff})), Offset),
                Mode);
  }

  Value table(X86MpxMode Mode, Value Entry, Value Pointer) {
    const bool Wide = Mode == X86MpxMode::Bits64;
    auto Index = O.bitAnd(O.right(Pointer, O.constant(Wide ? 3 : 2)),
                          O.constant(Wide ? 0x1ffff : 0x3ff));
    auto Offset = O.left(Index, O.constant(Wide ? 5 : 4));
    return word(
        O.add(O.bitAnd(Entry, O.constant(Wide ? ~uint64_t{7} : ~uint64_t{3})),
              Offset),
        Mode);
  }
};

struct X86MpxIntegerOps {
  using Value = uint64_t;
  static Value constant(uint64_t V) { return V; }
  static Value add(Value A, Value B) { return A + B; }
  static Value bitAnd(Value A, Value B) { return A & B; }
  static Value bitXor(Value A, Value B) { return A ^ B; }
  static Value right(Value A, Value B) { return A >> B; }
  static Value left(Value A, Value B) { return A << B; }
  static bool less(Value A, Value B) { return A < B; }
};

} // namespace neverd

#endif // NEVERD_IR_X86_X86MPXSEMANTICS_H
