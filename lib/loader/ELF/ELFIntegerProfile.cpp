//===- ELFIntegerProfile.cpp - RISC-V and PowerPC ELF profile admission -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate the class, encoding, ABI and extension evidence at the ELF
/// boundary.
///
//===----------------------------------------------------------------------===//

#include "ELFIntegerProfile.h"

#include "neverd/ArchSupport.h"
#include "neverd/loader/ELF/PowerPCABI.h"

#include "llvm/ADT/TypeSwitch.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/RISCVAttributeParser.h"
#include "llvm/TargetParser/RISCVISAInfo.h"

#include <cstring>
#include <optional>

namespace neverd::elf_loader {
namespace {
template <typename ELFT>
llvm::Error readPPC64Profile(const llvm::object::ELFFile<ELFT> &File,
                             llvm::ArrayRef<typename ELFT::Shdr> Sections,
                             Arch &A, PPC64LinkageTables *Linkage) {
  using namespace llvm::ELF;
  const auto &Header = File.getHeader();
  llvm::StringRef SectionNames;
  if (Header.e_shstrndx != SHN_UNDEF) {
    auto Names = File.getSectionStringTable(Sections);
    if (!Names)
      return Names.takeError();
    SectionNames = *Names;
  }
  PPC64LinkageTables Tables;
  for (const auto &S : Sections) {
    if (SectionNames.empty() || !S.sh_name)
      continue;
    auto Name = File.getSectionName(S, SectionNames);
    if (!Name)
      return Name.takeError();
    if (*Name != ppc_elf::DescriptorSection)
      continue;
    if (Tables.Size || S.sh_type != SHT_PROGBITS ||
        (S.sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) != SHF_ALLOC || !S.sh_size ||
        (S.sh_entsize && ((S.sh_entsize != ppc_elf::EnvironmentOffset &&
                           S.sh_entsize != ppc_elf::DescriptorSize) ||
                          S.sh_size % S.sh_entsize)))
      return llvm::createStringError("elf: invalid PPC64 .opd section");
    Tables = {S.sh_addr, S.sh_size, static_cast<unsigned>(S.sh_entsize)};
  }
  std::optional<va_t> Address, Size;
  std::optional<va_t> JumpRel, JumpSize, JumpType, Glink, Options;
  auto Segments = File.program_headers();
  if (!Segments)
    return Segments.takeError();
  for (const auto &P : *Segments) {
    if (P.p_type != PT_DYNAMIC)
      continue;
    auto Bytes = File.getSegmentContents(P);
    if (!Bytes)
      return Bytes.takeError();
    if (Bytes->size() % sizeof(typename ELFT::Dyn))
      return llvm::createStringError("elf: truncated PPC64 dynamic table");
    bool Terminated = false;
    for (size_t I = 0; I + sizeof(typename ELFT::Dyn) <= Bytes->size();
         I += sizeof(typename ELFT::Dyn)) {
      typename ELFT::Dyn D;
      std::memcpy(&D, Bytes->data() + I, sizeof(D));
      if (D.d_tag == DT_NULL) {
        Terminated = true;
        break;
      }
      std::optional<va_t> *Field = nullptr;
      switch (D.d_tag) {
      case ppc_elf::DynamicOPD:
        Field = &Address;
        break;
      case ppc_elf::DynamicOPDSize:
        Field = &Size;
        break;
      case DT_JMPREL:
        Field = &JumpRel;
        break;
      case DT_PLTRELSZ:
        Field = &JumpSize;
        break;
      case DT_PLTREL:
        Field = &JumpType;
        break;
      case ppc_elf::DynamicGlink:
        Field = &Glink;
        break;
      case ppc_elf::DynamicOptions:
        Field = &Options;
        break;
      default:
        continue;
      }
      if (*Field)
        return llvm::createStringError("elf: duplicate PPC64 dynamic tag");
      *Field = D.d_un.d_val;
    }
    if (!Terminated)
      return llvm::createStringError("elf: unterminated PPC64 dynamic table");
  }
  if (Address || Size) {
    if (!Address || !Size || !*Size ||
        (Tables.Size && (Tables.Address != *Address || Tables.Size != *Size)))
      return llvm::createStringError(
          "elf: conflicting PPC64 descriptor tables");
    Tables = {*Address, *Size, Tables.EntrySize, true};
  }
  if (Tables.Size && Header.e_flags == ppc_elf::ELFv2)
    return llvm::createStringError("elf: PPC64 ELFv2 conflicts with .opd");
  if (Tables.Size) {
    if (Tables.Address % ppc_elf::WordSize || Tables.Size % ppc_elf::WordSize ||
        Tables.Size < ppc_elf::EnvironmentOffset ||
        Tables.Size > InvalidVA - Tables.Address)
      return llvm::createStringError(
          "elf: invalid PPC64 descriptor table bounds");
    A = ELFT::Endianness == llvm::endianness::little ? Arch::PPC64LEELFv1
                                                     : Arch::PPC64ELFv1;
  } else if (Header.e_flags != ppc_elf::ELFv2)
    return llvm::createStringError("elf: missing PPC64 ABI or .opd evidence");
  if (JumpRel || JumpSize || JumpType) {
    if (!JumpRel || !JumpSize || JumpType != DT_RELA ||
        *JumpSize % sizeof(typename ELFT::Rela) ||
        *JumpSize > InvalidVA - *JumpRel)
      return llvm::createStringError("elf: invalid PPC64 PLT relocation table");
    Tables.JumpRelocations = *JumpRel;
    Tables.JumpRelocationSize = *JumpSize;
  }
  Tables.Glink = Glink;
  Tables.PLTLocalEntry = Options.value_or(0) & ppc_elf::LocalEntryOption;
  if (Linkage)
    *Linkage = Tables;
  return llvm::Error::success();
}

// Identify and load must agree even when ELF machine, class and flags conflict.
template <typename ELFT>
llvm::Error readProfile(const llvm::object::ELFObjectFile<ELFT> &Obj, Arch &A,
                        RISCVProfile &RV, PPC64LinkageTables *Linkage) {
  using namespace llvm::ELF;
  const auto &File = Obj.getELFFile();
  const auto &Header = File.getHeader();
  if ((isRISCV64(A) || isPowerPC64(A)) != ELFT::Is64Bits)
    return llvm::createStringError("elf: machine and ELF class disagree");
  if (Header.e_type != ET_EXEC && Header.e_type != ET_DYN &&
      Header.e_type != ET_REL)
    return llvm::createStringError("elf: unsupported integer ELF image type");
  if (Header.e_ident[EI_OSABI] != ELFOSABI_NONE &&
      Header.e_ident[EI_OSABI] != ELFOSABI_LINUX)
    return llvm::createStringError("elf: unsupported integer ELF platform ABI");

  if (isRISCV(A)) {
    if constexpr (ELFT::Endianness != llvm::endianness::little)
      return llvm::createStringError("elf: unsupported big-endian RISC-V");
    const uint32_t ABI = Header.e_flags & EF_RISCV_FLOAT_ABI;
    if ((Header.e_flags & ~uint32_t(EF_RISCV_RVC | EF_RISCV_FLOAT_ABI)) ||
        (ABI != EF_RISCV_FLOAT_ABI_SOFT && ABI != EF_RISCV_FLOAT_ABI_DOUBLE))
      return llvm::createStringError("elf: unsupported RISC-V ABI flags");
    if (ABI == EF_RISCV_FLOAT_ABI_DOUBLE)
      A = ELFT::Is64Bits ? Arch::RISCV64D : Arch::RISCV32D;
  } else {
    if (isPowerPC64(A) ? Header.e_flags > ppc_elf::ELFv2 : Header.e_flags != 0)
      return llvm::createStringError("elf: unsupported PowerPC ABI flags");
    auto Segments = File.program_headers();
    if (!Segments)
      return Segments.takeError();
    for (const auto &P : *Segments)
      if (P.p_type == PT_LOAD && (P.p_flags & PF_X) &&
          (P.p_flags & ppc_elf::VLE))
        return llvm::createStringError(
            "elf: PowerPC VLE segments are unsupported");
  }

  auto Sections = File.sections();
  if (!Sections)
    return Sections.takeError();
  bool SeenAttributes = false;
  for (const auto &S : *Sections) {
    if (isRISCV(A) && S.sh_type == SHT_RISCV_ATTRIBUTES) {
      if (SeenAttributes)
        return llvm::createStringError(
            "elf: duplicate RISC-V attributes sections");
      SeenAttributes = true;
      auto Bytes = File.getSectionContents(S);
      if (!Bytes)
        return Bytes.takeError();
      if (Bytes->empty())
        return llvm::createStringError("elf: empty RISC-V attributes");
      llvm::RISCVAttributeParser Attrs;
      if (auto E = Attrs.parse(*Bytes, ELFT::Endianness))
        return E;
      if (auto Align = Attrs.getAttributeValue(llvm::RISCVAttrs::STACK_ALIGN);
          Align &&
          *Align != guaranteedEntryStackAlignment(A, BinaryFormat::ELF))
        return llvm::createStringError(
            "elf: unsupported RISC-V stack alignment for the integer ABI");
      if (auto ArchString = Attrs.getAttributeString(llvm::RISCVAttrs::ARCH)) {
        auto Parsed =
            llvm::RISCVISAInfo::parseNormalizedArchString(*ArchString);
        if (!Parsed)
          return Parsed.takeError();
        const auto &ISA = **Parsed;
        if (ISA.getXLen() != Obj.getBytesInAddress() * 8)
          return llvm::createStringError("elf: RISC-V architecture attribute "
                                         "conflicts with integer ELF class");
        const auto &Extensions = ISA.getExtensions();
        const bool SupportedRegisterFile = !Extensions.count("q") &&
                                           !Extensions.count("zfinx") &&
                                           !Extensions.count("zdinx");
        if (hasRISCVDoubleABI(A) &&
            (!Extensions.count("d") || !SupportedRegisterFile))
          return llvm::createStringError(
              "elf: double-float RISC-V ABI conflicts with ISA attributes");
        struct Extension {
          const char *Name;
          unsigned Major;
          unsigned MaxMinor;
          bool RISCVProfile::*Member;
        };
        static constexpr Extension Supported[] = {
#define NEVERD_RISCV_BASE(Name, Major, MaxMinor)                               \
  {Name, Major, MaxMinor, nullptr},
#define NEVERD_RISCV_EXTENSION(Member, Name, Major, MaxMinor, ...)             \
  {Name, Major, MaxMinor, &RISCVProfile::Member},
#include "neverd/lift/RISCV/RISCVExtensions.def"
        };
        for (const auto &Spec : Supported) {
          const auto It = Extensions.find(Spec.Name);
          if (It == Extensions.end()) {
            if (!Spec.Member)
              return llvm::createStringError(
                  "elf: missing RISC-V integer base");
            continue;
          }
          if (It->second.Major != Spec.Major ||
              It->second.Minor > Spec.MaxMinor)
            return llvm::createStringError("elf: unsupported RISC-V %s version",
                                           Spec.Name);
          if (Spec.Member)
            RV.*Spec.Member = true;
        }
        if (!SupportedRegisterFile)
          RV.SingleFloat = RV.DoubleFloat = RV.Zcf = RV.Zcd = false;
      }
    }
    if (isPowerPC(A) && (S.sh_flags & SHF_EXECINSTR) &&
        (S.sh_flags & ppc_elf::VLE))
      return llvm::createStringError(
          "elf: PowerPC VLE sections are unsupported");
    if ((S.sh_type == SHT_REL || S.sh_type == SHT_RELA) && S.sh_size &&
        Header.e_type == ET_REL) {
      if (S.sh_info >= Sections->size())
        return llvm::createStringError(
            "elf: invalid relocation target section");
      if ((*Sections)[S.sh_info].sh_flags & SHF_ALLOC)
        return llvm::createStringError("elf: link RISC-V/PowerPC allocated "
                                       "relocations before native analysis");
    }
  }
  if (isPowerPC64(A))
    return readPPC64Profile(File, *Sections, A, Linkage);
  if (hasRISCVDoubleABI(A))
    RV.SingleFloat = RV.DoubleFloat = true;
  if (isRISCV(A) && (Header.e_flags & EF_RISCV_RVC) && !RV.Zca)
    RV.Compressed = true;
  return llvm::Error::success();
}

} // namespace

llvm::Error readIntegerProfile(const llvm::object::ObjectFile &Object, Arch &A,
                               RISCVProfile &Profile,
                               PPC64LinkageTables *Linkage) {
  using namespace llvm::object;
  return llvm::TypeSwitch<const ObjectFile *, llvm::Error>(&Object)
      .Case<ELF32LEObjectFile, ELF32BEObjectFile, ELF64LEObjectFile,
            ELF64BEObjectFile>([&](const auto *ELF) {
        return readProfile(*ELF, A, Profile, Linkage);
      })
      .Default([](const ObjectFile *) {
        return llvm::createStringError("elf: unsupported class");
      });
}
} // namespace neverd::elf_loader
