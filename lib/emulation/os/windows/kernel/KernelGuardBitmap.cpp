//===- KernelGuardBitmap.cpp - Guest-owned kernel CFG bitmap ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest-owned kernel CFG bitmap.
///

#include "KernelGuardBitmap.h"

#include "KernelReferenceImage.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
llvm::Error invalid(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace
llvm::Error
KernelGuardBitmap::initialize(const KernelReferenceImage &Reference) {
  if (MappingSize || !Reference.debugger() ||
      !Reference.debugger()->GuardBitmapPointer || Reference.base() % 4096)
    return invalid("invalid kernel CFG bitmap initialization");
  Base = Reference.base();
  ImageSize = Reference.size();
  // Native 19041 code indexes qwords by VA >> 9, then tests VA >> 3.
  // Unsigned addition intentionally wraps the biased pointer into kernel VA.
  Bias = StoreBase - (Base >> 9) * 8;
  DataSize = ((ImageSize + 511) / 512) * 8;
  MappingSize = (DataSize + 4095) & ~uint64_t(4095);
  Bytes.resize(MappingSize);
  SuppressedExports.insert(Reference.debugger()->SuppressedExports.begin(),
                           Reference.debugger()->SuppressedExports.end());
  // Map only this reference image's bitmap span. Unmodeled address domains
  // have no fabricated all-valid/all-zero bitmap pages.
  if (auto E = Memory.map(StoreBase, MappingSize, Read))
    return E;
  for (uint64_t Target : Reference.debugger()->GuardTargets)
    if (auto E = mark(Target))
      return E;
  return Memory.writeBacking(StoreBase, Bytes);
}
llvm::Error KernelGuardBitmap::mark(uint64_t Address) {
  if (Address < Base || Address - Base >= ImageSize)
    return invalid("CFG target exceeds the reference bitmap domain");
  const uint64_t Offset = ((Address >> 9) - (Base >> 9)) * 8;
  const unsigned Bit = (Address >> 3) & 63;
  uint64_t Mask = uint64_t(1) << (Bit & ~1u);
  // A misaligned function grants the complete 16-byte slot, as tested by the
  // original kernel accessor. An aligned entry only sets the even bit.
  if (Address & 15)
    Mask |= Mask << 1;
  auto *Word = Bytes.data() + Offset;
  llvm::support::endian::write64le(Word, llvm::support::endian::read64le(Word) |
                                             Mask);
  return llvm::Error::success();
}
llvm::Error KernelGuardBitmap::resolveExport(uint64_t Address) {
  auto Entry = SuppressedExports.find(Address);
  if (Entry == SuppressedExports.end())
    return llvm::Error::success();
  if (auto E = mark(Address))
    return E;
  const uint64_t Offset = ((Address >> 9) - (Base >> 9)) * 8;
  if (auto E = Memory.writeBacking(StoreBase + Offset,
                                   llvm::ArrayRef(Bytes).slice(Offset, 8)))
    return E;
  SuppressedExports.erase(Entry);
  return llvm::Error::success();
}
llvm::Error KernelGuardBitmap::validateAccess(uint64_t Address, uint32_t Size,
                                              bool Write) const {
  if (Address >= StoreBase + MappingSize || Address + Size <= StoreBase)
    return llvm::Error::success();
  if (Write || Address < StoreBase || Size > DataSize ||
      Address - StoreBase > DataSize - Size)
    return invalid("access to read-only kernel CFG bitmap or its padding");
  return llvm::Error::success();
}
} // namespace neverd::emulation
