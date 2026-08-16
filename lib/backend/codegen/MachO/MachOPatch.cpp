//===- MachOPatch.cpp - Mach-O binary patching --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Mach-O section-based binary patching implementation.  Inserts a new
/// segment containing recompiled code before __LINKEDIT and installs
/// trampolines at original function entry points.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOPatch.h"

#include "MachOStrictLayout.h"
#include "MachOSymbolResolution.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/MachOARM32Mode.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/object/MachOLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <string>

#define DEBUG_TYPE "neverd-macho-patch"

namespace neverd {

using namespace llvm::MachO;

namespace {

struct MachOSectionIdentity {
  std::string Name;
  std::string SegmentName;
  uint64_t VA = 0;
  uint64_t Size = 0;
  uint64_t FileOff = 0;
  uint64_t FileSize = 0;
};

struct MachORewriteTarget {
  std::string Triple;
  InstructionMode Mode = InstructionMode::Default;
  std::optional<macho_arm32::ModeInfo> ARM32Modes;
};

} // namespace

namespace macho_patch_detail {

namespace {

namespace symbol_specifier {

constexpr uint32_t None = 0;

namespace x86 {
constexpr uint32_t GOT = 7;
constexpr uint32_t GOTPCRel = 11;
constexpr uint32_t GOTPCRelNoRelax = 12;
constexpr uint32_t TLVP = 26;
} // namespace x86

namespace arm {
constexpr uint32_t High16 = 4;
constexpr uint32_t Low16 = 5;
} // namespace arm

namespace aarch64 {
constexpr uint32_t Auth = 0x00a;
constexpr uint32_t AuthAddress = 0x00b;
constexpr uint32_t GOTAuth = 0x00c;
constexpr uint32_t TLSDescriptorAuth = 0x00d;
constexpr uint32_t MachOGOT = 0x403;
constexpr uint32_t MachOGOTPage = 0x404;
constexpr uint32_t MachOGOTPageOff = 0x405;
constexpr uint32_t MachOPage = 0x406;
constexpr uint32_t MachOPageOff = 0x407;
constexpr uint32_t MachOTLVP = 0x408;
constexpr uint32_t MachOTLVPPage = 0x409;
constexpr uint32_t MachOTLVPPageOff = 0x40a;
} // namespace aarch64

} // namespace symbol_specifier

enum class MachOSymbolNameMatch : uint8_t { Exact, UnderscoreAlias };

std::optional<MachOSymbolNameMatch>
classifyMachOSymbolName(llvm::StringRef Requested, llvm::StringRef Candidate) {
  if (Candidate == Requested)
    return MachOSymbolNameMatch::Exact;
  if ((Requested.starts_with("_") && Candidate == Requested.drop_front()) ||
      (Candidate.starts_with("_") && Candidate.drop_front() == Requested))
    return MachOSymbolNameMatch::UnderscoreAlias;
  return std::nullopt;
}

bool isX86TLSSpecifier(uint32_t Specifier) {
  switch (Specifier) {
  case 5:  // DTPOFF
  case 6:  // DTPREL
  case 9:  // GOTNTPOFF
  case 14: // GOTTPOFF
  case 15: // INDNTPOFF
  case 16: // NTPOFF
  case 21: // TLSCALL
  case 22: // TLSDESC
  case 23: // TLSGD
  case 24: // TLSLD
  case 25: // TLSLDM
  case symbol_specifier::x86::TLVP:
  case 27: // TLVPPAGE
  case 28: // TLVPPAGEOFF
  case 29: // TPOFF
    return true;
  default:
    return false;
  }
}

bool isARMTLSSpecifier(uint32_t Specifier) {
  switch (Specifier) {
  case 16: // GOTTPOFF
  case 17: // GOTTPOFF_FDPIC
  case 24: // TLSCALL
  case 25: // TLSDESC
  case 26: // TLSDESCSEQ
  case 27: // TLSGD
  case 28: // TLSGD_FDPIC
  case 29: // TLSLDM
  case 30: // TLSLDM_FDPIC
  case 31: // TLSLDO
  case 32: // TPOFF
    return true;
  default:
    return false;
  }
}

bool isAArch64TLSSpecifier(uint32_t Specifier) {
  if (Specifier >= 0x400)
    return Specifier == symbol_specifier::aarch64::MachOTLVP ||
           Specifier == symbol_specifier::aarch64::MachOTLVPPage ||
           Specifier == symbol_specifier::aarch64::MachOTLVPPageOff;
  constexpr uint32_t SymbolLocationMask = 0x00f;
  switch (Specifier & SymbolLocationMask) {
  case 5: // DTPREL
  case 6: // GOTTPREL
  case 7: // TPREL
  case 8: // TLSDESC
    return true;
  default:
    return false;
  }
}

bool isAArch64AuthenticatedSpecifier(uint32_t Specifier) {
  if (Specifier >= 0x400)
    return false;
  constexpr uint32_t SymbolLocationMask = 0x00f;
  switch (Specifier & SymbolLocationMask) {
  case symbol_specifier::aarch64::Auth:
  case symbol_specifier::aarch64::AuthAddress:
  case symbol_specifier::aarch64::GOTAuth:
  case symbol_specifier::aarch64::TLSDescriptorAuth:
    return true;
  default:
    return false;
  }
}

std::optional<MachOSymbolTargetKind>
classifyMappedMachOAddress(const BinaryImage &Image, uint64_t Address) {
  const Segment *Segment = Image.getSegmentFor(Address);
  if (!Segment || !Segment->isReadable())
    return std::nullopt;
  if (Image.isImportStubAt(Address))
    return MachOSymbolTargetKind::Callable;

  const Section *Section = Image.getSectionFor(Address);
  if (!Section) {
    if (!Segment->isExecutable())
      return MachOSymbolTargetKind::Data;
    return std::nullopt;
  }

  const uint32_t Type = Section->Type & SECTION_TYPE;
  const uint32_t Attributes = Section->Type & SECTION_ATTRIBUTES;
  if (Type == S_SYMBOL_STUBS ||
      (Attributes & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) != 0)
    return MachOSymbolTargetKind::Callable;
  return MachOSymbolTargetKind::Data;
}

bool isCallableFixup(llvm::StringRef Name) {
  return Name.contains_insensitive("branch") ||
         Name.contains_insensitive("call") ||
         Name.ends_with_insensitive("_bl") ||
         Name.contains_insensitive("_blx") ||
         Name.contains_insensitive("_thumb_bl");
}

bool isTLSSpecifierName(llvm::StringRef Name) {
  return Name.contains_insensitive("TLS") ||
         Name.contains_insensitive("TLVP") ||
         Name.contains_insensitive("TPOFF") ||
         Name.contains_insensitive("DTPREL") ||
         Name.contains_insensitive("DTPOFF") ||
         Name.contains_insensitive("NTPOFF");
}

bool isAuthenticatedSpecifierName(llvm::StringRef Name) {
  return Name.contains_insensitive("AUTH");
}

bool isGOTSpecifierName(llvm::StringRef Name) {
  return Name == "GOT" || Name == "GOTPAGE" || Name == "GOTPAGEOFF" ||
         Name == "GOTPCREL" || Name == "GOTPCREL_NORELAX" || Name == "GOT_PREL";
}

llvm::Expected<MachOSymbolUse> unsupportedMachOSymbolUse(
    Arch TargetArch, llvm::StringRef Kind,
    const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request) {
  return llvm::createStringError(
      llvm::errc::not_supported,
      (llvm::Twine("unsupported Mach-O ") + getArchName(TargetArch) + " " +
       Kind + " symbol reference for '" + Request.Symbol + "' (specifier=" +
       (Request.SpecifierName.empty()
            ? llvm::Twine("0x") + llvm::utohexstr(Request.Specifier)
            : llvm::Twine(Request.SpecifierName)) +
       ", fixup=" + Request.FixupKindName + ", section=" + Request.SectionName +
       ")")
          .str());
}

llvm::Expected<MachOSymbolUse> classifyRawMachOSymbolSpecifier(
    Arch TargetArch,
    const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request) {
  const uint32_t Specifier = Request.Specifier;
  auto Unsupported = [&](llvm::StringRef Kind) {
    return unsupportedMachOSymbolUse(TargetArch, Kind, Request);
  };

  switch (TargetArch) {
  case Arch::X64:
  case Arch::X86:
    if (Specifier == symbol_specifier::None)
      return MachOSymbolUse::Direct;
    if (Specifier == symbol_specifier::x86::GOT ||
        Specifier == symbol_specifier::x86::GOTPCRel ||
        Specifier == symbol_specifier::x86::GOTPCRelNoRelax)
      return MachOSymbolUse::ImportSlot;
    if (isX86TLSSpecifier(Specifier))
      return Unsupported("TLS");
    return Unsupported("unknown");
  case Arch::AArch64:
    if (Specifier == symbol_specifier::None ||
        Specifier == symbol_specifier::aarch64::MachOPage ||
        Specifier == symbol_specifier::aarch64::MachOPageOff)
      return MachOSymbolUse::Direct;
    if (Specifier == symbol_specifier::aarch64::MachOGOT ||
        Specifier == symbol_specifier::aarch64::MachOGOTPage ||
        Specifier == symbol_specifier::aarch64::MachOGOTPageOff)
      return MachOSymbolUse::ImportSlot;
    if (isAArch64TLSSpecifier(Specifier))
      return Unsupported("TLS");
    if (isAArch64AuthenticatedSpecifier(Specifier))
      return Unsupported("authenticated-pointer");
    return Unsupported("unknown");
  case Arch::ARM:
    if (Specifier == symbol_specifier::None ||
        Specifier == symbol_specifier::arm::High16 ||
        Specifier == symbol_specifier::arm::Low16)
      return MachOSymbolUse::Direct;
    if (isARMTLSSpecifier(Specifier))
      return Unsupported("TLS");
    return Unsupported("unknown");
  case Arch::EVM:
  case Arch::SBF:
  case Arch::Unknown:
    return Unsupported("target-incompatible");
  }
  llvm_unreachable("unhandled architecture");
}

} // namespace

llvm::Expected<MachOSymbolUse> classifyMachOSymbolUse(
    Arch TargetArch,
    const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request) {
  if (TargetArch == Arch::EVM || TargetArch == Arch::SBF ||
      TargetArch == Arch::Unknown)
    return unsupportedMachOSymbolUse(TargetArch, "target-incompatible",
                                     Request);
  if (Request.Symbol.empty())
    return unsupportedMachOSymbolUse(TargetArch, "unnamed", Request);
  if (Request.IsSubtrahend)
    return unsupportedMachOSymbolUse(TargetArch, "subtractive", Request);

  if (isAuthenticatedSpecifierName(Request.SpecifierName) ||
      (TargetArch == Arch::AArch64 &&
       isAArch64AuthenticatedSpecifier(Request.Specifier)))
    return unsupportedMachOSymbolUse(TargetArch, "authenticated-pointer",
                                     Request);
  if (isTLSSpecifierName(Request.SpecifierName))
    return unsupportedMachOSymbolUse(TargetArch, "TLS", Request);

  if (Request.SectionName == section_names::macho::CompactUnwind) {
    const uint64_t PointerWidth =
        TargetArch == Arch::X86 || TargetArch == Arch::ARM ? 4 : 8;
    const uint64_t RecordSize = 3 * PointerWidth + 8;
    const uint64_t PersonalityFieldOffset = PointerWidth + 8;
    if (Request.Specifier == symbol_specifier::None && !Request.IsPCRel &&
        Request.BitWidth == PointerWidth * 8 &&
        Request.SectionOffset % RecordSize == PersonalityFieldOffset)
      return MachOSymbolUse::ImportSlot;
    return unsupportedMachOSymbolUse(
        TargetArch, "invalid compact-unwind external", Request);
  }

  if (isGOTSpecifierName(Request.SpecifierName))
    return MachOSymbolUse::ImportSlot;
  if (Request.SpecifierName == "PLT")
    return MachOSymbolUse::Callable;

  auto ClassifySpecifier = [&]() -> llvm::Expected<MachOSymbolUse> {
    if (Request.SpecifierName == "PAGE" || Request.SpecifierName == "PAGEOFF" ||
        Request.SpecifierName == "PCREL" || Request.SpecifierName == "ABS8")
      return MachOSymbolUse::Direct;
    return classifyRawMachOSymbolSpecifier(TargetArch, Request);
  };
  auto Use = ClassifySpecifier();
  if (!Use)
    return Use.takeError();
  if (*Use != MachOSymbolUse::Direct)
    return *Use;

  if (isCallableFixup(Request.FixupKindName))
    return MachOSymbolUse::Callable;
  return MachOSymbolUse::Direct;
}

llvm::Expected<std::optional<MachOSymbolTarget>>
resolveUniqueMachOSymbol(const BinaryImage &Image, llvm::StringRef Requested,
                         MachOSymbolUse Use) {
  using CandidateKey = std::pair<uint64_t, MachOSymbolTargetKind>;
  std::set<CandidateKey> ExactCandidates;
  std::set<CandidateKey> AliasCandidates;
  bool InvalidExactCandidate = false;
  bool InvalidAliasCandidate = false;

  auto Record = [&](llvm::StringRef Name, uint64_t Address,
                    MachOSymbolTargetKind Kind) {
    std::optional<MachOSymbolNameMatch> Match =
        classifyMachOSymbolName(Requested, Name);
    if (!Match)
      return;
    auto &Candidates = *Match == MachOSymbolNameMatch::Exact ? ExactCandidates
                                                             : AliasCandidates;
    Candidates.emplace(Address, Kind);
  };
  auto RecordInvalid = [&](llvm::StringRef Name) {
    std::optional<MachOSymbolNameMatch> Match =
        classifyMachOSymbolName(Requested, Name);
    if (!Match)
      return;
    if (*Match == MachOSymbolNameMatch::Exact)
      InvalidExactCandidate = true;
    else
      InvalidAliasCandidate = true;
  };
  if (Use == MachOSymbolUse::ImportSlot) {
    const uint32_t PointerSize = Image.getPointerSize();
    for (const auto &[SlotVA, ImportedName] : Image.ImportPtrSlots) {
      if (!classifyMachOSymbolName(Requested, ImportedName))
        continue;
      const Section *Section = Image.getSectionFor(SlotVA);
      if (!Section ||
          (Section->Type & SECTION_TYPE) != S_NON_LAZY_SYMBOL_POINTERS) {
        RecordInvalid(ImportedName);
        continue;
      }
      const Segment *Segment = Image.getSegmentFor(SlotVA);
      if (!Segment || !Segment->isReadable()) {
        RecordInvalid(ImportedName);
        continue;
      }
      const uint64_t SectionOffset = SlotVA - Section->VA;
      const uint64_t SegmentOffset = SlotVA - Segment->VA;
      if (PointerSize == 0 || SectionOffset > Section->Size ||
          PointerSize > Section->Size - SectionOffset ||
          SegmentOffset > Segment->Size ||
          PointerSize > Segment->Size - SegmentOffset) {
        RecordInvalid(ImportedName);
        continue;
      }
      Record(ImportedName, SlotVA, MachOSymbolTargetKind::ImportSlot);
    }
  } else {
    for (const auto &[StubVA, ImportIndex] : Image.ImportStubIndices) {
      if (ImportIndex >= Image.Imports.size())
        continue;
      const Import &Item = Image.Imports[ImportIndex];
      std::optional<MachOSymbolTargetKind> Kind =
          classifyMappedMachOAddress(Image, StubVA);
      if (!Kind || *Kind != MachOSymbolTargetKind::Callable) {
        RecordInvalid(Item.Name);
        continue;
      }
      Record(Item.Name, StubVA, *Kind);
    }
    for (const Export &Item : Image.Exports) {
      std::optional<MachOSymbolTargetKind> Kind =
          classifyMappedMachOAddress(Image, Item.Addr);
      if (!Kind || (Use == MachOSymbolUse::Callable &&
                    *Kind != MachOSymbolTargetKind::Callable)) {
        RecordInvalid(Item.Name);
        continue;
      }
      Record(Item.Name, Item.Addr, *Kind);
    }
    for (const Symbol &Item : Image.Symbols) {
      std::optional<MachOSymbolTargetKind> Kind =
          classifyMappedMachOAddress(Image, Item.Addr);
      if (!Kind ||
          (Item.IsFunc != (*Kind == MachOSymbolTargetKind::Callable)) ||
          (Use == MachOSymbolUse::Callable &&
           *Kind != MachOSymbolTargetKind::Callable)) {
        RecordInvalid(Item.Name);
        continue;
      }
      Record(Item.Name, Item.Addr, *Kind);
    }
  }

  const bool HasExactEvidence =
      InvalidExactCandidate || !ExactCandidates.empty();
  const std::set<CandidateKey> &Candidates =
      HasExactEvidence ? ExactCandidates : AliasCandidates;
  const bool HasInvalidCandidate =
      HasExactEvidence ? InvalidExactCandidate : InvalidAliasCandidate;
  if (HasInvalidCandidate)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        (llvm::Twine("Mach-O symbol '") + Requested +
         "' has a candidate with invalid address or kind provenance")
            .str());
  if (Candidates.size() > 1)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   (llvm::Twine("ambiguous Mach-O symbol '") +
                                    Requested +
                                    "' resolves to multiple addresses or kinds")
                                       .str());
  if (Candidates.empty())
    return std::optional<MachOSymbolTarget>();
  const auto &[Address, Kind] = *Candidates.begin();
  return std::optional<MachOSymbolTarget>(MachOSymbolTarget{Address, Kind});
}

} // namespace macho_patch_detail

