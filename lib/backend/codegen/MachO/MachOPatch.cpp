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
#include "llvm/Support/Endian.h"
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

struct MachOSymbolTarget {
  uint64_t Address = 0;
  bool IsCode = false;
};

enum class MachOSymbolNameMatch : uint8_t { Exact, UnderscoreAlias };

std::optional<MachOSymbolNameMatch>
classifyMachOSymbolName(llvm::StringRef Requested,
                        llvm::StringRef Candidate) {
  if (Candidate == Requested)
    return MachOSymbolNameMatch::Exact;
  if ((Requested.starts_with("_") &&
       Candidate == Requested.drop_front()) ||
      (Candidate.starts_with("_") &&
       Candidate.drop_front() == Requested))
    return MachOSymbolNameMatch::UnderscoreAlias;
  return std::nullopt;
}

llvm::Expected<std::optional<MachOSymbolTarget>>
resolveUniqueMachOSymbol(const BinaryImage &Image, llvm::StringRef Requested) {
  using CandidateKey = std::pair<uint64_t, bool>;
  std::set<CandidateKey> ExactCandidates;
  std::set<CandidateKey> AliasCandidates;
  bool InvalidExactCandidate = false;
  bool InvalidAliasCandidate = false;

  auto Record = [&](llvm::StringRef Name, uint64_t Address, bool IsCode) {
    std::optional<MachOSymbolNameMatch> Match =
        classifyMachOSymbolName(Requested, Name);
    if (!Match)
      return;
    auto &Candidates = *Match == MachOSymbolNameMatch::Exact
                           ? ExactCandidates
                           : AliasCandidates;
    Candidates.emplace(Address, IsCode);
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
  auto IsExecutable = [&](uint64_t Address) {
    const Segment *Segment = Image.getSegmentFor(Address);
    return Segment && Segment->isExecutable();
  };

  for (const auto &[SlotVA, ImportedName] : Image.ImportPtrSlots) {
    if (!classifyMachOSymbolName(Requested, ImportedName))
      continue;
    const uint8_t *Pointer = Image.readVA(SlotVA, Image.getPointerSize());
    if (!Pointer) {
      RecordInvalid(ImportedName);
      continue;
    }
    const uint64_t Target =
        Image.is64Bit() ? llvm::support::endian::read64le(Pointer)
                        : llvm::support::endian::read32le(Pointer);
    Record(ImportedName, Target, /*IsCode=*/true);
  }
  for (const Export &Item : Image.Exports)
    Record(Item.Name, Item.Addr, IsExecutable(Item.Addr));
  for (const Import &Item : Image.Imports)
    Record(Item.Name, Item.IATAddr, IsExecutable(Item.IATAddr));
  for (const Symbol &Item : Image.Symbols)
    if (Item.IsFunc)
      Record(Item.Name, Item.Addr, /*IsCode=*/true);

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
         "' has an unreadable pointer-slot candidate")
            .str());
  if (Candidates.size() > 1)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        (llvm::Twine("ambiguous Mach-O symbol '") + Requested +
         "' resolves to multiple addresses or kinds")
            .str());
  if (Candidates.empty())
    return std::optional<MachOSymbolTarget>();
  const auto &[Address, IsCode] = *Candidates.begin();
  return std::optional<MachOSymbolTarget>(
      MachOSymbolTarget{Address, IsCode});
}

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
        auto Resolve = [&](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
          std::string Name = Sym.str();
          if (SymbolResolutionFailure)
            return std::nullopt;
          auto Target = resolveUniqueMachOSymbol(AuthenticatedImage, Name);
          if (!Target) {
            SymbolResolutionFailure = llvm::toString(Target.takeError());
            return std::nullopt;
          }
          if (*Target)
            return SerializeResolvedCode((*Target)->Address,
                                         (*Target)->IsCode);
          if (auto VA = parseNdDataSymbol(Name)) {
            bool IsCode = IsExecutable(*VA) &&
                          AuthenticatedImage.CodeRefTargets.count(*VA) != 0;
            return SerializeResolvedCode(*VA, IsCode);
          }
          if (auto VA = parseNdCodePtrSymbol(Name))
            return SerializeResolvedCode(*VA, false);
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
        if (!RewriteTarget->Triple.empty() &&
            (Img.TargetTriple != RewriteTarget->Triple ||
             Img.TargetMode != RewriteTarget->Mode)) {
          llvm::WithColor::error()
              << "macho_patch: compiled target identity does not match the "
                 "authenticated input ABI\n";
          return false;
        }
        if (TargetArch == Arch::ARM && !Img.Unresolved.empty()) {
          llvm::WithColor::error()
              << "macho_patch: ARM code targets require exact nlist mode "
                 "provenance\n";
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

        if (!Img.Unresolved.empty()) {
          llvm::WithColor::warning() << "macho_patch: " << Img.Unresolved.size()
                                     << " unresolved symbols\n";
          LLVM_DEBUG({
            for (auto &U : Img.Unresolved)
              llvm::dbgs() << "  unresolved: " << U << "\n";
          });
        }

        Binary.swap(Candidate);
        Result.TrampolineCount = TrampolineCount;
        Result.Success = true;
        Result.CodeSize = TextSize;
        return true;
      });
}

} // namespace neverd
