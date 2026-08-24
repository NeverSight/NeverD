//===- EVMMemoryDataflow.cpp - EVM memory reaching definitions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMMemoryDataflow.h"

#include "EVMHighAnalysisDetail.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd::evm::detail {
namespace {

using MemoryByteState = std::map<uint64_t, uint8_t>;
using EntryPhiOwners = std::map<ValueID, std::set<MedStateLaneID>>;
using LanePredecessors = std::map<MedStateLaneID, std::vector<MedStateLaneID>>;

/// Entry and exit are retained independently for each reachable lane.
inline constexpr size_t kMemoryStatesPerLane = 2;

struct ReadRequest {
  uint64_t PC = 0;
  uint64_t Address = 0;
  size_t Size = 0;

  friend bool operator<(const ReadRequest &Left, const ReadRequest &Right) {
    if (Left.PC != Right.PC)
      return Left.PC < Right.PC;
    if (Left.Address != Right.Address)
      return Left.Address < Right.Address;
    return Left.Size < Right.Size;
  }
};

llvm::Error limitError(llvm::StringRef Name, size_t Limit, uint64_t PC) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine(kHighIRAnalysisDiagnosticPrefix) + Name +
       kAnalysisLimitSuffix + llvm::Twine(Limit) +
       kAnalysisLimitExceededSuffix + kAnalysisAtPCInfix + llvm::utohexstr(PC))
          .str(),
      llvm::inconvertibleErrorCode());
}

class MemoryAnalysisBudget {
public:
  explicit MemoryAnalysisBudget(const AnalyzeOptions &Options)
      : Options(Options) {}

  llvm::Error noteTransferCells(size_t Amount, uint64_t PC) {
    return charge(kMaxHighMemoryTransferCellsName,
                  Options.MaxHighMemoryTransferCells, TransferCells, Amount,
                  PC);
  }

  llvm::Error noteValueVisits(size_t Amount, uint64_t PC) {
    return charge(kMaxHighMemoryValueVisitsName,
                  Options.MaxHighMemoryValueVisits, ValueVisits, Amount, PC);
  }

private:
  static llvm::Error charge(llvm::StringRef Name, size_t Limit, size_t &Used,
                            size_t Amount, uint64_t PC) {
    if (Amount > Limit - std::min(Used, Limit))
      return limitError(Name, Limit, PC);
    Used += Amount;
    return llvm::Error::success();
  }

  const AnalyzeOptions &Options;
  size_t TransferCells = 0;
  size_t ValueVisits = 0;
};

class LaneConstantResolver {
public:
  LaneConstantResolver(const EVMMedIR &Med,
                       const LanePredecessors &Predecessors,
                       const EntryPhiOwners &EntryPhis,
                       MemoryAnalysisBudget &Budget)
      : Med(Med), Predecessors(Predecessors), EntryPhis(EntryPhis),
        Budget(Budget) {}

  llvm::Expected<std::optional<llvm::APInt>> resolve(ValueID ID,
                                                     MedStateLaneID Lane) {
    const Key Root{Lane, ID};
    if (llvm::Error Error = noteValue(Root))
      return std::move(Error);
    const auto Cached = Cache.find(Root);
    if (Cached != Cache.end() && Cached->second.State == MemoState::Complete)
      return Cached->second.Value;

    std::vector<Frame> Stack;
    auto RootFrame = makeFrame(Root);
    if (!RootFrame)
      return RootFrame.takeError();
    Stack.push_back(std::move(*RootFrame));
    while (!Stack.empty()) {
      Frame &Current = Stack.back();
      if (Current.NextDependency < Current.Dependencies.size()) {
        const Key Dependency = Current.Dependencies[Current.NextDependency++];
        if (llvm::Error Error = noteValue(Dependency))
          return std::move(Error);
        const auto DependencyIt = Cache.find(Dependency);
        if (DependencyIt == Cache.end()) {
          auto DependencyFrame = makeFrame(Dependency);
          if (!DependencyFrame)
            return DependencyFrame.takeError();
          Stack.push_back(std::move(*DependencyFrame));
          continue;
        }
        if (DependencyIt->second.State == MemoState::Active) {
          Current.Invalid = true;
          continue;
        }
        const std::optional<llvm::APInt> &Candidate =
            DependencyIt->second.Value;
        if (!Candidate || (Current.Common && *Current.Common != *Candidate)) {
          Current.Invalid = true;
          continue;
        }
        Current.Common = *Candidate;
        Current.SawValue = true;
        continue;
      }

      MemoEntry &Entry = Cache.find(Current.Node)->second;
      Entry.State = MemoState::Complete;
      Entry.Value = !Current.Invalid && Current.SawValue
                        ? Current.Common
                        : std::optional<llvm::APInt>{};
      Stack.pop_back();
    }
    return Cache.find(Root)->second.Value;
  }

private:
  using Key = std::pair<MedStateLaneID, ValueID>;

