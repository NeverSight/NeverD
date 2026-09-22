#include "neverd/loader/ObjC/ObjCClassGetterCalls.h"

#include "../MachO/SourceLocalCall.h"
#include "ObjCReceiverDeclarations.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/Endian.h"

namespace neverd {
namespace {
std::optional<SourceClassGetterCall> getter(const BinaryImage &Image,
                                            va_t Entry) {
  if (Entry % 4 || Entry > UINT64_MAX - 12)
    return std::nullopt;
  const auto Bytes = readImmutableCodeBytes(Image, Entry, 12);
  if (!Bytes)
    return std::nullopt;
  SourceClassGetterCall Proof;
  for (unsigned I = 0; I < 3; ++I)
    Proof.LeafWords[I] = llvm::support::endian::read32le(Bytes->data() + I * 4);
  const auto &Words = Proof.LeafWords;
  if ((Words[0] & 0x9f00001f) != 0x90000008 ||
      (Words[1] & 0xffc003ff) != 0xf9400100 || Words[2] != 0xd65f03c0 ||
      !sourceLocalLeafRange(Image, Entry, 12))
    return std::nullopt;
  const uint32_t Imm = ((Words[0] >> 29) & 3) | ((Words[0] >> 3) & 0x1ffffc);
  const int64_t Delta =
      (int64_t(Imm) - ((Imm & 0x100000) ? 0x200000 : 0)) * 4096;
  const va_t Base = Entry & ~va_t(0xfff);
  if ((Delta < 0 && Base < uint64_t(-Delta)) ||
      (Delta >= 0 && Base > UINT64_MAX - uint64_t(Delta)))
    return std::nullopt;
  const va_t Page = Base + Delta;
  const uint64_t Offset = ((Words[1] >> 10) & 0xfff) * 8;
  if (Page > UINT64_MAX - Offset)
    return std::nullopt;
  Proof.Slot = Page + Offset;
  if (!isImmutableImageClassImportSlot(Image, Proof.Slot))
    return std::nullopt;
  Proof.ClassName = Image.ObjCSourceReferences.at(Proof.Slot).Name;
  Proof.Module = Image.DyldBindSlots.at(Proof.Slot).Module;
  if (!objc::sdkClassImportProvider(Image.Arch, Proof.ClassName, Proof.Module))
    return std::nullopt;
  return Proof;
}
} // namespace

SourceClassGetterCalls sourceClassGetterCalls(const BinaryImage &Image,
                                              const LowFunc &Caller) {
  SourceClassGetterCalls Result;
  std::map<va_t, std::optional<SourceClassGetterCall>> Leaves;
  for (const auto &[Site, Word] : sourceLocalCalls(Image, Caller)) {
    auto [Found, Inserted] = Leaves.try_emplace(*Site.StaticTarget);
    if (Inserted)
      Found->second = getter(Image, *Site.StaticTarget);
    if (!Found->second)
      continue;
    auto Proof = *Found->second;
    Proof.Caller = Caller.Entry;
    Proof.Site = Site;
    Proof.CallWord = Word;
    Result.emplace(Site, std::move(Proof));
  }
  return Result;
}

bool validateSourceClassGetterCalls(const BinaryImage &Image,
                                    const LowFunc &Caller,
                                    const SourceClassGetterCalls &Receipts) {
  return sourceClassGetterCalls(Image, Caller) == Receipts;
}
} // namespace neverd
