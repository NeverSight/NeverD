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
/// frame accesses and final state. The no-fault writer checks before writes
/// and forwards pending CS accessed bits into later reads. The faultable
/// writer commits descriptor updates before later accesses. Both preserve
/// descriptor/frame aliasing and retain the original access order.
template <class Writer>
class X64ContinuationEvents : private X64ContinuationSegments<Writer> {
  using Base = X64ContinuationSegments<Writer>;
  using Base::accessed;
  using Base::AccessedAddress;
  using Base::AddressBits;
  using Base::both;
  using Base::canonical;
  using Base::choose;
  using Base::DeliveryExternal;
  using Base::descriptor;
  using Base::either;
  using Base::eq;
  using Base::faultIf;
  using Base::InstructionDescriptors;
  using Base::interruptReturnFlags;
  using Base::k;
  using Base::less;
  using Base::load;
  using Base::masked;
  using Base::narrow;
  using Base::ne;
  using Base::op;
  using Base::range;
  using Base::readMemory;
  using Base::readStackMemory;
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
    if constexpr (Writer::FaultableMemory) {
      DeliveryExternal = k(0);
      InstructionDescriptors = true;
    }
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
    if constexpr (!Writer::FaultableMemory) {
      range(Address, F.Bytes - 1);
      range(SelectorAddress, 1);
    } else {
      // Only SP/BP select the default stack segment. R12/R13 use DS despite
      // sharing their low encoding bits; FS/GS overrides select data access.
      const bool Stack = !F.Segment && (F.Base == 4 || F.Base == 5);
      W.instructionAccess(true, Stack);
    }
    const auto Value = op(NdOp::LOAD, {Address}, F.Bytes);
    const auto Selector =
        op(NdOp::INT_ZEXT, {op(NdOp::LOAD, {SelectorAddress}, 2)});
    if constexpr (Writer::FaultableMemory)
      W.instructionAccess(false);
    const auto Register = F.DestinationSegment == X64FarPointerSegment::SS
                              ? X86SegmentRegister::SS
                          : F.DestinationSegment == X64FarPointerSegment::GS
                              ? X86SegmentRegister::GS
                              : X86SegmentRegister::FS;
    this->loadDataSegment(Register, Selector, NullBasePolicy, [&] {
      W.write(NdVar::reg(F.Destination * 8, F.Bytes), Value);
      W.leave(Exit::Continue, k(NextPC));
    });
  }

  std::vector<X64ContinuationCompatibilityChoice> iret(uint16_t Bytes) {
    if constexpr (Writer::FaultableMemory) {
      DeliveryExternal = k(0);
      InstructionDescriptors = true;
      W.beginReturn();
    }
    faultIf(ne(masked(load(offsetof(State, RFlags)), 0x4000), k(0)), 13, k(0));
    const auto SP = load(offsetof(State, GPR) + x86reg::RSP);
    if constexpr (!Writer::FaultableMemory)
      range(SP, 5 * Bytes - 1);
    const auto ReadFrame = [&](NdVar Address) {
      return readStackMemory(Address, Bytes);
    };
    NdVar PC, CS, Flags;
    if constexpr (Writer::FaultableMemory) {
      Flags = ReadFrame(op(NdOp::INT_ADD, {SP, k(2 * Bytes)}));
      CS = masked(ReadFrame(op(NdOp::INT_ADD, {SP, k(Bytes)})), 0xffff);
      PC = ReadFrame(SP);
    } else {
      PC = ReadFrame(SP);
      CS = masked(ReadFrame(op(NdOp::INT_ADD, {SP, k(Bytes)})), 0xffff);
      Flags = ReadFrame(op(NdOp::INT_ADD, {SP, k(2 * Bytes)}));
    }
    if constexpr (!Writer::FaultableMemory)
      canonical(PC);
    const auto CPL = masked(CS, 3);
    std::optional<NdVar> OldCPL;
    if constexpr (Writer::FaultableMemory) {
      OldCPL =
          masked(load(offsetof(V5, CS) + offsetof(Segment, Selector), 2), 3);
    }
    const auto Code = returnCode(CS, OldCPL);
    if constexpr (Writer::FaultableMemory)
      accessed(Code);
    const auto Compatibility = eq(masked(Code.Value, uint64_t{1} << 53), k(0));
    const auto TargetPC = choose(Compatibility, masked(PC, UINT32_MAX), PC);
    if constexpr (!Writer::FaultableMemory)
      W.rejectIf(both(Compatibility, less(segmentLimit(Code.Value), TargetPC)));
    NdVar NewSP, SS;
    if constexpr (Writer::FaultableMemory) {
      SS = masked(ReadFrame(op(NdOp::INT_ADD, {SP, k(4 * Bytes)})), 0xffff);
      NewSP = ReadFrame(op(NdOp::INT_ADD, {SP, k(3 * Bytes)}));
    } else {
      NewSP = ReadFrame(op(NdOp::INT_ADD, {SP, k(3 * Bytes)}));
      SS = masked(ReadFrame(op(NdOp::INT_ADD, {SP, k(4 * Bytes)})), 0xffff);
    }
    const auto NullSS = eq(masked(SS, 0xfffc), k(0));
    faultIf(both(Compatibility, NullSS), 13, k(0));
    const auto Stack = returnStack(CS, CPL, SS, NullSS);
    const auto CompatSP =
        compatibilityStack(SP, NewSP, segmentAttributes(Stack.Value));
    const auto NewFlags = interruptReturnFlags(
        load(offsetof(State, RFlags)), Flags, OldCPL.value_or(k(0)), Bytes);
    if constexpr (!Writer::FaultableMemory)
      accessed(Code);
    accessed(Stack);
    if constexpr (Writer::FaultableMemory) {
      faultIf(both(Compatibility, less(segmentLimit(Code.Value), TargetPC)), 13,
              k(0));
      canonical(TargetPC);
    }
    segment(offsetof(V5, CS), CS, Code.Value);
    segment(offsetof(V5, SS), SS, Stack.Value, NullSS);
    const auto Unusable =
        op(NdOp::INT_OR, {masked(load(offsetof(V6, Unusable), 1), 0xfc),
                          choose(NullSS, k(2), k(0))});
    if constexpr (Writer::FaultableMemory) {
      W.store(offsetof(V6, Unusable), narrow(Unusable, 1));
      // Same-CPL IRET preserves the data caches, including caches inherited
      // from a fast system transfer. Only an outer return revalidates them.
      W.exitIf(less(*OldCPL, CPL), [&] { returnData(CPL, Unusable); });
    } else {
      returnData(CPL, Unusable);
    }
    W.store(offsetof(V6, NMIBlocked), NdVar::scalar(0, 1));
    W.store(offsetof(State, GPR) + x86reg::RSP,
            choose(Compatibility, CompatSP, NewSP));
    W.store(offsetof(State, RFlags), NewFlags);
    W.invalidateMonitor();
    const auto Choices = x64CompatibilityChoices(false);
    W.exitIf(Compatibility, [&] {
      W.compatibilityChoices(Choices);
      W.leave(Exit::PrivilegeReturn, TargetPC);
    });
    W.leave(Exit::PrivilegeReturn, PC);
    return Choices;
  }

  std::vector<X64ContinuationCompatibilityChoice>
  farReturn(uint16_t Bytes, uint16_t Discard,
            bool TruncateCompatibilityIP = false) {
    if constexpr (Writer::FaultableMemory) {
      DeliveryExternal = k(0);
      InstructionDescriptors = true;
    }
    const auto SP = load(offsetof(State, GPR) + x86reg::RSP);
    NdVar PC, CS;
    if constexpr (Writer::FaultableMemory) {
      CS = masked(readStackMemory(op(NdOp::INT_ADD, {SP, k(Bytes)}), Bytes),
                  0xffff);
      PC = readStackMemory(SP, Bytes);
    } else {
      range(SP, 2 * Bytes - 1);
      PC = readMemory(SP, Bytes);
      CS = masked(readMemory(op(NdOp::INT_ADD, {SP, k(Bytes)}), Bytes), 0xffff);
      canonical(PC);
    }
    const auto CPL = masked(CS, 3);
    NdVar Outer;
    std::optional<NdVar> OldCPL;
    if constexpr (Writer::FaultableMemory) {
      OldCPL =
          masked(load(offsetof(V5, CS) + offsetof(Segment, Selector), 2), 3);
      Outer = less(*OldCPL, CPL);
    } else {
      Outer = ne(CPL, k(0));
    }
    const auto Code = returnCode(CS, OldCPL);
    if constexpr (Writer::FaultableMemory)
      accessed(Code);
    const auto Compatibility = eq(masked(Code.Value, uint64_t{1} << 53), k(0));
    const auto TargetPC = choose(Compatibility, masked(PC, UINT32_MAX), PC);
    if constexpr (!Writer::FaultableMemory) {
      W.rejectIf(both(Compatibility, less(segmentLimit(Code.Value), TargetPC)));
      range(SP, choose(Outer, k(4 * Bytes - 1 + Discard), k(2 * Bytes - 1)));
    }
    const auto Frame =
        choose(Outer, op(NdOp::INT_ADD, {SP, k(2 * Bytes + Discard)}), SP);
    NdVar NewSP, SS;
    if constexpr (Writer::FaultableMemory) {
      SS = masked(
          readStackMemory(op(NdOp::INT_ADD, {Frame, k(Bytes)}), Bytes, Outer),
          0xffff);
      NewSP = choose(Outer, readStackMemory(Frame, Bytes, Outer),
                     op(NdOp::INT_ADD, {SP, k(2 * Bytes)}));
    } else {
      // The no-fault source uses validated RAM for its unused SELECT arm.
      NewSP = choose(Outer, readMemory(Frame, Bytes),
                     op(NdOp::INT_ADD, {SP, k(2 * Bytes)}));
      SS =
          choose(Outer,
                 masked(readMemory(op(NdOp::INT_ADD, {Frame, k(Bytes)}), Bytes),
                        0xffff),
                 k(0));
    }
    const auto NullSS = eq(masked(SS, 0xfffc), k(0));
    faultIf(both(Compatibility, both(Outer, NullSS)), 13, k(0));
    const auto Stack = returnStack(
        CS, CPL, SS, NullSS,
        Writer::FaultableMemory ? std::optional<NdVar>(Outer) : std::nullopt);
    const auto StackAttributes =
        choose(Outer, segmentAttributes(Stack.Value),
               load(offsetof(V5, SS) + offsetof(Segment, Attributes), 2));
    const auto CompatSP =
        compatibilityStack(SP, NewSP, StackAttributes, Discard);
    if constexpr (!Writer::FaultableMemory)
      accessed(Code);
    accessed(Stack);
    if constexpr (Writer::FaultableMemory) {
      faultIf(both(Compatibility, less(segmentLimit(Code.Value), TargetPC)), 13,
              k(0));
      canonical(TruncateCompatibilityIP ? TargetPC : PC);
    }
    segment(offsetof(V5, CS), CS, Code.Value);
    segment(offsetof(V5, SS),
            choose(Outer, SS,
                   load(offsetof(V5, SS) + offsetof(Segment, Selector), 2)),
            Stack.Value, NullSS);
    const auto Unusable = masked(load(offsetof(V6, Unusable), 1), 0xfe);
    const auto ReturnedUnusable = choose(
        Outer,
        op(NdOp::INT_OR, {masked(Unusable, 0xfd), choose(NullSS, k(2), k(0))}),
        Unusable);
    if constexpr (Writer::FaultableMemory) {
      W.store(offsetof(V6, Unusable), narrow(ReturnedUnusable, 1));
      W.exitIf(Outer, [&] { returnData(CPL, ReturnedUnusable); });
    } else {
      returnData(CPL, ReturnedUnusable);
    }
    W.store(offsetof(State, GPR) + x86reg::RSP,
            choose(Compatibility, CompatSP,
                   op(NdOp::INT_ADD, {NewSP, k(Discard)})));
    // RETF preserves flags except ordinary RF clearing, and retains NMI
    // blocking. No instruction at the returned target has executed yet.
    W.store(offsetof(State, RFlags),
            masked(load(offsetof(State, RFlags)), ~uint64_t{0x10000}));
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
    interrupt(k(Vector), k(NextPC),
              masked(load(offsetof(State, RFlags)), ~uint64_t{0x10000}), k(0),
              k(0), k(1), k(0), k(0), k(0));
  }

  /// Shared successful IA-32e delivery, including compatibility entry and
  /// privilege changes. Error-code presence belongs to the event kind, never
  /// merely to an IDT vector. The producer has already selected its saved PC
  /// and flags and checked interruptibility.
  void interrupt(NdVar Vector, NdVar SavedPC, NdVar SavedFlags, NdVar HasError,
                 NdVar ErrorCode, NdVar Software, NdVar NMI, NdVar PageFault,
                 NdVar FaultAddress) {
    const auto External = choose(ne(Software, k(0)), k(0), k(1));
    Base::DeliveryExternal = External;
    const auto GateError =
        op(NdOp::INT_OR, {op(NdOp::INT_LEFT, {Vector, k(3)}),
                          op(NdOp::INT_OR, {k(2), External})});
    const auto GateOffset = op(NdOp::INT_LEFT, {Vector, k(4)});
    const auto LastByte = op(NdOp::INT_ADD, {GateOffset, k(15)});
    const auto IDT = load(offsetof(V6, IDTR));
    faultIf(
        less(load(offsetof(V6, IDTR) + offsetof(Table, Limit), 2), LastByte),
        13, GateError);
    if constexpr (!Writer::FaultableMemory)
      W.rejectIf(less(op(NdOp::INT_SUB, {k(UINT64_MAX), LastByte}), IDT));
    const auto GateAddress = op(NdOp::INT_ADD, {IDT, GateOffset});
    range(GateAddress, 15);
    const auto Low = readMemory(GateAddress);
    const auto High = readMemory(op(NdOp::INT_ADD, {GateAddress, k(8)}));
    const auto Type = masked(shifted(Low, 40), 31);
    faultIf(both(ne(Type, k(14)), ne(Type, k(15))), 13, GateError);
    faultIf(ne(masked(Low, uint64_t{31} << 35), k(0)), 13, GateError);
    faultIf(ne(shifted(High, 32), k(0)), 13, GateError);
    const auto OldCS = load(offsetof(V5, CS) + offsetof(Segment, Selector), 2);
    const auto CPL = masked(OldCS, 3);
    faultIf(both(ne(Software, k(0)), less(masked(shifted(Low, 45), 3), CPL)),
            13, GateError);
    faultIf(eq(masked(Low, uint64_t{1} << 47), k(0)), 11, GateError);
    const auto CS = masked(shifted(Low, 16), 0xffff);
    const auto PC =
        op(NdOp::INT_OR,
           {op(NdOp::INT_OR,
               {masked(Low, 0xffff), masked(shifted(Low, 32), 0xffff0000)}),
            op(NdOp::INT_LEFT, {High, k(32)})});
    faultIf(eq(masked(CS, 0xfffc), k(0)), 13, External);
    const auto CodeError = op(NdOp::INT_OR, {masked(CS, 0xfffc), External});
    const auto Code = descriptor(CS);
    const auto Access = shifted(Code.Value, 40);
    faultIf(ne(masked(Access, 0x6018), k(0x2018)), 13, CodeError);
    const auto DPL = masked(shifted(Access, 5), 3);
    faultIf(less(CPL, DPL), 13, CodeError);
    faultIf(eq(masked(Access, 0x80), k(0)), 11, CodeError);
    const auto NewCPL = choose(ne(masked(Access, 4), k(0)), CPL, DPL);
    const auto Inner = less(NewCPL, CPL);
    if constexpr (Writer::FaultableMemory)
      accessed(Code);
    else
      AccessedAddress = op(NdOp::INT_ADD, {Code.Address, k(5)});
    const auto IST = masked(shifted(Low, 32), 7);
    const auto UseIST = ne(IST, k(0));
    const auto Switch = either(UseIST, Inner);
    const auto TssOffset = choose(
        UseIST, op(NdOp::INT_ADD, {k(28), op(NdOp::INT_LEFT, {IST, k(3)})}),
        op(NdOp::INT_ADD, {k(4), op(NdOp::INT_LEFT, {NewCPL, k(3)})}));
    const auto Last = op(NdOp::INT_ADD, {TssOffset, k(7)});
    const auto TR = load(offsetof(V6, TR));
    const auto BadTR = either(
        ne(masked(load(offsetof(V6, Unusable), 1), 128), k(0)),
        ne(masked(load(offsetof(V6, TR) + offsetof(Segment, Attributes), 2),
                  0x9f),
           k(0x8b)));
    const auto TssError =
        op(NdOp::INT_OR,
           {masked(load(offsetof(V6, TR) + offsetof(Segment, Selector), 2),
                   0xfffc),
            External});
    faultIf(both(Switch, BadTR), 10, TssError);
    faultIf(
        both(Switch,
             less(load(offsetof(V6, TR) + offsetof(Segment, Limit), 4), Last)),
        10, TssError);
    if constexpr (!Writer::FaultableMemory)
      W.rejectIf(
          both(Switch, less(op(NdOp::INT_SUB, {k(UINT64_MAX), Last}), TR)));
    const auto Address =
        choose(Switch, op(NdOp::INT_ADD, {TR, TssOffset}), Code.Address);
    range(Address, 7);
    const auto OldSP = load(offsetof(State, GPR) + x86reg::RSP);
    NdVar Stack;
    if constexpr (Writer::FaultableMemory)
      Stack = W.loadMemoryIf(Switch, Address, OldSP);
    else
      Stack = choose(Switch, readMemory(Address), OldSP);
    canonical(Stack, 12);
    const auto Top = masked(Stack, ~uint64_t{15});
    const auto FrameBytes = choose(ne(HasError, k(0)), k(48), k(40));
    if constexpr (!Writer::FaultableMemory)
      faultIf(less(Top, FrameBytes), 12, External);
    const auto SP = op(NdOp::INT_SUB, {Top, FrameBytes});
    range(SP, op(NdOp::INT_SUB, {FrameBytes, k(1)}), 12);
    canonical(PC);
    const auto FrameSP = op(NdOp::INT_SUB, {Top, k(40)});
    const auto Flags = load(offsetof(State, RFlags));
    const NdVar Frame[] = {
        SavedPC, OldCS, SavedFlags, OldSP,
        load(offsetof(V5, SS) + offsetof(Segment, Selector), 2)};
    if constexpr (!Writer::FaultableMemory)
      accessed(Code);
    W.eventStack(NewCPL);
    for (unsigned I = 5; I--;) {
      const auto Destination = op(NdOp::INT_ADD, {FrameSP, k(I * 8)});
      W.emit(NdOp::STORE, {}, {Destination, Frame[I]});
    }
    W.storeIf(ne(HasError, k(0)), SP, ErrorCode);
    segment(offsetof(V5, CS), op(NdOp::INT_OR, {masked(CS, 0xfffc), NewCPL}),
            Code.Value);
    // The inter-privilege path installs null SS:RPL. Same-privilege IST
    // retains SS (INT's INTRA-PRIVILEGE-LEVEL-INTERRUPT pseudocode).
    W.store(offsetof(V5, SS) + offsetof(Segment, Selector),
            narrow(choose(Inner, NewCPL, Frame[4]), 2));
    W.store(
        offsetof(V6, Unusable),
        narrow(op(NdOp::INT_OR, {masked(load(offsetof(V6, Unusable), 1), 0xfe),
                                 choose(Inner, k(2), k(0))}),
               1));
    W.store(
        offsetof(V6, NMIBlocked),
        narrow(choose(ne(NMI, k(0)), k(1), load(offsetof(V6, NMIBlocked), 1)),
               1));
    W.store(offsetof(X64ContinuationStateV4, CR2),
            choose(ne(PageFault, k(0)), FaultAddress,
                   load(offsetof(X64ContinuationStateV4, CR2))));
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
