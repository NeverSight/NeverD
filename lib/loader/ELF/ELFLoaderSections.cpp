//===- ELFLoaderSections.cpp - ELF section and symbol tables --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reads the two tables an ELF section header table describes: the sections
/// themselves, with the file bytes each is backed by, and the symbols of
/// every SHT_SYMTAB and SHT_DYNSYM among them.
///
//===----------------------------------------------------------------------===//

#include "ELFLoaderDetail.h"
#include "ELFSymbolCollector.h"

#include "neverd/loader/BinaryImageFlags.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"

#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-elf-loader"

namespace neverd {
namespace elf_loader {
namespace detail {

namespace {

/// The ELF ABIs for the Arm architectures mark where the instruction set
/// changes or data begins with the local untyped symbols `$a`, `$d`, `$t` and
/// `$x`, each optionally followed by `.` and a suffix.  They are the
/// toolchain's markers, not names: one sits at the start of practically every
/// function and data section, at the address of the symbol that names it.

} // namespace

template <typename ELFT>
llvm::Error buildSections(llvm::ArrayRef<typename ELFT::Shdr> Sections,
                          llvm::StringRef ShStrTab, const uint8_t *Data,
                          size_t Size, const std::vector<va_t> &SecBase,
                          bool IsRelocatable, BinaryImage &Img) {
  using namespace llvm::ELF;
  using Elf_Shdr = typename ELFT::Shdr;

  // --- Sections ---
  for (uint32_t I = 0; I < Sections.size(); ++I) {
    const Elf_Shdr &SH = Sections[I];
    if (SH.sh_type == SHT_NULL)
      continue;
    Section Sec;
    llvm::StringRef Name = getSectionName<ELFT>(ShStrTab, SH);
    if (!Name.empty())
      Sec.Name = Name.str();
    Sec.VA = IsRelocatable ? SecBase[I] : static_cast<va_t>(SH.sh_addr);
    Sec.Size = SH.sh_size;
    Sec.FileOff = SH.sh_offset;
    Sec.FileSz = (SH.sh_type != SHT_NOBITS) ? SH.sh_size : 0;
    Sec.Type = SH.sh_type;
    if (SH.sh_addralign > std::numeric_limits<uint32_t>::max() ||
        (SH.sh_addralign != 0 &&
         (SH.sh_addralign & (SH.sh_addralign - 1)) != 0))
      return llvm::make_error<llvm::StringError>(
          "elf: invalid section alignment", llvm::inconvertibleErrorCode());
    Sec.Alignment =
        SH.sh_addralign ? static_cast<uint32_t>(SH.sh_addralign) : 1;
    Sec.Flags = elfSHFlagsToNd(SH.sh_flags);
    if (Sec.Size > InvalidVA - Sec.VA)
      return llvm::make_error<llvm::StringError>(
          "elf: section virtual address range overflows",
          llvm::inconvertibleErrorCode());
    if (Sec.FileSz > 0 && !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
      return llvm::make_error<llvm::StringError>(
          "elf: section file range is out of bounds",
          llvm::inconvertibleErrorCode());
    if (Sec.FileSz > 0)
      Sec.Data.assign(Data + SH.sh_offset, Data + SH.sh_offset + SH.sh_size);
    Img.Sections.push_back(std::move(Sec));
  }

  return llvm::Error::success();
}

template <typename ELFT>
llvm::Error collectSymbols(const llvm::object::ELFFile<ELFT> &ELF,
                           llvm::ArrayRef<typename ELFT::Shdr> Sections,
                           llvm::ArrayRef<typename ELFT::Shdr> OwnerSections,
                           size_t Size, const std::vector<va_t> &SecBase,
                           bool IsRelocatable,
                           const std::map<std::string, va_t> &CommonSlots,
                           BinaryImage &Img) {
  using namespace llvm::ELF;
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Sym = typename ELFT::Sym;

  // A definition may be present in both .symtab and .dynsym. Keep aliases
  // distinct by name, and retain different symbol types at the same address.
  ELFSymbolCollector Symbols(Img);
  std::set<std::pair<va_t, llvm::StringRef>> ExportKeys;
  std::optional<InstructionMode> ARMMode;
  std::map<va_t, InstructionMode> ARMCodeModes;
  std::map<std::pair<uint32_t, va_t>, ARMCodeRegionKind> ARMMappingPoints;
  auto RecordARMMode = [&](va_t Address, InstructionMode Mode) -> llvm::Error {
    const auto [It, Inserted] = ARMCodeModes.emplace(Address, Mode);
    if (!Inserted && It->second != Mode)
      return llvm::make_error<llvm::StringError>(
          "elf: conflicting ARM/Thumb instruction modes at one code address",
          llvm::inconvertibleErrorCode());
    // Valid mixed images remain available to symbol, section and relocation
    // clients. Consumers needing one instruction mode reject this state.
    ARMMode =
        ARMMode && *ARMMode != Mode ? InstructionMode::MixedARMThumb : Mode;
    return llvm::Error::success();
  };
  if (Img.Arch == Arch::ARM && !IsRelocatable && ELF.getHeader().e_entry != 0 &&
      Img.hasExecutableCodeOwnerAt(Img.Entry))
    if (auto Error = RecordARMMode(Img.Entry, (ELF.getHeader().e_entry & 1u)
                                                  ? InstructionMode::Thumb
                                                  : InstructionMode::ARM))
      return Error;

  // --- Symbol tables ---
  auto AddSymbolsFrom = [&](const Elf_Shdr &SH) -> llvm::Error {
    if (SH.sh_type != SHT_SYMTAB && SH.sh_type != SHT_DYNSYM)
      return llvm::Error::success();
    // A linked image read without its static symbol table keeps what strip
    // leaves: .dynsym, which dynamic linking needs.
    if (SH.sh_type == SHT_SYMTAB && !IsRelocatable && !Img.ReadsStaticSymbols)
      return llvm::Error::success();
    if (!rangeInBounds(SH.sh_offset, SH.sh_size, Size) || SH.sh_entsize == 0)
      return llvm::Error::success();

    auto SymsOr = ELF.symbols(&SH);
    if (!SymsOr) {
      llvm::consumeError(SymsOr.takeError());
      return llvm::Error::success();
    }
    const auto StrTab = getLinkedStringTable<ELFT>(ELF, Sections, SH);
    if (StrTab.empty())
      return llvm::Error::success();

    for (size_t I = 1; I < SymsOr->size(); ++I) {
      const Elf_Sym &Sym = (*SymsOr)[I];
      if (Sym.st_shndx == SHN_UNDEF)
        continue;

      auto NameOr = Sym.getName(StrTab);
      if (!NameOr) {
        llvm::consumeError(NameOr.takeError());
        continue;
      }
      if (NameOr->empty())
        continue;

      const uint8_t Bind = Sym.getBinding();
      const uint8_t Type = Sym.getType();
      const bool IsFunction = Type == STT_FUNC || Type == STT_GNU_IFUNC;
      // The low bit belongs to the ELF symbol value, before a relocatable
      // section is placed in the image's synthesized address space.
      va_t Value = Img.Arch == Arch::ARM && IsFunction
                       ? clearThumbBit(Sym.st_value)
                       : static_cast<va_t>(Sym.st_value);
      if (IsRelocatable && Sym.st_shndx < SHN_LORESERVE &&
          Sym.st_shndx < SecBase.size()) {
        if (Value > InvalidVA - SecBase[Sym.st_shndx])
          continue;
        Value += SecBase[Sym.st_shndx];
      }
      // A common symbol's value is its alignment; it lives where the loader
      // allocated its storage, as a linker allocates it in .bss.
      if (Sym.st_shndx == SHN_COMMON) {
        auto It = CommonSlots.find(NameOr->str());
        if (It == CommonSlots.end())
          continue;
        Value = It->second;
      }
      // Relocatable .o symbols at section start have st_value==0; SecBase may
      // also be 0 — do not treat that as "no address".
      if (Value == 0 && Sym.st_value == 0 && !IsRelocatable)
        continue;

      if (Img.Arch == Arch::ARM) {
        const bool IsARMMapping =
            Bind == STB_LOCAL && Type == STT_NOTYPE &&
            (*NameOr == "$a" || NameOr->starts_with("$a."));
        const bool IsThumbMapping =
            Bind == STB_LOCAL && Type == STT_NOTYPE &&
            (*NameOr == "$t" || NameOr->starts_with("$t."));
        const bool IsDataMapping =
            Bind == STB_LOCAL && Type == STT_NOTYPE &&
            (*NameOr == "$d" || NameOr->starts_with("$d."));
        const auto SymbolMode =
            IsThumbMapping || (IsFunction && (Sym.st_value & 1u))
                ? InstructionMode::Thumb
                : InstructionMode::ARM;
        if ((IsFunction || IsARMMapping || IsThumbMapping || IsDataMapping) &&
            Sym.st_shndx < SHN_LORESERVE &&
            Sym.st_shndx < OwnerSections.size()) {
          const Elf_Shdr &Owner = OwnerSections[Sym.st_shndx];
          const va_t OwnerVA =
              sectionVA<ELFT>(IsRelocatable, SecBase, Owner, Sym.st_shndx);
          if ((Owner.sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) ==
                  (SHF_ALLOC | SHF_EXECINSTR) &&
              Value >= OwnerVA && Value - OwnerVA < Owner.sh_size) {
            const unsigned Alignment = IsDataMapping ? 1
                                       : SymbolMode == InstructionMode::Thumb
                                           ? 2
                                           : 4;
            if (Value % Alignment != 0)
              return llvm::make_error<llvm::StringError>(
                  "elf: misaligned ARM/Thumb code-mode symbol",
                  llvm::inconvertibleErrorCode());
            if (IsARMMapping || IsThumbMapping || IsDataMapping) {
              const ARMCodeRegionKind Kind =
                  IsDataMapping    ? ARMCodeRegionKind::Data
                  : IsThumbMapping ? ARMCodeRegionKind::Thumb
                                   : ARMCodeRegionKind::ARM;
              const auto [It, Inserted] = ARMMappingPoints.emplace(
                  std::make_pair(Sym.st_shndx, Value), Kind);
              if (!Inserted && It->second != Kind)
                return llvm::make_error<llvm::StringError>(
                    "elf: conflicting ARM/Thumb mapping symbols",
                    llvm::inconvertibleErrorCode());
            }
            if (!IsDataMapping)
              if (auto Error = RecordARMMode(Value, SymbolMode))
                return Error;
          }
        } else if (OwnerSections.empty() && !IsRelocatable && IsFunction &&
                   Sym.st_shndx < SHN_LORESERVE &&
                   Img.hasExecutableCodeOwnerAt(Value)) {
          // In a sectionless image st_shndx still names the removed table.
          // The defined dynamic function value retains its ARM/Thumb bit;
          // PT_LOAD supplies code ownership, not an invented section index.
          const unsigned Alignment =
              SymbolMode == InstructionMode::Thumb ? 2 : 4;
          if (Value % Alignment != 0)
            return llvm::make_error<llvm::StringError>(
                "elf: misaligned ARM/Thumb code-mode symbol",
                llvm::inconvertibleErrorCode());
          if (auto Error = RecordARMMode(Value, SymbolMode))
            return Error;
        }
      }

      // A mapping marker names nothing; ARM read its mode above.
      if ((Img.Arch == Arch::ARM || Img.Arch == Arch::AArch64) &&
          Bind == STB_LOCAL && Type == STT_NOTYPE &&
          isMappingSymbolName(*NameOr))
        continue;

      Symbols.add(*NameOr, Value, Sym.st_size, Type, IsFunction);

      // st_size is an object boundary only for a defined STT_OBJECT whose
      // complete range belongs to one allocated section.  STT_NOTYPE,
      // section symbols, TLS, COMMON, and distances to neighbouring symbols
      // are not interchangeable with a C object extent.
      if (Type == STT_OBJECT && Sym.st_size != 0 &&
          Sym.st_shndx < SHN_LORESERVE && Sym.st_shndx < OwnerSections.size()) {
        const Elf_Shdr &Owner = OwnerSections[Sym.st_shndx];
        const va_t OwnerVA =
            sectionVA<ELFT>(IsRelocatable, SecBase, Owner, Sym.st_shndx);
        if ((Owner.sh_flags & SHF_ALLOC) != 0 &&
            Owner.sh_size <= InvalidVA - OwnerVA &&
            Sym.st_size <= InvalidVA - Value) {
          const va_t OwnerEnd = OwnerVA + Owner.sh_size;
          const va_t ObjectEnd = Value + Sym.st_size;
          if (Value >= OwnerVA && ObjectEnd <= OwnerEnd)
            Img.ExactDataObjects.push_back(ExactDataObjectExtent{
                Value, Sym.st_size, ExactDataObjectEvidence::ELFObjectSymbol,
                ExactDataObjectPrecision::Storage});
        }
      }

      if (Type == STT_GNU_IFUNC)
        Img.recordRuntimeFunction(Value);

      if (IsFunction && (Bind == STB_GLOBAL || Bind == STB_WEAK) &&
          ExportKeys.emplace(Value, *NameOr).second) {
        Export Exp;
        Exp.Name = NameOr->str();
        Exp.Addr = Value;
        Img.Exports.push_back(std::move(Exp));
      }
    }
    return llvm::Error::success();
  };

  for (const Elf_Shdr &SH : Sections)
    if (auto Error = AddSymbolsFrom(SH))
      return Error;
  if (Img.Arch == Arch::ARM) {
    for (uint32_t Index = 0; Index < OwnerSections.size(); ++Index) {
      const Elf_Shdr &Owner = OwnerSections[Index];
      if ((Owner.sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) !=
              (SHF_ALLOC | SHF_EXECINSTR) ||
          Owner.sh_size == 0)
        continue;
      const va_t Start = sectionVA<ELFT>(IsRelocatable, SecBase, Owner, Index);
      if (Owner.sh_size > InvalidVA - Start)
        return llvm::make_error<llvm::StringError>(
            "elf: ARM code mapping range overflows",
            llvm::inconvertibleErrorCode());
      const va_t End = Start + Owner.sh_size;
      auto Point = ARMMappingPoints.lower_bound({Index, Start});
      while (Point != ARMMappingPoints.end() && Point->first.first == Index) {
        const auto Next = std::next(Point);
        const va_t RegionEnd =
            Next != ARMMappingPoints.end() && Next->first.first == Index
                ? Next->first.second
                : End;
        if (Point->first.second < RegionEnd)
          Img.ARMCodeRegions.push_back(
              {Point->first.second, RegionEnd, Point->second});
        Point = Next;
      }
    }
    std::sort(Img.ARMCodeRegions.begin(), Img.ARMCodeRegions.end(),
              [](const ARMCodeRegion &Left, const ARMCodeRegion &Right) {
                return Left.Start < Right.Start;
              });
    for (size_t Index = 1; Index < Img.ARMCodeRegions.size(); ++Index)
      if (Img.ARMCodeRegions[Index].Start < Img.ARMCodeRegions[Index - 1].End)
        return llvm::make_error<llvm::StringError>(
            "elf: overlapping ARM code mapping regions",
            llvm::inconvertibleErrorCode());
    Img.ARMCodeModeEntries = std::move(ARMCodeModes);
    Img.Mode = ARMMode.value_or(InstructionMode::ARM);
    for (const auto &[Address, Mode] : Img.ARMCodeModeEntries) {
      const auto Mapped = Img.instructionModeAt(Address);
      if (!Mapped || *Mapped != Mode)
        return llvm::make_error<llvm::StringError>(
            "elf: conflicting ARM/Thumb instruction modes at one code address",
            llvm::inconvertibleErrorCode());
    }
  }
  return llvm::Error::success();
}

// ===--------------------------------------------------------------------===//
// Explicit template instantiations for ELF32 and ELF64
// ===--------------------------------------------------------------------===//

template llvm::Error buildSections<llvm::object::ELF32LE>(
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, llvm::StringRef,
    const uint8_t *, size_t, const std::vector<va_t> &, bool, BinaryImage &);
template llvm::Error buildSections<llvm::object::ELF64LE>(
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, llvm::StringRef,
    const uint8_t *, size_t, const std::vector<va_t> &, bool, BinaryImage &);

template llvm::Error collectSymbols<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, size_t,
    const std::vector<va_t> &, bool, const std::map<std::string, va_t> &,
    BinaryImage &);
template llvm::Error collectSymbols<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, size_t,
    const std::vector<va_t> &, bool, const std::map<std::string, va_t> &,
    BinaryImage &);

} // namespace detail
} // namespace elf_loader
} // namespace neverd
