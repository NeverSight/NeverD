//===- SymState.cpp - Symbolic machine state ------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the byte-addressed store described in SymState.h.
///
/// Every read and write records the same byte-level split-and-join. Scalar
/// bytes keep an exact view of the written word; a matching read can reuse
/// that word without first creating extracts and intermediate concatenations.
/// Partial overwrites and unaligned reads still assemble the actual bytes.
/// Memory keeps its materialized byte expressions and provenance unchanged.
///
/// Memory adds one step in front of that: deciding which store a given address
/// belongs to.  The builders have already put an address into a canonical sum
/// with its constant term first, so `sp - 8` arrives as a base and a
/// displacement without this file having to walk address arithmetic, and every
/// frame slot lands in the same bank at a different offset.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymState.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace neverd::symbolic {

bool SymState::setX64MemoryAddressSpaceBase(NdMemoryAddressSpace Space,
                                            SymRef Base) {
  if ((Space != NdMemoryAddressSpace::X86FS &&
       Space != NdMemoryAddressSpace::X86GS) ||
      !Base || Base.index() >= Ctx->numNodes() || Ctx->width(Base) != 64)
    return false;
  X64MemoryAddressSpaceBases[Space == NdMemoryAddressSpace::X86FS ? 0 : 1] =
      Base;
  return true;
}

SymRef SymState::x64MemoryAddressSpaceBase(NdMemoryAddressSpace Space) const {
  if (Space == NdMemoryAddressSpace::X86FS)
    return X64MemoryAddressSpaceBases[0];
  if (Space == NdMemoryAddressSpace::X86GS)
    return X64MemoryAddressSpaceBases[1];
  return {};
}

void SymState::clearX64MemoryAddressSpaceBases() {
  X64MemoryAddressSpaceBases = {};
}

llvm::SmallVector<SymConstantByte, 32> SymState::constantScalarBytes() const {
  llvm::SmallVector<SymConstantByte, 32> Result;
  const auto Collect = [&](SymSpace Space, const Bank &Storage) {
    for (const auto &[Offset, View] : Storage.Bytes)
      if (const auto Constant = Ctx->constantWindow(View.Value, View.Low, 8))
        Result.push_back(
            {Space, Offset, static_cast<uint8_t>(Constant->getZExtValue())});
  };
  Collect(SymSpace::Register, Registers);
  Collect(SymSpace::Temporary, Temporaries);
  return Result;
}

llvm::SmallVector<SymConstantRegionByte, 32>
SymState::constantRegionBytes(SymRef Base) const {
  llvm::SmallVector<SymConstantRegionByte, 32> Result;
  if (!Base)
    return Result;
  const auto Found = Regions.find(Base.index());
  if (Found == Regions.end())
    return Result;
  for (const auto &[Offset, View] : Found->second.Bytes)
    if (const auto Constant = Ctx->asConst(View.Value))
      Result.push_back(
          {Offset, static_cast<uint8_t>(Constant->getZExtValue())});
  return Result;
}

llvm::SmallVector<SymConstantRegionByte, 32>
SymState::constantAbsoluteBytes() const {
  llvm::SmallVector<SymConstantRegionByte, 32> Result;
  for (const auto &[Offset, View] : AbsoluteMemory.Bytes)
    if (const auto Constant = Ctx->constantWindow(View.Value, View.Low, 8))
      Result.push_back(
          {Offset, static_cast<uint8_t>(Constant->getZExtValue())});
  return Result;
}

namespace {
constexpr uint32_t kByteBits = 8;

/// Widest address the region split works on.  A displacement is kept as a
/// 64-bit key, so a wider address space cannot be indexed that way and each of
/// its addresses becomes a region of its own instead.
constexpr uint32_t kMaxAddressBits = 64;

bool defersByteViews(std::optional<SymInputKind> Kind) {
  return Kind == SymInputKind::Register || Kind == SymInputKind::Temporary;
}

bool hasModularCoordinates(const SymContext &Ctx, SymRef RegionBase) {
  return RegionBase && Ctx.width(RegionBase) == kMaxAddressBits;
}

bool validBankWrite(const SymContext &Ctx, SymRef RegionBase, uint64_t Offset,
                    SymRef Value) {
  if (!Value)
    return false;
  const uint32_t Width = Ctx.width(Value);
  if (!Width || Width % kByteBits)
    return false;
  const uint32_t Bytes = Width / kByteBits;
  return Bytes <= std::numeric_limits<uint16_t>::max() &&
         (hasModularCoordinates(Ctx, RegionBase) ||
          Offset <= std::numeric_limits<uint64_t>::max() - (Bytes - 1));
}

std::string byteName(llvm::StringRef Prefix, uint64_t Offset) {
  return (llvm::Twine(Prefix) + "$" + llvm::Twine(Offset)).str();
}
} // namespace

