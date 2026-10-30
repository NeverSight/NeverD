//===- X64ContinuationSystemTransfers.h - Fast-call state ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit register, MSR and segment-cache transitions for fast system calls.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSYSTEMTRANSFERS_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSYSTEMTRANSFERS_H

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd::analysis::detail {

/// The writer owns state transport; this owner installs the complete target
/// context and returns before any instruction executes in that new context.
template <class Writer> class X64ContinuationSystemTransfers {
  using State = X64ContinuationStateV1;
  using StateV5 = X64ContinuationStateV5;
  using StateV6 = X64ContinuationStateV6;
  using StateV7 = X64ContinuationStateV7;
  using Segment = X64ContinuationSegment;
  using Exit = X64ContinuationExitV1;
  Writer &W;
  const X64ContinuationSourceOptions &Options;

  static NdVar k(uint64_t Value, uint16_t Bytes = 8) {
    return NdVar::scalar(Value, Bytes);
  }

  void flatSegment(bool Code, NdVar Selector, bool Wide, unsigned CPL) {
    const auto Offset = Code ? offsetof(StateV5, CS) : offsetof(StateV5, SS);
    W.store(Offset + offsetof(Segment, Base), k(0));
    W.store(Offset + offsetof(Segment, Limit), k(UINT32_MAX, 4));
    W.store(Offset + offsetof(Segment, Selector), Selector);
    W.store(Offset + offsetof(Segment, Attributes),
            k((Code ? (Wide ? 0xa09b : 0xc09b) : 0xc093) | (CPL << 5), 2));
  }

public:
  X64ContinuationSystemTransfers(Writer &W,
                                 const X64ContinuationSourceOptions &Options)
      : W(W), Options(Options) {}

  void sysret(bool Wide, llvm::ArrayRef<LowUndefinedEffect> Effects) {
    NdVar PC = W.read(NdVar::reg(x86reg::RCX, Wide ? 8 : 4));
    if (Wide) {
      const NdVar Shifted = W.temporary(), Canonical = W.temporary();
      const NdVar Bad = W.temporary(1);
      const auto Shift = k(64 - *Options.PagingAddressBits);
      W.emit(NdOp::INT_LEFT, Shifted, {PC, Shift});
      W.emit(NdOp::INT_ASHR, Canonical, {Shifted, Shift});
      W.emit(NdOp::INT_NOTEQUAL, Bad, {Canonical, PC});
      W.rejectIf(Bad);
    } else {
      const auto Low = PC;
      PC = W.temporary();
      W.emit(NdOp::INT_ZEXT, PC, {Low});
    }
    const NdVar Masked = W.temporary(), Packed = W.temporary(),
                Selectors = W.temporary();
    W.emit(NdOp::INT_AND, Masked,
           {W.read(NdVar::reg(x86reg::R11, 8)), k(0x3c7fd7)});
    W.emit(NdOp::INT_OR, Packed, {Masked, k(2)});
    W.emit(NdOp::INT_RIGHT, Selectors,
           {W.load(offsetof(StateV5, STAR)), k(48)});
    for (bool Code : {true, false}) {
      const NdVar Added = W.temporary(), User = W.temporary(),
                  Selector = W.temporary(2);
      W.emit(NdOp::INT_ADD, Added, {Selectors, k(Code ? (Wide ? 16 : 0) : 8)});
      W.emit(NdOp::INT_OR, User, {Added, k(3)});
      W.emit(NdOp::SUBBYTES, Selector, {User, k(0)});
      flatSegment(Code, Selector, Wide, 3);
    }
    if (Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6 ||
        Options.Profile == X64ContinuationProfile::KernelFastSystemNoFaultV7 ||
        Options.Profile == X64ContinuationProfile::UserInterruptNoFaultV8) {
      const auto Usable = W.temporary(1);
      W.emit(NdOp::INT_AND, Usable,
             {W.load(offsetof(StateV6, Unusable), 1), k(0xfc, 1)});
      W.store(offsetof(StateV6, Unusable), Usable);
    }
    W.store(offsetof(State, RFlags), Packed);
    for (size_t I = 0; I != Effects.size(); ++I)
      W.undefined(Effects[I], I);
    W.leave(Exit::PrivilegeReturn, PC);
  }

  void fastSystem(bool Enter, bool Wide,
                  llvm::ArrayRef<LowUndefinedEffect> Effects) {
    const NdVar CS = W.load(offsetof(StateV7, SysenterCS));
    const NdVar Index = W.temporary(), Null = W.temporary(1);
    W.emit(NdOp::INT_AND, Index, {CS, k(0xfffc)});
    W.emit(NdOp::INT_EQUAL, Null, {Index, k(0)});
    W.rejectIf(Null);
    NdVar PC = Enter ? W.load(offsetof(StateV7, SysenterEIP))
                     : W.read(NdVar::reg(x86reg::RDX, Wide ? 8 : 4));
    NdVar SP = Enter ? W.load(offsetof(StateV7, SysenterESP))
                     : W.read(NdVar::reg(x86reg::RCX, Wide ? 8 : 4));
    if (!Enter && !Wide) {
      const NdVar NarrowPC = PC, NarrowSP = SP;
      PC = W.temporary();
      SP = W.temporary();
      W.emit(NdOp::INT_ZEXT, PC, {NarrowPC});
      W.emit(NdOp::INT_ZEXT, SP, {NarrowSP});
    } else {
      // A noncanonical target cannot enter the declared nonfaulting context.
      // SYSENTER does not access its new stack; the MSR's processor-width
      // admission above is sufficient for that value. SYSEXIT checks both.
      for (NdVar Value :
           Enter ? std::vector<NdVar>{PC} : std::vector<NdVar>{PC, SP}) {
        const NdVar Shifted = W.temporary(), Canonical = W.temporary();
        const NdVar Bad = W.temporary(1);
        const auto Shift = k(64 - *Options.PagingAddressBits);
        W.emit(NdOp::INT_LEFT, Shifted, {Value, Shift});
        W.emit(NdOp::INT_ASHR, Canonical, {Shifted, Shift});
        W.emit(NdOp::INT_NOTEQUAL, Bad, {Value, Canonical});
        W.rejectIf(Bad);
      }
    }
    // All checks precede architectural writes. Neither transfer reads the
    // descriptor tables or the original/new stack, even for TI or wraparound.
    const NdVar Base = W.temporary();
    W.emit(NdOp::INT_AND, Base, {CS, k(0xfffc)});
    for (bool Code : {true, false}) {
      const NdVar Added = W.temporary(), Selected = W.temporary();
      const NdVar Selector = W.temporary(2);
      const unsigned Add = (Enter ? 0 : Wide ? 32 : 16) + (Code ? 0 : 8);
      W.emit(NdOp::INT_ADD, Added, {Base, k(Add)});
      W.emit(NdOp::INT_OR, Selected, {Added, k(Enter ? 0 : 3)});
      W.emit(NdOp::SUBBYTES, Selector, {Selected, k(0)});
      flatSegment(Code, Selector, Enter || Wide, Enter ? 0 : 3);
    }
    const NdVar Usable = W.temporary(1);
    W.emit(NdOp::INT_AND, Usable,
           {W.load(offsetof(StateV6, Unusable), 1), k(0xfc, 1)});
    W.store(offsetof(StateV6, Unusable), Usable);
    if (Enter) {
      const NdVar Flags = W.temporary();
      W.emit(NdOp::INT_AND, Flags,
             {W.load(offsetof(State, RFlags)), k(~uint64_t{0x20200})});
      W.store(offsetof(State, RFlags), Flags);
    }
    W.write(NdVar::reg(x86reg::RSP, 8), SP);
    for (size_t I = 0; I != Effects.size(); ++I)
      W.undefined(Effects[I], I);
    W.leave(Enter ? Exit::PrivilegeEnter : Exit::PrivilegeReturn, PC);
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSYSTEMTRANSFERS_H
