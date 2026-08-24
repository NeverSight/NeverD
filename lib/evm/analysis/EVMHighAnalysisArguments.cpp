//===- EVMHighAnalysisArguments.cpp - EVM calldata argument recovery ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMHighAnalysisDetail.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <limits>

namespace neverd::evm::detail {
namespace {

/// The value of \c ArgumentRecovery's owner map for a value that is not a
/// calldata head slot.
inline constexpr size_t kNoArgument = std::numeric_limits<size_t>::max();

/// The argument position a constant calldata offset designates, when the
/// offset starts a head slot. An offset inside a slot reads into a dynamic
/// value's payload rather than naming an argument of its own.
std::optional<size_t> headSlot(uint64_t Offset) {
  if (Offset < kSelectorBytes)
    return std::nullopt;
  const uint64_t Relative = Offset - kSelectorBytes;
  if (Relative % kWordBytes != 0)
    return std::nullopt;
  const uint64_t Position = Relative / kWordBytes;
  if (Position >= kMaxRecoveredArguments)
    return std::nullopt;
  return static_cast<size_t>(Position);
}

} // namespace

llvm::Expected<ArgumentRecovery> ArgumentRecovery::create(
    const EVMMedIR &Med, const ProducerIndex &Index,
    const DefiniteExecutionIndex &Execution,
    const std::set<MedStateLaneID> &Lanes,
    llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  ArgumentRecovery Result(Execution, Lanes, NoteOperationVisit,
                          NoteReferenceVisit);
  if (llvm::Error Error = Result.run(Med, Index))
    return std::move(Error);
  return std::move(Result);
}

llvm::Error ArgumentRecovery::run(const EVMMedIR &Med,
                                  const ProducerIndex &Index) {
  std::set<uint64_t> BlockPCs;
  for (MedStateLaneID ID : Lanes) {
    if (llvm::Error Error = NoteReferenceVisit(kEntryPC))
      return Error;
    if (const MedStateLane *Lane = Execution.lane(ID))
      BlockPCs.insert(Lane->LowLane.BlockPC);
  }
  for (uint64_t PC : BlockPCs) {
    if (llvm::Error Error = NoteReferenceVisit(PC))
      return Error;
    if (const MedBlock *Block = Index.block(PC))
      Ordered.push_back(Block);
  }
  Owner.assign(Med.Values.size(), kNoArgument);
  if (llvm::Error Error = seed(Med))
    return Error;
  if (Constraints.empty())
    return llvm::Error::success();
  if (llvm::Error Error = propagate(Med))
    return Error;
  for (const MedBlock *Block : Ordered)
    for (const MedOperation &Operation : Block->Operations) {
      if (llvm::Error Error = NoteOperationVisit(Operation.PC))
        return Error;
      if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
        return Error;
      auto Executes =
          Execution.executesInAny(Operation, Lanes, NoteReferenceVisit);
      if (!Executes)
        return Executes.takeError();
      if (*Executes)
        if (llvm::Error Error = observe(Med, Operation))
          return Error;
    }
  return llvm::Error::success();
}

llvm::Error ArgumentRecovery::noteReferences(size_t Count, uint64_t PC) const {
  for (size_t Index = 0; Index < Count; ++Index)
    if (llvm::Error Error = NoteReferenceVisit(PC))
      return Error;
  return llvm::Error::success();
}

size_t ArgumentRecovery::owner(ValueID Value) const {
  return Value < Owner.size() ? Owner[Value] : kNoArgument;
}

bool ArgumentRecovery::adopt(ValueID Value, size_t Position) {
  if (Position == kNoArgument || Value >= Owner.size() ||
      Owner[Value] != kNoArgument)
    return false;
  Owner[Value] = Position;
  return true;
}

llvm::Error ArgumentRecovery::seed(const EVMMedIR &Med) {
  llvm::SmallVector<std::pair<ValueID, size_t>, 8> Loads;
  size_t Highest = 0;
  for (const MedBlock *Block : Ordered)
    for (const MedOperation &Operation : Block->Operations) {
      if (llvm::Error Error = NoteOperationVisit(Operation.PC))
        return Error;
      if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
        return Error;
      auto Executes =
          Execution.executesInAny(Operation, Lanes, NoteReferenceVisit);
      if (!Executes)
        return Executes.takeError();
      if (!*Executes)
        continue;
      if (Operation.Op != Opcode::CALLDATALOAD ||
          Operation.Inputs.size() != 1 || Operation.Outputs.size() != 1)
        continue;
      if (llvm::Error Error = noteReferences(
              Operation.Inputs.size() + Operation.Outputs.size(), Operation.PC))
        return Error;
      const auto Offset = constantWord(Med.findValue(Operation.Inputs[0]));
      if (!Offset)
        continue;
      const auto Position = headSlot(*Offset);
      if (!Position)
        continue;
      Loads.emplace_back(Operation.Outputs[0], *Position);
      Highest = std::max(Highest, *Position);
    }
  if (Loads.empty())
    return llvm::Error::success();

  Constraints.resize(Highest + 1);
  Read.assign(Highest + 1, false);
  for (const auto &[Value, Position] : Loads) {
    adopt(Value, Position);
    Read[Position] = true;
  }
  return llvm::Error::success();
}

llvm::Error ArgumentRecovery::propagate(const EVMMedIR &Med) {
  for (size_t Round = 0; Round < kMaxArgumentAliasRounds; ++Round) {
    bool Changed = false;
    for (const MedBlock *Block : Ordered) {
      for (const MedOperation &Operation : Block->Operations) {
        if (llvm::Error Error = NoteOperationVisit(Operation.PC))
          return Error;
        if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
          return Error;
        auto Executes =
            Execution.executesInAny(Operation, Lanes, NoteReferenceVisit);
        if (!Executes)
          return Executes.takeError();
        if (!*Executes)
          continue;
        if (!evm::isDup(Operation.Op) && !evm::isDeepDup(Operation.Op))
          continue;
        if (Operation.Inputs.size() != 1 || Operation.Outputs.size() != 1)
          continue;
        if (llvm::Error Error = noteReferences(Operation.Inputs.size() +
                                                   Operation.Outputs.size(),
                                               Operation.PC))
          return Error;
        Changed |= adopt(Operation.Outputs[0], owner(Operation.Inputs.front()));
      }
      // A merge carries one argument only when every path brought that same
      // argument; a merge of an argument with anything else is neither.
      for (ValueID Phi : Block->PhiValues) {
        if (llvm::Error Error = NoteReferenceVisit(Block->StartPC))
          return Error;
        const MedValue *Value = Med.findValue(Phi);
        if (!Value || Value->PhiIncomings.empty())
          continue;
        size_t Common = kNoArgument;
        bool SawIncoming = false;
        for (const MedPhiIncoming &Incoming : Value->PhiIncomings) {
          if (llvm::Error Error = NoteReferenceVisit(Value->PC))
            return Error;
          if (!Execution.isReachable(Incoming.SourceLane) ||
              !Lanes.contains(Incoming.SourceLane))
            continue;
          const size_t Candidate = owner(Incoming.Value);
          if (!SawIncoming) {
            Common = Candidate;
            SawIncoming = true;
          } else if (Candidate != Common) {
            Common = kNoArgument;
          }
        }
        if (!SawIncoming)
          continue;
        Changed |= adopt(Phi, Common);
      }
    }
    if (!Changed)
      return llvm::Error::success();
  }
  return llvm::Error::success();
}

llvm::Error ArgumentRecovery::observe(const EVMMedIR &Med,
                                      const MedOperation &Operation) {
  if (llvm::Error Error = noteReferences(Operation.Inputs.size(), Operation.PC))
    return Error;
  const auto Argument = [&](size_t Position) -> ABIConstraint * {
    if (Position >= Operation.Inputs.size())
      return nullptr;
    const size_t Owned = owner(Operation.Inputs[Position]);
    return Owned == kNoArgument ? nullptr : &Constraints[Owned];
  };
  const auto Literal = [&](size_t Position) -> const llvm::APInt * {
    if (Position >= Operation.Inputs.size())
      return nullptr;
    const MedValue *Value = Med.findValue(Operation.Inputs[Position]);
    return Value && Value->Constant ? &*Value->Constant : nullptr;
  };
  const auto EveryOperand = [&](ABIEvidence Evidence) {
    for (size_t I = 0; I < Operation.Inputs.size(); ++I)
      if (ABIConstraint *Constraint = Argument(I))
        Constraint->observe(Evidence);
  };

  switch (Operation.Op) {
  case Opcode::AND:
  case Opcode::OR:
    if (Operation.Inputs.size() != 2)
      break;
    for (size_t I = 0; I < 2; ++I) {
      ABIConstraint *Constraint = Argument(I);
      if (!Constraint)
        continue;
      const llvm::APInt *Mask = Literal(1 - I);
      if (!Mask) {
        Constraint->observe(ABIEvidence::Bitwise);
        continue;
      }
      // AND keeps the bytes its mask sets; OR fills them, so what survives of
      // the value is the complement.
      const llvm::APInt Kept = Operation.Op == Opcode::AND ? *Mask : ~*Mask;
      if (const auto Bytes = lowByteMaskWidth(Kept)) {
        Constraint->observe(ABIEvidence::LowByteMask);
        Constraint->narrowTo(*Bytes);
      } else if (const auto Bytes = highByteMaskWidth(Kept)) {
        Constraint->observe(ABIEvidence::HighByteMask);
        Constraint->narrowTo(*Bytes);
      } else {
        Constraint->observe(ABIEvidence::Bitwise);
      }
    }
    break;
  case Opcode::XOR:
  case Opcode::NOT:
    EveryOperand(ABIEvidence::Bitwise);
    break;
  case Opcode::SIGNEXTEND: {
    ABIConstraint *Constraint = Argument(1);
    if (!Constraint)
      break;
    Constraint->observe(ABIEvidence::SignExtended);
    if (const llvm::APInt *Index = Literal(0); Index && Index->ult(kWordBytes))
      Constraint->narrowTo(static_cast<unsigned>(Index->getZExtValue()) + 1);
    break;
  }
  case Opcode::SLT:
  case Opcode::SGT:
  case Opcode::SDIV:
  case Opcode::SMOD:
    EveryOperand(ABIEvidence::SignedCompare);
    break;
  case Opcode::ISZERO:
    if (ABIConstraint *Constraint = Argument(0))
      Constraint->observe(ABIEvidence::BooleanTest);
    break;
  case Opcode::ADD:
  case Opcode::SUB:
  case Opcode::MUL:
  case Opcode::DIV:
  case Opcode::MOD:
  case Opcode::EXP:
  case Opcode::ADDMOD:
  case Opcode::MULMOD:
  case Opcode::LT:
  case Opcode::GT:
    EveryOperand(ABIEvidence::Arithmetic);
    break;
  case Opcode::SHL:
  case Opcode::SHR:
  case Opcode::SAR:
  case Opcode::BYTE:
    // The first operand says how far to shift or which byte to take, so only
    // the second is being treated as a byte string.
    if (ABIConstraint *Constraint = Argument(1))
      Constraint->observe(ABIEvidence::BitShift);
    break;
  case Opcode::BALANCE:
  case Opcode::EXTCODESIZE:
  case Opcode::EXTCODEHASH:
    if (ABIConstraint *Constraint = Argument(0))
      Constraint->observe(ABIEvidence::CallTarget);
    break;
  case Opcode::CALL:
  case Opcode::CALLCODE:
  case Opcode::DELEGATECALL:
  case Opcode::STATICCALL:
    // Every call in the family puts the callee second, after the gas
    // allowance.
    if (ABIConstraint *Constraint = Argument(1))
      Constraint->observe(ABIEvidence::CallTarget);
    break;
  default:
    break;
  }
  return llvm::Error::success();
}

} // namespace neverd::evm::detail
