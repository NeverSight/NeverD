//===- X86ReturnRules.h - Protected return rules --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared scalar/IR rules for protected IRET and far-return state.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86RETURNRULES_H
#define NEVERD_IR_X86_X86RETURNRULES_H

#include <cstdint>

namespace neverd::detail {

template <class Operations> struct X86ReturnRules {
  using Value = typename Operations::Value;

  /// The caller owns null selectors, descriptor access order and reserved
  /// legacy descriptor bits. IA-32e additionally rejects a code L/D conflict.
  static void checkCode(Operations &O, Value Selector, Value Descriptor,
                        Value OldCPL, bool IA32e) {
    const auto Error = O.masked(Selector, 0xfffc);
    const auto CPL = O.masked(Selector, 3);
    const auto Access = O.shifted(Descriptor, 40);
    const auto DPL = O.masked(O.shifted(Access, 5), 3);
    O.faultIf(O.ne(O.masked(Access, 0x18), O.k(0x18)), 13, Error);
    if (IA32e)
      O.faultIf(O.eq(O.masked(Access, 0x6000), O.k(0x6000)), 13, Error);
    O.faultIf(O.less(CPL, OldCPL), 13, Error);
    O.faultIf(
        O.either(O.both(O.ne(O.masked(Access, 4), O.k(0)), O.less(CPL, DPL)),
                 O.both(O.eq(O.masked(Access, 4), O.k(0)), O.ne(CPL, DPL))),
        13, Error);
    O.faultIf(O.eq(O.masked(Access, 0x80), O.k(0)), 11, Error);
  }

  static auto invalidateData(Operations &O, Value Selector, Value Attributes,
                             Value CPL) {
    return O.both(
        O.ne(CPL, O.k(0)),
        O.either(O.eq(O.masked(Selector, 0xfffc), O.k(0)),
                 O.both(O.less(O.masked(O.shifted(Attributes, 5), 3), CPL),
                        O.ne(O.masked(Attributes, 12), O.k(12)))));
  }

  /// Original CPL/IOPL govern restoration, independently of the destination.
  /// VM86 is a separate transition. Word IRET clears RF and retains the upper
  /// flags; wider IRET restores RF/AC/ID and, at CPL0, VIF/VIP.
  static Value interruptFlags(Operations &O, Value Old, Value Saved,
                              Value OldCPL, unsigned Bytes) {
    auto Flags = O.bitOr(O.masked(Saved, 0x4dd5), O.k(2));
    Flags = O.bitOr(Flags, Bytes == 2 ? O.masked(Old, 0x240000)
                                      : O.masked(Saved, 0x250000));
    Flags =
        O.bitOr(Flags, O.choose(O.less(O.masked(O.shifted(Old, 12), 3), OldCPL),
                                O.masked(Old, 0x200), O.masked(Saved, 0x200)));
    Flags =
        O.bitOr(Flags, O.choose(O.eq(OldCPL, O.k(0)), O.masked(Saved, 0x3000),
                                O.masked(Old, 0x3000)));
    return O.bitOr(Flags, Bytes == 2 ? O.masked(Old, 0x180000)
                                     : O.choose(O.eq(OldCPL, O.k(0)),
                                                O.masked(Saved, 0x180000),
                                                O.masked(Old, 0x180000)));
  }
};

} // namespace neverd::detail

#endif // NEVERD_IR_X86_X86RETURNRULES_H
