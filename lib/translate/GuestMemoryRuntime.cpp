//===- GuestMemoryRuntime.cpp - Checked guest memory execution boundary --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/GuestMemoryRuntime.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::translate {
namespace {

llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

bool isKnownPolicy(CodeInvalidationPolicy Policy) {
  switch (Policy) {
  case CodeInvalidationPolicy::RejectExecutableWrites:
  case CodeInvalidationPolicy::InvalidateOnExecutableWrite:
  case CodeInvalidationPolicy::ValidateBeforeDispatch:
    return true;
  }
  return false;
}

std::optional<uint64_t> scalarBytes(GuestScalarWidth Width) {
  switch (Width) {
  case GuestScalarWidth::I8:
    return 1;
  case GuestScalarWidth::I16:
    return 2;
  case GuestScalarWidth::I32:
    return 4;
  case GuestScalarWidth::I64:
    return 8;
  }
  return std::nullopt;
}

uint64_t maximumAddress(uint16_t AddressWidth) {
  if (AddressWidth == 64)
    return std::numeric_limits<uint64_t>::max();
  return (uint64_t{1} << AddressWidth) - 1;
}

bool isPowerOfTwo(uint32_t Value) {
  return Value != 0 && (Value & (Value - 1)) == 0;
}

llvm::Expected<RuntimeMemoryAccessKindV1>
runtimeMemoryAccessKind(MemoryAccessKind Access) {
  switch (Access) {
  case MemoryAccessKind::Read:
    return RuntimeMemoryAccessKindV1::Read;
  case MemoryAccessKind::Write:
    return RuntimeMemoryAccessKindV1::Write;
  case MemoryAccessKind::Execute:
    return RuntimeMemoryAccessKindV1::Execute;
  case MemoryAccessKind::AtomicReadModifyWrite:
    return invalid("atomic guest-memory access has no runtime ABI v1 encoding");
  }
  return invalid("unknown guest-memory access kind");
}

uint64_t addSaturating(std::atomic<uint64_t> &Counter, uint64_t Delta) {
  uint64_t Current = Counter.load(std::memory_order_relaxed);
  for (;;) {
    const uint64_t Next = Delta > std::numeric_limits<uint64_t>::max() - Current
                              ? std::numeric_limits<uint64_t>::max()
                              : Current + Delta;
    if (Counter.compare_exchange_weak(Current, Next, std::memory_order_relaxed,
                                      std::memory_order_relaxed))
      return Next;
  }
}

} // namespace

llvm::Expected<std::unique_ptr<GuestMemoryRuntime>>
GuestMemoryRuntime::create(const GuestState &State,
                           const GuestMemoryRuntimeConfig &Config) {
  if (llvm::Error Error = validateGuestState(State))
    return std::move(Error);
  if (State.ByteOrder != GuestEndianness::Little)
    return invalid("checked scalar runtime requires little-endian guest state");
  if (!isKnownPolicy(Config.CodeInvalidation))
    return invalid("unknown guest-memory code-invalidation policy");
  if (Config.Limits.MaxRegions != 0 &&
      State.Memory.size() > Config.Limits.MaxRegions)
    return invalid("guest-memory region limit exceeded");

  uint64_t TotalBytes = 0;
  for (const GuestMemoryRegion &Region : State.Memory) {
    const uint64_t RegionBytes = static_cast<uint64_t>(Region.Bytes.size());
    if (RegionBytes > std::numeric_limits<uint64_t>::max() - TotalBytes)
      return invalid("guest-memory byte count overflow");
    TotalBytes += RegionBytes;
    if (Config.Limits.MaxBytes != 0 && TotalBytes > Config.Limits.MaxBytes)
      return invalid("guest-memory byte limit exceeded");
  }

  std::vector<Region> Regions;
  Regions.reserve(State.Memory.size());
  for (const GuestMemoryRegion &Source : State.Memory) {
    const uint64_t LastAddress =
        Source.Address + static_cast<uint64_t>(Source.Bytes.size() - 1);
    Regions.push_back({Source.Address, LastAddress, Source.Permissions,
                       Source.Generation, Source.Bytes});
  }
  llvm::sort(Regions, [](const Region &Left, const Region &Right) {
    return Left.Address < Right.Address;
  });

  return std::unique_ptr<GuestMemoryRuntime>(new GuestMemoryRuntime(
      std::move(Regions), State.AddressWidth, TotalBytes, Config));
}

