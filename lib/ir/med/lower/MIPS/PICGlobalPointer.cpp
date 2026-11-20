//===- PICGlobalPointer.cpp - MIPS PIC entry GP projection ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PICGlobalPointer.h"

#include "neverd/ir/low/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"

namespace neverd {
namespace {

/// A scalar displacement or the entry $t9 plus a scalar displacement.
enum class Origin { Scalar, Entry, AdjustedEntry };
struct Value {
  uint64_t Bits;
  Origin Base;
  bool SignedWord;
  uint16_t Size = 0;
};

class EntryValues {
  llvm::SmallDenseMap<uint64_t, Value, 8> Registers, Temps;
  bool WordPointers;

  std::optional<Value> read(const NdVar &V) const {
    if (V.isConst() && !isAddressProvenance(V.Provenance))
      return Value{V.Offset, Origin::Scalar, false, V.Size};
    const auto &Values = V.isReg() ? Registers : Temps;
    auto It = Values.find(V.Offset);
    if ((!V.isReg() && !V.isTemp()) || It == Values.end() ||
        It->second.Size != V.Size)
      return std::nullopt;
    return It->second;
  }

  std::optional<Value> result(const LowOp &Op) const {
    if (!Op.NumInputs)
      return std::nullopt;
    auto A = read(Op.Inputs[0]);
    if (!A)
      return std::nullopt;
    if (Op.Opcode == NdOp::COPY && Op.Output.Size == Op.Inputs[0].Size)
      return A;
    if (Op.Opcode == NdOp::INT_SEXT && Op.Inputs[0].Size == 4 &&
        Op.Output.Size == 8 && (WordPointers || A->Base == Origin::Scalar))
      return Value{uint64_t(llvm::SignExtend64<32>(A->Bits)), A->Base, true};
    if (Op.NumInputs != 2)
      return std::nullopt;
    if (Op.Opcode == NdOp::SUBBYTES && Op.Inputs[0].Size == 8 &&
        Op.Output.Size == 4 && Op.Inputs[1].isConst() && !Op.Inputs[1].Offset &&
        (WordPointers || A->Base == Origin::Scalar))
      return Value{uint32_t(A->Bits), A->Base, false};
    if (Op.Opcode != NdOp::INT_ADD ||
        (Op.Output.Size != 8 && Op.Output.Size != 4))
      return std::nullopt;
    const auto B = read(Op.Inputs[1]);
    if (!B || (A->Base != Origin::Scalar && B->Base != Origin::Scalar))
      return std::nullopt;
    const bool Relative =
        A->Base != Origin::Scalar || B->Base != Origin::Scalar;
    if (Op.Output.Size == 4 && Relative && !WordPointers)
      return std::nullopt;
    const uint64_t Sum = A->Bits + B->Bits;
    return Value{Op.Output.Size == 4 ? uint32_t(Sum) : Sum,
                 Relative ? Origin::AdjustedEntry : Origin::Scalar, false};
  }

public:
  EntryValues(va_t Entry, bool WordPointers) : WordPointers(WordPointers) {
    Registers[mipsreg::T9] = {
        WordPointers ? uint64_t(llvm::SignExtend64<32>(Entry)) : Entry,
        Origin::Entry, WordPointers, 8};
  }

  void nextInstruction() { Temps.clear(); }

  std::optional<Value> write(const LowOp &Op) {
    auto V = result(Op);
    auto &Values = Op.Output.isReg() ? Registers : Temps;
    if (V) {
      V->Size = Op.Output.Size;
      Values[Op.Output.Offset] = *V;
    } else {
      Values.erase(Op.Output.Offset);
    }
    return V;
  }

  std::optional<uint64_t> knownWord(const NdVar &V) const {
    auto A = read(V);
    if (A && A->Base != Origin::Scalar && A->SignedWord)
      return A->Bits;
    return std::nullopt;
  }
};

} // namespace

std::optional<LowFunc> projectMIPSPICGlobalPointer(const LowFunc &Function,
                                                   const BinaryImage &Image) {
  if (!Image.MIPS || !Image.MIPS->PICGP || !isMIPS64(Image.Arch) ||
      Function.Blocks.empty())
    return std::nullopt;
  const LowBlock &Entry = Function.Blocks.front();
  if (Entry.StartAddr != Function.Entry || !Entry.Preds.empty() ||
      !Entry.ExceptionalPreds.empty())
    return std::nullopt;

  const bool WordPointers = hasMIPS32BitAddressIn64BitGPR(Image.Arch);
  EntryValues Values(Function.Entry, WordPointers);
  size_t End = 0;
  llvm::SmallVector<std::pair<size_t, uint64_t>, 4> WordChecks;
  for (const auto &Insn : Entry.InstructionBoundaries) {
    if (isMIPSCompressedMode(Insn.Mode) ||
        Insn.Control != LowInstructionControl::None)
      break;
    // Every preceding write to a carrier must belong to the decoded prefix.
    if (Insn.FirstOp != End || Insn.OpCount > Entry.Ops.size() - End)
      return std::nullopt;
    End += Insn.OpCount;
    Values.nextInstruction();
    for (size_t I = Insn.FirstOp; I < End; ++I) {
      const LowOp &Op = Entry.Ops[I];
      if (Op.Opcode == NdOp::INTRINSIC) {
        if (Op.NumInputs && Op.Inputs[0].isConst() &&
            Op.Inputs[0].Offset == unsigned(Intrinsic::RequireAligned))
          continue;
        if (!mipsScalarLowShapeIsValid(Op, Image.Arch) ||
            Op.Inputs[0].Offset != unsigned(Intrinsic::MIPSRequireWord))
          return std::nullopt;
        if (auto Bits = Values.knownWord(Op.Inputs[1]))
          WordChecks.emplace_back(I, *Bits);
        continue;
      }
      if (!Op.Output.isReg() && !Op.Output.isTemp())
        continue;
      if (Op.Output.isReg() && Op.Output.Offset == mipsreg::T9)
        return std::nullopt;
      auto V = Values.write(Op);
      if (!V || V->Base != Origin::AdjustedEntry ||
          V->Bits != *Image.MIPS->PICGP || (WordPointers && !V->SignedWord) ||
          !Op.Output.isReg() || Op.Output.Size != 8 || Op.Opcode != NdOp::COPY)
        continue;

      // Both GP setup instruction orders can leave an intermediate outside
      // every mapped segment. Bind the completed register definition, where
      // the ELF record independently confirms the linked address.
      LowFunc Projected = Function;
      auto &Ops = Projected.Blocks.front().Ops;
      Ops[I].Inputs[0] = NdVar::address(V->Bits, 8);
      // These guards observe only sign extension. The n32 ABI and word
      // operations establish it at every load bias.
      for (const auto &[Index, Bits] : WordChecks)
        Ops[Index].Inputs[1] = NdVar::scalar(Bits, 8);
      return Projected;
    }
  }
  return std::nullopt;
}

} // namespace neverd
