//===- PatchObject.cpp - Native object replacement input ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/PatchObject.h"

#include "neverd/backend/codegen/DwarfEHFrame.h"
#include "neverd/backend/codegen/ELF/ELFARMEHABIPatch.h"
#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"
#include "neverd/backend/codegen/ELF/ELFPatch.h"
#include "neverd/backend/codegen/ELF/ELFSectionPatch.h"
#include "neverd/backend/codegen/ELF/ELFSourceRelocations.h"
#include "neverd/backend/codegen/ELF/ELFSourceSymbols.h"
#include "neverd/backend/codegen/PatchStorage.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/ARMAttributeParser.h"
#include "llvm/Support/ARMBuildAttributes.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd {
namespace {
using SectionKind = llvm::mc_rewrite::RewriteSectionKind;
constexpr uint64_t MaxImageSize = 64 * 1024 * 1024;

llvm::Error fail(const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "patch object: " + Detail);
}

struct ObjectSymbol {
  std::string Name;
  uint32_t Section = 0;
  uint64_t Offset = 0;
  uint64_t Size = 0;
  unsigned Type = 0;
  unsigned Binding = 0;
  bool Thumb = false;
};

struct ObjectRelocation {
  size_t Section = 0;
  uint64_t Offset = 0;
  uint32_t Symbol = 0;
  uint32_t Type = 0;
  int64_t Addend = 0;
};

struct ResolvedSymbol {
  uint64_t VA = 0;
  uint32_t DynamicIndex = 0;
};

struct ObjectImage {
  // Only the common placement fields are populated. MC ownership and fixup
  // authentication fields intentionally remain empty.
  CompiledImage Storage;
  std::vector<ObjectSymbol> Symbols;
  std::vector<ObjectRelocation> Relocations;
  std::map<uint32_t, size_t> SectionIndices;
  std::map<std::string, uint32_t> NamedSymbols;
  std::map<uint32_t, uint64_t> GOT;
  std::vector<ELFSourcePointerRelocation> Pointers;
  uint64_t MetadataOffset = 0;
  uint64_t GOTBase = 0;
  bool NeedsGOTBase = false;
};

