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

#include "neverd/ArchSupport.h"
#include "neverd/Object/MachOLayout.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

#define DEBUG_TYPE "neverd-macho-patch"

namespace neverd {

using namespace llvm::MachO;

namespace {

struct EHFrameAppendRegion {
  bool Is64 = true;
  uint64_t SectionVA = 0;
  uint64_t SectionFileOff = 0;
  uint64_t AppendVA = 0;
  uint64_t AppendFileOff = 0;
  uint64_t LimitFileOff = 0;
  uint64_t SectionHeaderOff = 0;
};

/// Return the byte offset at which another .eh_frame sequence can be appended.
/// A zero-length record is a terminator, so it is replaced rather than left
/// between the original and regenerated records.
std::optional<uint64_t> getEHFrameAppendOffset(llvm::ArrayRef<uint8_t> Bytes) {
  uint64_t Off = 0;
  while (Off < Bytes.size()) {
    if (!rangeInBounds(Off, sizeof(uint32_t), Bytes.size()))
      return std::nullopt;
    uint32_t Length = readLE<uint32_t>(Bytes.data() + Off);
    if (Length == 0)
      return Off;

    uint64_t RecordSize = 0;
    if (Length == std::numeric_limits<uint32_t>::max()) {
      if (!rangeInBounds(Off, sizeof(uint32_t) + sizeof(uint64_t),
                         Bytes.size()))
        return std::nullopt;
      uint64_t ExtendedLength =
          readLE<uint64_t>(Bytes.data() + Off + sizeof(uint32_t));
      if (ExtendedLength >
          std::numeric_limits<uint64_t>::max() -
              (sizeof(uint32_t) + sizeof(uint64_t)))
        return std::nullopt;
      RecordSize = sizeof(uint32_t) + sizeof(uint64_t) + ExtendedLength;
    } else {
      RecordSize = sizeof(uint32_t) + static_cast<uint64_t>(Length);
    }
    if (RecordSize == 0 || !rangeInBounds(Off, RecordSize, Bytes.size()))
      return std::nullopt;
    Off += RecordSize;
  }
  return Off;
}

bool findEHFrameAppendRegion(const std::vector<uint8_t> &Binary,
                             EHFrameAppendRegion &Out) {
  bool Found = false;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t CmdSize, bool Is64) {
        if (Found || Cmd != getMachOSegmentCmdID(Is64))
          return;
        MachOSegFields Seg = readMachOSegment(LCPtr, Is64);
        if (readMachOName(Seg.SegName) != section_names::macho::TextSeg ||
            Seg.FileOff > std::numeric_limits<uint64_t>::max() - Seg.FileSize)
          return;

        const uint32_t BaseSize = getMachOSegmentCmdSize(Is64);
        const uint32_t SectionSize = getMachOSectionSize(Is64);
        const uint32_t Nsects =
            Is64 ? reinterpret_cast<const segment_command_64 *>(LCPtr)->nsects
                 : reinterpret_cast<const segment_command *>(LCPtr)->nsects;
        if (Nsects >
                (std::numeric_limits<uint32_t>::max() - BaseSize) /
                    SectionSize ||
            BaseSize + Nsects * SectionSize > CmdSize)
          return;

        uint64_t EHVA = 0;
        uint64_t EHSize = 0;
        uint64_t EHFileOff = 0;
        uint64_t HeaderOff = 0;
        std::vector<uint64_t> SectionOffsets;
        for (uint32_t I = 0; I < Nsects; ++I) {
          const uint8_t *SectionPtr = LCPtr + BaseSize + I * SectionSize;
          uint64_t Addr = 0;
          uint64_t Size = 0;
          uint32_t FileOff = 0;
          const char *Name = nullptr;
          if (Is64) {
            const auto *Section =
                reinterpret_cast<const section_64 *>(SectionPtr);
            Addr = Section->addr;
            Size = Section->size;
            FileOff = Section->offset;
            Name = Section->sectname;
          } else {
            const auto *Section = reinterpret_cast<const section *>(SectionPtr);
            Addr = Section->addr;
            Size = Section->size;
            FileOff = Section->offset;
            Name = Section->sectname;
          }
          if (FileOff != 0)
            SectionOffsets.push_back(FileOff);
          if (readMachOName(Name) != section_names::macho::EhFrame)
            continue;
          EHVA = Addr;
          EHSize = Size;
          EHFileOff = FileOff;
          HeaderOff = static_cast<uint64_t>(SectionPtr - Binary.data());
        }
        if (EHVA == 0 || EHSize == 0 || EHFileOff == 0 ||
            !rangeInBounds(EHFileOff, EHSize, Binary.size()))
          return;

        auto LogicalSize = getEHFrameAppendOffset(llvm::ArrayRef<uint8_t>(
            Binary.data() + EHFileOff, static_cast<size_t>(EHSize)));
        if (!LogicalSize || *LogicalSize > EHSize ||
            EHVA > std::numeric_limits<uint64_t>::max() - *LogicalSize ||
            EHFileOff > std::numeric_limits<uint64_t>::max() - *LogicalSize)
          return;

        uint64_t AppendFileOff = EHFileOff + *LogicalSize;
        uint64_t Limit = Seg.FileOff + Seg.FileSize;
        for (uint64_t Offset : SectionOffsets)
          if (Offset > AppendFileOff)
            Limit = std::min(Limit, Offset);
        if (Limit < AppendFileOff || Limit > Binary.size())
          return;

        Out.Is64 = Is64;
        Out.SectionVA = EHVA;
        Out.SectionFileOff = EHFileOff;
        Out.AppendVA = EHVA + *LogicalSize;
        Out.AppendFileOff = AppendFileOff;
        Out.LimitFileOff = Limit;
        Out.SectionHeaderOff = HeaderOff;
        Found = true;
      });
  return Found;
}

