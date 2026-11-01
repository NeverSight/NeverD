//===- X64ContinuationSgx.h - Original-site SGX transfer -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Import explicit native input state without relocating ENCLU's next PC.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSGX_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSGX_H

#include "X64ContinuationAvxImage.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstddef>
#include <cstdint>

namespace neverd::analysis::detail {

template <class Writer> class X64ContinuationSgx {
  Writer &W;
  using State = X64ContinuationStateV13;
  static NdVar k(uint64_t Value, uint16_t Bytes = 8) {
    return NdVar::scalar(Value, Bytes);
  }

public:
  explicit X64ContinuationSgx(Writer &Output) : W(Output) {}

  void execute(uint64_t OriginalPC) {
    const auto Leaf = W.load(offsetof(X64ContinuationStateV1, GPR), 4);
    const auto Index = W.temporary(4), Bad = W.temporary(1);
    W.emit(NdOp::INT_SUB, Index, {Leaf, k(2, 4)});
    W.emit(NdOp::INT_LESS, Bad, {k(2, 4), Index});
    W.rejectIf(Bad);
    const auto Flags = W.load(offsetof(X64ContinuationStateV1, RFlags));
    const auto IF = W.temporary();
    W.emit(NdOp::INT_AND, IF, {Flags, k(0x200)});
    W.emit(NdOp::INT_EQUAL, Bad, {IF, k(0)});
    W.rejectIf(Bad);
    const auto Control = offsetof(State, SGX);
    X64ContinuationAvxImage<Writer> Image(
        W, W.load(Control + offsetof(decltype(State::SGX), XState)));
    const auto BV = Image.validate(sizeof(State));
    // Every admission check precedes any image/state write.
    Image.importVectors(BV);
    for (unsigned I = 0; I != 16; ++I)
      W.store(Control + offsetof(decltype(State::SGX), GPR) + I * 8,
              W.load(offsetof(X64ContinuationStateV1, GPR) + I * 8));
    W.store(Control + offsetof(decltype(State::SGX), RFlags), Flags);
    W.emit(NdOp::INTRINSIC, {},
           {NdVar::scalar(uint64_t(Intrinsic::X86SgxTransferAtContext), 2),
            W.address(Control), k(OriginalPC)});
    // Structural LowIR return only: native success has no C continuation.
    W.leave(X64ContinuationExitV1::Continue);
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSGX_H
