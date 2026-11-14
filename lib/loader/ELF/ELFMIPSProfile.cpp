//===- ELFMIPSProfile.cpp - MIPS ELF profile admission -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate the class, ISA, ABI and encoding evidence before mapping MIPS ELF.
///
//===----------------------------------------------------------------------===//

#include "ELFMIPSProfile.h"

#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MipsABIFlags.h"

#include <cstring>
#include <optional>

namespace neverd::elf_loader {
namespace {

// GNU ELF AFL_ASE_MIPS16E2; not yet named by the pinned LLVM ABI header.
constexpr uint32_t kMIPS16E2ASE = 0x00004000;

llvm::Error profileError(const llvm::Twine &Reason) {
  return llvm::make_error<llvm::StringError>("elf: MIPS " + Reason,
                                             llvm::inconvertibleErrorCode());
}

template <typename ELFT>
llvm::Expected<MIPSImageMetadata>
readProfile(const llvm::object::ELFObjectFile<ELFT> &Object) {
  using namespace llvm::ELF;
  const auto &ELF = Object.getELFFile();
  const auto &Header = ELF.getHeader();
  if (Header.e_type != ET_EXEC && Header.e_type != ET_DYN)
    return profileError("only linked ET_EXEC/ET_DYN files are supported");
  const uint32_t Flags = Header.e_flags;
  const bool N32 = Flags & EF_MIPS_ABI2;
  if (N32 && (ELFT::Is64Bits || (Flags & (EF_MIPS_ABI | EF_MIPS_32BITMODE))))
    return profileError("n32 requires ELF32 with 64-bit registers and ABI2");
  const bool Wide = ELFT::Is64Bits || N32;
  if (Wide && (Flags & EF_MIPS_ARCH_ASE_M16))
    return profileError("MIPS16 64-bit encoding is not supported");
  if ((Flags & EF_MIPS_MICROMIPS) && (Flags & EF_MIPS_ARCH_ASE_M16))
    return profileError("conflicting microMIPS and MIPS16 capabilities");
  if (Flags &
      ((EF_MIPS_ARCH_ASE & ~(EF_MIPS_MICROMIPS | EF_MIPS_ARCH_ASE_M16)) |
       EF_MIPS_MACH))
    return profileError("vendor/ASE profile is not supported");
  const uint32_t ISA = Flags & EF_MIPS_ARCH;
  const bool R6 = ISA == (Wide ? EF_MIPS_ARCH_64R6 : EF_MIPS_ARCH_32R6);
  if (!R6 && ISA != (Wide ? EF_MIPS_ARCH_64R2 : EF_MIPS_ARCH_32R2))
    return profileError(
        "requires matching ELF class and MIPS32/MIPS64 r2 or r6");
  if ((Flags & EF_MIPS_ABI) != (Wide ? 0u : EF_MIPS_ABI_O32) ||
      (Flags & EF_MIPS_32BITMODE))
    return profileError("requires ELF32/o32, ELF32/n32 or ELF64/n64 ABI");
  constexpr uint32_t KnownFlags =
      EF_MIPS_NOREORDER | EF_MIPS_PIC | EF_MIPS_CPIC | EF_MIPS_ABI2 |
      EF_MIPS_ABI | EF_MIPS_FP64 | EF_MIPS_NAN2008 | EF_MIPS_ARCH |
      EF_MIPS_MICROMIPS | EF_MIPS_ARCH_ASE_M16;
  if (Flags & ~KnownFlags)
    return profileError("unrecognized ELF flags");

  MIPSImageMetadata Profile;
  Profile.ABI = N32 ? MIPSABI::N32 : Wide ? MIPSABI::N64 : MIPSABI::O32;
  Profile.ELFFlags = Flags;
  Profile.ISARevision = R6 ? 6 : 2;
  Profile.HasMicroMIPS = Flags & EF_MIPS_MICROMIPS;
  Profile.HasMIPS16 = Flags & EF_MIPS_ARCH_ASE_M16;
  const bool Compressed = Profile.HasMicroMIPS || Profile.HasMIPS16;
  if (R6 && Profile.HasMIPS16)
    return profileError("Release 6 compressed profile is unsupported");
  const auto CompressedMode =
      Profile.HasMIPS16 ? InstructionMode::MIPS16 : InstructionMode::MicroMIPS;
  const auto AddEntry = [&](uint64_t Address,
                            InstructionMode Mode) -> llvm::Error {
    if (Address % instructionModeAlignment(Mode))
      return profileError("misaligned code-mode entry");
    const auto [It, Added] = Profile.CodeModeEntries.emplace(Address, Mode);
    if (!Added && It->second != Mode)
      return profileError("conflicting instruction modes at code entry");
    return llvm::Error::success();
  };
  if ((Header.e_entry & 1) && !Compressed)
    return profileError("tagged entry without compressed MIPS capability");
  if (Header.e_entry && Compressed) {
    if (auto Error =
            AddEntry(Header.e_entry & ~uint64_t{1},
                     (Header.e_entry & 1) ? CompressedMode
                                          : InstructionMode::MIPSClassic))
      return std::move(Error);
  }
  const auto ReadSymbol = [&](const typename ELFT::Sym &Symbol) -> llvm::Error {
    const bool MIPS16 = (Symbol.st_other & 0xf0) == STO_MIPS_MIPS16;
    const bool Micro = !MIPS16 && (Symbol.st_other & STO_MIPS_MICROMIPS);
    if (MIPS16 && !Profile.HasMIPS16)
      return profileError("MIPS16 symbol encoding without ELF capability");
    if (Micro && !Profile.HasMicroMIPS)
      return profileError("microMIPS symbol encoding without ELF capability");
    if (!Compressed)
      return llvm::Error::success();
    if (Symbol.st_shndx == SHN_UNDEF ||
        (Symbol.getType() != STT_FUNC && Symbol.getType() != STT_GNU_IFUNC))
      return llvm::Error::success();
    const uint64_t Start = Micro || MIPS16 ? Symbol.st_value & ~uint64_t{1}
                                           : uint64_t(Symbol.st_value);
    const auto Mode =
        Micro || MIPS16 ? CompressedMode : InstructionMode::MIPSClassic;
    if (auto Error = AddEntry(Start, Mode))
      return Error;
    const uint64_t Limit = ELFT::Is64Bits ? InvalidVA : uint64_t{1} << 32;
    if (Symbol.st_size > Limit - Start)
      return profileError("code-mode symbol range overflows address space");
    if (Symbol.st_size)
      Profile.CodeRegions.push_back({Start, Start + Symbol.st_size, Mode});
    return llvm::Error::success();
  };
  std::optional<llvm::ArrayRef<uint8_t>> ABIFlags;
  const auto ReadABIFlags = [&](uint64_t Offset, uint64_t Size) -> llvm::Error {
    if (Size != 24 || !rangeInBounds(Offset, Size, ELF.getBufSize()))
      return profileError("malformed .MIPS.abiflags record");
    const llvm::ArrayRef<uint8_t> Bytes(ELF.base() + Offset, Size);
    if (ABIFlags && *ABIFlags != Bytes)
      return profileError("conflicting ABI flag records");
    ABIFlags = Bytes;
    return llvm::Error::success();
  };

  auto Sections = ELF.sections();
  if (!Sections)
    return Sections.takeError();
  for (const auto &Section : *Sections) {
    if (Section.sh_type == SHT_MIPS_ABIFLAGS)
      if (auto Error = ReadABIFlags(Section.sh_offset, Section.sh_size))
        return std::move(Error);
    if (Section.sh_type != SHT_SYMTAB && Section.sh_type != SHT_DYNSYM)
      continue;
    auto Symbols = ELF.symbols(&Section);
    if (!Symbols)
      return Symbols.takeError();
    for (const auto &Symbol : *Symbols)
      if (auto Error = ReadSymbol(Symbol))
        return std::move(Error);
  }
  auto Programs = ELF.program_headers();
  if (!Programs)
    return Programs.takeError();
  for (const auto &Program : *Programs)
    if (Program.p_type == PT_MIPS_ABIFLAGS)
      if (auto Error = ReadABIFlags(Program.p_offset, Program.p_filesz))
        return std::move(Error);

  // Sectionless firmware still carries symbol-level encoding flags. Read the
  // runtime symbol table through its declared size before admitting classic
  // decoding; a stripped microMIPS function cannot lose its mode evidence.
  if (llvm::any_of(*Programs,
                   [](const auto &P) { return P.p_type == PT_DYNAMIC; })) {
    auto Dynamic = ELF.dynamicEntries();
    if (!Dynamic)
      return Dynamic.takeError();
    std::optional<uint64_t> SymbolAddress, SymbolCount, SymbolSize;
    for (const auto &Entry : *Dynamic) {
      if (Entry.d_tag == DT_NULL)
        break;
      if (Entry.d_tag == DT_SYMTAB)
        SymbolAddress = Entry.d_un.d_ptr;
      else if (Entry.d_tag == DT_SYMENT)
        SymbolSize = Entry.d_un.d_val;
      else if (Entry.d_tag == DT_MIPS_SYMTABNO)
        SymbolCount = Entry.d_un.d_val;
    }
    if (SymbolAddress) {
      if (!SymbolSize || *SymbolSize != sizeof(typename ELFT::Sym))
        return profileError("invalid dynamic symbol entry size");
      if (!SymbolCount) {
        auto Count = ELF.getDynSymtabSize();
        if (!Count)
          return Count.takeError();
        SymbolCount = *Count;
      }
      if (*SymbolCount == 0)
        return profileError("dynamic symbol table has no bounded size");
      if (*SymbolCount > ELF.getBufSize() / *SymbolSize)
        return profileError("dynamic symbol table exceeds file bounds");
      const uint64_t TableBytes = *SymbolCount * *SymbolSize;
      std::optional<uint64_t> Offset;
      for (const auto &P : *Programs) {
        if (P.p_type != PT_LOAD || *SymbolAddress < P.p_vaddr ||
            !rangeInBounds(*SymbolAddress - P.p_vaddr, TableBytes,
                           P.p_filesz) ||
            !rangeInBounds(P.p_offset, P.p_filesz, ELF.getBufSize()))
          continue;
        const uint64_t Candidate = P.p_offset + (*SymbolAddress - P.p_vaddr);
        if (Offset && *Offset != Candidate)
          return profileError("ambiguous dynamic symbol mapping");
        Offset = Candidate;
      }
      if (!Offset)
        return profileError("unmapped dynamic symbol table");
      for (uint64_t I = 0; I < *SymbolCount; ++I) {
        typename ELFT::Sym Symbol;
        std::memcpy(&Symbol, ELF.base() + *Offset + I * *SymbolSize,
                    sizeof(Symbol));
        if (auto Error = ReadSymbol(Symbol))
          return std::move(Error);
      }
    }
  }

  if (ABIFlags) {
    const auto Bytes = *ABIFlags;
    const auto Read32 = [&](size_t Offset) {
      return llvm::support::endian::read32<ELFT::Endianness>(Bytes.data() +
                                                             Offset);
    };
    if (llvm::support::endian::read16<ELFT::Endianness>(Bytes.data()) != 0)
      return profileError("unsupported .MIPS.abiflags version");
    const uint32_t ASE = Read32(12);
    Profile.HasMIPS16E2 = ASE & kMIPS16E2ASE;
    if (Profile.HasMIPS16E2 && !Profile.HasMIPS16)
      return profileError("MIPS16e2 requires MIPS16 encoding capability");
    Profile.HasMT = ASE & llvm::Mips::AFL_ASE_MT;
    if (Profile.HasMT && (!Profile.HasMIPS16E2 || Wide))
      return profileError("MT currently requires MIPS16e2/o32");
    Profile.HasDSPR2 = ASE & llvm::Mips::AFL_ASE_DSPR2;
    Profile.HasDSP = Profile.HasDSPR2 || (ASE & llvm::Mips::AFL_ASE_DSP);
    Profile.HasMSA = ASE & llvm::Mips::AFL_ASE_MSA;
    const uint32_t AllowedXPA = R6 ? uint32_t(llvm::Mips::AFL_ASE_XPA) : 0u;
    if (R6 && (ASE & ~(AllowedXPA | llvm::Mips::AFL_ASE_MICROMIPS)))
      return profileError("unsupported Release 6 extension profile");
    if (Profile.HasMSA &&
        (Profile.HasMIPS16 || Profile.HasDSP || Bytes[3] != 5 ||
         Bytes[5] != 3 || Bytes[7] != (Wide ? 1 : 6)))
      return profileError("MSA requires r5 with 128-bit CPR1 and o32 FP64 or "
                          "n32/n64 hard double");
    if (Bytes[7] == (Wide ? 1 : 6) && Bytes[5] != (Profile.HasMSA ? 3 : 2))
      return profileError("FP64 hard-float ABI requires matching CPR1 width");
    if (Bytes[2] != (Wide ? 64 : 32) ||
        Bytes[3] != (R6               ? 6
                     : Profile.HasMSA ? 5
                                      : 2) ||
        Bytes[4] != (Wide ? 2 : 1))
      return profileError("ABI flags conflict with the admitted GPR profile");
    if (Bytes[5] > (Profile.HasMSA ? 3 : 2) || Bytes[6] != 0 || Bytes[7] > 7 ||
        Read32(8) != 0 ||
        (ASE & ~(llvm::Mips::AFL_ASE_DSP | llvm::Mips::AFL_ASE_DSPR2 |
                 llvm::Mips::AFL_ASE_MSA | llvm::Mips::AFL_ASE_MT |
                 kMIPS16E2ASE | AllowedXPA)) !=
            (Profile.HasMicroMIPS ? llvm::Mips::AFL_ASE_MICROMIPS
             : Profile.HasMIPS16  ? llvm::Mips::AFL_ASE_MIPS16
                                  : 0u) ||
        (Read32(16) & ~1u) != 0 || Read32(20) != 0)
      return profileError("unsupported ABI flags or instruction extensions");

    if (Profile.HasDSP && Profile.HasMIPS16)
      return profileError("DSP is unavailable in MIPS16 mode");
    Profile.ISARevision = Bytes[3];
    Profile.HasABIFlags = true;
    Profile.FPABI = Bytes[7];
  }
  // A disjoint, sorted index keeps occurrence lookups logarithmic. Exact
  // entries also participate: a zero-sized classic symbol cannot hide inside
  // a sized microMIPS symbol (or conversely).
  llvm::sort(Profile.CodeRegions,
             [](const auto &A, const auto &B) { return A.Start < B.Start; });
  size_t Count = 0;
  for (const auto &Region : Profile.CodeRegions) {
    if (Count && Region.Start < Profile.CodeRegions[Count - 1].End) {
      auto &Previous = Profile.CodeRegions[Count - 1];
      if (Previous.Mode != Region.Mode)
        return profileError("overlapping code ranges have conflicting modes");
      Previous.End = std::max(Previous.End, Region.End);
    } else {
      Profile.CodeRegions[Count++] = Region;
    }
  }
  Profile.CodeRegions.resize(Count);
  for (const auto &Region : Profile.CodeRegions)
    for (auto It = Profile.CodeModeEntries.lower_bound(Region.Start);
         It != Profile.CodeModeEntries.end() && It->first < Region.End; ++It)
      if (It->second != Region.Mode)
        return profileError("code entry conflicts with containing mode range");
  return Profile;
}

} // namespace

llvm::Expected<MIPSImageMetadata>
readMIPSProfile(const llvm::object::ObjectFile &Object) {
  if (const auto *ELF =
          llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(&Object))
    return readProfile(*ELF);
  if (const auto *ELF =
          llvm::dyn_cast<llvm::object::ELF32BEObjectFile>(&Object))
    return readProfile(*ELF);
  if (const auto *ELF =
          llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(&Object))
    return readProfile(*ELF);
  if (const auto *ELF =
          llvm::dyn_cast<llvm::object::ELF64BEObjectFile>(&Object))
    return readProfile(*ELF);
  return profileError("unsupported ELF class");
}

} // namespace neverd::elf_loader
