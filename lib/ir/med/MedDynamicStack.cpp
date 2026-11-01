//===- MedDynamicStack.cpp - Dynamic stack allocations --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The classification of run-time stack allocations both routes lower: the
/// LLVM emitter turns one into an `alloca`, and the C writer into
/// `__builtin_alloca`.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedDynamicStack.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"

#include <algorithm>
#include <functional>
#include <set>

namespace neverd {

uint64_t dynamicStackCallArea(Arch Architecture, BinaryFormat Format) {
  return getTargetRegInfo(Architecture)
      .integerArgumentLayout(Format)
      .CallStackBase;
}

bool medStackPointerDerived(const MedVar &V, const MedDynamicStackLookups &L,
                            int Depth) {
  // Cycle-detecting walk: a loop-carried stack pointer is defined by a PHI
  // whose back-edge feeds itself (`sp_next = phi(sp_init, sp_next - size)`),
  // so the recursion must not re-enter a PHI it is already exploring.
  std::set<std::pair<int, int>> VisitedPhis;
  std::function<bool(const MedVar &, int)> Rec = [&](const MedVar &Cur,
                                                     int D) -> bool {
    if (Cur.isConst() || !L.Func || D > limits::kMaxStackPtrTraceDepth)
      return false;
    if (Cur.Kind == MedVar::Reg && Cur.SSAVer == 0 && L.StackPointer != 0 &&
        Cur.RegOff == L.StackPointer)
      return true;
    // An in-loop dynamic allocation subtracts from the loop-carried SP, which
    // is a PHI (`sp_next = phi(sp_entry, sp_prev - size)`).  The PHI is
    // stack-pointer-derived when any incoming value is; the visited set breaks
    // the self-referential back-edge so the cycle terminates (the entry edge
    // reaches the entry SP and proves it).  Without this an alloca inside a
    // loop is left as raw integer SP arithmetic into the fixed logical frame,
    // so its memory aliases unreserved stack a later call clobbers.
    if (const PhiNode *Phi = L.Phi ? L.Phi(Cur) : nullptr) {
      if (!VisitedPhis.insert({Cur.Id, Cur.SSAVer}).second)
        return false;
      for (const auto &A : Phi->Args)
        if (Rec(A.second, D + 1))
          return true;
      return false;
    }
    const MedOp *Def = L.Definition ? L.Definition(Cur) : nullptr;
    if (!Def || Def->NumInputs < 1)
      return false;
    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::SUBBYTES:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::INT_AND:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      return Rec(Def->Inputs[0], D + 1);
    default:
      return false;
    }
  };
  return Rec(V, Depth);
}

bool medValueReaches(const MedVar &V, const MedVar &Target,
                     const MedDynamicStackLookups &L, int Depth) {
  if (!L.Func || Depth > limits::kMaxStackPtrTraceDepth)
    return false;
  if (V.Kind == Target.Kind && V.Id == Target.Id && V.SSAVer == Target.SSAVer)
    return true;
  if (V.isConst())
    return false;
  const MedOp *Def = L.Definition ? L.Definition(V) : nullptr;
  if (!Def || Def->NumInputs < 1)
    return false;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return medValueReaches(Def->Inputs[0], Target, L, Depth + 1);
  default:
    return false;
  }
}

std::pair<int, int> medSizeRootKey(const MedVar &V,
                                   const MedDynamicStackLookups &L, int Depth) {
  if (!L.Func || V.isConst() || Depth > limits::kMaxStackPtrTraceDepth)
    return {V.Id, V.SSAVer};
  const MedOp *Def = L.Definition ? L.Definition(V) : nullptr;
  if (!Def || Def->NumInputs < 1)
    return {V.Id, V.SSAVer};
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return medSizeRootKey(Def->Inputs[0], L, Depth + 1);
  case NdOp::SUBBYTES:
    return Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
                   Def->Inputs[1].ConstVal == 0
               ? medSizeRootKey(Def->Inputs[0], L, Depth + 1)
               : std::make_pair(V.Id, V.SSAVer);
  default:
    return {V.Id, V.SSAVer};
  }
}

