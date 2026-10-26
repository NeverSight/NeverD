//===- SBFDataflow.cpp - Solana SBF address and memory model --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The address a tracked register value denotes, which addresses belong to a
/// program's own frame and heap, and the byte-run model that records what a
/// program has provably written into them.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/analysis/SBFDataflow.h"

#include "neverd/sbf/SBFConstants.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::sbf {

std::optional<va_t> effectiveAddress(const RegisterValue &Base,
                                     int64_t Displacement) {
  switch (Base.ValueKind) {
  case RegisterValue::Kind::Unknown:
    return std::nullopt;
  case RegisterValue::Kind::Constant:
    return Base.Value + static_cast<uint64_t>(Displacement);
  case RegisterValue::Kind::StackAddress:
  case RegisterValue::Kind::RodataAddress:
    return Base.Value + static_cast<uint64_t>(Base.Offset) +
           static_cast<uint64_t>(Displacement);
  case RegisterValue::Kind::InstructionDataAddress:
    // The base depends on how many accounts the transaction passed and how
    // long each one's data is, none of which the program file says. Tracking
    // the displacement is still worth doing — it is how a discriminator read
    // is recognized — but naming an address would be naming a guess.
    return std::nullopt;
  }
  return std::nullopt;
}

bool isScratchRange(va_t Address, uint64_t Size) {
  const auto IsWithin = [Address, Size](va_t RegionStart) {
    if (Address < RegionStart)
      return false;
    const uint64_t Offset = Address - RegionStart;
    return Offset < kMemoryRegionSize && Size <= kMemoryRegionSize - Offset;
  };
  return IsWithin(kStackStart) || IsWithin(kHeapStart);
}

bool isScratchAddress(va_t Address) {
  return isScratchRange(Address, /*Size=*/1);
}

//===----------------------------------------------------------------------===//
// MemoryModel
//===----------------------------------------------------------------------===//
void MemoryModel::clear() {
  Runs.clear();
  TrackedBytes = 0;
}

uint64_t MemoryModel::retainedByteEstimate() const {
  if (Runs.empty())
    return 0;

  // A std::map node owns its value plus parent/left/right links and balancing
  // metadata. Pointer-width charges conservatively cover those fields plus
  // allocator bookkeeping on the supported standard libraries; vector
  // capacity accounts for bytes reserved beyond its current logical size.
  constexpr uint64_t OrderedNodeLinkAndTagWords = 4;
  constexpr uint64_t AllocatorBookkeepingWords = 2;
  constexpr uint64_t OrderedNodeOverhead =
      (OrderedNodeLinkAndTagWords + AllocatorBookkeepingWords) * sizeof(void *);
  uint64_t Bytes = sizeof(*this);
  for (const auto &[Address, Run] : Runs) {
    (void)Address;
    Bytes += sizeof(decltype(Runs)::value_type) + OrderedNodeOverhead;
    Bytes += Run.capacity() * sizeof(uint8_t);
  }
  return Bytes;
}

void MemoryModel::invalidate(va_t Address, uint64_t Size) {
  if (Size == 0 || Runs.empty())
    return;
  // A counted syscall can receive the full register width as its length.
  // Once the half-open end cannot be represented, there is no sound finite
  // interval to retain: conservatively forget every scratch fact instead of
  // letting the wrapped end make the write look empty.
  if (Size > std::numeric_limits<va_t>::max() - Address) {
    clear();
    return;
  }
  const va_t End = Address + Size;

  auto It = Runs.upper_bound(Address);
  if (It != Runs.begin())
    --It;
  while (It != Runs.end() && It->first < End) {
    const va_t Start = It->first;
    const uint64_t Length = It->second.size();
    if (Start + Length <= Address) {
      ++It;
      continue;
    }

    std::vector<uint8_t> Head;
    if (Start < Address)
      Head.assign(It->second.begin(),
                  It->second.begin() + static_cast<ptrdiff_t>(Address - Start));
    std::vector<uint8_t> Tail;
    if (Start + Length > End)
      Tail.assign(It->second.begin() + static_cast<ptrdiff_t>(End - Start),
                  It->second.end());

    TrackedBytes -= Length;
    It = Runs.erase(It);
    if (!Head.empty()) {
      TrackedBytes += Head.size();
      Runs.emplace(Start, std::move(Head));
    }
    if (!Tail.empty()) {
      TrackedBytes += Tail.size();
      Runs.emplace(End, std::move(Tail));
    }
  }
}