  enum class MemoState : uint8_t { Active, Complete };

  struct MemoEntry {
    MemoState State = MemoState::Active;
    std::optional<llvm::APInt> Value;
  };

  struct Frame {
    Key Node;
    std::vector<Key> Dependencies;
    size_t NextDependency = 0;
    std::optional<llvm::APInt> Common;
    bool SawValue = false;
    bool Invalid = false;
  };

  uint64_t pcFor(const Key &Node) const {
    const MedValue *Value = Med.findValue(Node.second);
    return Value ? Value->PC : kEntryPC;
  }

  llvm::Error noteValue(const Key &Node) {
    return Budget.noteValueVisits(1, pcFor(Node));
  }

  llvm::Expected<Frame> makeFrame(const Key &Node) {
    Cache.try_emplace(Node);
    Frame Result;
    Result.Node = Node;
    const MedValue *Value = Med.findValue(Node.second);
    if (!Value) {
      Result.Invalid = true;
      return Result;
    }
    if (Value->Constant) {
      Result.Common = Value->Constant;
      Result.SawValue = true;
      return Result;
    }
    if (Value->Kind != ValueKind::Phi) {
      Result.Invalid = true;
      return Result;
    }

    const auto Owners = EntryPhis.find(Value->ID);
    bool IsEntryPhi = false;
    if (Owners != EntryPhis.end()) {
      if (llvm::Error Error = Budget.noteValueVisits(1, Value->PC))
        return std::move(Error);
      IsEntryPhi = Owners->second.contains(Node.first);
    }
    if (!IsEntryPhi) {
      for (const MedPhiIncoming &Incoming : Value->PhiIncomings) {
        if (llvm::Error Error = Budget.noteValueVisits(1, Value->PC))
          return std::move(Error);
        if (Incoming.SourceLane < Node.first)
          continue;
        if (Incoming.SourceLane == Node.first)
          Result.Dependencies.emplace_back(Node.first, Incoming.Value);
        break;
      }
      Result.Invalid = Result.Dependencies.empty();
      return Result;
    }

    const auto PredecessorIt = Predecessors.find(Node.first);
    if (PredecessorIt == Predecessors.end() || PredecessorIt->second.empty()) {
      Result.Invalid = true;
      return Result;
    }
    size_t IncomingIndex = 0;
    for (MedStateLaneID Predecessor : PredecessorIt->second) {
      if (llvm::Error Error = Budget.noteValueVisits(1, Value->PC))
        return std::move(Error);
      while (IncomingIndex < Value->PhiIncomings.size()) {
        if (llvm::Error Error = Budget.noteValueVisits(1, Value->PC))
          return std::move(Error);
        const MedPhiIncoming &Incoming = Value->PhiIncomings[IncomingIndex];
        if (Incoming.SourceLane < Predecessor) {
          ++IncomingIndex;
          continue;
        }
        if (Incoming.SourceLane != Predecessor) {
          Result.Invalid = true;
          return Result;
        }
        Result.Dependencies.emplace_back(Predecessor, Incoming.Value);
        ++IncomingIndex;
        break;
      }
      if (Result.Dependencies.empty() ||
          Result.Dependencies.back().first != Predecessor) {
        Result.Invalid = true;
        return Result;
      }
    }
    return Result;
  }

  const EVMMedIR &Med;
  const LanePredecessors &Predecessors;
  const EntryPhiOwners &EntryPhis;
  MemoryAnalysisBudget &Budget;
  std::map<Key, MemoEntry> Cache;
};

llvm::Expected<std::optional<uint64_t>>
constantAddressForLane(LaneConstantResolver &Resolver, ValueID ID,
                       MedStateLaneID Lane) {
  auto Value = Resolver.resolve(ID, Lane);
  if (!Value)
    return Value.takeError();
  if (!*Value ||
      (*Value)->getActiveBits() > std::numeric_limits<uint64_t>::digits)
    return std::optional<uint64_t>{};
  return std::optional<uint64_t>((*Value)->getZExtValue());
}

