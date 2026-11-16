//===- LiftCommon.h - Common types and base for instruction lifters -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares RegInfo (shared register descriptor) and LiftStateBase (common
/// per-instruction lifting context) used by all architecture-specific lifters.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_LIFTCOMMON_H
#define NEVERD_LIFT_LIFTCOMMON_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace neverd {

/// Descriptor returned by architecture-specific mapCapstoneReg() functions.
struct RegInfo {
  uint64_t Offset;
  uint16_t Size;
};

/// Common per-instruction lifting state shared by X86Lifter, ARMLifter, and
/// AArch64Lifter.  Holds mutable counters that advance as LowOps are emitted
/// and provides the emit helpers every category dispatcher needs.
struct LiftStateBase {
  va_t Addr;
  uint16_t InsnSize;
  int Seq = 0;
  uint64_t TmpId = 0;
  std::vector<LowOp> &Ops;
  size_t OpsStart;

  LiftStateBase(va_t A, uint16_t ISz, std::vector<LowOp> &O)
      : Addr(A), InsnSize(ISz), Ops(O), OpsStart(O.size()) {}

  NdVar makeTemp(uint16_t Sz) {
    // TEMP offsets name byte storage, including in symbolic execution. Reserve
    // the complete extent before allocating another value. Small values keep
    // their existing eight-byte spacing; zero-sized accumulator placeholders
    // still consume one slot and must be replaced before emitting a value.
    const uint64_t Slots = Sz ? (uint64_t{Sz} + TmpStride - 1) / TmpStride : 1;
    constexpr uint64_t LastSlot = (UINT64_MAX - TmpBase) / TmpStride;
    if (TmpId > LastSlot || Slots - 1 > LastSlot - TmpId)
      llvm::report_fatal_error(
          llvm::Twine("LiftStateBase: temporary byte storage exhausted "
                      "(instruction address ") +
          llvm::Twine(Addr) + ")");
    const uint64_t Offset = TmpBase + TmpId * TmpStride;
    // LastSlot + 1 fits the slot cursor and represents exhausted storage.
    TmpId += Slots;
    return NdVar::tmp(Offset, Sz);
  }

  void emit(
      NdOp Opc, NdVar Out, std::initializer_list<NdVar> Ins,
      NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None,
      NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default) {
    if ((Opc == NdOp::INT_ZEXT || Opc == NdOp::INT_SEXT) && Ins.size() == 1) {
      const NdVar &In = *Ins.begin();
      if (In.Size > Out.Size)
        llvm::report_fatal_error(
            llvm::Twine("LiftStateBase: integer extension input must be "
                        "narrower than output") +
            " (instruction address " + llvm::Twine(Addr) + ", input bytes " +
            llvm::Twine(In.Size) + ", output bytes " + llvm::Twine(Out.Size) +
            ")");
      if (In.Size == Out.Size)
        Opc = NdOp::COPY;
    }

    // Numeric operation operands are scalar occurrences unless the decoder or
    // loader attached a more precise address role to this exact field.  Keep
    // this at the shared emission boundary: architecture helpers synthesize
    // masks, shift counts, bounds, and strides in many different files, and a
    // low-VA image can otherwise make an untagged value such as 0xff look like
    // unrelated object data.  COPY/SELECT/value-transport and memory/control
    // operands deliberately stay untouched because their constants may be
    // first-class data or code pointers.
    // Check the complete operand list before publishing an operation in place.
    if (Ins.size() > kLowOperationInputs)
      throw std::length_error(kLowOperationInputOverflow);
    auto &Op = Ops.emplace_back();
    Op.Opcode = Opc;
    Op.MemoryOrdering = MemoryOrdering;
    Op.MemoryAddressSpace = MemoryAddressSpace;
    Op.Addr = Addr;
    Op.Seq = Seq++;
    Op.Output = Out;
    for (NdVar V : Ins) {
      if (V.isConst() && V.Provenance == ConstantAddressProvenance::Unknown &&
          isNumericConstantOperand(Opc, Op.NumInputs))
        V.Provenance = ConstantAddressProvenance::Scalar;
      Op.Inputs[Op.NumInputs++] = V;
    }
  }

  void emitIntrinsic(
      Intrinsic Id, NdVar Out, std::initializer_list<NdVar> Extra = {},
      NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None,
      NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default) {
    if (Extra.size() >= kLowOperationInputs)
      throw std::length_error(kLowOperationInputOverflow);
    auto &Op = Ops.emplace_back();
    Op.Opcode = NdOp::INTRINSIC;
    Op.MemoryOrdering = MemoryOrdering;
    Op.MemoryAddressSpace = MemoryAddressSpace;
    Op.Addr = Addr;
    Op.Seq = Seq++;
    Op.Output = Out;
    Op.Inputs[Op.NumInputs++] = NdVar::cst(static_cast<uint64_t>(Id), 2);
    for (const auto &V : Extra)
      Op.Inputs[Op.NumInputs++] = V;
  }

  /// Emit a side-effect-only intrinsic (a barrier / fence / prefetch) that
  /// produces no value.  The INTRINSIC carries no output nd-var, so SSA
  /// construction never bumps a register version for it: giving such a barrier
  /// a register destination would shadow a live value with the op's
  /// never-assigned output (e.g. an ARM `dmb` between an LCG update and the
  /// following `lsr` clobbering the just-computed result to a default zero).
  void emitVoidIntrinsic(Intrinsic Id,
                         std::initializer_list<NdVar> Extra = {}) {
    emitIntrinsic(Id, NdVar(), Extra);
  }

  /// Emit a NEON rounding shift-right by immediate (SRSHR/URSHR/VRSHR), adding
  /// the 1<<(ShAmt-1) bias in precision wider than the lane so a max-range
  /// value (e.g. 0xFFFFFFFF >> 1) does not overflow the lane to 0.  Mirrors
  /// Unicorn's neon_rshl / handle_shri_with_rndacc 64-bit accumulator.  ShAmt
  /// must be a constant in 1..ElemSz*8; the value is read from / written to a
  /// LaneSz-byte nd-var.
  NdVar emitRoundedShr(NdVar Lane, uint16_t LaneSz, unsigned ShAmt,
                       bool IsSigned) {
    uint16_t WideSz = LaneSz * 2;
    NdVar Wide = makeTemp(WideSz);
    emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Wide, {Lane});
    NdVar Biased = makeTemp(WideSz);
    emit(NdOp::INT_ADD, Biased,
         {Wide, NdVar::cst(1ULL << (ShAmt - 1), WideSz)});
    NdVar Shifted = makeTemp(WideSz);
    emit(IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT, Shifted,
         {Biased, NdVar::cst(ShAmt, WideSz)});
    NdVar Out = makeTemp(LaneSz);
    emit(NdOp::SUBBYTES, Out, {Shifted, NdVar::cst(0, 4)});
    return Out;
  }
};

} // namespace neverd

#endif // NEVERD_LIFT_LIFTCOMMON_H
