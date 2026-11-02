//===- X64ContinuationSmm.h - Native RSM state transport ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Restore software-owned banks before native RSM consumes the ambient save
/// image.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSMM_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSMM_H

#include "X64ContinuationAvxImage.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstddef>
#include <cstdint>

namespace neverd::analysis::detail {

template <class Writer> class X64ContinuationSmm {
  Writer &W;
  using State = X64ContinuationStateV12;
  static NdVar k(uint64_t Value) { return NdVar::scalar(Value, 8); }

public:
  explicit X64ContinuationSmm(Writer &Output) : W(Output) {}

  void execute() {
    const auto KernelGS =
        W.load(offsetof(X64ContinuationStateV2, KernelGSBase));
    const auto Left = W.temporary(), Canonical = W.temporary();
    const auto Bad = W.temporary(1);
    const auto Shift = k(64 - *W.Options.SegmentBaseMSRAddressBits);
    W.emit(NdOp::INT_LEFT, Left, {KernelGS, Shift});
    W.emit(NdOp::INT_ASHR, Canonical, {Left, Shift});
    W.emit(NdOp::INT_NOTEQUAL, Bad, {Canonical, KernelGS});
    W.rejectIf(Bad);
    X64ContinuationAvxImage<Writer> Image(
        W,
        W.load(offsetof(State, SMM) + offsetof(decltype(State::SMM), XState)));
    const auto BV = Image.validate(sizeof(State));
    Image.importVectors(BV);
    const auto Control = offsetof(State, SMM);
    W.store(Control + offsetof(decltype(State::SMM), XStateMask), k(7));
    W.store(Control + offsetof(decltype(State::SMM), CR2),
            W.load(offsetof(X64ContinuationStateV4, CR2)));
    W.store(Control + offsetof(decltype(State::SMM), CR8),
            W.load(offsetof(X64ContinuationStateV2, CR8)));
    W.store(Control + offsetof(decltype(State::SMM), KernelGSBase), KernelGS);
    W.emit(NdOp::INTRINSIC, {},
           {NdVar::scalar(uint64_t(Intrinsic::X86SmmReturnContext), 2),
            W.address(Control)});
    // Structural LowIR return only: the intrinsic is noreturn in both source
    // routes. No ordinary dispatcher continuation survives successful RSM.
    W.leave(X64ContinuationExitV1::Continue);
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSMM_H
