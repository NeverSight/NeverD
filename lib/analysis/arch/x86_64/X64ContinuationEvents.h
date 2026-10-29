//===- X64ContinuationEvents.h - Explicit long-mode events -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IRETQ and software-interrupt transitions through the continuation writer.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONEVENTS_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONEVENTS_H

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace neverd::analysis::detail {

/// Writer supplies temporary/emit/load/store/storeIf/rejectIf/leave. Keeping
/// this lowering above both C backends gives one owner for descriptor
/// validation, frame accesses and final state. All checks precede writes. A
/// pending CS accessed-bit update is forwarded into later reads: eager frame
/// loads or committing descriptors only after frame stores would change aliased
/// RAM.
template <class Writer> class X64ContinuationEvents {
  using State = X64ContinuationStateV1;
  using V5 = X64ContinuationStateV5;
  using V6 = X64ContinuationStateV6;
  using Segment = X64ContinuationSegment;
  using Metadata = X64ContinuationSegmentMetadata;
  using Table = X64ContinuationDescriptorTable;
  using Exit = X64ContinuationExitV1;
  Writer &W;
  unsigned AddressBits;
  std::optional<NdVar> AccessedAddress;

  static NdVar k(uint64_t V) { return NdVar::scalar(V, 8); }

  NdVar op(NdOp Code, std::initializer_list<NdVar> Inputs, uint16_t Bytes = 8) {
    const auto V = W.temporary(Bytes);
    W.emit(Code, V, Inputs);
    return V;
  }

  NdVar narrow(NdVar V, uint16_t Bytes) {
    return op(NdOp::SUBBYTES, {V, k(0)}, Bytes);
  }

  NdVar load(uint64_t Offset, uint16_t Bytes = 8) {
    const auto V = W.load(Offset, Bytes);
    return Bytes == 8 ? V : op(NdOp::INT_ZEXT, {V});
  }

  NdVar masked(NdVar V, uint64_t Mask) {
    return op(NdOp::INT_AND, {V, k(Mask)});
  }

  NdVar shifted(NdVar V, unsigned Bits) {
    return op(NdOp::INT_RIGHT, {V, k(Bits)});
  }

  NdVar eq(NdVar A, NdVar B) { return op(NdOp::INT_EQUAL, {A, B}, 1); }
  NdVar ne(NdVar A, NdVar B) { return op(NdOp::INT_NOTEQUAL, {A, B}, 1); }
  NdVar less(NdVar A, NdVar B) { return op(NdOp::INT_LESS, {A, B}, 1); }
  NdVar both(NdVar A, NdVar B) { return op(NdOp::BOOL_AND, {A, B}, 1); }
  NdVar either(NdVar A, NdVar B) { return op(NdOp::BOOL_OR, {A, B}, 1); }
  NdVar choose(NdVar C, NdVar A, NdVar B) {
    return op(NdOp::SELECT, {C, A, B}, A.Size);
  }

  void canonical(NdVar V) {
    const auto Shift = k(64 - AddressBits);
    const auto Signed =
        op(NdOp::INT_ASHR, {op(NdOp::INT_LEFT, {V, Shift}), Shift});
    W.rejectIf(ne(V, Signed));
  }

  void range(NdVar Address, uint64_t LastByte) {
    W.rejectIf(less(k(UINT64_MAX - LastByte), Address));
    canonical(Address);
    canonical(op(NdOp::INT_ADD, {Address, k(LastByte)}));
  }

  NdVar readMemory(NdVar Address) {
    auto Value = op(NdOp::LOAD, {Address});
    if (AccessedAddress) {
      const auto Delta = op(NdOp::INT_SUB, {*AccessedAddress, Address});
      // Mask before shifting: a rejected SELECT arm must never introduce a
      // C/LLVM shift outside the value width, even for disjoint addresses.
      const auto Shift = op(NdOp::INT_LEFT, {masked(Delta, 7), k(3)});
      const auto Bit =
          choose(less(Delta, k(8)), op(NdOp::INT_LEFT, {k(1), Shift}), k(0));
      Value = op(NdOp::INT_OR, {Value, Bit});
    }
    return Value;
  }

  struct Descriptor {
    NdVar Address;
    NdVar Value;
  };

  Descriptor descriptor(NdVar Selector) {
    const auto Local = ne(masked(Selector, 4), k(0));
    const auto BadLdt = either(
        ne(masked(load(offsetof(V6, Unusable), 1), 64), k(0)),
        ne(masked(load(offsetof(V6, LDTR) + offsetof(Segment, Attributes), 2),
                  0x9f),
           k(0x82)));
    W.rejectIf(both(Local, BadLdt));
    const auto Base =
        choose(Local, load(offsetof(V6, LDTR)), load(offsetof(V6, GDTR)));
    const auto Limit =
        choose(Local, load(offsetof(V6, LDTR) + offsetof(Segment, Limit), 4),
               load(offsetof(V6, GDTR) + offsetof(Table, Limit), 2));
    const auto Index = masked(Selector, 0xfff8);
    const auto Last = op(NdOp::INT_ADD, {Index, k(7)});
    W.rejectIf(less(Limit, Last));
    W.rejectIf(less(op(NdOp::INT_SUB, {k(UINT64_MAX), Last}), Base));
    const auto Address = op(NdOp::INT_ADD, {Base, Index});
    range(Address, 7);
    return {Address, readMemory(Address)};
  }

  void accessed(Descriptor D) {
    const auto Address = op(NdOp::INT_ADD, {D.Address, k(5)});
    const auto Byte = narrow(op(NdOp::INT_OR, {shifted(D.Value, 40), k(1)}), 1);
    // A descriptor with A=1 may live in read-only RAM. Do not introduce a
    // redundant write that the original instruction would never perform.
    W.storeIf(eq(masked(D.Value, uint64_t{1} << 40), k(0)), Address, Byte);
  }

  void segment(uint64_t Offset, NdVar Selector, NdVar Descriptor,
               std::optional<NdVar> KeepCache = std::nullopt) {
    auto Base = op(NdOp::INT_OR, {masked(shifted(Descriptor, 16), 0xffffff),
                                  masked(shifted(Descriptor, 32), 0xff000000)});
    auto Limit = op(NdOp::INT_OR, {masked(Descriptor, 0xffff),
                                   masked(shifted(Descriptor, 32), 0xf0000)});
    Limit =
        choose(ne(masked(Descriptor, uint64_t{1} << 55), k(0)),
               op(NdOp::INT_OR, {op(NdOp::INT_LEFT, {Limit, k(12)}), k(0xfff)}),
               Limit);
    auto Attributes =
        op(NdOp::INT_OR, {masked(shifted(Descriptor, 40), 0xf0ff), k(1)});
    if (KeepCache) {
      Base = choose(*KeepCache, load(Offset + offsetof(Segment, Base)), Base);
      Limit =
          choose(*KeepCache, load(Offset + offsetof(Segment, Limit), 4), Limit);
      Attributes =
          choose(*KeepCache, load(Offset + offsetof(Segment, Attributes), 2),
                 Attributes);
    }
    W.store(Offset + offsetof(Segment, Base), Base);
    W.store(Offset + offsetof(Segment, Limit), narrow(Limit, 4));
    W.store(Offset + offsetof(Segment, Selector), narrow(Selector, 2));
    W.store(Offset + offsetof(Segment, Attributes), narrow(Attributes, 2));
  }

  NdVar returnedData(uint64_t Offset, bool Base, unsigned Bit, NdVar CPL,
                     NdVar Unusable) {
    const auto SelectorOffset = Offset + (Base ? offsetof(Segment, Selector)
                                               : offsetof(Metadata, Selector));
    const auto AttributeOffset =
        Offset +
        (Base ? offsetof(Segment, Attributes) : offsetof(Metadata, Attributes));
    const auto Selector = load(SelectorOffset, 2);
    const auto Attributes = load(AttributeOffset, 2);
    const auto Invalidate = both(
        ne(CPL, k(0)), either(eq(masked(Selector, 0xfffc), k(0)),
                              both(less(masked(shifted(Attributes, 5), 3), CPL),
                                   ne(masked(Attributes, 12), k(12)))));
    W.store(SelectorOffset, narrow(choose(Invalidate, k(0), Selector), 2));
    return op(NdOp::INT_OR, {Unusable, choose(Invalidate, k(Bit), k(0))});
  }

public:
  X64ContinuationEvents(Writer &Output, unsigned Bits)
      : W(Output), AddressBits(Bits) {}

  void iret() {
    W.rejectIf(ne(masked(load(offsetof(State, RFlags)), 0x4000), k(0)));
    const auto SP = load(offsetof(State, GPR) + x86reg::RSP);
    range(SP, 39);
    const auto PC = readMemory(SP);
    const auto CS = masked(readMemory(op(NdOp::INT_ADD, {SP, k(8)})), 0xffff);
    const auto Flags = readMemory(op(NdOp::INT_ADD, {SP, k(16)}));
    canonical(PC);
    W.rejectIf(eq(masked(CS, 0xfffc), k(0)));
    const auto CPL = masked(CS, 3);
    const auto Code = descriptor(CS);
    const auto Access = shifted(Code.Value, 40);
    const auto DPL = masked(shifted(Access, 5), 3);
    W.rejectIf(ne(masked(Access, 0x6098), k(0x2098)));
    W.rejectIf(
        choose(ne(masked(Access, 4), k(0)), less(CPL, DPL), ne(CPL, DPL)));
    AccessedAddress = op(NdOp::INT_ADD, {Code.Address, k(5)});
    const auto NewSP = readMemory(op(NdOp::INT_ADD, {SP, k(24)}));
    const auto SS = masked(readMemory(op(NdOp::INT_ADD, {SP, k(32)})), 0xffff);
    W.rejectIf(ne(masked(SS, 3), CPL));
    const auto NullSS = eq(masked(SS, 0xfffc), k(0));
    W.rejectIf(both(NullSS, eq(CPL, k(3))));
    // A null SS requires no table access. The already validated CS provides
    // a safe speculative read; its accessed bit is already pending. It cannot
    // acquire another write, and sequential RAM makes the read unobservable.
    const auto Stack = descriptor(choose(NullSS, CS, SS));
    const auto StackAccess = masked(shifted(Stack.Value, 40), 0xfa);
    const auto Expected =
        op(NdOp::INT_OR, {k(0x92), op(NdOp::INT_LEFT, {CPL, k(5)})});
    W.rejectIf(both(ne(masked(SS, 0xfffc), k(0)), ne(StackAccess, Expected)));
    accessed(Code);
    accessed(Stack);
    segment(offsetof(V5, CS), CS, Code.Value);
    segment(offsetof(V5, SS), SS, Stack.Value, NullSS);
    auto Unusable =
        op(NdOp::INT_OR, {masked(load(offsetof(V6, Unusable), 1), 0xfc),
                          choose(NullSS, k(2), k(0))});
    Unusable = returnedData(offsetof(V6, DS), true, 4, CPL, Unusable);
    Unusable = returnedData(offsetof(V6, ES), true, 8, CPL, Unusable);
    Unusable = returnedData(offsetof(V6, FS), false, 16, CPL, Unusable);
    Unusable = returnedData(offsetof(V6, GS), false, 32, CPL, Unusable);
    W.store(offsetof(V6, Unusable), narrow(Unusable, 1));
    W.store(offsetof(V6, NMIBlocked), NdVar::scalar(0, 1));
    W.store(offsetof(State, GPR) + x86reg::RSP, NewSP);
    W.store(offsetof(State, RFlags),
            op(NdOp::INT_OR, {masked(Flags, 0x3d7fd7), k(2)}));
    W.leave(Exit::PrivilegeReturn, PC);
  }

  void softwareInterrupt(uint8_t Vector, uint64_t NextPC) {
    const uint64_t GateOffset = uint64_t{Vector} * 16;
    const uint64_t LastByte = GateOffset + 15;
    const auto IDT = load(offsetof(V6, IDTR));
    W.rejectIf(less(load(offsetof(V6, IDTR) + offsetof(Table, Limit), 2),
                    k(LastByte)));
    W.rejectIf(less(k(UINT64_MAX - LastByte), IDT));
    const auto GateAddress = op(NdOp::INT_ADD, {IDT, k(GateOffset)});
    range(GateAddress, 15);
    const auto Low = readMemory(GateAddress);
    const auto High = readMemory(op(NdOp::INT_ADD, {GateAddress, k(8)}));
    const auto Type = masked(shifted(Low, 40), 31);
    W.rejectIf(both(ne(Type, k(14)), ne(Type, k(15))));
    W.rejectIf(eq(masked(Low, uint64_t{1} << 47), k(0)));
    W.rejectIf(ne(masked(Low, uint64_t{31} << 35), k(0)));
    W.rejectIf(ne(shifted(High, 32), k(0)));
    const auto CS = masked(shifted(Low, 16), 0xffff);
    const auto PC =
        op(NdOp::INT_OR,
           {op(NdOp::INT_OR,
               {masked(Low, 0xffff), masked(shifted(Low, 32), 0xffff0000)}),
            op(NdOp::INT_LEFT, {High, k(32)})});
    canonical(PC);
    W.rejectIf(eq(masked(CS, 0xfffc), k(0)));
    const auto Code = descriptor(CS);
    W.rejectIf(ne(masked(shifted(Code.Value, 40), 0x60f8), k(0x2098)));
    AccessedAddress = op(NdOp::INT_ADD, {Code.Address, k(5)});
    const auto IST = masked(shifted(Low, 32), 7);
    const auto UseIST = ne(IST, k(0));
    const auto TssOffset =
        op(NdOp::INT_ADD, {k(28), op(NdOp::INT_LEFT, {IST, k(3)})});
    const auto Last = op(NdOp::INT_ADD, {TssOffset, k(7)});
    const auto TR = load(offsetof(V6, TR));
    const auto BadTR = either(
        ne(masked(load(offsetof(V6, Unusable), 1), 128), k(0)),
        ne(masked(load(offsetof(V6, TR) + offsetof(Segment, Attributes), 2),
                  0x9f),
           k(0x8b)));
    W.rejectIf(both(UseIST, BadTR));
    W.rejectIf(
        both(UseIST,
             less(load(offsetof(V6, TR) + offsetof(Segment, Limit), 4), Last)));
    W.rejectIf(
        both(UseIST, less(op(NdOp::INT_SUB, {k(UINT64_MAX), Last}), TR)));
    const auto Address =
        choose(UseIST, op(NdOp::INT_ADD, {TR, TssOffset}), Code.Address);
    range(Address, 7);
    const auto OldSP = load(offsetof(State, GPR) + x86reg::RSP);
    const auto Stack = choose(UseIST, readMemory(Address), OldSP);
    canonical(Stack);
    const auto Top = masked(Stack, ~uint64_t{15});
    W.rejectIf(less(Top, k(40)));
    const auto SP = op(NdOp::INT_SUB, {Top, k(40)});
    range(SP, 39);
    const auto Flags = load(offsetof(State, RFlags));
    const NdVar Frame[] = {
        k(NextPC), load(offsetof(V5, CS) + offsetof(Segment, Selector), 2),
        masked(Flags, ~uint64_t{0x10000}), OldSP,
        load(offsetof(V5, SS) + offsetof(Segment, Selector), 2)};
    accessed(Code);
    for (unsigned I = 5; I--;) {
      const auto Destination = op(NdOp::INT_ADD, {SP, k(I * 8)});
      W.emit(NdOp::STORE, {}, {Destination, Frame[I]});
    }
    segment(offsetof(V5, CS), masked(CS, 0xfffc), Code.Value);
    W.store(offsetof(V6, Unusable),
            narrow(masked(load(offsetof(V6, Unusable), 1), 0xfe), 1));
    W.store(offsetof(State, GPR) + x86reg::RSP, SP);
    const auto Clear = choose(eq(Type, k(14)), k(0x34300), k(0x34100));
    W.store(
        offsetof(State, RFlags),
        op(NdOp::INT_AND, {Flags, op(NdOp::INT_XOR, {Clear, k(UINT64_MAX)})}));
    W.leave(Exit::Exception, PC);
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONEVENTS_H
