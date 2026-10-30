//===- X64ContinuationUserInterrupts.h - UIF and return ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Successful UINTR state transitions, shared above both C emission routes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONUSERINTERRUPTS_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONUSERINTERRUPTS_H

#include "X64UserInterruptEncoding.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstddef>
#include <cstdint>

namespace neverd::analysis::detail {

template <class Writer> class X64ContinuationUserInterrupts {
  using State = X64ContinuationStateV1;
  using StateV8 = X64ContinuationStateV8;
  using Exit = X64ContinuationExitV1;
  Writer &W;
  const X64ContinuationSourceOptions &Options;

  static NdVar k(uint64_t Value, uint16_t Bytes = 8) {
    return NdVar::scalar(Value, Bytes);
  }

  NdVar noncanonical(NdVar Value) {
    const auto Shift = k(64 - *Options.PagingAddressBits);
    const auto Shifted = W.temporary(), Canonical = W.temporary();
    const auto Bad = W.temporary(1);
    W.emit(NdOp::INT_LEFT, Shifted, {Value, Shift});
    W.emit(NdOp::INT_ASHR, Canonical, {Shifted, Shift});
    W.emit(NdOp::INT_NOTEQUAL, Bad, {Value, Canonical});
    return Bad;
  }

public:
  X64ContinuationUserInterrupts(Writer &W,
                                const X64ContinuationSourceOptions &Options)
      : W(W), Options(Options) {}

  void execute(X64UserInterrupt Kind, va_t Fallthrough) {
    if (Kind != X64UserInterrupt::Return) {
      // RF clears at instruction start (SDM Vol. 3B, 20.3.1.1), even when
      // the instruction-specific flags list says that flags are unaffected.
      const auto CurrentFlags = W.temporary();
      W.emit(NdOp::INT_AND, CurrentFlags,
             {W.load(offsetof(State, RFlags)), k(~uint64_t{0x10000})});
      if (Kind == X64UserInterrupt::Test) {
        const auto Flags = W.temporary(), UIF = W.temporary();
        const auto Result = W.temporary();
        W.emit(NdOp::INT_AND, Flags, {CurrentFlags, k(~uint64_t{0x8d5})});
        W.emit(NdOp::INT_ZEXT, UIF, {W.load(offsetof(StateV8, UIF), 1)});
        W.emit(NdOp::INT_OR, Result, {Flags, UIF});
        W.store(offsetof(State, RFlags), Result);
      } else {
        W.store(offsetof(StateV8, UIF), k(Kind == X64UserInterrupt::Set, 1));
        W.store(offsetof(State, RFlags), CurrentFlags);
      }
      W.leave(Exit::Continue, k(Fallthrough));
      return;
    }

    const auto SP = W.read(NdVar::reg(x86reg::RSP, 8));
    const auto End = W.temporary(), Wrapped = W.temporary(1);
    const auto BadRange = W.temporary(1), Bad = W.temporary(1);
    W.emit(NdOp::INT_ADD, End, {SP, k(23)});
    W.emit(NdOp::INT_LESS, Wrapped, {End, SP});
    W.emit(NdOp::BOOL_OR, BadRange, {noncanonical(SP), noncanonical(End)});
    W.emit(NdOp::BOOL_OR, Bad, {BadRange, Wrapped});
    W.rejectIf(Bad);
    NdVar Words[3];
    for (unsigned I = 0; I != 3; ++I) {
      const auto Address = W.temporary();
      Words[I] = W.temporary();
      W.emit(NdOp::INT_ADD, Address, {SP, k(I * 8)});
      W.emit(NdOp::LOAD, Words[I], {Address});
    }
    W.rejectIf(noncanonical(Words[0]));
    // SDM 325462-090, UIRET: selective flag restore, no RSP canonical check,
    // no segment or NMI change. Saved bit 1 only selects the enhanced UIF rule.
    const auto Kept = W.temporary(), Restored = W.temporary();
    const auto Flags = W.temporary();
    W.emit(NdOp::INT_AND, Kept,
           {W.load(offsetof(State, RFlags)), k(~uint64_t{0x254dd5})});
    W.emit(NdOp::INT_AND, Restored, {Words[1], k(0x254dd5)});
    W.emit(NdOp::INT_OR, Flags, {Kept, Restored});
    NdVar UIF = k(1, 1);
    if (*Options.UiretRestoresUIF) {
      const auto Shifted = W.temporary(), Bit = W.temporary();
      UIF = W.temporary(1);
      W.emit(NdOp::INT_RIGHT, Shifted, {Words[1], k(1)});
      W.emit(NdOp::INT_AND, Bit, {Shifted, k(1)});
      W.emit(NdOp::SUBBYTES, UIF, {Bit, k(0)});
    }
    W.store(offsetof(State, RFlags), Flags);
    W.write(NdVar::reg(x86reg::RSP, 8), Words[2]);
    W.store(offsetof(StateV8, UIF), UIF);
    W.store(offsetof(StateV8, MonitorActive), k(0, 1));
    W.leave(Exit::UserInterruptReturn, Words[0]);
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONUSERINTERRUPTS_H