template <typename ELF>
llvm::Expected<ObjectImage> readELFObject(llvm::StringRef Bytes,
                                          const BinaryImage &Original) {
  constexpr bool Is64 = sizeof(typename ELF::Addr) == 8;
  if (Bytes.size() < sizeof(typename ELF::Ehdr) ||
      !Bytes.starts_with("\177ELF") ||
      static_cast<uint8_t>(Bytes[llvm::ELF::EI_CLASS]) !=
          (Is64 ? llvm::ELF::ELFCLASS64 : llvm::ELF::ELFCLASS32) ||
      static_cast<uint8_t>(Bytes[llvm::ELF::EI_DATA]) != llvm::ELF::ELFDATA2LSB)
    return fail("expected a native ELF64 little-endian object, not bitcode");
  auto File = llvm::object::ELFFile<ELF>::create(Bytes);
  if (!File)
    return File.takeError();
  const auto &Header = File->getHeader();
  const uint16_t Machine = Original.Arch == Arch::X86   ? llvm::ELF::EM_386
                           : Original.Arch == Arch::X64 ? llvm::ELF::EM_X86_64
                           : Original.Arch == Arch::ARM ? llvm::ELF::EM_ARM
                           : Original.Arch == Arch::AArch64
                               ? llvm::ELF::EM_AARCH64
                               : 0;
  if (Header.e_type != llvm::ELF::ET_REL || Header.e_machine != Machine ||
      (Original.Arch == Arch::ARM
           ? (Header.e_flags & llvm::ELF::EF_ARM_EABIMASK) !=
                 llvm::ELF::EF_ARM_EABI_VER5
           : Header.e_flags != 0) ||
      Header.e_phnum != 0 || Header.e_version != llvm::ELF::EV_CURRENT ||
      (Header.e_ident[llvm::ELF::EI_OSABI] != llvm::ELF::ELFOSABI_NONE &&
       Header.e_ident[llvm::ELF::EI_OSABI] != llvm::ELF::ELFOSABI_GNU))
    return fail("object ISA or ELF ABI does not match the original");
  auto Sections = File->sections();
  if (!Sections)
    return Sections.takeError();
  if (Sections->empty() || Sections->size() > 65536)
    return fail("invalid section count");
  ObjectImage Result;
  Result.Storage.InputKind = RewriteInputKind::ReplacementObject;
  Result.Storage.TargetArch = Original.Arch;
  Result.Storage.Format = BinaryFormat::ELF;
  auto Triple = sourcePatchTargetTriple(Original);
  if (!Triple)
    return Triple.takeError();
  Result.Storage.TargetTriple = *Triple;
  Result.Storage.PointerWidth = Original.getPointerSize();
  uint64_t AllocatedBytes = 0;
  std::optional<uint32_t> Symtab;
  bool HasARMAttributes = false;
  unsigned ARMVFPArguments = 0;
  for (uint32_t I = 0; I < Sections->size(); ++I) {
    const auto &Section = (*Sections)[I];
    auto Name = File->getSectionName(Section);
    if (!Name)
      return Name.takeError();
    if (Original.Arch == Arch::ARM &&
        Section.sh_type == llvm::ELF::SHT_ARM_ATTRIBUTES) {
      if (HasARMAttributes)
        return fail("multiple ARM ABI attribute sections");
      HasARMAttributes = true;
      auto Contents = File->getSectionContents(Section);
      if (!Contents)
        return Contents.takeError();
      llvm::ARMAttributeParser Attributes;
      if (auto Error = Attributes.parse(*Contents, llvm::endianness::little))
        return std::move(Error);
      ARMVFPArguments =
          Attributes.getAttributeValue(llvm::ARMBuildAttrs::ABI_VFP_args)
              .value_or(0);
      if (ARMVFPArguments > 1)
        return fail(
            "custom or ambiguous ARM floating-point calling convention");
    }
    if (Name->starts_with(".gnu.lto_") || Name->starts_with(".llvm.lto") ||
        Name->starts_with(".llvmbc"))
      return fail("LTO payloads are not native code");
    if (Section.sh_type == llvm::ELF::SHT_SYMTAB) {
      if (Symtab)
        return fail("multiple symbol tables");
      Symtab = I;
    }
    if (*Name == ".note.GNU-stack" &&
        (Section.sh_flags & llvm::ELF::SHF_EXECINSTR))
      return fail("executable-stack requirement is unsupported");
    if (!(Section.sh_flags & llvm::ELF::SHF_ALLOC))
      continue;
    if (*Name == ".init" || *Name == ".fini" || Name->starts_with(".ctors") ||
        Name->starts_with(".dtors"))
      return fail("runtime initialization/finalization is unsupported: " +
                  *Name);
    const uint64_t Flags = Section.sh_flags;
    constexpr uint64_t AllowedFlags =
        llvm::ELF::SHF_ALLOC | llvm::ELF::SHF_WRITE | llvm::ELF::SHF_EXECINSTR |
        llvm::ELF::SHF_MERGE | llvm::ELF::SHF_STRINGS | llvm::ELF::SHF_GROUP |
        llvm::ELF::SHF_GNU_RETAIN | llvm::ELF::SHF_LINK_ORDER;
    const bool Property = *Name == ".note.gnu.property" &&
                          Section.sh_type == llvm::ELF::SHT_NOTE &&
                          Flags == llvm::ELF::SHF_ALLOC;
    if (Property) {
      auto Note = File->getSectionContents(Section);
      if (!Note)
        return Note.takeError();
      auto Features =
          readELFPatchPropertyNote(*Note, Original.getPointerSize());
      if (!Features)
        return Features.takeError();
    }
    if ((Flags & llvm::ELF::SHF_LINK_ORDER) &&
        (Section.sh_link == 0 || Section.sh_link >= Sections->size() ||
         !((*Sections)[Section.sh_link].sh_flags & llvm::ELF::SHF_ALLOC)))
      return fail("invalid link-order section dependency");
    if ((Flags & ~AllowedFlags) ||
        (Section.sh_type != llvm::ELF::SHT_PROGBITS &&
         Section.sh_type != llvm::ELF::SHT_NOBITS &&
         Section.sh_type != llvm::ELF::SHT_X86_64_UNWIND &&
         !(Original.Arch == Arch::ARM &&
           Section.sh_type == llvm::ELF::SHT_ARM_EXIDX) &&
         !Property) ||
        ((Flags & llvm::ELF::SHF_WRITE) && (Flags & llvm::ELF::SHF_EXECINSTR)))
      return fail("unsupported allocated section or permissions: " + *Name);
    const uint64_t Alignment = std::max<uint64_t>(Section.sh_addralign, 1);
    if (!llvm::isPowerOf2_64(Alignment) || Alignment > 1024 * 1024 ||
        Section.sh_size > MaxImageSize || Name->empty())
      return fail("invalid allocated section: " + *Name);
    if (Section.sh_size > MaxImageSize - AllocatedBytes)
      return fail("total allocated storage exceeds the profile limit");
    AllocatedBytes += Section.sh_size;
    if (*Name == ".got.source")
      return fail("reserved synthetic GOT section name");
    CompiledSection Item;
    Item.Name = Name->str();
    Item.Alignment = Alignment;
    Item.Size = Section.sh_size;
    Item.Kind = (Flags & llvm::ELF::SHF_EXECINSTR) ? SectionKind::Code
                : Section.sh_type == llvm::ELF::SHT_NOBITS
                    ? SectionKind::UninitializedData
                : (Flags & llvm::ELF::SHF_WRITE) ? SectionKind::WritableData
                                                 : SectionKind::ReadOnlyData;
    if (Section.sh_type == llvm::ELF::SHT_NOBITS) {
      if (!(Flags & llvm::ELF::SHF_WRITE))
        return fail("non-writable zero-fill storage is unsupported");
      Item.ExternalBytes.resize(Item.Size, 0);
    } else {
      auto Contents = File->getSectionContents(Section);
      if (!Contents)
        return Contents.takeError();
      Item.ExternalBytes.assign(Contents->begin(), Contents->end());
    }
    Result.SectionIndices[I] = Result.Storage.Sections.size();
    Result.Storage.Sections.push_back(std::move(Item));
  }
  if (Original.Arch == Arch::ARM) {
    if (!Original.ELFMetadata || !HasARMAttributes ||
        bool(Original.ELFMetadata->Flags & llvm::ELF::EF_ARM_ABI_FLOAT_HARD) !=
            bool(ARMVFPArguments))
      return fail("native ARM floating-point ABI does not match the original");
    if (Header.e_flags &
        ~(llvm::ELF::EF_ARM_EABIMASK | llvm::ELF::EF_ARM_ABI_FLOAT_HARD |
          llvm::ELF::EF_ARM_ABI_FLOAT_SOFT))
      return fail("unsupported native ARM ELF flags");
  }
  if (!Symtab)
    return fail("a symbol table is required");
  auto Symbols = File->symbols(&(*Sections)[*Symtab]);
  if (!Symbols)
    return Symbols.takeError();
  auto Names = File->getStringTableForSymtab((*Sections)[*Symtab]);
  if (!Names)
    return Names.takeError();
  if (Symbols->size() > 1024 * 1024)
    return fail("excessive symbol count");
  for (const auto &Symbol : *Symbols) {
    auto Name = Symbol.getName(*Names);
    if (!Name)
      return Name.takeError();
    ObjectSymbol Item{Name->str(),    Symbol.st_shndx,  Symbol.st_value,
                      Symbol.st_size, Symbol.getType(), Symbol.getBinding()};
    if (Original.Arch == Arch::ARM && Item.Type == llvm::ELF::STT_FUNC &&
        Item.Section != llvm::ELF::SHN_UNDEF) {
      Item.Thumb = Item.Offset & 1;
      Item.Offset &= ~uint64_t(1);
    }
    if (Item.Type == llvm::ELF::STT_TLS ||
        Item.Type == llvm::ELF::STT_GNU_IFUNC ||
        Item.Section == llvm::ELF::SHN_COMMON ||
        Item.Section == llvm::ELF::SHN_XINDEX ||
        (Item.Binding != llvm::ELF::STB_LOCAL &&
         Item.Binding != llvm::ELF::STB_GLOBAL &&
         Item.Binding != llvm::ELF::STB_WEAK))
      return fail("unsupported symbol: " + Item.Name);
    if (Item.Section != llvm::ELF::SHN_UNDEF &&
        Item.Section != llvm::ELF::SHN_ABS) {
      if (Item.Section >= Sections->size() ||
          !rangeInBounds(Item.Offset, Item.Size,
                         (*Sections)[Item.Section].sh_size))
        return fail("symbol exceeds its section: " + Item.Name);
    }
    if (!Item.Name.empty() && Item.Type != llvm::ELF::STT_FILE &&
        Item.Type != llvm::ELF::STT_SECTION &&
        !(Item.Binding == llvm::ELF::STB_LOCAL &&
          Item.Type == llvm::ELF::STT_NOTYPE &&
          (Original.Arch == Arch::ARM || Original.Arch == Arch::AArch64) &&
          elf_loader::isMappingSymbolName(Item.Name)) &&
        !Result.NamedSymbols.emplace(Item.Name, Result.Symbols.size()).second)
      return fail("duplicate symbol: " + Item.Name);
    Result.Symbols.push_back(std::move(Item));
  }
  // One object is installed in its entirety: COMDAT members are retained
  // together, without archive selection or cross-object ODR coalescing.
  std::set<uint32_t> GroupMembers;
  std::set<std::string> GroupSignatures;
  for (uint32_t I = 0; I < Sections->size(); ++I) {
    const auto &Section = (*Sections)[I];
    if (Section.sh_type == llvm::ELF::SHT_GROUP) {
      auto Contents = File->getSectionContents(Section);
      if (!Contents)
        return Contents.takeError();
      if (Section.sh_link != *Symtab ||
          Section.sh_info >= Result.Symbols.size() || Contents->size() < 8 ||
          Contents->size() % 4 ||
          readLE<uint32_t>(Contents->data()) != llvm::ELF::GRP_COMDAT)
        return fail("malformed or unsupported section group");
      const auto &Signature = Result.Symbols[Section.sh_info].Name;
      if (Signature.empty() || !GroupSignatures.insert(Signature).second)
        return fail("ambiguous COMDAT signature");
      for (size_t O = 4; O < Contents->size(); O += 4) {
        const auto Member = readLE<uint32_t>(Contents->data() + O);
        if (!Member || Member >= Sections->size() || Member == I ||
            !((*Sections)[Member].sh_flags & llvm::ELF::SHF_GROUP) ||
            !GroupMembers.insert(Member).second)
          return fail("invalid or multiply owned COMDAT member");
      }
    }
    if (Section.sh_type != llvm::ELF::SHT_RELA &&
        Section.sh_type != llvm::ELF::SHT_REL)
      continue;
    if (Section.sh_info >= Sections->size())
      return fail("invalid relocation target section");
    auto Target = Result.SectionIndices.find(Section.sh_info);
    if (Target == Result.SectionIndices.end())
      continue; // Debug relocations do not belong to the loaded image.
    if (Section.sh_link != *Symtab)
      return fail("allocated relocations require the unique symbol table");
    auto Add = [&](uint64_t Offset, uint32_t Symbol, uint32_t Type,
                   int64_t Addend) -> llvm::Error {
      if (Symbol >= Result.Symbols.size())
        return fail("relocation has invalid symbol index");
      Result.Relocations.push_back(
          {Target->second, Offset, Symbol, Type, Addend});
      const Arch A = Original.Arch;
      const bool GOT =
          (A == Arch::X64 && (Type == llvm::ELF::R_X86_64_GOTPCREL ||
                              Type == llvm::ELF::R_X86_64_GOTPCRELX ||
                              Type == llvm::ELF::R_X86_64_REX_GOTPCRELX)) ||
          (A == Arch::X86 && (Type == llvm::ELF::R_386_GOT32 ||
                              Type == llvm::ELF::R_386_GOT32X)) ||
          (A == Arch::ARM && Type == llvm::ELF::R_ARM_GOT_PREL) ||
          (A == Arch::AArch64 &&
           (Type == llvm::ELF::R_AARCH64_ADR_GOT_PAGE ||
            Type == llvm::ELF::R_AARCH64_LD64_GOT_LO12_NC));
      if (GOT)
        Result.GOT.emplace(Symbol, 0);
      if (GOT || (A == Arch::X86 && (Type == llvm::ELF::R_386_GOTPC ||
                                     Type == llvm::ELF::R_386_GOTOFF)))
        Result.NeedsGOTBase = true;
      return llvm::Error::success();
    };
    if (Section.sh_type == llvm::ELF::SHT_RELA) {
      auto Relocations = File->relas(Section);
      if (!Relocations)
        return Relocations.takeError();
      for (const auto &Relocation : *Relocations)
        if (auto Error = Add(Relocation.r_offset, Relocation.getSymbol(false),
                             Relocation.getType(false), Relocation.r_addend))
          return std::move(Error);
    } else {
      auto Relocations = File->rels(Section);
      if (!Relocations)
        return Relocations.takeError();
      const auto &Data = Result.Storage.Sections[Target->second].ExternalBytes;
      for (const auto &Relocation : *Relocations) {
        const uint32_t Type = Relocation.getType(false);
        const auto Width =
            elf_loader::objectRelocationFieldWidth(Original.Arch, Type);
        if (!Width)
          return fail("unsupported implicit native relocation field");
        if (!rangeInBounds(Relocation.r_offset, *Width, Data.size()))
          return fail("implicit relocation addend exceeds its section");
        const auto *Field = Data.data() + Relocation.r_offset;
        const uint64_t Raw = *Width == 8   ? readLE<uint64_t>(Field)
                             : *Width == 4 ? readLE<uint32_t>(Field)
                             : *Width == 2 ? readLE<uint16_t>(Field)
                             : *Width == 1 ? *Field
                                           : 0;
        const uint32_t Word = uint32_t(Raw);
        int64_t Addend = *Width ? llvm::SignExtend64(Raw, *Width * 8) : 0;
        if (Original.Arch == Arch::ARM) {
          if (Type == llvm::ELF::R_ARM_PREL31)
            Addend = llvm::SignExtend64<31>(Word & 0x7fffffff);
          else if (Type == llvm::ELF::R_ARM_CALL ||
                   Type == llvm::ELF::R_ARM_JUMP24 ||
                   Type == llvm::ELF::R_ARM_PC24)
            Addend = llvm::SignExtend64<24>(Word & 0xffffff) * 4;
          else if (Type == llvm::ELF::R_ARM_THM_CALL ||
                   Type == llvm::ELF::R_ARM_THM_JUMP24) {
            const uint16_t H = Word, L = Word >> 16;
            const unsigned S = (H >> 10) & 1, I1 = !(((L >> 13) & 1) ^ S),
                           I2 = !(((L >> 11) & 1) ^ S);
            Addend = llvm::SignExtend64<25>((S << 24) | (I1 << 23) |
                                            (I2 << 22) | ((H & 0x3ff) << 12) |
                                            ((L & 0x7ff) << 1));
          }
        }
        if (auto Error = Add(Relocation.r_offset, Relocation.getSymbol(false),
                             Type, Addend))
          return std::move(Error);
      }
    }
  }
  for (uint32_t I = 0; I < Sections->size(); ++I)
    if (((*Sections)[I].sh_flags & llvm::ELF::SHF_GROUP) &&
        !GroupMembers.count(I))
      return fail("section group membership is missing");
  return Result;
}

