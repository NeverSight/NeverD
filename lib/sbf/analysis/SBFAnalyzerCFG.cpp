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
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

size_t nextInstructionSlot(const LowInstruction &Instruction) {
  return Instruction.Slot + Instruction.SlotWidth;
}

constexpr size_t kNoBlock = std::numeric_limits<size_t>::max();

} // namespace

namespace analyzer_detail {

void buildCFG(LowIR &Low, const llvm::BitVector &FunctionEntrySlots) {
  if (Low.Instructions.empty())
    return;
  llvm::BitVector Leaders(Low.Instructions.size());
  llvm::BitVector ContinuationLeaders(Low.Instructions.size());
  Leaders.set(0);
  if (Low.EntrySlot < Leaders.size())
    Leaders.set(Low.EntrySlot);
  for (int Slot = FunctionEntrySlots.find_first(); Slot >= 0;
       Slot = FunctionEntrySlots.find_next(Slot))
    if (static_cast<size_t>(Slot) < Leaders.size())
      Leaders.set(static_cast<size_t>(Slot));
  if (Low.EntrySlot < Low.Instructions.size() &&
      Low.Instructions[Low.EntrySlot].IsContinuation) {
    ContinuationLeaders.set(Low.EntrySlot);
    if (Low.EntrySlot + 1 < Low.Instructions.size())
      Leaders.set(Low.EntrySlot + 1);
  }
  for (const LowInstruction &Instruction : Low.Instructions) {
    if (Instruction.IsContinuation)
      continue;
    if (Instruction.isInvalid()) {
      const size_t Next = nextInstructionSlot(Instruction);
      if (Next < Low.Instructions.size())
        Leaders.set(Next);
      continue;
    }
    if (!Instruction.Info)
      continue;
    if (Instruction.BranchTarget &&
        *Instruction.BranchTarget < Low.Instructions.size())
      Leaders.set(*Instruction.BranchTarget);
    if (Instruction.CallTarget &&
        *Instruction.CallTarget < Low.Instructions.size()) {
      Leaders.set(*Instruction.CallTarget);
      if (*Instruction.CallTarget < Low.Instructions.size() &&
          Low.Instructions[*Instruction.CallTarget].IsContinuation) {
        ContinuationLeaders.set(*Instruction.CallTarget);
        const size_t Next = *Instruction.CallTarget + 1;
        if (Next < Low.Instructions.size())
          Leaders.set(Next);
      }
    }
    if (Instruction.Info->isBranch() || Instruction.Info->isCall() ||
        Instruction.Info->isExit()) {
      const size_t Next = nextInstructionSlot(Instruction);
      if (Next < Low.Instructions.size())
        Leaders.set(Next);
    }
  }

  std::vector<size_t> Ordered;
  Ordered.reserve(Leaders.count());
  for (int Slot = Leaders.find_first(); Slot >= 0;
       Slot = Leaders.find_next(Slot)) {
    const size_t Leader = static_cast<size_t>(Slot);
    if (!Low.Instructions[Leader].IsContinuation ||
        ContinuationLeaders.test(Leader))
      Ordered.push_back(Leader);
  }
  Low.Blocks.reserve(Ordered.size());
  // Every non-empty block contributes at least one typed terminal edge in the
  // current CFG model. Reserving that known floor avoids geometric growth for
  // the common one-successor shape without over-allocating the two-successor
  // conditional upper bound.
  Low.Edges.reserve(Ordered.size());
  std::vector<size_t> SlotToBlock(Low.Instructions.size(), kNoBlock);
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
    std::vector<size_t> &Successors = Low.Blocks[From].Successors;
    if (std::find(Successors.begin(), Successors.end(), *To) !=
        Successors.end())
      return;
    Successors.push_back(*To);
    // A SBF terminator has at most two typed targets, so endpoint
    // deduplication belongs on its bounded outgoing list. Searching the
    // destination's unbounded predecessor list makes a high-indegree loop
    // header quadratic.
    Low.Blocks[*To].Predecessors.push_back(From);
  };