namespace {

llvm::Expected<MachORewriteTarget>
resolveMachORewriteTarget(llvm::ArrayRef<uint8_t> Binary, Arch TargetArch,
                          InstructionMode RequestedMode,
                          const llvm::Module &Module) {
  MachORewriteTarget Result;
  Result.Mode = RequestedMode;
  if (TargetArch != Arch::ARM)
    return Result;

  auto ModeInfo = macho_arm32::parseModeInfo(Binary);
  if (!ModeInfo)
    return ModeInfo.takeError();
  if (ModeInfo->CPUSubtype != CPU_SUBTYPE_ARM_V7K)
    return llvm::createStringError(
        llvm::errc::not_supported,
        "32-bit ARM Mach-O rewrite currently requires the ARMv7k ABI");

  std::vector<va_t> SourceEntries;
  for (const llvm::Function &Function : Module) {
    if (Function.isDeclaration())
      continue;
    auto Address = rewrite_source::getOriginalVA(Function);
    if (!Address)
      return Address.takeError();
    if (*Address)
      SourceEntries.push_back(**Address);
  }

  auto Mode = macho_arm32::requireUniformFunctionMode(*ModeInfo, SourceEntries);
  if (!Mode)
    return Mode.takeError();
  if (*Mode != InstructionMode::Thumb)
    return llvm::createStringError(
        llvm::errc::not_supported,
        "ARMv7k rewrite currently requires positive Thumb function evidence");
  if (RequestedMode != InstructionMode::Default && RequestedMode != *Mode)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "cached ARM code mode disagrees with authenticated nlist metadata");