SymState::Bank &SymState::bank(SymSpace Space) {
  switch (Space) {
  case SymSpace::Register:
    return Registers;
  case SymSpace::Temporary:
    return Temporaries;
  case SymSpace::Memory:
    return AbsoluteMemory;
  }
  llvm_unreachable("unhandled symbolic space");
}

SymRef SymState::freshInput(llvm::StringRef Prefix, uint32_t Width) {
  const std::string NamedPrefix = (Prefix + llvm::Twine('$')).str();
  return Ctx->mkFreshVar(Width, NamedPrefix);
}

//===----------------------------------------------------------------------===//
// Byte-addressed banks
//===----------------------------------------------------------------------===//

SymState::ByteView SymState::byteAt(Bank &B, uint64_t Offset) {
  auto It = B.Bytes.find(Offset);
  if (It != B.Bytes.end())
    return It->second;

  ByteView Input;
  if (B.Unknowns) {
    auto Unknown = B.Unknowns->Values.find(Offset);
    if (Unknown == B.Unknowns->Values.end()) {
      Input = {Ctx->mkFreshVar(kByteBits, byteName(B.Name, Offset) + "$")};
      B.Unknowns->Values.emplace(Offset, Input);
    } else {
      Input = Unknown->second;
    }
  } else {
    // Before anything was forgotten, name an untouched location for where it
    // is.  Once it has been, the shared epoch above keeps copied states equal
    // while independently forgetful forks receive different values.
    const std::string Name = byteName(B.Name, Offset);
    Input.Value =
        B.InputKind
            ? Ctx->mkInputVar(Name, kByteBits,
                              SymInputOrigin{*B.InputKind, Offset, 1, B.Epoch},
                              Order)
            : Ctx->mkVar(Name, kByteBits);
    if (B.RegionBase && B.Epoch == 0)
      recordMemoryInputOrigin(Input.Value, {B.RegionBase, Offset, 1});
  }
  B.Bytes.emplace(Offset, Input);
  return Input;
}

SymRef SymState::joinBytes(llvm::ArrayRef<SymRef> Bytes) const {
  assert(!Bytes.empty());
  if (Bytes.size() == 1)
    return Bytes.front();

  // Concatenation takes its operands most significant first, which is the
  // reverse of address order on a little-endian target.
  llvm::SmallVector<SymRef, 8> Ordered(Bytes.begin(), Bytes.end());
  if (Order == llvm::endianness::little)
    std::reverse(Ordered.begin(), Ordered.end());
  return Ctx->mkConcat(Ordered);
}

SymRef SymState::readBank(Bank &B, uint64_t Offset, uint16_t Bytes) {
  if (Bytes == 0)
    return {};
  if (Offset > std::numeric_limits<uint64_t>::max() - (Bytes - 1)) {
    if (!hasModularCoordinates(*Ctx, B.RegionBase))
      return {};
    // The relative coordinate can cross zero without the actual address
    // overflowing. Split at the key boundary so each input origin still
    // describes a nonwrapping span, including in shared unknown epochs.
    const auto FirstBytes = static_cast<uint16_t>(
        std::numeric_limits<uint64_t>::max() - Offset + 1);
    const auto First = readBank(B, Offset, FirstBytes);
    const auto Second = readBank(B, 0, Bytes - FirstBytes);
    return Order == llvm::endianness::little ? Ctx->mkConcat({Second, First})
                                             : Ctx->mkConcat({First, Second});
  }

  if (Bytes == 1) {
    const auto View = byteAt(B, Offset);
    return View.Low == 0 && Ctx->width(View.Value) == kByteBits
               ? View.Value
               : Ctx->mkExtract(View.Value, View.Low, kByteBits);
  }

  // Keep an untouched word as one input rather than eagerly turning it into a
  // concatenation of unrelated byte variables.  The write records its byte
  // views, so every later overlapping read still observes exact aliasing.
  bool HasMaterialisedByte = false;
  for (uint16_t I = 0; I < Bytes; ++I)
    HasMaterialisedByte |=
        B.Bytes.count(Offset + I) != 0 ||
        (B.Unknowns && B.Unknowns->Values.count(Offset + I) != 0);
  if (!HasMaterialisedByte) {
    const std::string Name = byteName(B.Name, Offset);
    const uint32_t Width = uint32_t(Bytes) * kByteBits;
    SymRef Input;
    if (B.Unknowns) {
      Input = Ctx->mkFreshVar(Width, Name + "$");
    } else if (B.InputKind) {
      Input = Ctx->mkInputVar(
          Name, Width, SymInputOrigin{*B.InputKind, Offset, Bytes, B.Epoch},
          Order);
    } else {
      Input = Ctx->mkVar(Name, Width);
      if (B.RegionBase && B.Epoch == 0)
        recordMemoryInputOrigin(Input, {B.RegionBase, Offset, Bytes});
    }
    if (!writeBank(B, Offset, Input))
      return {};
    if (B.Unknowns)
      for (uint16_t I = 0; I < Bytes; ++I)
        B.Unknowns->Values.emplace(Offset + I, B.Bytes.at(Offset + I));
    return Input;
  }

  if (!defersByteViews(B.InputKind)) {
    // Memory still stores materialized byte expressions. Avoid inspecting
    // word views or extracting the already complete byte on this path.
    llvm::SmallVector<SymRef, 8> Parts;
    Parts.reserve(Bytes);
    for (uint16_t I = 0; I < Bytes; ++I)
      Parts.push_back(byteAt(B, Offset + I).Value);
    return joinBytes(Parts);
  }

  llvm::SmallVector<ByteView, 8> Views;
  Views.reserve(Bytes);
  for (uint16_t I = 0; I < Bytes; ++I)
    Views.push_back(byteAt(B, Offset + I));

  // A byte view is an exact source and bit offset, not a claim that adjacent
  // locations still contain one word. Check every byte, including middle
  // overwrites, before taking this shortcut in either byte order.
  const auto &Low =
      Order == llvm::endianness::little ? Views.front() : Views.back();
  bool Contiguous = true;
  for (uint16_t I = 0; I < Bytes; ++I) {
    const uint32_t Lane = Order == llvm::endianness::little ? I : Bytes - I - 1;
    Contiguous &= Views[I].Value == Low.Value &&
                  Views[I].Low == Low.Low + Lane * kByteBits;
  }
  if (Contiguous)
    return Ctx->mkExtract(Low.Value, Low.Low, uint32_t(Bytes) * kByteBits);

  llvm::SmallVector<SymRef, 8> Parts;
  Parts.reserve(Bytes);
  for (const auto &View : Views)
    Parts.push_back(Ctx->mkExtract(View.Value, View.Low, kByteBits));
  return joinBytes(Parts);
}