  for (BasicBlock &Block : Low.Blocks) {
    const LowInstruction *Last = nullptr;
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      if (!Low.Instructions[Slot].IsContinuation)
        Last = &Low.Instructions[Slot];
    if (!Last) {
      AddEdge(Block.ID, std::nullopt, EdgeKind::Fault);
      continue;
    }
    if (Last->isInvalid()) {
      AddEdge(Block.ID, std::nullopt, EdgeKind::Invalid);
      continue;
    }
    if (!Last->Info) {
      AddEdge(Block.ID, std::nullopt, EdgeKind::Invalid);
      continue;
    }
    auto TargetBlock =
        [&](std::optional<size_t> Slot) -> std::optional<size_t> {
      if (!Slot)
        return std::nullopt;
      if (*Slot >= SlotToBlock.size() || SlotToBlock[*Slot] == kNoBlock)
        return std::nullopt;
      return SlotToBlock[*Slot];
    };
    if (Last->Info->Op == Operation::Jump) {
      AddEdge(Block.ID, TargetBlock(Last->BranchTarget), EdgeKind::Branch);
    } else if (Last->Info->isConditionalBranch()) {
      AddEdge(Block.ID, TargetBlock(Last->BranchTarget), EdgeKind::BranchTaken);
      if (auto Next = TargetBlock(nextInstructionSlot(*Last)))
        AddEdge(Block.ID, Next, EdgeKind::Fallthrough);
    } else if (Last->Info->isCall()) {
      if (Last->Call == CallKind::Unsupported) {
        AddEdge(Block.ID, std::nullopt, EdgeKind::Fault);
        continue;
      }
      if (Last->Call == CallKind::Internal)
        AddEdge(Block.ID, TargetBlock(Last->CallTarget), EdgeKind::Call);
      else if (Last->Call == CallKind::Indirect)
        AddEdge(Block.ID, std::nullopt, EdgeKind::IndirectCall);
      if (auto Next = TargetBlock(nextInstructionSlot(*Last)))
        AddEdge(Block.ID, Next, EdgeKind::Fallthrough);
    } else if (Last->Info->isExit()) {
      AddEdge(Block.ID, std::nullopt, EdgeKind::Return);
    } else if (auto Next = TargetBlock(nextInstructionSlot(*Last))) {
      AddEdge(Block.ID, Next, EdgeKind::Fallthrough);
    }
  }

