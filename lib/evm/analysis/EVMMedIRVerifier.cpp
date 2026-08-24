//===- EVMMedIRVerifier.cpp - LowIR/MedIR boundary verifier --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMMedIRVerifier.h"

#include "EVMMedAnalysis.h"

#include "neverd/evm/bytecode/EVMDecoder.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::evm::detail {
namespace {

using Failure = MedIRValidationFailure;
using FailureKind = MedIRValidationFailureKind;

inline constexpr llvm::StringLiteral kExecutionLowIRDiagnosticPrefix =
    "evm: non-canonical LowIR for execution at pc 0x";
inline constexpr llvm::StringLiteral kMedLoweringLowIRDiagnosticPrefix =
    "evm: invalid LowIR for MedIR lowering at pc 0x";
inline constexpr llvm::StringLiteral kExecutionLowIRDiagnosticInfix = ": ";

std::optional<Failure> fail(uint64_t PC, FailureKind Kind) {
  return Failure{PC, Kind};
}

llvm::Error medLoweringValidationError(const Failure &Failure) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine(kMedLoweringLowIRDiagnosticPrefix) +
       llvm::utohexstr(Failure.PC) + kExecutionLowIRDiagnosticInfix +
       medIRValidationFailureMessage(Failure.Kind))
          .str(),
      llvm::inconvertibleErrorCode());
}

bool validReachability(Reachability Evidence) {
  return Evidence == Reachability::Reachable ||
         Evidence == Reachability::MayReachable;
}

bool validEdgeKind(EdgeKind Kind) {
  switch (Kind) {
  case EdgeKind::Fallthrough:
  case EdgeKind::Jump:
  case EdgeKind::ConditionalTrue:
  case EdgeKind::ConditionalFalse:
  case EdgeKind::Indirect:
    return true;
  }
  return false;
}

bool validFaultKind(LowFaultKind Kind) {
  switch (Kind) {
#define EVM_LOW_FAULT_KIND(ID, CONSUMES_INPUTS) case LowFaultKind::ID:
#include "neverd/evm/analysis/EVMLowFaultKinds.def"
    return true;
  }
  return false;
}

bool validValueKind(ValueKind Kind) {
  switch (Kind) {
  case ValueKind::Constant:
  case ValueKind::Instruction:
  case ValueKind::Phi:
  case ValueKind::Unknown:
    return true;
  }
  return false;
}

bool validAbstractExactness(LowAbstractExactness Exactness) {
  return Exactness == LowAbstractExactness::Exact ||
         Exactness == LowAbstractExactness::OverApproximation;
}

bool wordLess(const llvm::APInt &Left, const llvm::APInt &Right) {
  return Left.ult(Right);
}

bool sameOpcodeInfo(const OpcodeInfo &Left, const OpcodeInfo &Right) {
  return Left.Op == Right.Op && Left.Name == Right.Name &&
         Left.StackPops == Right.StackPops &&
         Left.StackPushes == Right.StackPushes &&
         Left.ImmediateBytes == Right.ImmediateBytes &&
         Left.Immediate == Right.Immediate && Left.Class == Right.Class &&
         Left.Introduced == Right.Introduced && Left.Effect == Right.Effect &&
         Left.MemoryAccess == Right.MemoryAccess &&
         Left.StateAccess == Right.StateAccess &&
         Left.CallValueAccess == Right.CallValueAccess &&
         Left.IsTerminator == Right.IsTerminator;
}

bool sameWord(const llvm::APInt &Left, const llvm::APInt &Right) {
  return Left.getBitWidth() == Right.getBitWidth() && Left == Right;
}

bool sameInstruction(const LowInstruction &Left, const LowInstruction &Right) {
  return Left.PC == Right.PC && Left.NextPC == Right.NextPC &&
         sameOpcodeInfo(Left.Info, Right.Info) &&
         Left.DecodeStatus == Right.DecodeStatus &&
         sameWord(Left.Immediate, Right.Immediate) &&
         Left.ImmediateStatus == Right.ImmediateStatus &&
         Left.StackOperands == Right.StackOperands &&
         Left.StackOperandCount == Right.StackOperandCount &&
         Left.Encoding == Right.Encoding;
}

template <typename Range, typename Compare>
bool isSortedAndUnique(const Range &Values, Compare Less) {
  return std::is_sorted(Values.begin(), Values.end(), Less) &&
         std::adjacent_find(Values.begin(), Values.end(),
                            [&](const auto &Left, const auto &Right) {
                              return !Less(Left, Right) && !Less(Right, Left);
                            }) == Values.end();
}

template <typename Range> bool isSortedAndUnique(const Range &Values) {
  return isSortedAndUnique(Values, std::less<>{});
}

const LowInstruction *instructionAt(const EVMLowIR &Low, uint64_t PC) {
  const auto It = llvm::lower_bound(
      Low.Instructions, PC,
      [](const LowInstruction &Instruction, uint64_t Address) {
        return Instruction.PC < Address;
      });
  return It != Low.Instructions.end() && It->PC == PC ? &*It : nullptr;
}

std::optional<Failure> verifyLowIRExecutionStructure(const EVMLowIR &Low) {
  if (!isValidHardfork(Low.Fork) ||
      Low.Code.size() > std::numeric_limits<uint64_t>::max())
    return fail(kEntryPC, FailureKind::LowInstructionTable);

  uint64_t ExpectedPC = kEntryPC;
  auto JumpDestination = Low.JumpDestinations.begin();
  for (const LowInstruction &Instruction : Low.Instructions) {
    if (Instruction.PC != ExpectedPC || Instruction.Encoding.empty() ||
        Instruction.Encoding.size() >
            std::numeric_limits<uint64_t>::max() - Instruction.PC ||
        Instruction.NextPC !=
            Instruction.PC +
                static_cast<uint64_t>(Instruction.Encoding.size()) ||
        Instruction.NextPC > Low.Code.size())
      return fail(Instruction.PC, FailureKind::LowInstructionTable);
    const size_t Offset = static_cast<size_t>(Instruction.PC);
    const LowInstruction Canonical =
        decodeInstructionAt(Low.Code, Offset, Low.Fork, nullptr);
    if (!sameInstruction(Instruction, Canonical))
      return fail(Instruction.PC, FailureKind::LowInstructionTable);
    if (!std::equal(Instruction.Encoding.begin(), Instruction.Encoding.end(),
                    Low.Code.begin() + Offset))
      return fail(Instruction.PC, FailureKind::LowInstructionTable);
    if (Instruction.is(Opcode::JUMPDEST)) {
      if (JumpDestination == Low.JumpDestinations.end() ||
          *JumpDestination != Instruction.PC)
        return fail(Instruction.PC, FailureKind::LowJumpDestination);
      ++JumpDestination;
    }
    ExpectedPC = Instruction.NextPC;
  }
  if (ExpectedPC != Low.Code.size() ||
      Low.Instructions.empty() != Low.Code.empty())
    return fail(kEntryPC, FailureKind::LowInstructionTable);
  if (JumpDestination != Low.JumpDestinations.end())
    return fail(kEntryPC, FailureKind::LowJumpDestination);
  return std::nullopt;
}