void MemoryModel::invalidateFrom(va_t Address) {
  // A buffer never crosses a VM region boundary, so a write of unproven length
  // starting here reaches no further than the end of its own region.
  const va_t RegionEnd = (Address / kMemoryRegionSize + 1) * kMemoryRegionSize;
  invalidate(Address, RegionEnd - Address);
}

void MemoryModel::write(va_t Address, llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.empty())
    return;
  invalidate(Address, Bytes.size());
  // Past the budget the model stops describing new bytes rather than dropping
  // what it already proved, so a store loop degrades into silence.
  if (TrackedBytes + Bytes.size() > kMaxModeledScratchBytes)
    return;

  std::vector<uint8_t> Run(Bytes.begin(), Bytes.end());
  va_t Start = Address;

  auto After = Runs.upper_bound(Address);
  if (After != Runs.begin()) {
    auto Before = std::prev(After);
    if (Before->first + Before->second.size() == Address) {
      Start = Before->first;
      std::vector<uint8_t> Merged = std::move(Before->second);
      Merged.insert(Merged.end(), Run.begin(), Run.end());
      Run = std::move(Merged);
      Runs.erase(Before);
    }
  }
  if (auto Next = Runs.find(Address + Bytes.size()); Next != Runs.end()) {
    Run.insert(Run.end(), Next->second.begin(), Next->second.end());
    Runs.erase(Next);
  }

  TrackedBytes += Bytes.size();
  Runs.insert_or_assign(Start, std::move(Run));
}

llvm::ArrayRef<uint8_t> MemoryModel::read(va_t Address, uint64_t Size) const {
  if (Size == 0 || Runs.empty())
    return {};
  auto It = Runs.upper_bound(Address);
  if (It == Runs.begin())
    return {};
  --It;
  const uint64_t Offset = Address - It->first;
  const std::vector<uint8_t> &Bytes = It->second;
  if (Offset > Bytes.size() || Size > Bytes.size() - Offset)
    return {};
  return llvm::ArrayRef(Bytes).slice(Offset, Size);
}

std::optional<uint64_t> MemoryModel::readWord(va_t Address) const {
  const llvm::ArrayRef<uint8_t> Bytes = read(Address, sizeof(uint64_t));
  if (Bytes.empty())
    return std::nullopt;
  return llvm::support::endian::read64le(Bytes.data());
}

void MemoryModel::meet(const MemoryModel &Other) {
  MemoryModel Agreed;
  for (const auto &[Start, Bytes] : Runs) {
    std::vector<uint8_t> Span;
    va_t SpanStart = Start;
    for (size_t Index = 0; Index < Bytes.size(); ++Index) {
      const llvm::ArrayRef<uint8_t> Byte = Other.read(Start + Index, 1);
      if (!Byte.empty() && Byte.front() == Bytes[Index]) {
        if (Span.empty())
          SpanStart = Start + Index;
        Span.push_back(Bytes[Index]);
        continue;
      }
      if (!Span.empty()) {
        Agreed.write(SpanStart, Span);
        Span.clear();
      }
    }
    if (!Span.empty())
      Agreed.write(SpanStart, Span);
  }
  *this = std::move(Agreed);
}

bool MemoryModel::operator==(const MemoryModel &Other) const {
  return Runs == Other.Runs;
}

ProvenValueSet::ProvenValueSet(const RegisterValue &Value) {
  if (Value.ValueKind != RegisterValue::Kind::Unknown)
    Values.push_back(Value);
}

