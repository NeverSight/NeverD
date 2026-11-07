//===- X64ContinuationAvxImage.h - Native xstate exchange -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared standard XSAVE image admission and explicit SIMD import for XCR0=7.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONAVXIMAGE_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONAVXIMAGE_H

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace neverd::analysis::detail {

template <class Writer> class X64ContinuationAvxImage {
  Writer &W;
  NdVar Pointer;
  static NdVar k(uint64_t Value) { return NdVar::scalar(Value, 8); }
  NdVar op(NdOp Code, std::initializer_list<NdVar> Inputs, uint16_t Bytes = 8) {
    const auto Value = W.temporary(Bytes);
    W.emit(Code, Value, Inputs);
    return Value;
  }
  NdVar address(uint64_t Offset) {
    return op(NdOp::INT_ADD, {Pointer, k(Offset)});
  }
  NdVar load(uint64_t Offset) { return op(NdOp::LOAD, {address(Offset)}); }
  void store(uint64_t Offset, NdVar Value) {
    W.emit(NdOp::STORE, {}, {address(Offset), Value});
  }

public:
  static constexpr uint64_t Bytes = 832;
  X64ContinuationAvxImage(Writer &Output, NdVar Image)
      : W(Output), Pointer(Image) {}

  NdVar validate(uint64_t StateBytes) {
    const auto Low = op(NdOp::INT_AND, {Pointer, k(63)});
    W.rejectIf(op(NdOp::INT_NOTEQUAL, {Low, k(0)}, 1));
    W.rejectIf(op(NdOp::INT_EQUAL, {Pointer, k(0)}, 1));
    const auto End = address(Bytes);
    W.rejectIf(op(NdOp::INT_LESS, {End, Pointer}, 1));
    const auto First = op(NdOp::INT_LESS, {Pointer, W.address(StateBytes)}, 1);
    const auto Second = op(NdOp::INT_LESS, {W.address(0), End}, 1);
    W.rejectIf(op(NdOp::BOOL_AND, {First, Second}, 1));
    auto Reserved = load(520);
    for (unsigned Offset = 528; Offset != 576; Offset += 8)
      Reserved = op(NdOp::INT_OR, {Reserved, load(Offset)});
    W.rejectIf(op(NdOp::INT_NOTEQUAL, {Reserved, k(0)}, 1));
    const auto BV = load(512);
    const auto Unknown = op(NdOp::INT_AND, {BV, k(~uint64_t{7})});
    W.rejectIf(op(NdOp::INT_NOTEQUAL, {Unknown, k(0)}, 1));
    return BV;
  }

  /// Explicit SIMD cells override the image; x87 retains its image ownership.
  void importVectors(NdVar BV) {
    store(512, op(NdOp::INT_OR, {BV, k(6)}));
    store(24, W.load(offsetof(X64ContinuationStateV2, MXCSR), 4));
    for (unsigned I = 0; I != 16; ++I) {
      store(160 + I * 16,
            W.load(offsetof(X64ContinuationStateV1, ZMM) + I * 64, 16));
      store(576 + I * 16,
            W.load(offsetof(X64ContinuationStateV1, ZMM) + I * 64 + 16, 16));
    }
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONAVXIMAGE_H
