//===- X64ContinuationMpx.h - Explicit MPX source state --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Long-mode MPX source projection with explicit bank and #BR boundaries.
/// The profile owns successful sequential RAM; native fault delivery and
/// transactional memory are not inferred from these value operations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONMPX_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONMPX_H

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/ir/X86/X86MpxSemantics.h"
#include "neverd/lift/X86/X86MpxEncoding.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstddef>
#include <cstdint>

namespace neverd::analysis::detail {

template <class Writer> class X64ContinuationMpx {
  using State = X64ContinuationStateV1;
  using Mpx = X64ContinuationStateV11;
  using Exit = X64ContinuationExitV1;
  Writer &W;
  static constexpr auto Mode = X86MpxMode::Bits64;

  NdVar binary(NdOp Op, NdVar A, NdVar B, uint16_t Bytes = 8) {
    const auto V = W.temporary(Bytes);
    W.emit(Op, V, {A, B});
    return V;
  }
  NdVar equal(NdVar A, NdVar B) { return binary(NdOp::INT_EQUAL, A, B, 1); }
  NdVar select(NdVar Condition, NdVar A, NdVar B) {
    const auto V = W.temporary();
    W.emit(NdOp::SELECT, V, {Condition, A, B});
    return V;
  }
  NdVar general(x86_reg Register, va_t NextIP) {
    if (Register == X86_REG_INVALID)
      return constant(0);
    if (Register == X86_REG_RIP)
      return constant(NextIP);
    return W.read(NdVar::reg(mapCapstoneReg(Register).Offset, 8));
  }
  NdVar effective(const x86_op_mem &M, va_t NextIP) {
    return add(
        add(general(M.base, NextIP), constant(uint64_t(M.disp))),
        binary(NdOp::INT_MULT, general(M.index, NextIP), constant(M.scale)));
  }
  NdVar segmentBase(x86_reg Segment) {
    if (Segment == X86_REG_FS)
      return W.load(offsetof(State, FSBase));
    if (Segment == X86_REG_GS)
      return W.load(offsetof(State, GSBase));
    return constant(0);
  }
  NdVar bound(unsigned Register, unsigned Part) {
    return W.load(offsetof(Mpx, Bounds) + Register * 16 + Part * 8);
  }
  void writeBound(unsigned Register, NdVar Lower, NdVar Upper) {
    W.store(offsetof(Mpx, Bounds) + Register * 16, Lower);
    W.store(offsetof(Mpx, Bounds) + Register * 16 + 8, Upper);
  }
  NdVar memory(NdVar Address, uint64_t Offset = 0) {
    const auto V = W.temporary();
    W.emit(NdOp::LOAD, V, {add(Address, constant(Offset))});
    return V;
  }
  void storeMemory(NdVar Address, uint64_t Offset, NdVar V) {
    W.emit(NdOp::STORE, {}, {add(Address, constant(Offset)), V});
  }
  void faultIf(NdVar Failed, NdVar Status) {
    W.exitIf(Failed, [&] {
      W.store(offsetof(Mpx, BNDSTATUS), Status);
      W.leave(Exit::BoundsFault);
    });
  }

public:
  using Value = NdVar;
  explicit X64ContinuationMpx(Writer &W) : W(W) {}
  static NdVar constant(uint64_t V) { return NdVar::scalar(V, 8); }
  NdVar add(NdVar A, NdVar B) { return binary(NdOp::INT_ADD, A, B); }
  NdVar bitAnd(NdVar A, NdVar B) { return binary(NdOp::INT_AND, A, B); }
  NdVar bitXor(NdVar A, NdVar B) { return binary(NdOp::INT_XOR, A, B); }
  NdVar left(NdVar A, NdVar B) { return binary(NdOp::INT_LEFT, A, B); }
  NdVar right(NdVar A, NdVar B) { return binary(NdOp::INT_RIGHT, A, B); }
  NdVar less(NdVar A, NdVar B) { return binary(NdOp::INT_LESS, A, B, 1); }

  void validate() {
    for (auto Offset : {offsetof(Mpx, MPXSupported), offsetof(Mpx, OSXSAVE)})
      W.rejectIf(
          binary(NdOp::INT_LESS, NdVar::scalar(1, 1), W.load(Offset, 1), 1));
    W.rejectIf(binary(NdOp::INT_NOTEQUAL,
                      W.load(offsetof(Mpx, InTransaction), 1),
                      NdVar::scalar(0, 1), 1));
    W.rejectIf(binary(
        NdOp::INT_NOTEQUAL,
        bitAnd(W.load(offsetof(Mpx, BNDCFGS)), constant(X86MpxConfigReserved)),
        constant(0), 1));
  }

  NdVar enabled() {
    auto Enabled =
        binary(NdOp::BOOL_AND, W.load(offsetof(Mpx, MPXSupported), 1),
               W.load(offsetof(Mpx, OSXSAVE), 1), 1);
    Enabled = binary(
        NdOp::BOOL_AND, Enabled,
        equal(bitAnd(W.load(offsetof(Mpx, XCR0)), constant(X86MpxXcr0Mask)),
              constant(X86MpxXcr0Mask)),
        1);
    return binary(NdOp::BOOL_AND, Enabled,
                  equal(bitAnd(W.load(offsetof(Mpx, BNDCFGS)),
                               constant(X86MpxConfigEnable)),
                        constant(X86MpxConfigEnable)),
                  1);
  }

  void execute(const X86MpxEncoding &E, va_t Fallthrough) {
    W.exitIf(equal(enabled(), NdVar::scalar(0, 1)),
             [&] { W.leave(Exit::Continue, constant(Fallthrough)); });
    const auto &X = E.Detail;
    const bool Store = X.operands[0].type == X86_OP_MEM;
    const unsigned Bank = X.operands[Store ? 1 : 0].reg - X86_REG_BND0;
    const auto &Operand = X.operands[Store ? 0 : 1];
    if (E.Id == X86_INS_BNDMOV) {
      if (Operand.type == X86_OP_REG) {
        const unsigned Source = Operand.reg - X86_REG_BND0;
        const auto Lower = bound(Source, 0), Upper = bound(Source, 1);
        writeBound(Bank, Lower, Upper);
      } else {
        const auto Address = add(effective(Operand.mem, Fallthrough),
                                 segmentBase(Operand.mem.segment));
        if (Store) {
          const auto Lower = bound(Bank, 0), Upper = bound(Bank, 1);
          storeMemory(Address, 0, Lower);
          storeMemory(Address, 8, Upper);
        } else {
          const auto Lower = memory(Address), Upper = memory(Address, 8);
          writeBound(Bank, Lower, Upper);
        }
      }
    } else if (E.Id == X86_INS_BNDMK) {
      writeBound(
          Bank, general(Operand.mem.base, Fallthrough),
          bitXor(effective(Operand.mem, Fallthrough), constant(UINT64_MAX)));
    } else if (E.Id == X86_INS_BNDCL || E.Id == X86_INS_BNDCU ||
               E.Id == X86_INS_BNDCN) {
      const auto V = Operand.type == X86_OP_REG
                         ? general(Operand.reg, Fallthrough)
                         : effective(Operand.mem, Fallthrough);
      const auto Kind = E.Id == X86_INS_BNDCL   ? X86MpxCheck::Lower
                        : E.Id == X86_INS_BNDCU ? X86MpxCheck::UpperComplement
                                                : X86MpxCheck::UpperRaw;
      faultIf(
          X86MpxSemantics(*this).failed(
              Mode, Kind, V, bound(Bank, Kind == X86MpxCheck::Lower ? 0 : 1)),
          constant(1));
    } else {
      // MIB scale is ignored and an absent base discards displacement. Only
      // the translated directory/table is memory, never the pointer operand.
      const auto &M = Operand.mem;
      const auto Offset =
          M.base == X86_REG_INVALID
              ? constant(0)
              : add(general(M.base, Fallthrough), constant(uint64_t(M.disp)));
      const auto PointerAddress = add(Offset, segmentBase(M.segment));
      const auto PointerValue = general(M.index, Fallthrough);
      X86MpxSemantics Semantics(*this);
      const auto Directory = Semantics.directory(
          Mode, W.load(offsetof(Mpx, BNDCFGS)), PointerAddress, 0);
      const auto Entry = memory(Directory);
      faultIf(equal(bitAnd(Entry, constant(1)), constant(0)),
              binary(NdOp::INT_OR, Directory, constant(2)));
      const auto Table = Semantics.table(Mode, Entry, PointerAddress);
      if (Store) {
        const auto Lower = bound(Bank, 0), Upper = bound(Bank, 1);
        storeMemory(Table, 0, Lower);
        storeMemory(Table, 8, Upper);
        storeMemory(Table, 16, PointerValue);
      } else {
        const auto Lower = memory(Table), Upper = memory(Table, 8);
        const auto Match = equal(memory(Table, 16), PointerValue);
        writeBound(Bank, select(Match, Lower, constant(0)),
                   select(Match, Upper, constant(0)));
      }
    }
    W.leave(Exit::Continue, constant(Fallthrough));
  }

  void completeBranch(X86MpxBranch Effect) {
    if (Effect == X86MpxBranch::Preserve)
      return;
    const auto Initialize = binary(NdOp::BOOL_AND, enabled(),
                                   equal(bitAnd(W.load(offsetof(Mpx, BNDCFGS)),
                                                constant(X86MpxConfigPreserve)),
                                         constant(0)),
                                   1);
    for (unsigned I = 0; I != X86MpxRegisterCount; ++I) {
      const auto Lower = select(Initialize, constant(0), bound(I, 0));
      const auto Upper = select(Initialize, constant(0), bound(I, 1));
      writeBound(I, Lower, Upper);
    }
  }
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONMPX_H