std::optional<Failure> verifyLowIR(const EVMLowIR &Low) {
  if (auto Failure = verifyLowIRExecutionStructure(Low))
    return Failure;
  uint64_t PreviousBlockPC = 0;
  bool FirstBlock = true;
  size_t InstructionCursor = 0;
  for (size_t BlockIndex = 0; BlockIndex < Low.Blocks.size(); ++BlockIndex) {
    const LowBlock &Block = Low.Blocks[BlockIndex];
    if ((!FirstBlock && Block.StartPC <= PreviousBlockPC) ||
        (FirstBlock && Block.StartPC != kEntryPC))
      return fail(Block.StartPC, FailureKind::LowBlockTable);
    FirstBlock = false;
    PreviousBlockPC = Block.StartPC;
    if (Block.FirstInstruction != InstructionCursor ||
        Block.InstructionCount == 0 ||
        Block.InstructionCount > Low.Instructions.size() - InstructionCursor)
      return fail(Block.StartPC, FailureKind::LowBlockInstructionRange);
    const size_t EndInstruction = InstructionCursor + Block.InstructionCount;
    const uint64_t ExpectedEnd = BlockIndex + 1 == Low.Blocks.size()
                                     ? static_cast<uint64_t>(Low.Code.size())
                                     : Low.Blocks[BlockIndex + 1].StartPC;
    if (Low.Instructions[InstructionCursor].PC != Block.StartPC ||
        Low.Instructions[EndInstruction - 1].NextPC != ExpectedEnd ||
        Block.EndPC != ExpectedEnd)
      return fail(Block.StartPC, FailureKind::LowBlockInstructionRange);
    InstructionCursor = EndInstruction;
  }
  if (InstructionCursor != Low.Instructions.size() ||
      Low.Blocks.empty() != Low.Instructions.empty())
    return fail(kEntryPC, FailureKind::LowBlockInstructionRange);

  if (Low.AbstractValues.size() > std::numeric_limits<uint32_t>::max())
    return fail(kEntryPC, FailureKind::LowAbstractValueTable);
  for (size_t Index = 0; Index < Low.AbstractValues.size(); ++Index) {
    const LowAbstractValue &Value = Low.AbstractValues[Index];
    const auto Expected =
        static_cast<LowAbstractValueID>(static_cast<uint32_t>(Index));
    if (Value.ID != Expected || !validAbstractExactness(Value.Exactness))
      return fail(kEntryPC, FailureKind::LowAbstractValueTable);
    if (llvm::any_of(Value.Constants, [](const llvm::APInt &Constant) {
          return Constant.getBitWidth() != kWordBits;
        }))
      return fail(kEntryPC, FailureKind::LowAbstractValuePayload);
    const bool HasConstants = !Value.Constants.empty();
    const bool HasSymbol = Value.Symbol.has_value();
    const bool HasExpression = Value.Expression.has_value();
    switch (Value.Kind) {
    case LowAbstractValueKind::Top:
      if (Value.Exactness != LowAbstractExactness::OverApproximation ||
          HasConstants || HasSymbol || HasExpression)
        return fail(kEntryPC, FailureKind::LowAbstractValuePayload);
      break;
    case LowAbstractValueKind::ConstantSet:
      if (!HasConstants || HasSymbol || HasExpression ||
          !isSortedAndUnique(Value.Constants, wordLess))
        return fail(kEntryPC, FailureKind::LowAbstractValuePayload);
      break;
    case LowAbstractValueKind::Symbol:
      if (HasConstants || !HasSymbol || HasExpression ||
          Value.Exactness != LowAbstractExactness::Exact)
        return fail(kEntryPC, FailureKind::LowAbstractValuePayload);
      break;
    case LowAbstractValueKind::Expression:
      if (HasConstants || HasSymbol || !HasExpression)
        return fail(kEntryPC, FailureKind::LowAbstractValuePayload);
      for (LowAbstractValueID Operand : Value.Expression->Operands)
        if (lowAbstractValueIndex(Operand) >= Low.AbstractValues.size())
          return fail(kEntryPC, FailureKind::LowAbstractValueReference);
      break;
    default:
      return fail(kEntryPC, FailureKind::LowAbstractValuePayload);
    }
  }

  if (Low.AbstractStacks.size() > std::numeric_limits<uint32_t>::max())
    return fail(kEntryPC, FailureKind::LowAbstractStackTable);
  for (size_t Index = 0; Index < Low.AbstractStacks.size(); ++Index) {
    const LowAbstractStack &Stack = Low.AbstractStacks[Index];
    const auto Expected =
        static_cast<LowAbstractStackID>(static_cast<uint32_t>(Index));
    if (Stack.ID != Expected)
      return fail(kEntryPC, FailureKind::LowAbstractStackTable);
    for (LowAbstractValueID Value : Stack.Words)
      if (lowAbstractValueIndex(Value) >= Low.AbstractValues.size())
        return fail(kEntryPC, FailureKind::LowAbstractStackReference);
  }

  size_t ExpectedLaneCount = 0;
  for (const LowBlock &Block : Low.Blocks) {
    if (Block.StateLanes.size() > std::numeric_limits<uint32_t>::max())
      return fail(Block.StartPC, FailureKind::LowBlockLaneTable);
    for (size_t Ordinal = 0; Ordinal < Block.StateLanes.size(); ++Ordinal) {
      const LowStateLaneID Expected{Block.StartPC,
                                    static_cast<uint32_t>(Ordinal)};
      if (Block.StateLanes[Ordinal] != Expected)
        return fail(Block.StartPC, FailureKind::LowBlockLaneTable);
    }
    if (Block.StateLanes.size() >
        std::numeric_limits<size_t>::max() - ExpectedLaneCount)
      return fail(Block.StartPC, FailureKind::LowBlockLaneTable);
    ExpectedLaneCount += Block.StateLanes.size();
  }
  if (ExpectedLaneCount != Low.StateLanes.size())
    return fail(kEntryPC, FailureKind::LowStateLaneTable);

  size_t LaneIndex = 0;
  for (const LowBlock &Block : Low.Blocks)
    for (LowStateLaneID Expected : Block.StateLanes) {
      const LowStateLane &Lane = Low.StateLanes[LaneIndex++];
      if (Lane.ID != Expected || !Lane.ID.isValid() ||
          !validReachability(Lane.Evidence))
        return fail(Block.StartPC, FailureKind::LowStateLaneTable);
      if (lowAbstractStackIndex(Lane.EntryState) >= Low.AbstractStacks.size() ||
          !Lane.ExitState ||
          lowAbstractStackIndex(*Lane.ExitState) >= Low.AbstractStacks.size())
        return fail(Block.StartPC, FailureKind::LowStateLaneRecord);
    }

  for (const LowAbstractValue &Value : Low.AbstractValues)
    if (Value.Symbol && !Low.findStateLane(Value.Symbol->ProducerLane))
      return fail(Value.Symbol->ProducerPC,
                  FailureKind::LowAbstractValueReference);

  const auto FaultLess = [](const LowFaultPrefix &Left,
                            const LowFaultPrefix &Right) {
    return std::tie(Left.Lane, Left.PC, Left.Kind) <
           std::tie(Right.Lane, Right.PC, Right.Kind);
  };
  for (const LowBlock &Block : Low.Blocks) {
    if (!isSortedAndUnique(Block.FaultPrefixes, FaultLess))
      return fail(Block.StartPC, FailureKind::LowFaultTable);
    for (const LowFaultPrefix &Fault : Block.FaultPrefixes) {
      const LowStateLane *Lane = Low.findStateLane(Fault.Lane);
      const LowAbstractStack *Entry =
          Lane ? Low.findAbstractStack(Lane->EntryState) : nullptr;
      const LowInstruction *Instruction = instructionAt(Low, Fault.PC);
      if (!validFaultKind(Fault.Kind) || !Lane || !Entry ||
          Fault.Lane.BlockPC != Block.StartPC ||
          Fault.EntryStackHeight != Entry->Words.size() || !Instruction ||
          Fault.PC < Block.StartPC || Fault.PC >= Block.EndPC)
        return fail(Fault.PC, FailureKind::LowFaultReference);
    }
  }

  const auto TransitionLess = [](const LowLaneTransition &Left,
                                 const LowLaneTransition &Right) {
    return std::tie(Left.Source, Left.Kind, Left.TargetPC, Left.Target,
                    Left.Evidence) < std::tie(Right.Source, Right.Kind,
                                              Right.TargetPC, Right.Target,
                                              Right.Evidence);
  };
  if (!isSortedAndUnique(Low.LaneTransitions, TransitionLess))
    return fail(kEntryPC, FailureKind::LowTransitionTable);
  for (const LowLaneTransition &Transition : Low.LaneTransitions) {
    const LowStateLane *Source = Low.findStateLane(Transition.Source);
    const LowStateLane *Target =
        Transition.Target ? Low.findStateLane(*Transition.Target) : nullptr;
    const LowAbstractStack *SourceExit =
        Source && Source->ExitState ? Low.findAbstractStack(*Source->ExitState)
                                    : nullptr;
    const LowAbstractStack *TargetEntry =
        Target ? Low.findAbstractStack(Target->EntryState) : nullptr;
    if (!validEdgeKind(Transition.Kind) ||
        !validReachability(Transition.Evidence) || !Source || !SourceExit ||
        Transition.Target.has_value() != Transition.TargetPC.has_value() ||
        (Transition.Target.has_value() && !Target) ||
        (Transition.Target && Transition.TargetPC &&
         Transition.Target->BlockPC != *Transition.TargetPC) ||
        (Target && (!TargetEntry || Target->Evidence != Transition.Evidence)) ||
        (TargetEntry && SourceExit->Words.size() != TargetEntry->Words.size()))
      return fail(Transition.Source.BlockPC,
                  FailureKind::LowTransitionReference);
  }
  return std::nullopt;
}

