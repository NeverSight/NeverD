//===- EVMHighAnalysis.cpp - EVM source-level fact recovery -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMHighAnalysisDetail.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd::evm {

using detail::ArgumentRecovery;
using detail::classifyCall;
using detail::classifyDelegation;
using detail::classifyRevert;
using detail::constantWord;
using detail::nonPayableGuardReads;
using detail::ProducerIndex;
using detail::reachesFallback;
using detail::selectorHex;
using detail::SemanticClassifier;
using detail::SemanticKind;
using detail::SemanticValue;
using detail::storageKeyKind;
using detail::wordHexDigits;

namespace {

const LowInstruction *instructionAt(const EVMLowIR &Low, uint64_t PC) {
  const auto It =
      std::lower_bound(Low.Instructions.begin(), Low.Instructions.end(), PC,
                       [](const LowInstruction &Instruction, uint64_t Address) {
                         return Instruction.PC < Address;
                       });
  return It != Low.Instructions.end() && It->PC == PC ? &*It : nullptr;
}

std::set<uint64_t> reachableFrom(const EVMLowIR &Low, uint64_t Entry) {
  std::set<uint64_t> Seen;
  std::queue<uint64_t> Queue;
  if (!Low.findBlock(Entry))
    return Seen;
  Seen.insert(Entry);
  Queue.push(Entry);
  while (!Queue.empty()) {
    const uint64_t PC = Queue.front();
    Queue.pop();
    const LowBlock *Block = Low.findBlock(PC);
    for (const LowEdge &Edge : Block->Successors)
      if (Edge.Target && Seen.insert(*Edge.Target).second)
        Queue.push(*Edge.Target);
  }
  return Seen;
}

Mutability recoveredMutability(StateAccessKind Access, bool ReadsCallValue) {
  if (ReadsCallValue)
    return Mutability::Payable;
  if (Access == StateAccessKind::Unknown)
    return Mutability::NonPayable;
  switch (Access) {
  case StateAccessKind::None:
    return Mutability::Pure;
  case StateAccessKind::Read:
    return Mutability::View;
  case StateAccessKind::Write:
  case StateAccessKind::Unknown:
    return Mutability::NonPayable;
  }
  return Mutability::NonPayable;
}

std::optional<uint64_t> jumpDestination(const EVMMedIR &Med,
                                        const MedOperation &Jump) {
  if (Jump.Inputs.size() != 2)
    return std::nullopt;
  return constantWord(Med.findValue(Jump.Inputs[0]));
}

bool hasConcreteTrueEdge(const EVMLowIR &Low, uint64_t BlockPC,
                         uint64_t Destination) {
  return Low.JumpDestinations.contains(Destination) &&
         Low.hasEdge(BlockPC, Destination, EdgeKind::ConditionalTrue);
}

} // namespace