  Result.Mode = *Mode;
  Result.Triple = "thumbv7k-apple-watchos";
  Result.ARM32Modes = std::move(*ModeInfo);
  return Result;
}

bool machOHeaderMatchesTarget(llvm::ArrayRef<uint8_t> Binary, Arch TargetArch) {
  const MachOHeaderInfo Header = parseMachOHeader(Binary.data(), Binary.size());
  if (Header.HeaderSize == 0)
    return false;
  const uint32_t CPUType =
      Header.Is64
          ? reinterpret_cast<const mach_header_64 *>(Binary.data())->cputype
          : reinterpret_cast<const mach_header *>(Binary.data())->cputype;
  switch (TargetArch) {
  case Arch::X86:
    return !Header.Is64 && CPUType == CPU_TYPE_X86;
  case Arch::X64:
    return Header.Is64 && CPUType == CPU_TYPE_X86_64;
  case Arch::ARM:
    return !Header.Is64 && CPUType == CPU_TYPE_ARM;
  case Arch::AArch64:
    return Header.Is64 && CPUType == CPU_TYPE_ARM64;
  default:
    return false;
  }
}

bool cachedImageBytesMatchInput(llvm::ArrayRef<uint8_t> Binary,
                                const BinaryImage &Image, Arch TargetArch,
                                bool Is64, std::string &Detail) {
  if (Image.Format != BinaryFormat::MachO) {
    Detail = "cached format is not Mach-O";
    return false;
  }
  if (Image.Arch != TargetArch ||
      !machOHeaderMatchesTarget(Binary, TargetArch)) {
    Detail = "cached or file architecture differs from the target";
    return false;
  }
  if ((Is64 && Image.Bits != Bitness::Bits64) ||
      (!Is64 && Image.Bits != Bitness::Bits32)) {
    Detail = "cached pointer width differs from the Mach header";
    return false;
  }
  if (Image.Raw.size() != Binary.size() ||
      !std::equal(Binary.begin(), Binary.end(), Image.Raw.begin())) {
    Detail = "cached raw bytes differ from the input file";
    return false;
  }
  return true;
}

std::optional<std::vector<MachOSectionIdentity>>
collectMachOSectionIdentities(llvm::ArrayRef<uint8_t> Binary) {
  std::vector<MachOSectionIdentity> Result;
  bool Valid = true;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t CommandSize,
          bool Is64) {
        if (ID != getMachOSegmentCmdID(Is64))
          return;
        const uint32_t BaseSize = getMachOSegmentCmdSize(Is64);
        const uint32_t SectionSize = getMachOSectionSize(Is64);
        if (CommandSize < BaseSize ||
            (CommandSize - BaseSize) % SectionSize != 0) {
          Valid = false;
          return;
        }
        const MachOSegFields Segment = readMachOSegment(Command, Is64);
        if (Segment.NSects != (CommandSize - BaseSize) / SectionSize) {
          Valid = false;
          return;
        }
        const std::string SegmentName = readMachOName(Segment.SegName);
        for (uint32_t I = 0; I < Segment.NSects; ++I) {
          const uint8_t *SectionPtr =
              Command + BaseSize + uint64_t(I) * SectionSize;
          MachOSectionIdentity Identity;
          uint32_t Flags = 0;
          if (Is64) {
            const auto *Section =
                reinterpret_cast<const section_64 *>(SectionPtr);
            Identity.Name = readMachOName(Section->sectname);
            Identity.SegmentName = readMachOName(Section->segname);
            Identity.VA = Section->addr;
            Identity.Size = Section->size;
            Identity.FileOff = Section->offset;
            Flags = Section->flags;
          } else {
            const auto *Section = reinterpret_cast<const section *>(SectionPtr);
            Identity.Name = readMachOName(Section->sectname);
            Identity.SegmentName = readMachOName(Section->segname);
            Identity.VA = Section->addr;
            Identity.Size = Section->size;
            Identity.FileOff = Section->offset;
            Flags = Section->flags;
          }
          if (Identity.SegmentName != SegmentName)
            continue;
          const uint32_t Type = Flags & SECTION_TYPE;
          const bool IsZeroFill = Type == S_ZEROFILL || Type == S_GB_ZEROFILL ||
                                  Type == S_THREAD_LOCAL_ZEROFILL;
          Identity.FileSize = IsZeroFill ? 0 : Identity.Size;
          Result.push_back(std::move(Identity));
        }
      });
  if (!Valid)
    return std::nullopt;
  return Result;
}

bool containsPointer(const MachOSectionIdentity &Section, uint64_t Address,
                     uint64_t Width) {
  if (Address < Section.VA)
    return false;
  const uint64_t Offset = Address - Section.VA;
  return Offset <= Section.Size && Width <= Section.Size - Offset &&
         Offset <= Section.FileSize && Width <= Section.FileSize - Offset;
}

bool containsPointer(const Section &Section, uint64_t Address, uint64_t Width) {
  if (Address < Section.VA)
    return false;
  const uint64_t Offset = Address - Section.VA;
  return Offset <= Section.Size && Width <= Section.Size - Offset &&
         Offset <= Section.FileSz && Width <= Section.FileSz - Offset;
}

bool cachedAddressModelMatchesInput(llvm::ArrayRef<uint8_t> Binary,
                                    const BinaryImage &Image,
                                    std::string &Detail) {
  MachOHeaderInfo Header;
  if (!macho_patch_detail::validateLoadCommandRegion(Binary, Header)) {
    Detail = "input load-command region is malformed";
    return false;
  }
  const uint64_t LoadCommandsEnd = Header.HeaderSize + Header.SizeOfCmds;

  std::optional<MachOSegFields> InputHeaderSegment;
  unsigned InputHeaderMappings = 0;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t, bool Is64) {
        if (ID != getMachOSegmentCmdID(Is64))
          return;
        const MachOSegFields Segment = readMachOSegment(Command, Is64);
        if (Segment.FileOff != 0 || Segment.FileSize < LoadCommandsEnd)
          return;
        ++InputHeaderMappings;
        InputHeaderSegment = Segment;
      });
  if (InputHeaderMappings != 1 || !InputHeaderSegment) {
    Detail = "input does not have one exact Mach-header mapping";
    return false;
  }

  const Segment *HeaderSegment = nullptr;
  unsigned CachedHeaderMappings = 0;
  for (const Segment &Segment : Image.Segments) {
    if (Segment.FileOff != 0 || Segment.FileSz < LoadCommandsEnd)
      continue;
    ++CachedHeaderMappings;
    HeaderSegment = &Segment;
  }
  if (CachedHeaderMappings != 1 || !HeaderSegment ||
      HeaderSegment->Name != readMachOName(InputHeaderSegment->SegName) ||
      HeaderSegment->VA != InputHeaderSegment->VMAddr ||
      HeaderSegment->Size != InputHeaderSegment->VMSize ||
      HeaderSegment->FileOff != InputHeaderSegment->FileOff ||
      HeaderSegment->FileSz != InputHeaderSegment->FileSize) {
    Detail = "cached Mach-header segment identity differs from the input";
    return false;
  }

  unsigned ExactHeaderSegments = 0;
  bool ValidSegments = true;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t CommandSize,
          bool Is64) {
        if (ID != getMachOSegmentCmdID(Is64))
          return;
        if (CommandSize < getMachOSegmentCmdSize(Is64)) {
          ValidSegments = false;
          return;
        }
        const MachOSegFields Segment = readMachOSegment(Command, Is64);
        ExactHeaderSegments +=
            readMachOName(Segment.SegName) == HeaderSegment->Name &&
            Segment.VMAddr == HeaderSegment->VA &&
            Segment.VMSize == HeaderSegment->Size &&
            Segment.FileOff == HeaderSegment->FileOff &&
            Segment.FileSize == HeaderSegment->FileSz;
      });
  if (!ValidSegments || ExactHeaderSegments != 1) {
    Detail = "cached Mach-header segment identity differs from the input";
    return false;
  }

  auto ParsedImportPtrSlots = macho_loader::parseImportPtrSlots(Binary);
  if (!ParsedImportPtrSlots) {
    Detail = "cannot reconstruct input pointer-slot provenance: " +
             llvm::toString(ParsedImportPtrSlots.takeError());
    return false;
  }
  if (*ParsedImportPtrSlots != Image.ImportPtrSlots) {
    Detail = "cached pointer-slot symbol map differs from the input";
    return false;
  }

  const uint64_t PointerWidth = Image.getPointerSize();
  const std::optional<std::vector<MachOSectionIdentity>> Sections =
      collectMachOSectionIdentities(Binary);
  if (!Sections) {
    Detail = "input section table is malformed";
    return false;
  }
  for (const auto &[SlotVA, Symbol] : Image.ImportPtrSlots) {
    const Section *CachedSection = nullptr;
    unsigned CachedCount = 0;
    for (const Section &Section : Image.Sections) {
      if (!containsPointer(Section, SlotVA, PointerWidth))
        continue;
      CachedSection = &Section;
      ++CachedCount;
    }

    const MachOSectionIdentity *InputSection = nullptr;
    unsigned InputCount = 0;
    for (const MachOSectionIdentity &Section : *Sections) {
      if (!containsPointer(Section, SlotVA, PointerWidth))
        continue;
      InputSection = &Section;
      ++InputCount;
    }
    if (CachedCount != 1 || InputCount != 1 || !CachedSection ||
        !InputSection || CachedSection->Name != InputSection->Name ||
        CachedSection->SegmentName != InputSection->SegmentName ||
        CachedSection->VA != InputSection->VA ||
        CachedSection->Size != InputSection->Size ||
        CachedSection->FileOff != InputSection->FileOff ||
        CachedSection->FileSz != InputSection->FileSize) {
      Detail = "cached pointer-section identity differs for " + Symbol;
      return false;
    }
  }
  return true;
}

