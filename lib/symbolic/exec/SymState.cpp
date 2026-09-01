//===- SymState.cpp - Symbolic machine state ------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the byte-addressed store described in SymState.h.
///
/// Every read and write goes through the same split-and-join: a word is taken
/// apart into bytes on the way in and put back together on the way out.  It
/// costs an extract and a concatenation per byte, which the expression
/// builders fold away again whenever the write and the read line up — the
/// common case by far.  What it buys is that partial overwrites, sub-register
/// writes and unaligned memory all work without any of them being a case.
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

namespace {
constexpr uint32_t kByteBits = 8;

/// Widest address the region split works on.  A displacement is kept as a
/// 64-bit key, so a wider address space cannot be indexed that way and each of
/// its addresses becomes a region of its own instead.
constexpr uint32_t kMaxAddressBits = 64;

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

SymRef SymState::byteAt(Bank &B, uint64_t Offset) {
  auto It = B.Bytes.find(Offset);
  if (It != B.Bytes.end())
    return It->second;

  SymRef Input;
  if (B.Unknowns) {
    auto Unknown = B.Unknowns->Values.find(Offset);
    if (Unknown == B.Unknowns->Values.end()) {
      Input = Ctx->mkFreshVar(kByteBits, byteName(B.Name, Offset) + "$");
      B.Unknowns->Values.emplace(Offset, Input);
    } else {
      Input = Unknown->second;
    }
  } else {
    // Before anything was forgotten, name an untouched location for where it
    // is.  Once it has been, the shared epoch above keeps copied states equal
    // while independently forgetful forks receive different values.
    const std::string Name = byteName(B.Name, Offset);
    Input = B.InputKind
                ? Ctx->mkInputVar(
                      Name, kByteBits,
                      SymInputOrigin{*B.InputKind, Offset, 1, B.Epoch}, Order)
                : Ctx->mkVar(Name, kByteBits);
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
  if (Bytes == 0 || Offset > std::numeric_limits<uint64_t>::max() - (Bytes - 1))
    return {};

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
    }
    if (!writeBank(B, Offset, Input))
      return {};
    if (B.Unknowns)
      for (uint16_t I = 0; I < Bytes; ++I)
        B.Unknowns->Values.emplace(Offset + I, B.Bytes.at(Offset + I));
    return Input;
  }

  llvm::SmallVector<SymRef, 8> Parts;
  Parts.reserve(Bytes);
  for (uint16_t I = 0; I < Bytes; ++I)
    Parts.push_back(byteAt(B, Offset + I));
  return joinBytes(Parts);
}

bool SymState::writeBank(Bank &B, uint64_t Offset, SymRef Value) {
  if (!Value.isValid())
    return false;
  const uint32_t Width = Ctx->width(Value);
  if (Width == 0 || Width % kByteBits != 0)
    return false;
  const uint32_t ByteCount = Width / kByteBits;
  if (ByteCount > std::numeric_limits<uint16_t>::max() ||
      Offset > std::numeric_limits<uint64_t>::max() - (ByteCount - 1))
    return false;
  const uint16_t Bytes = static_cast<uint16_t>(ByteCount);

  for (uint16_t I = 0; I < Bytes; ++I) {
    // Bit position of byte I of the address range, which is the low end of the
    // word on a little-endian target and the high end on a big-endian one.
    const uint32_t Low = Order == llvm::endianness::little
                             ? uint32_t(I) * kByteBits
                             : Width - (uint32_t(I) + 1) * kByteBits;
    B.Bytes[Offset + I] = Ctx->mkExtract(Value, Low, kByteBits);
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
  return Regions.emplace(Where.Base.index(), std::move(Fresh)).first->second;
}

void SymState::forgetRegions(SymRef Except) {
  for (auto &Entry : Regions)
    if (!Except.isValid() || Entry.first != Except.index())
      forget(Entry.second);
}

void SymState::clobberMemory() {
  forget(AbsoluteMemory);
  forgetRegions(SymRef());
  MemoryClobbered = true;
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

//===----------------------------------------------------------------------===//
// Whole-state operations
//===----------------------------------------------------------------------===//

bool SymState::holdsSameBytes(const Bank &A, const Bank &B) {
  // Two banks that agree byte for byte still differ if they are in different
  // epochs, because the next read of an untouched byte would name a different
  // unknown in each.
  return A.Unknowns == B.Unknowns && A.Bytes == B.Bytes;
}

bool SymState::mergeIdentical(const SymState &Other) {
  if (Order != Other.Order || MemoryClobbered != Other.MemoryClobbered)
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