llvm::Expected<ObjectImage> readObject(llvm::StringRef Bytes,
                                       const BinaryImage &Original) {
  return Original.getPointerSize() == 8
             ? readELFObject<llvm::object::ELF64LE>(Bytes, Original)
             : readELFObject<llvm::object::ELF32LE>(Bytes, Original);
}

llvm::Error placeObject(ObjectImage &Object, const BinaryImage &Original,
                        uint64_t BaseVA, uint64_t OriginalUnwindSize) {
  auto &Image = Object.Storage;
  Image.BaseVA = BaseVA;
  if (!BaseVA || BaseVA > UINT64_MAX - MaxImageSize)
    return fail("invalid placement base");
  if (Object.NeedsGOTBase) {
    CompiledSection GOT;
    GOT.Name = ".got.source";
    GOT.Kind = SectionKind::WritableData;
    GOT.Size = std::max<size_t>(Object.GOT.size(), 1) * Image.PointerWidth;
    GOT.Alignment = Image.PointerWidth;
    GOT.ExternalBytes.resize(GOT.Size, 0);
    Image.Sections.push_back(std::move(GOT));
  }
  uint64_t Cursor = BaseVA;
  auto Place = [&](CompiledSection &Section) -> llvm::Error {
    Cursor = llvm::alignTo(Cursor, Section.Alignment);
    if (Cursor < BaseVA || Cursor - BaseVA > MaxImageSize ||
        Section.Size > MaxImageSize - (Cursor - BaseVA))
      return fail("object placement exceeds the profile limit");
    Section.VA = Cursor;
    Section.Offset = Cursor - BaseVA;
    Cursor += Section.Size;
    return llvm::Error::success();
  };
  for (auto &Section : Image.Sections)
    if (Section.Kind == SectionKind::Code)
      if (auto Error = Place(Section))
        return Error;
  for (auto &Section : Image.Sections)
    if (Section.Kind == SectionKind::ReadOnlyData)
      if (auto Error = Place(Section))
        return Error;
  Object.MetadataOffset = llvm::alignTo(Cursor - BaseVA, 8);
  // Reserve enough for the old and new search/RELA tables before deciding RW
  // addresses. The actual tables are rebuilt from their authenticated inputs.
  uint64_t Reserve = OriginalUnwindSize + Object.Symbols.size() * 8 + 64 +
                     (Object.Relocations.size() + Object.GOT.size()) * 24;
  forEachELFShdr(Original.Raw.data(), Original.Raw.size(),
                 [&](const ELFShdrFields &Section, uint16_t) {
                   if (Section.Type == llvm::ELF::SHT_RELA ||
                       Section.Type == llvm::ELF::SHT_REL)
                     Reserve += Section.Size;
                 });
  if (Reserve > MaxImageSize || Object.MetadataOffset > MaxImageSize - Reserve)
    return fail("object metadata exceeds the profile limit");
  Cursor = BaseVA + Object.MetadataOffset + Reserve;
  const bool HasWritable = std::any_of(
      Image.Sections.begin(), Image.Sections.end(), [](const auto &Section) {
        return Section.Size && (Section.Kind == SectionKind::WritableData ||
                                Section.Kind == SectionKind::UninitializedData);
      });
  if (HasWritable)
    Cursor = llvm::alignTo(
        Cursor, sourcePatchStorageAlignment(Original.Arch, Original.Format));
  for (auto &Section : Image.Sections)
    if (Section.Kind == SectionKind::WritableData ||
        Section.Kind == SectionKind::UninitializedData)
      if (auto Error = Place(Section))
        return Error;
  if (Cursor - BaseVA > MaxImageSize)
    return fail("object image is too large");
  Image.Bytes.resize(Cursor - BaseVA, 0);
  for (auto &Section : Image.Sections) {
    std::copy(Section.ExternalBytes.begin(), Section.ExternalBytes.end(),
              Image.Bytes.begin() + Section.Offset);
    Section.ExternalBytes.clear();
  }
  if (Object.NeedsGOTBase) {
    uint64_t Slot = Image.Sections.back().VA;
    Object.GOTBase = Slot;
    for (auto &[Symbol, VA] : Object.GOT) {
      VA = Slot;
      Slot += Image.PointerWidth;
    }
  }
  for (const auto &[Name, Index] : Object.NamedSymbols) {
    const auto &Symbol = Object.Symbols[Index];
    auto Section = Object.SectionIndices.find(Symbol.Section);
    if (Section != Object.SectionIndices.end())
      Image.SymbolAddrs[Name] =
          Image.Sections[Section->second].VA + Symbol.Offset;
  }
  Image.Success = true;
  auto Layout = validatePatchStorageLayout(
      Image, sourcePatchStorageAlignment(Original.Arch, Original.Format));
  return Layout ? llvm::Error::success() : Layout.takeError();
}