llvm::Error sourceIdentityError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "macho patch source identity: " + Message);
}

bool isAuthenticatedSourceFunctionEntry(const BinaryImage &Image,
                                        uint64_t Address) {
  if (Address == InvalidVA ||
      normalizeCodeAddress(Address, Image.Arch, Image.Mode) != Address)
    return false;

  for (const Symbol &Symbol : Image.Symbols)
    if (Symbol.IsFunc &&
        normalizeCodeAddress(Symbol.Addr, Image.Arch, Image.Mode) == Address)
      return true;

  for (const auto &[Begin, End] : Image.KnownCodeRanges)
    if (Begin < End &&
        normalizeCodeAddress(Begin, Image.Arch, Image.Mode) == Address)
      return true;

  for (const Export &Export : Image.Exports) {
    const uint64_t ExportAddress =
        normalizeCodeAddress(Export.Addr, Image.Arch, Image.Mode);
    const Segment *Segment = Image.getSegmentFor(ExportAddress);
    if (ExportAddress == Address && Segment && Segment->isExecutable())
      return true;
  }
  return false;
}

llvm::Error validateSourceFunctionIdentities(const CompiledImage &Compiled,
                                             const llvm::Module &Module,
                                             const BinaryImage *Image) {
  if (!Image)
    return sourceIdentityError(
        "an exact loader-authenticated image context is required");
  if (Compiled.SourceFunctionOriginalVAs.empty())
    return sourceIdentityError(
        "the compiled image has no exact original function entries");
  if (!llvm::mc_rewrite::validateRewriteSourceFunctionOwners(
          Compiled.SourceFunctionOwners))
    return sourceIdentityError("compiler source-owner provenance is invalid");

  std::set<uint64_t> SeenOriginalEntries;
  for (const auto &[SourceFunction, OriginalVA] :
       Compiled.SourceFunctionOriginalVAs) {
    const size_t OwnerCount = std::count_if(
        Compiled.SourceFunctionOwners.begin(),
        Compiled.SourceFunctionOwners.end(), [&](const auto &Owner) {
          return Owner.SourceFunction == SourceFunction;
        });
    if (OwnerCount != 1)
      return sourceIdentityError(
          "an original entry does not have exactly one compiler owner");
    if (!SeenOriginalEntries.insert(OriginalVA).second)
      return sourceIdentityError(
          "two source functions share an original entry");
    if (!isAuthenticatedSourceFunctionEntry(*Image, OriginalVA))
      return sourceIdentityError(
          "an original entry is not a loader-authenticated function start");
  }

  auto Requirements =
      exception_rewrite::validateExceptionRewriteContracts(Module);
  if (!Requirements)
    return Requirements.takeError();
  for (const exception_rewrite::Requirements::Function &Function :
       Requirements->Functions)
    if (!Compiled.SourceFunctionOriginalVAs.contains(Function.Name))
      return sourceIdentityError(
          "an exception-bearing source function has no original entry");
  return llvm::Error::success();
}

llvm::Error validateSourceFunctionTrampolineClosure(
    const CompiledImage &Compiled,
    llvm::ArrayRef<PatchedFunctionEntry> PatchedFunctions,
    size_t TrampolineCount) {
  if (TrampolineCount != PatchedFunctions.size() ||
      PatchedFunctions.size() != Compiled.SourceFunctionOriginalVAs.size())
    return sourceIdentityError(
        "installed trampolines do not cover every original entry exactly once");

  for (const auto &[SourceFunction, OriginalVA] :
       Compiled.SourceFunctionOriginalVAs) {
    const auto Owner = std::find_if(
        Compiled.SourceFunctionOwners.begin(),
        Compiled.SourceFunctionOwners.end(), [&](const auto &Candidate) {
          return Candidate.SourceFunction == SourceFunction;
        });
    if (Owner == Compiled.SourceFunctionOwners.end())
      return sourceIdentityError("an installed source has no compiler owner");
    const size_t Matches =
        std::count_if(PatchedFunctions.begin(), PatchedFunctions.end(),
                      [&](const PatchedFunctionEntry &Patched) {
                        return Patched.SourceFunction == SourceFunction &&
                               Patched.OriginalVA == OriginalVA &&
                               Patched.OwnerSymbol == Owner->OwnerSymbol &&
                               Patched.OwnerVA == Owner->OwnerVA;
                      });
    if (Matches != 1)
      return sourceIdentityError(
          "an original entry has no exact installed trampoline receipt");
  }
  return llvm::Error::success();
}

bool checkedAddU32(uint32_t Left, uint32_t Right, uint32_t &Result) {
  if (Right > std::numeric_limits<uint32_t>::max() - Left)
    return false;
  Result = Left + Right;
  return true;
}

bool checkedMul(uint64_t Left, uint64_t Right, uint64_t &Result) {
  if (Left != 0 && Right > std::numeric_limits<uint64_t>::max() / Left)
    return false;
  Result = Left * Right;
  return true;
}

bool checkedShiftLinkeditOffset(uint32_t &Offset, uint64_t Size, uint64_t Shift,
                                uint64_t LinkeditFileOff, uint64_t InputSize) {
  if (Offset == 0)
    return Size == 0;
  if (Offset < LinkeditFileOff || !rangeInBounds(Offset, Size, InputSize))
    return false;
  uint64_t Shifted = 0;
  if (!macho_patch_detail::checkedAdd(Offset, Shift, Shifted) ||
      Shifted > std::numeric_limits<uint32_t>::max())
    return false;
  Offset = static_cast<uint32_t>(Shifted);
  return true;
}

bool checkedShiftOptionalFileOffset(uint64_t &Offset, uint64_t Size,
                                    uint64_t Shift, uint64_t LinkeditFileOff,
                                    uint64_t InputSize) {
  if (Offset == 0)
    return Size == 0;
  if (!rangeInBounds(Offset, Size, InputSize))
    return false;
  if (Offset < LinkeditFileOff)
    return true;
  return macho_patch_detail::checkedAdd(Offset, Shift, Offset);
}

bool checkedShiftMachOLinkeditOffsets(uint8_t *Command, uint32_t ID, bool Is64,
                                      uint64_t Shift, uint64_t LinkeditFileOff,
                                      uint64_t InputSize) {
  using namespace llvm::MachO;
  uint64_t Size = 0;
  switch (ID) {
  case LC_SYMTAB: {
    auto *Value = reinterpret_cast<symtab_command *>(Command);
    if (!checkedMul(Value->nsyms, getMachONListSize(Is64), Size) ||
        !checkedShiftLinkeditOffset(Value->symoff, Size, Shift, LinkeditFileOff,
                                    InputSize) ||
        !checkedShiftLinkeditOffset(Value->stroff, Value->strsize, Shift,
                                    LinkeditFileOff, InputSize))
      return false;
    break;
  }
  case LC_DYSYMTAB: {
    auto *Value = reinterpret_cast<dysymtab_command *>(Command);
    const uint64_t ModuleSize =
        Is64 ? sizeof(dylib_module_64) : sizeof(dylib_module);
    if (!checkedMul(Value->ntoc, sizeof(dylib_table_of_contents), Size) ||
        !checkedShiftLinkeditOffset(Value->tocoff, Size, Shift, LinkeditFileOff,
                                    InputSize) ||
        !checkedMul(Value->nmodtab, ModuleSize, Size) ||
        !checkedShiftLinkeditOffset(Value->modtaboff, Size, Shift,
                                    LinkeditFileOff, InputSize) ||
        !checkedMul(Value->nextrefsyms, sizeof(dylib_reference), Size) ||
        !checkedShiftLinkeditOffset(Value->extrefsymoff, Size, Shift,
                                    LinkeditFileOff, InputSize) ||
        !checkedMul(Value->nindirectsyms, sizeof(uint32_t), Size) ||
        !checkedShiftLinkeditOffset(Value->indirectsymoff, Size, Shift,
                                    LinkeditFileOff, InputSize) ||
        !checkedMul(Value->nextrel, sizeof(relocation_info), Size) ||
        !checkedShiftLinkeditOffset(Value->extreloff, Size, Shift,
                                    LinkeditFileOff, InputSize) ||
        !checkedMul(Value->nlocrel, sizeof(relocation_info), Size) ||
        !checkedShiftLinkeditOffset(Value->locreloff, Size, Shift,
                                    LinkeditFileOff, InputSize))
      return false;
    break;
  }
  case LC_DYLD_INFO:
  case LC_DYLD_INFO_ONLY: {
    auto *Value = reinterpret_cast<dyld_info_command *>(Command);
    if (!checkedShiftLinkeditOffset(Value->rebase_off, Value->rebase_size,
                                    Shift, LinkeditFileOff, InputSize) ||
        !checkedShiftLinkeditOffset(Value->bind_off, Value->bind_size, Shift,
                                    LinkeditFileOff, InputSize) ||
        !checkedShiftLinkeditOffset(Value->weak_bind_off, Value->weak_bind_size,
                                    Shift, LinkeditFileOff, InputSize) ||
        !checkedShiftLinkeditOffset(Value->lazy_bind_off, Value->lazy_bind_size,
                                    Shift, LinkeditFileOff, InputSize) ||
        !checkedShiftLinkeditOffset(Value->export_off, Value->export_size,
                                    Shift, LinkeditFileOff, InputSize))
      return false;
    break;
  }
  case LC_CODE_SIGNATURE:
  case LC_FUNCTION_STARTS:
  case LC_DATA_IN_CODE:
  case LC_DYLD_CHAINED_FIXUPS:
  case LC_DYLD_EXPORTS_TRIE:
  case LC_SEGMENT_SPLIT_INFO:
  case LC_DYLIB_CODE_SIGN_DRS:
  case LC_LINKER_OPTIMIZATION_HINT:
  case LC_ATOM_INFO: {
    auto *Value = reinterpret_cast<linkedit_data_command *>(Command);
    if (!checkedShiftLinkeditOffset(Value->dataoff, Value->datasize, Shift,
                                    LinkeditFileOff, InputSize))
      return false;
    break;
  }
  case LC_TWOLEVEL_HINTS: {
    auto *Value = reinterpret_cast<twolevel_hints_command *>(Command);
    if (!checkedMul(Value->nhints, sizeof(twolevel_hint), Size) ||
        !checkedShiftLinkeditOffset(Value->offset, Size, Shift, LinkeditFileOff,
                                    InputSize))
      return false;
    break;
  }
  case LC_SYMSEG: {
    auto *Value = reinterpret_cast<symseg_command *>(Command);
    if (!checkedShiftLinkeditOffset(Value->offset, Value->size, Shift,
                                    LinkeditFileOff, InputSize))
      return false;
    break;
  }
  case LC_NOTE: {
    auto *Value = reinterpret_cast<note_command *>(Command);
    if (!checkedShiftOptionalFileOffset(Value->offset, Value->size, Shift,
                                        LinkeditFileOff, InputSize))
      return false;
    break;
  }
  case LC_FILESET_ENTRY: {
    auto *Value = reinterpret_cast<fileset_entry_command *>(Command);
    if (!checkedShiftOptionalFileOffset(Value->fileoff, 0, Shift,
                                        LinkeditFileOff, InputSize))
      return false;
    break;
  }
  default:
    break;
  }
  return true;
}

} // namespace