bool SymState::writeBank(Bank &B, uint64_t Offset, SymRef Value) {
  if (!validBankWrite(*Ctx, B.RegionBase, Offset, Value))
    return false;
  const uint32_t Width = Ctx->width(Value);
  const uint32_t ByteCount = Width / kByteBits;
  const uint16_t Bytes = static_cast<uint16_t>(ByteCount);
  const bool Scalar = defersByteViews(B.InputKind);

  for (uint16_t I = 0; I < Bytes; ++I) {
    // Bit position of byte I of the address range, which is the low end of the
    // word on a little-endian target and the high end on a big-endian one.
    const uint32_t Low = Order == llvm::endianness::little
                             ? uint32_t(I) * kByteBits
                             : Width - (uint32_t(I) + 1) * kByteBits;
    B.Bytes[Offset + I] = Scalar
                              ? ByteView{Value, Low}
                              : ByteView{Ctx->mkExtract(Value, Low, kByteBits)};
  }
  return true;
}

void SymState::forget(Bank &B) {
  B.Bytes.clear();
  B.Unknowns = std::make_shared<UnknownBytes>();
  if (B.Epoch != std::numeric_limits<uint64_t>::max())
    ++B.Epoch;
}

SymRef SymState::read(SymSpace Space, uint64_t Offset, uint16_t Bytes) {
  return readBank(bank(Space), Offset, Bytes);
}

void SymState::write(SymSpace Space, uint64_t Offset, SymRef Value) {
  (void)writeBank(bank(Space), Offset, Value);
}

bool SymState::canWriteRegister(uint64_t Offset, SymRef Value) const {
  return validBankWrite(*Ctx, {}, Offset, Value);
}

size_t SymState::numLiveBytes() const {
  size_t Total = Registers.Bytes.size() + Temporaries.Bytes.size() +
                 AbsoluteMemory.Bytes.size();
  for (const auto &Entry : Regions)
    Total += Entry.second.Bytes.size();
  return Total;
}

//===----------------------------------------------------------------------===//
// Memory regions
//===----------------------------------------------------------------------===//

SymState::Location SymState::locate(SymRef Addr) {
  Location Where;

  if (std::optional<llvm::APInt> Value = Ctx->asConst(Addr)) {
    if (Value->getActiveBits() <= kMaxAddressBits) {
      Where.Offset = Value->getZExtValue();
      return Where;
    }
    Where.Base = Addr;
    return Where;
  }

  // The builders put a sum's constant term first and leave at most one of
  // them, so every displacement off a pointer already looks the same here
  // whether the code wrote an addition, a subtraction or an indexed address.
  if (Ctx->op(Addr) == SymOp::Add && Ctx->width(Addr) <= kMaxAddressBits) {
    // Copied out because building the residual base appends to the operand
    // pool the operands are a view of.
    llvm::SmallVector<SymRef, 4> Terms(Ctx->operands(Addr).begin(),
                                       Ctx->operands(Addr).end());
    if (Terms.size() >= 2 && Ctx->isConst(Terms[0])) {
      Where.Offset = Ctx->constValue(Terms[0]).getZExtValue();
      Where.Base = Terms.size() == 2
                       ? Terms[1]
                       : Ctx->mkAdd(llvm::ArrayRef<SymRef>(Terms).drop_front());
      return Where;
    }
  }

  Where.Base = Addr;
  return Where;
}