GuestMemoryRuntime::GuestMemoryRuntime(std::vector<Region> Regions,
                                       uint16_t AddressWidth,
                                       uint64_t GuestMemoryBytes,
                                       const GuestMemoryRuntimeConfig &Config)
    : Regions(std::move(Regions)), AddressWidth(AddressWidth),
      GuestMemoryBytes(GuestMemoryBytes),
      CodeInvalidation(Config.CodeInvalidation),
      InstructionBudget(Config.InstructionBudget),
      BlockBudget(Config.BlockBudget),
      Control(makeRuntimeControlBlockV1(Config.CodeInvalidation,
                                        Config.InstructionBudget,
                                        Config.BlockBudget)) {}

std::optional<std::size_t>
GuestMemoryRuntime::findRegionIndex(uint64_t Address) const {
  const auto It = std::upper_bound(Regions.begin(), Regions.end(), Address,
                                   [](uint64_t Key, const Region &Candidate) {
                                     return Key < Candidate.Address;
                                   });
  if (It == Regions.begin())
    return std::nullopt;
  const Region &Candidate = *std::prev(It);
  if (Address > Candidate.LastAddress)
    return std::nullopt;
  return static_cast<std::size_t>(std::prev(It) - Regions.begin());
}

GuestMemoryRuntime::AccessPlan GuestMemoryRuntime::planAccess(
    uint64_t Address, uint64_t Size, MemoryPermission RequiredPermission,
    RuntimeMemoryFaultKindV1 ContinuationFault) const {
  AccessPlan Plan;
  const std::optional<std::size_t> FirstRegion = findRegionIndex(Address);
  if (!FirstRegion) {
    Plan.Fault = RuntimeMemoryFaultKindV1::Unmapped;
    return Plan;
  }

  std::size_t RegionIndex = *FirstRegion;
  uint64_t Cursor = Address;
  uint64_t Remaining = Size;
  while (Remaining != 0) {
    const Region &Current = Regions[RegionIndex];
    if (!hasPermission(Current.Permissions, RequiredPermission)) {
      Plan.Fault = RuntimeMemoryFaultKindV1::PermissionDenied;
      return Plan;
    }

    const std::size_t Offset =
        static_cast<std::size_t>(Cursor - Current.Address);
    const uint64_t Available =
        static_cast<uint64_t>(Current.Bytes.size() - Offset);
    const uint64_t SpanSize = std::min(Remaining, Available);
    Plan.Spans.push_back({RegionIndex, Offset, Cursor, SpanSize});
    Remaining -= SpanSize;
    if (Remaining == 0)
      return Plan;

    Cursor += SpanSize;
    ++RegionIndex;
    if (RegionIndex == Regions.size() ||
        Regions[RegionIndex].Address != Cursor) {
      Plan.Fault = ContinuationFault;
      return Plan;
    }
  }
  return Plan;
}

GuestMemoryAccessResult GuestMemoryRuntime::fault(
    RuntimeMemoryFaultKindV1 Kind, uint64_t Address, uint64_t AccessSize,
    MemoryAccessKind Access, uint32_t RequiredAlignment,
    uint64_t ExpectedGeneration, uint64_t ObservedGeneration) {
  GuestMemoryAccessResult Result;
  Result.Status = GuestMemoryAccessStatus::Fault;
  uint32_t AccessWidthBits = 0;
  if (AccessSize <= std::numeric_limits<uint32_t>::max() / 8)
    AccessWidthBits = static_cast<uint32_t>(AccessSize * 8);
  Result.Fault = GuestMemoryFault{
      Kind,
      MemoryFaultExit{Address, Access, AccessWidthBits, RequiredAlignment},
      AccessSize, ExpectedGeneration, ObservedGeneration};
  recordResult(Result);
  return Result;
}

GuestInstructionFetchResult
GuestMemoryRuntime::fetchFault(RuntimeMemoryFaultKindV1 Kind, uint64_t Address,
                               uint64_t AccessSize) {
  GuestMemoryAccessResult Access =
      fault(Kind, Address, AccessSize, MemoryAccessKind::Execute, 0);
  GuestInstructionFetchResult Result;
  Result.Status = GuestMemoryAccessStatus::Fault;
  Result.Fault = std::move(Access.Fault);
  return Result;
}

