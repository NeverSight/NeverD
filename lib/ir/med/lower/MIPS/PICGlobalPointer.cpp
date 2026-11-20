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
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>

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

using Registers = llvm::SmallDenseMap<uint64_t, Value, 8>;

bool intersect(Registers &Into, const Registers &Other) {
  llvm::SmallVector<uint64_t, 8> Remove;
  for (const auto &[Reg, V] : Into) {
    const auto It = Other.find(Reg);
    if (It == Other.end() || It->second != V)
      Remove.push_back(Reg);
  }
  for (uint64_t Reg : Remove)
    Into.erase(Reg);
  return !Remove.empty();
}

class EntryValues {
  Registers GPRs, Temps;
  bool WordPointers;

  std::optional<Value> read(const NdVar &V) const {
    if (V.isConst() && !isAddressProvenance(V.Provenance))
      return Value{V.Offset, Origin::Scalar, false, V.Size};
    const auto &Values = V.isReg() ? GPRs : Temps;
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
  EntryValues(const Registers &Incoming, bool WordPointers)
      : GPRs(Incoming), WordPointers(WordPointers) {}

  const Registers &registers() const { return GPRs; }
  void nextInstruction() { Temps.clear(); }

  std::optional<Value> write(const LowOp &Op) {
    auto V = result(Op);
    auto &Values = Op.Output.isReg() ? GPRs : Temps;
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
  llvm::SmallVector<std::pair<size_t, uint64_t>, 4> Words;
  llvm::SmallVector<size_t, 1> Writes;
  size_t RequiredWords = 0;
  // Each edge records how many checks can execute before taking it.
  llvm::SmallVector<std::pair<int, size_t>, 2> Edges;
};

struct SkippedSlot {
  va_t Resume, Taken;
  Registers Before;
  size_t Words;
};

/// The final register state follows the executed slot. A guarded unit also
/// publishes the state just before its slot for the annulled edge.
bool scanPrefix(const LowBlock &Block, Arch Target, uint64_t GP,
                EntryValues &Values, BlockProof &Proof,
                std::optional<SkippedSlot> &Skipped) {
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
        if (Direct && Op.Opcode == NdOp::COND_BR && Insn.DelaySlot &&
            hasLowInstructionControlFlag(
                Insn.ControlFlags,
                LowInstructionControlFlag::InstructionGuard) &&
            I + 1 == Insn.FirstOp + Insn.DelaySlot->FirstOp) {
          Skipped.emplace(Insn.Address + Insn.Size, *Insn.Immediate,
                          Values.registers(), Proof.Words.size());
          continue;
        }
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
      Proof.RequiredWords = Proof.Words.size();
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
  if (Entry.StartAddr != Function.Entry || !Entry.ExceptionalPreds.empty())
    return std::nullopt;

  struct Input {
    Registers GPRs;
    size_t Predecessors = 0;
    bool Scanned = false;
    BlockProof Proof;
  };
  llvm::SmallDenseMap<int, Input, 2> Inputs;
  const bool WordPointers = hasMIPS32BitAddressIn64BitGPR(Image.Arch);
  Inputs[0].GPRs[mipsreg::T9] = {
      WordPointers ? uint64_t(llvm::SignExtend64<32>(Function.Entry))
                   : Function.Entry,
      Origin::Entry, WordPointers, 8};
  const uint64_t GP = *Image.MIPS->PICGP;
  const bool Complete = Function.hasCompleteLiftCoverage() &&
                        Function.UnsafeIndirectBranchAddresses.empty();

  // Start with the first incoming path. Later paths can only remove facts,
  // so a changing loop carrier becomes unknown on its first disagreement.
  llvm::SmallSetVector<int, 8> Ready;
  Ready.insert(0);
  while (!Ready.empty()) {
    const int Id = Ready.pop_back_val();
    auto &In = Inputs.find(Id)->second;
    const bool First = !In.Scanned;
    In.Scanned = true;
    EntryValues Values(In.GPRs, WordPointers);
    const LowBlock &Block = Function.Blocks[Id];
    BlockProof Proof;
    std::optional<SkippedSlot> Skipped;
    const bool Full = scanPrefix(Block, Image.Arch, GP, Values, Proof, Skipped);
    if (Complete && (Full || Skipped)) {
      for (int Succ : Block.Succs) {
        const auto &Next = Function.Blocks[Succ];
        if (!Next.ExceptionalPreds.empty() ||
            (Succ != 0 &&
             (Function.ModuleAnalysisRoots.contains(Next.StartAddr) ||
              Function.OrdinaryModuleAnalysisRoots.contains(Next.StartAddr))))
          continue;
        const bool Skip = Skipped && Next.StartAddr == Skipped->Resume;
        const bool Execute = !Skipped || Next.StartAddr == Skipped->Taken;
        if ((Execute && !Full) || (!Skip && !Execute))
          continue;
        const Registers &Outgoing =
            Execute ? Values.registers() : Skipped->Before;
        auto [At, New] = Inputs.try_emplace(Succ);
        auto &State = At->second;
        bool Changed = New;
        if (New)
          State.GPRs = Outgoing;
        else
          Changed = intersect(State.GPRs, Outgoing);
        // A branch to its own resume address still has two machine paths.
        if (Skip && Execute)
          Changed |= intersect(State.GPRs, Skipped->Before);
        if (First)
          ++State.Predecessors;
        if (Changed)
          Ready.insert(Succ);
        Proof.Edges.emplace_back(Succ,
                                 Execute ? Proof.Words.size() : Skipped->Words);
      }
    }
    Inputs.find(Id)->second.Proof = std::move(Proof);
  }

  // An opaque predecessor (for example a call or a separate entry) can
  // arrive with every entry-derived register clobbered. No descendant can
  // reconstruct entry provenance from a scalar alone.
  for (const auto &[Id, State] : Inputs)
    if (State.Predecessors != Function.Blocks[Id].Preds.size())
      Ready.insert(Id);
  while (!Ready.empty()) {
    auto At = Inputs.find(Ready.pop_back_val());
    if (At == Inputs.end())
      continue;
    for (const auto &[Succ, Words] : At->second.Proof.Edges)
      Ready.insert(Succ);
    Inputs.erase(At);
  }

  for (const auto &[Id, State] : Inputs)
    if (!State.Proof.Writes.empty())
      Ready.insert(Id);
  if (Ready.empty())
    return std::nullopt;
  // Only discharge n32 word checks preceding a proved GP write. On a
  // skipped edge the checks in the slot belong solely to the taken path.
  llvm::SmallDenseSet<int, 8> Needed(Ready.begin(), Ready.end());
  while (!Ready.empty()) {
    int Id = Ready.pop_back_val();
    for (int Pred : Function.Blocks[Id].Preds) {
      auto &Proof = Inputs.find(Pred)->second.Proof;
      for (const auto &[Succ, Words] : Proof.Edges)
        if (Succ == Id)
          Proof.RequiredWords = std::max(Proof.RequiredWords, Words);
      if (Needed.insert(Pred).second)
        Ready.insert(Pred);
    }
  }

  LowFunc Projected = Function;
  for (int Id : Needed) {
    const auto &Proof = Inputs.find(Id)->second.Proof;
    auto &Ops = Projected.Blocks[Id].Ops;
    for (size_t I : Proof.Writes)
      Ops[I].Inputs[0] = NdVar::address(GP, 8);
    for (const auto &[Index, Bits] :
         llvm::ArrayRef(Proof.Words).take_front(Proof.RequiredWords))
      Ops[Index].Inputs[1] = NdVar::scalar(Bits, 8);
  }
  return Projected;
}

} // namespace neverd
