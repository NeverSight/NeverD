//===- ObjCMethodLists.cpp - Bounded Objective-C method records ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared method-list decoding for local and cache-backed metadata views.
//===----------------------------------------------------------------------===//

#include "ObjCMethodLists.h"

#include "ObjCRelativeLists.h"
#include "ObjCRuntimeData.h"

#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/DyldSharedCache.h"

#include <iterator>

namespace neverd::objc {
namespace {
#define NEVERD_OBJC_METHOD_VALUE(Type, Name, Value)                            \
  inline constexpr Type Name = Value;
#define NEVERD_OBJC_METHOD_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "ObjCMethodLists.def"

std::optional<std::vector<MethodRecord>>
readOneMethodList(const BinaryImage &Image, va_t Address, size_t &Remaining,
                  std::string &Diagnostic) {
  Diagnostic.clear();
  const RuntimeData Data(Image, PointerView::MetadataTarget);
  const auto Flags = Data.u32(Address);
  const auto Count = Address <= InvalidVA - kCountOffset
                         ? Data.u32(Address + kCountOffset)
                         : std::nullopt;
  if (!Flags || !Count) {
    Diagnostic = kTruncatedList;
    return std::nullopt;
  }
  // Empty cache lists can contain only the two runtime fixup bits. Their
  // zero entry size never advances an iterator because the count is zero.
  if (!*Count && !(*Flags & ~kFixupFlags))
    return std::vector<MethodRecord>();
  const bool Small = (*Flags & kSmall) != 0;
  const bool DirectSelectors = (*Flags & kDirectSelectors) != 0;
  const uint32_t EntrySize = *Flags & kEntrySizeMask;
  if ((*Flags & kUnknownFlags) || (DirectSelectors && !Small) ||
      EntrySize < (Small ? kSmallEntryBytes : kBigEntryBytes) ||
      EntrySize > ::neverd::limits::loader::
                      kObjCMethodListsReadMethodListEntrySizeLimit ||
      *Count > Remaining ||
      !Data.bytes(Address,
                  uint64_t(kHeaderBytes) + uint64_t(*Count) * EntrySize)) {
    Diagnostic = kInvalidList;
    return std::nullopt;
  }
  const bool CachedSmall = Small && Image.MachOSharedCache &&
                           Image.MachOSharedCache->mappingFor(Address);
  std::optional<va_t> SelectorBase;
  if (CachedSmall) {
    auto Layout = Image.MachOSharedCache->objcSelectorLayout();
    if (!Layout) {
      Diagnostic = llvm::toString(Layout.takeError());
      return std::nullopt;
    }
    if (!*Layout) {
      Diagnostic = kMissingSelectorBase;
      return std::nullopt;
    }
    if ((*Layout)->Encoding ==
        macho_loader::DyldCacheSelectorEncoding::BaseRelative)
      SelectorBase = (*Layout)->Base;
  }
  auto Relative = [&](va_t Slot, bool Nullable = false,
                      std::optional<va_t> Origin =
                          std::nullopt) -> std::optional<va_t> {
    const auto Value = Data.u32(Slot);
    if (!Value)
      return std::nullopt;
    if (Nullable && !*Value)
      return 0;
    const int64_t Offset = static_cast<int32_t>(*Value);
    const va_t Base = Origin.value_or(Slot);
    if ((Offset < 0 && Base < uint64_t(-Offset)) ||
        (Offset > 0 && Base > InvalidVA - uint64_t(Offset)))
      return std::nullopt;
    return Offset < 0 ? Base - uint64_t(-Offset) : Base + uint64_t(Offset);
  };
  Remaining -= *Count;
  std::vector<MethodRecord> Records;
  Records.reserve(*Count);
  for (uint32_t I = 0; I < *Count; ++I) {
    MethodRecord Record;
    Record.Address = Address + kHeaderBytes + uint64_t(I) * EntrySize;
    auto Selector = Small ? Relative(Record.Address, false, SelectorBase)
                          : Data.localPointer(Record.Address);
    if (Small && !DirectSelectors && !CachedSmall && Selector)
      Selector = Data.localPointer(*Selector);
    const auto Types =
        Small ? Relative(Record.Address + kRelativeTypesOffset)
              : Data.localPointer(Record.Address + kAbsoluteTypesOffset);
    Record.Implementation =
        Small ? Relative(Record.Address + kRelativeIMPOffset, true)
              : Data.localPointer(Record.Address + kAbsoluteIMPOffset);
    if (Selector)
      Record.Selector = Data.string(*Selector).value_or("");
    if (Types)
      Record.TypeEncoding = Data.string(*Types).value_or("");
    Records.push_back(std::move(Record));
  }
  return Records;
}
} // namespace

std::optional<std::vector<MethodRecord>>
readMethodList(const BinaryImage &Image, va_t Address, size_t &Remaining,
               std::string &Diagnostic) {
  Diagnostic.clear();
  auto References = readListReferences(Image, Address, Remaining);
  if (!References) {
    Diagnostic = llvm::toString(References.takeError());
    return std::nullopt;
  }
  std::vector<MethodRecord> Records;
  for (const auto &Reference : *References) {
    auto Part =
        readOneMethodList(Image, Reference.Address, Remaining, Diagnostic);
    if (!Part)
      return std::nullopt;
    for (auto &Record : *Part)
      Record.CacheOrigin = Reference.CacheOrigin;
    Records.insert(Records.end(), std::make_move_iterator(Part->begin()),
                   std::make_move_iterator(Part->end()));
  }
  return Records;
}

} // namespace neverd::objc
