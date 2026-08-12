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
  return Ctx->mkVar((Prefix + llvm::Twine('$') + llvm::Twine(FreshCounter++)).str(),
                   Width);
}

SymRef SymState::byteAt(SymSpace Space, uint64_t Offset) {
  std::map<uint64_t, SymRef> &Bytes = spaceMap(Space);
  auto It = Bytes.find(Offset);
  if (It != Bytes.end())
    return It->second;

  // Named for where it is rather than merely numbered, so that an expression
  // built out of never-written locations still says which ones.
  SymRef Input = Ctx->mkVar(
      (llvm::Twine(spaceName(Space)) + "$" + llvm::Twine(Offset)).str(),
      kByteBits);
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
    const uint32_t Low =
        Order == llvm::endianness::little
            ? uint32_t(I) * kByteBits
            : Width - (uint32_t(I) + 1) * kByteBits;
    Store[Offset + I] = Ctx->mkExtract(Value, Low, kByteBits);
  }
}

void SymState::clobberMemory() {
  Memory.clear();
  MemoryClobbered = true;
}

const SymState::LoadOrigin *SymState::loadOrigin(SymRef Value) const {
  auto It = LoadOrigins.find(Value.index());
  return It == LoadOrigins.end() ? nullptr : &It->second;
}

SymRef SymState::load(SymRef Addr, uint16_t Bytes) {
  assert(Bytes > 0);
  const uint32_t Width = uint32_t(Bytes) * kByteBits;

  // An address the engine cannot pin down names no particular byte, and once
  // something has been written through such an address no byte can be trusted
  // to still hold what it did.
  std::optional<llvm::APInt> Concrete = Ctx->asConst(Addr);
  if (!Concrete || MemoryClobbered) {
    SymRef Value = freshInput("load", Width);
    // Keep what it was reading.  The value is only a name, so without this the
    // address is lost exactly when it becomes the interesting part.
    LoadOrigins.emplace(Value.index(), LoadOrigin{Addr, Bytes});
    return Value;
  }

  const uint64_t Base = Concrete->getZExtValue();
  llvm::SmallVector<SymRef, 8> Parts;
  Parts.reserve(Bytes);
  for (uint16_t I = 0; I < Bytes; ++I)
    Parts.push_back(byteAt(SymSpace::Memory, Base + I));
  return joinBytes(Parts);
}

void SymState::store(SymRef Addr, SymRef Value) {
  std::optional<llvm::APInt> Concrete = Ctx->asConst(Addr);
  if (!Concrete) {
    clobberMemory();
    return;
  }
  if (MemoryClobbered)
    return;
  write(SymSpace::Memory, Concrete->getZExtValue(), Value);
}

void SymState::clobberRegistersExcept(
    llvm::ArrayRef<uint64_t> PreservedOffsets) {
  // A lifter's temporaries never live across an instruction, let alone a call,
  // so there is nothing there worth keeping either way.
  Temporaries.clear();

  if (PreservedOffsets.empty()) {
    Registers.clear();
    return;
  }
  for (auto It = Registers.begin(); It != Registers.end();) {
    // Offsets name bytes, and a preserved register covers the bytes from its
    // own offset onwards; without a width table the best that can be said is
    // that the byte belongs to a preserved register when it is at or after one
    // of the given offsets and before the next recorded byte of another.
    bool Preserved = false;
    for (uint64_t Base : PreservedOffsets) {
      if (It->first >= Base && It->first < Base + 8) {
        Preserved = true;
        break;
      }
    }
    It = Preserved ? std::next(It) : Registers.erase(It);
  }
}

} // namespace neverd::symbolic