bool exactJoin(const EVMMedIR &Med, ValueID Joined,
               llvm::ArrayRef<MedPhiIncoming> Incomings) {
  if (Incomings.empty() || Joined >= Med.Values.size())
    return false;
  const ValueID First = Incomings.front().Value;
  if (llvm::all_of(Incomings, [First](const MedPhiIncoming &Incoming) {
        return Incoming.Value == First;
      }))
    return Joined == First;
  const MedValue &Value = Med.Values[Joined];
  return Value.Kind == ValueKind::Phi &&
         Value.PhiIncomings.size() == Incomings.size() &&
         std::equal(Value.PhiIncomings.begin(), Value.PhiIncomings.end(),
                    Incomings.begin());
}

bool chargeWithin(size_t &Used, size_t Amount, size_t Limit) {
  if (Used > Limit || Amount > Limit - Used)
    return false;
  Used += Amount;
  return true;
}

std::optional<Failure>
verifyLowIRResourceBounds(const EVMLowIR &Low, const AnalyzeOptions &Options) {
#define EVM_ANALYSIS_LIMIT_DECODE(NAME, DEFAULT_VALUE)                         \
  if (Options.NAME == 0)                                                       \
    return fail(kEntryPC, FailureKind::AnalysisResourceLimit);
#define EVM_ANALYSIS_LIMIT_CONTROL_FLOW(NAME, DEFAULT_VALUE)                   \
  if (Options.NAME == 0)                                                       \
    return fail(kEntryPC, FailureKind::AnalysisResourceLimit);
#define EVM_ANALYSIS_LIMIT_MEDIUM_IR(NAME, DEFAULT_VALUE)                      \
  if (Options.NAME == 0)                                                       \
    return fail(kEntryPC, FailureKind::AnalysisResourceLimit);
#define EVM_ANALYSIS_LIMIT_HIGH_IR(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT(STAGE, NAME, DEFAULT_VALUE)                         \
  EVM_ANALYSIS_LIMIT_##STAGE(NAME, DEFAULT_VALUE)
#include "neverd/evm/analysis/EVMAnalysisLimits.def"
#undef EVM_ANALYSIS_LIMIT_DECODE
#undef EVM_ANALYSIS_LIMIT_CONTROL_FLOW
#undef EVM_ANALYSIS_LIMIT_MEDIUM_IR
#undef EVM_ANALYSIS_LIMIT_HIGH_IR

  if (Low.Code.size() > Options.MaxCodeSize ||
      Low.Instructions.size() > Options.MaxInstructions ||
      Low.Blocks.size() > Options.MaxBlocks ||
      Low.AbstractValues.size() > Options.MaxAbstractValueNodes ||
      Low.AbstractStacks.size() > Options.MaxAbstractStackNodes ||
      Low.StateLanes.size() > Options.MaxMedStateLanes ||
      Low.LaneTransitions.size() > Options.MaxEdges ||
      Low.Diagnostics.size() > Options.MaxLowDiagnostics)
    return fail(kEntryPC, FailureKind::AnalysisResourceLimit);

  size_t DiagnosticBytes = 0;
  for (const Diagnostic &Diagnostic : Low.Diagnostics)
    if (!chargeWithin(DiagnosticBytes, Diagnostic.Message.size(),
                      Options.MaxLowDiagnosticBytes))
      return fail(Diagnostic.PC, FailureKind::AnalysisResourceLimit);

  for (const LowAbstractValue &Value : Low.AbstractValues) {
    if (Value.Constants.size() > Options.MaxAbstractValuesPerSlot ||
        (Value.Expression &&
         Value.Expression->Operands.size() > kMaxOpcodeStackPops))
      return fail(kEntryPC, FailureKind::AnalysisResourceLimit);
  }

  size_t AbstractStackEntries = 0;
  for (const LowAbstractStack &Stack : Low.AbstractStacks)
    if (Stack.Words.size() > kStackLimit ||
        !chargeWithin(AbstractStackEntries, Stack.Words.size(),
                      Options.MaxAbstractStackEntries))
      return fail(kEntryPC, FailureKind::AnalysisResourceLimit);

  size_t LowLaneReferences = 0;
  size_t LowEdgeReferences = Low.LaneTransitions.size();
  size_t LowPredecessorReferences = 0;
  size_t LowMayPredecessorReferences = 0;
  size_t LowFaultReferences = 0;
  for (const LowBlock &Block : Low.Blocks) {
    if (Block.StateLanes.size() > Options.MaxAbstractStateLanesPerBlock ||
        !chargeWithin(LowLaneReferences, Block.StateLanes.size(),
                      Options.MaxMedStateLanes) ||
        !chargeWithin(LowEdgeReferences, Block.Successors.size(),
                      Options.MaxEdges) ||
        !chargeWithin(LowPredecessorReferences, Block.Predecessors.size(),
                      Options.MaxEdges) ||
        !chargeWithin(LowMayPredecessorReferences, Block.MayPredecessors.size(),
                      Options.MaxEdges) ||
        !chargeWithin(LowFaultReferences, Block.FaultPrefixes.size(),
                      Options.MaxMedStateLanes) ||
        Block.EntryStackHeights.values().size() >
            Options.MaxStackHeightVariants ||
        Block.ExitStackHeights.values().size() >
            Options.MaxStackHeightVariants ||
        Block.MayEntryStackHeights.values().size() >
            Options.MaxStackHeightVariants ||
        Block.MayExitStackHeights.values().size() >
            Options.MaxStackHeightVariants)
      return fail(Block.StartPC, FailureKind::AnalysisResourceLimit);
  }
  return std::nullopt;
}

