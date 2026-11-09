//===- PatchStorage.cpp - Replacement storage layout ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/PatchStorage.h"

#include "neverd/ArchSupport.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

namespace neverd {
namespace {
#define PATCH_STORAGE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "PatchStorage.def"
} // namespace

uint64_t sourcePatchStorageAlignment(Arch Architecture, BinaryFormat Format) {
  if (Format == BinaryFormat::ELF)
    return elfPageSize(Architecture);
  if (Format == BinaryFormat::MachO)
    return machoPageSize(Architecture);
  return 64 * 1024;
}

llvm::Expected<PatchStorageLayout>
validatePatchStorageLayout(const CompiledImage &Image, uint64_t PageAlignment) {
  auto Fail = [](const llvm::Twine &Detail) {
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   kPrefix + Detail);
  };
  if (!Image.Success || !llvm::isPowerOf2_64(PageAlignment) ||
      Image.Bytes.size() > InvalidVA - Image.BaseVA)
    return Fail(kImage);
  std::optional<uint64_t> Writable;
  std::optional<uint64_t> Relro, Mutable;
  uint64_t RelroEnd = 0;
  uint64_t ReadOnlyEnd = 0;
  for (const auto &Section : Image.Sections) {
    if (!Section.IsAllocated || Section.Size == 0)
      continue;
    if (llvm::StringRef(Section.Name).starts_with(kELFRelroPrefix))
      return Fail(kELFRelro);
    const bool Write =
        Section.Kind == llvm::mc_rewrite::RewriteSectionKind::WritableData ||
        Section.Kind == llvm::mc_rewrite::RewriteSectionKind::UninitializedData;
    if (Section.ReadOnlyAfterFixups &&
        (Image.Format != BinaryFormat::MachO || !Write || !Section.IsInImage))
      return Fail(kRelro);
    if (!Section.IsInImage) {
      if (Write)
        return Fail(kExternal);
      continue;
    }
    if (!rangeInBounds(Section.Offset, Section.Size, Image.Bytes.size()) ||
        Section.VA != Image.BaseVA + Section.Offset)
      return Fail(kExtent);
    if (Write) {
      Writable = std::min(Writable.value_or(Section.Offset), Section.Offset);
      if (Section.ReadOnlyAfterFixups) {
        Relro = std::min(Relro.value_or(Section.Offset), Section.Offset);
        RelroEnd = std::max(RelroEnd, Section.Offset + Section.Size);
      } else
        Mutable = std::min(Mutable.value_or(Section.Offset), Section.Offset);
    } else
      ReadOnlyEnd = std::max(ReadOnlyEnd, Section.Offset + Section.Size);
  }
  if (!Writable)
    return PatchStorageLayout{0, ReadOnlyEnd};
  if (*Writable == 0 || *Writable < ReadOnlyEnd ||
      (Image.BaseVA + *Writable) % PageAlignment != 0)
    return Fail(kSplit);
  uint64_t RelroSize = 0;
  if (Relro) {
    if (*Relro != *Writable || Image.BaseVA % PageAlignment ||
        RelroEnd > UINT64_MAX - (PageAlignment - 1))
      return Fail(kRelro);
    const auto End = llvm::alignTo(RelroEnd, PageAlignment);
    if (Mutable && (*Mutable < End || *Mutable % PageAlignment))
      return Fail(kRelro);
    RelroSize = Mutable.value_or(End) - *Relro;
  }
  return PatchStorageLayout{*Writable, ReadOnlyEnd, RelroSize};
}

} // namespace neverd
