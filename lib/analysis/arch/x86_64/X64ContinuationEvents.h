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

#include "X64ContinuationSegments.h"
#include "X64FarPointerEncoding.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

namespace neverd::analysis::detail {

/// Writer supplies
/// temporary/emit/load/store/storeIf/write/rejectIf/exitIf/leave. Keeping this
/// lowering above both C backends gives one owner for descriptor validation,
/// frame accesses and final state. All checks precede writes. A pending CS
/// accessed-bit update is forwarded into later reads: eager frame loads or
/// committing descriptors only after frame stores would change aliased RAM.
template <class Writer>
class X64ContinuationEvents : private X64ContinuationSegments<Writer> {
  using Base = X64ContinuationSegments<Writer>;
  using Base::accessed;
  using Base::AccessedAddress;
  using Base::AddressBits;
  using Base::both;
  using Base::canonical;
  using Base::choose;
  using Base::descriptor;
  using Base::either;
  using Base::eq;
  using Base::k;
  using Base::less;
  using Base::load;
  using Base::masked;
  using Base::narrow;
  using Base::ne;
  using Base::op;
  using Base::range;
  using Base::readMemory;
  using Base::returnCode;
  using Base::returnData;
  using Base::returnedData;
  using Base::returnStack;
  using Base::segment;
  using Base::segmentAttributes;
  using Base::segmentBase;
  using Base::segmentLimit;
  using Base::shifted;
  using Base::W;
  using typename Base::Exit;
  using typename Base::Metadata;
  using typename Base::Segment;
  using typename Base::State;
  using typename Base::Table;
  using typename Base::V5;
  using typename Base::V6;

  NdVar compatibilityStack(NdVar OldSP, NdVar NewSP, NdVar Attributes,
                           uint16_t Discard = 0) {
    // A 16-bit target stack replaces only SP, retaining the old ESP[31:16].
    // Neither frame pops nor RETF parameter cleanup may carry into those bits.
    const auto WideStack = ne(masked(Attributes, 0x4000), k(0));
    const auto Mask = choose(WideStack, k(UINT32_MAX), k(UINT16_MAX));
    const auto Updated =
        op(NdOp::INT_AND, {op(NdOp::INT_ADD, {NewSP, k(Discard)}), Mask});
    return op(NdOp::INT_OR,
              {Updated, choose(WideStack, k(0), masked(OldSP, 0xffff0000))});
  }

public:
  X64ContinuationEvents(Writer &Output, unsigned Bits) : Base(Output, Bits) {}

  void farPointerLoad(const X64FarPointerLoad &F, uint64_t NextPC,
                      std::optional<X64NullSegmentBasePolicy> NullBasePolicy) {
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
    const bool SS = F.DestinationSegment == X64FarPointerSegment::SS;
    const bool GS = F.DestinationSegment == X64FarPointerSegment::GS;
    const uint64_t Offset = SS   ? offsetof(V5, SS)
                            : GS ? offsetof(V6, GS)
                                 : offsetof(V6, FS);
    const uint64_t SelectorOffset =
        Offset +
        (SS ? offsetof(Segment, Selector) : offsetof(Metadata, Selector));
    const uint64_t BaseOffset =
        GS ? offsetof(State, GSBase) : offsetof(State, FSBase);
    const unsigned UnusableBit = SS ? 2 : GS ? 32 : 16;
    const auto Null = eq(masked(Selector, 0xfffc), k(0));
    if (SS || NullBasePolicy) {
      // CPL0 permits only null SS selector 0. A null load never consults the
      // GDT/LDT, even when their addresses are unmapped or noncanonical.
      if (SS)
        W.rejectIf(both(Null, ne(Selector, k(0))));
      W.exitIf(Null, [&] {
        if (!SS && *NullBasePolicy == X64NullSegmentBasePolicy::Clear)
          W.store(BaseOffset, k(0));
        W.store(SelectorOffset, narrow(Selector, 2));
        W.store(offsetof(V6, Unusable),
                narrow(op(NdOp::INT_OR,
                          {load(offsetof(V6, Unusable), 1), k(UnusableBit)}),
                       1));
        W.write(NdVar::reg(F.Destination * 8, F.Bytes), Value);
        W.leave(Exit::Continue, k(NextPC));
      });
    } else {
      W.rejectIf(Null);
    }
    const auto D = descriptor(Selector);
    const auto Access = shifted(D.Value, 40);
    if (SS) {
      W.rejectIf(ne(masked(Selector, 3), k(0)));
      // Present writable data, DPL=CPL=0; expand-down and A are independent.
      W.rejectIf(ne(masked(Access, 0xfa), k(0x92)));
    } else {
      W.rejectIf(ne(masked(Access, 0x90), k(0x90)));
      W.rejectIf(eq(masked(Access, 0x0a), k(8))); // Execute-only code.
      const auto DPL = masked(shifted(Access, 5), 3);
      W.rejectIf(
          both(ne(masked(Access, 12), k(12)), less(DPL, masked(Selector, 3))));
    }
    // Both pointer fields are captured before the descriptor's A-bit write.
    // In particular the loaded offset can alias that very descriptor byte.
    accessed(D);
    if (SS) {
      segment(Offset, Selector, D.Value);
    } else {
      W.store(BaseOffset, segmentBase(D.Value));
      W.store(Offset + offsetof(Metadata, Limit),
              narrow(segmentLimit(D.Value), 4));
      W.store(SelectorOffset, narrow(Selector, 2));
      W.store(Offset + offsetof(Metadata, Attributes),
              narrow(segmentAttributes(D.Value), 2));
    }
    W.store(
        offsetof(V6, Unusable),
        narrow(masked(load(offsetof(V6, Unusable), 1), 0xff ^ UnusableBit), 1));
    W.write(NdVar::reg(F.Destination * 8, F.Bytes), Value);
    W.leave(Exit::Continue, k(NextPC));
  }