std::optional<Failure> verifyResourceBounds(const EVMLowIR &Low,
                                            const EVMMedIR &Med,
                                            const AnalyzeOptions &Options) {
  if (auto Failure = verifyLowIRResourceBounds(Low, Options))
    return Failure;

  if (Low.Diagnostics.size() > Options.MaxHighDiagnostics ||
      Med.Diagnostics.size() >
          Options.MaxHighDiagnostics - Low.Diagnostics.size() ||
      Med.Values.size() > Options.MaxMedValues ||
      Med.Blocks.size() > Options.MaxBlocks ||
      Med.StateLanes.size() > Options.MaxMedStateLanes ||
      Med.Values.size() > Options.MaxMedWorklistUpdates)
    return fail(kEntryPC, FailureKind::AnalysisResourceLimit);

  size_t DiagnosticBytes = 0;
  const auto ChargeDiagnostics = [&](llvm::ArrayRef<Diagnostic> Diagnostics) {
    for (const Diagnostic &Diagnostic : Diagnostics) {
      if (!chargeWithin(DiagnosticBytes, Diagnostic.Message.size(),
                        Options.MaxHighDiagnosticBytes))
        return false;
    }
    return true;
  };
  if (!ChargeDiagnostics(Low.Diagnostics) ||
      !ChargeDiagnostics(Med.Diagnostics))
    return fail(kEntryPC, FailureKind::AnalysisResourceLimit);

  size_t PhiIncomings = 0;
  for (const MedValue &Value : Med.Values) {
    if (Value.Kind == ValueKind::Phi) {
      if (!chargeWithin(PhiIncomings, Value.PhiIncomings.size(),
                        Options.MaxMedPhiIncomings) ||
          Value.Inputs.size() > Options.MaxMedPhiIncomings)
        return fail(Value.PC, FailureKind::AnalysisResourceLimit);
    } else if (Value.Inputs.size() > kMaxOpcodeStackPops) {
      return fail(Value.PC, FailureKind::AnalysisResourceLimit);
    }
  }

  size_t StackEntries = 0;
  for (const MedStateLane &Lane : Med.StateLanes)
    if (!chargeWithin(StackEntries, Lane.EntryStack.size(),
                      Options.MaxMedStackEntries) ||
        !chargeWithin(StackEntries, Lane.ExitStack.size(),
                      Options.MaxMedStackEntries))
      return fail(Lane.LowLane.BlockPC, FailureKind::AnalysisResourceLimit);

  size_t Operations = 0;
  size_t OperationLaneReferences = 0;
  size_t BlockLaneReferences = 0;
  for (const MedBlock &Block : Med.Blocks) {
    if (!chargeWithin(BlockLaneReferences, Block.StateLanes.size(),
                      Options.MaxMedStateLanes) ||
        !chargeWithin(StackEntries, Block.EntryStack.size(),
                      Options.MaxMedStackEntries) ||
        !chargeWithin(StackEntries, Block.ExitStack.size(),
                      Options.MaxMedStackEntries) ||
        Block.PhiValues.size() > Block.EntryStack.size() ||
        !chargeWithin(Operations, Block.Operations.size(),
                      Options.MaxMedOperations))
      return fail(Block.StartPC, FailureKind::AnalysisResourceLimit);
    for (const MedOperation &Operation : Block.Operations)
      if (Operation.Inputs.size() > kMaxOpcodeStackPops ||
          Operation.Outputs.size() > kMaxOpcodeStackPops ||
          !chargeWithin(OperationLaneReferences,
                        Operation.ExecutingLanes.size(),
                        Options.MaxMedOperationLaneReferences) ||
          !chargeWithin(OperationLaneReferences, Operation.FaultingLanes.size(),
                        Options.MaxMedOperationLaneReferences))
        return fail(Operation.PC, FailureKind::AnalysisResourceLimit);
  }
  return std::nullopt;
}

bool sameDiagnostics(llvm::ArrayRef<Diagnostic> Left,
                     llvm::ArrayRef<Diagnostic> Right) {
  if (Left.size() != Right.size())
    return false;
  for (size_t Index = 0; Index < Left.size(); ++Index)
    if (Left[Index].PC != Right[Index].PC ||
        Left[Index].Message != Right[Index].Message)
      return false;
  return true;
}

