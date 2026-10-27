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

#include "llvm/IR/ConstantRange.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <bit>
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
  case RegisterValue::Kind::UnsignedRange:
  case RegisterValue::Kind::SymbolicAddress:
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

RegisterValue unsignedRangeValue(UnsignedValueRange Range) {
  if (Range.Lower == Range.Upper)
    return {RegisterValue::Kind::Constant, Range.Lower, 0};
  if (Range.Lower == 0 && Range.Upper == UINT64_MAX)
    return {};
  return {RegisterValue::Kind::UnsignedRange, Range.Lower,
          std::bit_cast<int64_t>(Range.Upper)};
}

std::optional<UnsignedValueRange>
unsignedValueRange(const RegisterValue &Value,
                   const SymbolicAddressSpace *Addresses) {
  if (Value.ValueKind == RegisterValue::Kind::Constant)
    return UnsignedValueRange{Value.Value, Value.Value};
  if (Value.ValueKind == RegisterValue::Kind::UnsignedRange)
    return UnsignedValueRange{Value.Value,
                              std::bit_cast<uint64_t>(Value.Offset)};
  if (Value.ValueKind != RegisterValue::Kind::SymbolicAddress || !Addresses)
    return std::nullopt;
  const auto Range = (*Addresses)[Value.Value];
  const auto Delta = static_cast<uint64_t>(Value.Offset);
  if ((Value.Offset >= 0 && Delta <= UINT64_MAX - Range.Upper) ||
      (Value.Offset < 0 && uint64_t{0} - Delta <= Range.Lower))
    return UnsignedValueRange{Range.Lower + Delta, Range.Upper + Delta};
  return std::nullopt;
}

std::optional<UnsignedValueRange>
addressValueRange(const RegisterValue &Value,
                  const SymbolicAddressSpace *Addresses) {
  if (Value.ValueKind != RegisterValue::Kind::SymbolicAddress)
    return unsignedValueRange(Value, Addresses);
  if (!Addresses)
    return std::nullopt;
  const auto Bounds = (*Addresses)[Value.Value];
  return UnsignedValueRange{Bounds.Lower + static_cast<uint64_t>(Value.Offset),
                            Bounds.Upper + static_cast<uint64_t>(Value.Offset)};
}