GuestMemoryAccessResult
GuestMemoryRuntime::loadScalar(uint64_t Address, GuestScalarWidth Width,
                               uint32_t RequiredAlignment) {
  const std::optional<uint64_t> Bytes = scalarBytes(Width);
  if (!Bytes)
    return fault(RuntimeMemoryFaultKindV1::InvalidAccessWidth, Address, 0,
                 MemoryAccessKind::Read, RequiredAlignment);
  if (RequiredAlignment != 0 && !isPowerOfTwo(RequiredAlignment))
    return fault(RuntimeMemoryFaultKindV1::InvalidAlignment, Address, *Bytes,
                 MemoryAccessKind::Read, RequiredAlignment);
  if (RequiredAlignment != 0 && Address % RequiredAlignment != 0)
    return fault(RuntimeMemoryFaultKindV1::Misaligned, Address, *Bytes,
                 MemoryAccessKind::Read, RequiredAlignment);

  const uint64_t MaxAddress = maximumAddress(AddressWidth);
  if (Address > MaxAddress || *Bytes - 1 > MaxAddress - Address)
    return fault(RuntimeMemoryFaultKindV1::AddressOverflow, Address, *Bytes,
                 MemoryAccessKind::Read, RequiredAlignment);

  const AccessPlan Plan = planAccess(Address, *Bytes, MemoryPermission::Read,
                                     RuntimeMemoryFaultKindV1::CrossRegion);
  if (Plan.Fault != RuntimeMemoryFaultKindV1::None)
    return fault(Plan.Fault, Address, *Bytes, MemoryAccessKind::Read,
                 RequiredAlignment);

  uint64_t Value = 0;
  uint64_t ByteIndex = 0;
  for (const RegionSpan &Span : Plan.Spans) {
    const Region &Current = Regions[Span.RegionIndex];
    for (uint64_t Index = 0; Index != Span.Size; ++Index, ++ByteIndex)
      Value |= uint64_t{Current.Bytes[Span.Offset + Index]} << (ByteIndex * 8);
  }

  GuestMemoryAccessResult Result;
  Result.Value = Value;
  recordResult(Result);
  return Result;
}

