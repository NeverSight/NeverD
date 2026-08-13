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

#include "neverd/object/ELFLayout.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/object/PELayout.h"
#include "neverd/support/TargetCodegenInfo.h"
#include "neverd/backend/codegen/BinaryUtils.h"
#include "neverd/backend/codegen/COFF/COFFInplace.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/ELF/ELFInplace.h"
#include "neverd/backend/codegen/ELF/ELFPatch.h"
#include "neverd/backend/codegen/MachO/MachOInplace.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#define DEBUG_TYPE "neverd-rewriter"

namespace neverd {

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

static uint64_t provenFunctionSpan(
    uint64_t OrigVA, uint64_t TextStartVA, uint64_t TextEndVA,
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

size_t BinaryPatcher::installTrampolines(
    std::vector<uint8_t> &Binary,
    const std::map<std::string, uint64_t> &SymbolAddrs, uint64_t OrigTextVA,
    uint64_t OrigTextSize, uint64_t OrigTextFileOff, uint64_t ImageBase,
    Arch TargetArch, InstructionMode Mode, const std::vector<Symbol> *Symbols,
    const std::vector<std::pair<va_t, va_t>> *KnownRanges,
    const std::vector<Export> *Exports,
    std::vector<va_t> *PatchedOriginalEntries,
    std::vector<std::pair<va_t, va_t>> *PatchedEntryMappings) {
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

  size_t Count = 0;
  for (auto &[Name, FuncNewVA] : SymbolAddrs) {
    uint64_t OrigVA = 0;
    bool HasOrig = false;

    llvm::StringRef NameRef(Name);
    // Mach-O's object symbol table adds one global-prefix underscore to the
    // LLVM function name.  Auto-generated names encode their original VA, so
    // accept both `sub_<VA>` and the object spelling `_sub_<VA>`.
    llvm::StringRef AutoName = NameRef;
    if (AutoName.starts_with("_") &&
        AutoName.drop_front().starts_with(kAutoFuncPrefix))
      AutoName = AutoName.drop_front();
    if (AutoName.starts_with(kAutoFuncPrefix)) {
      llvm::StringRef HexPart = AutoName.drop_front(kAutoFuncPrefix.size());
      if (!HexPart.empty() && !HexPart.getAsInteger(16, OrigVA))
        HasOrig = true;
    }

    if (!HasOrig && !ExpMap.empty()) {
      std::string Key = resolveSymbolAlias(Name, ExpMap);
      if (!Key.empty()) {
        OrigVA = ExpMap[Key];
        HasOrig = true;
      }
    }

    if (!HasOrig)
      continue;

    uint64_t OrigRVA = OrigVA >= ImageBase ? OrigVA - ImageBase : OrigVA;
    if (OrigRVA < OrigTextVA || OrigRVA >= OrigTextVA + OrigTextSize)
      continue;

    uint64_t TextDelta = OrigRVA - OrigTextVA;
    if (OrigTextFileOff > InvalidVA - TextDelta)
      continue;
    uint64_t OrigOff = OrigTextFileOff + TextDelta;
    uint64_t OrigAnalysisVA = ImageBase + OrigRVA;
    uint64_t MaxOverwriteBytes = TextEndVA - OrigAnalysisVA;
    if (TargetArch == Arch::ARM && Mode == InstructionMode::Thumb)
      MaxOverwriteBytes = provenFunctionSpan(OrigAnalysisVA, TextStartVA,
                                             TextEndVA, Symbols, KnownRanges,
                                             Exports);
    if (writeTrampoline(Binary, OrigOff, FuncNewVA, ImageBase + OrigRVA,
                        TargetArch, Mode, MaxOverwriteBytes)) {
      ++Count;
      if (PatchedOriginalEntries)
        PatchedOriginalEntries->push_back(OrigAnalysisVA);
      if (PatchedEntryMappings)
        PatchedEntryMappings->push_back({OrigAnalysisVA, FuncNewVA});
      LLVM_DEBUG(llvm::dbgs() << "patch: trampoline " << Name << " @ VA 0x"
                              << llvm::utohexstr(OrigRVA) << " -> 0x"
                              << llvm::utohexstr(FuncNewVA) << "\n");
    } else {
      LLVM_DEBUG(llvm::dbgs()
                 << "patch: skipped unsafe/unreachable trampoline " << Name
                 << " @ VA 0x" << llvm::utohexstr(OrigAnalysisVA) << "\n");
    }
  }
  return Count;
}

} // namespace neverd