llvm::Expected<ResolvedSymbol> resolveObjectSymbol(
    const ObjectImage &Object, uint32_t Index, const BinaryImage &Original,
    const std::map<std::string, SourcePatchBinding> &Bindings, bool Callable) {
  const auto &Symbol = Object.Symbols[Index];
  if (Symbol.Section != llvm::ELF::SHN_UNDEF) {
    auto Section = Object.SectionIndices.find(Symbol.Section);
    if (Section == Object.SectionIndices.end())
      return fail("reference to non-allocated or absolute symbol: " +
                  Symbol.Name);
    const auto &Storage = Object.Storage.Sections[Section->second];
    if (Callable &&
        (Storage.Kind != SectionKind::Code || Symbol.Offset >= Storage.Size))
      return fail("callable symbol does not name code: " + Symbol.Name);
    return ResolvedSymbol{Storage.VA + Symbol.Offset + (Symbol.Thumb ? 1 : 0),
                          0};
  }
  if (auto Binding = Bindings.find(Symbol.Name); Binding != Bindings.end()) {
    if (Callable && Binding->second.Kind != SourcePatchBindingKind::Function)
      return fail("data binding used as a callable target: " + Symbol.Name);
    return ResolvedSymbol{Binding->second.OriginalVA, 0};
  }
  if (Symbol.Binding == llvm::ELF::STB_WEAK)
    return fail("unresolved weak symbol: " + Symbol.Name);
  if (!Callable) {
    auto Dynamic = findELFSourceDynamicSymbol(Original, Symbol.Name);
    if (!Dynamic)
      return Dynamic.takeError();
    if (*Dynamic)
      return ResolvedSymbol{0, **Dynamic};
    return fail("external data/address requires an explicit binding or "
                "existing import: " +
                Symbol.Name);
  }
  std::set<uint64_t> Addresses;
  for (const auto &Candidate : Original.Symbols)
    if (Candidate.Name == Symbol.Name && Candidate.IsFunc &&
        Original.hasAuthenticatedFunctionEntryAt(Candidate.Addr))
      Addresses.insert(Candidate.Addr);
  for (const auto &Export : Original.Exports)
    if (Export.Name == Symbol.Name &&
        Original.hasAuthenticatedFunctionEntryAt(Export.Addr))
      Addresses.insert(Export.Addr);
  for (const auto &[VA, Import] : Original.ImportStubIndices)
    if (Import < Original.Imports.size() &&
        Original.Imports[Import].Name == Symbol.Name &&
        Original.findImportStubAt(VA) && Original.isCodeAddress(VA))
      Addresses.insert(VA);
  if (Addresses.size() != 1)
    return fail("missing or ambiguous original callable symbol: " +
                Symbol.Name);
  return ResolvedSymbol{*Addresses.begin(), 0};
}

