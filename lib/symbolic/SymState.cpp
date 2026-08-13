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
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymState.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <cassert>

namespace neverd::symbolic {

namespace {
constexpr uint32_t kByteBits = 8;

std::optional<uint64_t> concreteAddress(const SymContext &Ctx, SymRef Addr) {
  std::optional<llvm::APInt> Value = Ctx.asConst(Addr);
  if (!Value || Value->getActiveBits() > 64)
    return std::nullopt;
  return Value->getZExtValue();
}
} // namespace

std::map<uint64_t, SymRef> &SymState::spaceMap(SymSpace Space) {
  switch (Space) {
  case SymSpace::Register:
    return Registers;
  case SymSpace::Temporary:
    return Temporaries;
  case SymSpace::Memory:
    return Memory;
  }
  llvm_unreachable("unhandled symbolic space");
}

const std::map<uint64_t, SymRef> &SymState::spaceMap(SymSpace Space) const {
  return const_cast<SymState *>(this)->spaceMap(Space);
}

std::shared_ptr<SymState::UnknownBytes> &
SymState::unknownBytes(SymSpace Space) {
  switch (Space) {
  case SymSpace::Register:
    return RegisterUnknowns;
  case SymSpace::Temporary:
    return TemporaryUnknowns;
  case SymSpace::Memory:
    return MemoryUnknowns;
  }
  llvm_unreachable("unhandled symbolic space");
}

const char *SymState::spaceName(SymSpace Space) {
  switch (Space) {
  case SymSpace::Register:
    return "reg";
  case SymSpace::Temporary:
    return "tmp";
  case SymSpace::Memory:
    return "mem";
  }
  llvm_unreachable("unhandled symbolic space");
}

SymRef SymState::freshInput(llvm::StringRef Prefix, uint32_t Width) {
  const std::string NamedPrefix = (Prefix + llvm::Twine('$')).str();
  return Ctx->mkFreshVar(Width, NamedPrefix);
}

SymRef SymState::byteAt(SymSpace Space, uint64_t Offset) {
  std::map<uint64_t, SymRef> &Bytes = spaceMap(Space);
  auto It = Bytes.find(Offset);
  if (It != Bytes.end())
    return It->second;

  SymRef Input;
  std::shared_ptr<UnknownBytes> &Unknowns = unknownBytes(Space);
  if (Unknowns) {
    auto Unknown = Unknowns->Values.find(Offset);
    if (Unknown == Unknowns->Values.end()) {
      const std::string Prefix =
          (llvm::Twine(spaceName(Space)) + "$" + llvm::Twine(Offset) + "$")
              .str();
      Input = Ctx->mkFreshVar(kByteBits, Prefix);
      Unknowns->Values.emplace(Offset, Input);
    } else {
      Input = Unknown->second;
    }
  } else {
    // Before any clobber, name an untouched location for where it is.  Once a
    // clobber has happened, the shared event above keeps copied states equal
    // while independently clobbered forks receive different values.
    Input = Ctx->mkVar(
        (llvm::Twine(spaceName(Space)) + "$" + llvm::Twine(Offset)).str(),
        kByteBits);
  }
  Bytes.emplace(Offset, Input);
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

SymRef SymState::read(SymSpace Space, uint64_t Offset, uint16_t Bytes) {
  assert(Bytes > 0 && "a read of no bytes has no value");

  // Keep an untouched word as one input rather than eagerly turning it into a
  // concatenation of unrelated byte variables.  The write records its byte
  // views, so every later overlapping read still observes exact aliasing.
  std::map<uint64_t, SymRef> &Known = spaceMap(Space);
  std::shared_ptr<UnknownBytes> &Unknowns = unknownBytes(Space);
  bool HasMaterialisedByte = false;
  for (uint16_t I = 0; I < Bytes; ++I)
    HasMaterialisedByte |=
        Known.count(Offset + I) != 0 ||
        (Unknowns && Unknowns->Values.count(Offset + I) != 0);
  if (!HasMaterialisedByte) {
    const std::string Name =
        (llvm::Twine(spaceName(Space)) + "$" + llvm::Twine(Offset)).str();
    SymRef Input =
        Unknowns ? Ctx->mkFreshVar(uint32_t(Bytes) * kByteBits, Name + "$")
                 : Ctx->mkVar(Name, uint32_t(Bytes) * kByteBits);
    write(Space, Offset, Input);
    if (Unknowns)
      for (uint16_t I = 0; I < Bytes; ++I)
        Unknowns->Values.emplace(Offset + I, Known.at(Offset + I));
    return Input;
  }

  llvm::SmallVector<SymRef, 8> Parts;
  Parts.reserve(Bytes);
  for (uint16_t I = 0; I < Bytes; ++I)
    Parts.push_back(byteAt(Space, Offset + I));
  return joinBytes(Parts);
}

void SymState::write(SymSpace Space, uint64_t Offset, SymRef Value) {
  const uint32_t Width = Ctx->width(Value);
  assert(Width % kByteBits == 0 && "a stored value must be whole bytes");
  const uint16_t Bytes = static_cast<uint16_t>(Width / kByteBits);

  std::map<uint64_t, SymRef> &Store = spaceMap(Space);
  for (uint16_t I = 0; I < Bytes; ++I) {
    // Bit position of byte I of the address range, which is the low end of the
    // word on a little-endian target and the high end on a big-endian one.
    const uint32_t Low = Order == llvm::endianness::little
                             ? uint32_t(I) * kByteBits
                             : Width - (uint32_t(I) + 1) * kByteBits;
    Store[Offset + I] = Ctx->mkExtract(Value, Low, kByteBits);
  }
}

void SymState::clobberMemory() {
  Memory.clear();
  MemoryUnknowns = std::make_shared<UnknownBytes>();
  SymbolicLoads = std::make_shared<SymbolicLoadValues>();
  MemoryClobbered = true;
}

const SymState::LoadOrigin *SymState::loadOrigin(SymRef Value) const {
  auto It = LoadOrigins.find(Value.index());
  return It == LoadOrigins.end() ? nullptr : &It->second;
}

SymRef SymState::load(SymRef Addr, uint16_t Bytes) {
  assert(Bytes > 0);
  const uint32_t Width = uint32_t(Bytes) * kByteBits;

  // An address the engine cannot represent in its 64-bit byte map names no
  // particular byte.  Keep the complete expression as provenance.
  std::optional<uint64_t> Concrete = concreteAddress(*Ctx, Addr);
  if (!Concrete) {
    llvm::SmallVector<SymRef, 8> Parts;
    Parts.reserve(Bytes);
    const uint32_t AddressWidth = Ctx->width(Addr);
    for (uint16_t I = 0; I < Bytes; ++I) {
      SymRef ByteAddr =
          I == 0 ? Addr
                 : Ctx->mkAdd(Addr, Ctx->mkConst(AddressWidth, uint64_t(I)));
      auto It = SymbolicLoads->Bytes.find(ByteAddr.index());
      if (It == SymbolicLoads->Bytes.end()) {
        SymRef Byte = freshInput("load", kByteBits);
        SymbolicLoads->Bytes.emplace(ByteAddr.index(), Byte);
        Parts.push_back(Byte);
      } else {
        Parts.push_back(It->second);
      }
    }
    SymRef Value = joinBytes(Parts);
    // Keep what it was reading.  The value is only a name, so without this the
    // address is lost exactly when it becomes the interesting part.
    LoadOrigins.emplace(Value.index(), LoadOrigin{Addr, Bytes});
    return Value;
  }

  return read(SymSpace::Memory, *Concrete, Bytes);
}

bool SymState::store(SymRef Addr, SymRef Value) {
  std::optional<uint64_t> Concrete = concreteAddress(*Ctx, Addr);
  if (!Concrete) {
    clobberMemory();
    return false;
  }
  // A known write establishes these bytes even when an earlier unknown write
  // forced the rest of memory into an unknown epoch.  It may alias any
  // unresolved address, so those cached reads belong to the previous epoch.
  SymbolicLoads = std::make_shared<SymbolicLoadValues>();
  write(SymSpace::Memory, *Concrete, Value);
  return true;
}

void SymState::clobberRegistersExcept(
    llvm::ArrayRef<SymRegisterRange> PreservedRanges) {
  // A lifter's temporaries never live across an instruction, let alone a call,
  // so there is nothing there worth keeping either way.
  Temporaries.clear();
  TemporaryUnknowns = std::make_shared<UnknownBytes>();

  // Materialise untouched bytes before replacing the default unknown set.  A
  // preserved register that had not been read yet still denotes its entry (or
  // previous-call) value after this call.
  for (const SymRegisterRange &Range : PreservedRanges)
    if (Range.Bytes != 0)
      read(SymSpace::Register, Range.Offset, Range.Bytes);

  RegisterUnknowns = std::make_shared<UnknownBytes>();

  if (PreservedRanges.empty()) {
    Registers.clear();
    return;
  }
  for (auto It = Registers.begin(); It != Registers.end();) {
    bool Preserved = false;
    for (const SymRegisterRange &Range : PreservedRanges) {
      if (It->first >= Range.Offset && It->first - Range.Offset < Range.Bytes) {
        Preserved = true;
        break;
      }
    }
    It = Preserved ? std::next(It) : Registers.erase(It);
  }
}

} // namespace neverd::symbolic