bool MachOPatcher::parseLayout(const std::vector<uint8_t> &Data,
                               PatchLayout &Layout, Arch TargetArch) {
  Layout = PatchLayout{};
  MachOHeaderInfo Hdr;
  if (!macho_patch_detail::validateLoadCommandRegion(Data, Hdr) ||
      !machOHeaderMatchesTarget(Data, TargetArch))
    return false;
  std::vector<macho_patch_detail::MachOFileRange> SegmentRanges;
  std::vector<macho_patch_detail::MachOFileRange> SectionRanges;
  if (!macho_patch_detail::collectMachOFileRanges(Data, SegmentRanges,
                                                  SectionRanges))
    return false;

  Layout.Is64 = Hdr.Is64;
  Layout.NCmds = Hdr.NCmds;
  Layout.SizeOfCmds = Hdr.SizeOfCmds;
  Layout.HeaderSize = Hdr.HeaderSize;

  // Largest executable segment, used as a fallback code region when no named
  // "__text" (or --text-section) match is found below.
  uint64_t FallbackExecVA = 0, FallbackExecSize = 0, FallbackExecFileOff = 0;
  unsigned LinkeditCount = 0;
  uint64_t MaxNonLinkeditFileEnd = 0;
  uint64_t MaxNonLinkeditVMEnd = 0;

  forEachMachOLoadCommand(
      Data.data(), Data.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t /*CmdSize*/, bool Is64) {
        if (Cmd != getMachOSegmentCmdID(Is64))
          return;

        auto SF = readMachOSegment(LCPtr, Is64);
        std::string SegName = readMachOName(SF.SegName);
        uint64_t SegEndVM = 0;
        uint64_t SegEndFile = 0;
        if (!macho_patch_detail::checkedAdd(SF.VMAddr, SF.VMSize, SegEndVM))
          return;
        if (!macho_patch_detail::checkedAdd(SF.FileOff, SF.FileSize,
                                            SegEndFile))
          return;
        if (SegEndVM > Layout.MaxVA)
          Layout.MaxVA = SegEndVM;

        if ((SF.InitProt & VM_PROT_EXECUTE) && SF.VMSize > FallbackExecSize) {
          FallbackExecVA = SF.VMAddr;
          FallbackExecSize = SF.VMSize;
          FallbackExecFileOff = SF.FileOff;
        }

        if (SegName == section_names::macho::TextSeg) {
          Layout.TextSegVA = SF.VMAddr;
          Layout.TextSegSize = SF.VMSize;
          Layout.TextSegFileOff = SF.FileOff;
          Layout.TextSegFileSize = SF.FileSize;
        }

        // Locate the target code section.  With an explicit --text-section we
        // scan every segment's sections (COFF's findPESection is likewise
        // global) so a renamed code section is found regardless of which
        // segment owns it; without an override we only honour the canonical
        // "__text" inside __TEXT.  The VA→file-offset mapping is linear within
        // a section either way, so installTrampolines()'s offset math stays
        // correct. Code relocated into a segment *other* than __TEXT is
        // patchable too: MachOLoader recovers the image base from whichever
        // segment maps file offset 0, so the entry point / LC_FUNCTION_STARTS /
        // export VAs stay correct even when the __TEXT segment itself was
        // renamed.
        if (!TextSectionOverride.empty() ||
            SegName == section_names::macho::TextSeg) {
          forEachMachOSectionAuto(
              LCPtr, Is64,
              [&](uint64_t Addr, uint64_t Size, uint32_t SectOff,
                  uint32_t /*R1*/, uint32_t /*R2*/, const char *SectName) {
                std::string SecName = readMachOName(SectName);
                bool IsTarget = TextSectionOverride.empty()
                                    ? (SecName == section_names::macho::Text)
                                    : (SecName == TextSectionOverride);
                if (IsTarget) {
                  Layout.TextSectVA = Addr;
                  Layout.TextSectSize = Size;
                  Layout.TextSectFileOff = SectOff;
                }
              });
        }

        if (SegName == section_names::macho::LinkeditSeg) {
          ++LinkeditCount;
          Layout.LinkeditVA = SF.VMAddr;
          Layout.LinkeditVMSize = SF.VMSize;
          Layout.LinkeditFileOff = SF.FileOff;
          Layout.LinkeditFileSize = SF.FileSize;
          Layout.LinkeditMaxProt = SF.MaxProt;
          Layout.LinkeditInitProt = SF.InitProt;
          Layout.LinkeditNSects = SF.NSects;
          Layout.LinkeditCmdOff = static_cast<uint32_t>(LCPtr - Data.data());
        } else {
          MaxNonLinkeditFileEnd = std::max(MaxNonLinkeditFileEnd, SegEndFile);
          MaxNonLinkeditVMEnd = std::max(MaxNonLinkeditVMEnd, SegEndVM);
        }
      });

  // No "__text" (or --text-section) section matched — fall back to the
  // executable segment so trampolines still land on a binary whose code section
  // was renamed by a protector.  Mirrors BinaryImage::getTextSection() and the
  // ELF patcher's PT_LOAD(PF_X) pick; the VA→file-offset mapping is linear
  // within a segment, so the offset math in installTrampolines() stays correct.
  if (Layout.TextSectVA == 0 && FallbackExecSize != 0) {
    Layout.TextSectVA = FallbackExecVA;
    Layout.TextSectSize = FallbackExecSize;
    Layout.TextSectFileOff = FallbackExecFileOff;
  }

  Layout.MaxFileOff = Data.size();
  uint64_t LinkeditFileEnd = 0;
  const uint64_t PageSize = machoPageSize(TargetArch);
  return LinkeditCount == 1 && Layout.LinkeditFileOff != 0 &&
         Layout.LinkeditVA != 0 && Layout.LinkeditNSects == 0 &&
         PageSize != 0 && Layout.LinkeditFileOff % PageSize == 0 &&
         Layout.LinkeditVA % PageSize == 0 &&
         macho_patch_detail::checkedAdd(Layout.LinkeditFileOff,
                                        Layout.LinkeditFileSize,
                                        LinkeditFileEnd) &&
         LinkeditFileEnd == Data.size() &&
         MaxNonLinkeditFileEnd <= Layout.LinkeditFileOff &&
         MaxNonLinkeditVMEnd <= Layout.LinkeditVA;
}

uint64_t MachOPatcher::plannedExecSegmentVA(const std::vector<uint8_t> &Binary,
                                            Arch TargetArch) {
  PatchLayout Layout;
  if (!parseLayout(Binary, Layout, TargetArch))
    return 0;
  // The new segment takes over the old __LINKEDIT VA (which is shifted up).
  return Layout.LinkeditVA;
}