bool medReloadedStackPointer(const MedVar &V, const MedDynamicStackLookups &L,
                             int Depth) {
  if (!L.Func || V.isConst() || Depth > limits::kMaxStackPtrTraceDepth)
    return false;
  const MedOp *Def = L.Definition ? L.Definition(V) : nullptr;
  if (!Def || Def->NumInputs < 1)
    return false;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return medReloadedStackPointer(Def->Inputs[0], L, Depth + 1);
  case NdOp::SUBBYTES:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
        Def->Inputs[1].ConstVal == 0)
      return medReloadedStackPointer(Def->Inputs[0], L, Depth + 1);
    return false;
  case NdOp::LOAD: {
    if (Def->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !L.ReloadSources)
      return false;
    std::vector<MedVar> Sources;
    if (!L.ReloadSources(*Def, Sources) || Sources.empty())
      return false;
    return std::all_of(Sources.begin(), Sources.end(),
                       [&](const MedVar &Source) {
                         return medStackPointerDerived(Source, L);
                       });
  }
  default:
    return false;
  }
}

const MedVar *medDynamicStackAllocationSize(const MedOp &Op,
                                            const MedDynamicStackLookups &L) {
  if (Op.Opcode != NdOp::INT_SUB || Op.NumInputs < 2 || !L.Func)
    return nullptr;
  const MedVar &Size = Op.Inputs[1];
  // A constant decrement is the fixed prologue frame, not a dynamic
  // allocation.
  if (Size.isConst())
    return nullptr;
  // `sp - size`: minuend is the stack pointer, subtrahend a runtime byte count
  // (and not itself a stack pointer, which would be a pointer difference).
  if (!medStackPointerDerived(Op.Inputs[0], L) ||
      medStackPointerDerived(Size, L))
    return nullptr;
  // The result must become the new stack pointer: written straight to SP
  // (`sub sp,sp,xN`) or copied there through a width chain (i386 routes the
  // value `ecx -> esp`).  This rules out any other `sp - value`.
  if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == L.StackPointer)
    return &Size;
  for (const auto &Blk : L.Func->Blocks)
    for (const auto &O : Blk.Ops)
      if (O.Output.Kind == MedVar::Reg && O.Output.RegOff == L.StackPointer &&
          O.NumInputs >= 1 && medValueReaches(O.Inputs[0], Op.Output, L))
        return &Size;
  return nullptr;
}

bool medTryDynamicStackAddressSizes(
    const MedOp &Op, const MedDynamicStackLookups &L,
    llvm::function_ref<bool(const MedVar &Size)> Try) {
  if (Op.NumInputs < 2 ||
      (Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB))
    return false;

  // The negation behind an INT_ADD's offset (`old_sp + (-size)`): look through
  // the width casts the negated size threads through, then follow an INT_NEG2
  // or `0 - size` to the size itself.  Returns the size operand, or nullptr
  // when the offset is not a negated runtime value.
  auto NegatedSize = [&](const MedVar &OffIn) -> const MedVar * {
    MedVar Off = OffIn;
    for (int Guard = 0; Guard <= limits::kMaxStackPtrTraceDepth; ++Guard) {
      const MedOp *Def = L.Definition ? L.Definition(Off) : nullptr;
      if (!Def || Def->NumInputs < 1)
        return nullptr;
      switch (Def->Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
        Off = Def->Inputs[0];
        continue;
      case NdOp::SUBBYTES:
        if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
            Def->Inputs[1].ConstVal == 0) {
          Off = Def->Inputs[0];
          continue;
        }
        return nullptr;
      case NdOp::INT_NEG2:
        return &Def->Inputs[0];
      case NdOp::INT_SUB:
        if (Def->NumInputs >= 2 && Def->Inputs[0].isConst() &&
            Def->Inputs[0].ConstVal == 0)
          return &Def->Inputs[1];
        return nullptr;
      default:
        return nullptr;
      }
    }
    return nullptr;
  };

  // A candidate (stack-pointer base, runtime size) pair of this op.
  auto Pair = [&](const MedVar &Base, const MedVar *Size) {
    return Size && !Size->isConst() &&
           (medStackPointerDerived(Base, L) ||
            medReloadedStackPointer(Base, L)) &&
           Try(*Size);
  };

  if (Op.Opcode == NdOp::INT_SUB)
    return Pair(Op.Inputs[0], &Op.Inputs[1]);
  // INT_ADD: either operand may be the SP base; the other must be a negated
  // size.
  return Pair(Op.Inputs[0], NegatedSize(Op.Inputs[1])) ||
         Pair(Op.Inputs[1], NegatedSize(Op.Inputs[0]));
}

} // namespace neverd