EVMHighIR recoverHighIR(const EVMLowIR &Low, const EVMMedIR &Med) {
  EVMHighIR High;
  High.Diagnostics = Med.Diagnostics;
  const ProducerIndex Index(Med);
  if (!Index.valid())
    High.Diagnostics.push_back(
        {Index.errorPC(), kMalformedMedIRDiagnostic.str()});
  SemanticClassifier Classifier(Med, Index);

  std::map<uint32_t, RecoveredFunction> Functions;
  std::set<uint32_t> AmbiguousSelectors;
  if (Index.valid()) {
    for (const MedBlock &Block : Med.Blocks) {
      const LowBlock *LowBlock = Low.findBlock(Block.StartPC);
      if (!LowBlock || !LowBlock->Reachable)
        continue;
      for (const MedOperation &Operation : Block.Operations) {
        if (Operation.Op != Opcode::JUMPI || Operation.Inputs.size() != 2)
          continue;
        const SemanticValue Condition =
            Classifier.classify(Operation.Inputs[1]);
        if (Condition.Kind != SemanticKind::SelectorEquality)
          continue;
        const auto Entry = jumpDestination(Med, Operation);
        if (!Entry || !hasConcreteTrueEdge(Low, Block.StartPC, *Entry))
          continue;
        const uint32_t Selector = Condition.Selector;
        if (AmbiguousSelectors.contains(Selector))
          continue;
        auto [FunctionIt, Inserted] = Functions.try_emplace(Selector);
        if (!Inserted) {
          if (FunctionIt->second.EntryPC == *Entry)
            continue;
          High.Diagnostics.push_back(
              {Operation.PC, "duplicate selector 0x" + selectorHex(Selector) +
                                 " maps to multiple entry points"});
          Functions.erase(FunctionIt);
          AmbiguousSelectors.insert(Selector);
          continue;
        }
        RecoveredFunction &Function = FunctionIt->second;
        Function.Selector = Selector;
        Function.EntryPC = *Entry;
        // A tabulated signature that hashes to this selector exhibits a
        // preimage, so the name is recovered rather than invented.
        Function.Known = findKnownFunction(Selector);
        Function.Name = Function.Known ? Function.Known->name().str()
                                       : kRecoveredFunctionPrefix.str() +
                                             selectorHex(Selector);
      }
    }
  }

  for (auto &[Selector, Function] : Functions) {
    (void)Selector;
    const std::set<uint64_t> FunctionBlocks =
        reachableFrom(Low, Function.EntryPC);
    const std::set<uint64_t> GuardReads =
        nonPayableGuardReads(Low, Index, Classifier, FunctionBlocks);
    StateAccessKind StateAccess = StateAccessKind::None;
    bool ReadsCallValue = false;
    bool ReturnsWord = false;
    for (uint64_t BlockPC : FunctionBlocks) {
      const LowBlock *LowBlock = Low.findBlock(BlockPC);
      const MedBlock *Block = Index.block(BlockPC);
      if (!LowBlock || !Block) {
        StateAccess = mergeStateAccess(StateAccess, StateAccessKind::Unknown);
        continue;
      }
      if (LowBlock->HasIndirectSuccessor)
        StateAccess = mergeStateAccess(StateAccess, StateAccessKind::Unknown);
      for (const MedOperation &Operation : Block->Operations) {
        const bool IsGuardRead = GuardReads.contains(Operation.PC);
        StateAccess =
            mergeStateAccess(StateAccess, IsGuardRead ? StateAccessKind::None
                                                      : Operation.StateAccess);
        ReadsCallValue |= !IsGuardRead && Operation.CallValueAccess ==
                                              CallValueAccessKind::Read;

        if (Operation.Op == Opcode::RETURN && Operation.Inputs.size() == 2) {
          const MedValue *Size = Med.findValue(Operation.Inputs[1]);
          ReturnsWord |= Size && Size->Constant &&
                         *Size->Constant == llvm::APInt(kWordBits, kWordBytes);
        }
      }
    }

    const ArgumentRecovery Arguments(Med, Index, FunctionBlocks);
    // A hashed signature settles the argument list, so it decides both how
    // many arguments there are and what each one is. Otherwise the head slots
    // the body read decide, and every slot below the highest is reported even
    // when nothing read it: dropping a gap would renumber the rest.
    const llvm::SmallVector<llvm::StringRef, 8> Declared =
        Function.Known ? signatureArgumentTypes(Function.Known->Signature)
                       : llvm::SmallVector<llvm::StringRef, 8>{};
    const size_t Count = Function.Known ? Declared.size() : Arguments.count();
    for (size_t Position = 0; Position < Count; ++Position) {
      RecoveredArgument Argument;
      Argument.Index = static_cast<unsigned>(Position);
      Argument.CalldataOffset = kSelectorBytes + Position * kWordBytes;
      Argument.Name = kRecoveredArgumentPrefix.str() + std::to_string(Position);
      Argument.Read = Position < Arguments.count() && Arguments.read(Position);
      if (Function.Known) {
        Argument.Type = Declared[Position].str();
        Argument.TypeSource = ABITypeSource::KnownSignature;
      } else {
        const ABIConstraint &Constraint = Arguments.constraint(Position);
        Argument.Type = Constraint.resolve().spelling();
        Argument.TypeSource = Constraint.source();
      }
      Function.Arguments.push_back(std::move(Argument));
    }

    if (Function.Known) {
      for (llvm::StringRef Type : splitTypeList(Function.Known->Returns))
        Function.Returns.push_back(Type.str());
      Function.ReturnSource = ABITypeSource::KnownSignature;
    } else if (ReturnsWord) {
      Function.Returns.push_back(kDefaultRecoveredWordType.str());
    }
    Function.StateMutability = recoveredMutability(StateAccess, ReadsCallValue);
    High.Functions.push_back(Function);
    High.Regions.push_back({Function.EntryPC,
                            RegionKind::Function,
                            {FunctionBlocks.begin(), FunctionBlocks.end()}});
  }

  for (const MedBlock &Block : Med.Blocks) {
    for (const MedOperation &Operation : Block.Operations) {
      const LowInstruction *Instruction = instructionAt(Low, Operation.PC);
      if (!Instruction || !Instruction->isExecutable())
        continue;
      if (Operation.Op == Opcode::SLOAD || Operation.Op == Opcode::SSTORE ||
          Operation.Op == Opcode::TLOAD || Operation.Op == Opcode::TSTORE) {
        StorageFact Fact;
        Fact.PC = Operation.PC;
        Fact.IsWrite =
            Operation.Op == Opcode::SSTORE || Operation.Op == Opcode::TSTORE;
        Fact.IsTransient =
            Operation.Op == Opcode::TLOAD || Operation.Op == Opcode::TSTORE;
        if (Index.valid() && !Operation.Inputs.empty()) {
          Fact.KeyKind = storageKeyKind(Med, Index, Operation.Inputs[0]);
          if (const MedValue *Key = Med.findValue(Operation.Inputs[0]);
              Key && Key->Constant) {
            Fact.Slot = Key->Constant;
            Fact.Known = findKnownSlot(*Key->Constant);
          }
        }
        Fact.SuggestedName = kUnknownStorageName.str();
        // A slot a specification fixes carries its published name; a slot a
        // compiler allocated carries only its number, because nothing outside
        // the source says what it holds.
        if (Fact.Known)
          Fact.SuggestedName = Fact.Known->Name.str();
        else if (Fact.Slot)
          Fact.SuggestedName =
              kStorageSlotPrefix.str() + wordHexDigits(*Fact.Slot);
        else if (Fact.KeyKind == StorageKeyKind::Hashed ||
                 Fact.KeyKind == StorageKeyKind::HashedOffset)
          Fact.SuggestedName =
              kStorageElementPrefix.str() + llvm::utohexstr(Operation.PC);
        High.Storage.push_back(std::move(Fact));
      }
      if (Index.valid())
        if (const CallFamilyInfo *Family = findCallFamily(Operation.Op)) {
          High.Calls.push_back(
              classifyCall(Med, Index, Block, *Family, Operation, Low.Fork));
          // A delegating call is also an outgoing call, but it is the only one
          // whose callee runs against this program's own storage, so it stays
          // reported on its own.
          if (Family->Delegates)
            High.Proxies.push_back(classifyDelegation(Med, Index, Operation));
        }
      if (evm::isLog(Operation.Op)) {
        EventFact Fact;
        Fact.PC = Operation.PC;
        Fact.Topics = logTopicCount(Operation.Op);
        if (Index.valid() && Fact.Topics != 0 && Operation.Inputs.size() > 2)
          if (const MedValue *Topic = Med.findValue(Operation.Inputs[2]);
              Topic && Topic->Constant) {
            Fact.Topic0 = Topic->Constant;
            Fact.Known = findKnownEvent(*Topic->Constant);
          }
        Fact.SuggestedName = Fact.Known ? Fact.Known->name().str()
                                        : kRecoveredEventPrefix.str() +
                                              llvm::utohexstr(Operation.PC);
        High.Events.push_back(std::move(Fact));
      }
    }
  }

  for (const LowInstruction &Instruction : Low.Instructions) {
    if (!Instruction.is(Opcode::REVERT))
      continue;
    const MedBlock *Block = Index.containingBlock(Instruction.PC);
    const MedOperation *Revert = Index.operation(Instruction.PC);
    if (Index.valid() && Block && Revert) {
      High.Errors.push_back(classifyRevert(Med, *Block, *Revert));
      continue;
    }
    // Without a usable value graph the site is still worth reporting; what it
    // hands back is not.
    ErrorFact Fact;
    Fact.PC = Instruction.PC;
    Fact.SuggestedName = kRecoveredRevertName.str();
    High.Errors.push_back(std::move(Fact));
  }

  if (Index.valid()) {
    for (const MedBlock &Block : Med.Blocks) {
      const LowBlock *LowBlock = Low.findBlock(Block.StartPC);
      if (!LowBlock || !LowBlock->Reachable)
        continue;
      for (const MedOperation &Operation : Block.Operations) {
        if (Operation.Op != Opcode::JUMPI || Operation.Inputs.size() != 2)
          continue;
        const SemanticValue Condition =
            Classifier.classify(Operation.Inputs[1]);
        const auto Destination = jumpDestination(Med, Operation);
        if (Condition.Kind == SemanticKind::IsZeroCalldataSize && Destination &&
            hasConcreteTrueEdge(Low, Block.StartPC, *Destination))
          High.HasReceive = true;
      }
    }
  }

  // Report the standards in table order rather than in the order the program
  // happens to mention them, so two builds of one contract summarize alike.
  std::vector<bool> Matched(knownStandardInfos().size(), false);
  const auto Note = [&](const KnownSignatureInfo *Known) {
    if (Known)
      Matched[static_cast<size_t>(Known->Standard)] = true;
  };
  for (const RecoveredFunction &Function : High.Functions)
    Note(Function.Known);
  for (const EventFact &Event : High.Events)
    Note(Event.Known);
  for (const ErrorFact &Error : High.Errors)
    Note(Error.Known);
  // A named slot says which specification the contract speaks just as plainly
  // as a matched selector, and it keeps saying so for a proxy whose whole ABI
  // belongs to the implementation behind it.
  for (const StorageFact &Storage : High.Storage)
    if (Storage.Known)
      Matched[static_cast<size_t>(Storage.Known->Standard)] = true;
  for (const ProxyFact &Proxy : High.Proxies)
    if (Proxy.Known)
      Matched[static_cast<size_t>(Proxy.Known->Standard)] = true;
  for (const KnownStandardInfo &Standard : knownStandardInfos())
    if (Matched[static_cast<size_t>(Standard.ID)])
      High.Standards.push_back(Standard.ID);

  if (Index.valid())
    High.HasFallback = reachesFallback(Low, Index, Classifier);
  if (High.Regions.empty()) {
    StructuredRegion Root;
    Root.EntryPC = kEntryPC;
    Root.Kind = RegionKind::CFG;
    for (const LowBlock &Block : Low.Blocks)
      Root.Blocks.push_back(Block.StartPC);
    High.Regions.push_back(std::move(Root));
  }
  return High;
}

} // namespace neverd::evm
