//===- PICGlobalPointer.cpp - MIPS PIC entry GP projection ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PICGlobalPointer.h"

#include "neverd/ir/low/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
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

  bool operator==(const Value &) const = default;
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
    // Adding zero preserves both the entry identity and any proven sign
    // extension, including a DADDU used to save the incoming register.
    if (B->Base != Origin::Scalar && Op.Inputs[0].isConst() && !A->Bits)
      return B;
    if (A->Base != Origin::Scalar && Op.Inputs[1].isConst() && !B->Bits)
      return A;
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

  void intersect(const EntryValues &Other) {
    llvm::SmallVector<uint64_t, 8> Remove;
    for (const auto &[Reg, V] : Registers) {
      const auto It = Other.Registers.find(Reg);
      if (It == Other.Registers.end() || It->second != V)
        Remove.push_back(Reg);
    }
    for (uint64_t Reg : Remove)
      Registers.erase(Reg);
  }

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

struct BlockProof {
  int Id;
  llvm::SmallVector<std::pair<size_t, uint64_t>, 4> Words;
  llvm::SmallVector<size_t, 1> Writes;
  size_t WordsBeforeGP = 0;
};

/// Returns true when the register state reaches every ordinary successor.
bool scanPrefix(const LowBlock &Block, Arch Target, uint64_t GP,
                EntryValues &Values, BlockProof &Proof) {
  const bool WordPointers = hasMIPS32BitAddressIn64BitGPR(Target);
  size_t End = 0;
  for (const auto &Insn : Block.InstructionBoundaries) {
    const bool Direct =
        Insn.Control == LowInstructionControl::Branch && Insn.Immediate &&
        !hasLowInstructionControlFlag(Insn.ControlFlags,
                                      LowInstructionControlFlag::Indirect);
    if (isMIPSCompressedMode(Insn.Mode) ||
        (Insn.Control != LowInstructionControl::None && !Direct) ||
        Insn.FirstOp != End || Insn.OpCount > Block.Ops.size() - End)
      return false;
    End += Insn.OpCount;
    Values.nextInstruction();
    for (size_t I = Insn.FirstOp; I < End; ++I) {
      const LowOp &Op = Block.Ops[I];
      if (Op.Opcode == NdOp::INTRINSIC) {
        if (Op.NumInputs && Op.Inputs[0].isConst() &&
            Op.Inputs[0].Offset == unsigned(Intrinsic::RequireAligned))
          continue;
        if (!mipsScalarLowShapeIsValid(Op, Target) ||
            Op.Inputs[0].Offset != unsigned(Intrinsic::MIPSRequireWord))
          return false;
        if (auto Bits = Values.knownWord(Op.Inputs[1]))
          Proof.Words.emplace_back(I, *Bits);
        continue;
      }
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
        // A branch-likely guards its slot with an earlier COND_BR. Only
        // a final direct transfer has one register state for every edge.
        return Direct && I + 1 == Block.Ops.size() && Op.NumInputs &&
               Op.Inputs[0].isConst() && Op.Inputs[0].Offset == *Insn.Immediate;
      }
      if (!Op.Output.isReg() && !Op.Output.isTemp())
        continue;
      auto V = Values.write(Op);
      if (!V || V->Base != Origin::AdjustedEntry || V->Bits != GP ||
          (WordPointers && !V->SignedWord) || !Op.Output.isReg() ||
          Op.Output.Size != 8 || Op.Opcode != NdOp::COPY)
        continue;

      Proof.Writes.push_back(I);
      Proof.WordsBeforeGP = Proof.Words.size();
    }
    if (Direct)
      return false;
  }
  return Block.hasInstructionBoundaries() && End == Block.Ops.size();
}

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

  struct Incoming {
    EntryValues Values;
    size_t Reached = 0;
    explicit Incoming(const EntryValues &Values) : Values(Values) {}
  };
  llvm::SmallDenseMap<int, Incoming, 2> Pending;
  EntryValues Values(Function.Entry, hasMIPS32BitAddressIn64BitGPR(Image.Arch));
  llvm::SmallVector<int, 4> Ready;
  llvm::SmallVector<BlockProof, 4> Proofs;
  const uint64_t GP = *Image.MIPS->PICGP;
  const bool Complete = Function.hasCompleteLiftCoverage() &&
                        Function.UnsafeIndirectBranchAddresses.empty();
  bool Found = false;
  for (int Id = 0;;) {
    const LowBlock &Block = Function.Blocks[Id];
    auto &Proof = Proofs.emplace_back(BlockProof{.Id = Id});
    const bool ReachesExit = scanPrefix(Block, Image.Arch, GP, Values, Proof);
    Found |= !Proof.Writes.empty();
    Values.nextInstruction();
    if (ReachesExit && Complete) {
      for (int Succ : Block.Succs) {
        const auto &Next = Function.Blocks[Succ];
        if (!Next.ExceptionalPreds.empty() ||
            Function.ModuleAnalysisRoots.contains(Next.StartAddr) ||
            Function.OrdinaryModuleAnalysisRoots.contains(Next.StartAddr))
          continue;
        auto [State, First] = Pending.try_emplace(Succ, Values);
        if (!First)
          State->second.Values.intersect(Values);
        // A call or an independent entry leaves an incoming edge unproved.
        // A loop header also waits on its back edge and stays unvisited.
        if (++State->second.Reached == Next.Preds.size())
          Ready.push_back(Succ);
      }
    }
    if (Ready.empty())
      break;
    Id = Ready.pop_back_val();
    auto It = Pending.find(Id);
    Values = std::move(It->second.Values);
    Pending.erase(It);
  }
  if (!Found)
    return std::nullopt;

  LowFunc Projected = Function;
  llvm::SmallDenseSet<int, 4> Needed;
  for (const auto &Proof : llvm::reverse(Proofs)) {
    const bool Downstream = Needed.erase(Proof.Id);
    if (!Downstream && Proof.Writes.empty())
      continue;
    auto &Block = Projected.Blocks[Proof.Id];
    for (size_t I : Proof.Writes)
      Block.Ops[I].Inputs[0] = NdVar::address(GP, 8);
    // These n32 guards observe only sign extension. Discharge proven checks
    // through the completed write, including those in its predecessors.
    const size_t Count = Downstream ? Proof.Words.size() : Proof.WordsBeforeGP;
    for (const auto &[Index, Bits] :
         llvm::ArrayRef(Proof.Words).take_front(Count))
      Block.Ops[Index].Inputs[1] = NdVar::scalar(Bits, 8);
    Needed.insert_range(Block.Preds);
  }
  return Projected;
}

} // namespace neverd
