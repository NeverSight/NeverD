//===- PEImports.cpp - Bounded ordinary PE import metadata
//-----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/Limits.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace neverd::coff_loader {
namespace {
using namespace llvm::COFF;
using llvm::support::endian::read32le;
using llvm::support::endian::read64le;

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "PE imports: " + Message);
}

class Reader {
  llvm::ArrayRef<uint8_t> Bytes;
  std::vector<detail::RawBackedSectionRange> Sections;
  uint64_t Used = 0;
  uint64_t Budget;
  bool Exhausted = false;
  bool BoundImports = false;

public:
  explicit Reader(const llvm::object::COFFObjectFile &Object)
      : Bytes(llvm::arrayRefFromStringRef(Object.getData())),
        Budget(std::min<uint64_t>(
                   Bytes.size(),
                   ::neverd::limits::loader::kPEImportsReaderMinArgument2) *
               8) {
    if (const auto *D = Object.getDataDirectory(BOUND_IMPORT))
      BoundImports = D->RelativeVirtualAddress || D->Size;
    for (const auto &Ref : Object.sections()) {
      const auto *S = Object.getCOFFSection(Ref);
      Sections.push_back({S->VirtualAddress, S->VirtualSize,
                          S->PointerToRawData, S->SizeOfRawData});
    }
  }

  llvm::Error account(uint64_t Size) {
    if (Exhausted || Size > Budget - Used) {
      Exhausted = true;
      return invalid("metadata budget exhausted");
    }
    Used += Size;
    return llvm::Error::success();
  }
  bool exhausted() const { return Exhausted; }

  llvm::Expected<llvm::ArrayRef<uint8_t>> tail(uint32_t RVA) const {
    if (!RVA)
      return invalid("metadata RVA is zero");
    auto Offset = detail::resolveUniqueRawBackedFileOffset(
        Sections, Bytes.size(), RVA, 1);
    if (!Offset)
      return Offset.takeError();
    for (const auto &S : Sections)
      if (RVA >= S.RVA && uint64_t{RVA} - S.RVA < S.VirtualSize) {
        const uint64_t Available =
            std::min(S.VirtualSize, S.RawSize) - (uint64_t{RVA} - S.RVA);
        // Validate the whole tail before exposing it, including aliases that
        // start after the first byte of this table or string.
        auto End = detail::resolveUniqueRawBackedFileOffset(
            Sections, Bytes.size(), RVA, Available);
        if (!End)
          return End.takeError();
        return Bytes.slice(*Offset, Available);
      }
    return invalid("metadata has no unique file-backed section");
  }

  llvm::Expected<std::string> string(uint32_t RVA, size_t Prefix = 0) {
    auto Data = tail(RVA);
    if (!Data)
      return Data.takeError();
    if (Data->size() <= Prefix)
      return invalid("truncated import name");
    if (auto Error = account(Prefix))
      return std::move(Error);
    const auto Maximum =
        std::min<size_t>(Data->size() - Prefix, limits::kMaxStringScanLen);
    const auto ScanSize = std::min<uint64_t>(Maximum, Budget - Used);
    const auto *Begin = Data->data() + Prefix;
    const auto *End =
        static_cast<const uint8_t *>(std::memchr(Begin, 0, ScanSize));
    if (auto Error = account(End ? size_t(End - Begin) + 1 : ScanSize))
      return std::move(Error);
    if (!End && ScanSize < Maximum) {
      Exhausted = true;
      return invalid("metadata budget exhausted");
    }
    if (!End || End == Begin)
      return invalid("empty, unterminated or oversized import name");
    return std::string(reinterpret_cast<const char *>(Begin), End - Begin);
  }

  llvm::Expected<std::vector<Import>> descriptor(const uint8_t *Data,
                                                 const BinaryImage &Image) {
    auto Module = string(read32le(Data + 12));
    if (!Module)
      return Module.takeError();
    const uint32_t IATRVA = read32le(Data + 16);
    if (!detail::validImportLookupSource(read32le(Data), read32le(Data + 4),
                                         BoundImports))
      return invalid("bound IAT has no independent import lookup table");
    const uint32_t LookupRVA = read32le(Data) ? read32le(Data) : IATRVA;
    auto Lookup = tail(LookupRVA);
    if (!Lookup)
      return Lookup.takeError();
    auto IAT = tail(IATRVA);
    if (!IAT)
      return IAT.takeError();
    const uint32_t Width = Image.getPointerSize();
    const uint64_t OrdinalFlag = uint64_t{1} << (Width * 8 - 1);
    const auto word = [Width](const uint8_t *At) {
      return Width == 8 ? read64le(At) : uint64_t{read32le(At)};
    };
    std::vector<Import> Result;
    const size_t Count = std::min(Lookup->size(), IAT->size()) / Width;
    for (size_t I = 0; I != Count; ++I) {
      if (auto Error = account(Width * 2))
        return std::move(Error);
      const uint64_t Value = word(Lookup->data() + I * Width);
      // The separate lookup table limits binding to its declared cells;
      // the following IAT cell can belong to another owner.
      if (!Value)
        return Result;
      const uint64_t SlotRVA = uint64_t{IATRVA} + I * Width;
      if (SlotRVA >= (uint64_t{1} << 32) || SlotRVA > InvalidVA - Image.Base ||
          Width - 1 > InvalidVA - (Image.Base + SlotRVA) ||
          (Width == 4 && Image.Base + SlotRVA > UINT32_MAX - (Width - 1)))
        return invalid("IAT slot address overflows");
      Import Imp;
      if (auto Error = account(sizeof(Import) + Module->size()))
        return std::move(Error);
      Imp.Module = *Module;
      Imp.IATAddr = Image.Base + SlotRVA;
      if (Value & OrdinalFlag) {
        if (Value & ~(OrdinalFlag | uint64_t{0xffff}))
          return invalid("ordinal import contains reserved bits");
        Imp.Ordinal = Value & 0xffff;
        Imp.Name = ordinalImportName(Image.Arch, *Module, Imp.Ordinal);
      } else {
        if (!detail::validImportNameBits(Value))
          return invalid("import name thunk contains reserved bits");
        auto Name = string(Value, sizeof(uint16_t));
        if (!Name)
          return Name.takeError();
        Imp.Name = std::move(*Name);
      }
      Result.push_back(std::move(Imp));
    }
    return invalid("import thunk table has no complete terminator");
  }
};

