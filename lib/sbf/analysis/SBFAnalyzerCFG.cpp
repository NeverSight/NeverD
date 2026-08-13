//===- SBFAnalyzerCFG.cpp - SBF basic blocks, MedIR, and registers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Partitions resolved LowIR into basic blocks and edges, lowers it into the
/// normalized MedIR stream, and runs the block-level constant register
/// lattice over that stream.
///
//===----------------------------------------------------------------------===//

#include "SBFAnalyzerDetail.h"

#include "neverd/sbf/analysis/SBFDataflow.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <vector>

namespace neverd::sbf {
namespace {

size_t nextInstructionSlot(const LowInstruction &Instruction) {
  return Instruction.Slot + Instruction.SlotWidth;
}

void addUnique(std::vector<size_t> &Values, size_t Value) {
  if (std::find(Values.begin(), Values.end(), Value) == Values.end())
    Values.push_back(Value);
}

bool sameValue(const RegisterValue &L, const RegisterValue &R) {
  return L.ValueKind == R.ValueKind && L.Value == R.Value &&
         L.Offset == R.Offset;
}

RegisterValue mergeValue(const std::vector<const MedBlock *> &Predecessors,
                         unsigned Register) {
  if (Predecessors.empty())
    return {};
  RegisterValue Result = Predecessors.front()->Outputs[Register];
  for (const MedBlock *Block : Predecessors)
    if (!sameValue(Result, Block->Outputs[Register]))
      return {};
  return Result;
}

} // namespace

namespace analyzer_detail {

void buildCFG(LowIR &Low) {
  std::set<size_t> Leaders{0, Low.EntrySlot};
  for (const LowInstruction &Instruction : Low.Instructions) {
    if (Instruction.IsContinuation || !Instruction.Info)
      continue;
    if (Instruction.BranchTarget)
      Leaders.insert(*Instruction.BranchTarget);
    if (Instruction.CallTarget)
      Leaders.insert(*Instruction.CallTarget);
    if (Instruction.Info->isBranch() || Instruction.Info->isCall() ||
        Instruction.Info->isExit()) {
      const size_t Next = nextInstructionSlot(Instruction);
      if (Next < Low.Instructions.size())
        Leaders.insert(Next);
    }
  }

  std::vector<size_t> Ordered(Leaders.begin(), Leaders.end());
  Ordered.erase(std::remove_if(Ordered.begin(), Ordered.end(),
                               [&](size_t Slot) {
                                 return Slot >= Low.Instructions.size() ||
                                        Low.Instructions[Slot].IsContinuation;
                               }),
                Ordered.end());
  std::map<size_t, size_t> SlotToBlock;
  for (size_t I = 0; I < Ordered.size(); ++I) {
    BasicBlock Block;
    Block.ID = I;
    Block.StartSlot = Ordered[I];
    Block.EndSlot =
        I + 1 < Ordered.size() ? Ordered[I + 1] : Low.Instructions.size();
    Low.Blocks.push_back(std::move(Block));
    for (size_t Slot = Low.Blocks.back().StartSlot;
         Slot < Low.Blocks.back().EndSlot; ++Slot)
      SlotToBlock[Slot] = I;
  }

  auto AddEdge = [&](size_t From, std::optional<size_t> To, EdgeKind Kind) {
    Low.Edges.push_back({From, To, Kind});
    if (!To)
      return;
    addUnique(Low.Blocks[From].Successors, *To);
    addUnique(Low.Blocks[*To].Predecessors, From);
  };

  for (BasicBlock &Block : Low.Blocks) {
    const LowInstruction *Last = nullptr;
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      if (!Low.Instructions[Slot].IsContinuation)
        Last = &Low.Instructions[Slot];
    if (!Last || !Last->Info) {
      if (Block.ID + 1 < Low.Blocks.size())
        AddEdge(Block.ID, Block.ID + 1, EdgeKind::Fallthrough);
      continue;
    }
    auto TargetBlock =
        [&](std::optional<size_t> Slot) -> std::optional<size_t> {
      if (!Slot)
        return std::nullopt;
      auto It = SlotToBlock.find(*Slot);
      return It == SlotToBlock.end() ? std::nullopt
                                     : std::optional<size_t>(It->second);
    };
    if (Last->Info->Op == Operation::Jump) {
      AddEdge(Block.ID, TargetBlock(Last->BranchTarget), EdgeKind::Branch);
    } else if (Last->Info->isConditionalBranch()) {
      AddEdge(Block.ID, TargetBlock(Last->BranchTarget), EdgeKind::BranchTaken);
      if (Block.ID + 1 < Low.Blocks.size())
        AddEdge(Block.ID, Block.ID + 1, EdgeKind::Fallthrough);
    } else if (Last->Info->isCall()) {
      if (Last->Call == CallKind::Internal)
        AddEdge(Block.ID, TargetBlock(Last->CallTarget), EdgeKind::Call);
      else if (Last->Call == CallKind::Indirect)
        AddEdge(Block.ID, std::nullopt, EdgeKind::IndirectCall);
      if (Block.ID + 1 < Low.Blocks.size())
        AddEdge(Block.ID, Block.ID + 1, EdgeKind::Fallthrough);
    } else if (Last->Info->isExit()) {
      AddEdge(Block.ID, std::nullopt, EdgeKind::Return);
    } else if (Block.ID + 1 < Low.Blocks.size()) {
      AddEdge(Block.ID, Block.ID + 1, EdgeKind::Fallthrough);
    }
  }

  const auto EntryIt = SlotToBlock.find(Low.EntrySlot);
  if (EntryIt != SlotToBlock.end()) {
    std::deque<size_t> Work{EntryIt->second};
    while (!Work.empty()) {
      const size_t ID = Work.front();
      Work.pop_front();
      if (Low.Blocks[ID].Reachable)
        continue;
      Low.Blocks[ID].Reachable = true;
      for (size_t Successor : Low.Blocks[ID].Successors)
        Work.push_back(Successor);
    }
  }
}

void buildMedIR(SBFProgram &Program) {
  Program.Med.TheVersion = Program.Low.TheVersion;
  for (const LowInstruction &Instruction : Program.Low.Instructions) {
    if (Instruction.IsContinuation || !Instruction.Info)
      continue;
    MedInstruction Med;
    Med.Slot = Instruction.Slot;
    Med.Address = Instruction.Address;
    Med.SourceOpcode = Instruction.Info->ID;
    Med.Op = Instruction.Info->Op;
    Med.Form = Instruction.Info->Form;
    Med.Width = Instruction.Info->Width;
    Med.Dst = Instruction.Dst;
    Med.Src = Instruction.Src;
    Med.SlotWidth = Instruction.SlotWidth;
    Med.Offset = Instruction.Offset;
    Med.Immediate = Instruction.Immediate;
    Med.Semantics = semanticTraits(*Instruction.Info, Program.Low.TheVersion);
    Med.BranchTarget = Instruction.BranchTarget;
    Med.Call = Instruction.Call;
    Med.CallTarget = Instruction.CallTarget;
    Med.SyscallHash = Instruction.SyscallHash;
    Med.Syscall = Instruction.Syscall;
    if (Instruction.Info->ID == Opcode::CALL_REG)
      Med.CallRegister = Instruction.CallRegister;
    Program.Med.Instructions.push_back(std::move(Med));
  }
  for (const BasicBlock &Block : Program.Low.Blocks) {
    MedBlock MedBlock;
    MedBlock.ID = Block.ID;
    MedBlock.StartSlot = Block.StartSlot;
    MedBlock.EndSlot = Block.EndSlot;
    Program.Med.Blocks.push_back(std::move(MedBlock));
  }
}

void runRegisterDataflow(SBFProgram &Program) {
  if (Program.Med.Blocks.empty())
    return;
  const MedInstructionIndex Index(Program.Med);

  size_t EntryBlock = 0;
  for (const BasicBlock &Block : Program.Low.Blocks)
    if (Program.Low.EntrySlot >= Block.StartSlot &&
        Program.Low.EntrySlot < Block.EndSlot) {
      EntryBlock = Block.ID;
      break;
    }
  Program.Med.Blocks[EntryBlock].Inputs[kFramePointerRegister] = {
      RegisterValue::Kind::StackAddress,
      initialFramePointer(Program.Low.TheVersion, Program.Config), 0};
  // The loader invokes a Solana program with the address of the serialized
  // input buffer, which is always the base of the input region.
  Program.Med.Blocks[EntryBlock].Inputs[kFirstArgumentRegister] = {
      RegisterValue::Kind::Constant, kInputStart, 0};
  // A runtime that has activated it also hands over the address of the
  // instruction data. Where that lands depends on the accounts, so it is its
  // own kind of value; before activation the register simply arrives zero, and
  // a program that reads it reads a zero rather than an address.
  Program.Med.Blocks[EntryBlock].Inputs[kInstructionDataRegister] =
      isFeatureActive(Program.Profile, RuntimeFeature::InstructionDataPointer)
          ? RegisterValue{RegisterValue::Kind::InstructionDataAddress, 0, 0}
          : RegisterValue{RegisterValue::Kind::Constant, 0, 0};
  const size_t IterationLimit = Program.Med.Blocks.size() * 4 + 1;
  for (size_t Iteration = 0; Iteration < IterationLimit; ++Iteration) {
    bool Changed = false;
    for (MedBlock &Block : Program.Med.Blocks) {
      if (Block.ID != EntryBlock) {
        std::vector<const MedBlock *> Predecessors;
        for (size_t ID : Program.Low.Blocks[Block.ID].Predecessors)
          Predecessors.push_back(&Program.Med.Blocks[ID]);
        for (unsigned Register = 0; Register < kRegisterCount; ++Register) {
          RegisterValue Merged = mergeValue(Predecessors, Register);
          if (!sameValue(Block.Inputs[Register], Merged)) {
            Block.Inputs[Register] = Merged;
            Changed = true;
          }
        }
      }

      RegisterState State = Block.Inputs;
      for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
        if (const MedInstruction *Instruction = Index.find(Slot))
          applyRegisterTransfer(*Instruction, State);
      if (State != Block.Outputs) {
        Block.Outputs = State;
        Changed = true;
      }
    }
    if (!Changed)
      break;
  }
}

} // namespace analyzer_detail
} // namespace neverd::sbf
