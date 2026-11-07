//===- X64ContinuationHle.h - Original HLE source operands ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compute original effective addresses before the native memory operation.
/// Fault classes and transaction checkpoints are outside this source binding.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONHLE_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONHLE_H

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/lift/X86/X86HleEncoding.h"

#include <cstddef>

namespace neverd::analysis::detail {

template <class Writer>
void emitX64HleStep(Writer &W, const X86HleEncoding &E, uint64_t NextIP) {
  using State = X64ContinuationStateV1;
  const auto Constant = [](uint64_t V) { return NdVar::scalar(V, 8); };
  const auto Binary = [&](NdOp Op, NdVar A, NdVar B) {
    const auto V = W.temporary();
    W.emit(Op, V, {A, B});
    return V;
  };
  const auto General = [&](unsigned I) {
    return W.load(offsetof(State, GPR) + I * sizeof(uint64_t));
  };
  auto Address = Constant(E.Relative ? NextIP : 0);
  if (E.Base)
    Address = General(*E.Base);
  Address = Binary(NdOp::INT_ADD, Address, Constant(uint64_t(E.Displacement)));
  if (E.Index)
    Address =
        Binary(NdOp::INT_ADD, Address,
               Binary(NdOp::INT_MULT, General(*E.Index), Constant(E.Scale)));
  auto Source = Constant(E.Immediate);
  if (E.RegisterSource) {
    const auto Raw = W.load(offsetof(State, GPR) + E.SourceOffset, E.Bytes);
    if (E.Bytes == 8)
      Source = Raw;
    else {
      Source = W.temporary();
      W.emit(E.bitString() ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Source, {Raw});
    }
  }
  if (E.bitString()) {
    if (E.RegisterSource) {
      const unsigned Shift = E.Bytes == 8 ? 6 : E.Bytes == 4 ? 5 : 4;
      const auto WordIndex = Binary(NdOp::INT_ASHR, Source, Constant(Shift));
      Address = Binary(NdOp::INT_ADD, Address,
                       Binary(NdOp::INT_MULT, WordIndex, Constant(E.Bytes)));
    }
    Source = Binary(NdOp::INT_AND, Source, Constant(E.Bytes * 8 - 1));
  }
  // Bit-string indexing wraps in the original address size before FS/GS.
  if (E.AddressBytes == 4)
    Address = Binary(NdOp::INT_AND, Address, Constant(UINT32_MAX));
  if (E.Segment)
    Address = Binary(NdOp::INT_ADD, Address,
                     W.load(E.Segment == 1 ? offsetof(State, FSBase)
                                           : offsetof(State, GSBase)));
  W.emit(NdOp::INTRINSIC, {},
         {NdVar::scalar(uint64_t(Intrinsic::X86HleState), 2), Address,
          W.address(offsetof(State, GPR)), W.address(offsetof(State, RFlags)),
          Source, NdVar::scalar(E.control(), 4)});
  W.leave(X64ContinuationExitV1::Continue, Constant(NextIP));
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONHLE_H
