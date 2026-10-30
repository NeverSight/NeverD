//===- X64ContinuationEvents.h - Explicit long-mode events -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Segment loads, far returns and interrupts through the continuation writer.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONEVENTS_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONEVENTS_H

#include "X64FarPointerEncoding.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace neverd::analysis::detail {

/// Writer supplies temporary/emit/load/store/storeIf/write/rejectIf/leave.
/// Keeping this lowering above both C backends gives one owner for descriptor
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

  void range(NdVar Address, NdVar LastByte) {
    W.rejectIf(less(op(NdOp::INT_SUB, {k(UINT64_MAX), LastByte}), Address));
    canonical(Address);
    canonical(op(NdOp::INT_ADD, {Address, LastByte}));
  }

  void range(NdVar Address, uint64_t LastByte) { range(Address, k(LastByte)); }

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

  NdVar segmentBase(NdVar Descriptor) {
    return op(NdOp::INT_OR, {masked(shifted(Descriptor, 16), 0xffffff),
                             masked(shifted(Descriptor, 32), 0xff000000)});
  }

  NdVar segmentLimit(NdVar Descriptor) {
    auto Limit = op(NdOp::INT_OR, {masked(Descriptor, 0xffff),
                                   masked(shifted(Descriptor, 32), 0xf0000)});
    Limit =
        choose(ne(masked(Descriptor, uint64_t{1} << 55), k(0)),
               op(NdOp::INT_OR, {op(NdOp::INT_LEFT, {Limit, k(12)}), k(0xfff)}),
               Limit);
    return Limit;
  }

  NdVar segmentAttributes(NdVar Descriptor) {
    return op(NdOp::INT_OR, {masked(shifted(Descriptor, 40), 0xf0ff), k(1)});
  }

  void segment(uint64_t Offset, NdVar Selector, NdVar Descriptor,
               std::optional<NdVar> KeepCache = std::nullopt) {
    auto Base = segmentBase(Descriptor);
    auto Limit = segmentLimit(Descriptor);
    auto Attributes = segmentAttributes(Descriptor);
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

  Descriptor returnCode(NdVar CS, NdVar CPL) {
    W.rejectIf(eq(masked(CS, 0xfffc), k(0)));
    const auto Code = descriptor(CS);
    const auto Access = shifted(Code.Value, 40);
    const auto DPL = masked(shifted(Access, 5), 3);
    W.rejectIf(ne(masked(Access, 0x6098), k(0x2098)));
    W.rejectIf(
        choose(ne(masked(Access, 4), k(0)), less(CPL, DPL), ne(CPL, DPL)));
    AccessedAddress = op(NdOp::INT_ADD, {Code.Address, k(5)});
    return Code;
  }

  Descriptor returnStack(NdVar CS, NdVar CPL, NdVar SS, NdVar NullSS) {
    W.rejectIf(ne(masked(SS, 3), CPL));
    W.rejectIf(both(NullSS, eq(CPL, k(3))));
    // A null SS requires no table access. The already validated CS provides
    // a safe speculative read; its accessed bit is already pending. It cannot
    // acquire another write, and sequential RAM makes the read unobservable.
    const auto Stack = descriptor(choose(NullSS, CS, SS));
    const auto StackAccess = masked(shifted(Stack.Value, 40), 0xfa);
    const auto Expected =
        op(NdOp::INT_OR, {k(0x92), op(NdOp::INT_LEFT, {CPL, k(5)})});
    W.rejectIf(both(ne(masked(SS, 0xfffc), k(0)), ne(StackAccess, Expected)));
    return Stack;
  }

  void returnData(NdVar CPL, NdVar Unusable) {
    Unusable = returnedData(offsetof(V6, DS), true, 4, CPL, Unusable);
    Unusable = returnedData(offsetof(V6, ES), true, 8, CPL, Unusable);
    Unusable = returnedData(offsetof(V6, FS), false, 16, CPL, Unusable);
    Unusable = returnedData(offsetof(V6, GS), false, 32, CPL, Unusable);
    W.store(offsetof(V6, Unusable), narrow(Unusable, 1));
  }

public:
  X64ContinuationEvents(Writer &Output, unsigned Bits)
      : W(Output), AddressBits(Bits) {}

  void farPointerLoad(const X64FarPointerLoad &F, uint64_t NextPC) {
    auto Address = k(uint64_t(F.Displacement));
    if (F.Relative)
      Address = op(NdOp::INT_ADD, {Address, k(NextPC)});
    if (F.Base)
      Address = op(NdOp::INT_ADD,
                   {Address, load(offsetof(State, GPR) + *F.Base * 8)});
    if (F.Index)
      Address =
          op(NdOp::INT_ADD,
             {Address,
              op(NdOp::INT_MULT,
                 {load(offsetof(State, GPR) + *F.Index * 8), k(F.Scale)})});
    if (F.AddressBytes == 4)
      Address = masked(Address, UINT32_MAX);
    auto SelectorAddress = op(NdOp::INT_ADD, {Address, k(F.Bytes)});
    if (F.AddressBytes == 4)
      SelectorAddress = masked(SelectorAddress, UINT32_MAX);
    // Capture the old segment base and source registers before publishing
    // either destination. Each field's effective address wraps independently
    // before segment addition; an addr32 selector can be 4 GiB away from the
    // end of the offset field in linear memory.
    if (F.Segment) {
      const auto Base = load(F.Segment == 1 ? offsetof(State, FSBase)
                                            : offsetof(State, GSBase));
      Address = op(NdOp::INT_ADD, {Address, Base});
      SelectorAddress = op(NdOp::INT_ADD, {SelectorAddress, Base});
    }
    range(Address, F.Bytes - 1);
    range(SelectorAddress, 1);
    const auto Value = op(NdOp::LOAD, {Address}, F.Bytes);
    const auto Selector =
        op(NdOp::INT_ZEXT, {op(NdOp::LOAD, {SelectorAddress}, 2)});
    // Null-selector base updates differ between processor families. V6 has
    // no CPU-specific null-load policy, so reject before descriptor access or
    // any write, rather than guessing whether a live 64-bit base survives.
    W.rejectIf(eq(masked(Selector, 0xfffc), k(0)));
    const auto D = descriptor(Selector);
    const auto Access = shifted(D.Value, 40);
    W.rejectIf(ne(masked(Access, 0x90), k(0x90)));
    W.rejectIf(eq(masked(Access, 0x0a), k(8))); // Execute-only code.
    const auto DPL = masked(shifted(Access, 5), 3);
    W.rejectIf(
        both(ne(masked(Access, 12), k(12)), less(DPL, masked(Selector, 3))));
    // Both pointer fields are captured before the descriptor's A-bit write.
    // In particular the loaded offset can alias that very descriptor byte.
    accessed(D);
    const uint64_t Offset = F.GS ? offsetof(V6, GS) : offsetof(V6, FS);
    W.store(F.GS ? offsetof(State, GSBase) : offsetof(State, FSBase),
            segmentBase(D.Value));
    W.store(Offset + offsetof(Metadata, Limit),
            narrow(segmentLimit(D.Value), 4));
    W.store(Offset + offsetof(Metadata, Selector), narrow(Selector, 2));
    W.store(Offset + offsetof(Metadata, Attributes),
            narrow(segmentAttributes(D.Value), 2));
    W.store(
        offsetof(V6, Unusable),
        narrow(masked(load(offsetof(V6, Unusable), 1), F.GS ? 0xdf : 0xef), 1));
    W.write(NdVar::reg(F.Destination * 8, F.Bytes), Value);
    W.leave(Exit::Continue, k(NextPC));
  }

  void iret() {
    W.rejectIf(ne(masked(load(offsetof(State, RFlags)), 0x4000), k(0)));
    const auto SP = load(offsetof(State, GPR) + x86reg::RSP);
    range(SP, 39);
    const auto PC = readMemory(SP);
    const auto CS = masked(readMemory(op(NdOp::INT_ADD, {SP, k(8)})), 0xffff);
    const auto Flags = readMemory(op(NdOp::INT_ADD, {SP, k(16)}));
    canonical(PC);
    const auto CPL = masked(CS, 3);
    const auto Code = returnCode(CS, CPL);
    const auto NewSP = readMemory(op(NdOp::INT_ADD, {SP, k(24)}));
    const auto SS = masked(readMemory(op(NdOp::INT_ADD, {SP, k(32)})), 0xffff);
    const auto NullSS = eq(masked(SS, 0xfffc), k(0));
    const auto Stack = returnStack(CS, CPL, SS, NullSS);
    accessed(Code);
    accessed(Stack);
    segment(offsetof(V5, CS), CS, Code.Value);
    segment(offsetof(V5, SS), SS, Stack.Value, NullSS);
    const auto Unusable =
        op(NdOp::INT_OR, {masked(load(offsetof(V6, Unusable), 1), 0xfc),
                          choose(NullSS, k(2), k(0))});
    returnData(CPL, Unusable);
    W.store(offsetof(V6, NMIBlocked), NdVar::scalar(0, 1));
    W.store(offsetof(State, GPR) + x86reg::RSP, NewSP);
    W.store(offsetof(State, RFlags),
            op(NdOp::INT_OR, {masked(Flags, 0x3d7fd7), k(2)}));
    W.leave(Exit::PrivilegeReturn, PC);
  }

  void farReturn(uint16_t Discard) {
    const auto SP = load(offsetof(State, GPR) + x86reg::RSP);
    range(SP, 15);
    const auto PC = readMemory(SP);
    const auto CS = masked(readMemory(op(NdOp::INT_ADD, {SP, k(8)})), 0xffff);
    canonical(PC);
    const auto CPL = masked(CS, 3);
    const auto Outer = ne(CPL, k(0));
    const auto Code = returnCode(CS, CPL);
    range(SP, choose(Outer, k(31 + Discard), k(15)));
    // Same-privilege RETFQ has only two frame words. Use already validated
    // RAM for the unused SELECT arm; never touch an absent saved SS:RSP.
    const auto Frame =
        choose(Outer, op(NdOp::INT_ADD, {SP, k(16 + Discard)}), SP);
    const auto NewSP =
        choose(Outer, readMemory(Frame), op(NdOp::INT_ADD, {SP, k(16)}));
    const auto SS = choose(
        Outer, masked(readMemory(op(NdOp::INT_ADD, {Frame, k(8)})), 0xffff),
        k(0));
    const auto NullSS = eq(masked(SS, 0xfffc), k(0));
    const auto Stack = returnStack(CS, CPL, SS, NullSS);
    accessed(Code);
    accessed(Stack);
    segment(offsetof(V5, CS), CS, Code.Value);
    segment(offsetof(V5, SS),
            choose(Outer, SS,
                   load(offsetof(V5, SS) + offsetof(Segment, Selector), 2)),
            Stack.Value, NullSS);
    const auto Unusable = masked(load(offsetof(V6, Unusable), 1), 0xfe);
    returnData(CPL, choose(Outer,
                           op(NdOp::INT_OR, {masked(Unusable, 0xfd),
                                             choose(NullSS, k(2), k(0))}),
                           Unusable));
    W.store(offsetof(State, GPR) + x86reg::RSP,
            op(NdOp::INT_ADD, {NewSP, k(Discard)}));
    // RETFQ preserves flags and NMI blocking in the admitted entry domain.
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