bool appendGeneratedEHFrame(std::vector<uint8_t> &Binary,
                            const EHFrameAppendRegion &Region,
                            const CompiledSection &Generated) {
  if (Generated.Name != section_names::macho::EhFrame ||
      Generated.IsInImage || Generated.VA != Region.AppendVA ||
      Generated.Size != Generated.ExternalBytes.size() ||
      Generated.Size > Region.LimitFileOff - Region.AppendFileOff ||
      !rangeInBounds(Region.AppendFileOff, Generated.Size, Binary.size()))
    return false;

  if (!Generated.ExternalBytes.empty())
    std::memcpy(Binary.data() + Region.AppendFileOff,
                Generated.ExternalBytes.data(), Generated.ExternalBytes.size());

  uint64_t NewSize =
      Region.AppendVA - Region.SectionVA + Generated.ExternalBytes.size();
  if (Region.Is64) {
    if (!rangeInBounds(Region.SectionHeaderOff, sizeof(section_64),
                       Binary.size()))
      return false;
    reinterpret_cast<section_64 *>(Binary.data() + Region.SectionHeaderOff)
        ->size = NewSize;
  } else {
    if (NewSize > std::numeric_limits<uint32_t>::max() ||
        !rangeInBounds(Region.SectionHeaderOff, sizeof(section), Binary.size()))
      return false;
    reinterpret_cast<section *>(Binary.data() + Region.SectionHeaderOff)->size =
        static_cast<uint32_t>(NewSize);
  }
  return true;
}

} // namespace