std::optional<uint64_t> constantAddress(const EVMMedIR &Med, ValueID ID) {
  const MedValue *Value = Med.findValue(ID);
  if (!Value || !Value->Constant ||
      Value->Constant->getActiveBits() > std::numeric_limits<uint64_t>::digits)
    return std::nullopt;
  return Value->Constant->getZExtValue();
}

bool constantAtLeast(const EVMMedIR &Med, ValueID ID, uint64_t Minimum) {
  const MedValue *Value = Med.findValue(ID);
  return Value && Value->Constant &&
         Value->Constant->uge(llvm::APInt(kWordBits, Minimum));
}

bool rangeFits(uint64_t Address, size_t Size) {
  return Size == 0 ||
         Size - 1 <= std::numeric_limits<uint64_t>::max() - Address;
}

llvm::Error
addRequest(std::set<ReadRequest> &Requests,
           std::set<uint64_t> &TrackedAddresses, const AnalyzeOptions &Options,
           uint64_t PC, uint64_t Address, size_t Size,
           MemoryAnalysisBudget &Budget,
           llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  if (Size == 0 || !rangeFits(Address, Size))
    return llvm::Error::success();
  if (Size > Options.MaxHighTrackedMemoryBytes)
    return limitError(kMaxHighTrackedMemoryBytesName,
                      Options.MaxHighTrackedMemoryBytes, PC);
  const ReadRequest Request{PC, Address, Size};
  if (llvm::Error Error = NoteReferenceVisit(PC))
    return Error;
  if (!Requests.contains(Request)) {
    if (Requests.size() >= Options.MaxHighMemoryReadRequests)
      return limitError(kMaxHighMemoryReadRequestsName,
                        Options.MaxHighMemoryReadRequests, PC);
    if (llvm::Error Error = NoteReferenceVisit(PC))
      return Error;
    Requests.insert(Request);
  }

  size_t NewAddresses = 0;
  for (size_t I = 0; I < Size; ++I) {
    if (llvm::Error Error = Budget.noteTransferCells(1, PC))
      return Error;
    if (llvm::Error Error = NoteReferenceVisit(PC))
      return Error;
    NewAddresses += !TrackedAddresses.contains(Address + I);
  }
  const size_t Remaining =
      TrackedAddresses.size() >= Options.MaxHighTrackedMemoryBytes
          ? 0
          : Options.MaxHighTrackedMemoryBytes - TrackedAddresses.size();
  if (NewAddresses > Remaining)
    return limitError(kMaxHighTrackedMemoryBytesName,
                      Options.MaxHighTrackedMemoryBytes, PC);
  for (size_t I = 0; I < Size; ++I) {
    if (llvm::Error Error = Budget.noteTransferCells(1, PC))
      return Error;
    if (llvm::Error Error = NoteReferenceVisit(PC))
      return Error;
    TrackedAddresses.insert(Address + I);
  }
  return llvm::Error::success();
}