GuestMemoryAccessResult
GuestMemoryRuntime::storeScalar(uint64_t Address, GuestScalarWidth Width,
                                uint64_t Value, uint32_t RequiredAlignment) {
  const std::optional<uint64_t> Bytes = scalarBytes(Width);
  if (!Bytes)
    return fault(RuntimeMemoryFaultKindV1::InvalidAccessWidth, Address, 0,
                 MemoryAccessKind::Write, RequiredAlignment);
  if (RequiredAlignment != 0 && !isPowerOfTwo(RequiredAlignment))
    return fault(RuntimeMemoryFaultKindV1::InvalidAlignment, Address, *Bytes,
                 MemoryAccessKind::Write, RequiredAlignment);
  if (RequiredAlignment != 0 && Address % RequiredAlignment != 0)
    return fault(RuntimeMemoryFaultKindV1::Misaligned, Address, *Bytes,
                 MemoryAccessKind::Write, RequiredAlignment);

  const uint64_t MaxAddress = maximumAddress(AddressWidth);
  if (Address > MaxAddress || *Bytes - 1 > MaxAddress - Address)
    return fault(RuntimeMemoryFaultKindV1::AddressOverflow, Address, *Bytes,
                 MemoryAccessKind::Write, RequiredAlignment);

  const AccessPlan Plan = planAccess(Address, *Bytes, MemoryPermission::Write,
                                     RuntimeMemoryFaultKindV1::CrossRegion);
  if (Plan.Fault != RuntimeMemoryFaultKindV1::None)
    return fault(Plan.Fault, Address, *Bytes, MemoryAccessKind::Write,
                 RequiredAlignment);

  std::optional<std::size_t> ExecutableRegionIndex;
  for (const RegionSpan &Span : Plan.Spans) {
    if (!hasPermission(Regions[Span.RegionIndex].Permissions,
                       MemoryPermission::Execute))
      continue;
    if (ExecutableRegionIndex)
      return fault(RuntimeMemoryFaultKindV1::CrossRegion, Address, *Bytes,
                   MemoryAccessKind::Write, RequiredAlignment);
    ExecutableRegionIndex = Span.RegionIndex;
  }

  const bool IsExecutable = ExecutableRegionIndex.has_value();
  uint64_t OldGeneration =
      IsExecutable ? Regions[*ExecutableRegionIndex].Generation : 0;
  uint64_t NewGeneration = OldGeneration;
  if (IsExecutable) {
    if (CodeInvalidation == CodeInvalidationPolicy::RejectExecutableWrites)
      return fault(RuntimeMemoryFaultKindV1::ExecutableWriteRejected, Address,
                   *Bytes, MemoryAccessKind::Write, RequiredAlignment);
    if (CodeInvalidation !=
            CodeInvalidationPolicy::InvalidateOnExecutableWrite &&
        CodeInvalidation != CodeInvalidationPolicy::ValidateBeforeDispatch)
      return fault(RuntimeMemoryFaultKindV1::PolicyViolation, Address, *Bytes,
                   MemoryAccessKind::Write, RequiredAlignment);
    if (OldGeneration == std::numeric_limits<uint64_t>::max())
      return fault(RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow,
                   Address, *Bytes, MemoryAccessKind::Write, RequiredAlignment,
                   0, OldGeneration);
    NewGeneration = OldGeneration + 1;
  }

  uint64_t ByteIndex = 0;
  for (const RegionSpan &Span : Plan.Spans) {
    Region &Current = Regions[Span.RegionIndex];
    for (uint64_t Index = 0; Index != Span.Size; ++Index, ++ByteIndex)
      Current.Bytes[Span.Offset + Index] =
          static_cast<uint8_t>(Value >> (ByteIndex * 8));
  }
  if (IsExecutable)
    Regions[*ExecutableRegionIndex].Generation = NewGeneration;

  GuestMemoryAccessResult Result;
  if (IsExecutable &&
      CodeInvalidation == CodeInvalidationPolicy::InvalidateOnExecutableWrite) {
    Result.Status = GuestMemoryAccessStatus::SelfModification;
    Result.SelfModification =
        SelfModificationExit{Address, *Bytes, OldGeneration, NewGeneration};
  } else if (IsExecutable) {
    Result.Value = NewGeneration;
  }
  recordResult(Result);
  if (IsExecutable &&
      CodeInvalidation == CodeInvalidationPolicy::ValidateBeforeDispatch)
    Control.ObservedGeneration = NewGeneration;
  return Result;
}

GuestInstructionFetchResult GuestMemoryRuntime::fetchInstructionBytes(
    uint64_t Address, llvm::MutableArrayRef<uint8_t> Output) {
  const uint64_t Size = static_cast<uint64_t>(Output.size());
  if (Size == 0)
    return fetchFault(RuntimeMemoryFaultKindV1::InvalidAccessWidth, Address,
                      Size);

  const uint64_t MaxAddress = maximumAddress(AddressWidth);
  if (Address > MaxAddress || Size - 1 > MaxAddress - Address)
    return fetchFault(RuntimeMemoryFaultKindV1::AddressOverflow, Address, Size);

  const AccessPlan Plan = planAccess(Address, Size, MemoryPermission::Execute,
                                     RuntimeMemoryFaultKindV1::Unmapped);
  if (Plan.Fault != RuntimeMemoryFaultKindV1::None)
    return fetchFault(Plan.Fault, Address, Size);

  GuestInstructionFetchResult Result;
  Result.Bindings.reserve(Plan.Spans.size());
  for (const RegionSpan &Span : Plan.Spans) {
    const Region &Current = Regions[Span.RegionIndex];
    Result.Bindings.push_back({Span.Address, Span.Size, Current.Generation});
  }

  std::size_t OutputOffset = 0;
  for (const RegionSpan &Span : Plan.Spans) {
    const Region &Current = Regions[Span.RegionIndex];
    const std::size_t SpanSize = static_cast<std::size_t>(Span.Size);
    std::copy_n(Current.Bytes.begin() + Span.Offset, SpanSize,
                Output.begin() + OutputOffset);
    OutputOffset += SpanSize;
  }

  recordResult(GuestMemoryAccessResult{});
  return Result;
}

