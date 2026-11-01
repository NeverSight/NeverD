//===- X64ContinuationDelivery.h - Event admission ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit event classification, interruptibility and saved-frame inputs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONDELIVERY_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONDELIVERY_H

#include "X64ContinuationEvents.h"
#include "X64ContinuationSegments.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationEvent.h"
#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/ir/low/LowIR.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace neverd::analysis::detail {

template <class Writer>
class X64ContinuationDelivery : private X64ContinuationSegments<Writer> {
  using Base = X64ContinuationSegments<Writer>;
  using Base::both;
  using Base::canonical;
  using Base::choose;
  using Base::either;
  using Base::eq;
  using Base::k;
  using Base::less;
  using Base::load;
  using Base::masked;
  using Base::ne;
  using Base::op;
  using Base::shifted;
  using Base::W;
  using typename Base::Exit;
  using typename Base::Segment;
  using typename Base::State;
  using typename Base::V5;
  using typename Base::V6;
  using Event = X64ContinuationEventV1;
  using Kind = X64ContinuationEventKind;

  NdVar event(NdVar Pointer, uint64_t Offset, uint16_t Bytes = 1) {
    const auto Value =
        op(NdOp::LOAD, {op(NdOp::INT_ADD, {Pointer, k(Offset)})}, Bytes);
    return Bytes == 8 ? Value : op(NdOp::INT_ZEXT, {Value});
  }

  NdVar member(NdVar Value, std::initializer_list<unsigned> Values) {
    auto Result = eq(Value, k(*Values.begin()));
    for (auto I = Values.begin() + 1; I != Values.end(); ++I)
      Result = either(Result, eq(Value, k(*I)));
    return Result;
  }

public:
  X64ContinuationDelivery(Writer &Output, unsigned Bits) : Base(Output, Bits) {}

  void execute(NdVar Pointer) {
    const auto Type = event(Pointer, offsetof(Event, Kind));
    const auto Vector = event(Pointer, offsetof(Event, Vector));
    const auto Is = [&](Kind K) { return eq(Type, k(unsigned(K))); };
    const auto Fault = Is(Kind::Fault), Trap = Is(Kind::Trap);
    const auto Interrupt = Is(Kind::Interrupt), NMI = Is(Kind::NMI);
    const auto Software = Is(Kind::Software);
    const auto Breakpoint = Is(Kind::InstructionBreakpoint);
    const auto Abort = Is(Kind::Abort);
    auto Valid = either(Interrupt, Software);
    Valid = either(Valid, both(NMI, eq(Vector, k(2))));
    Valid = either(Valid, both(Breakpoint, eq(Vector, k(1))));
    Valid = either(Valid, both(Trap, member(Vector, {1, 3, 4})));
    Valid = either(Valid, both(Abort, member(Vector, {8, 18})));
    Valid =
        either(Valid, both(Fault, member(Vector, {0, 1, 5, 6, 7, 10, 11, 12, 13,
                                                  14, 16, 17, 19, 20})));
    W.rejectIf(eq(Valid, NdVar::scalar(0, 1)));
    for (auto Offset :
         {offsetof(Event, STIBlocked), offsetof(Event, SSBlocked),
          offsetof(Event, NMIBlockedBySTI), offsetof(Event, RepeatedString)})
      W.rejectIf(less(k(1), event(Pointer, Offset)));
    W.rejectIf(ne(event(Pointer, offsetof(Event, Reserved), 2), k(0)));
    const auto Repeated =
        ne(event(Pointer, offsetof(Event, RepeatedString)), k(0));
    W.rejectIf(both(Repeated, eq(either(either(Interrupt, NMI), Trap),
                                 NdVar::scalar(0, 1))));
    const auto Error = event(Pointer, offsetof(Event, ErrorCode), 8);
    const auto HasError =
        both(either(Fault, Abort), member(Vector, {8, 10, 11, 12, 13, 14, 17}));
    W.rejectIf(both(eq(HasError, NdVar::scalar(0, 1)), ne(Error, k(0))));
    W.rejectIf(both(member(Vector, {8, 17}), ne(Error, k(0))));
    W.rejectIf(less(k(UINT32_MAX), Error));
    const auto PageFault = both(Fault, eq(Vector, k(14)));
    const auto FaultAddress = event(Pointer, offsetof(Event, FaultAddress), 8);
    W.rejectIf(
        both(eq(PageFault, NdVar::scalar(0, 1)), ne(FaultAddress, k(0))));

    const auto Flags = load(offsetof(State, RFlags));
    W.rejectIf(ne(masked(Flags, ~uint64_t{0x3d7fd7}), k(0)));
    W.rejectIf(eq(masked(Flags, 2), k(0)));
    W.rejectIf(less(k(15), load(offsetof(X64ContinuationStateV2, CR8))));
    W.rejectIf(ne(masked(load(offsetof(X64ContinuationStateV2, MXCSR), 4),
                         uint32_t(~*W.Options.MXCSRMask)),
                  k(0)));
    W.rejectIf(ne(masked(load(offsetof(V6, Unusable), 1), 1), k(0)));
    const auto NMIBlocked = load(offsetof(V6, NMIBlocked), 1);
    W.rejectIf(less(k(1), NMIBlocked));
    const auto Attributes =
        load(offsetof(V5, CS) + offsetof(Segment, Attributes), 2);
    W.rejectIf(ne(masked(Attributes, 0x98), k(0x98)));
    W.rejectIf(eq(masked(Attributes, 0x6000), k(0x6000)));
    const auto CPL =
        masked(load(offsetof(V5, CS) + offsetof(Segment, Selector), 2), 3);
    const auto DPL = masked(shifted(Attributes, 5), 3);
    W.rejectIf(less(CPL, DPL));
    W.rejectIf(both(eq(masked(Attributes, 4), k(0)), ne(CPL, DPL)));

    const auto STI = ne(event(Pointer, offsetof(Event, STIBlocked)), k(0));
    const auto SS = ne(event(Pointer, offsetof(Event, SSBlocked)), k(0));
    const auto BlockInterrupt =
        both(Interrupt, either(either(STI, SS), eq(masked(Flags, 512), k(0))));
    const auto BlockNMI = both(
        NMI,
        either(either(SS, ne(NMIBlocked, k(0))),
               both(STI, ne(event(Pointer, offsetof(Event, NMIBlockedBySTI)),
                            k(0)))));
    const auto Debug =
        both(eq(Vector, k(1)), either(either(Fault, Trap), Breakpoint));
    W.exitIf(either(either(BlockInterrupt, BlockNMI), both(SS, Debug)),
             [&] { W.leave(Exit::EventDeferred); });

    const auto SavedPC = event(Pointer, offsetof(Event, SavedPC), 8);
    canonical(SavedPC);
    W.rejectIf(both(
        eq(masked(Attributes, 0x2000), k(0)),
        less(load(offsetof(V5, CS) + offsetof(Segment, Limit), 4), SavedPC)));
    const auto SavedFlags =
        choose(Software, masked(Flags, ~uint64_t{0x10000}),
               choose(either(Fault, Repeated),
                      op(NdOp::INT_OR, {Flags, k(0x10000)}), Flags));
    X64ContinuationEvents<Writer>(W, Base::AddressBits)
        .interrupt(Vector, SavedPC, SavedFlags, op(NdOp::INT_ZEXT, {HasError}),
                   Error, op(NdOp::INT_ZEXT, {Software}),
                   op(NdOp::INT_ZEXT, {NMI}), op(NdOp::INT_ZEXT, {PageFault}),
                   FaultAddress);
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONDELIVERY_H