llvm::Error
collectRequests(const EVMMedIR &Med, const DefiniteExecutionIndex &Execution,
                const AnalyzeOptions &Options, std::set<ReadRequest> &Requests,
                std::set<uint64_t> &TrackedAddresses,
                std::map<uint64_t, const MedOperation *> &Operations,
                MemoryAnalysisBudget &Budget,
                llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
                llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  constexpr uint64_t kPanicPayloadBytes = kSelectorBytes + kWordBytes;
  for (const MedBlock &Block : Med.Blocks) {
    for (const MedOperation &Operation : Block.Operations) {
      if (llvm::Error Error = NoteOperationVisit(Operation.PC))
        return Error;
      Operations.emplace(Operation.PC, &Operation);
      auto Eligible = Execution.isEligible(Operation, NoteReferenceVisit);
      if (!Eligible)
        return Eligible.takeError();
      if (!*Eligible)
        continue;
      if (Operation.Op == Opcode::REVERT && Operation.Inputs.size() == 2) {
        if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
          return Error;
        if (llvm::Error Error = Budget.noteValueVisits(1, Operation.PC))
          return Error;
        const auto Address = constantAddress(Med, Operation.Inputs[0]);
        if (!Address)
          continue;
        if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
          return Error;
        if (llvm::Error Error = Budget.noteValueVisits(1, Operation.PC))
          return Error;
        const bool HasSelector =
            constantAtLeast(Med, Operation.Inputs[1], kSelectorBytes);
        if (HasSelector)
          if (llvm::Error Error = addRequest(
                  Requests, TrackedAddresses, Options, Operation.PC, *Address,
                  kSelectorBytes, Budget, NoteReferenceVisit))
            return Error;
        if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
          return Error;
        if (llvm::Error Error = Budget.noteValueVisits(1, Operation.PC))
          return Error;
        if (constantAtLeast(Med, Operation.Inputs[1], kPanicPayloadBytes) &&
            *Address <= std::numeric_limits<uint64_t>::max() - kSelectorBytes)
          if (llvm::Error Error =
                  addRequest(Requests, TrackedAddresses, Options, Operation.PC,
                             *Address + kSelectorBytes, kWordBytes, Budget,
                             NoteReferenceVisit))
            return Error;
        continue;
      }

      const CallFamilyInfo *Family = findCallFamily(Operation.Op);
      if (!Family ||
          Operation.Inputs.size() <= Family->argumentsLengthOperand())
        continue;
      if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
        return Error;
      if (llvm::Error Error = Budget.noteValueVisits(1, Operation.PC))
        return Error;
      const auto Address = constantAddress(
          Med, Operation.Inputs[Family->argumentsOffsetOperand()]);
      if (!Address)
        continue;
      if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
        return Error;
      if (llvm::Error Error = Budget.noteValueVisits(1, Operation.PC))
        return Error;
      if (!constantAtLeast(Med,
                           Operation.Inputs[Family->argumentsLengthOperand()],
                           kSelectorBytes))
        continue;
      if (llvm::Error Error =
              addRequest(Requests, TrackedAddresses, Options, Operation.PC,
                         *Address, kSelectorBytes, Budget, NoteReferenceVisit))
        return Error;
    }
  }
  return llvm::Error::success();
}

bool productExceeds(size_t Left, size_t Right, size_t Limit) {
  return Left != 0 && Right > Limit / Left;
}

llvm::Expected<MemoryByteState> copyState(const MemoryByteState &Source,
                                          MemoryAnalysisBudget &Budget,
                                          uint64_t PC) {
  if (llvm::Error Error = Budget.noteTransferCells(Source.size(), PC))
    return std::move(Error);
  return Source;
}

llvm::Error assignState(std::optional<MemoryByteState> &Destination,
                        const MemoryByteState &Source,
                        MemoryAnalysisBudget &Budget, uint64_t PC) {
  if (Destination)
    if (llvm::Error Error = Budget.noteTransferCells(Destination->size(), PC))
      return Error;
  if (llvm::Error Error = Budget.noteTransferCells(Source.size(), PC))
    return Error;
  Destination = Source;
  return llvm::Error::success();
}

llvm::Error moveState(std::optional<MemoryByteState> &Destination,
                      MemoryByteState Source, MemoryAnalysisBudget &Budget,
                      uint64_t PC) {
  if (Destination)
    if (llvm::Error Error = Budget.noteTransferCells(Destination->size(), PC))
      return Error;
  Destination = std::move(Source);
  return llvm::Error::success();
}

llvm::Expected<bool> sameState(const MemoryByteState &Left,
                               const MemoryByteState &Right,
                               MemoryAnalysisBudget &Budget, uint64_t PC) {
  if (Left.size() != Right.size())
    return false;
  auto LeftIt = Left.begin();
  auto RightIt = Right.begin();
  for (; LeftIt != Left.end(); ++LeftIt, ++RightIt) {
    if (llvm::Error Error = Budget.noteTransferCells(1, PC))
      return std::move(Error);
    if (*LeftIt != *RightIt)
      return false;
  }
  return true;
}

llvm::Expected<MemoryByteState> meet(MemoryByteState Result,
                                     const MemoryByteState &Other,
                                     MemoryAnalysisBudget &Budget,
                                     uint64_t PC) {
  for (auto It = Result.begin(); It != Result.end();) {
    if (llvm::Error Error = Budget.noteTransferCells(1, PC))
      return std::move(Error);
    const auto OtherIt = Other.find(It->first);
    if (OtherIt == Other.end() || OtherIt->second != It->second)
      It = Result.erase(It);
    else
      ++It;
  }
  return Result;
}