// Relocate native ELF32 and AArch64 producer bytes. The ELF symbol table owns
// function extents; no MC range identities are synthesized for object input.
llvm::Error relocateOtherELFObject(
    ObjectImage &Object, const BinaryImage &Original,
    const std::map<std::string, SourcePatchBinding> &Bindings) {
  auto &Image = Object.Storage;
  const Arch A = Image.TargetArch;
  for (const auto &[Symbol, Field] : Object.GOT) {
    auto Target =
        resolveObjectSymbol(Object, Symbol, Original, Bindings, false);
    if (!Target)
      return Target.takeError();
    Object.Pointers.push_back(
        {Field, Target->DynamicIndex, int64_t(Target->VA)});
    if (Image.PointerWidth == 8)
      writeLE<uint64_t>(Image.Bytes.data() + Field - Image.BaseVA, Target->VA);
    else {
      if (Target->VA > UINT32_MAX)
        return fail("GOT pointer exceeds ELF32");
      writeLE<uint32_t>(Image.Bytes.data() + Field - Image.BaseVA, Target->VA);
    }
  }
  std::map<uint64_t, unsigned> Written;
  for (const auto &R : Object.Relocations) {
    if (R.Type == 0)
      continue;
    if (A == Arch::ARM && R.Type == llvm::ELF::R_ARM_V4BX) {
      const auto &S = Image.Sections[R.Section];
      if (!rangeInBounds(R.Offset, 4, S.Size) ||
          (readLE<uint32_t>(Image.Bytes.data() + S.Offset + R.Offset) &
           0x0ffffff0) != 0x012fff10)
        return fail(
            "V4BX relocation does not name BX on the admitted ARMv7 target");
      continue; // ARMv7 implements BX; no legacy MOV-PC conversion is needed.
    }
    const auto &Section = Image.Sections[R.Section];
    const bool Absolute =
        (A == Arch::X86 && R.Type == llvm::ELF::R_386_32) ||
        (A == Arch::ARM && R.Type == llvm::ELF::R_ARM_ABS32) ||
        (A == Arch::AArch64 && R.Type == llvm::ELF::R_AARCH64_ABS64);
    const bool GOT = Object.GOT.count(R.Symbol) &&
                     ((A == Arch::X86 && (R.Type == llvm::ELF::R_386_GOT32 ||
                                          R.Type == llvm::ELF::R_386_GOT32X)) ||
                      (A == Arch::ARM && R.Type == llvm::ELF::R_ARM_GOT_PREL) ||
                      (A == Arch::AArch64 &&
                       (R.Type == llvm::ELF::R_AARCH64_ADR_GOT_PAGE ||
                        R.Type == llvm::ELF::R_AARCH64_LD64_GOT_LO12_NC)));
    const bool GOTPC = A == Arch::X86 && R.Type == llvm::ELF::R_386_GOTPC;
    const bool Call =
        (A == Arch::X86 && R.Type == llvm::ELF::R_386_PLT32) ||
        (A == Arch::ARM &&
         (R.Type == llvm::ELF::R_ARM_CALL || R.Type == llvm::ELF::R_ARM_PC24 ||
          R.Type == llvm::ELF::R_ARM_JUMP24 ||
          R.Type == llvm::ELF::R_ARM_THM_CALL ||
          R.Type == llvm::ELF::R_ARM_THM_JUMP24)) ||
        (A == Arch::AArch64 && (R.Type == llvm::ELF::R_AARCH64_CALL26 ||
                                R.Type == llvm::ELF::R_AARCH64_JUMP26));
    const auto FieldWidth = elf_loader::objectRelocationFieldWidth(A, R.Type);
    if (!FieldWidth)
      return fail("unsupported native relocation field width");
    const unsigned Width = *FieldWidth;
    if (!rangeInBounds(R.Offset, Width, Section.Size))
      return fail("relocation exceeds its native section");
    const uint64_t P = Section.VA + R.Offset;
    auto Next = Written.lower_bound(P);
    if ((Next != Written.end() && Next->first - P < Width) ||
        (Next != Written.begin() &&
         P - std::prev(Next)->first < std::prev(Next)->second))
      return fail("overlapping native relocation writers");
    Written.emplace(P, Width);
    auto Target =
        GOT ? llvm::Expected<ResolvedSymbol>(
                  ResolvedSymbol{Object.GOT.at(R.Symbol), 0})
        : GOTPC
            ? llvm::Expected<ResolvedSymbol>(ResolvedSymbol{Object.GOTBase, 0})
            : resolveObjectSymbol(Object, R.Symbol, Original, Bindings, Call);
    if (!Target)
      return Target.takeError();
    auto *Out = Image.Bytes.data() + Section.Offset + R.Offset;
    llvm::APInt Value(128, Target->VA);
    Value += llvm::APInt(128, uint64_t(R.Addend), true);
    auto SignedDelta = [&](uint64_t Base,
                           unsigned Bits) -> llvm::Expected<int64_t> {
      auto Delta = Value - llvm::APInt(128, Base);
      if (!Delta.isSignedIntN(Bits))
        return fail("native relocation displacement overflows");
      return Delta.sextOrTrunc(64).getSExtValue();
    };
    if (Absolute) {
      if (!Value.isIntN(Width * 8))
        return fail("native absolute pointer overflows");
      const uint64_t V = Value.getLoBits(64).getZExtValue();
      if (Width == 8)
        writeLE<uint64_t>(Out, V);
      else
        writeLE<uint32_t>(Out, V);
      if (Section.Kind != SectionKind::WritableData &&
          Section.Kind != SectionKind::UninitializedData)
        return fail("absolute native pointer requires writable storage");
      Object.Pointers.push_back({P, Target->DynamicIndex,
                                 Target->DynamicIndex ? R.Addend : int64_t(V)});
      continue;
    }
    if (Target->DynamicIndex)
      return fail("native direct reference to dynamic data requires binding");
    if (A == Arch::X86) {
      uint64_t Base = P;
      if (R.Type == llvm::ELF::R_386_GOTOFF ||
          R.Type == llvm::ELF::R_386_GOT32 || R.Type == llvm::ELF::R_386_GOT32X)
        Base = Object.GOTBase;
      else if (R.Type != llvm::ELF::R_386_PC32 &&
               R.Type != llvm::ELF::R_386_PLT32 && !GOTPC)
        return fail("unsupported i386 relocation " + llvm::Twine(R.Type));
      auto D = SignedDelta(Base, 32);
      if (!D)
        return D.takeError();
      writeLE<uint32_t>(Out, uint32_t(*D));
    } else if (A == Arch::ARM) {
      if (R.Type == llvm::ELF::R_ARM_PREL31 ||
          R.Type == llvm::ELF::R_ARM_REL32 ||
          R.Type == llvm::ELF::R_ARM_GOT_PREL) {
        if (R.Type == llvm::ELF::R_ARM_PREL31)
          Value.clearBit(0);
        auto D = SignedDelta(P, R.Type == llvm::ELF::R_ARM_PREL31 ? 31 : 32);
        if (!D)
          return D.takeError();
        writeLE<uint32_t>(Out, uint32_t(*D) & (R.Type == llvm::ELF::R_ARM_PREL31
                                                   ? 0x7fffffff
                                                   : 0xffffffff));
      } else if (R.Type == llvm::ELF::R_ARM_CALL ||
                 R.Type == llvm::ELF::R_ARM_PC24 ||
                 R.Type == llvm::ELF::R_ARM_JUMP24) {
        const bool Thumb = Target->VA & 1;
        Value.clearBit(0);
        auto D = SignedDelta(P, 26);
        if (!D)
          return D.takeError();
        const uint32_t Old = readLE<uint32_t>(Out);
        if ((*D & 1) || (P & 3) || (Thumb && R.Type != llvm::ELF::R_ARM_CALL) ||
            (!Thumb && (*D & 3)) || (Old & 0x0e000000) != 0x0a000000)
          return fail("ARM call requires an unsupported interworking veneer");
        const uint32_t Prefix =
            Thumb ? 0xfa000000
                  : (Old >> 28 == 15 ? 0xeb000000 : Old & 0xff000000);
        writeLE<uint32_t>(Out, Prefix | (uint32_t(*D >> 2) & 0xffffff) |
                                   (Thumb ? (uint32_t(*D) & 2) << 23 : 0));
      } else if (R.Type == llvm::ELF::R_ARM_THM_CALL ||
                 R.Type == llvm::ELF::R_ARM_THM_JUMP24) {
        const bool Thumb = Target->VA & 1;
        if (!Thumb && R.Type != llvm::ELF::R_ARM_THM_CALL)
          return fail("Thumb tail branch requires an ARM veneer");
        Value.clearBit(0);
        auto D = SignedDelta(Thumb ? P : (P & ~uint64_t(3)), 25);
        if (!D)
          return D.takeError();
        if ((*D & (Thumb ? 1 : 3)) || (P & 1))
          return fail("unaligned Thumb branch");
        const uint32_t Imm = uint32_t(*D) & 0x1ffffff;
        const unsigned S = (Imm >> 24) & 1, J1 = !(((Imm >> 23) & 1) ^ S),
                       J2 = !(((Imm >> 22) & 1) ^ S);
        const uint16_t H = 0xf000 | (S << 10) | ((Imm >> 12) & 0x3ff);
        const uint16_t Prefix = R.Type == llvm::ELF::R_ARM_THM_JUMP24 ? 0x9000
                                : Thumb                               ? 0xd000
                                                                      : 0xc000;
        writeLE<uint16_t>(Out, H);
        writeLE<uint16_t>(Out + 2, Prefix | (J1 << 13) | (J2 << 11) |
                                       ((Imm >> 1) & 0x7ff));
      } else
        return fail("unsupported ARM native relocation " + llvm::Twine(R.Type));
    } else if (A == Arch::AArch64) {
      if (Width < 4)
        return fail("unsupported AArch64 native relocation field width");
      uint32_t Word = readLE<uint32_t>(Out);
      if (R.Type == llvm::ELF::R_AARCH64_PREL32 ||
          R.Type == llvm::ELF::R_AARCH64_PREL64) {
        auto D = SignedDelta(P, Width * 8);
        if (!D)
          return D.takeError();
        if (Width == 8)
          writeLE<uint64_t>(Out, uint64_t(*D));
        else
          writeLE<uint32_t>(Out, uint32_t(*D));
      } else if (R.Type == llvm::ELF::R_AARCH64_CALL26 ||
                 R.Type == llvm::ELF::R_AARCH64_JUMP26) {
        auto D = SignedDelta(P, 28);
        if (!D)
          return D.takeError();
        const uint32_t Opcode =
            R.Type == llvm::ELF::R_AARCH64_CALL26 ? 0x94000000 : 0x14000000;
        if ((*D & 3) || (P & 3) || (Word & 0xfc000000) != Opcode)
          return fail("invalid AArch64 branch relocation");
        writeLE<uint32_t>(Out, Opcode | (uint32_t(*D >> 2) & 0x3ffffff));
      } else if (R.Type == llvm::ELF::R_AARCH64_ADR_PREL_PG_HI21 ||
                 R.Type == llvm::ELF::R_AARCH64_ADR_PREL_PG_HI21_NC ||
                 R.Type == llvm::ELF::R_AARCH64_ADR_GOT_PAGE) {
        const uint64_t VA =
            Value.getLoBits(64).getZExtValue() & ~uint64_t(4095);
        const int64_t D = int64_t(VA) - int64_t(P & ~uint64_t(4095));
        if ((Word & 0x9f000000) != 0x90000000 || !llvm::isInt<33>(D))
          return fail("AArch64 ADRP page exceeds its encoding");
        const uint32_t Pages = uint32_t(D >> 12) & 0x1fffff;
        writeLE<uint32_t>(Out, (Word & ~uint32_t(0x60ffffe0)) |
                                   ((Pages & 3) << 29) | ((Pages >> 2) << 5));
      } else {
        unsigned Scale = 0;
        switch (R.Type) {
        case llvm::ELF::R_AARCH64_ADD_ABS_LO12_NC:
          break;
        case llvm::ELF::R_AARCH64_LDST8_ABS_LO12_NC:
          break;
        case llvm::ELF::R_AARCH64_LDST16_ABS_LO12_NC:
          Scale = 1;
          break;
        case llvm::ELF::R_AARCH64_LDST32_ABS_LO12_NC:
          Scale = 2;
          break;
        case llvm::ELF::R_AARCH64_LDST64_ABS_LO12_NC:
        case llvm::ELF::R_AARCH64_LD64_GOT_LO12_NC:
          Scale = 3;
          break;
        case llvm::ELF::R_AARCH64_LDST128_ABS_LO12_NC:
          Scale = 4;
          break;
        default:
          return fail("unsupported AArch64 native relocation " +
                      llvm::Twine(R.Type));
        }
        const uint32_t Low = Value.getLoBits(12).getZExtValue();
        if (Low & ((1u << Scale) - 1))
          return fail("unaligned AArch64 low-page field");
        if (R.Type == llvm::ELF::R_AARCH64_ADD_ABS_LO12_NC) {
          if ((Word & 0x7f000000) != 0x11000000 || (Word & (1u << 22)))
            return fail("low-page relocation is not ADD immediate");
        } else if ((Word & 0x3b000000) != 0x39000000)
          return fail("low-page relocation is not load/store immediate");
        writeLE<uint32_t>(Out, (Word & ~uint32_t(0x003ffc00)) |
                                   ((Low >> Scale) << 10));
      }
    }
  }
  return llvm::Error::success();
}