RegisterValue ProvenValueSet::singleton() const {
  return Values.size() == 1 ? Values.front() : RegisterValue{};
}

void ProvenValueSet::join(const ProvenValueSet &Other) {
  if (unknown() || Other.unknown()) {
    Values.clear();
    return;
  }
  for (const RegisterValue &Value : Other.Values)
    if (std::find(Values.begin(), Values.end(), Value) == Values.end()) {
      if (Values.size() == kCallXContextValueAlternatives) {
        Values.clear();
        return;
      }
      Values.push_back(Value);
    }
  std::sort(Values.begin(), Values.end(), [](const auto &L, const auto &R) {
    if (L.ValueKind != R.ValueKind)
      return L.ValueKind < R.ValueKind;
    if (L.Value != R.Value)
      return L.Value < R.Value;
    return L.Offset < R.Offset;
  });
}

void ProvenValueSet::profile(llvm::FoldingSetNodeID &ID) const {
  ID.AddInteger(Values.size());
  for (const auto &Value : Values) {
    ID.AddInteger(static_cast<unsigned>(Value.ValueKind));
    ID.AddInteger(Value.Value);
    ID.AddInteger(static_cast<uint64_t>(Value.Offset));
  }
}

uint64_t ProvenValueSet::allocatedBytes() const {
  return Values.capacity() > 1 ? Values.capacity() * sizeof(RegisterValue) : 0;
}

void StoredRegisterMemory::write(va_t Address, const RegisterValue &Value,
                                 MemoryCoordinate Coordinate) {
  writeValues(Address, ProvenValueSet(Value), Coordinate);
}

void StoredRegisterMemory::writeValues(va_t Address, ProvenValueSet Value,
                                       MemoryCoordinate Coordinate) {
  // Value is snapshotted before invalidation, including a readValues result
  // borrowed from this very cell.
  invalidate(Address, sizeof(uint64_t), Coordinate);
  if (!Value.unknown() &&
      Values.size() < kMaxModeledScratchBytes / sizeof(uint64_t))
    Values.emplace(std::make_pair(Coordinate, Address), std::move(Value));
}

void StoredRegisterMemory::invalidate(va_t Address, uint64_t Size,
                                      MemoryCoordinate Coordinate) {
  if (Size == 0)
    return;
  if (Size > std::numeric_limits<va_t>::max() - Address) {
    clear();
    return;
  }
  const va_t End = Address + Size;
  for (auto It = Values.begin(); It != Values.end();) {
    const auto [OtherCoordinate, Start] = It->first;
    if (OtherCoordinate == Coordinate &&
        (Start >= End ||
         (Start < Address && Address - Start >= sizeof(uint64_t))))
      ++It;
    else
      It = Values.erase(It);
  }
}

void StoredRegisterMemory::invalidateFrom(va_t Address,
                                          MemoryCoordinate Coordinate) {
  const va_t RegionEnd = (Address / kMemoryRegionSize + 1) * kMemoryRegionSize;
  invalidate(Address, RegionEnd - Address, Coordinate);
}

void StoredRegisterMemory::discardFrameCoordinates() {
  for (auto It = Values.begin(); It != Values.end();) {
    const bool FrameValue = std::any_of(
        It->second.values().begin(), It->second.values().end(),
        [](const RegisterValue &Value) {
          return Value.ValueKind == RegisterValue::Kind::StackAddress;
        });
    if (It->first.first == MemoryCoordinate::FrameRelative || FrameValue)
      It = Values.erase(It);
    else
      ++It;
  }
}

uint64_t StoredRegisterMemory::retainedByteEstimate() const {
  // The same ordered-node and allocator charge used by MemoryModel.
  constexpr uint64_t NodeOverhead = 6 * sizeof(void *);
  uint64_t Allocated = 0;
  for (const auto &[Key, Value] : Values)
    Allocated += Value.allocatedBytes();
  return Values.empty()
             ? 0
             : sizeof(*this) + Allocated +
                   Values.size() *
                       (sizeof(decltype(Values)::value_type) + NodeOverhead);
}