bool sameHeightDomain(const StackHeightDomain &Left,
                      const StackHeightDomain &Right) {
  return Left.values() == Right.values();
}

bool sameEdges(llvm::ArrayRef<LowEdge> Left, llvm::ArrayRef<LowEdge> Right) {
  if (Left.size() != Right.size())
    return false;
  for (size_t Index = 0; Index < Left.size(); ++Index)
    if (Left[Index].Kind != Right[Index].Kind ||
        Left[Index].Target != Right[Index].Target ||
        Left[Index].Evidence != Right[Index].Evidence)
      return false;
  return true;
}

bool sameFaults(llvm::ArrayRef<LowFaultPrefix> Left,
                llvm::ArrayRef<LowFaultPrefix> Right) {
  if (Left.size() != Right.size())
    return false;
  for (size_t Index = 0; Index < Left.size(); ++Index)
    if (Left[Index].Lane != Right[Index].Lane ||
        Left[Index].EntryStackHeight != Right[Index].EntryStackHeight ||
        Left[Index].PC != Right[Index].PC ||
        Left[Index].Kind != Right[Index].Kind)
      return false;
  return true;
}

bool sameBlock(const LowBlock &Left, const LowBlock &Right) {
  return Left.StartPC == Right.StartPC && Left.EndPC == Right.EndPC &&
         Left.FirstInstruction == Right.FirstInstruction &&
         Left.InstructionCount == Right.InstructionCount &&
         Left.StateLanes == Right.StateLanes &&
         sameEdges(Left.Successors, Right.Successors) &&
         Left.Predecessors == Right.Predecessors &&
         Left.MayPredecessors == Right.MayPredecessors &&
         Left.Reachable == Right.Reachable &&
         Left.MayReachable == Right.MayReachable &&
         Left.HasIndirectSuccessor == Right.HasIndirectSuccessor &&
         sameHeightDomain(Left.EntryStackHeights, Right.EntryStackHeights) &&
         sameHeightDomain(Left.ExitStackHeights, Right.ExitStackHeights) &&
         sameHeightDomain(Left.MayEntryStackHeights,
                          Right.MayEntryStackHeights) &&
         sameHeightDomain(Left.MayExitStackHeights,
                          Right.MayExitStackHeights) &&
         sameFaults(Left.FaultPrefixes, Right.FaultPrefixes);
}

std::optional<uint64_t> lowMismatchPC(const EVMLowIR &Actual,
                                      const EVMLowIR &Canonical) {
  if (Actual.Fork != Canonical.Fork || Actual.Strict != Canonical.Strict ||
      Actual.Code != Canonical.Code ||
      Actual.Instructions.size() != Canonical.Instructions.size() ||
      Actual.Blocks.size() != Canonical.Blocks.size() ||
      Actual.AbstractValues != Canonical.AbstractValues ||
      Actual.AbstractStacks != Canonical.AbstractStacks ||
      Actual.StateLanes != Canonical.StateLanes ||
      Actual.LaneTransitions.size() != Canonical.LaneTransitions.size() ||
      Actual.JumpDestinations != Canonical.JumpDestinations ||
      !sameDiagnostics(Actual.Diagnostics, Canonical.Diagnostics))
    return kEntryPC;
  for (size_t Index = 0; Index < Actual.Instructions.size(); ++Index)
    if (!sameInstruction(Actual.Instructions[Index],
                         Canonical.Instructions[Index]))
      return Actual.Instructions[Index].PC;
  for (size_t Index = 0; Index < Actual.Blocks.size(); ++Index)
    if (!sameBlock(Actual.Blocks[Index], Canonical.Blocks[Index]))
      return Actual.Blocks[Index].StartPC;
  for (size_t Index = 0; Index < Actual.LaneTransitions.size(); ++Index)
    if (Actual.LaneTransitions[Index] != Canonical.LaneTransitions[Index])
      return Actual.LaneTransitions[Index].Source.BlockPC;
  return std::nullopt;
}

bool sameValue(const MedValue &Left, const MedValue &Right) {
  return Left.ID == Right.ID && Left.Kind == Right.Kind &&
         Left.PC == Right.PC && Left.Name == Right.Name &&
         Left.Inputs == Right.Inputs &&
         Left.PhiIncomings == Right.PhiIncomings &&
         Left.Constant == Right.Constant;
}

bool sameOperation(const MedOperation &Left, const MedOperation &Right) {
  return Left.PC == Right.PC && Left.Op == Right.Op &&
         Left.Name == Right.Name && Left.Inputs == Right.Inputs &&
         Left.Outputs == Right.Outputs && Left.Effect == Right.Effect &&
         Left.MemoryAccess == Right.MemoryAccess &&
         Left.StateAccess == Right.StateAccess &&
         Left.CallValueAccess == Right.CallValueAccess &&
         Left.ExecutingLanes == Right.ExecutingLanes &&
         Left.FaultingLanes == Right.FaultingLanes;
}

std::optional<uint64_t> medMismatchPC(const EVMMedIR &Actual,
                                      const EVMMedIR &Canonical) {
  if (Actual.Values.size() != Canonical.Values.size() ||
      Actual.Blocks.size() != Canonical.Blocks.size() ||
      Actual.StateLanes != Canonical.StateLanes ||
      !sameDiagnostics(Actual.Diagnostics, Canonical.Diagnostics))
    return kEntryPC;
  for (size_t Index = 0; Index < Actual.Values.size(); ++Index)
    if (!sameValue(Actual.Values[Index], Canonical.Values[Index]))
      return Actual.Values[Index].PC;
  for (size_t BlockIndex = 0; BlockIndex < Actual.Blocks.size(); ++BlockIndex) {
    const MedBlock &Left = Actual.Blocks[BlockIndex];
    const MedBlock &Right = Canonical.Blocks[BlockIndex];
    if (Left.StartPC != Right.StartPC || Left.StateLanes != Right.StateLanes ||
        Left.EntryStack != Right.EntryStack ||
        Left.PhiValues != Right.PhiValues ||
        Left.ExitStack != Right.ExitStack ||
        Left.Operations.size() != Right.Operations.size())
      return Left.StartPC;
    for (size_t OperationIndex = 0; OperationIndex < Left.Operations.size();
         ++OperationIndex)
      if (!sameOperation(Left.Operations[OperationIndex],
                         Right.Operations[OperationIndex]))
        return Left.Operations[OperationIndex].PC;
  }
  return std::nullopt;
}

} // namespace

