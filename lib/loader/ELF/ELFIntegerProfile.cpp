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
                             Arch &A, PPC64DescriptorTable *Descriptors) {
  using namespace llvm::ELF;
  const auto &Header = File.getHeader();
  llvm::StringRef SectionNames;
  if (Header.e_shstrndx != SHN_UNDEF) {
    auto Names = File.getSectionStringTable(Sections);
    if (!Names)
      return Names.takeError();
    SectionNames = *Names;
  }
  PPC64DescriptorTable OPD;
  for (const auto &S : Sections) {
    if (SectionNames.empty() || !S.sh_name)
      continue;
    auto Name = File.getSectionName(S, SectionNames);
    if (!Name)
      return Name.takeError();
    if (*Name != ppc_elf::DescriptorSection)
      continue;
    if (OPD.Size || S.sh_type != SHT_PROGBITS ||
        (S.sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) != SHF_ALLOC || !S.sh_size ||
        (S.sh_entsize && ((S.sh_entsize != ppc_elf::EnvironmentOffset &&
                           S.sh_entsize != ppc_elf::DescriptorSize) ||
                          S.sh_size % S.sh_entsize)))
      return llvm::createStringError("elf: invalid PPC64 .opd section");
    OPD = {S.sh_addr, S.sh_size, static_cast<unsigned>(S.sh_entsize)};
  }
  std::optional<va_t> Address, Size;
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
      if (D.d_tag != ppc_elf::DynamicOPD && D.d_tag != ppc_elf::DynamicOPDSize)
        continue;
      auto &Field = D.d_tag == ppc_elf::DynamicOPD ? Address : Size;
      if (Field)
        return llvm::createStringError("elf: duplicate PPC64 descriptor tag");
      Field = D.d_un.d_val;
    }
    if (!Terminated)
      return llvm::createStringError("elf: unterminated PPC64 dynamic table");
  }
  if (Address || Size) {
    if (!Address || !Size || !*Size ||
        (OPD.Size && (OPD.Address != *Address || OPD.Size != *Size)))
      return llvm::createStringError(
          "elf: conflicting PPC64 descriptor tables");
    OPD = {*Address, *Size, OPD.EntrySize, true};
  }
  if (OPD.Size && Header.e_flags == ppc_elf::ELFv2)
    return llvm::createStringError("elf: PPC64 ELFv2 conflicts with .opd");
  if (OPD.Size) {
    if (OPD.Address % ppc_elf::WordSize || OPD.Size % ppc_elf::WordSize ||
        OPD.Size < ppc_elf::EnvironmentOffset ||
        OPD.Size > InvalidVA - OPD.Address)
      return llvm::createStringError(
          "elf: invalid PPC64 descriptor table bounds");
    A = ELFT::Endianness == llvm::endianness::little ? Arch::PPC64LEELFv1
                                                     : Arch::PPC64ELFv1;
    if (Descriptors)
      *Descriptors = OPD;
  } else if (Header.e_flags != ppc_elf::ELFv2)
    return llvm::createStringError("elf: missing PPC64 ABI or .opd evidence");
  return llvm::Error::success();
}

// Identify and load must agree even when ELF machine, class and flags conflict.
template <typename ELFT>
llvm::Error readProfile(const llvm::object::ELFObjectFile<ELFT> &Obj, Arch &A,
                        RISCVProfile &RV, PPC64DescriptorTable *Descriptors) {
  using namespace llvm::ELF;
  const auto &File = Obj.getELFFile();
  const auto &Header = File.getHeader();
  if ((A == Arch::RISCV64 || isPowerPC64(A)) != ELFT::Is64Bits)
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
    if (Header.e_flags & ~uint32_t(EF_RISCV_RVC))
      return llvm::createStringError(
          "elf: RISC-V requires the integer ILP32 or LP64 ABI");
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
    return readPPC64Profile(File, *Sections, A, Descriptors);
  if (isRISCV(A) && (Header.e_flags & EF_RISCV_RVC) && !RV.Zca)
    RV.Compressed = true;
  return llvm::Error::success();
}

} // namespace

llvm::Error readIntegerProfile(const llvm::object::ObjectFile &Object, Arch &A,
                               RISCVProfile &Profile,
                               PPC64DescriptorTable *Descriptors) {
  using namespace llvm::object;
  return llvm::TypeSwitch<const ObjectFile *, llvm::Error>(&Object)
      .Case<ELF32LEObjectFile, ELF32BEObjectFile, ELF64LEObjectFile,
            ELF64BEObjectFile>([&](const auto *ELF) {
        return readProfile(*ELF, A, Profile, Descriptors);
      })
      .Default([](const ObjectFile *) {
        return llvm::createStringError("elf: unsupported class");
      });
}
} // namespace neverd::elf_loader