llvm::Error clearState(MemoryByteState &State, MemoryAnalysisBudget &Budget,
                       uint64_t PC) {
  if (llvm::Error Error = Budget.noteTransferCells(State.size(), PC))
    return Error;
  State.clear();
  return llvm::Error::success();
}

llvm::Error forgetRange(MemoryByteState &State,
                        const std::set<uint64_t> &TrackedAddresses,
                        uint64_t Address, size_t Size,
                        MemoryAnalysisBudget &Budget, uint64_t PC) {
  if (!rangeFits(Address, Size)) {
    return clearState(State, Budget, PC);
  }
  for (size_t I = 0; I < Size; ++I)
    if (TrackedAddresses.contains(Address + I)) {
      if (llvm::Error Error = Budget.noteTransferCells(1, PC))
        return Error;
      State.erase(Address + I);
    }
  return llvm::Error::success();
}

llvm::Error writeWord(MemoryByteState &State,
                      const std::set<uint64_t> &TrackedAddresses,
                      uint64_t Address, const llvm::APInt &Value,
                      MemoryAnalysisBudget &Budget, uint64_t PC) {
  const llvm::APInt Word = Value.zextOrTrunc(kWordBits);
  for (size_t I = 0; I < kWordBytes; ++I) {
    const uint64_t ByteAddress = Address + I;
    if (!TrackedAddresses.contains(ByteAddress))
      continue;
    if (llvm::Error Error = Budget.noteTransferCells(1, PC))
      return Error;
    const unsigned Bit =
        static_cast<unsigned>((kWordBytes - I - 1) * kBitsPerByte);
    State[ByteAddress] =
        static_cast<uint8_t>(Word.extractBitsAsZExtValue(kBitsPerByte, Bit));
  }
  return llvm::Error::success();
}

llvm::Error applyMemoryWrite(MemoryByteState &State,
                             const std::set<uint64_t> &TrackedAddresses,
                             LaneConstantResolver &Resolver,
                             MemoryAnalysisBudget &Budget,
                             const MedOperation &Operation,
                             MedStateLaneID Lane) {
  if (Operation.Op == Opcode::MSTORE) {
    if (Operation.Inputs.size() != 2)
      return clearState(State, Budget, Operation.PC);
    auto Address = constantAddressForLane(Resolver, Operation.Inputs[0], Lane);
    if (!Address)
      return Address.takeError();
    if (!*Address || !rangeFits(**Address, kWordBytes))
      return clearState(State, Budget, Operation.PC);
    auto Value = Resolver.resolve(Operation.Inputs[1], Lane);
    if (!Value)
      return Value.takeError();
    if (!*Value)
      return forgetRange(State, TrackedAddresses, **Address, kWordBytes, Budget,
                         Operation.PC);
    return writeWord(State, TrackedAddresses, **Address, **Value, Budget,
                     Operation.PC);
  }

  if (Operation.Op == Opcode::MSTORE8) {
    if (Operation.Inputs.size() != 2)
      return clearState(State, Budget, Operation.PC);
    auto Address = constantAddressForLane(Resolver, Operation.Inputs[0], Lane);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      return clearState(State, Budget, Operation.PC);
    auto Value = Resolver.resolve(Operation.Inputs[1], Lane);
    if (!Value)
      return Value.takeError();
    if (TrackedAddresses.contains(**Address)) {
      if (llvm::Error Error = Budget.noteTransferCells(1, Operation.PC))
        return Error;
      if (!*Value) {
        State.erase(**Address);
        return llvm::Error::success();
      }
      State[**Address] = static_cast<uint8_t>(
          (*Value)->extractBitsAsZExtValue(kBitsPerByte, 0));
    }
    return llvm::Error::success();
  }

  if (Operation.MemoryAccess == MemoryAccessKind::Write ||
      Operation.MemoryAccess == MemoryAccessKind::ReadWrite)
    return clearState(State, Budget, Operation.PC);
  return llvm::Error::success();
}

llvm::Expected<std::optional<llvm::APInt>>
readBytes(const MemoryByteState &State, uint64_t Address, size_t Size,
          MemoryAnalysisBudget &Budget, uint64_t PC) {
  if (Size == 0 || !rangeFits(Address, Size) ||
      Size > std::numeric_limits<unsigned>::max() / kBitsPerByte)
    return std::optional<llvm::APInt>{};
  const unsigned Width = static_cast<unsigned>(Size * kBitsPerByte);
  llvm::APInt Result(Width, 0);
  for (size_t I = 0; I < Size; ++I) {
    if (llvm::Error Error = Budget.noteTransferCells(1, PC))
      return std::move(Error);
    const auto It = State.find(Address + I);
    if (It == State.end())
      return std::optional<llvm::APInt>{};
    Result = Result.shl(kBitsPerByte);
    Result |= llvm::APInt(Width, It->second);
  }
  return std::optional<llvm::APInt>(std::move(Result));
}

} // namespace