llvm::Error verifyLowIRForExecution(const EVMLowIR &Low) {
  const auto Failure = verifyLowIRExecutionStructure(Low);
  if (!Failure)
    return llvm::Error::success();
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine(kExecutionLowIRDiagnosticPrefix) +
       llvm::utohexstr(Failure->PC) + kExecutionLowIRDiagnosticInfix +
       medIRValidationFailureMessage(Failure->Kind))
          .str(),
      llvm::inconvertibleErrorCode());
}

llvm::Error verifyLowIRForMedLowering(const EVMLowIR &Low,
                                      const AnalyzeOptions &Options) {
  if (llvm::Error Error = validateMedAnalysisOptions(Options))
    return Error;
  std::optional<Failure> Failure = verifyLowIRResourceBounds(Low, Options);
  if (!Failure)
    Failure = verifyLowIR(Low);
  if (!Failure)
    return llvm::Error::success();
  return medLoweringValidationError(*Failure);
}

llvm::Error verifyCanonicalLowIRForMedLowering(const EVMLowIR &Low,
                                               const AnalyzeOptions &Options) {
  if (llvm::Error Error = verifyLowIRForMedLowering(Low, Options))
    return Error;

  AnalyzeOptions ReplayOptions = Options;
  ReplayOptions.Fork = Low.Fork;
  ReplayOptions.Strict = Low.Strict;
  ReplayOptions.RecoverHighLevel = false;
  auto Canonical = decodeLowIR(Low.Code, ReplayOptions);
  if (!Canonical) {
    llvm::consumeError(Canonical.takeError());
    return medLoweringValidationError(
        {kEntryPC, FailureKind::CanonicalReplayFailure});
  }
  if (const auto PC = lowMismatchPC(Low, *Canonical))
    return medLoweringValidationError({*PC, FailureKind::LowCanonicalMismatch});
  return llvm::Error::success();
}

llvm::StringRef medIRValidationFailureMessage(MedIRValidationFailureKind Kind) {
  switch (Kind) {
#define EVM_MED_IR_VALIDATION_FAILURE(ID, MESSAGE)                             \
  case MedIRValidationFailureKind::ID:                                         \
    return MESSAGE;
#include "neverd/evm/analysis/EVMMedIRValidation.def"
  }
  llvm_unreachable("invalid MedIR validation failure kind");
}

