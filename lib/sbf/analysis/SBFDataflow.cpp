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

#include <cstdint>
#include <iterator>
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

bool isScratchAddress(va_t Address) {
  const bool OnStack =
      Address >= kStackStart && Address < kStackStart + kMemoryRegionSize;
  const bool OnHeap =
      Address >= kHeapStart && Address < kHeapStart + kMemoryRegionSize;
  return OnStack || OnHeap;
}

//===----------------------------------------------------------------------===//
// MemoryModel
//===----------------------------------------------------------------------===//
void MemoryModel::clear() {
  Runs.clear();
  TrackedBytes = 0;
}

void MemoryModel::invalidate(va_t Address, uint64_t Size) {
  if (Size == 0 || Runs.empty())
    return;
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

void ScratchState::meet(const ScratchState &Other) {
  Memory.meet(Other.Memory);
  Escaped = Escaped || Other.Escaped;
}

bool ScratchState::operator==(const ScratchState &Other) const {
  return Escaped == Other.Escaped && Memory == Other.Memory;
}

} // namespace neverd::sbf