RegisterValue StoredRegisterMemory::read(va_t Address,
                                         MemoryCoordinate Coordinate) const {
  return readValues(Address, Coordinate).singleton();
}

const ProvenValueSet &
StoredRegisterMemory::readValues(va_t Address,
                                 MemoryCoordinate Coordinate) const {
  const auto It = Values.find(std::make_pair(Coordinate, Address));
  static const ProvenValueSet Unknown;
  return It == Values.end() ? Unknown : It->second;
}

void StoredRegisterMemory::meet(const StoredRegisterMemory &Other) {
  for (auto It = Values.begin(); It != Values.end();) {
    if (It->second != Other.readValues(It->first.second, It->first.first))
      It = Values.erase(It);
    else
      ++It;
  }
}

void StoredRegisterMemory::joinValues(const StoredRegisterMemory &Other) {
  for (auto It = Values.begin(); It != Values.end();) {
    It->second.join(Other.readValues(It->first.second, It->first.first));
    if (It->second.unknown())
      It = Values.erase(It);
    else
      ++It;
  }
}

void MemoryModel::profile(llvm::FoldingSetNodeID &ID) const {
  ID.AddInteger(Runs.size());
  for (const auto &[Address, Bytes] : Runs) {
    ID.AddInteger(Address);
    ID.AddString(llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                                 Bytes.size()));
  }
}

void StoredRegisterMemory::profile(llvm::FoldingSetNodeID &ID) const {
  ID.AddInteger(Values.size());
  for (const auto &[Key, Value] : Values) {
    ID.AddInteger(static_cast<unsigned>(Key.first));
    ID.AddInteger(Key.second);
    Value.profile(ID);
  }
}

void StoredRegisterMemory::copyFrom(const StoredRegisterMemory &Source,
                                    va_t SourceAddress, va_t Destination,
                                    uint64_t Size,
                                    MemoryCoordinate SourceCoordinate,
                                    MemoryCoordinate DestinationCoordinate) {
  // Snapshot first: memmove can overlap and invalidation must not erase its
  // source. Partial pointer words deliberately prove no full-width value.
  std::vector<std::pair<va_t, ProvenValueSet>> Copied;
  for (auto It = Source.Values.lower_bound(
           std::make_pair(SourceCoordinate, SourceAddress));
       It != Source.Values.end() && It->first.first == SourceCoordinate; ++It) {
    const uint64_t Offset = It->first.second - SourceAddress;
    if (Offset >= Size)
      break;
    if (sizeof(uint64_t) <= Size - Offset)
      Copied.emplace_back(Destination + Offset, It->second);
  }
  invalidate(Destination, Size, DestinationCoordinate);
  for (const auto &[Address, Value] : Copied)
    writeValues(Address, Value, DestinationCoordinate);
}

uint64_t ScratchState::retainedByteEstimate() const {
  return sizeof(*this) +
         (Memory.empty() ? 0 : Memory.retainedByteEstimate() - sizeof(Memory)) +
         (Values.empty() ? 0 : Values.retainedByteEstimate() - sizeof(Values));
}

void ScratchState::clear() {
  Memory.clear();
  Values.clear();
}

void ScratchState::invalidate(va_t Address, uint64_t Size) {
  Memory.invalidate(Address, Size);
  Values.invalidate(Address, Size);
}

void ScratchState::invalidateFrom(va_t Address) {
  Memory.invalidateFrom(Address);
  Values.invalidateFrom(Address);
}

void ScratchState::meet(const ScratchState &Other) {
  Memory.meet(Other.Memory);
  Values.meet(Other.Values);
}

bool ScratchState::operator==(const ScratchState &Other) const {
  return Memory == Other.Memory && Values == Other.Values;
}

} // namespace neverd::sbf