SymState::Bank &SymState::bankFor(const Location &Where) {
  if (!Where.Base.isValid())
    return AbsoluteMemory;

  auto It = Regions.find(Where.Base.index());
  if (It != Regions.end())
    return It->second;

  Bank Fresh;
  // Named after the base it hangs off rather than after an address, because
  // that is all this region has: what it holds is `*(base + n)` for the
  // displacements the code used, and nothing says what base is.
  Fresh.Name = ("ptr$" + llvm::Twine(Where.Base.index())).str();
  Fresh.RegionBase = Where.Base;
  if (UnseenRegions) {
    auto &Unknowns = UnseenRegions->Values[Where.Base.index()];
    if (!Unknowns)
      Unknowns = std::make_shared<UnknownBytes>();
    Fresh.Unknowns = Unknowns;
  }
  return Regions.emplace(Where.Base.index(), std::move(Fresh)).first->second;
}

void SymState::forgetRegions(SymRef Except) {
  // A write can affect a pointer region that has never been read.  Retain the
  // new default memory epoch as well as forgetting materialised regions, or a
  // later first read would reuse the entry value from an unaffected fork.
  UnseenRegions = std::make_shared<UnknownRegions>();
  for (auto &Entry : Regions)
    if (!Except.isValid() || Entry.first != Except.index())
      forget(Entry.second);
}

void SymState::clobberMemory() {
  forget(AbsoluteMemory);
  forgetRegions(SymRef());
  MemoryClobbered = true;
}

bool SymState::bindLoadOrigins(llvm::ArrayRef<LoadOriginBinding> Bindings) {
  for (const auto &Binding : Bindings)
    if (!Binding.Value || !Binding.Origin.Address || !Binding.Origin.Bytes ||
        Binding.Value.index() >= Ctx->numNodes() ||
        Binding.Origin.Address.index() >= Ctx->numNodes() ||
        Ctx->width(Binding.Value) != unsigned(Binding.Origin.Bytes) * 8)
      return false;
  for (const auto &Binding : Bindings)
    recordLoadOrigin(Binding.Value, Binding.Origin);
  return true;
}

std::optional<llvm::SmallVector<SymState::MaterializedRange, 8>>
SymState::materializedRegionRanges(SymRef Base, uint64_t &WorkBudget) const {
  if (!WorkBudget)
    return std::nullopt;
  --WorkBudget;
  llvm::SmallVector<MaterializedRange, 8> Result;
  if (!Base || Base.index() >= Ctx->numNodes())
    return Result;
  const auto Found = Regions.find(Base.index());
  if (Found == Regions.end())
    return Result;
  return materializedBankRanges(Found->second, WorkBudget);
}

std::optional<llvm::SmallVector<SymState::MaterializedRange, 8>>
SymState::materializedAbsoluteRanges(uint64_t &WorkBudget) const {
  if (!WorkBudget)
    return std::nullopt;
  --WorkBudget;
  return materializedBankRanges(AbsoluteMemory, WorkBudget);
}

bool SymState::hasMaterializedRegionRange(SymRef Base, uint64_t Offset,
                                          uint16_t Bytes) const {
  if (!Base || !Bytes)
    return false;
  const auto Found = Regions.find(Base.index());
  if (Found == Regions.end() ||
      (!hasModularCoordinates(*Ctx, Base) &&
       Offset > std::numeric_limits<uint64_t>::max() - (Bytes - 1)))
    return false;
  for (uint16_t I = 0; I < Bytes; ++I)
    if (!Found->second.Bytes.count(Offset + I))
      return false;
  return true;
}

std::optional<llvm::SmallVector<SymState::MaterializedRange, 8>>
SymState::materializedBankRanges(const Bank &B, uint64_t &WorkBudget) const {
  llvm::SmallVector<MaterializedRange, 8> Result;
  const auto &Bytes = B.Bytes;
  if (Bytes.size() > WorkBudget)
    return std::nullopt;
  WorkBudget -= Bytes.size();
  for (const auto &[Offset, View] : Bytes) {
    if (!Result.empty()) {
      auto &Last = Result.back();
      if (Last.Offset <= std::numeric_limits<uint64_t>::max() - Last.Bytes &&
          Offset == Last.Offset + Last.Bytes) {
        ++Last.Bytes;
        continue;
      }
    }
    Result.push_back({Offset, 1});
  }
  return Result;
}