llvm::Expected<EVMMemoryDataflow> EVMMemoryDataflow::analyze(
    const EVMLowIR &Low, const EVMMedIR &Med,
    const DefiniteExecutionIndex &Execution, const AnalyzeOptions &Options,
    llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  (void)Low;
  EVMMemoryDataflow Result;
  MemoryAnalysisBudget Budget(Options);
  std::set<ReadRequest> Requests;
  std::set<uint64_t> TrackedAddresses;
  std::map<uint64_t, const MedOperation *> Operations;
  if (llvm::Error Error = collectRequests(
          Med, Execution, Options, Requests, TrackedAddresses, Operations,
          Budget, NoteOperationVisit, NoteReferenceVisit))
    return std::move(Error);
  if (Requests.empty())
    return Result;

  std::vector<MedStateLaneID> ReachableLanes;
  EntryPhiOwners EntryPhis;
  for (const MedStateLane &Lane : Med.StateLanes)
    if (Lane.Evidence == Reachability::Reachable) {
      ReachableLanes.push_back(Lane.ID);
      for (ValueID Value : Lane.EntryStack) {
        if (llvm::Error Error = Budget.noteValueVisits(1, Lane.LowLane.BlockPC))
          return std::move(Error);
        if (const MedValue *Entry = Med.findValue(Value);
            Entry && Entry->Kind == ValueKind::Phi)
          EntryPhis[Value].insert(Lane.ID);
      }
    }
  if (productExceeds(ReachableLanes.size(), TrackedAddresses.size(),
                     Options.MaxHighMemoryStateCells))
    return limitError(kMaxHighMemoryStateCellsName,
                      Options.MaxHighMemoryStateCells, kEntryPC);
  const size_t CellsPerState = ReachableLanes.size() * TrackedAddresses.size();
  if (productExceeds(CellsPerState, kMemoryStatesPerLane,
                     Options.MaxHighMemoryStateCells))
    return limitError(kMaxHighMemoryStateCellsName,
                      Options.MaxHighMemoryStateCells, kEntryPC);

  std::map<uint64_t, const MedBlock *> Blocks;
  for (const MedBlock &Block : Med.Blocks) {
    Blocks.emplace(Block.StartPC, &Block);
  }

  LanePredecessors Predecessors;
  for (MedStateLaneID Source : ReachableLanes) {
    auto Successors = Execution.successors(Source, std::nullopt, std::nullopt,
                                           NoteReferenceVisit);
    if (!Successors)
      return Successors.takeError();
    for (MedStateLaneID Target : *Successors) {
      Predecessors[Target].push_back(Source);
    }
  }
  for (auto &[Target, Sources] : Predecessors) {
    (void)Target;
    llvm::sort(Sources);
    Sources.erase(std::unique(Sources.begin(), Sources.end()), Sources.end());
  }

  MemoryByteState Initial;
  for (uint64_t Address : TrackedAddresses) {
    if (llvm::Error Error = Budget.noteTransferCells(1, kEntryPC))
      return std::move(Error);
    Initial.emplace(Address, 0);
  }
  LaneConstantResolver Resolver(Med, Predecessors, EntryPhis, Budget);

  std::vector<std::optional<MemoryByteState>> Entries(Med.StateLanes.size());
  std::vector<std::optional<MemoryByteState>> Exits(Med.StateLanes.size());
  std::deque<MedStateLaneID> Worklist;
  std::vector<bool> Queued(Med.StateLanes.size(), false);
  const auto Enqueue = [&](MedStateLaneID Lane) {
    const size_t Index = medStateLaneIndex(Lane);
    if (Index >= Queued.size() || Queued[Index] || !Execution.isReachable(Lane))
      return;
    Queued[Index] = true;
    Worklist.push_back(Lane);
  };
  const auto RootLane = Execution.medLane({kEntryPC, kEntryStateLaneOrdinal});
  if (RootLane && Execution.isReachable(*RootLane))
    Enqueue(*RootLane);

  size_t WorklistTransfers = 0;
  bool Bootstrap = true;
  while (Bootstrap || !Worklist.empty()) {
    if (Worklist.empty()) {
      Bootstrap = false;
      // The bootstrap walk creates provisional exits so cyclic predecessors
      // are represented. The strict pass below computes entries only after
      // every exact predecessor has an exit; no read fact is materialized
      // until that pass reaches its fixed point.
      for (auto &Entry : Entries) {
        if (Entry)
          if (llvm::Error Error = clearState(*Entry, Budget, kEntryPC))
            return std::move(Error);
        Entry.reset();
      }
      for (MedStateLaneID Lane : ReachableLanes)
        Enqueue(Lane);
      continue;
    }
    const MedStateLaneID Lane = Worklist.front();
    const MedStateLane *StateLane = Execution.lane(Lane);
    const uint64_t LanePC = StateLane ? StateLane->LowLane.BlockPC : kEntryPC;
    if (WorklistTransfers >= Options.MaxHighMemoryWorklistUpdates)
      return limitError(kMaxHighMemoryWorklistUpdatesName,
                        Options.MaxHighMemoryWorklistUpdates, LanePC);
    ++WorklistTransfers;
    if (llvm::Error Error = NoteLaneVisit(LanePC))
      return std::move(Error);
    Worklist.pop_front();
    const size_t LaneIndex = medStateLaneIndex(Lane);
    Queued[LaneIndex] = false;

    std::optional<MemoryByteState> Entry;
    const bool IsRoot = RootLane && Lane == *RootLane;
    if (IsRoot) {
      auto RootEntry = copyState(Initial, Budget, LanePC);
      if (!RootEntry)
        return RootEntry.takeError();
      Entry = std::move(*RootEntry);
    }
    const auto PredIt = Predecessors.find(Lane);
    bool HasUnavailablePredecessor = false;
    if (PredIt != Predecessors.end())
      for (MedStateLaneID Predecessor : PredIt->second) {
        const auto &Exit = Exits[medStateLaneIndex(Predecessor)];
        if (!Exit) {
          HasUnavailablePredecessor = true;
          continue;
        }
        if (Entry) {
          auto Joined = meet(std::move(*Entry), *Exit, Budget, LanePC);
          if (!Joined)
            return Joined.takeError();
          Entry = std::move(*Joined);
        } else {
          auto First = copyState(*Exit, Budget, LanePC);
          if (!First)
            return First.takeError();
          Entry = std::move(*First);
        }
      }
    if (!Bootstrap && !IsRoot &&
        (PredIt == Predecessors.end() || HasUnavailablePredecessor))
      continue;
    if (!Entry)
      continue;
    if (Entries[LaneIndex]) {
      auto Unchanged = sameState(*Entries[LaneIndex], *Entry, Budget, LanePC);
      if (!Unchanged)
        return Unchanged.takeError();
      if (*Unchanged)
        continue;
    }
    if (llvm::Error Error =
            assignState(Entries[LaneIndex], *Entry, Budget, LanePC))
      return std::move(Error);

    MemoryByteState Exit = std::move(*Entry);
    const auto BlockIt =
        StateLane ? Blocks.find(StateLane->LowLane.BlockPC) : Blocks.end();
    if (BlockIt != Blocks.end())
      for (const MedOperation &Operation : BlockIt->second->Operations) {
        if (llvm::Error Error = NoteOperationVisit(Operation.PC))
          return std::move(Error);
        auto Faults = Execution.faultsIn(Operation, Lane, NoteReferenceVisit);
        if (!Faults)
          return Faults.takeError();
        if (*Faults)
          break;
        auto Executes =
            Execution.executesIn(Operation, Lane, NoteReferenceVisit);
        if (!Executes)
          return Executes.takeError();
        if (*Executes)
          if (llvm::Error Error = applyMemoryWrite(
                  Exit, TrackedAddresses, Resolver, Budget, Operation, Lane))
            return std::move(Error);
      }
    if (Exits[LaneIndex]) {
      auto Unchanged = sameState(*Exits[LaneIndex], Exit, Budget, LanePC);
      if (!Unchanged)
        return Unchanged.takeError();
      if (*Unchanged)
        continue;
    }
    if (llvm::Error Error =
            moveState(Exits[LaneIndex], std::move(Exit), Budget, LanePC))
      return std::move(Error);
    auto Successors = Execution.successors(Lane, std::nullopt, std::nullopt,
                                           NoteReferenceVisit);
    if (!Successors)
      return Successors.takeError();
    for (MedStateLaneID Successor : *Successors) {
      Enqueue(Successor);
    }
  }

  std::map<uint64_t, std::vector<ReadRequest>> RequestsByPC;
  for (const ReadRequest &Request : Requests)
    RequestsByPC[Request.PC].push_back(Request);
  struct Consensus {
    size_t Expected = 0;
    size_t Observed = 0;
    bool Conflict = false;
    std::optional<llvm::APInt> Value;
  };
  std::map<ReadRequest, Consensus> ConsensusByRequest;
  for (const ReadRequest &Request : Requests) {
    const auto OperationIt = Operations.find(Request.PC);
    if (OperationIt == Operations.end())
      continue;
    Consensus &ReadConsensus = ConsensusByRequest[Request];
    for (MedStateLaneID Lane : OperationIt->second->ExecutingLanes) {
      if (llvm::Error Error = NoteReferenceVisit(Request.PC))
        return std::move(Error);
      if (llvm::Error Error = NoteOperationVisit(Request.PC))
        return std::move(Error);
      ReadConsensus.Expected += Execution.isReachable(Lane);
    }
  }

  for (MedStateLaneID Lane : ReachableLanes) {
    const MedStateLane *StateLane = Execution.lane(Lane);
    const uint64_t LanePC = StateLane ? StateLane->LowLane.BlockPC : kEntryPC;
    if (llvm::Error Error = NoteLaneVisit(LanePC))
      return std::move(Error);
    const size_t LaneIndex = medStateLaneIndex(Lane);
    if (!Entries[LaneIndex])
      continue;
    const auto BlockIt =
        StateLane ? Blocks.find(StateLane->LowLane.BlockPC) : Blocks.end();
    if (BlockIt == Blocks.end())
      continue;
    auto StateCopy = copyState(*Entries[LaneIndex], Budget, LanePC);
    if (!StateCopy)
      return StateCopy.takeError();
    MemoryByteState State = std::move(*StateCopy);
    for (const MedOperation &Operation : BlockIt->second->Operations) {
      if (llvm::Error Error = NoteOperationVisit(Operation.PC))
        return std::move(Error);
      auto Faults = Execution.faultsIn(Operation, Lane, NoteReferenceVisit);
      if (!Faults)
        return Faults.takeError();
      if (*Faults)
        break;
      auto Executes = Execution.executesIn(Operation, Lane, NoteReferenceVisit);
      if (!Executes)
        return Executes.takeError();
      if (!*Executes)
        continue;
      const auto Requested = RequestsByPC.find(Operation.PC);
      if (Requested != RequestsByPC.end())
        for (const ReadRequest &Request : Requested->second) {
          if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
            return std::move(Error);
          Consensus &ReadConsensus = ConsensusByRequest[Request];
          ++ReadConsensus.Observed;
          auto Value = readBytes(State, Request.Address, Request.Size, Budget,
                                 Operation.PC);
          if (!Value)
            return Value.takeError();
          if (!*Value ||
              (ReadConsensus.Value && *ReadConsensus.Value != **Value)) {
            ReadConsensus.Conflict = true;
          } else if (!ReadConsensus.Value) {
            ReadConsensus.Value = **Value;
          }
        }
      if (llvm::Error Error = applyMemoryWrite(
              State, TrackedAddresses, Resolver, Budget, Operation, Lane))
        return std::move(Error);
    }
  }

  for (auto &[Request, ReadConsensus] : ConsensusByRequest) {
    if (llvm::Error Error = NoteReferenceVisit(Request.PC))
      return std::move(Error);
    if (!ReadConsensus.Conflict && ReadConsensus.Value &&
        ReadConsensus.Expected != 0 &&
        ReadConsensus.Observed == ReadConsensus.Expected)
      Result.Reads.emplace(ReadKey{Request.PC, Request.Address, Request.Size},
                           std::move(*ReadConsensus.Value));
  }
  return Result;
}

std::optional<llvm::APInt>
EVMMemoryDataflow::read(uint64_t PC, uint64_t Address, size_t Size) const {
  const auto It = Reads.find({PC, Address, Size});
  return It == Reads.end() ? std::nullopt
                           : std::optional<llvm::APInt>(It->second);
}

} // namespace neverd::evm::detail