std::optional<MedIRValidationFailure>
verifyMedIRStructure(const EVMLowIR &Low, const EVMMedIR &Med) {
  if (auto Failure = verifyLowIR(Low))
    return Failure;
  if (Med.Blocks.size() != Low.Blocks.size() ||
      Med.StateLanes.size() != Low.StateLanes.size())
    return fail(kEntryPC, FailureKind::MedTableCardinality);
  if (Med.Values.size() > std::numeric_limits<ValueID>::max() ||
      Med.StateLanes.size() > std::numeric_limits<uint32_t>::max())
    return fail(kEntryPC, FailureKind::MedTableCardinality);

  const auto ValidValue = [&](ValueID ID) { return ID < Med.Values.size(); };
  const auto ValidLane = [&](MedStateLaneID ID) {
    return medStateLaneIndex(ID) < Med.StateLanes.size();
  };

  for (size_t Index = 0; Index < Med.Values.size(); ++Index) {
    const MedValue &Value = Med.Values[Index];
    if (Value.ID != Index)
      return fail(Value.PC, FailureKind::MedValueTable);
    if (!validValueKind(Value.Kind) ||
        (Value.Constant && Value.Constant->getBitWidth() != kWordBits))
      return fail(Value.PC, FailureKind::MedValuePayload);
    if (!llvm::all_of(Value.Inputs, ValidValue))
      return fail(Value.PC, FailureKind::MedValueReference);
    if (Value.Kind != ValueKind::Phi) {
      if (!Value.PhiIncomings.empty())
        return fail(Value.PC, FailureKind::MedValuePayload);
      if ((Value.Kind == ValueKind::Constant && !Value.Constant) ||
          (Value.Kind == ValueKind::Unknown &&
           (Value.Constant || !Value.Inputs.empty())))
        return fail(Value.PC, FailureKind::MedValuePayload);
      continue;
    }
    if (Value.PhiIncomings.empty() ||
        Value.PhiIncomings.size() != Value.Inputs.size() ||
        !isSortedAndUnique(Value.PhiIncomings, [](const MedPhiIncoming &Left,
                                                  const MedPhiIncoming &Right) {
          return Left.SourceLane < Right.SourceLane;
        }))
      return fail(Value.PC, FailureKind::MedPhiTable);
    for (size_t IncomingIndex = 0; IncomingIndex < Value.PhiIncomings.size();
         ++IncomingIndex) {
      const MedPhiIncoming &Incoming = Value.PhiIncomings[IncomingIndex];
      if (!ValidLane(Incoming.SourceLane) || !ValidValue(Incoming.Value) ||
          Incoming.Value != Value.Inputs[IncomingIndex])
        return fail(Value.PC, FailureKind::MedPhiReference);
    }
  }

  std::map<LowStateLaneID, MedStateLaneID> MedByLowLane;
  for (size_t Index = 0; Index < Med.StateLanes.size(); ++Index) {
    const MedStateLane &Lane = Med.StateLanes[Index];
    const auto Expected =
        static_cast<MedStateLaneID>(static_cast<uint32_t>(Index));
    if (Lane.ID != Expected || !isValidMedStateLaneID(Lane.ID))
      return fail(Lane.LowLane.BlockPC, FailureKind::MedStateLaneTable);
    if (Lane.LowLane != Low.StateLanes[Index].ID ||
        Lane.Evidence != Low.StateLanes[Index].Evidence ||
        !MedByLowLane.emplace(Lane.LowLane, Lane.ID).second)
      return fail(Lane.LowLane.BlockPC, FailureKind::MedStateLaneReference);
    const LowStateLane &LowLane = Low.StateLanes[Index];
    const LowAbstractStack *LowEntry =
        Low.findAbstractStack(LowLane.EntryState);
    const LowAbstractStack *LowExit =
        LowLane.ExitState ? Low.findAbstractStack(*LowLane.ExitState) : nullptr;
    if (!LowEntry || !LowExit ||
        Lane.EntryStack.size() != LowEntry->Words.size() ||
        Lane.ExitStack.size() != LowExit->Words.size() ||
        !llvm::all_of(Lane.EntryStack, ValidValue) ||
        !llvm::all_of(Lane.ExitStack, ValidValue))
      return fail(Lane.LowLane.BlockPC, FailureKind::MedStateLaneStack);
  }

  std::map<LowStateLaneID, std::vector<MedStateLaneID>> IncomingLanes;
  for (const LowLaneTransition &Transition : Low.LaneTransitions)
    if (Transition.Target)
      IncomingLanes[*Transition.Target].push_back(
          MedByLowLane.find(Transition.Source)->second);
  for (auto &[Target, Sources] : IncomingLanes) {
    (void)Target;
    llvm::sort(Sources);
    Sources.erase(std::unique(Sources.begin(), Sources.end()), Sources.end());
  }

  for (const MedStateLane &Lane : Med.StateLanes) {
    const bool IsRoot = Lane.LowLane.BlockPC == kEntryPC &&
                        Lane.LowLane.Ordinal == kEntryStateLaneOrdinal;
    const auto SourcesIt = IncomingLanes.find(Lane.LowLane);
    const llvm::ArrayRef<MedStateLaneID> Sources =
        SourcesIt == IncomingLanes.end()
            ? llvm::ArrayRef<MedStateLaneID>{}
            : llvm::ArrayRef<MedStateLaneID>(SourcesIt->second);
    if ((IsRoot && !Lane.EntryStack.empty()) || (!IsRoot && Sources.empty()))
      return fail(Lane.LowLane.BlockPC, FailureKind::MedStateLanePhi);
    for (size_t Slot = 0; Slot < Lane.EntryStack.size(); ++Slot) {
      const MedValue &Phi = Med.Values[Lane.EntryStack[Slot]];
      if (Phi.Kind != ValueKind::Phi ||
          Phi.PhiIncomings.size() != Sources.size())
        return fail(Lane.LowLane.BlockPC, FailureKind::MedStateLanePhi);
      for (size_t SourceIndex = 0; SourceIndex < Sources.size();
           ++SourceIndex) {
        const MedStateLaneID SourceID = Sources[SourceIndex];
        const MedStateLane &Source =
            Med.StateLanes[medStateLaneIndex(SourceID)];
        if (Source.ExitStack.size() <= Slot ||
            Phi.PhiIncomings[SourceIndex] !=
                MedPhiIncoming{SourceID, Source.ExitStack[Slot]})
          return fail(Lane.LowLane.BlockPC, FailureKind::MedStateLanePhi);
      }
    }
  }

  std::vector<const MedOperation *> Definitions(Med.Values.size(), nullptr);
  std::set<std::pair<LowStateLaneID, uint64_t>> ExpectedFaults;
  for (const LowBlock &Block : Low.Blocks)
    for (const LowFaultPrefix &Fault : Block.FaultPrefixes)
      ExpectedFaults.emplace(Fault.Lane, Fault.PC);
  std::set<std::pair<MedStateLaneID, uint64_t>> SeenFaults;
  for (size_t BlockIndex = 0; BlockIndex < Med.Blocks.size(); ++BlockIndex) {
    const MedBlock &Block = Med.Blocks[BlockIndex];
    const LowBlock &LowBlock = Low.Blocks[BlockIndex];
    if (Block.StartPC != LowBlock.StartPC)
      return fail(Block.StartPC, FailureKind::MedBlockTable);
    if (Block.StateLanes.size() != LowBlock.StateLanes.size() ||
        !isSortedAndUnique(Block.StateLanes))
      return fail(Block.StartPC, FailureKind::MedBlockLaneTable);
    for (size_t LaneOrdinal = 0; LaneOrdinal < Block.StateLanes.size();
         ++LaneOrdinal) {
      const auto LaneIt = MedByLowLane.find(LowBlock.StateLanes[LaneOrdinal]);
      if (LaneIt == MedByLowLane.end() ||
          Block.StateLanes[LaneOrdinal] != LaneIt->second)
        return fail(Block.StartPC, FailureKind::MedBlockLaneTable);
    }
    if (!llvm::all_of(Block.EntryStack, ValidValue) ||
        !llvm::all_of(Block.PhiValues, ValidValue) ||
        !llvm::all_of(Block.ExitStack, ValidValue) ||
        !isSortedAndUnique(Block.PhiValues))
      return fail(Block.StartPC, FailureKind::MedBlockStack);

    std::vector<MedStateLaneID> ReachableLanes;
    for (MedStateLaneID ID : Block.StateLanes)
      if (Med.StateLanes[medStateLaneIndex(ID)].Evidence ==
          Reachability::Reachable)
        ReachableLanes.push_back(ID);
    const auto VerifyCompatibilityStack = [&](llvm::ArrayRef<ValueID> Joined,
                                              bool Entry) {
      if (ReachableLanes.empty())
        return Joined.empty();
      const auto &First =
          Med.StateLanes[medStateLaneIndex(ReachableLanes.front())];
      const size_t Height =
          Entry ? First.EntryStack.size() : First.ExitStack.size();
      if (llvm::any_of(ReachableLanes, [&](MedStateLaneID ID) {
            const MedStateLane &Candidate =
                Med.StateLanes[medStateLaneIndex(ID)];
            return (Entry ? Candidate.EntryStack.size()
                          : Candidate.ExitStack.size()) != Height;
          }))
        return Joined.empty();
      if (Joined.size() != Height)
        return false;
      for (size_t Slot = 0; Slot < Height; ++Slot) {
        std::vector<MedPhiIncoming> Incomings;
        Incomings.reserve(ReachableLanes.size());
        for (MedStateLaneID ID : ReachableLanes) {
          const MedStateLane &Candidate = Med.StateLanes[medStateLaneIndex(ID)];
          Incomings.push_back({ID, Entry ? Candidate.EntryStack[Slot]
                                         : Candidate.ExitStack[Slot]});
        }
        if (!exactJoin(Med, Joined[Slot], Incomings))
          return false;
      }
      return true;
    };
    if (!VerifyCompatibilityStack(Block.EntryStack, true) ||
        !VerifyCompatibilityStack(Block.ExitStack, false))
      return fail(Block.StartPC, FailureKind::MedBlockStack);
    std::vector<ValueID> ExpectedPhiValues;
    for (ValueID ID : Block.EntryStack)
      if (Med.Values[ID].Kind == ValueKind::Phi)
        ExpectedPhiValues.push_back(ID);
    llvm::sort(ExpectedPhiValues);
    ExpectedPhiValues.erase(
        std::unique(ExpectedPhiValues.begin(), ExpectedPhiValues.end()),
        ExpectedPhiValues.end());
    if (Block.PhiValues != ExpectedPhiValues)
      return fail(Block.StartPC, FailureKind::MedBlockStack);

    uint64_t PreviousOperationPC = 0;
    bool FirstOperation = true;
    for (const MedOperation &Operation : Block.Operations) {
      if ((!FirstOperation && Operation.PC <= PreviousOperationPC) ||
          (Operation.ExecutingLanes.empty() && Operation.FaultingLanes.empty()))
        return fail(Operation.PC, FailureKind::MedOperationTable);
      FirstOperation = false;
      PreviousOperationPC = Operation.PC;
      const LowInstruction *Instruction = instructionAt(Low, Operation.PC);
      if (!Instruction || Operation.PC < LowBlock.StartPC ||
          Operation.PC >= LowBlock.EndPC ||
          Instruction->opcode() != Operation.Op)
        return fail(Operation.PC, FailureKind::MedOperationLowLink);
      if (!isSortedAndUnique(Operation.ExecutingLanes) ||
          !isSortedAndUnique(Operation.FaultingLanes))
        return fail(Operation.PC, FailureKind::MedOperationLaneTable);
      for (MedStateLaneID ID : Operation.ExecutingLanes) {
        if (!ValidLane(ID))
          return fail(Operation.PC, FailureKind::MedOperationLaneTable);
        const MedStateLane &Lane = Med.StateLanes[medStateLaneIndex(ID)];
        if (Lane.LowLane.BlockPC != Block.StartPC ||
            std::binary_search(Operation.FaultingLanes.begin(),
                               Operation.FaultingLanes.end(), ID))
          return fail(Operation.PC, FailureKind::MedOperationLaneTable);
      }
      for (MedStateLaneID ID : Operation.FaultingLanes) {
        if (!ValidLane(ID))
          return fail(Operation.PC, FailureKind::MedOperationLaneTable);
        const MedStateLane &Lane = Med.StateLanes[medStateLaneIndex(ID)];
        if (Lane.LowLane.BlockPC != Block.StartPC ||
            !ExpectedFaults.contains({Lane.LowLane, Operation.PC}) ||
            !SeenFaults.emplace(ID, Operation.PC).second)
          return fail(Operation.PC, FailureKind::MedOperationLaneTable);
      }
      if (!llvm::all_of(Operation.Inputs, ValidValue) ||
          !llvm::all_of(Operation.Outputs, ValidValue))
        return fail(Operation.PC, FailureKind::MedOperationReference);
      if (Operation.ExecutingLanes.empty()) {
        if (!Operation.Inputs.empty() || !Operation.Outputs.empty())
          return fail(Operation.PC, FailureKind::MedOperationResult);
      } else {
        if (!Instruction->isExecutable())
          return fail(Operation.PC, FailureKind::MedOperationLowLink);
        const size_t ExpectedInputs =
            Instruction->isPush()  ? 0
            : Instruction->isDup() ? 1
            : (Instruction->isSwap() || Instruction->isExchange())
                ? 2
                : Instruction->Info.StackPops;
        if (Operation.Inputs.size() != ExpectedInputs ||
            Operation.Outputs.size() != Instruction->Info.StackPushes)
          return fail(Operation.PC, FailureKind::MedOperationResult);
      }
      const EffectKind ExpectedEffect = Instruction->isExecutable()
                                            ? Instruction->Info.Effect
                                            : EffectKind::Unknown;
      const MemoryAccessKind ExpectedMemory =
          Instruction->isExecutable() ? Instruction->Info.MemoryAccess
                                      : MemoryAccessKind::Unknown;
      const StateAccessKind ExpectedState = Instruction->isExecutable()
                                                ? Instruction->Info.StateAccess
                                                : StateAccessKind::Unknown;
      const CallValueAccessKind ExpectedCallValue =
          Instruction->isExecutable() ? Instruction->Info.CallValueAccess
                                      : CallValueAccessKind::Unknown;
      if (Operation.Name != Instruction->Info.Name ||
          Operation.Effect != ExpectedEffect ||
          Operation.MemoryAccess != ExpectedMemory ||
          Operation.StateAccess != ExpectedState ||
          Operation.CallValueAccess != ExpectedCallValue)
        return fail(Operation.PC, FailureKind::MedOperationMetadata);
      for (ValueID Output : Operation.Outputs) {
        const MedValue &Result = Med.Values[Output];
        const ValueKind ExpectedKind = Instruction->isPush()
                                           ? ValueKind::Constant
                                           : ValueKind::Instruction;
        if (Definitions[Output] || Result.Kind != ExpectedKind ||
            Result.PC != Operation.PC || Result.Inputs != Operation.Inputs ||
            (Instruction->isPush() &&
             (!Result.Constant || *Result.Constant != Instruction->Immediate)))
          return fail(Operation.PC, FailureKind::MedOperationResult);
        Definitions[Output] = &Operation;
      }
    }
  }

  if (SeenFaults.size() != ExpectedFaults.size())
    return fail(kEntryPC, FailureKind::MedOperationLaneTable);
  for (const MedValue &Value : Med.Values) {
    const bool NeedsOperation = Value.Kind == ValueKind::Constant ||
                                Value.Kind == ValueKind::Instruction;
    if (NeedsOperation != (Definitions[Value.ID] != nullptr))
      return fail(Value.PC, FailureKind::MedValueDefinition);
  }
  return std::nullopt;
}