SymState::MemoryKnowledge SymState::memoryKnowledge() const {
  MemoryKnowledge Out{Ctx, Order, {}};
  auto Export = [&](SymRef Base, const Bank &B) {
    auto It = B.Bytes.begin();
    while (It != B.Bytes.end()) {
      const uint64_t Offset = It->first;
      llvm::SmallVector<SymRef, 8> Bytes;
      while (It != B.Bytes.end() && Bytes.size() < 8 &&
             Offset <= std::numeric_limits<uint64_t>::max() - Bytes.size() &&
             It->first == Offset + Bytes.size()) {
        Bytes.push_back(
            Ctx->mkExtract(It->second.Value, It->second.Low, kByteBits));
        ++It;
      }
      Out.Facts.push_back({Base, Offset, joinBytes(Bytes)});
    }
  };
  Export({}, AbsoluteMemory);
  for (const auto &[Base, B] : Regions)
    Export(SymRef(Base), B);
  return Out;
}

bool SymState::restoreMemoryKnowledge(const MemoryKnowledge &Knowledge) {
  if (Knowledge.Owner != Ctx || Knowledge.Order != Order)
    return false;
  SymState Draft(*this);
  Draft.clobberMemory();
  // Historical load provenance remains true of unchanged value identities
  // even when memory knowledge is replaced. It is not used to seed facts.
  for (const auto &Fact : Knowledge.Facts) {
    if (!Fact.Value || Fact.Value.index() >= Ctx->numNodes() ||
        (Fact.Base && Fact.Base.index() >= Ctx->numNodes()))
      return false;
    const uint32_t Width = Ctx->width(Fact.Value);
    if (!Width || Width % kByteBits ||
        Width / kByteBits > std::numeric_limits<uint16_t>::max())
      return false;
    const uint16_t Bytes = uint16_t(Width / kByteBits);
    // A rebind may make a base absolute or expose a constant displacement.
    // Canonical region selection remains owned by locate(), as for loads.
    Location Where = Fact.Base ? Draft.locate(Fact.Base) : Location{};
    if (!Where.Base &&
        Fact.Offset > std::numeric_limits<uint64_t>::max() - Where.Offset)
      return false;
    Where.Offset += Fact.Offset;
    if (!validBankWrite(*Ctx, Where.Base, Where.Offset, Fact.Value))
      return false;
    Bank &B = Draft.bankFor(Where);
    for (uint16_t I = 0; I < Bytes; ++I) {
      const uint32_t Low = Order == llvm::endianness::little
                               ? uint32_t(I) * kByteBits
                               : Width - (uint32_t(I) + 1) * kByteBits;
      auto Value = Ctx->mkExtract(Fact.Value, Low, kByteBits);
      if (auto Found = B.Bytes.find(Where.Offset + I);
          Found != B.Bytes.end() &&
          Ctx->mkExtract(Found->second.Value, Found->second.Low, kByteBits) !=
              Value)
        return false;
    }
    if (!Draft.writeBank(B, Where.Offset, Fact.Value))
      return false;
  }
  *this = std::move(Draft);
  return true;
}

llvm::ArrayRef<SymState::LoadOrigin> SymState::loadOrigins(SymRef Value) const {
  auto It = LoadOrigins.find(Value.index());
  return It == LoadOrigins.end() ? llvm::ArrayRef<LoadOrigin>()
                                 : llvm::ArrayRef<LoadOrigin>(It->second);
}

const SymState::LoadOrigin *SymState::loadOrigin(SymRef Value) const {
  llvm::ArrayRef<LoadOrigin> Origins = loadOrigins(Value);
  return Origins.size() == 1 ? &Origins.front() : nullptr;
}

llvm::ArrayRef<SymState::MemoryInputOrigin>
SymState::memoryInputOrigins(SymRef Value) const {
  auto It = MemoryInputOrigins.find(Value.index());
  return It == MemoryInputOrigins.end()
             ? llvm::ArrayRef<MemoryInputOrigin>()
             : llvm::ArrayRef<MemoryInputOrigin>(It->second);
}

void SymState::recordMemoryInputOrigin(SymRef Value, MemoryInputOrigin Origin) {
  auto &Origins = MemoryInputOrigins[Value.index()];
  const auto Less = [](const MemoryInputOrigin &Left,
                       const MemoryInputOrigin &Right) {
    if (Left.Base != Right.Base)
      return Left.Base < Right.Base;
    if (Left.Offset != Right.Offset)
      return Left.Offset < Right.Offset;
    return Left.Bytes < Right.Bytes;
  };
  auto It = std::lower_bound(Origins.begin(), Origins.end(), Origin, Less);
  if (It == Origins.end() || *It != Origin)
    Origins.insert(It, Origin);
}

