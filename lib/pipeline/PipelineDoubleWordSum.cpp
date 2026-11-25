//===- PipelineDoubleWordSum.cpp - Register-pair addition evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recover a double-word sum in a leaf selected without its callers.
///
//===----------------------------------------------------------------------===//

#include "PipelineWideIntReturnDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/IntrinsicValueEffects.h"
#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/ir/med/IndirectCallSlot.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <map>
#include <utility>

namespace neverd {
namespace {

class DoubleWordSum {
  const MedFunc &Func;
  MedDefinitionIndex Definitions;
  const TargetRegInfo &TRI;
  std::map<const MedOp *, const MedVar *> Reloads;

  // Only whole, aligned words in the active frame can transport inputs here.
  // In particular a caller-visible store of an intermediate sum cannot turn
  // a scalar result into a pair. Group an update-form store with its SP write.
  bool indexSpills() {
    EntryStackOffsetProof Frame(Func, TRI.TheArch, BinaryFormat::ELF,
                                Func.EntryKind);
    const int64_t Word = TRI.integerReturnWordBytes();
    std::map<int64_t, const MedVar *> Slots;
    int64_t Stack = 0;
    bool Releasing = false;
    auto Ops = llvm::ArrayRef(Func.Blocks.front().Ops);
    while (!Ops.empty()) {
      const auto Insn =
          Ops.take_while([Addr = Ops.front().Addr](const MedOp &Op) {
            return Op.Addr == Addr;
          });
      int64_t NextStack = Stack;
      for (const MedOp &Op : Insn)
        if (Op.Output.Kind == MedVar::Reg && Op.Output.Size &&
            Op.Output.RegOff == TRI.StackPointer) {
          auto Offset = Frame.offset(Op.Output);
          if (!Offset || *Offset > 0 || (Releasing && *Offset < Stack))
            return false;
          NextStack = *Offset;
        }
      for (const MedOp &Op : Insn) {
        if (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE)
          continue;
        const bool Store = Op.Opcode == NdOp::STORE;
        const MedVar &Value = Store ? Op.Inputs[1] : Op.Output;
        auto Offset = Frame.offset(Op.Inputs[0]);
        if (!Offset || *Offset % Word || *Offset < std::min(Stack, NextStack) ||
            *Offset > -Word || Value.Size != Word ||
            Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          return false;
        if (Store)
          Slots[*Offset] = &Value;
        else if (auto It = Slots.find(*Offset); It != Slots.end())
          Reloads.emplace(&Op, It->second);
        else
          return false;
      }
      Releasing |= NextStack > Stack;
      Stack = NextStack;
      Ops = Ops.drop_front(Insn.size());
    }
    return Stack == 0;
  }

  // In a straight-line SSA block a copy names an earlier value, except for
  // the entry identity which names the incoming register itself.
  MedVar source(MedVar V) {
    while (!V.isConst()) {
      const MedOp *D = Definitions.find(V);
      if (!D)
        break;
      if (D->Opcode == NdOp::COPY && D->Inputs[0] != V &&
          D->Inputs[0].Size == V.Size)
        V = D->Inputs[0];
      else if (const auto It = Reloads.find(D); It != Reloads.end())
        V = *It->second;
      else
        break;
    }
    return V;
  }

  const MedOp *operation(MedVar V, NdOp Opcode) {
    V = source(V);
    const MedOp *D = V.isConst() ? nullptr : Definitions.find(V);
    return D && D->Opcode == Opcode ? D : nullptr;
  }

  bool incoming(MedVar V, uint64_t Register) {
    V = source(V);
    if (V.Kind != MedVar::Reg || V.RegOff != Register ||
        V.Size != TRI.integerReturnWordBytes())
      return false;
    const MedOp *D = Definitions.find(V);
    return D ? D->Opcode == NdOp::COPY && D->Inputs[0] == V : V.SSAVer == 0;
  }

  bool addsArgumentWords(const MedOp &Add, bool High) {
    const unsigned Lane = High != TRI.IntegerReturnPairHighFirst;
    const auto A = TRI.IntParamRegs[Lane], B = TRI.IntParamRegs[2 + Lane];
    return (incoming(Add.Inputs[0], A) && incoming(Add.Inputs[1], B)) ||
           (incoming(Add.Inputs[0], B) && incoming(Add.Inputs[1], A));
  }

  // A packed status register can transport the carry between instructions.
  // Match the read of one bit and the reaching write of that same bit. Other
  // bits of the old register can have arbitrary values.
  const MedOp *unpackedCarry(MedVar V) {
    const MedOp *Byte = operation(V, NdOp::SUBBYTES);
    if (!Byte || !Byte->Inputs[1].isConst() || Byte->Inputs[1].ConstVal != 0)
      return nullptr;
    const MedOp *Mask = operation(Byte->Inputs[0], NdOp::INT_AND);
    if (!Mask || !Mask->Inputs[1].isConst() || Mask->Inputs[1].ConstVal != 1)
      return nullptr;
    const MedOp *Read = operation(Mask->Inputs[0], NdOp::INT_RIGHT);
    if (!Read || !Read->Inputs[1].isConst() ||
        Read->Inputs[1].ConstVal >= TRI.integerReturnWordBytes() * 8)
      return nullptr;
    const MedOp *Pack = operation(Read->Inputs[0], NdOp::INT_OR);
    if (!Pack)
      return nullptr;
    const MedOp *Keep = operation(Pack->Inputs[0], NdOp::INT_AND);
    const MedOp *Set = operation(Pack->Inputs[1], NdOp::INT_LEFT);
    const uint64_t Bit = uint64_t{1} << Read->Inputs[1].ConstVal;
    if (!Keep || !Keep->Inputs[1].isConst() ||
        (Keep->Inputs[1].ConstVal & Bit) || !Set ||
        Set->Inputs[1] != Read->Inputs[1])
      return nullptr;
    const MedOp *Extend = operation(Set->Inputs[0], NdOp::INT_ZEXT);
    return Extend ? operation(Extend->Inputs[0], NdOp::INT_CARRY) : nullptr;
  }

  bool carries(const MedVar &V, const MedOp &Low) {
    const MedOp *Extend = operation(V, NdOp::INT_ZEXT);
    if (!Extend)
      return false;
    const MedVar &Bit = Extend->Inputs[0];
    if (const MedOp *Cmp = operation(Bit, NdOp::INT_LESS)) {
      const MedVar Operand = source(Cmp->Inputs[1]);
      return source(Cmp->Inputs[0]) == Low.Output &&
             (Operand == source(Low.Inputs[0]) ||
              Operand == source(Low.Inputs[1]));
    }
    const MedOp *Carry = unpackedCarry(Bit);
    return Carry && addsArgumentWords(*Carry, /*High=*/false);
  }

public:
  DoubleWordSum(const MedFunc &Func, const TargetRegInfo &TRI)
      : Func(Func), Definitions(Func), TRI(TRI) {}

  bool matches(const MedVar &LowResult, const MedVar &HighResult,
               bool HasMemory) {
    const MedOp *Low = operation(LowResult, NdOp::INT_ADD);
    const MedOp *High = operation(HighResult, NdOp::INT_ADD);
    if (!Low || !High || (HasMemory && !indexSpills()) ||
        !addsArgumentWords(*Low, /*High=*/false))
      return false;
    for (unsigned I : {0u, 1u}) {
      const MedOp *Sum = operation(High->Inputs[I], NdOp::INT_ADD);
      if (Sum && addsArgumentWords(*Sum, /*High=*/true) &&
          carries(High->Inputs[1 - I], *Low))
        return true;
    }
    return false;
  }
};

} // namespace

bool returnsDoubleWordSum(const MedFunc &Func, const TargetRegInfo &TRI) {
  if (Func.SkippedSSA || Func.SourceTypeHint || Func.SourceParametersBound ||
      !Func.MultiReturn.empty() || Func.Blocks.size() != 1)
    return false;
  const MedBlock &Block = Func.Blocks.front();
  if (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN ||
      !Block.Phis.empty())
    return false;

  // Both final register writes must be meaningful. A stored wide intermediate
  // followed by a scalar return, or a call that clobbers a carrier, supplies
  // no standalone result evidence.
  const MedVar *First = nullptr, *Second = nullptr;
  bool HasMemory = false, HasCarry = false;
  for (const MedOp &Op : llvm::drop_end(Block.Ops)) {
    if (Op.Output.Kind == MedVar::Reg && Op.Output.Size) {
      if (Op.Output.RegOff == TRI.IntReturnReg)
        First = &Op.Output;
      if (Op.Output.RegOff == TRI.IntReturnReg2)
        Second = &Op.Output;
    }
    if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) {
      HasMemory = true;
      continue;
    }
    // An RV32I return checks the alignment of ra. A fault check leaves the
    // result registers intact on the path that actually returns.
    if (hasValidMedIntrinsicId(Op) &&
        onlyChecksForAFault(static_cast<Intrinsic>(Op.Inputs[0].ConstVal)))
      continue;
    if (!isSourceFrameScalarTransfer(Op.Opcode) ||
        Op.Output.Kind == MedVar::Stack)
      return false;
    HasCarry |= Op.Opcode == NdOp::INT_CARRY || Op.Opcode == NdOp::INT_LESS;
  }
  if (!HasCarry || !First || !Second || First->SSAVer == 0 ||
      Second->SSAVer == 0 || First->Size != TRI.integerReturnWordBytes() ||
      First->Size != Second->Size)
    return false;
  if (TRI.IntegerReturnPairHighFirst)
    std::swap(First, Second);
  return DoubleWordSum(Func, TRI).matches(*First, *Second, HasMemory);
}

} // namespace neverd