GuestMemoryAccessResult
GuestMemoryRuntime::validateExecutableGeneration(uint64_t Address,
                                                 uint64_t Expected) {
  if (CodeInvalidation != CodeInvalidationPolicy::ValidateBeforeDispatch)
    return fault(RuntimeMemoryFaultKindV1::PolicyViolation, Address, 1,
                 MemoryAccessKind::Execute, 0);

  const uint64_t MaxAddress = maximumAddress(AddressWidth);
  if (Address > MaxAddress)
    return fault(RuntimeMemoryFaultKindV1::AddressOverflow, Address, 1,
                 MemoryAccessKind::Execute, 0);
  const std::optional<std::size_t> RegionIndex = findRegionIndex(Address);
  if (!RegionIndex)
    return fault(RuntimeMemoryFaultKindV1::Unmapped, Address, 1,
                 MemoryAccessKind::Execute, 0);
  const Region &Region = Regions[*RegionIndex];
  if (!hasPermission(Region.Permissions, MemoryPermission::Execute))
    return fault(RuntimeMemoryFaultKindV1::PermissionDenied, Address, 1,
                 MemoryAccessKind::Execute, 0);

  if (Region.Generation != Expected)
    return fault(RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch,
                 Address, 1, MemoryAccessKind::Execute, 0, Expected,
                 Region.Generation);

  GuestMemoryAccessResult Result;
  Result.Value = Region.Generation;
  recordResult(Result);
  Control.Flags = static_cast<uint32_t>(RuntimeABIFlagV1::GenerationValidated);
  Control.CurrentPC = Address;
  Control.ExpectedGeneration = Expected;
  Control.ObservedGeneration = Region.Generation;
  return Result;
}

std::optional<uint64_t>
GuestMemoryRuntime::generationForAddress(uint64_t Address) const {
  const std::optional<std::size_t> RegionIndex = findRegionIndex(Address);
  if (!RegionIndex)
    return std::nullopt;
  return Regions[*RegionIndex].Generation;
}

RuntimePollResult GuestMemoryRuntime::poll(uint64_t InstructionDelta,
                                           uint64_t BlockDelta) {
  RuntimePollResult Result;
  Result.InstructionCount = addSaturating(InstructionCount, InstructionDelta);
  Result.BlockCount = addSaturating(BlockCount, BlockDelta);

  if (CancellationRequested.load(std::memory_order_acquire)) {
    Result.Status = RuntimePollStatus::Cancelled;
  } else if (InstructionBudget != 0 &&
             Result.InstructionCount >= InstructionBudget) {
    Result.Status = RuntimePollStatus::BudgetExhausted;
    Result.Budget = BudgetExit{TranslationBudgetKind::GuestInstructions,
                               InstructionBudget, Result.InstructionCount};
  } else if (BlockBudget != 0 && Result.BlockCount >= BlockBudget) {
    Result.Status = RuntimePollStatus::BudgetExhausted;
    Result.Budget = BudgetExit{TranslationBudgetKind::Blocks, BlockBudget,
                               Result.BlockCount};
  }
  recordPoll(Result);
  return Result;
}

void GuestMemoryRuntime::requestCancellation() noexcept {
  CancellationRequested.store(true, std::memory_order_release);
}

bool GuestMemoryRuntime::cancellationRequested() const noexcept {
  return CancellationRequested.load(std::memory_order_acquire);
}

RuntimeControlBlockV1 GuestMemoryRuntime::snapshotControlBlock(
    uint64_t CurrentPC, uint64_t ExpectedGeneration) const noexcept {
  RuntimeControlBlockV1 Snapshot = makeRuntimeControlBlockV1(
      CodeInvalidation, InstructionBudget, BlockBudget);
  Snapshot.CurrentPC = CurrentPC;
  if (Control.Flags ==
          static_cast<uint32_t>(RuntimeABIFlagV1::GenerationValidated) &&
      Control.CurrentPC == CurrentPC)
    Snapshot.Flags = Control.Flags;
  Snapshot.ExpectedGeneration = Control.ExpectedGeneration;
  Snapshot.ObservedGeneration = Control.ObservedGeneration;
  Snapshot.InstructionCount = InstructionCount.load(std::memory_order_relaxed);
  Snapshot.BlockCount = BlockCount.load(std::memory_order_relaxed);
  Snapshot.ScalarResult = Control.ScalarResult;
  Snapshot.Exit = Control.Exit;
  if (Snapshot.Exit.Kind == RuntimeABIExitKindV1::None)
    Snapshot.ExpectedGeneration = ExpectedGeneration;

  if (CancellationRequested.load(std::memory_order_acquire)) {
    Snapshot.Flags = 0;
    Snapshot.CancellationRequested = 1;
    Snapshot.ExpectedGeneration = 0;
    Snapshot.ObservedGeneration = 0;
    Snapshot.ScalarResult = 0;
    Snapshot.Exit = {};
    Snapshot.Exit.Kind = RuntimeABIExitKindV1::Cancelled;
  }
  return Snapshot;
}