void SymState::recordLoadOrigin(SymRef Value, LoadOrigin Origin) {
  llvm::SmallVector<LoadOrigin, 1> &Origins = LoadOrigins[Value.index()];
  auto Less = [](const LoadOrigin &Left, const LoadOrigin &Right) {
    return Left.Address != Right.Address ? Left.Address < Right.Address
                                         : Left.Bytes < Right.Bytes;
  };
  auto It = std::lower_bound(Origins.begin(), Origins.end(), Origin, Less);
  if (It == Origins.end() || *It != Origin)
    Origins.insert(It, Origin);
}

SymRef SymState::load(SymRef Addr, uint16_t Bytes) {
  if (!Addr.isValid() || Bytes == 0)
    return {};

  Location Where = locate(Addr);
  SymRef Value = !Where.Base.isValid()
                     ? readBank(AbsoluteMemory, Where.Offset, Bytes)
                     : readBank(bankFor(Where), Where.Offset, Bytes);
  if (!Value.isValid())
    return {};
  // Keep what it was reading.  The value is only a name, so without this the
  // address is lost exactly when it becomes the interesting part.
  recordLoadOrigin(Value, LoadOrigin{Addr, Bytes});
  return Value;
}

bool SymState::store(SymRef Addr, SymRef Value) {
  if (!Addr.isValid() || !Value.isValid()) {
    clobberMemory();
    return false;
  }
  Location Where = locate(Addr);
  if (!Where.Base.isValid()) {
    // One number cannot be another, so absolute memory keeps everything it
    // held; but nothing rules out this address being where some pointer
    // pointed, so what was reached through a pointer is given up.
    if (!writeBank(AbsoluteMemory, Where.Offset, Value)) {
      clobberMemory();
      return false;
    }
    forgetRegions(SymRef());
    return true;
  }

  // Within the region the displacement says exactly which bytes changed, which
  // is what keeps two frame slots two frame slots.  Across regions nothing
  // says anything, so the rest is given up.
  if (!writeBank(bankFor(Where), Where.Offset, Value)) {
    clobberMemory();
    return false;
  }
  forget(AbsoluteMemory);
  forgetRegions(Where.Base);
  MemoryClobbered = true;
  return false;
}