llvm::Error
relocateObject(ObjectImage &Object, const BinaryImage &Original,
               const std::map<std::string, SourcePatchBinding> &Bindings) {
  if (Object.Storage.TargetArch != Arch::X64)
    return relocateOtherELFObject(Object, Original, Bindings);
  auto &Image = Object.Storage;
  for (const auto &[Symbol, Field] : Object.GOT) {
    auto Target =
        resolveObjectSymbol(Object, Symbol, Original, Bindings, false);
    if (!Target)
      return Target.takeError();
    Object.Pointers.push_back(
        {Field, Target->DynamicIndex, static_cast<int64_t>(Target->VA)});
    writeLE<uint64_t>(Image.Bytes.data() + Field - Image.BaseVA, Target->VA);
  }
  std::map<uint64_t, uint64_t> Written;
  for (const auto &Relocation : Object.Relocations) {
    if (Relocation.Type == llvm::ELF::R_X86_64_NONE)
      continue;
    const auto &Section = Image.Sections[Relocation.Section];
    const bool Absolute = Relocation.Type == llvm::ELF::R_X86_64_64;
    const bool PCRel = Relocation.Type == llvm::ELF::R_X86_64_PC32 ||
                       Relocation.Type == llvm::ELF::R_X86_64_PLT32;
    const bool GOT = Relocation.Type == llvm::ELF::R_X86_64_GOTPCREL ||
                     Relocation.Type == llvm::ELF::R_X86_64_GOTPCRELX ||
                     Relocation.Type == llvm::ELF::R_X86_64_REX_GOTPCRELX;
    const uint64_t Width = Absolute ? 8 : 4;
    if (!Absolute && !PCRel && !GOT)
      return fail("unsupported x64 relocation " + llvm::Twine(Relocation.Type));
    if (!rangeInBounds(Relocation.Offset, Width, Section.Size))
      return fail("relocation exceeds its owning section");
    const uint64_t Field = Section.VA + Relocation.Offset;
    auto Next = Written.lower_bound(Field);
    if ((Next != Written.end() && Next->first - Field < Width) ||
        (Next != Written.begin() &&
         Field - std::prev(Next)->first < std::prev(Next)->second))
      return fail("overlapping relocation writers");
    Written[Field] = Width;
    auto Target =
        GOT ? llvm::Expected<ResolvedSymbol>(
                  ResolvedSymbol{Object.GOT.at(Relocation.Symbol), 0})
            : resolveObjectSymbol(Object, Relocation.Symbol, Original, Bindings,
                                  Relocation.Type == llvm::ELF::R_X86_64_PLT32);
    if (!Target)
      return Target.takeError();
    uint8_t *Output = Image.Bytes.data() + Section.Offset + Relocation.Offset;
    if (Absolute) {
      if (Section.Kind != SectionKind::WritableData &&
          Section.Kind != SectionKind::UninitializedData)
        return fail("absolute pointer needs writable relocation storage");
      const uint64_t Value =
          Target->VA + static_cast<uint64_t>(Relocation.Addend);
      Object.Pointers.push_back({Field, Target->DynamicIndex,
                                 Target->DynamicIndex
                                     ? Relocation.Addend
                                     : static_cast<int64_t>(Value)});
      writeLE<uint64_t>(Output, Value);
    } else {
      if (Target->DynamicIndex)
        return fail(
            "direct PC-relative reference to dynamic data requires binding");
      // APInt checks the full signed expression before narrowing to rel32.
      llvm::APInt Delta(128, Target->VA);
      Delta += llvm::APInt(128, static_cast<uint64_t>(Relocation.Addend), true);
      Delta -= llvm::APInt(128, Field);
      if (!Delta.isSignedIntN(32))
        return fail(
            "PC-relative relocation is outside the signed 32-bit range");
      writeLE<uint32_t>(
          Output, static_cast<uint32_t>(Delta.getLoBits(32).getZExtValue()));
    }
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint64_t>>
validateObjectUnwind(const ObjectImage &Object) {
  const auto &Image = Object.Storage;
  const CompiledSection *EH = nullptr;
  std::map<uint64_t, uint64_t> Functions;
  for (const auto &Symbol : Object.Symbols) {
    if (Symbol.Type != llvm::ELF::STT_FUNC ||
        Symbol.Section == llvm::ELF::SHN_UNDEF)
      continue;
    auto Section = Object.SectionIndices.find(Symbol.Section);
    if (Section == Object.SectionIndices.end() || Symbol.Size == 0 ||
        Image.Sections[Section->second].Kind != SectionKind::Code)
      return fail("function requires a bounded executable symbol: " +
                  Symbol.Name);
    const uint64_t Begin = Image.Sections[Section->second].VA + Symbol.Offset;
    auto [It, Inserted] = Functions.emplace(Begin, Begin + Symbol.Size);
    if (!Inserted && It->second != Begin + Symbol.Size)
      return fail("aliased functions disagree about their range");
  }
  for (const auto &Section : Image.Sections)
    if (Section.Name == ".eh_frame") {
      if (EH)
        return fail("duplicate object unwind section");
      EH = &Section;
    }
  if (Image.TargetArch == Arch::ARM) {
    if (!hasGeneratedELFARMEHABI(Image) || Functions.empty())
      return fail("native ARM functions require compiler EHABI coverage");
    std::vector<uint64_t> Starts;
    for (const auto &[Begin, End] : Functions)
      Starts.push_back(Begin);
    return Starts;
  }
  if (!EH || Functions.empty())
    return fail(
        "object profile requires compiler-generated CFI for every function");
  auto Records = decodeDwarfEHFrameRecords(
      llvm::ArrayRef<uint8_t>(Image.Bytes).slice(EH->Offset, EH->Size), EH->VA,
      Image.PointerWidth == 8);
  if (!Records)
    return Records.takeError();
  std::set<uint64_t> Covered;
  for (const auto &Record : *Records) {
    auto Function = Functions.find(Record.BeginVA);
    if (Function == Functions.end() || Function->second != Record.EndVA ||
        !Covered.insert(Record.BeginVA).second)
      return fail("FDE range does not match an exact object function range");
  }
  if (Covered.size() != Functions.size())
    return fail("not every object function has registered unwind coverage");
  return std::vector<uint64_t>(Covered.begin(), Covered.end());
}

} // namespace

llvm::Error
validateDynCodePatchImage(const DynCodePatchEvidence &Evidence,
                          llvm::StringRef Object, const BinaryImage &Original,
                          const CompiledImage &Compiled,
                          llvm::ArrayRef<SourcePatchMapping> Mappings,
                          llvm::ArrayRef<SourcePatchBinding> Bindings) {
  if ((Original.Arch != Arch::X64 && Original.Arch != Arch::AArch64) ||
      Compiled.TargetArch != Original.Arch ||
      Compiled.Format != Original.Format ||
      Compiled.InputKind != RewriteInputKind::ReplacementObject ||
      Mappings.size() != 1 || Mappings.front().Symbol.empty() ||
      !Bindings.empty() || Evidence.Payload.empty() ||
      Evidence.Payload.size() > MaxImageSize)
    return fail("hosted DynCode requires one explicit x64/ARM64 entry, "
                "a bounded payload and no original bindings");
  auto Parsed = llvm::json::parse(Evidence.Audit);
  if (!Parsed)
    return fail("malformed DynCode audit: " +
                llvm::toString(Parsed.takeError()));
  const auto *Audit = Parsed->getAsObject();
  if (!Audit || Audit->getString("execution_profile") != "hosted-v1" ||
      Audit->getBoolean("keep_unwind") != true ||
      Audit->getInteger("selected_section_count") != 1 ||
      Audit->getInteger("remaining_external_count") != 0 ||
      Audit->getInteger("runtime_contract_count") != 0 ||
      Audit->getInteger("padding_size") != 0 ||
      Audit->getInteger("bad_byte_hit_count") != 0 ||
      Audit->getInteger("alignment") != 1 ||
      Audit->getInteger("entry_offset") != 0 ||
      Audit->getInteger("image_size") !=
          static_cast<int64_t>(Evidence.Payload.size()))
    return fail(
        "DynCode audit does not prove the hosted single-section profile");
  auto Target = sourcePatchTargetTriple(Original);
  if (!Target)
    return Target.takeError();
  const auto Entry = Original.Format == BinaryFormat::MachO
                         ? "_" + Mappings.front().Symbol
                         : Mappings.front().Symbol;
  auto Hash = [](llvm::StringRef Bytes) {
    return llvm::toHex(
        llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
            reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size())),
        true);
  };
  if (Audit->getString("target_triple") != *Target ||
      Audit->getString("entry_symbol") != Entry ||
      Audit->getString("output_digest") != Hash(Evidence.Payload) ||
      Audit->getString("input_digest") != Hash(Object))
    return fail(
        "DynCode target, entry or artifact digest disagrees with its audit");
  const auto *Journal = Audit->getArray("journal");
  for (llvm::StringRef Key : {"binary", "plan", "relocations"}) {
    if (!Journal || llvm::count_if(*Journal, [&](const auto &Value) {
                      const auto *Record = Value.getAsObject();
                      return Record && Record->getString("key") == Key &&
                             Record->getString("provider") ==
                                 "builtin.final_verifier" &&
                             Record->getString("value") == "ok";
                    }) != 1)
      return fail("DynCode sealed final verification is incomplete");
  }
  const CompiledSection *Code = nullptr;
  for (const auto &Section : Compiled.Sections) {
    if (!Section.IsAllocated || !Section.IsInImage || !Section.Size)
      continue;
    if (Section.Kind == SectionKind::Code) {
      if (Code)
        return fail("DynCode code requires one unchanged native section");
      Code = &Section;
      continue;
    }
    const llvm::StringRef Name(Section.Name);
    const bool Metadata = Name == ".eh_frame" || Name == ".xdata" ||
                          Name == ".pdata" || Name == ".note.gnu.property" ||
                          Name == "__eh_frame" || Name == "__compact_unwind";
    if (Section.Kind != SectionKind::ReadOnlyData || !Metadata)
      return fail("hosted DynCode has unexpected native data storage: " + Name);
  }
  const auto Symbol = Compiled.SymbolAddrs.find(Mappings.front().Symbol);
  if (!Code || Code->Size != Evidence.Payload.size() ||
      !rangeInBounds(Code->Offset, Code->Size, Compiled.Bytes.size()) ||
      Symbol == Compiled.SymbolAddrs.end() || Symbol->second != Code->VA ||
      std::memcmp(Compiled.Bytes.data() + Code->Offset, Evidence.Payload.data(),
                  Evidence.Payload.size()))
    return fail("linked native code/entry layout differs from the audited "
                "DynCode payload");
  return llvm::Error::success();
}