std::optional<RegisterValue> memoryAddress(const RegisterValue &Base,
                                           int64_t Displacement) {
  if (Base.ValueKind == RegisterValue::Kind::SymbolicAddress) {
    int64_t Offset;
    if (llvm::AddOverflow(Base.Offset, Displacement, Offset))
      return std::nullopt;
    return RegisterValue{Base.ValueKind, Base.Value, Offset};
  }
  const auto Number = effectiveAddress(Base, Displacement);
  if (!Number)
    return std::nullopt;
  return RegisterValue{Base.ValueKind == RegisterValue::Kind::StackAddress
                           ? RegisterValue::Kind::StackAddress
                           : RegisterValue::Kind::Constant,
                       *Number, 0};
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

namespace {
RegisterValue coordinateAddress(va_t Address, MemoryCoordinate Coordinate) {
  return {Coordinate == MemoryCoordinate::FrameRelative
              ? RegisterValue::Kind::StackAddress
              : RegisterValue::Kind::Constant,
          Address, 0};
}

std::pair<uint64_t, uint64_t> memoryKey(const RegisterValue &Address) {
  if (Address.ValueKind == RegisterValue::Kind::SymbolicAddress)
    return {Address.Value + 2, static_cast<uint64_t>(Address.Offset)};
  return {Address.ValueKind == RegisterValue::Kind::StackAddress ? 1 : 0,
          Address.Value};
}

RegisterValue cellAddress(const std::pair<uint64_t, uint64_t> &Key) {
  if (Key.first >= 2)
    return {RegisterValue::Kind::SymbolicAddress, Key.first - 2,
            std::bit_cast<int64_t>(Key.second)};
  return {Key.first == 1 ? RegisterValue::Kind::StackAddress
                         : RegisterValue::Kind::Constant,
          Key.second, 0};
}
} // namespace

std::optional<uint64_t> StoredRegisterMemory::frameTranslationBound() const {
  return Coordinates ? Coordinates->FrameTranslationLimit : std::nullopt;
}

const std::shared_ptr<const SymbolicAddressSpace> &
StoredRegisterMemory::symbolicAddresses() const {
  static const std::shared_ptr<const SymbolicAddressSpace> Empty;
  return Coordinates ? Coordinates->Addresses : Empty;
}

void StoredRegisterMemory::setFrameTranslationLimit(
    std::optional<uint64_t> Limit) {
  if (Limit == frameTranslationBound())
    return;
  Coordinates = std::make_shared<StoredMemoryCoordinates>(
      StoredMemoryCoordinates{Limit, symbolicAddresses()});
}

void StoredRegisterMemory::setSymbolicAddresses(
    std::shared_ptr<const SymbolicAddressSpace> Space) {
  if (Space == symbolicAddresses())
    return;
  Coordinates = std::make_shared<StoredMemoryCoordinates>(
      StoredMemoryCoordinates{frameTranslationBound(), std::move(Space)});
}

bool StoredRegisterMemory::operator==(const StoredRegisterMemory &Other) const {
  return Values == Other.Values &&
         frameTranslationBound() == Other.frameTranslationBound() &&
         symbolicAddresses() == Other.symbolicAddresses();
}

void StoredRegisterMemory::write(va_t Address, const RegisterValue &Value,
                                 MemoryCoordinate Coordinate) {
  writeValues(Address, ProvenValueSet(Value), Coordinate);
}

void StoredRegisterMemory::writeValues(va_t Address, ProvenValueSet Value,
                                       MemoryCoordinate Coordinate) {
  writeValues(coordinateAddress(Address, Coordinate), std::move(Value));
}

void StoredRegisterMemory::writeValues(const RegisterValue &Address,
                                       ProvenValueSet Value) {
  const auto Key = memoryAddress(Address);
  invalidate(Address, sizeof(uint64_t));
  if (Key && !Value.unknown() &&
      Values.size() < kMaxModeledScratchBytes / sizeof(uint64_t))
    Values.emplace(memoryKey(*Key), std::move(Value));
}

void StoredRegisterMemory::invalidate(va_t Address, uint64_t Size,
                                      MemoryCoordinate Coordinate) {
  if (Size > UINT64_MAX - Address) {
    clear();
    return;
  }
  invalidate(coordinateAddress(Address, Coordinate), Size);
}

void StoredRegisterMemory::invalidate(const RegisterValue &Address,
                                      uint64_t Size) {
  if (Size == 0)
    return;
  const auto Key = memoryAddress(Address);
  const auto Footprint =
      [&](const RegisterValue &Start,
          uint64_t Bytes) -> std::optional<llvm::ConstantRange> {
    auto Range = addressValueRange(Start, symbolicAddresses().get());
    if (Start.ValueKind == RegisterValue::Kind::StackAddress) {
      const auto Translation = frameTranslationBound();
      if (!Translation)
        return std::nullopt;
      Range = UnsignedValueRange{Start.Value, Start.Value + *Translation};
    }
    if (!Range)
      return std::nullopt;
    auto Result = llvm::ConstantRange::getNonEmpty(
        llvm::APInt(64, Range->Lower), llvm::APInt(64, Range->Upper + 1));
    if (Bytes > 1)
      Result = Result.add(
          llvm::ConstantRange(llvm::APInt(64, 0), llvm::APInt(64, Bytes)));
    return Result;
  };
  const auto Written = Footprint(Key.value_or(Address), Size);
  for (auto It = Values.begin(); It != Values.end();) {
    const auto Other = cellAddress(It->first);
    const bool SameAnchor =
        Key && Key->ValueKind == Other.ValueKind &&
        (Other.ValueKind != RegisterValue::Kind::SymbolicAddress ||
         Key->Value == Other.Value);
    bool Disjoint = false;
    if (SameAnchor) {
      const uint64_t Start =
          Other.ValueKind == RegisterValue::Kind::SymbolicAddress
              ? static_cast<uint64_t>(Other.Offset)
              : Other.Value;
      const uint64_t Write =
          Key->ValueKind == RegisterValue::Kind::SymbolicAddress
              ? static_cast<uint64_t>(Key->Offset)
              : Key->Value;
      Disjoint = Start - Write >= Size && Write - Start >= sizeof(uint64_t);
    } else if (Written) {
      const auto Existing = Footprint(Other, sizeof(uint64_t));
      Disjoint = Existing && Existing->intersectWith(*Written).isEmptySet();
    }
    if (Disjoint)
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
  std::erase_if(Values, [](const auto &Cell) {
    return Cell.first.first == 1 ||
           std::any_of(Cell.second.values().begin(), Cell.second.values().end(),
                       [](const RegisterValue &Value) {
                         return Value.ValueKind ==
                                RegisterValue::Kind::StackAddress;
                       });
  });
}

uint64_t StoredRegisterMemory::retainedByteEstimate() const {
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
  return readValues(coordinateAddress(Address, Coordinate));
}

const ProvenValueSet &
StoredRegisterMemory::readValues(const RegisterValue &Address) const {
  const auto Key = memoryAddress(Address);
  static const ProvenValueSet Unknown;
  if (!Key)
    return Unknown;
  const auto It = Values.find(memoryKey(*Key));
  return It == Values.end() ? Unknown : It->second;
}

void StoredRegisterMemory::meet(const StoredRegisterMemory &Other) {
  const auto Bound = frameTranslationBound();
  const auto OtherBound = Other.frameTranslationBound();
  setFrameTranslationLimit(Bound && OtherBound
                               ? std::optional(std::max(*Bound, *OtherBound))
                               : std::nullopt);
  std::erase_if(Values, [&](const auto &Cell) {
    return Cell.second != Other.readValues(cellAddress(Cell.first));
  });
}

void StoredRegisterMemory::joinValues(const StoredRegisterMemory &Other) {
  const auto Bound = frameTranslationBound();
  const auto OtherBound = Other.frameTranslationBound();
  setFrameTranslationLimit(Bound && OtherBound
                               ? std::optional(std::max(*Bound, *OtherBound))
                               : std::nullopt);
  for (auto It = Values.begin(); It != Values.end();) {
    It->second.join(Other.readValues(cellAddress(It->first)));
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
  const auto Bound = frameTranslationBound();
  ID.AddBoolean(Bound.has_value());
  if (Bound)
    ID.AddInteger(*Bound);
  ID.AddInteger(Values.size());
  for (const auto &[Key, Value] : Values) {
    ID.AddInteger(Key.first);
    ID.AddInteger(Key.second);
    Value.profile(ID);
  }
}

void StoredRegisterMemory::copyFrom(const StoredRegisterMemory &Source,
                                    va_t SourceAddress, va_t Destination,
                                    uint64_t Size,
                                    MemoryCoordinate SourceCoordinate,
                                    MemoryCoordinate DestinationCoordinate) {
  copyFrom(Source, coordinateAddress(SourceAddress, SourceCoordinate),
           coordinateAddress(Destination, DestinationCoordinate), Size);
}

void StoredRegisterMemory::copyFrom(const StoredRegisterMemory &Source,
                                    const RegisterValue &SourceAddress,
                                    const RegisterValue &Destination,
                                    uint64_t Size) {
  const auto From = memoryAddress(SourceAddress);
  const auto To = memoryAddress(Destination);
  std::vector<std::pair<RegisterValue, ProvenValueSet>> Copied;
  if (From && To)
    for (const auto &[Key, Value] : Source.Values) {
      const auto Cell = cellAddress(Key);
      if (Cell.ValueKind != From->ValueKind ||
          (Cell.ValueKind == RegisterValue::Kind::SymbolicAddress &&
           Cell.Value != From->Value))
        continue;
      const uint64_t Offset =
          Cell.ValueKind == RegisterValue::Kind::SymbolicAddress
              ? static_cast<uint64_t>(Cell.Offset) -
                    static_cast<uint64_t>(From->Offset)
              : Cell.Value - From->Value;
      if (Offset >= Size || sizeof(uint64_t) > Size - Offset ||
          Offset > static_cast<uint64_t>(INT64_MAX))
        continue;
      if (auto Address = memoryAddress(*To, static_cast<int64_t>(Offset)))
        Copied.emplace_back(*Address, Value);
    }
  invalidate(Destination, Size);
  // Invalidate the complete destination once. Repeated strong stores through
  // possibly aliasing anchors must not erase other snapshotted copy fields.
  for (const auto &[Address, Value] : Copied)
    if (Values.size() < kMaxModeledScratchBytes / sizeof(uint64_t))
      Values[memoryKey(Address)] = Value;
}

void StoredRegisterMemory::transformSymbols(
    llvm::function_ref<RegisterValue(RegisterValue)> Map) {
  decltype(Values) Transformed;
  for (const auto &[Key, Value] : Values) {
    auto Address = cellAddress(Key);
    if (Address.ValueKind == RegisterValue::Kind::SymbolicAddress)
      Address = Map(Address);
    const auto NewKey = memoryAddress(Address);
    if (!NewKey)
      continue;
    std::optional<ProvenValueSet> Stored;
    for (auto Alternative : Value.values()) {
      if (Alternative.ValueKind == RegisterValue::Kind::SymbolicAddress)
        Alternative = Map(Alternative);
      if (!Stored)
        Stored = Alternative;
      else
        Stored->join(Alternative);
    }
    if (Stored && !Stored->unknown())
      Transformed.emplace(memoryKey(*NewKey), std::move(*Stored));
  }
  Values = std::move(Transformed);
}

void StoredRegisterMemory::visitCells(
    llvm::function_ref<void(const RegisterValue &, const ProvenValueSet &)>
        Visit) const {
  for (const auto &[Key, Value] : Values)
    Visit(cellAddress(Key), Value);
}

void StoredRegisterMemory::includeCells(
    const StoredRegisterMemory &Other,
    llvm::function_ref<bool(const RegisterValue &)> Include) {
  Other.visitCells(
      [&](const RegisterValue &Address, const ProvenValueSet &Value) {
        if (Include(Address) &&
            Values.size() < kMaxModeledScratchBytes / sizeof(uint64_t))
          Values[memoryKey(Address)] = Value;
      });
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