std::optional<MedIRValidationFailure>
verifyIRResourceBoundsForHighAnalysis(const EVMLowIR &Low, const EVMMedIR &Med,
                                      const AnalyzeOptions &Options) {
  return verifyResourceBounds(Low, Med, Options);
}

std::optional<MedIRValidationFailure>
verifyMedIRForHighAnalysis(const EVMLowIR &Low, const EVMMedIR &Med,
                           const AnalyzeOptions &Options) {
  if (auto Failure = verifyIRResourceBoundsForHighAnalysis(Low, Med, Options))
    return Failure;
  if (auto Failure = verifyMedIRStructure(Low, Med))
    return Failure;

  AnalyzeOptions ReplayOptions = Options;
  ReplayOptions.Fork = Low.Fork;
  ReplayOptions.Strict = Low.Strict;
  ReplayOptions.RecoverHighLevel = false;
  auto CanonicalLow = decodeLowIR(Low.Code, ReplayOptions);
  if (!CanonicalLow) {
    llvm::consumeError(CanonicalLow.takeError());
    return fail(kEntryPC, FailureKind::CanonicalReplayFailure);
  }
  if (const auto PC = lowMismatchPC(Low, *CanonicalLow))
    return fail(*PC, FailureKind::LowCanonicalMismatch);

  auto CanonicalMed = lowerCanonicalLowToMedIR(*CanonicalLow, ReplayOptions);
  if (!CanonicalMed) {
    llvm::consumeError(CanonicalMed.takeError());
    return fail(kEntryPC, FailureKind::CanonicalReplayFailure);
  }
  if (const auto PC = medMismatchPC(Med, *CanonicalMed))
    return fail(*PC, FailureKind::MedCanonicalMismatch);
  return std::nullopt;
}

} // namespace neverd::evm::detail
