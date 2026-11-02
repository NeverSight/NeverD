//===- X64ContinuationSegmentMoves.h - Selector transitions ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind selector MOV/PUSH/POP to the shared descriptor and cache owner.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSEGMENTMOVES_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSEGMENTMOVES_H

#include "X64ContinuationSegments.h"
#include "X64SegmentMoveEncoding.h"

#include "llvm/Support/ErrorHandling.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::analysis::detail {

template <class Writer>
class X64ContinuationSegmentMoves : private X64ContinuationSegments<Writer> {
  using Base = X64ContinuationSegments<Writer>;
  using Base::k;
  using Base::load;
  using Base::masked;
  using Base::narrow;
  using Base::op;
  using Base::W;
  using typename Base::Exit;
  using typename Base::Metadata;
  using typename Base::Segment;
  using typename Base::State;
  using typename Base::V5;
  using typename Base::V6;

public:
  X64ContinuationSegmentMoves(Writer &Output, unsigned Bits)
      : Base(Output, Bits) {}

  void execute(const X64SegmentMove &F, uint64_t NextPC,
               std::optional<X64NullSegmentBasePolicy> NullBasePolicy) {
    static_assert(Writer::FaultableMemory);
    this->DeliveryExternal = k(0);
    this->InstructionDescriptors = true;
    NdVar Address;
    if (!F.Register) {
      Address = k(uint64_t(F.Displacement));
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
      if (F.Segment)
        Address = op(NdOp::INT_ADD,
                     {Address, load(F.Segment == 1 ? offsetof(State, FSBase)
                                                   : offsetof(State, GSBase))});
      if (F.Push)
        Address = op(NdOp::INT_SUB, {Address, k(F.Bytes)});
    }
    const bool Stack =
        F.Pop || F.Push || (!F.Segment && (F.Base == 4 || F.Base == 5));
    if (F.Read) {
      const auto SelectorOffset = [&] {
        switch (F.SegmentRegister) {
        case X86SegmentRegister::CS:
          return offsetof(V5, CS) + offsetof(Segment, Selector);
        case X86SegmentRegister::SS:
          return offsetof(V5, SS) + offsetof(Segment, Selector);
        case X86SegmentRegister::DS:
          return offsetof(V6, DS) + offsetof(Segment, Selector);
        case X86SegmentRegister::ES:
          return offsetof(V6, ES) + offsetof(Segment, Selector);
        case X86SegmentRegister::FS:
          return offsetof(V6, FS) + offsetof(Metadata, Selector);
        case X86SegmentRegister::GS:
          return offsetof(V6, GS) + offsetof(Metadata, Selector);
        }
        llvm_unreachable("validated segment register");
      }();
      const auto Selector = load(SelectorOffset, 2);
      if (F.Register)
        W.write(NdVar::reg(*F.Register * 8, F.Bytes),
                F.Bytes == 8 ? Selector : narrow(Selector, F.Bytes));
      else {
        W.instructionAccess(true, Stack);
        // Unlike POP's selector read, a qword PUSH stores all eight bytes.
        const auto Value =
            F.Push && F.Bytes == 8 ? Selector : narrow(Selector, 2);
        W.emit(NdOp::STORE, {}, {Address, Value});
        W.instructionAccess(false);
        if (F.Push)
          W.store(offsetof(State, GPR) + x86reg::RSP, Address);
      }
      W.leave(Exit::Continue, k(NextPC));
      return;
    }
    NdVar Selector;
    if (F.Register)
      Selector = load(offsetof(State, GPR) + *F.Register * 8, 2);
    else {
      W.instructionAccess(true, Stack);
      // The qualified POP profile consumes a word selector even when the
      // stack increment is eight bytes. Bytes beyond that word are not read.
      const auto Value = op(NdOp::LOAD, {Address}, 2);
      Selector = op(NdOp::INT_ZEXT, {Value});
      W.instructionAccess(false);
    }
    this->loadDataSegment(F.SegmentRegister, Selector, NullBasePolicy, [&] {
      if (F.Pop)
        W.store(offsetof(State, GPR) + x86reg::RSP,
                op(NdOp::INT_ADD, {Address, k(F.Bytes)}));
      W.leave(Exit::Continue, k(NextPC));
    });
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSEGMENTMOVES_H