bool MachOPatcher::validateExecSegmentInstall(
    const std::vector<uint8_t> &Binary, Arch TargetArch,
    const ExecSegmentInstallReceipt &Receipt) {
  const uint64_t PageSize = machoPageSize(TargetArch);
  PatchLayout Layout;
  if (PageSize == 0 || !parseLayout(Binary, Layout, TargetArch) ||
      Layout.Is64 != Receipt.Is64 || Layout.NCmds != Receipt.NCmds ||
      Layout.SizeOfCmds != Receipt.SizeOfCmds ||
      Layout.LinkeditVA != Receipt.LinkeditVA ||
      Layout.LinkeditVMSize != Receipt.LinkeditVMSize ||
      Layout.LinkeditFileOff != Receipt.LinkeditFileOff ||
      Layout.LinkeditFileSize != Receipt.LinkeditFileSize ||
      Layout.LinkeditMaxProt != Receipt.LinkeditMaxProt ||
      Layout.LinkeditInitProt != Receipt.LinkeditInitProt ||
      Layout.LinkeditNSects != Receipt.LinkeditNSects)
    return false;

  unsigned SegmentCount = 0;
  unsigned LinkeditCount = 0;
  bool Exact = true;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t, bool Is64) {
        if (ID != getMachOSegmentCmdID(Is64))
          return;
        const MachOSegFields Segment = readMachOSegment(Command, Is64);
        const std::string Name = readMachOName(Segment.SegName);
        if (Name == Receipt.SegmentName) {
          ++SegmentCount;
          Exact &= Segment.VMAddr == Receipt.SegmentVA &&
                   Segment.VMSize == Receipt.SegmentVMSize &&
                   Segment.FileOff == Receipt.SegmentFileOff &&
                   Segment.FileSize == Receipt.SegmentFileSize &&
                   Segment.MaxProt == (VM_PROT_READ | VM_PROT_EXECUTE) &&
                   Segment.InitProt == (VM_PROT_READ | VM_PROT_EXECUTE) &&
                   Segment.NSects == 0;
        }
        if (Name == section_names::macho::LinkeditSeg) {
          ++LinkeditCount;
          Exact &= Segment.VMAddr == Receipt.LinkeditVA &&
                   Segment.VMSize == Receipt.LinkeditVMSize &&
                   Segment.FileOff == Receipt.LinkeditFileOff &&
                   Segment.FileSize == Receipt.LinkeditFileSize &&
                   Segment.MaxProt == Receipt.LinkeditMaxProt &&
                   Segment.InitProt == Receipt.LinkeditInitProt &&
                   Segment.NSects == Receipt.LinkeditNSects;
        }
      });

  uint64_t SegmentVMEnd = 0;
  uint64_t SegmentFileEnd = 0;
  uint64_t LinkeditFileEnd = 0;
  return Exact && SegmentCount == 1 && LinkeditCount == 1 &&
         Receipt.SegmentVA % PageSize == 0 &&
         Receipt.SegmentFileOff % PageSize == 0 &&
         Receipt.LinkeditVA % PageSize == 0 &&
         Receipt.LinkeditFileOff % PageSize == 0 &&
         macho_patch_detail::checkedAdd(Receipt.SegmentVA,
                                        Receipt.SegmentVMSize, SegmentVMEnd) &&
         macho_patch_detail::checkedAdd(
             Receipt.SegmentFileOff, Receipt.SegmentFileSize, SegmentFileEnd) &&
         macho_patch_detail::checkedAdd(Receipt.LinkeditFileOff,
                                        Receipt.LinkeditFileSize,
                                        LinkeditFileEnd) &&
         SegmentVMEnd == Receipt.LinkeditVA &&
         SegmentFileEnd == Receipt.LinkeditFileOff &&
         LinkeditFileEnd == Binary.size();
}

uint64_t MachOPatcher::appendExecSegment(std::vector<uint8_t> &Binary,
                                         llvm::ArrayRef<uint8_t> Code,
                                         llvm::StringRef SegName,
                                         Arch TargetArch) {
  return appendExecSegmentImpl(Binary, Code, SegName, TargetArch, nullptr);
}

uint64_t
MachOPatcher::appendExecSegmentImpl(std::vector<uint8_t> &Binary,
                                    llvm::ArrayRef<uint8_t> Code,
                                    llvm::StringRef SegName, Arch TargetArch,
                                    ExecSegmentInstallReceipt *Receipt) {
  const uint64_t PageSize = machoPageSize(TargetArch);
  PatchLayout Layout;
  if (!parseLayout(Binary, Layout, TargetArch) || Code.empty())
    return 0;
  std::string SegStr = SegName.empty() ? kDefaultNdTextSegment : SegName.str();
  if (SegStr.empty() || SegStr.size() > kMachONameSize ||
      SegStr.find('\0') != std::string::npos)
    return 0;
  bool SegmentNameConflict = false;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t, bool Is64) {
        if (ID == getMachOSegmentCmdID(Is64) &&
            readMachOName(readMachOSegment(Command, Is64).SegName) == SegStr)
          SegmentNameConflict = true;
      });
  if (SegmentNameConflict)
    return 0;

  const bool Is64 = Layout.Is64;
  const uint32_t SegCmdID = getMachOSegmentCmdID(Is64);
  const uint32_t HdrSize = Layout.HeaderSize;
  const uint32_t NewSegCmdSize = getMachOSegmentCmdSize(Is64);
  const uint64_t TextSize = Code.size();
  uint64_t InsertOff = 0;
  uint64_t CodePadded = 0;
  uint64_t NewLinkeditOff = 0;
  uint64_t NewLinkeditVMAddr = 0;
  uint64_t FinalSize = 0;
  if (!macho_patch_detail::checkedAlignUp(Layout.LinkeditFileOff, PageSize,
                                          InsertOff) ||
      !macho_patch_detail::checkedAlignUp(TextSize, PageSize, CodePadded) ||
      CodePadded == 0 ||
      !macho_patch_detail::checkedAdd(InsertOff, CodePadded, NewLinkeditOff) ||
      !macho_patch_detail::checkedAdd(Layout.LinkeditVA, CodePadded,
                                      NewLinkeditVMAddr) ||
      !macho_patch_detail::checkedAdd(NewLinkeditOff, Layout.LinkeditFileSize,
                                      FinalSize) ||
      NewLinkeditOff < Layout.LinkeditFileOff ||
      FinalSize > std::numeric_limits<size_t>::max() ||
      FinalSize > Binary.max_size())
    return 0;
  const uint64_t Shift = NewLinkeditOff - Layout.LinkeditFileOff;
  if (Shift > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      NewLinkeditOff > std::numeric_limits<uint32_t>::max() ||
      (!Is64 &&
       (Layout.LinkeditVA > std::numeric_limits<uint32_t>::max() ||
        CodePadded > std::numeric_limits<uint32_t>::max() ||
        InsertOff > std::numeric_limits<uint32_t>::max() ||
        NewLinkeditVMAddr > std::numeric_limits<uint32_t>::max() ||
        Layout.LinkeditVMSize > std::numeric_limits<uint32_t>::max() ||
        Layout.LinkeditFileSize > std::numeric_limits<uint32_t>::max())))
    return 0;

  std::vector<uint8_t> NewSegBuf = buildMachOSegmentCmd(Is64, SegStr);
  if (readMachOName(readMachOSegment(NewSegBuf.data(), Is64).SegName) != SegStr)
    return 0;
  MachOSegFields NewSF{};
  NewSF.VMAddr = Layout.LinkeditVA;
  NewSF.VMSize = CodePadded;
  NewSF.FileOff = InsertOff;
  NewSF.FileSize = CodePadded;
  NewSF.MaxProt = VM_PROT_READ | VM_PROT_EXECUTE;
  NewSF.InitProt = VM_PROT_READ | VM_PROT_EXECUTE;
  writeMachOSegment(NewSegBuf.data(), Is64, NewSF);

  struct CmdInfo {
    uint64_t Off = 0;
    uint32_t Size = 0;
    uint32_t Cmd = 0;
  };
  std::vector<CmdInfo> Cmds;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t CmdSize, bool) {
        const uint64_t Offset = LCPtr - Binary.data();
        if (Offset <= std::numeric_limits<uint32_t>::max())
          Cmds.push_back({Offset, CmdSize, Cmd});
      });
  if (Cmds.size() != Layout.NCmds)
    return 0;

  uint64_t FirstDataOff =
      findFirstMachODataOffset(Binary.data(), Binary.size());
  uint64_t HdrPlusCmds = 0;
  if (!macho_patch_detail::checkedAdd(HdrSize, Layout.SizeOfCmds,
                                      HdrPlusCmds) ||
      FirstDataOff > Binary.size() || FirstDataOff < HdrPlusCmds) {
    llvm::WithColor::error() << "macho_patch: no header space\n";
    return 0;
  }
  const uint64_t Avail = FirstDataOff - HdrPlusCmds;
  uint32_t Freed = 0;
  for (const CmdInfo &Command : Cmds)
    if (shouldDropLoadCommand(Command.Cmd) &&
        !checkedAddU32(Freed, Command.Size, Freed))
      return 0;
  uint64_t AvailableWithFreed = 0;
  if (!macho_patch_detail::checkedAdd(Avail, Freed, AvailableWithFreed) ||
      AvailableWithFreed < NewSegCmdSize) {
    llvm::WithColor::error() << "macho_patch: no header space\n";
    return 0;
  }

  std::vector<std::vector<uint8_t>> OutCmds;
  uint32_t TotCmds = 0, TotSize = 0;
  unsigned InsertedSegments = 0;
  for (const CmdInfo &Command : Cmds) {
    if (shouldDropLoadCommand(Command.Cmd))
      continue;
    bool IsLinkedit = false;
    if (Command.Cmd == SegCmdID) {
      const MachOSegFields SF =
          readMachOSegment(Binary.data() + Command.Off, Is64);
      IsLinkedit =
          (readMachOName(SF.SegName) == section_names::macho::LinkeditSeg);
    }
    if (IsLinkedit) {
      OutCmds.push_back(NewSegBuf);
      ++InsertedSegments;
      if (!checkedAddU32(TotCmds, 1, TotCmds) ||
          !checkedAddU32(TotSize, NewSegCmdSize, TotSize))
        return 0;
    }
    std::vector<uint8_t> CmdData(Command.Size);
    std::memcpy(CmdData.data(), Binary.data() + Command.Off, Command.Size);
    if (IsLinkedit) {
      auto SF = readMachOSegment(CmdData.data(), Is64);
      SF.VMAddr = NewLinkeditVMAddr;
      SF.FileOff = NewLinkeditOff;
      SF.VMSize = Layout.LinkeditVMSize;
      SF.FileSize = Layout.LinkeditFileSize;
      writeMachOSegment(CmdData.data(), Is64, SF);
    }
    if (!checkedShiftMachOLinkeditOffsets(CmdData.data(), Command.Cmd, Is64,
                                          Shift, Layout.LinkeditFileOff,
                                          Binary.size()))
      return 0;
    OutCmds.push_back(std::move(CmdData));
    if (!checkedAddU32(TotCmds, 1, TotCmds) ||
        !checkedAddU32(TotSize, Command.Size, TotSize))
      return 0;
  }
  uint64_t FinalCommandsEnd = 0;
  if (InsertedSegments != 1 ||
      !macho_patch_detail::checkedAdd(HdrSize, TotSize, FinalCommandsEnd) ||
      FinalCommandsEnd > FirstDataOff)
    return 0;

  ExecSegmentInstallReceipt Expected;
  Expected.SegmentName = SegStr;
  Expected.Is64 = Is64;
  Expected.NCmds = TotCmds;
  Expected.SizeOfCmds = TotSize;
  Expected.SegmentVA = Layout.LinkeditVA;
  Expected.SegmentVMSize = CodePadded;
  Expected.SegmentFileOff = InsertOff;
  Expected.SegmentFileSize = CodePadded;
  Expected.LinkeditVA = NewLinkeditVMAddr;
  Expected.LinkeditVMSize = Layout.LinkeditVMSize;
  Expected.LinkeditFileOff = NewLinkeditOff;
  Expected.LinkeditFileSize = Layout.LinkeditFileSize;
  Expected.LinkeditMaxProt = Layout.LinkeditMaxProt;
  Expected.LinkeditInitProt = Layout.LinkeditInitProt;
  Expected.LinkeditNSects = Layout.LinkeditNSects;

  const size_t LinkeditOffset = static_cast<size_t>(Layout.LinkeditFileOff);
  std::vector<uint8_t> LinkeditData(Binary.begin() + LinkeditOffset,
                                    Binary.end());
  if (LinkeditData.size() != Layout.LinkeditFileSize)
    return 0;
  std::vector<uint8_t> Candidate = Binary;
  Candidate.resize(static_cast<size_t>(InsertOff), 0);
  Candidate.resize(static_cast<size_t>(NewLinkeditOff), 0);
  std::memcpy(Candidate.data() + static_cast<size_t>(InsertOff), Code.data(),
              Code.size());
  Candidate.insert(Candidate.end(), LinkeditData.begin(), LinkeditData.end());
  if (Candidate.size() != FinalSize)
    return 0;

  setMachOHeaderCmds(Candidate.data(), Is64, TotCmds, TotSize);
  uint64_t WriteOff = HdrSize;
  for (const std::vector<uint8_t> &Command : OutCmds) {
    if (!rangeInBounds(WriteOff, Command.size(), FirstDataOff))
      return 0;
    std::memcpy(Candidate.data() + static_cast<size_t>(WriteOff),
                Command.data(), Command.size());
    WriteOff += Command.size();
  }
  if (WriteOff < FirstDataOff)
    std::memset(Candidate.data() + static_cast<size_t>(WriteOff), 0,
                static_cast<size_t>(FirstDataOff - WriteOff));

  if (!validateExecSegmentInstall(Candidate, TargetArch, Expected))
    return 0;

  Binary.swap(Candidate);
  if (Receipt)
    *Receipt = std::move(Expected);
  return Layout.LinkeditVA;
}