bool SymState::store(SymRef Addr, SymRef Value,
                     SymStorePreservation &Preservation) {
  Preservation.WorkUsed = 0;
  Preservation.Status = SymStorePreservationStatus::Invalid;
  const auto ValidRef = [&](SymRef Ref) {
    return Ref && Ref.index() < Ctx->numNodes();
  };
  const auto ValidBase = [&](SymRef Base) {
    return !Base ||
           (ValidRef(Base) && Ctx->width(Base) == 64 && !Ctx->isConst(Base));
  };
  if (!ValidRef(Addr) || Ctx->width(Addr) != 64 || !ValidRef(Value) ||
      !ValidBase(Preservation.Base) || Preservation.Begin >= Preservation.End)
    return false;
  const Location Where = locate(Addr);
  if (Where.Base == Preservation.Base ||
      !validBankWrite(*Ctx, Where.Base, Where.Offset, Value))
    return false;

  const auto Charge = [&](uint64_t Count) {
    if (Count > Preservation.MaxWork - Preservation.WorkUsed) {
      Preservation.Status = SymStorePreservationStatus::BudgetExceeded;
      return false;
    }
    Preservation.WorkUsed += Count;
    return true;
  };
  // Bound the complete descriptor walk before dereferencing any extra range.
  // The old single-range path pays no new work.
  if (!Charge(Preservation.AdditionalRanges.size()))
    return false;
  uint64_t PreviousLast = static_cast<uint64_t>(Preservation.End) - 1;
  bool OrderedRanges =
      static_cast<uint64_t>(Preservation.Begin) <= PreviousLast;
  for (const auto &Range : Preservation.AdditionalRanges) {
    if (Range.Begin >= Range.End)
      return false;
    const auto First = static_cast<uint64_t>(Range.Begin);
    const auto Last = static_cast<uint64_t>(Range.End) - 1;
    OrderedRanges &= First <= Last && PreviousLast < First;
    PreviousLast = Last;
  }
  if (!Charge(Preservation.AdditionalRegions.size()))
    return false;
  for (const auto &Region : Preservation.AdditionalRegions)
    if (!ValidBase(Region.Base) || Region.Begin >= Region.End ||
        Region.Base == Where.Base)
      return false;

  struct SavedByte {
    Bank *Storage;
    uint64_t Offset;
    ByteView Byte;
  };
  llvm::SmallVector<SavedByte, 32> Saved;
  const auto SaveRegion = [&](SymRef Base, int64_t Lower, int64_t Upper,
                              llvm::ArrayRef<SymStorePreservationRange> More) {
    Bank *Storage = &AbsoluteMemory;
    if (Base) {
      auto Found = Regions.find(Base.index());
      if (Found == Regions.end())
        return true;
      Storage = &Found->second;
    }
    const uint64_t Begin = static_cast<uint64_t>(Lower);
    const uint64_t Size = static_cast<uint64_t>(Upper) - Begin;
    // Already ordered, nonwrapping ranges and byte keys share one cursor.
    // Other modular unions retain their original membership semantics.
    if (!More.empty() && OrderedRanges) {
      uint64_t First = Begin;
      uint64_t Last = static_cast<uint64_t>(Upper) - 1;
      size_t Next = 0;
      for (const auto &[Offset, Byte] : Storage->Bytes) {
        if (!Charge(1))
          return false;
        while (Offset > Last && Next != More.size()) {
          if (!Charge(1))
            return false;
          const auto &Range = More[Next++];
          First = static_cast<uint64_t>(Range.Begin);
          Last = static_cast<uint64_t>(Range.End) - 1;
        }
        if (Offset >= First && Offset <= Last) {
          if (!Charge(2))
            return false;
          Saved.push_back({Storage, Offset, Byte});
        }
      }
      return true;
    }
    for (const auto &[Offset, Byte] : Storage->Bytes) {
      if (!Charge(1))
        return false;
      bool Protected = Offset - Begin < Size;
      if (!Protected)
        for (const auto &Range : More) {
          if (!Charge(1))
            return false;
          if (Offset - static_cast<uint64_t>(Range.Begin) <
              static_cast<uint64_t>(Range.End) -
                  static_cast<uint64_t>(Range.Begin)) {
            Protected = true;
            break;
          }
        }
      if (Protected) {
        if (!Charge(2))
          return false;
        Saved.push_back({Storage, Offset, Byte});
      }
    }
    return true;
  };
  if (!SaveRegion(Preservation.Base, Preservation.Begin, Preservation.End,
                  Preservation.AdditionalRanges))
    return false;
  for (const auto &Region : Preservation.AdditionalRegions)
    if (!SaveRegion(Region.Base, Region.Begin, Region.End, {}))
      return false;

  const bool Absolute = store(Addr, Value);
  // Restore only known bytes. The real STORE owns the new epochs, unknown
  // defaults, other banks and provenance; rolling back a whole bank would
  // incorrectly reintroduce untouched entry-memory identities outside bounds.
  for (const auto &Entry : Saved)
    Entry.Storage->Bytes.emplace(Entry.Offset, Entry.Byte);
  Preservation.Status = SymStorePreservationStatus::Applied;
  return Absolute;
}

bool SymState::storeConditional(SymRef Addr, SymRef Value,
                                SymConditionalStore &Control) {
  Control.WorkUsed = 0;
  Control.Status = SymConditionalStoreStatus::Invalid;
  const auto ValidRef = [&](SymRef Ref) {
    return Ref && Ref.index() < Ctx->numNodes();
  };
  if (!ValidRef(Addr) || Ctx->width(Addr) != 64 || !ValidRef(Value))
    return false;
  const Location Where = locate(Addr);
  if (!validBankWrite(*Ctx, Where.Base, Where.Offset, Value))
    return false;
  const uint32_t Width = Ctx->width(Value);
  const uint64_t Bytes = Width / kByteBits;
  const auto Charge = [&](uint64_t Work) {
    if (Work > Control.MaxWork - Control.WorkUsed) {
      Control.Status = SymConditionalStoreStatus::BudgetExceeded;
      return false;
    }
    Control.WorkUsed += Work;
    return true;
  };
  if (!Charge(Bytes))
    return false;

  struct PlannedByte {
    Bank *Storage;
    SymRef Base;
    uint64_t Offset;
    ByteView Byte;
  };
  llvm::SmallVector<PlannedByte, 32> Plan;
  const auto Inspect = [&](SymRef Base, Bank &Storage) {
    if (!Charge(1))
      return false;
    if (Base == Where.Base || Storage.Bytes.empty())
      return true;
    if (Base && (!ValidRef(Base) || Ctx->width(Base) != 64))
      return false;
    for (const auto &[Offset, Byte] : Storage.Bytes) {
      if (!Charge(Bytes + 3))
        return false;
      Plan.push_back({&Storage, Base, Offset, Byte});
    }
    return true;
  };
  if (!Inspect({}, AbsoluteMemory))
    return false;
  for (auto &[Base, Storage] : Regions)
    if (!Inspect(SymRef(Base), Storage))
      return false;

  // Every charge and candidate validation has succeeded. Nothing below reads
  // missing memory or reconstructs historical facts.
  llvm::SmallVector<std::pair<SymRef, SymRef>, 8> WriteBytes;
  for (uint64_t I = 0; I != Bytes; ++I) {
    const auto Address = Ctx->mkAdd(Addr, Ctx->mkConst(64, I));
    const uint32_t Low = Order == llvm::endianness::little
                             ? uint32_t(I) * kByteBits
                             : Width - (uint32_t(I) + 1) * kByteBits;
    WriteBytes.emplace_back(Address, Ctx->mkExtract(Value, Low, kByteBits));
  }
  for (auto &Entry : Plan) {
    const auto Offset = Ctx->mkConst(64, Entry.Offset);
    const auto Address = Entry.Base ? Ctx->mkAdd(Entry.Base, Offset) : Offset;
    auto Updated = Ctx->mkExtract(Entry.Byte.Value, Entry.Byte.Low, kByteBits);
    for (const auto &[WriteAddress, Byte] : WriteBytes)
      Updated = Ctx->mkIte(Ctx->mkEq(Address, WriteAddress), Byte, Updated);
    Entry.Byte = {Updated};
  }
  const bool Absolute = store(Addr, Value);
  for (const auto &Entry : Plan)
    Entry.Storage->Bytes.emplace(Entry.Offset, Entry.Byte);
  Control.Status = SymConditionalStoreStatus::Applied;
  return Absolute;
}