bool MachOPatcher::parseLayout(const std::vector<uint8_t> &Data,
                               PatchLayout &Layout) {
  auto Hdr = parseMachOHeader(Data.data(), Data.size());
  if (Hdr.HeaderSize == 0)
    return false;

  Layout.Is64 = Hdr.Is64;
  Layout.NCmds = Hdr.NCmds;
  Layout.SizeOfCmds = Hdr.SizeOfCmds;
  Layout.HeaderSize = Hdr.HeaderSize;

  // Largest executable segment, used as a fallback code region when no named
  // "__text" (or --text-section) match is found below.
  uint64_t FallbackExecVA = 0, FallbackExecSize = 0, FallbackExecFileOff = 0;

  forEachMachOLoadCommand(
      Data.data(), Data.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t /*CmdSize*/, bool Is64) {
        if (Cmd != getMachOSegmentCmdID(Is64))
          return;

        auto SF = readMachOSegment(LCPtr, Is64);
        std::string SegName = readMachOName(SF.SegName);
        uint64_t SegEndVM = SF.VMAddr + SF.VMSize;
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
          Layout.LinkeditVA = SF.VMAddr;
          Layout.LinkeditFileOff = SF.FileOff;
          Layout.LinkeditCmdOff = static_cast<uint32_t>(LCPtr - Data.data());
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
  return Layout.LinkeditFileOff != 0;
}

// shiftLinkeditField / shiftMachOLoadCommandOffsets live in BinaryUtils.h

uint64_t MachOPatcher::plannedExecSegmentVA(const std::vector<uint8_t> &Binary,
                                            Arch /*TargetArch*/) {
  PatchLayout Layout;
  if (!parseLayout(Binary, Layout))
    return 0;
  // The new segment takes over the old __LINKEDIT VA (which is shifted up).
  return Layout.LinkeditVA;
}

uint64_t MachOPatcher::appendExecSegment(std::vector<uint8_t> &Binary,
                                         llvm::ArrayRef<uint8_t> Code,
                                         llvm::StringRef SegName,
                                         Arch TargetArch) {
  const uint64_t PageSize = machoPageSize(TargetArch);
  PatchLayout Layout;
  if (!parseLayout(Binary, Layout))
    return 0;
  // A crafted __LINKEDIT fileoff past EOF would make the iterator range below
  // (Binary.begin() + LinkeditFileOff .. Binary.end()) invalid; reject it here.
  if (Layout.LinkeditFileOff > Binary.size())
    return 0;

  const bool Is64 = Layout.Is64;
  const uint32_t SegCmdID = getMachOSegmentCmdID(Is64);
  const uint32_t HdrSize = Layout.HeaderSize;
  const uint32_t NewSegCmdSize = getMachOSegmentCmdSize(Is64);

  uint64_t InsertOff = alignUp(Layout.LinkeditFileOff, PageSize);
  uint64_t NewSegVMAddr = Layout.LinkeditVA;

  uint64_t TextSize = Code.size();
  uint64_t CodePadded = alignUp(TextSize, PageSize);

  std::vector<uint8_t> LinkeditData(Binary.begin() + Layout.LinkeditFileOff,
                                    Binary.end());
  uint64_t Shift = InsertOff + CodePadded - Layout.LinkeditFileOff;

  Binary.resize(InsertOff, 0);
  Binary.resize(static_cast<size_t>(InsertOff + TextSize), 0);
  std::memcpy(Binary.data() + InsertOff, Code.data(),
              static_cast<size_t>(TextSize));
  Binary.resize(static_cast<size_t>(InsertOff + CodePadded), 0);
  uint64_t NewLinkeditOff = Binary.size();
  Binary.insert(Binary.end(), LinkeditData.begin(), LinkeditData.end());

  uint64_t NewLinkeditVMAddr = NewSegVMAddr + CodePadded;

  std::string SegStr = SegName.empty() ? kDefaultNdTextSegment : SegName.str();
  std::vector<uint8_t> NewSegBuf = buildMachOSegmentCmd(Is64, SegStr);
  MachOSegFields NewSF{};
  NewSF.VMAddr = NewSegVMAddr;
  NewSF.VMSize = CodePadded;
  NewSF.FileOff = InsertOff;
  NewSF.FileSize = CodePadded;
  NewSF.MaxProt = VM_PROT_READ | VM_PROT_EXECUTE;
  NewSF.InitProt = VM_PROT_READ | VM_PROT_EXECUTE;
  writeMachOSegment(NewSegBuf.data(), Is64, NewSF);

  struct CmdInfo {
    uint32_t Off, Size, Cmd;
  };
  std::vector<CmdInfo> Cmds;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t CmdSize, bool) {
        uint32_t O = static_cast<uint32_t>(LCPtr - Binary.data());
        Cmds.push_back({O, CmdSize, Cmd});
      });

  uint64_t FirstDataOff =
      findFirstMachODataOffset(Binary.data(), Binary.size());
  uint32_t SizeOfCmds = Layout.SizeOfCmds;
  // findFirstMachODataOffset returns UINT64_MAX when no section offset is found,
  // and is otherwise an untrusted section offset.  Require it to sit within the
  // buffer and past the mach header + load-command block so Avail cannot
  // underflow and the trailing memset up to FirstDataOff stays in bounds.
  uint64_t HdrPlusCmds = static_cast<uint64_t>(HdrSize) + SizeOfCmds;
  if (FirstDataOff > Binary.size() || FirstDataOff < HdrPlusCmds) {
    llvm::WithColor::error() << "macho_patch: no header space\n";
    return 0;
  }
  uint32_t Avail = static_cast<uint32_t>(FirstDataOff - HdrPlusCmds);
  uint32_t Freed = 0;
  for (auto &C : Cmds)
    if (shouldDropLoadCommand(C.Cmd))
      Freed += C.Size;
  if (Avail + Freed < NewSegCmdSize) {
    llvm::WithColor::error() << "macho_patch: no header space\n";
    return 0;
  }

  std::vector<std::vector<uint8_t>> OutCmds;
  uint32_t TotCmds = 0, TotSize = 0;
  for (auto &C : Cmds) {
    if (shouldDropLoadCommand(C.Cmd))
      continue;
    bool IsLinkedit = false;
    if (C.Cmd == SegCmdID) {
      auto SF = readMachOSegment(Binary.data() + C.Off, Is64);
      IsLinkedit =
          (readMachOName(SF.SegName) == section_names::macho::LinkeditSeg);
    }
    if (IsLinkedit) {
      OutCmds.push_back(NewSegBuf);
      TotCmds++;
      TotSize += NewSegCmdSize;
    }
    std::vector<uint8_t> CmdData(C.Size);
    std::memcpy(CmdData.data(), Binary.data() + C.Off, C.Size);
    if (IsLinkedit) {
      auto SF = readMachOSegment(CmdData.data(), Is64);
      SF.VMAddr = NewLinkeditVMAddr;
      SF.FileOff = NewLinkeditOff;
      SF.VMSize = alignUp(LinkeditData.size(), PageSize);
      SF.FileSize = LinkeditData.size();
      writeMachOSegment(CmdData.data(), Is64, SF);
    }
    shiftMachOLoadCommandOffsets(CmdData.data(), C.Cmd,
                                 static_cast<int64_t>(Shift));
    OutCmds.push_back(std::move(CmdData));
    TotCmds++;
    TotSize += C.Size;
  }

  setMachOHeaderCmds(Binary.data(), Is64, TotCmds, TotSize);
  uint32_t WriteOff = HdrSize;
  for (auto &CD : OutCmds) {
    std::memcpy(Binary.data() + WriteOff, CD.data(), CD.size());
    WriteOff += CD.size();
  }
  if (WriteOff < FirstDataOff)
    std::memset(Binary.data() + WriteOff, 0, FirstDataOff - WriteOff);

  return NewSegVMAddr;
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

  return readPatchWrite(
      InputPath, OutputPath, /*SetExecPerm=*/true, "macho_patch",
      [&](std::vector<uint8_t> &Binary, PatchResult &Result) -> bool {
        PatchLayout Layout;
        if (!parseLayout(Binary, Layout))
          return false;

        uint64_t NewSegVMAddr = plannedExecSegmentVA(Binary, TargetArch);
        if (NewSegVMAddr == 0) {
          llvm::WithColor::error() << "macho_patch: cannot plan exec segment\n";
          return false;
        }

        // The address-model resolver: maps external symbol names to VAs in the
        // target binary (exports / import stubs / __nd_data_* absolute data).
        InstructionMode ResolveMode = CachedMode;
        auto SerializeResolvedCode = [&](uint64_t VA, bool IsCode) {
          return IsCode ? serializeCodePointer(VA, TargetArch, ResolveMode)
                        : VA;
        };
        auto IsExecutable = [&](uint64_t VA) {
          const Segment *Seg = CachedImage ? CachedImage->getSegmentFor(VA)
                                           : nullptr;
          return Seg && Seg->isExecutable();
        };
        auto Resolve = [&](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
          std::string Name = Sym.str();
          if (CachedExports) {
            for (auto &E : *CachedExports)
              if (E.Name == Name || ("_" + E.Name) == Name ||
                  (Name.size() > 1 && Name[0] == '_' &&
                   E.Name == Name.substr(1)))
                return SerializeResolvedCode(E.Addr, IsExecutable(E.Addr));
          }
          if (CachedImports) {
            for (auto &I : *CachedImports)
              if (I.Name == Name || ("_" + I.Name) == Name ||
                  (Name.size() > 1 && Name[0] == '_' &&
                   I.Name == Name.substr(1)))
                return SerializeResolvedCode(I.IATAddr,
                                             IsExecutable(I.IATAddr));
          }
          if (CachedSymbols) {
            for (const auto &S : *CachedSymbols)
              if (S.IsFunc &&
                  (S.Name == Name || ("_" + S.Name) == Name ||
                   (Name.size() > 1 && Name[0] == '_' &&
                    S.Name == Name.substr(1))))
                return SerializeResolvedCode(S.Addr, true);
          }
          if (auto VA = parseNdDataSymbol(Name)) {
            bool IsCode = IsExecutable(*VA) && CachedImage &&
                          CachedImage->CodeRefTargets.count(*VA) != 0;
            return SerializeResolvedCode(*VA, IsCode);
          }
          if (auto VA = parseNdCodePtrSymbol(Name))
            return SerializeResolvedCode(*VA, false);
          return std::nullopt;
        };

        // dyld exposes the image's __TEXT,__eh_frame to libunwind.  Keep the
        // original records and compile regenerated records directly into the
        // file-backed tail of that section so their PC-relative fields are
        // based on the address at which dyld will actually register them.
        EHFrameAppendRegion EHRegion;
        const bool HasEHFrameRegion =
            findEHFrameAppendRegion(Binary, EHRegion);
        auto FixedSectionVA =
            [&](llvm::StringRef Name) -> std::optional<uint64_t> {
          if (HasEHFrameRegion &&
              Name == section_names::macho::EhFrame)
            return EHRegion.AppendVA;
          return std::nullopt;
        };

        CompiledImage Img = compileImageForPatchWithFixedSectionVAs(
            Mod, TargetArch, BinaryFormat::MachO, NewSegVMAddr, Resolve,
            FixedSectionVA);
        if (!Img.Success || Img.Bytes.empty()) {
          llvm::WithColor::error()
              << "macho_patch: compileImageForPatch failed\n";
          return false;
        }

        const CompiledSection *GeneratedEHFrame = nullptr;
        for (const CompiledSection &Section : Img.Sections)
          if (Section.IsAllocated &&
              Section.Name == section_names::macho::EhFrame) {
            GeneratedEHFrame = &Section;
            break;
          }
        if (GeneratedEHFrame &&
            (!HasEHFrameRegion ||
             !appendGeneratedEHFrame(Binary, EHRegion, *GeneratedEHFrame))) {
          llvm::WithColor::error()
              << "macho_patch: cannot register regenerated __eh_frame\n";
          return false;
        }

        uint64_t TextSize = Img.Bytes.size();
        uint64_t Placed =
            appendExecSegment(Binary, Img.Bytes, Opts.SegmentName, TargetArch);
        if (Placed == 0) {
          llvm::WithColor::error() << "macho_patch: appendExecSegment failed\n";
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

        if (Layout.TextSectVA != 0 && Layout.TextSectSize != 0) {
          Result.TrampolineCount =
              installTrampolines(Binary, Img.SymbolAddrs, Layout.TextSectVA,
                                 Layout.TextSectSize, Layout.TextSectFileOff,
                                 /*ImageBase=*/0, TargetArch, CachedMode,
                                 CachedSymbols, CachedCodeRanges,
                                 CachedExports);
        }

        Result.Success = true;
        Result.CodeSize = TextSize;
        return true;
      });
}

} // namespace neverd