bool sameImport(const Import &Left, const Import &Right) {
  return llvm::StringRef(Left.Module).equals_insensitive(Right.Module) &&
         Left.Name == Right.Name && Left.Ordinal == Right.Ordinal &&
         Left.IATAddr == Right.IATAddr;
}
} // namespace

void parseImports(const llvm::object::COFFObjectFile &Object,
                  BinaryImage &Image) {
  const auto *D = Object.getDataDirectory(IMPORT_TABLE);
  if (!D || (!D->RelativeVirtualAddress && !D->Size))
    return;
  Reader Raw(Object);
  auto Directory = Raw.tail(D->RelativeVirtualAddress);
  if (!Directory || D->Size > Directory->size()) {
    const auto Message = Directory ? "import directory exceeds mapped bytes"
                                   : llvm::toString(Directory.takeError());
    Image.addLoadDiagnostic("pe.import_directory_invalid", Message);
    return;
  }
  const auto Bytes = Directory->take_front(D->Size);
  constexpr size_t DescriptorSize =
      sizeof(llvm::object::coff_import_directory_table_entry);
  std::vector<std::vector<Import>> Descriptors;
  bool Terminated = false;
  for (size_t Offset = 0; Bytes.size() - Offset >= DescriptorSize;
       Offset += DescriptorSize) {
    const auto Data = Bytes.slice(Offset, DescriptorSize);
    if (std::all_of(Data.begin(), Data.end(),
                    [](uint8_t Byte) { return !Byte; })) {
      Terminated = true;
      break;
    }
    if (auto Error = Raw.account(DescriptorSize)) {
      Image.addLoadDiagnostic("pe.import_budget_exhausted",
                              llvm::toString(std::move(Error)));
      break;
    }
    auto Imports = Raw.descriptor(Data.data(), Image);
    if (!Imports) {
      if (Raw.exhausted()) {
        Image.addLoadDiagnostic("pe.import_budget_exhausted",
                                llvm::toString(Imports.takeError()));
        break;
      }
      Image.addLoadDiagnostic(
          "pe.import_descriptor_invalid",
          "PE import descriptor " + std::to_string(Offset / DescriptorSize) +
              " ignored: " + llvm::toString(Imports.takeError()));
      continue;
    }
    Descriptors.push_back(std::move(*Imports));
  }
  if (!Terminated) {
    if (!Raw.exhausted())
      Image.addLoadDiagnostic("pe.import_directory_unterminated",
                              "PE import directory has no complete terminator; "
                              "no ordinary import bindings were published.");
    return;
  }

  // Validate all descriptor identities before publishing any storage facts.
  // Even equal symbol names from different modules are distinct bindings.
  struct Slot {
    const Import *Imp;
    std::vector<size_t> Descriptors;
    bool Conflicted = false;
  };
  std::map<va_t, Slot> Slots;
  std::set<size_t> Conflicts;
  const uint32_t Width = Image.getPointerSize();
  for (size_t I = 0; I != Descriptors.size(); ++I)
    for (const auto &Imp : Descriptors[I]) {
      const auto conflict = [&](Slot &Other) {
        if (Other.Conflicted || !sameImport(Imp, *Other.Imp)) {
          Conflicts.insert(I);
          if (!Other.Conflicted) {
            Conflicts.insert(Other.Descriptors.begin(),
                             Other.Descriptors.end());
            Other.Conflicted = true;
          }
        }
      };
      auto Next = Slots.lower_bound(Imp.IATAddr);
      if (Next != Slots.end() && Next->first - Imp.IATAddr < Width)
        conflict(Next->second);
      if (Next != Slots.begin()) {
        const auto Prev = std::prev(Next);
        if (Imp.IATAddr - Prev->first < Width)
          conflict(Prev->second);
      }
      auto SlotAt =
          Slots
              .try_emplace(Imp.IATAddr, Slot{&Imp, {}, Conflicts.count(I) != 0})
              .first;
      SlotAt->second.Descriptors.push_back(I);
    }
  std::set<va_t> Published;
  for (size_t I = 0; I != Descriptors.size(); ++I) {
    if (Conflicts.count(I)) {
      Image.addLoadDiagnostic("pe.import_slots_conflict",
                              "Conflicting PE import descriptors share IAT "
                              "bytes; their bindings were ignored.");
      continue;
    }
    for (auto &Imp : Descriptors[I]) {
      if (!Published.insert(Imp.IATAddr).second)
        continue;
      // Windows accepts unaligned IATs and IATs in executable sections. A
      // complete descriptor still owns their import identities; it does not
      // relax the shared canonical pointer-storage contract.
      if (Image.isValidImportStorageSlot(Imp.IATAddr, Imp.Name) &&
          !Image.recordImportStorageSlot(
              Imp.IATAddr, Imp.Name, 0,
              ImportStorageEvidence::ImportDirectory)) {
        Image.addLoadDiagnostic("pe.import_binding_conflict",
                                "PE import storage conflicts with an existing "
                                "identity; no import identity was published.");
        continue;
      }
      Image.Imports.push_back(std::move(Imp));
    }
  }
}
} // namespace neverd::coff_loader
