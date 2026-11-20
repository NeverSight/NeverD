//===- BinaryRewriter.cpp - Common binary patching logic -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements format-agnostic binary patching operations: text section
/// discovery, trampoline installation, and the read-patch-write skeleton
/// shared by the COFF, ELF, and Mach-O patching pipelines.
///
/// Placement-ready image compilation lives in CompiledImage.cpp.
/// In-place rewriting logic lives in InplaceRewriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/BinaryRewriter.h"

#include "BlockAddressUse.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryUtils.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/codegen/COFF/COFFInplace.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/ELF/ELFInplace.h"
#include "neverd/backend/codegen/ELF/ELFPatch.h"
#include "neverd/backend/codegen/MachO/MachOInplace.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/object/PELayout.h"
#include "neverd/support/BranchEncoding.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#define DEBUG_TYPE "neverd-rewriter"

namespace neverd {

// LLVM's final-image assembler can resolve an internal ARM BL against a
// private label before ELF symbol flags are available. The immediate then
// reaches the target but leaves the CPU in the caller's state. Reconcile
// direct calls against exact source-owner receipts and rewrite only a proven
// cross-state BL into BLX. Other branch forms retain their compiler encoding;
// an ambiguous cross-state edge fails closed.
bool repairMixedARMInterworkingCalls(CompiledImage &Compiled,
                                     const BinaryImage &Source,
                                     std::string &Detail) {
  if (Source.Arch != Arch::ARM || Source.Mode != InstructionMode::MixedARMThumb)
    return true;

  std::map<va_t, InstructionMode> OwnerModes;
  for (const auto &Owner : Compiled.SourceFunctionOwners) {
    if (Owner.Kind !=
        llvm::mc_rewrite::RewriteSourceFunctionOwnerKind::FunctionEntry)
      continue;
    if (Compiled.InputKind == RewriteInputKind::ReplacementIR) {
      // Replacement helpers have their own compiler ABI. Their instruction
      // state is independent of the original entry that receives a trampoline.
      const auto Mode = Compiled.ReplacementFunctionModes.find(Owner.OwnerVA);
      if (Mode == Compiled.ReplacementFunctionModes.end() ||
          (Mode->second != InstructionMode::ARM &&
           Mode->second != InstructionMode::Thumb) ||
          !OwnerModes.emplace(Owner.OwnerVA, Mode->second).second) {
        Detail = "replacement ARM function has ambiguous compiler mode";
        return false;
      }
      continue;
    }
    const auto Original =
        Compiled.SourceFunctionOriginalVAs.find(Owner.SourceFunction);
    if (Original == Compiled.SourceFunctionOriginalVAs.end()) {
      Detail = "generated ARM function has no exact source entry";
      return false;
    }
    const auto Mode = Source.instructionModeAt(Original->second);
    if (!Mode || !OwnerModes.emplace(Owner.OwnerVA, *Mode).second) {
      Detail = "generated ARM function has ambiguous instruction mode";
      return false;
    }
  }
  if (OwnerModes.empty()) {
    Detail = "mixed ARM rewrite has no authenticated source functions";
    return false;
  }

  for (const CompiledSection &Section : Compiled.Sections) {
    if (Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code ||
        !Section.IsInImage)
      continue;
    for (const CompiledFixupReference &Fixup : Section.FixupReferences) {
      if (!Fixup.IsPCRel || (Fixup.BitWidth != 24 && Fixup.BitWidth != 32) ||
          Fixup.Offset > Section.Size || Section.Size - Fixup.Offset < 4 ||
          Section.Offset > Compiled.Bytes.size() ||
          Fixup.Offset > Compiled.Bytes.size() - Section.Offset ||
          Compiled.Bytes.size() - Section.Offset - Fixup.Offset < 4)
        continue;
      const va_t Site = Section.VA + Fixup.Offset;
      auto Caller = OwnerModes.upper_bound(Site);
      if (Caller == OwnerModes.begin()) {
        Detail = "generated ARM branch has no authenticated caller mode";
        return false;
      }
      --Caller;
      if (Caller->first < Section.VA) {
        Detail = "generated ARM branch is outside its source function";
        return false;
      }
      const InstructionMode CallerMode = Caller->second;
      if ((CallerMode == InstructionMode::ARM &&
           (Fixup.BitWidth != 24 || (Site & 3u) != 0)) ||
          (CallerMode == InstructionMode::Thumb &&
           (Fixup.BitWidth != 32 || (Site & 1u) != 0)))
        continue;

      const size_t Offset = Section.Offset + Fixup.Offset;
      const uint32_t Insn = readLE<uint32_t>(Compiled.Bytes.data() + Offset);
      std::optional<va_t> ThumbPackedTarget;
      if (CallerMode == InstructionMode::ARM) {
        if (!branch::ArmBranch.matches(Insn))
          continue;
      } else {
        size_t Length = 0;
        ThumbPackedTarget = decodeDirectBranchTarget(
            Arch::ARM, InstructionMode::Thumb, Compiled.Bytes.data() + Offset,
            4, Site, Length);
        if (!ThumbPackedTarget || Length != 4)
          continue;
      }
      if (!Fixup.IsResolved) {
        Detail = "generated ARM branch has no resolved target";
        return false;
      }
      const int64_t ExactDelta =
          Fixup.ResolvedValue <= INT64_MAX
              ? static_cast<int64_t>(Fixup.ResolvedValue)
              : -static_cast<int64_t>(~Fixup.ResolvedValue) - 1;
      if (ExactDelta < -static_cast<int64_t>(Site) ||
          ExactDelta >
              static_cast<int64_t>(UINT32_MAX) - static_cast<int64_t>(Site)) {
        Detail = "generated ARM branch target is outside AArch32";
        return false;
      }
      const int64_t ExactDestination = static_cast<int64_t>(Site) + ExactDelta;
      const va_t Target = static_cast<va_t>(ExactDestination);
      std::optional<InstructionMode> TargetMode;
      if (auto Exact = OwnerModes.find(Target); Exact != OwnerModes.end())
        TargetMode = Exact->second;
      else if (Target >= Section.VA && Target < Section.VA + Section.Size) {
        auto Containing = OwnerModes.upper_bound(Target);
        if (Containing != OwnerModes.begin()) {
          --Containing;
          if (Containing->first >= Section.VA)
            TargetMode = Containing->second;
        }
      } else {
        TargetMode = Source.instructionModeAt(Target);
      }
      if (!TargetMode) {
        Detail = "generated ARM branch target has no authenticated mode";
        return false;
      }
      if (CallerMode == InstructionMode::Thumb) {
        const uint16_t Low = readLE<uint16_t>(Compiled.Bytes.data() + Offset +
                                              branch::ThumbHalfwordBytes);
        const bool IsBL = branch::ThumbLinkLow.matches(Low);
        const bool IsBLX = branch::ThumbLinkExchangeLow.matches(Low);
        if (*TargetMode == InstructionMode::Thumb) {
          if (IsBLX || *ThumbPackedTarget != Target) {
            Detail = "generated Thumb branch does not encode its Thumb target";
            return false;
          }
          continue;
        }
        if (IsBLX && *ThumbPackedTarget == Target)
          continue;
        // A T32 `blx` counts from its program counter rounded down to a word.
        const int64_t Base =
            (static_cast<int64_t>(Site) + branch::ThumbPCOffset) &
            ~int64_t{branch::ArmInstructionBytes - 1};
        const int64_t Displacement = ExactDestination - Base;
        if (!IsBL || *ThumbPackedTarget != Target ||
            Target % branch::ArmInstructionBytes != 0 ||
            !branch::isT32BranchLinkExchangeDisplacement(Displacement)) {
          Detail = "generated Thumb-to-ARM branch needs an interworking veneer";
          return false;
        }
        const auto [BLXHigh, BLXLow] =
            branch::t32BranchLinkExchange(Displacement);
        writeLE<uint16_t>(Compiled.Bytes.data() + Offset, BLXHigh);
        writeLE<uint16_t>(Compiled.Bytes.data() + Offset +
                              branch::ThumbHalfwordBytes,
                          BLXLow);
        continue;
      }
      const int64_t PackedDisplacement =
          llvm::SignExtend64(branch::ArmBranchOffset.extract(Insn),
                             branch::ArmBranchOffset.Width) *
          branch::ArmInstructionBytes;
      // A32 `blx` is the unconditional form; its halfword bit adds half a
      // word.
      const bool IsBLX = branch::ArmCondition.extract(Insn) ==
                         branch::ArmConditionUnconditional;
      const int64_t PackedDestination =
          static_cast<int64_t>(Site) + branch::ArmPCOffset +
          PackedDisplacement +
          (IsBLX ? int64_t{branch::ArmLinkOrHalfword.extract(Insn)} *
                       branch::ThumbHalfwordBytes
                 : 0);
      if (PackedDestination < 0 || PackedDestination > UINT32_MAX) {
        Detail = "generated ARM branch target is outside AArch32";
        return false;
      }
      if (*TargetMode != InstructionMode::Thumb) {
        if (IsBLX || PackedDestination != ExactDestination) {
          Detail = "generated ARM branch does not encode its ARM target";
          return false;
        }
        continue;
      }
      if (IsBLX && PackedDestination == ExactDestination)
        continue;
      const int64_t Displacement =
          ExactDestination - (static_cast<int64_t>(Site) + 8);
      // Only an unconditional `bl` can become a `blx`.
      const bool IsBL =
          branch::ArmCondition.extract(Insn) == branch::ArmConditionAlways &&
          branch::ArmLinkOrHalfword.extract(Insn);
      if (!IsBL || (Target & branch::ThumbStateBit) != 0 ||
          Displacement % branch::ThumbHalfwordBytes != 0 ||
          PackedDestination !=
              (ExactDestination & ~int64_t{branch::ArmInstructionBytes - 1})) {
        Detail = "generated ARM-to-Thumb branch needs an interworking veneer";
        return false;
      }
      writeLE<uint32_t>(Compiled.Bytes.data() + Offset,
                        branch::a32BranchLinkExchange(Displacement));
    }
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Factory methods (cf. llvm::object::ObjectFile::createObjectFile)
//===----------------------------------------------------------------------===//

std::unique_ptr<BinaryPatcher> BinaryPatcher::create(BinaryFormat Format) {
  switch (Format) {
  case BinaryFormat::COFF:
    return std::make_unique<COFFPatcher>();
  case BinaryFormat::ELF:
    return std::make_unique<ELFPatcher>();
  case BinaryFormat::MachO:
    return std::make_unique<MachOPatcher>();
  default:
    return nullptr;
  }
}

std::unique_ptr<InplaceRewriter> InplaceRewriter::create(BinaryFormat Format) {
  switch (Format) {
  case BinaryFormat::COFF:
    return std::make_unique<COFFInplaceRewriter>();
  case BinaryFormat::ELF:
    return std::make_unique<ELFInplaceRewriter>();
  case BinaryFormat::MachO:
    return std::make_unique<MachOInplaceRewriter>();
  default:
    return nullptr;
  }
}

std::vector<Export>
BinaryPatcher::authenticatedFunctionExports(const BinaryImage *Image) {
  std::vector<Export> Result;
  if (!Image)
    return Result;
  for (const Export &Exp : Image->Exports)
    if (Image->hasAuthenticatedFunctionEntryAt(Exp.Addr))
      Result.push_back(Exp);
  return Result;
}

bool BinaryPatcher::validateSourceFunctionPatchPlan(
    const CompiledImage &Compiled, const BinaryImage *Image,
    std::string &Detail) {
  Detail.clear();
  if (Compiled.SourceFunctionOriginalVAs.empty())
    return true;
  if (!Image) {
    Detail = "an exact loader image is required for source identities";
    return false;
  }
  if (!llvm::mc_rewrite::validateRewriteSourceFunctionOwners(
          Compiled.SourceFunctionOwners)) {
    Detail = "compiler source-owner provenance is invalid";
    return false;
  }

  std::set<va_t> SeenEntries;
  for (const auto &[SourceFunction, OriginalVA] :
       Compiled.SourceFunctionOriginalVAs) {
    const auto Mode = Image->instructionModeAt(OriginalVA);
    if (!Mode ||
        getTargetCodegenInfo(Image->Arch, *Mode).trampolineSize() == 0) {
      Detail = "an original entry has no supported instruction-mode trampoline";
      return false;
    }
    const va_t Normalized =
        normalizeCodeAddress(OriginalVA, Image->Arch, *Mode);
    if (Normalized != OriginalVA) {
      Detail = "an original entry uses a non-canonical code address";
      return false;
    }
    const size_t OwnerCount =
        llvm::count_if(Compiled.SourceFunctionOwners, [&](const auto &Owner) {
          return Owner.SourceFunction == SourceFunction;
        });
    if (OwnerCount != 1) {
      Detail = "an original entry does not have exactly one compiler owner";
      return false;
    }
    if (!SeenEntries.insert(Normalized).second) {
      Detail = "two source functions share an original entry";
      return false;
    }
    if (!Image->hasAuthenticatedFunctionEntryAt(OriginalVA)) {
      Detail = "an original entry is not loader-authenticated code";
      return false;
    }
  }
  return true;
}

bool BinaryPatcher::validatePatchedSourceTrampolineClosure(
    const SourceTrampolinePlan &Plan,
    llvm::ArrayRef<PatchedFunctionEntry> PatchedFunctions,
    size_t TrampolineCount, std::string &Detail) {
  Detail.clear();
  if (TrampolineCount != PatchedFunctions.size() ||
      PatchedFunctions.size() != Plan.OriginalVAs.size()) {
    Detail = "installed trampolines do not cover every replaceable original "
             "entry";
    return false;
  }

  for (const auto &[SourceFunction, OriginalVA] : Plan.OriginalVAs) {
    const auto Owner = llvm::find_if(Plan.Owners, [&](const auto &Candidate) {
      return Candidate.SourceFunction == SourceFunction;
    });
    if (Owner == Plan.Owners.end()) {
      Detail = "an installed source has no compiler owner";
      return false;
    }
    const size_t Matches = llvm::count_if(
        PatchedFunctions, [&](const PatchedFunctionEntry &Patched) {
          return Patched.SourceFunction == SourceFunction &&
                 Patched.OriginalVA == OriginalVA &&
                 Patched.OwnerSymbol == Owner->OwnerSymbol &&
                 Patched.OwnerVA == Owner->OwnerVA;
        });
    if (Matches != 1) {
      Detail = "an original entry has no exact trampoline receipt";
      return false;
    }
  }
  return true;
}

//===----------------------------------------------------------------------===//
// findObjectTextSection — helper for InplaceRewriter subclasses
//===----------------------------------------------------------------------===//

bool findObjectTextSection(const std::vector<uint8_t> &Binary,
                           llvm::StringRef SectionName, TextLayout &TL) {
  if (Binary.size() < 4)
    return false;

  const uint8_t *Data = Binary.data();
  size_t Size = Binary.size();

  auto Format = llvm::identify_magic(
      llvm::StringRef(reinterpret_cast<const char *>(Data), Size));

  switch (Format) {
  case llvm::file_magic::pecoff_executable: {
    auto PE = locatePEHeaders(const_cast<uint8_t *>(Data), Size);
    if (!PE.valid())
      return false;
    PESectionFields Sec;
    if (!findPESection(PE, SectionName, Sec))
      return false;
    TL.SectionFileoff = Sec.PointerToRawData;
    TL.SectionVA = getPEImageBase(PE) + Sec.VirtualAddress;
    TL.SectionSize = Sec.VirtualSize;
    return true;
  }
  case llvm::file_magic::elf:
  case llvm::file_magic::elf_relocatable:
  case llvm::file_magic::elf_executable:
  case llvm::file_magic::elf_shared_object:
  case llvm::file_magic::elf_core: {
    ELFShdrFields Shdr;
    if (!findELFSection(Data, Size, SectionName, Shdr))
      return false;
    TL.SectionFileoff = Shdr.Offset;
    TL.SectionVA = Shdr.Addr;
    TL.SectionSize = Shdr.Size;
    return true;
  }
  case llvm::file_magic::macho_object:
  case llvm::file_magic::macho_executable:
  case llvm::file_magic::macho_dynamically_linked_shared_lib:
  case llvm::file_magic::macho_bundle: {
    uint64_t VA = 0, SecSize = 0;
    uint32_t FileOff = 0;
    if (!findMachOSection(Binary, SectionName, VA, SecSize, FileOff))
      return false;
    TL.SectionVA = VA;
    TL.SectionSize = SecSize;
    TL.SectionFileoff = FileOff;
    return true;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// BinaryPatcher common methods
//===----------------------------------------------------------------------===//

bool BinaryPatcher::writeTrampoline(std::vector<uint8_t> &Data,
                                    uint64_t FromOff, uint64_t TargetVA,
                                    uint64_t FromVA, Arch TargetArch,
                                    InstructionMode Mode,
                                    uint64_t MaxOverwriteBytes) {
  return getTargetCodegenInfo(TargetArch, Mode)
      .writeTrampoline(Data, FromOff, TargetVA, FromVA, MaxOverwriteBytes);
}

void padWithNops(uint8_t *Dst, uint64_t Len, Arch TargetArch,
                 InstructionMode Mode) {
  getTargetCodegenInfo(TargetArch, Mode).fillPadding(Dst, Len);
}

PatchResult BinaryPatcher::readPatchWrite(
    const std::filesystem::path &InputPath,
    const std::filesystem::path &OutputPath, bool SetExecPerm,
    llvm::StringRef DebugTag,
    llvm::unique_function<bool(std::vector<uint8_t> &, PatchResult &)>
        PatchFn) {
  PatchResult Result;

  auto BufOrErr = llvm::MemoryBuffer::getFile(InputPath.string());
  if (!BufOrErr) {
    llvm::WithColor::error()
        << DebugTag << ": cannot open " << InputPath.string() << "\n";
    return Result;
  }
  auto &Buf = *BufOrErr;
  std::vector<uint8_t> Binary(Buf->getBufferStart(), Buf->getBufferEnd());

  if (!PatchFn(Binary, Result))
    return Result;

  unsigned Flags = SetExecPerm ? llvm::FileOutputBuffer::F_executable : 0;
  auto OutOrErr =
      llvm::FileOutputBuffer::create(OutputPath.string(), Binary.size(), Flags);
  if (!OutOrErr) {
    llvm::WithColor::error()
        << DebugTag << ": cannot write " << OutputPath.string() << "\n";
    llvm::consumeError(OutOrErr.takeError());
    Result.Success = false;
    return Result;
  }
  std::memcpy((*OutOrErr)->getBufferStart(), Binary.data(), Binary.size());
  if (auto Err = (*OutOrErr)->commit()) {
    llvm::consumeError(std::move(Err));
    Result.Success = false;
    return Result;
  }

  Result.Success = true;
  Result.OutputPath = OutputPath.string();
  LLVM_DEBUG(llvm::dbgs() << DebugTag << ": written " << Binary.size()
                          << " bytes to " << OutputPath.string() << "\n");
  return Result;
}

static uint64_t
provenFunctionSpan(uint64_t OrigVA, uint64_t TextStartVA, uint64_t TextEndVA,
                   const std::vector<Symbol> *Symbols,
                   const std::vector<std::pair<va_t, va_t>> *KnownRanges,
                   const std::vector<Export> *Exports) {
  uint64_t Best = 0;
  if (Symbols) {
    for (const auto &S : *Symbols)
      if (S.IsFunc && S.Addr == OrigVA && S.Size != 0)
        Best = Best == 0 ? S.Size : std::min<uint64_t>(Best, S.Size);
  }
  if (KnownRanges) {
    for (const auto &[Begin, End] : *KnownRanges)
      if (Begin == OrigVA && End > Begin)
        Best = Best == 0 ? End - Begin : std::min<uint64_t>(Best, End - Begin);
  }
  if (Best == 0 && (Symbols || Exports)) {
    uint64_t Next = TextEndVA;
    bool FoundCurrent = false;
    bool FoundNext = false;
    if (Symbols) {
      for (const auto &S : *Symbols) {
        if (!S.IsFunc || S.Addr < TextStartVA || S.Addr >= TextEndVA)
          continue;
        FoundCurrent |= S.Addr == OrigVA;
        if (S.Addr > OrigVA) {
          Next = std::min<uint64_t>(Next, S.Addr);
          FoundNext = true;
        }
      }
    }
    if (Exports) {
      for (const auto &E : *Exports) {
        if (E.Addr < TextStartVA || E.Addr >= TextEndVA)
          continue;
        FoundCurrent |= E.Addr == OrigVA;
        if (E.Addr > OrigVA) {
          Next = std::min<uint64_t>(Next, E.Addr);
          FoundNext = true;
        }
      }
    }
    if (FoundCurrent && FoundNext && Next > OrigVA)
      Best = Next - OrigVA;
  }
  return OrigVA >= TextStartVA && OrigVA < TextEndVA
             ? std::min<uint64_t>(Best, TextEndVA - OrigVA)
             : 0;
}

static uint64_t
provenNextEntrySpan(uint64_t OrigVA, uint64_t TextStartVA, uint64_t TextEndVA,
                    const std::vector<Symbol> *Symbols,
                    const std::vector<std::pair<va_t, va_t>> *KnownRanges,
                    const std::vector<Export> *Exports) {
  if (OrigVA < TextStartVA || OrigVA >= TextEndVA)
    return 0;
  uint64_t Next = TextEndVA;
  bool Found = false;
  auto Consider = [&](uint64_t Candidate) {
    if (Candidate > OrigVA && Candidate < Next && Candidate < TextEndVA) {
      Next = Candidate;
      Found = true;
    }
  };
  if (Symbols)
    for (const Symbol &S : *Symbols)
      if (S.IsFunc && S.Addr >= TextStartVA)
        Consider(S.Addr);
  if (KnownRanges)
    for (const auto &[Begin, End] : *KnownRanges) {
      (void)End;
      if (Begin >= TextStartVA)
        Consider(Begin);
    }
  if (Exports)
    for (const Export &E : *Exports)
      if (E.Addr >= TextStartVA)
        Consider(E.Addr);
  return Found ? Next - OrigVA : 0;
}

bool sourceEntryCanHostTrampoline(const BinaryImage &Image, va_t OriginalVA) {
  const auto Mode = Image.instructionModeAt(OriginalVA);
  if (!Mode)
    return false;
  const uint64_t TrampolineSize =
      getTargetCodegenInfo(Image.Arch, *Mode).trampolineSize();
  if (TrampolineSize == 0 || OriginalVA == InvalidVA)
    return false;
  const va_t Normalized = normalizeCodeAddress(OriginalVA, Image.Arch, *Mode);
  if (Normalized != OriginalVA ||
      !Image.hasExecutableCodeOwnerRange(Normalized, TrampolineSize) ||
      (Image.Arch == Arch::ARM &&
       Image.instructionModeAt(Normalized + TrampolineSize - 1, *Mode) != Mode))
    return false;

  for (uint64_t Offset = 1; Offset < TrampolineSize; ++Offset) {
    if (OriginalVA > InvalidVA - Offset)
      return false;
    const va_t Candidate =
        normalizeCodeAddress(OriginalVA + Offset, Image.Arch, Image.Mode);
    if (Candidate != Normalized &&
        Image.hasAuthenticatedFunctionEntryAt(Candidate))
      return false;
  }

  if (Image.Arch != Arch::ARM || *Mode != InstructionMode::Thumb)
    return true;
  const Segment *Seg = Image.getSegmentFor(Normalized);
  const uint64_t End =
      Seg && Seg->Size <= InvalidVA - Seg->VA ? Seg->VA + Seg->Size : 0;
  if (!Seg || End <= Seg->VA)
    return false;
  return provenFunctionSpan(Normalized, Seg->VA, End, &Image.Symbols,
                            &Image.KnownCodeRanges,
                            &Image.Exports) >= TrampolineSize;
}

llvm::StringRef blockAddressEscape(
    const llvm::Function &Body,
    llvm::function_ref<bool(const llvm::Function *)> RemovesBody) {
#define BLOCK_ADDRESS_ESCAPE(Name, Value)                                      \
  constexpr llvm::StringLiteral Name = Value;
#include "BlockAddressUse.def"
  for (const llvm::BasicBlock &Block : Body) {
    llvm::BlockAddress *Address = llvm::BlockAddress::lookup(&Block);
    if (!Address)
      continue;
    if (Address->isUsedByMetadata())
      return kMetadataEscape;

    llvm::SmallVector<const llvm::User *, 8> Worklist;
    llvm::SmallPtrSet<const llvm::User *, 16> SeenUsers;
    llvm::append_range(Worklist, Address->users());
    while (!Worklist.empty()) {
      const llvm::User *User = Worklist.pop_back_val();
      if (!SeenUsers.insert(User).second)
        continue;
      if (User->isUsedByMetadata())
        return kMetadataEscape;

      if (const auto *Instruction = llvm::dyn_cast<llvm::Instruction>(User)) {
        if (!RemovesBody(Instruction->getFunction()))
          return kValueEscape;

        continue;
      }
      if (llvm::isa<llvm::GlobalValue>(User))
        return kValueEscape;

      if (llvm::isa<llvm::Constant>(User)) {
        llvm::append_range(Worklist, User->users());
        continue;
      }
      return kValueEscape;
    }
  }
  return {};
}

bool BinaryPatcher::prepareSourceFunctionsForPatch(
    llvm::Module &Module, const BinaryImage *Image,
    SourceFunctionPreparation &Preparation, std::string &Detail,
    RewriteInputKind InputKind) {
  Preparation = SourceFunctionPreparation{};
  Detail.clear();

  // COFF source preparation can turn a preserved lifted definition into a
  // declaration.  Authenticate the complete attachment/table/source-image
  // closure while every body and every named-table row is still intact; a
  // later planner must never be asked to infer whether a declaration used to
  // be a valid source definition.
  const bool LiftedCOFF = Image && Image->Format == BinaryFormat::COFF &&
                          InputKind == RewriteInputKind::LiftedIR;
  if (LiftedCOFF) {
    if (llvm::Error Error =
            validateCOFFExceptionSourceIdentityClosure(Module, *Image)) {
      Detail = llvm::toString(std::move(Error));
      return false;
    }
  }

  std::set<va_t> SeenEntries;
  struct SourceCandidate {
    llvm::Function *Function = nullptr;
    bool Replaceable = false;
  };
  std::vector<SourceCandidate> Candidates;
  for (llvm::Function &Function : Module) {
    if (Function.isDeclaration())
      continue;
    auto Address = rewrite_source::getOriginalVA(Function);
    if (!Address) {
      Detail = llvm::toString(Address.takeError());
      return false;
    }
    if (!*Address)
      continue;
    Preparation.HasExactSources = true;
    const va_t OriginalVA = **Address;
    if (!Image) {
      Detail = "an exact loader image is required for source identities";
      return false;
    }
    const auto Mode = Image->instructionModeAt(OriginalVA);
    if (!Mode) {
      Detail = "an original entry has no authenticated instruction mode";
      return false;
    }
    const va_t Normalized =
        normalizeCodeAddress(OriginalVA, Image->Arch, *Mode);
    if (Normalized != OriginalVA) {
      Detail = "an original entry uses a non-canonical code address";
      return false;
    }
    if (!SeenEntries.insert(Normalized).second) {
      Detail = "two source functions share an original entry";
      return false;
    }
    if (!Image->hasAuthenticatedFunctionEntryAt(Normalized)) {
      Detail = "an original entry is not loader-authenticated code";
      return false;
    }
    if (Image->Arch == Arch::ARM &&
        (Image->Mode == InstructionMode::MixedARMThumb ||
         InputKind == RewriteInputKind::ReplacementIR)) {
      // A tail B/B.W cannot change instruction state without a veneer. Keep
      // direct calls as calls so final-image interworking can use BLX.
      Function.addFnAttr("disable-tail-calls", "true");
      const llvm::Attribute Existing =
          Function.getFnAttribute("target-features");
      std::string Features = Existing.isValid()
                                 ? Existing.getValueAsString().str()
                                 : std::string();
      const llvm::StringRef Wanted =
          *Mode == InstructionMode::Thumb ? "+thumb-mode" : "-thumb-mode";
      const llvm::StringRef Conflict =
          *Mode == InstructionMode::Thumb ? "-thumb-mode" : "+thumb-mode";
      if (InputKind == RewriteInputKind::ReplacementIR) {
        const std::string OriginalFeatures = Features;
        llvm::SmallVector<llvm::StringRef, 16> Parts;
        llvm::StringRef(OriginalFeatures).split(Parts, ',');
        Features.clear();
        for (auto Part : Parts)
          if (!Part.empty() && Part != "+thumb-mode" && Part != "-thumb-mode") {
            if (!Features.empty())
              Features += ',';
            Features += Part.str();
          }
      } else if (llvm::StringRef(Features).contains(Conflict)) {
        Detail = "source target features contradict authenticated ARM mode";
        return false;
      }
      if (!llvm::StringRef(Features).contains(Wanted)) {
        if (!Features.empty())
          Features += ',';
        Features += Wanted.str();
        Function.addFnAttr("target-features", Features);
      }
    }
    const bool Replaceable = sourceEntryCanHostTrampoline(*Image, Normalized);
    auto &OriginalVAs = Replaceable ? Preparation.ReplaceableOriginalVAs
                                    : Preparation.PreservedOriginalVAs;
    OriginalVAs.emplace(Function.getName().str(), Normalized);
    Candidates.push_back({&Function, Replaceable});
  }

  std::vector<va_t> ReplaceableEntries;
  ReplaceableEntries.reserve(Preparation.ReplaceableOriginalVAs.size());
  for (const auto &[FunctionName, OriginalVA] :
       Preparation.ReplaceableOriginalVAs) {
    (void)FunctionName;
    ReplaceableEntries.push_back(OriginalVA);
  }
  if (LiftedCOFF) {
    if (llvm::Error Error = validateCOFFCxxGroupReplacementClosure(
            Module, *Image, ReplaceableEntries)) {
      Detail = llvm::toString(std::move(Error));
      return false;
    }
  }

  // Nothing from an exact source plan needs recompilation. Keep the clone
  // intact and let the format patcher commit a true no-op, irrespective of
  // unrelated helper definitions or blockaddress constants in the module.
  if (Preparation.isExactNoOp())
    return true;

  llvm::SmallPtrSet<const llvm::Function *, 8> FunctionsToExternalize;
  for (const SourceCandidate &Candidate : Candidates)
    if (!Candidate.Replaceable)
      FunctionsToExternalize.insert(Candidate.Function);

  // Deleting a body destroys its BasicBlocks. LLVM intentionally rewrites a
  // still-live blockaddress for such a block to inttoptr(1), which would turn
  // a cross-function label reference into a silent stale identity. Validate
  // the complete preservation set before mutating any source body and reject
  // the transaction when a block address escapes that body. There is no
  // authoritative BasicBlock-to-original-VA map here, so guessing a numeric
  // replacement would be less safe than failing closed.
  for (const SourceCandidate &Candidate : Candidates) {
    if (Candidate.Replaceable)
      continue;
    const auto Escape =
        blockAddressEscape(*Candidate.Function, [&](const auto *F) {
          return FunctionsToExternalize.contains(F);
        });
    if (!Escape.empty()) {
      Detail = Escape.str();
      return false;
    }
  }

  if (LiftedCOFF) {
    if (llvm::Error Error = retainCOFFCxxGroupRewriteContractsForReplacement(
            Module, *Image, ReplaceableEntries)) {
      Detail = llvm::toString(std::move(Error));
      return false;
    }
  }

  // A preserved source is resolved to its authenticated original body and is
  // not compiled.  Remove its Windows EH row and attachment together before
  // externalizing it, so the post-preparation module remains an exact closure
  // over only the definitions whose unwind records will actually be generated.
  // The preauthentication above makes every row shape and pointer identity
  // trustworthy; all fallible validation, including blockaddress escape
  // checks, has completed before this first mutation.
  llvm::NamedMDNode *WindowsEHTable =
      Image && Image->Format == BinaryFormat::COFF
          ? Module.getNamedMetadata(windows_eh_md::FunctionTable)
          : nullptr;
  std::vector<llvm::MDNode *> RetainedWindowsEHRows;
  bool RemovesWindowsEHRow = false;
  if (WindowsEHTable) {
    RetainedWindowsEHRows.reserve(WindowsEHTable->getNumOperands());
    for (llvm::MDNode *Row : WindowsEHTable->operands()) {
      const auto *FunctionValue =
          llvm::dyn_cast<llvm::ValueAsMetadata>(Row->getOperand(0).get());
      const auto *Function =
          llvm::cast<llvm::Function>(FunctionValue->getValue());
      if (FunctionsToExternalize.contains(Function)) {
        RemovesWindowsEHRow = true;
        continue;
      }
      RetainedWindowsEHRows.push_back(Row);
    }
  }
  if (RemovesWindowsEHRow) {
    WindowsEHTable->clearOperands();
    for (llvm::MDNode *Row : RetainedWindowsEHRows)
      WindowsEHTable->addOperand(Row);
    if (RetainedWindowsEHRows.empty())
      WindowsEHTable->eraseFromParent();
  }

  for (const SourceCandidate &Candidate : Candidates) {
    if (Candidate.Replaceable)
      continue;
    if (Image && Image->Format == BinaryFormat::COFF) {
      Candidate.Function->setMetadata(windows_eh_md::FunctionAttachment,
                                      nullptr);
      Candidate.Function->setMetadata(windows_eh_md::NativeAttachment, nullptr);
    }
    Candidate.Function->deleteBody();
    Candidate.Function->setLinkage(llvm::GlobalValue::ExternalLinkage);
    Candidate.Function->setDSOLocal(true);
  }
  return true;
}

SourceTrampolinePlan
BinaryPatcher::makeSourceTrampolinePlan(const CompiledImage &Compiled,
                                        const BinaryImage *Image) {
  SourceTrampolinePlan Plan;
  if (!Image)
    return Plan;
  for (const auto &[SourceFunction, OriginalVA] :
       Compiled.SourceFunctionOriginalVAs) {
    const bool Replaceable = sourceEntryCanHostTrampoline(*Image, OriginalVA);

    if (!Replaceable) {
      ++Plan.PreservedCount;
      continue;
    }
    Plan.OriginalVAs.emplace(SourceFunction, OriginalVA);
    for (const auto &Owner : Compiled.SourceFunctionOwners)
      if (Owner.SourceFunction == SourceFunction)
        Plan.Owners.push_back(Owner);
  }
  return Plan;
}

size_t BinaryPatcher::installTrampolines(
    std::vector<uint8_t> &Binary,
    const std::map<std::string, uint64_t> &SymbolAddrs, uint64_t OrigTextVA,
    uint64_t OrigTextSize, uint64_t OrigTextFileOff, uint64_t ImageBase,
    Arch TargetArch, InstructionMode Mode, const std::vector<Symbol> *Symbols,
    const std::vector<std::pair<va_t, va_t>> *KnownRanges,
    const std::vector<Export> *Exports,
    std::vector<va_t> *PatchedOriginalEntries,
    std::vector<std::pair<va_t, va_t>> *PatchedEntryMappings,
    std::vector<PatchedFunctionEntry> *PatchedFunctions,
    const BinaryImage *SourceImage) {
  return installTrampolines(
      Binary, SymbolAddrs, OrigTextVA, OrigTextSize, OrigTextFileOff, ImageBase,
      TargetArch, Mode, Symbols, KnownRanges, Exports, PatchedOriginalEntries,
      PatchedEntryMappings, PatchedFunctions, {}, {}, SourceImage);
}

size_t BinaryPatcher::installTrampolines(
    std::vector<uint8_t> &Binary,
    const std::map<std::string, uint64_t> &SymbolAddrs, uint64_t OrigTextVA,
    uint64_t OrigTextSize, uint64_t OrigTextFileOff, uint64_t ImageBase,
    Arch TargetArch, InstructionMode Mode, const std::vector<Symbol> *Symbols,
    const std::vector<std::pair<va_t, va_t>> *KnownRanges,
    const std::vector<Export> *Exports,
    std::vector<va_t> *PatchedOriginalEntries,
    std::vector<std::pair<va_t, va_t>> *PatchedEntryMappings,
    std::vector<PatchedFunctionEntry> *PatchedFunctions,
    llvm::ArrayRef<llvm::mc_rewrite::RewriteSourceFunctionOwner>
        SourceFunctionOwners,
    const std::map<std::string, uint64_t> &SourceFunctionOriginalVAs,
    const BinaryImage *SourceImage,
    const std::map<uint64_t, InstructionMode> *ReplacementModes) {
  if (OrigTextSize > InvalidVA - OrigTextVA ||
      OrigTextVA > InvalidVA - ImageBase)
    return 0;
  uint64_t TextStartVA = ImageBase + OrigTextVA;
  if (OrigTextSize > InvalidVA - TextStartVA)
    return 0;
  uint64_t TextEndVA = TextStartVA + OrigTextSize;

  std::map<std::string, uint64_t> ExpMap;
  if (Exports)
    for (const auto &Exp : *Exports)
      if (Exp.Addr != 0)
        ExpMap[Exp.Name] = Exp.Addr;

  if (!SourceFunctionOwners.empty() &&
      !llvm::mc_rewrite::validateRewriteSourceFunctionOwners(
          SourceFunctionOwners))
    return 0;
  for (const auto &[SourceFunction, OriginalVA] : SourceFunctionOriginalVAs) {
    if (OriginalVA == InvalidVA ||
        llvm::count_if(SourceFunctionOwners, [&](const auto &Owner) {
          return Owner.SourceFunction == SourceFunction;
        }) != 1)
      return 0;
  }

  size_t Count = 0;
  auto InstallOne = [&](llvm::StringRef SourceFunction,
                        llvm::StringRef OwnerSymbol, uint64_t FuncNewVA,
                        std::optional<uint64_t> AuthenticatedOriginalVA) {
    uint64_t OrigVA = 0;
    bool HasOrig = AuthenticatedOriginalVA.has_value();
    if (HasOrig)
      OrigVA = *AuthenticatedOriginalVA;

    if (!HasOrig) {
      llvm::StringRef NameRef(SourceFunction);
      // Legacy object paths without source-owner provenance still recover
      // automatically named entries.  Authenticated rewrite output never
      // enters this spelling-based compatibility path.
      llvm::StringRef AutoName = NameRef;
      if (AutoName.starts_with("_") &&
          AutoName.drop_front().starts_with(kAutoFuncPrefix))
        AutoName = AutoName.drop_front();
      if (AutoName.starts_with(kAutoFuncPrefix)) {
        llvm::StringRef HexPart = AutoName.drop_front(kAutoFuncPrefix.size());
        if (!HexPart.empty() && !HexPart.getAsInteger(16, OrigVA))
          HasOrig = true;
      }
    }

    if (!HasOrig && SourceFunctionOwners.empty() && !ExpMap.empty()) {
      std::string Key = resolveSymbolAlias(SourceFunction.str(), ExpMap);
      if (!Key.empty()) {
        OrigVA = ExpMap[Key];
        HasOrig = true;
      }
    }

    if (!HasOrig)
      return;

    uint64_t OrigRVA = OrigVA >= ImageBase ? OrigVA - ImageBase : OrigVA;
    if (OrigRVA < OrigTextVA || OrigRVA >= OrigTextVA + OrigTextSize)
      return;

    uint64_t TextDelta = OrigRVA - OrigTextVA;
    if (OrigTextFileOff > InvalidVA - TextDelta)
      return;
    uint64_t OrigOff = OrigTextFileOff + TextDelta;
    uint64_t OrigAnalysisVA = ImageBase + OrigRVA;
    const auto EntryMode = SourceImage
                               ? SourceImage->instructionModeAt(OrigAnalysisVA)
                               : std::optional<InstructionMode>(Mode);
    if (!EntryMode || !isSingleInstructionMode(*EntryMode))
      return;
    uint64_t MaxOverwriteBytes = TextEndVA - OrigAnalysisVA;
    // A function-body extent and a safe overwrite boundary are different
    // concepts.  Every target must stop at the next authenticated entry, while
    // a fixed-size trampoline may otherwise consume ordinary alignment bytes
    // after a short body.  Thumb retains the stricter body-span requirement
    // because its variable-width stream cannot safely infer such padding.
    const uint64_t NextEntrySpan = provenNextEntrySpan(
        OrigAnalysisVA, TextStartVA, TextEndVA, Symbols, KnownRanges, Exports);
    if (NextEntrySpan != 0)
      MaxOverwriteBytes = std::min(MaxOverwriteBytes, NextEntrySpan);
    const bool IsThumb =
        TargetArch == Arch::ARM && *EntryMode == InstructionMode::Thumb;
    if (IsThumb && SourceFunctionOwners.empty()) {
      const uint64_t ProvenSpan =
          provenFunctionSpan(OrigAnalysisVA, TextStartVA, TextEndVA, Symbols,
                             KnownRanges, Exports);
      if (ProvenSpan != 0)
        MaxOverwriteBytes = std::min(MaxOverwriteBytes, ProvenSpan);
      else if (IsThumb)
        MaxOverwriteBytes = 0;
    }
    for (const auto &[OtherSource, OtherVA] : SourceFunctionOriginalVAs) {
      (void)OtherSource;
      if (OtherVA > OrigAnalysisVA)
        MaxOverwriteBytes =
            std::min<uint64_t>(MaxOverwriteBytes, OtherVA - OrigAnalysisVA);
    }
    bool Written = false;
    if (ReplacementModes && TargetArch == Arch::ARM) {
      const auto Generated = ReplacementModes->find(FuncNewVA);
      if (Generated == ReplacementModes->end() ||
          Generated->second != *EntryMode)
        return;
      Written = getTargetCodegenInfo(TargetArch, *EntryMode)
                    .writeSourceTrampoline(Binary, OrigOff, FuncNewVA,
                                           OrigAnalysisVA, MaxOverwriteBytes);
    } else {
      Written = writeTrampoline(Binary, OrigOff, FuncNewVA, OrigAnalysisVA,
                                TargetArch, *EntryMode, MaxOverwriteBytes);
    }
    if (Written) {
      ++Count;
      if (PatchedOriginalEntries)
        PatchedOriginalEntries->push_back(OrigAnalysisVA);
      if (PatchedEntryMappings)
        PatchedEntryMappings->push_back({OrigAnalysisVA, FuncNewVA});
      if (PatchedFunctions)
        PatchedFunctions->push_back({OwnerSymbol.str(), OrigAnalysisVA,
                                     FuncNewVA, SourceFunction.str()});
      LLVM_DEBUG(llvm::dbgs()
                 << "patch: trampoline " << SourceFunction << " ("
                 << OwnerSymbol << ") @ VA 0x" << llvm::utohexstr(OrigRVA)
                 << " -> 0x" << llvm::utohexstr(FuncNewVA) << "\n");
    } else {
      LLVM_DEBUG(llvm::dbgs() << "patch: skipped unsafe/unreachable trampoline "
                              << SourceFunction << " @ VA 0x"
                              << llvm::utohexstr(OrigAnalysisVA) << "\n");
    }
  };

  if (!SourceFunctionOwners.empty()) {
    for (const llvm::mc_rewrite::RewriteSourceFunctionOwner &Owner :
         SourceFunctionOwners) {
      const auto Original =
          SourceFunctionOriginalVAs.find(Owner.SourceFunction);
      if (Original == SourceFunctionOriginalVAs.end())
        continue;
      InstallOne(Owner.SourceFunction, Owner.OwnerSymbol, Owner.OwnerVA,
                 Original->second);
    }
  } else {
    for (const auto &[Name, FuncNewVA] : SymbolAddrs)
      InstallOne(Name, Name, FuncNewVA, std::nullopt);
  }
  return Count;
}

} // namespace neverd