llvm::Expected<PatchResult> installObjectPatch(
    llvm::StringRef Bytes, const BinaryImage &Original,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<SourcePatchBinding> RequestedBindings,
    const std::filesystem::path &Output,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt,
    const DynCodePatchEvidence *DynCode) {
  if (Original.Format == BinaryFormat::COFF)
    return installCOFFObjectPatch(Bytes, Original, Mappings, RequestedBindings,
                                  Output, PrepareReceipt, DynCode);
  if (Original.Format == BinaryFormat::MachO)
    return installMachOObjectPatch(Bytes, Original, Mappings, RequestedBindings,
                                   Output, PrepareReceipt, DynCode);
  if (Original.Format != BinaryFormat::ELF)
    return fail("native object format is unsupported");
  if (Mappings.empty())
    return fail("explicit entry and original address are required");
  if (auto Error = validateELFSourceFeatures(Original))
    return std::move(Error);
  auto Object = readObject(Bytes, Original);
  if (!Object)
    return Object.takeError();
  BinaryImage References = Original;
  scanImportThunks(References);
  std::set<std::string> Entries;
  std::set<va_t> Addresses;
  for (const auto &Mapping : Mappings) {
    auto Symbol = Object->NamedSymbols.find(Mapping.Symbol);
    if (Mapping.Symbol.empty() || Symbol == Object->NamedSymbols.end() ||
        !Entries.insert(Mapping.Symbol).second ||
        !Addresses.insert(Mapping.OriginalVA).second)
      return fail("missing or duplicate explicit object entry");
    const auto &Definition = Object->Symbols[Symbol->second];
    if (Definition.Type != llvm::ELF::STT_FUNC ||
        Definition.Section == llvm::ELF::SHN_UNDEF ||
        !Original.hasAuthenticatedFunctionEntryAt(Mapping.OriginalVA) ||
        !sourceEntryCanHostTrampoline(Original, Mapping.OriginalVA))
      return fail(
          "entry lacks a bounded definition or replaceable original target");
  }
  std::map<std::string, SourcePatchBinding> Bindings;
  std::set<va_t> BoundAddresses;
  for (const auto &Binding : RequestedBindings) {
    auto Symbol = Object->NamedSymbols.find(Binding.Symbol);
    if (Symbol == Object->NamedSymbols.end() ||
        Object->Symbols[Symbol->second].Section != llvm::ELF::SHN_UNDEF ||
        !Bindings.emplace(Binding.Symbol, Binding).second ||
        !BoundAddresses.insert(Binding.OriginalVA).second)
      return fail("binding requires a unique undefined object symbol");
    if (auto Error = validateSourcePatchBinding(Binding, References, Mappings))
      return std::move(Error);
    const auto &Declaration = Object->Symbols[Symbol->second];
    const auto ExpectedType = Binding.Kind == SourcePatchBindingKind::Function
                                  ? llvm::ELF::STT_FUNC
                                  : llvm::ELF::STT_OBJECT;
    if (Declaration.Type != llvm::ELF::STT_NOTYPE &&
        Declaration.Type != ExpectedType)
      return fail("binding disagrees with object symbol type: " +
                  Binding.Symbol);
    if (Binding.Kind != SourcePatchBindingKind::Function &&
        Declaration.Size > Binding.Size)
      return fail("binding is smaller than the object declaration: " +
                  Binding.Symbol);
  }
  auto EH = findELFEHFrameRegion(Original.Raw);
  auto EHABI = findELFARMEHABIRegion(Original.Raw);
  const bool ARM = Original.Arch == Arch::ARM;
  if (ARM ? !EHABI.has_value() : (!EH || !EH->HasHdr))
    return fail("original requires a registered native unwind search table");
  ELFPatcher Patcher;
  Patcher.setInputKind(RewriteInputKind::ReplacementObject);
  const uint64_t Base =
      Patcher.plannedExecSegmentVA(Original.Raw, Original.Arch);
  if (auto Error = placeObject(*Object, Original, Base,
                               ARM ? EHABI->IndexSize : EH->HdrSize))
    return std::move(Error);
  if (auto Error = relocateObject(*Object, References, Bindings))
    return std::move(Error);
  if (DynCode)
    if (auto Error = validateDynCodePatchImage(*DynCode, Bytes, Original,
                                               Object->Storage, Mappings,
                                               RequestedBindings))
      return std::move(Error);
  auto Functions = validateObjectUnwind(*Object);
  if (!Functions)
    return Functions.takeError();
  auto &Image = Object->Storage;
  const uint64_t HdrVA = Image.BaseVA + Object->MetadataOffset;
  auto Hdr = ARM ? buildRelocatedELFARMEHABIIndex(Original.Raw, *EHABI, Image,
                                                  HdrVA, *Functions)
                 : buildRelocatedELFEHFrameHdr(Original.Raw, *EH, Image,
                                               *Functions, HdrVA);
  if (!Hdr)
    return Hdr.takeError();
  auto Storage = validatePatchStorageLayout(
      Image, sourcePatchStorageAlignment(Original.Arch, Original.Format));
  if (!Storage)
    return Storage.takeError();
  const uint64_t Limit =
      Storage->WritableOffset ? Storage->WritableOffset : Image.Bytes.size();
  if (!rangeInBounds(Object->MetadataOffset, Hdr->size(), Limit))
    return fail("unwind search table exceeds its read-only reservation");
  std::copy(Hdr->begin(), Hdr->end(),
            Image.Bytes.begin() + Object->MetadataOffset);
  auto Dynamic =
      prepareELFPointerRelocations(References, Image, Object->Pointers,
                                   Object->MetadataOffset + Hdr->size());
  if (!Dynamic)
    return Dynamic.takeError();
  return publishSourcePatch(
      Original, Mappings, Output,
      [&](const std::filesystem::path &Input,
          const std::filesystem::path &Candidate)
          -> llvm::Expected<PatchResult> {
        std::string Failure;
        auto Result = BinaryPatcher::readPatchWrite(
            Input, Candidate, true, "object_patch",
            [&](std::vector<uint8_t> &Binary, PatchResult &Result) {
              auto Reject = [&](llvm::Error Error) {
                Failure = llvm::toString(std::move(Error));
                return false;
              };
              const auto Writable =
                  Storage->WritableOffset
                      ? std::optional<uint64_t>(Image.BaseVA +
                                                Storage->WritableOffset)
                      : std::nullopt;
              if (Patcher.appendPatchSegments(Binary, Image.Bytes,
                                              Original.Arch,
                                              Writable) != Image.BaseVA)
                return Reject(fail("placement changed during installation"));
              if (auto Error =
                      ARM ? retargetELFARMEHABIIndex(Binary, HdrVA, Hdr->size(),
                                                     &Image)
                          : retargetELFEHFrameHdr(Binary, HdrVA, Hdr->size()))
                return Reject(std::move(Error));
              if (auto Error = installELFSourceRelocations(Binary, *Dynamic))
                return Reject(std::move(Error));
              if (auto Error = registerELFPatchSections(Binary, Image.Sections))
                return Reject(std::move(Error));
              for (const auto &Mapping : Mappings) {
                const uint64_t Owner = Image.SymbolAddrs.at(Mapping.Symbol);
                const uint64_t Offset =
                    Original.vaToFileOffset(Mapping.OriginalVA);
                const auto Mode =
                    Original.instructionModeAt(Mapping.OriginalVA);
                const auto &Definition =
                    Object->Symbols[Object->NamedSymbols.at(Mapping.Symbol)];
                if (!Mode ||
                    (ARM &&
                     (Definition.Thumb ? InstructionMode::Thumb
                                       : InstructionMode::ARM) != *Mode) ||
                    !getTargetCodegenInfo(Original.Arch, *Mode)
                         .writeSourceTrampoline(
                             Binary, Offset, Owner, Mapping.OriginalVA,
                             getTargetCodegenInfo(Original.Arch, *Mode)
                                 .sourceTrampolineSize()))
                  return Reject(
                      fail("object entry is outside trampoline range"));
                Result.PatchedFunctions.push_back({Mapping.Symbol,
                                                   Mapping.OriginalVA, Owner,
                                                   Mapping.Symbol});
                Result.PatchedOriginalEntries.push_back(Mapping.OriginalVA);
              }
              std::sort(Result.PatchedOriginalEntries.begin(),
                        Result.PatchedOriginalEntries.end());
              Result.TrampolineCount = Mappings.size();
              Result.CodeSize = Image.Bytes.size();
              return true;
            });
        if (!Result.Success)
          return fail(Failure.empty() ? "candidate write failed" : Failure);
        return Result;
      },
      PrepareReceipt);
}

} // namespace neverd