  if (Low.EntrySlot < SlotToBlock.size() &&
      SlotToBlock[Low.EntrySlot] != kNoBlock) {
    std::deque<size_t> Work{SlotToBlock[Low.EntrySlot]};
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
  const size_t InstructionCount = static_cast<size_t>(std::count_if(
      Program.Low.Instructions.begin(), Program.Low.Instructions.end(),
      [](const LowInstruction &Instruction) {
        return !Instruction.IsContinuation;
      }));
  Program.Med.Instructions.reserve(InstructionCount);
  Program.Med.Blocks.reserve(Program.Low.Blocks.size());
  for (const LowInstruction &Instruction : Program.Low.Instructions) {
    if (Instruction.IsContinuation)
      continue;
    MedInstruction Med;
    Med.Slot = Instruction.Slot;
    Med.Address = Instruction.Address;
    Med.SlotWidth = Instruction.SlotWidth;
    Med.InvalidReason = Instruction.InvalidReason;
    if (Instruction.isInvalid() || !Instruction.Info) {
      Med.SourceOpcode =
          Instruction.Info ? Instruction.Info->ID : Opcode::Unknown;
      Med.Op = Operation::Invalid;
      Program.Med.Instructions.push_back(std::move(Med));
      continue;
    }
    Med.SourceOpcode = Instruction.Info->ID;
    Med.Op = Instruction.Info->Op;
    Med.Form = Instruction.Info->Form;
    Med.Width = Instruction.Info->Width;
    Med.Dst = Instruction.Dst;
    Med.Src = Instruction.Src;
    Med.Offset = Instruction.Offset;
    Med.Immediate = Instruction.Immediate;
    Med.Semantics = semanticTraits(*Instruction.Info, Program.Low.TheVersion);
    Med.BranchTarget = Instruction.BranchTarget;
    Med.Call = Instruction.Call;
    Med.Dispatch = Instruction.Dispatch;
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

RegisterDataflowStatistics
runRegisterDataflow(SBFProgram &Program,
                    const llvm::BitVector &FunctionEntrySlots) {
  RegisterDataflowStatistics Stats;
  Stats.BlockCount = Program.Med.Blocks.size();
  if (Program.Med.Blocks.empty())
    return Stats;
  const MedInstructionIndex Index(Program.Med);

  // A call edge names another function, not a same-frame predecessor.  The
  // caller's block output has already applied the ABI clobber and is the state
  // seen after return on the fallthrough edge; feeding that state into the
  // callee would be both context-dependent and temporally wrong.  Keep the
  // fixed point intraprocedural, like LLVM's ordinary per-function dataflow.
  std::vector<std::vector<size_t>> DataflowSuccessors(
      Program.Low.Blocks.size());
  std::vector<char> HasPredecessor(Program.Low.Blocks.size());
  for (const CFGEdge &Edge : Program.Low.Edges) {
    if (!Edge.To || Edge.From >= Program.Low.Blocks.size() ||
        *Edge.To >= Program.Low.Blocks.size())
      continue;
    if (!getEdgeKindInfo(Edge.Kind).IsIntraprocedural)
      continue;
    DataflowSuccessors[Edge.From].push_back(*Edge.To);
    HasPredecessor[*Edge.To] = 1;
  }

  std::vector<size_t> BlockByEntrySlot(Program.Low.Instructions.size(),
                                       Program.Med.Blocks.size());
  for (const BasicBlock &Block : Program.Low.Blocks)
    if (Block.StartSlot < BlockByEntrySlot.size())
      BlockByEntrySlot[Block.StartSlot] = Block.ID;

  const size_t EntryBlock = Program.Low.EntrySlot < BlockByEntrySlot.size()
                                ? BlockByEntrySlot[Program.Low.EntrySlot]
                                : Program.Med.Blocks.size();
  if (EntryBlock >= Program.Med.Blocks.size())
    return Stats;
  // Unreached is a separate lattice bottom. RegisterValue::Unknown is the
  // conservative top, so treating the zero-initialized output of an
  // unvisited predecessor as Unknown would make the solver non-monotone: a
  // value could first become precise and then be forgotten. Starting only at
  // CFG roots gives the standard finite-height must-analysis fixed point.
  std::vector<char> IsRoot(Program.Med.Blocks.size());
  std::vector<char> IsFunctionRoot(Program.Med.Blocks.size());
  IsRoot[EntryBlock] = 1;
  IsFunctionRoot[EntryBlock] = 1;
  for (int Slot = FunctionEntrySlots.find_first(); Slot >= 0;
       Slot = FunctionEntrySlots.find_next(Slot)) {
    const size_t EntrySlot = static_cast<size_t>(Slot);
    if (EntrySlot < BlockByEntrySlot.size() &&
        BlockByEntrySlot[EntrySlot] < IsRoot.size()) {
      IsRoot[BlockByEntrySlot[EntrySlot]] = 1;
      IsFunctionRoot[BlockByEntrySlot[EntrySlot]] = 1;
    }
  }
  for (size_t ID = 0; ID < HasPredecessor.size(); ++ID)
    if (!HasPredecessor[ID])
      IsRoot[ID] = 1;

  std::vector<char> Reached(Program.Med.Blocks.size());
  std::vector<char> HasInput(Program.Med.Blocks.size());
  std::vector<char> Queued(Program.Med.Blocks.size());
  std::deque<size_t> Worklist;
  for (size_t ID = 0; ID < IsRoot.size(); ++ID) {
    if (!IsRoot[ID])
      continue;
    ++Stats.RootCount;
    Stats.FunctionRootCount += IsFunctionRoot[ID] != 0;

    // A trusted function entry has a virtual ABI predecessor. Construct that
    // seed only while initializing its root instead of retaining one 264-byte
    // all-Unknown RegisterState for every block in the program. A real
    // backedge still meets this value through the ordinary Inputs lattice.
    RegisterState Boundary;
    if (IsFunctionRoot[ID])
      Boundary[kFramePointerRegister] = {
          RegisterValue::Kind::StackAddress,
          initialFramePointer(Program.Low.TheVersion, Program.Config), 0};
    if (ID == EntryBlock) {
      Boundary[kFirstArgumentRegister] = {RegisterValue::Kind::Constant,
                                          kInputStart, 0};
      Boundary[kInstructionDataRegister] =
          hasFeature(Program.ActiveRuntimeFeatures,
                     RuntimeFeature::InstructionDataPointer)
              ? RegisterValue{RegisterValue::Kind::InstructionDataAddress, 0, 0}
              : RegisterValue{RegisterValue::Kind::Constant, 0, 0};
    }
    Program.Med.Blocks[ID].Inputs = std::move(Boundary);
    ++Stats.BoundarySeedCount;
    Stats.PeakBoundaryWorkspaceEntryCount = 1;
    HasInput[ID] = 1;
    Worklist.push_back(ID);
    Queued[ID] = 1;
  }

  while (!Worklist.empty()) {
    const size_t ID = Worklist.front();
    Worklist.pop_front();
    Queued[ID] = 0;
    MedBlock &Block = Program.Med.Blocks[ID];
    if (!HasInput[ID])
      continue;

    RegisterState State = Block.Inputs;
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      if (const MedInstruction *Instruction = Index.find(Slot))
        applyRegisterTransfer(*Instruction, State);
    const bool FirstEvaluation = !Reached[ID];
    const bool OutputChanged = State != Block.Outputs;
    if (!FirstEvaluation && !OutputChanged)
      continue;
    Block.Outputs = std::move(State);
    Reached[ID] = 1;
    for (size_t Successor : DataflowSuccessors[ID]) {
      MedBlock &SuccessorBlock = Program.Med.Blocks[Successor];
      bool InputChanged = false;
      if (!HasInput[Successor]) {
        SuccessorBlock.Inputs = Block.Outputs;
        HasInput[Successor] = 1;
        InputChanged = true;
      } else {
        for (size_t Register = 0; Register < kRegisterCount; ++Register) {
          if (SuccessorBlock.Inputs[Register] == Block.Outputs[Register])
            continue;
          const RegisterValue Unknown;
          if (SuccessorBlock.Inputs[Register] != Unknown) {
            SuccessorBlock.Inputs[Register] = Unknown;
            InputChanged = true;
          }
        }
      }
      if (!InputChanged || Queued[Successor])
        continue;
      Worklist.push_back(Successor);
      Queued[Successor] = 1;
    }
  }
  return Stats;
}

bool refineCallXTargets(DecodeContext &Context) {
  SBFProgram &Program = Context.Program;
  if (Program.Med.Blocks.empty())
    return false;

  std::vector<MedInstruction *> InstructionsBySlot(
      Program.Low.Instructions.size());
  for (MedInstruction &Instruction : Program.Med.Instructions)
    if (Instruction.Slot < InstructionsBySlot.size())
      InstructionsBySlot[Instruction.Slot] = &Instruction;

  bool Changed = false;
  llvm::BitVector DiagnosedContinuationTargets(Program.Low.Instructions.size());
  for (const MedBlock &Block : Program.Med.Blocks) {
    RegisterState State = Block.Inputs;
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      MedInstruction *Instruction = InstructionsBySlot[Slot];
      if (!Instruction)
        continue;
      if (Instruction->Op == Operation::CallX &&
          Instruction->CallRegister < State.size()) {
        LowInstruction &Low = Program.Low.Instructions[Instruction->Slot];
        const RegisterValue &Target = State[Instruction->CallRegister];
        std::optional<size_t> ExactTarget;
        if (Target.ValueKind == RegisterValue::Kind::Constant) {
          // The VM deliberately uses wrapping subtraction and floors an
          // unaligned address to its containing instruction slot.
          const uint64_t TargetSlot =
              (Target.Value - Program.Low.TextAddress) / kInstructionSize;
          if (TargetSlot < Program.Low.Instructions.size())
            ExactTarget = static_cast<size_t>(TargetSlot);
        }

        if (!ExactTarget) {
          const bool ClassificationChanged =
              Low.Call != CallKind::Indirect || Low.CallTarget.has_value() ||
              !Low.ResolvedName.empty() ||
              Instruction->Call != CallKind::Indirect ||
              Instruction->CallTarget.has_value();
          Low.Call = CallKind::Indirect;
          Low.CallTarget.reset();
          Low.ResolvedName.clear();
          Instruction->Call = CallKind::Indirect;
          Instruction->CallTarget.reset();
          Changed |= ClassificationChanged;
          applyRegisterTransfer(*Instruction, State);
          continue;
        }

        const va_t Address =
            Program.Low.TextAddress + *ExactTarget * kInstructionSize;
        const Symbol *Symbol = Context.findFunctionSymbol(Address);
        const std::string ResolvedName =
            Symbol ? Symbol->Name : syntheticFunctionName(Address);
        const bool ClassificationChanged =
            Low.Call != CallKind::Internal || Low.CallTarget != ExactTarget ||
            Low.ResolvedName != ResolvedName ||
            Instruction->Call != CallKind::Internal ||
            Instruction->CallTarget != ExactTarget;
        Low.Call = CallKind::Internal;
        Low.CallTarget = ExactTarget;
        Low.ResolvedName = ResolvedName;
        Instruction->Call = CallKind::Internal;
        Instruction->CallTarget = ExactTarget;
        Changed |= ClassificationChanged;

        if (!Program.Low.Instructions[*ExactTarget].IsContinuation) {
          if (!Context.FunctionEntrySlots.test(*ExactTarget)) {
            Context.FunctionEntrySlots.set(*ExactTarget);
            Changed = true;
          }
        } else if (ClassificationChanged &&
                   !DiagnosedContinuationTargets.test(*ExactTarget)) {
          Program.Low.Diagnostics.push_back(
              {DiagnosticSeverity::Warning, *ExactTarget, Address,
               kCallTargetInsideWideLoadDiagnostic.str()});
          DiagnosedContinuationTargets.set(*ExactTarget);
        }
      }
      applyRegisterTransfer(*Instruction, State);
    }
  }
  return Changed;
}

} // namespace analyzer_detail
} // namespace neverd::sbf
