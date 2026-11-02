//===- X64ContinuationFredReturns.h - FRED return state --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Successful long-mode FRED returns, before target or pending-event execution.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONFREDRETURNS_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONFREDRETURNS_H

#include "X64ContinuationSegments.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd::analysis::detail {

template <class Writer>
class X64ContinuationFredReturns : private X64ContinuationSegments<Writer> {
  using Base = X64ContinuationSegments<Writer>;
  using typename Base::Exit;
  using typename Base::Segment;
  using typename Base::State;
  using typename Base::V5;
  using typename Base::V6;
  using V9 = X64ContinuationStateV9;
  using Base::accessed;
  using Base::both;
  using Base::canonical;
  using Base::choose;
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
  using Base::returnStack;
  using Base::segment;
  using Base::segmentLimit;
  using Base::shifted;
  using Base::W;

  void finish(NdVar PC, NdVar Flags, NdVar SP, NdVar SS, bool User) {
    const auto Pending = either(
        ne(masked(load(offsetof(State, RFlags)), 0x100), k(0)),
        both(ne(masked(SS, 0x20000), k(0)), ne(masked(Flags, 0x100), k(0))));
    const auto STI = User ? NdVar::scalar(0, 1)
                          : both(both(ne(masked(SS, 0x10000), k(0)),
                                      ne(masked(Flags, 0x200), k(0))),
                                 eq(load(offsetof(V9, STIBlocked), 1), k(0)));
    W.store(offsetof(V9, PendingDebug), Pending);
    W.store(offsetof(V9, STIBlocked), STI);
    W.store(offsetof(V6, NMIBlocked),
            narrow(choose(ne(masked(SS, 0x40000), k(0)), k(0),
                          load(offsetof(V6, NMIBlocked), 1)),
                   1));
    W.store(offsetof(State, RFlags), Flags);
    W.store(offsetof(State, GPR) + x86reg::RSP, SP);
    W.leave(Exit::FredReturn, PC);
  }

  void userFinish(NdVar PC, NdVar Flags, NdVar SP, NdVar SS) {
    // FRED deliberately retains DS/ES/FS/GS selectors and caches, even when
    // their DPL is below 3. Only GS.base and the kernel GS-base MSR swap.
    const auto GS = load(offsetof(State, GSBase));
    const auto Kernel = load(offsetof(X64ContinuationStateV2, KernelGSBase));
    W.store(offsetof(State, GSBase), Kernel);
    W.store(offsetof(X64ContinuationStateV2, KernelGSBase), GS);
    W.invalidateMonitor();
    W.store(offsetof(V6, Unusable),
            narrow(masked(load(offsetof(V6, Unusable), 1), 0xfc), 1));
    finish(PC, Flags, SP, SS, true);
  }

public:
  X64ContinuationFredReturns(Writer &Output, unsigned Bits)
      : Base(Output, Bits) {}

  std::vector<X64ContinuationCompatibilityChoice> execute(bool User) {
    if (User)
      W.rejectIf(ne(masked(load(offsetof(V9, FredConfig)), 3), k(0)));
    // The error code itself is not read. The five return words occupy
    // offsets 8..47, irrespective of the event-information area above them.
    const auto Frame =
        op(NdOp::INT_ADD, {load(offsetof(State, GPR) + x86reg::RSP), k(8)});
    range(Frame, 39);
    NdVar Words[5];
    for (unsigned I = 0; I != 5; ++I)
      Words[I] = readMemory(op(NdOp::INT_ADD, {Frame, k(I * 8)}));
    const auto PC = Words[0], CS = Words[1], Flags = Words[2];
    const auto SP = Words[3], SS = Words[4];
    W.rejectIf(
        ne(masked(Flags, User ? 0xffffffffffc2b02aULL : 0xffffffffffc2802aULL),
           k(2)));
    if (!User) {
      W.rejectIf(ne(masked(CS, 0xfffffffffff8ffffULL),
                    load(offsetof(V5, CS) + offsetof(Segment, Selector), 2)));
      W.rejectIf(ne(masked(SS, 0xfff8ffff),
                    load(offsetof(V5, SS) + offsetof(Segment, Selector), 2)));
      canonical(PC);
      const auto Config = load(offsetof(V9, FredConfig));
      const auto OldLevel = masked(Config, 3);
      const auto Requested = masked(shifted(CS, 16), 3);
      const auto Level = choose(less(Requested, OldLevel), Requested, OldLevel);
      W.store(offsetof(V9, FredConfig),
              op(NdOp::INT_OR, {masked(Config, ~uint64_t{3}), Level}));
      finish(PC, Flags, SP, SS, false);
      return {};
    }
    W.rejectIf(ne(masked(CS, 0xffffffffffff0003ULL), k(3)));
    W.rejectIf(ne(masked(SS, 0xfff80003), k(3)));
    const auto SelectorCS = masked(CS, 0xffff);
    const auto SelectorSS = masked(SS, 0xffff);
    const auto Star = shifted(load(offsetof(V5, STAR)), 48);
    const auto StandardSS = masked(op(NdOp::INT_ADD, {Star, k(8)}), 0xffff);
    const auto StandardCS = masked(op(NdOp::INT_ADD, {Star, k(16)}), 0xffff);
    const auto Choices = x64CompatibilityChoices(true);
    W.exitIf(both(eq(SelectorCS, StandardCS), eq(SelectorSS, StandardSS)), [&] {
      canonical(PC);
      writeX64FlatContinuationSegment(W, true, narrow(SelectorCS, 2), true, 3);
      writeX64FlatContinuationSegment(W, false, narrow(SelectorSS, 2), true, 3);
      userFinish(PC, Flags, SP, SS);
    });
    W.exitIf(both(eq(SelectorCS, Star), eq(SelectorSS, StandardSS)), [&] {
      writeX64FlatContinuationSegment(W, true, narrow(SelectorCS, 2), false, 3);
      writeX64FlatContinuationSegment(W, false, narrow(SelectorSS, 2), false,
                                      3);
      W.compatibilityChoices(Choices);
      userFinish(masked(PC, UINT32_MAX), Flags, masked(SP, UINT32_MAX), SS);
    });
    const auto Code = returnCode(SelectorCS, k(3));
    const auto Stack = returnStack(SelectorCS, k(3), SelectorSS,
                                   eq(masked(SelectorSS, 0xfffc), k(0)));
    const auto Compatibility = eq(masked(Code.Value, uint64_t{1} << 53), k(0));
    const auto NarrowPC = masked(PC, UINT32_MAX);
    W.rejectIf(both(Compatibility, less(segmentLimit(Code.Value), NarrowPC)));
    canonical(choose(Compatibility, NarrowPC, PC));
    accessed(Code);
    accessed(Stack);
    segment(offsetof(V5, CS), SelectorCS, Code.Value);
    segment(offsetof(V5, SS), SelectorSS, Stack.Value);
    W.exitIf(Compatibility, [&] {
      W.compatibilityChoices(Choices);
      userFinish(NarrowPC, Flags, masked(SP, UINT32_MAX), SS);
    });
    userFinish(PC, Flags, SP, SS);
    return Choices;
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONFREDRETURNS_H