PatchResult MachOPatcher::patch(const std::filesystem::path &InputPath,
                                const std::filesystem::path &OutputPath,
                                llvm::Module &Mod, Arch TargetArch) {
  return patch(InputPath, OutputPath, Mod, TargetArch, {});
}

PatchResult MachOPatcher::patch(const std::filesystem::path &InputPath,
                                const std::filesystem::path &OutputPath,
                                llvm::Module &Mod, Arch TargetArch,
                                const MachOPatchOptions &Opts) {
  if (!archMachOPatchSupported(TargetArch)) {
    llvm::WithColor::error()
        << "macho_patch: unsupported arch " << getArchName(TargetArch) << "\n";
    return PatchResult{};
  }

  MachOLoader Loader;
  auto LoadedImage = Loader.load(InputPath);
  if (!LoadedImage) {
    llvm::WithColor::error() << "macho_patch: loader authentication failed: "
                             << llvm::toString(LoadedImage.takeError()) << "\n";
    return PatchResult{};
  }
  const BinaryImage AuthenticatedImage = std::move(*LoadedImage);

  return readPatchWrite(
      InputPath, OutputPath, /*SetExecPerm=*/true, "macho_patch",
      [&](std::vector<uint8_t> &Binary, PatchResult &Result) -> bool {
        PatchLayout Layout;
        if (!parseLayout(Binary, Layout, TargetArch))
          return false;

        std::string AuthenticatedDetail;
        if (!cachedImageBytesMatchInput(Binary, AuthenticatedImage, TargetArch,
                                        Layout.Is64, AuthenticatedDetail) ||
            !cachedAddressModelMatchesInput(Binary, AuthenticatedImage,
                                            AuthenticatedDetail)) {
          llvm::WithColor::error()
              << "macho_patch: loader authentication does not match input: "
              << AuthenticatedDetail << "\n";
          return false;
        }

        if (CachedImage) {
          std::string Detail;
          if (!cachedImageBytesMatchInput(Binary, *CachedImage, TargetArch,
                                          Layout.Is64, Detail)) {
            llvm::WithColor::error()
                << "macho_patch: image context does not match input: " << Detail
                << "\n";
            return false;
          }
          if (!cachedAddressModelMatchesInput(Binary, *CachedImage, Detail)) {
            llvm::WithColor::error()
                << "macho_patch: image context does not match input: " << Detail
                << "\n";
            return false;
          }
          if (TargetArch == Arch::ARM &&
              CachedImage->Mode != InstructionMode::Default &&
              CachedImage->Mode != AuthenticatedImage.Mode) {
            llvm::WithColor::error()
                << "macho_patch: cached ARM code mode disagrees with "
                   "authenticated nlist metadata\n";
            return false;
          }
        }

        auto RewriteTarget = resolveMachORewriteTarget(
            Binary, TargetArch, AuthenticatedImage.Mode, Mod);
        if (!RewriteTarget) {
          llvm::WithColor::error()
              << "macho_patch: " << llvm::toString(RewriteTarget.takeError())
              << "\n";
          return false;
        }

        uint64_t NewSegVMAddr = plannedExecSegmentVA(Binary, TargetArch);
        if (NewSegVMAddr == 0) {
          llvm::WithColor::error() << "macho_patch: cannot plan exec segment\n";
          return false;
        }

        // The address-model resolver: maps external symbol names to VAs in the
        // target binary (exports / import stubs / __nd_data_* absolute data).
        auto SerializeResolvedCode =
            [&](uint64_t VA, bool IsCode) -> std::optional<uint64_t> {
          if (!IsCode)
            return VA;
          if (TargetArch != Arch::ARM)
            return serializeCodePointer(VA, TargetArch, RewriteTarget->Mode);
          if (!RewriteTarget->ARM32Modes)
            return std::nullopt;
          auto Serialized =
              macho_arm32::serializeCodePointer(*RewriteTarget->ARM32Modes, VA);
          if (!Serialized) {
            llvm::consumeError(Serialized.takeError());
            return std::nullopt;
          }
          return *Serialized;
        };
        auto IsExecutable = [&](uint64_t VA) {
          const Segment *Seg = AuthenticatedImage.getSegmentFor(VA);
          return Seg && Seg->isExecutable();
        };
        std::optional<std::string> SymbolResolutionFailure;
        auto Resolve =
            [&](const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request)
            -> std::optional<uint64_t> {
          std::string Name = Request.Symbol.str();
          if (SymbolResolutionFailure)
            return std::nullopt;
          auto Use =
              macho_patch_detail::classifyMachOSymbolUse(TargetArch, Request);
          if (!Use) {
            SymbolResolutionFailure = llvm::toString(Use.takeError());
            return std::nullopt;
          }
          auto Target = macho_patch_detail::resolveUniqueMachOSymbol(
              AuthenticatedImage, Name, *Use);
          if (!Target) {
            SymbolResolutionFailure = llvm::toString(Target.takeError());
            return std::nullopt;
          }
          if (*Target) {
            const bool IsCode =
                (*Target)->Kind ==
                macho_patch_detail::MachOSymbolTargetKind::Callable;
            return SerializeResolvedCode((*Target)->Address, IsCode);
          }
          if (auto VA = parseNdDataSymbol(Name)) {
            if (*Use == macho_patch_detail::MachOSymbolUse::ImportSlot) {
              // Darwin emits LSDA type-table entries through a GOT reference
              // even when the IR names one of our synthetic address symbols.
              // Such EH symbols encode the original non-lazy pointer-slot VA,
              // so accept the exact slot only after authenticating its mapped
              // section and bounds.  Local GOT entries intentionally have no
              // import-map name, but are still valid non-lazy pointer slots.
              const Section *Section = AuthenticatedImage.getSectionFor(*VA);
              const Segment *Segment = AuthenticatedImage.getSegmentFor(*VA);
              const uint32_t PointerSize = AuthenticatedImage.getPointerSize();
              if (!Section ||
                  (Section->Type & SECTION_TYPE) !=
                      S_NON_LAZY_SYMBOL_POINTERS ||
                  !Segment || !Segment->isReadable()) {
                SymbolResolutionFailure =
                    (llvm::Twine("Mach-O synthetic import slot '") + Name +
                     "' is not in authenticated readable non-lazy pointers")
                        .str();
                return std::nullopt;
              }
              const uint64_t SectionOffset = *VA - Section->VA;
              const uint64_t SegmentOffset = *VA - Segment->VA;
              if (PointerSize == 0 || SectionOffset % PointerSize != 0 ||
                  SectionOffset > Section->Size ||
                  PointerSize > Section->Size - SectionOffset ||
                  SegmentOffset > Segment->Size ||
                  PointerSize > Segment->Size - SegmentOffset) {
                SymbolResolutionFailure =
                    (llvm::Twine("Mach-O synthetic import slot '") + Name +
                     "' exceeds authenticated pointer bounds")
                        .str();
                return std::nullopt;
              }
              return *VA;
            }
            const Segment *Segment = AuthenticatedImage.getSegmentFor(*VA);
            if (!Segment || !Segment->isReadable()) {
              SymbolResolutionFailure =
                  (llvm::Twine("Mach-O synthetic data symbol '") + Name +
                   "' is outside authenticated readable memory")
                      .str();
              return std::nullopt;
            }
            bool IsCode = IsExecutable(*VA) &&
                          AuthenticatedImage.CodeRefTargets.count(*VA) != 0;
            if (*Use == macho_patch_detail::MachOSymbolUse::Callable &&
                !IsCode) {
              SymbolResolutionFailure =
                  (llvm::Twine("Mach-O callable reference cannot target '") +
                   Name + "' without authenticated code provenance")
                      .str();
              return std::nullopt;
            }
            return SerializeResolvedCode(*VA, IsCode);
          }
          if (auto VA = parseNdCodePtrSymbol(Name)) {
            if (*Use == macho_patch_detail::MachOSymbolUse::ImportSlot) {
              SymbolResolutionFailure =
                  (llvm::Twine("Mach-O import-slot reference cannot target '") +
                   Name + "'")
                      .str();
              return std::nullopt;
            }
            const Segment *Segment = AuthenticatedImage.getSegmentFor(*VA);
            if (!Segment || !Segment->isReadable()) {
              SymbolResolutionFailure =
                  (llvm::Twine("Mach-O synthetic code-pointer table '") + Name +
                   "' is outside authenticated readable memory")
                      .str();
              return std::nullopt;
            }
            const Section *CodeSection = AuthenticatedImage.getSectionFor(*VA);
            const uint32_t SectionType =
                CodeSection ? CodeSection->Type & SECTION_TYPE : 0;
            const uint32_t SectionAttrs =
                CodeSection ? CodeSection->Type & SECTION_ATTRIBUTES : 0;
            const bool IsCode =
                IsExecutable(*VA) && CodeSection &&
                (AuthenticatedImage.isImportStubAt(*VA) ||
                 SectionType == S_SYMBOL_STUBS ||
                 (SectionAttrs &
                  (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) != 0);
            if (*Use == macho_patch_detail::MachOSymbolUse::Callable &&
                !IsCode) {
              SymbolResolutionFailure =
                  (llvm::Twine("Mach-O callable reference cannot target '") +
                   Name + "' without authenticated code provenance")
                      .str();
              return std::nullopt;
            }
            return SerializeResolvedCode(*VA, IsCode);
          }
          return std::nullopt;
        };

        auto CompactRegion = findMachOCompactUnwindRegion(Binary);
        if (!CompactRegion) {
          llvm::WithColor::error()
              << llvm::toString(CompactRegion.takeError()) << "\n";
          return false;
        }
        const bool HasFinalCompactSection = CompactRegion->has_value();

        // dyld exposes the image's __TEXT,__eh_frame to libunwind.  Keep the
        // original records and compile regenerated records directly into the
        // file-backed tail of that section so their PC-relative fields are
        // based on the address at which dyld will actually register them.
        std::optional<MachOEHFrameRegion> EHRegion =
            findMachOEHFrameRegion(Binary);
        auto FixedSectionVA =
            [&](llvm::StringRef Name) -> std::optional<uint64_t> {
          if (EHRegion && Name == section_names::macho::EhFrame)
            return EHRegion->AppendVA;
          return std::nullopt;
        };

        CompiledImage Img = compileImageForPatchWithFixedSectionVAs(
            Mod, TargetArch, BinaryFormat::MachO, NewSegVMAddr, Resolve,
            FixedSectionVA, /*ImageBaseVA=*/0, RewriteTarget->Triple);
        if (SymbolResolutionFailure) {
          llvm::WithColor::error()
              << "macho_patch: " << *SymbolResolutionFailure << "\n";
          return false;
        }
        if (!Img.Success || Img.Bytes.empty()) {
          llvm::WithColor::error()
              << "macho_patch: compileImageForPatch failed\n";
          return false;
        }
        if (!Img.Unresolved.empty()) {
          llvm::WithColor::error()
              << "macho_patch: " << Img.Unresolved.size()
              << " unresolved symbols; refusing partial patch\n";
          LLVM_DEBUG({
            for (const std::string &Unresolved : Img.Unresolved)
              llvm::dbgs() << "  unresolved: " << Unresolved << "\n";
          });
          return false;
        }
        if (!RewriteTarget->Triple.empty() &&
            (Img.TargetTriple != RewriteTarget->Triple ||
             Img.TargetMode != RewriteTarget->Mode)) {
          llvm::WithColor::error()
              << "macho_patch: compiled target identity does not match the "
                 "authenticated input ABI\n";
          return false;
        }
        if (llvm::Error Error = validateSourceFunctionIdentities(
                Img, Mod, &AuthenticatedImage)) {
          llvm::WithColor::error() << llvm::toString(std::move(Error)) << "\n";
          return false;
        }

        MachOCompactUnwindRecords GeneratedCompact;
        bool HasGeneratedCompact = false;
        for (const CompiledSection &Section : Img.Sections)
          HasGeneratedCompact |=
              Section.Name == section_names::macho::CompactUnwind;
        const bool InstallGeneratedCompact =
            HasGeneratedCompact && HasFinalCompactSection;
        if (InstallGeneratedCompact) {
          auto Parsed = parseGeneratedMachOCompactUnwind(
              Img, AuthenticatedImage, (**CompactRegion).MachHeaderVA,
              llvm::endianness::little);
          if (!Parsed) {
            llvm::WithColor::error()
                << llvm::toString(Parsed.takeError()) << "\n";
            return false;
          }
          GeneratedCompact = std::move(*Parsed);
        }

        std::vector<uint8_t> Candidate = Binary;
        auto EHReceipt = installMachOEHFrameWithReceipt(
            Candidate, EHRegion, Img, Mod,
            InstallGeneratedCompact ? &GeneratedCompact : nullptr);
        if (!EHReceipt) {
          llvm::WithColor::error()
              << llvm::toString(EHReceipt.takeError()) << "\n";
          return false;
        }

        if (InstallGeneratedCompact) {
          auto Bound =
              bindMachOCompactUnwindDwarfFDEs(GeneratedCompact, *EHReceipt);
          if (!Bound) {
            llvm::WithColor::error()
                << llvm::toString(Bound.takeError()) << "\n";
            return false;
          }
          GeneratedCompact = std::move(*Bound);
        }

        const uint64_t TextSize = Img.Bytes.size();
        ExecSegmentInstallReceipt SegmentReceipt;
        uint64_t Placed =
            appendExecSegmentImpl(Candidate, Img.Bytes, Opts.SegmentName,
                                  TargetArch, &SegmentReceipt);
        if (Placed != NewSegVMAddr) {
          llvm::WithColor::error() << "macho_patch: appendExecSegment failed\n";
          return false;
        }

        size_t TrampolineCount = 0;
        std::vector<PatchedFunctionEntry> PatchedFunctions;
        if (Layout.TextSectVA != 0 && Layout.TextSectSize != 0) {
          TrampolineCount = installTrampolines(
              Candidate, Img.SymbolAddrs, Layout.TextSectVA,
              Layout.TextSectSize, Layout.TextSectFileOff,
              /*ImageBase=*/0, TargetArch, RewriteTarget->Mode,
              &AuthenticatedImage.Symbols, &AuthenticatedImage.KnownCodeRanges,
              &AuthenticatedImage.Exports,
              /*PatchedOriginalEntries=*/nullptr,
              /*PatchedEntryMappings=*/nullptr, &PatchedFunctions,
              Img.SourceFunctionOwners, Img.SourceFunctionOriginalVAs);
        }
        if (llvm::Error Error = validateSourceFunctionTrampolineClosure(
                Img, PatchedFunctions, TrampolineCount)) {
          llvm::WithColor::error() << llvm::toString(std::move(Error)) << "\n";
          return false;
        }

        std::vector<MachOCompactUnwindRangeMapping> RangeMappings;
        if (InstallGeneratedCompact) {
          auto BuiltMappings = buildMachOCompactUnwindRangeMappings(
              Candidate, TargetArch, GeneratedCompact, PatchedFunctions,
              llvm::endianness::little);
          if (!BuiltMappings) {
            llvm::WithColor::error()
                << llvm::toString(BuiltMappings.takeError()) << "\n";
            return false;
          }
          RangeMappings = std::move(*BuiltMappings);
        }

        auto CompactPlan = prepareMachOCompactUnwindInstall(
            Candidate, TargetArch, GeneratedCompact, RangeMappings,
            llvm::endianness::little);
        if (!CompactPlan) {
          llvm::WithColor::error()
              << llvm::toString(CompactPlan.takeError()) << "\n";
          return false;
        }
        auto CompactReceipt =
            applyMachOCompactUnwindInstall(Candidate, *CompactPlan);
        if (!CompactReceipt) {
          llvm::WithColor::error()
              << llvm::toString(CompactReceipt.takeError()) << "\n";
          return false;
        }
        if (!validateExecSegmentInstall(Candidate, TargetArch,
                                        SegmentReceipt)) {
          llvm::WithColor::error()
              << "macho_patch: final segment layout validation failed\n";
          return false;
        }

        Binary.swap(Candidate);
        Result.TrampolineCount = TrampolineCount;
        Result.Success = true;
        Result.CodeSize = TextSize;
        return true;
      });
}

} // namespace neverd