std::vector<GuestMemoryRegion>
GuestMemoryRuntime::snapshotMemoryRegions() const {
  std::vector<GuestMemoryRegion> Snapshot;
  Snapshot.reserve(Regions.size());
  for (const Region &Current : Regions)
    Snapshot.push_back({Current.Address, Current.Permissions,
                        Current.Generation, Current.Bytes});
  return Snapshot;
}

void GuestMemoryRuntime::recordResult(const GuestMemoryAccessResult &Result) {
  Control.Flags = 0;
  Control.CurrentPC = 0;
  Control.ExpectedGeneration = 0;
  Control.ObservedGeneration = 0;
  Control.ScalarResult = 0;
  Control.Exit = {};
  switch (Result.Status) {
  case GuestMemoryAccessStatus::Completed:
    Control.ScalarResult = Result.Value;
    return;
  case GuestMemoryAccessStatus::Fault: {
    if (!Result.Fault)
      return;
    Control.Exit.Kind = RuntimeABIExitKindV1::MemoryFault;
    Control.Exit.Fault = Result.Fault->Kind;
    Control.Exit.Address = Result.Fault->Exit.Address;
    Control.Exit.Size = Result.Fault->AccessSize;
    RuntimeMemoryFaultDetailsV1 Details;
    Details.Access =
        llvm::cantFail(runtimeMemoryAccessKind(Result.Fault->Exit.Access));
    Details.RequiredAlignment =
        Result.Fault->Kind == RuntimeMemoryFaultKindV1::InvalidAccessWidth
            ? 0
            : Result.Fault->Exit.RequiredAlignment;
    Details.ExpectedGeneration = Result.Fault->ExpectedGeneration;
    Details.ObservedGeneration = Result.Fault->ObservedGeneration;
    const RuntimeMemoryFaultDetailEncodingV1 Encoding = llvm::cantFail(
        packRuntimeMemoryFaultDetailsV1(Result.Fault->Kind, Details));
    Control.Exit.Detail0 = Encoding.Detail0;
    Control.Exit.Detail1 = Encoding.Detail1;
    Control.ExpectedGeneration = Result.Fault->ExpectedGeneration;
    Control.ObservedGeneration = Result.Fault->ObservedGeneration;
    return;
  }
  case GuestMemoryAccessStatus::SelfModification:
    if (!Result.SelfModification)
      return;
    Control.Exit.Kind = RuntimeABIExitKindV1::SelfModification;
    Control.Exit.Address = Result.SelfModification->Address;
    Control.Exit.Size = Result.SelfModification->Size;
    Control.Exit.Detail0 = Result.SelfModification->OldGeneration;
    Control.Exit.Detail1 = Result.SelfModification->NewGeneration;
    Control.ObservedGeneration = Result.SelfModification->NewGeneration;
    return;
  }
}

void GuestMemoryRuntime::recordPoll(const RuntimePollResult &Result) {
  Control.Flags = 0;
  Control.CurrentPC = 0;
  Control.ExpectedGeneration = 0;
  Control.ObservedGeneration = 0;
  Control.ScalarResult = 0;
  Control.Exit = {};
  switch (Result.Status) {
  case RuntimePollStatus::Continue:
    return;
  case RuntimePollStatus::Cancelled:
    Control.Exit.Kind = RuntimeABIExitKindV1::Cancelled;
    return;
  case RuntimePollStatus::BudgetExhausted:
    if (!Result.Budget)
      return;
    Control.Exit.Kind = RuntimeABIExitKindV1::BudgetExhausted;
    Control.Exit.Size = static_cast<uint64_t>(Result.Budget->Kind);
    Control.Exit.Detail0 = Result.Budget->Limit;
    Control.Exit.Detail1 = Result.Budget->Observed;
    return;
  }
}

} // namespace neverd::translate