  std::vector<X64ContinuationCompatibilityChoice> iret() {
    W.rejectIf(ne(masked(load(offsetof(State, RFlags)), 0x4000), k(0)));
    const auto SP = load(offsetof(State, GPR) + x86reg::RSP);
    range(SP, 39);
    const auto PC = readMemory(SP);
    const auto CS = masked(readMemory(op(NdOp::INT_ADD, {SP, k(8)})), 0xffff);
    const auto Flags = readMemory(op(NdOp::INT_ADD, {SP, k(16)}));
    canonical(PC);
    const auto CPL = masked(CS, 3);
    const auto Code = returnCode(CS, CPL);
    const auto Compatibility = eq(masked(Code.Value, uint64_t{1} << 53), k(0));
    const auto TargetPC = choose(Compatibility, masked(PC, UINT32_MAX), PC);
    W.rejectIf(both(Compatibility, less(segmentLimit(Code.Value), TargetPC)));
    const auto NewSP = readMemory(op(NdOp::INT_ADD, {SP, k(24)}));
    const auto SS = masked(readMemory(op(NdOp::INT_ADD, {SP, k(32)})), 0xffff);
    const auto NullSS = eq(masked(SS, 0xfffc), k(0));
    W.rejectIf(both(Compatibility, NullSS));
    const auto Stack = returnStack(CS, CPL, SS, NullSS);
    const auto CompatSP =
        compatibilityStack(SP, NewSP, segmentAttributes(Stack.Value));
    accessed(Code);
    accessed(Stack);
    segment(offsetof(V5, CS), CS, Code.Value);
    segment(offsetof(V5, SS), SS, Stack.Value, NullSS);
    const auto Unusable =
        op(NdOp::INT_OR, {masked(load(offsetof(V6, Unusable), 1), 0xfc),
                          choose(NullSS, k(2), k(0))});
    returnData(CPL, Unusable);
    W.store(offsetof(V6, NMIBlocked), NdVar::scalar(0, 1));
    W.store(offsetof(State, GPR) + x86reg::RSP,
            choose(Compatibility, CompatSP, NewSP));
    W.store(offsetof(State, RFlags),
            op(NdOp::INT_OR, {masked(Flags, 0x3d7fd7), k(2)}));
    W.invalidateMonitor();
    const auto Choices = x64CompatibilityChoices(false);
    W.exitIf(Compatibility, [&] {
      W.compatibilityChoices(Choices);
      W.leave(Exit::PrivilegeReturn, TargetPC);
    });
    W.leave(Exit::PrivilegeReturn, PC);
    return Choices;
  }

  std::vector<X64ContinuationCompatibilityChoice> farReturn(uint16_t Discard) {
    const auto SP = load(offsetof(State, GPR) + x86reg::RSP);
    range(SP, 15);
    const auto PC = readMemory(SP);
    const auto CS = masked(readMemory(op(NdOp::INT_ADD, {SP, k(8)})), 0xffff);
    canonical(PC);
    const auto CPL = masked(CS, 3);
    const auto Outer = ne(CPL, k(0));
    const auto Code = returnCode(CS, CPL);
    const auto Compatibility = eq(masked(Code.Value, uint64_t{1} << 53), k(0));
    const auto TargetPC = choose(Compatibility, masked(PC, UINT32_MAX), PC);
    W.rejectIf(both(Compatibility, less(segmentLimit(Code.Value), TargetPC)));
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
    W.rejectIf(both(Compatibility, both(Outer, NullSS)));
    const auto Stack = returnStack(CS, CPL, SS, NullSS);
    const auto StackAttributes =
        choose(Outer, segmentAttributes(Stack.Value),
               load(offsetof(V5, SS) + offsetof(Segment, Attributes), 2));
    const auto CompatSP =
        compatibilityStack(SP, NewSP, StackAttributes, Discard);
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
            choose(Compatibility, CompatSP,
                   op(NdOp::INT_ADD, {NewSP, k(Discard)})));
    // RETFQ preserves flags and NMI blocking in the admitted entry domain.
    W.invalidateMonitor(Outer);
    const auto Choices = x64CompatibilityChoices(false);
    W.exitIf(Compatibility, [&] {
      W.compatibilityChoices(Choices);
      W.leave(Exit::PrivilegeReturn, TargetPC);
    });
    W.leave(Exit::PrivilegeReturn, PC);
    return Choices;
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