//===----------------------------------------------------------------------===//
// Whole-state operations
//===----------------------------------------------------------------------===//

bool SymState::holdsSameBytes(const Bank &A, const Bank &B) const {
  // Two banks that agree byte for byte still differ if they are in different
  // epochs, because the next read of an untouched byte would name a different
  // unknown in each.
  if (A.Unknowns != B.Unknowns || A.Bytes.size() != B.Bytes.size())
    return false;
  auto Other = B.Bytes.begin();
  for (const auto &[Offset, View] : A.Bytes) {
    const auto &[OtherOffset, OtherView] = *Other++;
    if (Offset != OtherOffset)
      return false;
    if (View != OtherView &&
        Ctx->mkExtract(View.Value, View.Low, kByteBits) !=
            Ctx->mkExtract(OtherView.Value, OtherView.Low, kByteBits))
      return false;
  }
  return true;
}

bool SymState::mergeIdentical(const SymState &Other) {
  if (Ctx != Other.Ctx || Order != Other.Order ||
      X64MemoryAddressSpaceBases != Other.X64MemoryAddressSpaceBases ||
      MemoryClobbered != Other.MemoryClobbered ||
      UnseenRegions != Other.UnseenRegions)
    return false;
  if (!holdsSameBytes(Registers, Other.Registers) ||
      !holdsSameBytes(Temporaries, Other.Temporaries) ||
      !holdsSameBytes(AbsoluteMemory, Other.AbsoluteMemory))
    return false;
  if (Regions.size() != Other.Regions.size())
    return false;
  for (const auto &Entry : Regions) {
    auto It = Other.Regions.find(Entry.first);
    if (It == Other.Regions.end() || !holdsSameBytes(Entry.second, It->second))
      return false;
  }

  // Provenance is the one thing the two may legitimately differ on: the same
  // value can have been read through addresses spelled differently.  Preserve
  // every possibility; exact consumers reject a conflict, while may-dependence
  // consumers follow all of them.
  for (const auto &Entry : Other.LoadOrigins)
    for (const LoadOrigin &Origin : Entry.second)
      recordLoadOrigin(SymRef(Entry.first), Origin);
  for (const auto &Entry : Other.MemoryInputOrigins)
    for (const MemoryInputOrigin &Origin : Entry.second)
      recordMemoryInputOrigin(SymRef(Entry.first), Origin);
  return true;
}

void SymState::clobberRegistersExcept(
    llvm::ArrayRef<SymRegisterRange> PreservedRanges) {
  // A lifter's temporaries never live across an instruction, let alone a call,
  // so there is nothing there worth keeping either way.
  forget(Temporaries);

  // Materialise untouched bytes before replacing the default unknown set.  A
  // preserved register that had not been read yet still denotes its entry (or
  // previous-call) value after this call.
  for (const SymRegisterRange &Range : PreservedRanges)
    if (Range.Bytes != 0)
      readBank(Registers, Range.Offset, Range.Bytes);

  if (PreservedRanges.empty()) {
    forget(Registers);
    return;
  }

  Registers.Unknowns = std::make_shared<UnknownBytes>();
  for (auto It = Registers.Bytes.begin(); It != Registers.Bytes.end();) {
    bool Preserved = false;
    for (const SymRegisterRange &Range : PreservedRanges) {
      if (It->first >= Range.Offset && It->first - Range.Offset < Range.Bytes) {
        Preserved = true;
        break;
      }
    }
    It = Preserved ? std::next(It) : Registers.Bytes.erase(It);
  }
}

} // namespace neverd::symbolic
