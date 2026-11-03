//===- X86SegmentDescriptorRules.h - Shared descriptor rules --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Data/stack descriptor checks and cache fields, evaluated as scalars or IR.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86SEGMENTDESCRIPTORRULES_H
#define NEVERD_IR_X86_X86SEGMENTDESCRIPTORRULES_H

#include <cstdint>

namespace neverd::detail {

/// The caller owns null selectors, table access, accessed publication and
/// mode-specific transitions. O supplies exact scalar or symbolic operations.
template <class Operations> struct X86SegmentDescriptorRules {
  using Value = typename Operations::Value;

  static void checkData(Operations &O, bool Stack, Value Selector,
                        Value Descriptor, Value CPL) {
    const auto Error = O.masked(Selector, 0xfffc);
    const auto Access = O.shifted(Descriptor, 40);
    const auto DPL = O.masked(O.shifted(Access, 5), 3);
    if (Stack) {
      O.faultIf(O.either(O.ne(O.masked(Selector, 3), CPL),
                         O.either(O.ne(O.masked(Access, 0x1a), O.k(0x12)),
                                  O.ne(DPL, CPL))),
                13, Error);
    } else {
      O.faultIf(O.either(O.eq(O.masked(Access, 0x10), O.k(0)),
                         O.eq(O.masked(Access, 0x0a), O.k(8))),
                13, Error);
      O.faultIf(O.both(O.ne(O.masked(Access, 12), O.k(12)),
                       O.either(O.less(DPL, O.masked(Selector, 3)),
                                O.less(DPL, CPL))),
                13, Error);
    }
    O.faultIf(O.eq(O.masked(Access, 0x80), O.k(0)), Stack ? 12 : 11, Error);
  }

  static Value base(Operations &O, Value Descriptor) {
    return O.bitOr(O.masked(O.shifted(Descriptor, 16), 0xffffff),
                   O.masked(O.shifted(Descriptor, 32), 0xff000000));
  }

  static Value limit(Operations &O, Value Descriptor) {
    const auto Limit = O.bitOr(O.masked(Descriptor, 0xffff),
                               O.masked(O.shifted(Descriptor, 32), 0xf0000));
    return O.choose(O.ne(O.masked(Descriptor, uint64_t{1} << 55), O.k(0)),
                    O.bitOr(O.left(Limit, 12), O.k(0xfff)), Limit);
  }

  static Value attributes(Operations &O, Value Descriptor) {
    return O.bitOr(O.masked(O.shifted(Descriptor, 40), 0xf0ff), O.k(1));
  }
};

} // namespace neverd::detail

#endif // NEVERD_IR_X86_X86SEGMENTDESCRIPTORRULES_H
