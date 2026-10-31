//===- X64ContinuationSvm.h - Native VMEXIT state transport -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Raw VMRUN completion through the shared explicit native context intrinsic.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSVM_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSVM_H

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/ir/X86/X86SvmContext.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace neverd::analysis::detail {

template <class Writer> class X64ContinuationSvm {
  using State = X64ContinuationStateV1;
  using StateV2 = X64ContinuationStateV2;
  using StateV4 = X64ContinuationStateV4;
  using StateV10 = X64ContinuationStateV10;
  Writer &W;
  static constexpr uint64_t Context = offsetof(StateV10, SVM);

  static NdVar k(uint64_t Value, uint16_t Bytes = 8) {
    return NdVar::scalar(Value, Bytes);
  }

  NdVar address(NdVar Base, uint64_t Offset) {
    const auto Result = W.temporary();
    W.emit(NdOp::INT_ADD, Result, {Base, k(Offset)});
    return Result;
  }

  NdVar load(NdVar Base, uint64_t Offset, uint16_t Bytes = 8) {
    const auto Value = W.temporary(Bytes);
    W.emit(NdOp::LOAD, Value, {address(Base, Offset)});
    return Value;
  }

  void store(NdVar Base, uint64_t Offset, NdVar Value) {
    W.emit(NdOp::STORE, {}, {address(Base, Offset), Value});
  }

public:
  explicit X64ContinuationSvm(Writer &W) : W(W) {}

  void execute(unsigned Width, va_t Fallthrough) {
    const auto Guest = W.load(Context + offsetof(X86SvmContext, GuestXState));
    const auto Host = W.load(Context + offsetof(X86SvmContext, HostXState));
    // Validate the serialized exchange controls before any state/image write.
    // Accessibility, buffer extents and guest disjointness are domain inputs.
    for (auto Pointer : {Guest, Host}) {
      const auto Low = W.temporary(), Misaligned = W.temporary(1);
      const auto Null = W.temporary(1), Bad = W.temporary(1);
      W.emit(NdOp::INT_AND, Low, {Pointer, k(63)});
      W.emit(NdOp::INT_NOTEQUAL, Misaligned, {Low, k(0)});
      W.emit(NdOp::INT_EQUAL, Null, {Pointer, k(0)});
      W.emit(NdOp::BOOL_OR, Bad, {Null, Misaligned});
      W.rejectIf(Bad);
    }
    const auto GuestEnd = address(Guest, 832), HostEnd = address(Host, 832);
    for (auto Range : {std::pair{Guest, GuestEnd}, std::pair{Host, HostEnd}}) {
      const auto Wrapped = W.temporary(1);
      W.emit(NdOp::INT_LESS, Wrapped, {Range.second, Range.first});
      W.rejectIf(Wrapped);
    }
    const auto StateBegin = W.address(0);
    const auto StateEnd = W.address(sizeof(StateV10));
    const auto Overlap = [&](NdVar A, NdVar AEnd, NdVar B, NdVar BEnd) {
      const auto First = W.temporary(1), Second = W.temporary(1);
      const auto Both = W.temporary(1);
      W.emit(NdOp::INT_LESS, First, {A, BEnd});
      W.emit(NdOp::INT_LESS, Second, {B, AEnd});
      W.emit(NdOp::BOOL_AND, Both, {First, Second});
      W.rejectIf(Both);
    };
    Overlap(Guest, GuestEnd, Host, HostEnd);
    Overlap(Guest, GuestEnd, StateBegin, StateEnd);
    Overlap(Host, HostEnd, StateBegin, StateEnd);
    for (auto Pointer : {Guest, Host}) {
      auto Reserved = load(Pointer, 520);
      for (unsigned Offset = 528; Offset != 576; Offset += 8) {
        const auto Combined = W.temporary();
        W.emit(NdOp::INT_OR, Combined, {Reserved, load(Pointer, Offset)});
        Reserved = Combined;
      }
      const auto Bad = W.temporary(1);
      W.emit(NdOp::INT_NOTEQUAL, Bad, {Reserved, k(0)});
      W.rejectIf(Bad);
      const auto Unknown = W.temporary(), BadBits = W.temporary(1);
      W.emit(NdOp::INT_AND, Unknown, {load(Pointer, 512), k(~uint64_t{7})});
      W.emit(NdOp::INT_NOTEQUAL, BadBits, {Unknown, k(0)});
      W.rejectIf(BadBits);
    }
    const auto XStateBV = load(Guest, 512);

    W.store(Context + offsetof(X86SvmContext, VMCB),
            W.read(NdVar::reg(x86reg::RAX, 8)));
    W.store(Context + offsetof(X86SvmContext, XStateMask), k(7));
    for (unsigned I = 0; I != 14; ++I)
      W.store(Context + offsetof(X86SvmContext, GPR) + I * 8,
              W.load(offsetof(State, GPR) + X86SvmGPRs[I] * 8));
    // The state cells own SIMD/MXCSR. Only the guest image owns x87. Mark the
    // imported SIMD components present even when the prior XSAVE omitted them.
    const auto Imported = W.temporary();
    W.emit(NdOp::INT_OR, Imported, {XStateBV, k(6)});
    store(Guest, 512, Imported);
    store(Guest, 24, W.load(offsetof(StateV2, MXCSR), 4));
    for (unsigned I = 0; I != 16; ++I) {
      store(Guest, 160 + I * 16, W.load(offsetof(State, ZMM) + I * 64, 16));
      store(Guest, 576 + I * 16,
            W.load(offsetof(State, ZMM) + I * 64 + 16, 16));
    }
    W.emit(NdOp::INTRINSIC, {},
           {k(uint64_t(Intrinsic::X86SvmRunContext), 2), W.address(Context),
            k(Width, 4)});
    // CR2 is loaded for the guest and is not restored by VMEXIT. Capture the
    // actual result, including early invalid-VMCB exits, rather than guessing
    // from a VMCB field which may not have been installed.
    const auto CR2 = W.temporary();
    W.emit(NdOp::INTRINSIC, CR2, {k(uint64_t(Intrinsic::ReadCr2), 2)});
    W.store(offsetof(StateV4, CR2), CR2);
    for (unsigned I = 0; I != 14; ++I)
      W.store(offsetof(State, GPR) + X86SvmGPRs[I] * 8,
              W.load(Context + offsetof(X86SvmContext, GPR) + I * 8));
    const auto Completed = load(Guest, 512);
    for (unsigned Part = 0; Part != 2; ++Part) {
      const auto Bits = W.temporary(), Present = W.temporary(1);
      W.emit(NdOp::INT_AND, Bits, {Completed, k(2u << Part)});
      W.emit(NdOp::INT_NOTEQUAL, Present, {Bits, k(0)});
      for (unsigned I = 0; I != 16; ++I) {
        const auto Value = W.temporary(16);
        W.emit(
            NdOp::SELECT, Value,
            {Present, load(Guest, (Part ? 576 : 160) + I * 16, 16), k(0, 16)});
        W.store(offsetof(State, ZMM) + I * 64 + Part * 16, Value);
      }
    }
    // Standard XSAVE writes MXCSR whenever SSE or AVX is requested, even
    // when their XSTATE_BV component is in the initial state.
    W.store(offsetof(StateV2, MXCSR), load(Guest, 24, 4));
    const auto Flags = W.temporary();
    W.emit(NdOp::INT_AND, Flags,
           {W.load(offsetof(State, RFlags)), k(~uint64_t{0x10000})});
    W.store(offsetof(State, RFlags), Flags);
    W.leave(X64ContinuationExitV1::SvmExit, k(Fallthrough));
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSVM_H
