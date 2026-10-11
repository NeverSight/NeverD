//===- MachOLoader.cpp - Mach-O binary format loader --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements Mach-O loading using LLVM's Object/MachO API: header
/// validation, segment/section parsing, symbol table resolution, and
/// stub-to-import mapping via indirect symbol table.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/MachOLoader.h"

#include "neverd/Limits.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/Go/GoRuntimeEH.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/MachO/MachOARM32Mode.h"
#include "neverd/loader/MachO/MachOExceptions.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/ObjC/ObjCEH.h"
#include "neverd/loader/ObjC/ObjCMethods.h"
#include "neverd/loader/Rust/RustEH.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <vector>

#define DEBUG_TYPE "neverd-macho-loader"

namespace neverd {

namespace {

using namespace llvm::MachO;

Arch cpuTypeToArch(uint32_t CpuType, bool Is64) {
  if (Is64) {
    switch (CpuType) {
    case CPU_TYPE_X86_64:
      return Arch::X64;
    case CPU_TYPE_ARM64:
      return Arch::AArch64;
    default:
      return Arch::Unknown;
    }
  }
  switch (CpuType) {
  case CPU_TYPE_X86:
    return Arch::X86;
  case CPU_TYPE_ARM:
    return Arch::ARM;
  default:
    return Arch::Unknown;
  }
}

/// How the load dialog names a cputype value.
std::string cpuName(uint32_t CpuType) {
  switch (CpuType) {
#define NEVERD_MACHO_CPU(Value, Name)                                          \
  case Value:                                                                  \
    return Name;
#include "neverd/loader/MachO/MachONames.def"
  }
  return llvm::formatv("CPU {0:x}", CpuType).str();
}

/// How the load dialog names a filetype value.
std::string fileTypeName(uint32_t FileType) {
  switch (FileType) {
#define NEVERD_MACHO_FILE(Value, Name)                                         \
  case Value:                                                                  \
    return Name;
#include "neverd/loader/MachO/MachONames.def"
  }
  return llvm::formatv("type {0}", FileType).str();
}

/// Whether load() reads \p Obj, as which processor, into \p Row.
void judge(const llvm::object::MachOObjectFile &Obj, LoadCandidate &Row) {
  const uint32_t CpuType =
      Obj.is64Bit() ? Obj.getHeader64().cputype : Obj.getHeader().cputype;
  Row.TheArch = cpuTypeToArch(CpuType, Obj.is64Bit());
  Row.Bits = Obj.is64Bit() ? 64 : 32;
  Row.BigEndian = !Obj.isLittleEndian();
  Row.Loadable = Row.TheArch != Arch::Unknown;
  if (!Row.Loadable)
    Row.Reason = llvm::formatv(getLoadReasonText(LoadReason::Processor).data(),
                               cpuName(CpuType))
                     .str();
}

} // anonymous namespace

llvm::Expected<BinaryImage>
MachOLoader::load(const std::filesystem::path &Path) {
  auto Buffer = readFileBuffer(Path, BinaryFormat::MachO);
  if (!Buffer)
    return Buffer.takeError();
  return loadBuffer((*Buffer)->getMemBufferRef());
}

llvm::Expected<BinaryImage>
MachOLoader::loadBuffer(llvm::MemoryBufferRef Buffer) {
  auto OpenOr = macho_loader::openMachOBuffer(Buffer);
  if (!OpenOr)
    return OpenOr.takeError();

  auto MachOObj = std::move(*OpenOr);
  const auto &Obj = *MachOObj;

  bool Is64 = Obj.is64Bit();
  bool IsLE = Obj.isLittleEndian();
  if (!IsLE)
    return llvm::make_error<llvm::StringError>(
        "macho: big-endian not supported", llvm::inconvertibleErrorCode());

  BinaryImage Img;
  initializeImage(Buffer, Img, BinaryFormat::MachO, /*CopyRaw=*/false);
  Img.IsRelocatable = Obj.getHeader().filetype == MH_OBJECT;
  Img.MachOIsDylib = Obj.getHeader().filetype == MH_DYLIB;
  // An executable linked without MH_PIE runs at its link address.
  Img.LoadsAtLinkAddress = Obj.getHeader().filetype == MH_EXECUTE &&
                           !(Obj.getHeader().flags & MH_PIE);
  llvm::StringRef ObjBytes = Obj.getData();
  Img.Raw.assign(reinterpret_cast<const uint8_t *>(ObjBytes.data()),
                 reinterpret_cast<const uint8_t *>(ObjBytes.data()) +
                     ObjBytes.size());

  uint32_t CpuType = Is64 ? Obj.getHeader64().cputype : Obj.getHeader().cputype;
  Img.Arch = cpuTypeToArch(CpuType, Is64);
  Img.Bits = Is64 ? Bitness::Bits64 : Bitness::Bits32;
  if (Img.Arch == Arch::Unknown)
    return llvm::make_error<llvm::StringError>(
        "macho: unsupported cpu type 0x" + llvm::utohexstr(CpuType),
        llvm::inconvertibleErrorCode());
  if (Img.Arch == Arch::ARM) {
    auto ModeInfo = macho_arm32::parseModeInfo(Img.Raw);
    if (!ModeInfo)
      return ModeInfo.takeError();
    Img.Mode = ModeInfo->UniformMode;
    Img.ARMRequiredMode = ModeInfo->RequiredMode;
    Img.ARMCodeModeEntries = std::move(ModeInfo->CodeSymbolModes);
  }

  va_t TextVMAddr = 0;
  bool HasTextVMAddr = false;
  macho_loader::DyldInfoOffsets DyldInfo;
  macho_loader::FunctionStartsInfo FuncStarts;
  macho_loader::ChainedFixupsInfo ChainedFixups;
  std::vector<macho_loader::SectionInfo> Sections;
  const uint8_t *BasePtr = Img.Raw.data();
  size_t FileSize = Img.Raw.size();
  bool InvalidAddressRange = false;
  bool InvalidFileRange = false;
  bool OverlappingObjectSections = false;

  auto AddSection = [&](const char *SectName, const char *SegName,
                        uint64_t Addr, uint64_t Size, uint32_t Offset,
                        uint32_t Align, uint32_t Flags, uint32_t Reserved1,
                        uint32_t Reserved2, uint32_t SegProt) {
    if (Size > InvalidVA - Addr) {
      InvalidAddressRange = true;
      return;
    }
    macho_loader::SectionInfo Info;
    Info.Name = readMachOName(SectName);
    Info.SegName = readMachOName(SegName);
    Info.Addr = Addr;
    Info.Size = Size;
    Info.Reserved1 = Reserved1;
    Info.Flags = Flags & llvm::MachO::SECTION_TYPE;
    Info.StubSize = Reserved2;
    Sections.push_back(Info);
    bool IsZeroFill = Info.Flags == llvm::MachO::S_ZEROFILL ||
                      Info.Flags == llvm::MachO::S_GB_ZEROFILL ||
                      Info.Flags == llvm::MachO::S_THREAD_LOCAL_ZEROFILL;
    if (!IsZeroFill && Size > 0 && !rangeInBounds(Offset, Size, FileSize)) {
      InvalidFileRange = true;
      return;
    }

    Section ImgSec;
    ImgSec.Name = Info.Name;
    ImgSec.SegmentName = Info.SegName;
    ImgSec.VA = Addr;
    ImgSec.Size = Size;
    ImgSec.FileOff = Offset;
    ImgSec.FileSz = IsZeroFill ? 0 : Size;
    // section.align is an exponent.  Malformed values >= the uint32_t width
    // must not become an undefined shift; fall back to byte alignment.
    ImgSec.Alignment = Align < 32 ? (1u << Align) : 1u;
    ImgSec.Type = Flags;
    if (Img.IsRelocatable) {
      // MH_OBJECT uses one coarse LC_SEGMENT (commonly RWX) for sections that
      // will land in different final segments.  Section attrs and segname carry
      // the intended permissions.
      ImgSec.Flags = SegmentFlags::Readable;
      bool IsInstructions = Flags & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                                     llvm::MachO::S_ATTR_SOME_INSTRUCTIONS);
      bool IsReadOnlySegment =
          Info.SegName == section_names::macho::TextSeg ||
          Info.SegName == section_names::macho::DataConstSeg;
      if (IsInstructions) {
        ImgSec.Flags = ImgSec.Flags | SegmentFlags::Executable;
      } else if (!IsReadOnlySegment &&
                 (Info.SegName == section_names::macho::DataSeg ||
                  (SegProt & llvm::MachO::VM_PROT_WRITE))) {
        ImgSec.Flags = ImgSec.Flags | SegmentFlags::Writable;
      }
    } else {
      ImgSec.Flags = machoProtToNd(SegProt);
    }
    if (ImgSec.FileSz > 0) {
      ImgSec.Data.assign(BasePtr + Offset, BasePtr + Offset + Size);
    } else if (Img.IsRelocatable && IsZeroFill &&
               Size <= limits::kMaxSegmentZeroFill) {
      ImgSec.Data.resize(static_cast<size_t>(Size), 0);
    }
    if (Img.IsRelocatable && ImgSec.Size != 0) {
      Segment Seg;
      Seg.Name = ImgSec.SegmentName;
      Seg.VA = ImgSec.VA;
      Seg.Size = ImgSec.Size;
      Seg.FileOff = ImgSec.FileOff;
      Seg.FileSz = ImgSec.FileSz;
      Seg.Flags = ImgSec.Flags;
      Seg.Data = ImgSec.Data;
      for (const Segment &Existing : Img.Segments) {
        if (Seg.VA < Existing.VA + Existing.Size &&
            Existing.VA < Seg.VA + Seg.Size) {
          OverlappingObjectSections = true;
          break;
        }
      }
      if (!HasTextVMAddr && Seg.Name == section_names::macho::TextSeg &&
          Seg.isExecutable()) {
        TextVMAddr = Seg.VA;
        HasTextVMAddr = true;
      }
      Img.Segments.push_back(std::move(Seg));
    }
    Img.Sections.push_back(std::move(ImgSec));
  };

  auto AddSegment = [&](const char *Name, uint64_t VMAddr, uint64_t VMSize,
                        uint64_t FileOff, uint64_t FileSz, uint32_t Prot,
                        uint32_t Flags) {
    if (VMSize > InvalidVA - VMAddr) {
      InvalidAddressRange = true;
      return;
    }
    if (FileSz > 0 && !rangeInBounds(FileOff, FileSz, FileSize)) {
      InvalidFileRange = true;
      return;
    }
    if (Img.IsRelocatable)
      return;
    // A segment may carry more file bytes than it maps.  Go's internal Mach-O
    // linker emits `__DWARF` with a zero vmsize and the entire debug payload in
    // the file, and a `.dSYM` companion is built the same way.  Those bytes are
    // deliberately outside the address space, so the segment maps only what its
    // vmsize covers; treating the excess as corruption would reject every Go
    // macOS binary.
    const uint64_t MappedFileSz = std::min(FileSz, VMSize);
    Segment Seg;
    Seg.Name = readMachOName(Name);
    Seg.VA = VMAddr;
    Seg.Size = VMSize;
    Seg.FileOff = FileOff;
    Seg.FileSz = MappedFileSz;
    Seg.Flags = machoProtToNd(Prot);
    Seg.ReadOnlyAfterRelocations = (Flags & SG_READ_ONLY) != 0;
    if (MappedFileSz > 0) {
      Seg.Data.assign(BasePtr + FileOff, BasePtr + FileOff + MappedFileSz);
      // vmsize is untrusted; only zero-fill up to the cap (see
      // kMaxSegmentZeroFill) so a crafted size cannot force a huge allocation.
      if (VMSize > MappedFileSz && VMSize <= limits::kMaxSegmentZeroFill)
        Seg.Data.resize(static_cast<size_t>(VMSize), 0);
    }
    if (Seg.Name == section_names::macho::TextSeg) {
      TextVMAddr = Seg.VA;
      HasTextVMAddr = true;
    }
    Img.Segments.push_back(std::move(Seg));
  };

  for (const auto &LC : Obj.load_commands()) {
    if (LC.C.cmd == LC_SEGMENT_64 && Is64) {
      auto SegCmd = Obj.getSegment64LoadCommand(LC);
      AddSegment(SegCmd.segname, SegCmd.vmaddr, SegCmd.vmsize, SegCmd.fileoff,
                 SegCmd.filesize, SegCmd.initprot, SegCmd.flags);
      for (uint32_t SI = 0; SI < SegCmd.nsects; ++SI) {
        auto S = Obj.getSection64(LC, SI);
        AddSection(S.sectname, S.segname, S.addr, S.size, S.offset, S.align,
                   S.flags, S.reserved1, S.reserved2, SegCmd.initprot);
      }
    }

    if (LC.C.cmd == LC_SEGMENT && !Is64) {
      auto SegCmd = Obj.getSegmentLoadCommand(LC);
      AddSegment(SegCmd.segname, SegCmd.vmaddr, SegCmd.vmsize, SegCmd.fileoff,
                 SegCmd.filesize, SegCmd.initprot, SegCmd.flags);
      for (uint32_t SI = 0; SI < SegCmd.nsects; ++SI) {
        auto S = Obj.getSection(LC, SI);
        AddSection(S.sectname, S.segname, S.addr, S.size, S.offset, S.align,
                   S.flags, S.reserved1, S.reserved2, SegCmd.initprot);
      }
    }

    if (LC.C.cmd == LC_FUNCTION_STARTS &&
        LC.C.cmdsize >= sizeof(linkedit_data_command)) {
      auto LDC = Obj.getLinkeditDataLoadCommand(LC);
      FuncStarts.DataOff = LDC.dataoff;
      FuncStarts.DataSize = LDC.datasize;
    }

    if (LC.C.cmd == LC_DYLD_CHAINED_FIXUPS &&
        LC.C.cmdsize >= sizeof(linkedit_data_command)) {
      Img.MachOChainedFixupsAmbiguous |= Img.MachOHasChainedFixups;
      Img.MachOHasChainedFixups = true;
      auto LDC = Obj.getLinkeditDataLoadCommand(LC);
      ChainedFixups.DataOff = LDC.dataoff;
      ChainedFixups.DataSize = LDC.datasize;
    }

    if (LC.C.cmd == LC_DYLD_EXPORTS_TRIE &&
        LC.C.cmdsize >= sizeof(linkedit_data_command)) {
      auto LDC = Obj.getLinkeditDataLoadCommand(LC);
      if (DyldInfo.ExportOff == 0) {
        DyldInfo.ExportOff = LDC.dataoff;
        DyldInfo.ExportSize = LDC.datasize;
      }
    }
  }

  if (InvalidAddressRange)
    return llvm::make_error<llvm::StringError>(
        "macho: virtual address range overflows",
        llvm::inconvertibleErrorCode());
  if (InvalidFileRange)
    return llvm::make_error<llvm::StringError>(
        "macho: segment or section file range is invalid",
        llvm::inconvertibleErrorCode());
  if (OverlappingObjectSections)
    return llvm::make_error<llvm::StringError>(
        "macho: relocatable sections overlap", llvm::inconvertibleErrorCode());

  macho_loader::parseDyldInfoLoadCommands(Obj, DyldInfo);

  if (Img.Segments.empty())
    return llvm::make_error<llvm::StringError>("macho: no segments found",
                                               llvm::inconvertibleErrorCode());

  va_t Lo = InvalidVA;
  for (const auto &Seg : Img.Segments)
    if (Seg.VA < Lo)
      Lo = Seg.VA;
  Img.Base = (Lo != InvalidVA) ? Lo : 0;

  // TextVMAddr is the image base: the VA of the segment that maps the Mach
  // header (file offset 0).  LC_MAIN entryoff, LC_FUNCTION_STARTS deltas, the
  // export trie and chained-fixup offsets are all encoded relative to it.
  // Conventionally that segment is named __TEXT (handled above), but a
  // packer/protector can rename it; when no segment carries the canonical name
  // fall back to whichever segment actually maps file offset 0 so symbol VAs
  // stay correct for renamed layouts.  filesize>0 excludes __PAGEZERO (which
  // also has fileoff 0 but maps no bytes).
  if (!HasTextVMAddr)
    for (const auto &Seg : Img.Segments)
      if (Seg.FileOff == 0 && Seg.FileSz > 0) {
        TextVMAddr = Seg.VA;
        HasTextVMAddr = true;
        break;
      }

  macho_loader::parseEntryPoint(Obj, Img, TextVMAddr);
  macho_loader::parseRuntimeLoadCommands(Obj, Img);

  for (const macho_loader::SectionInfo &Sec : Sections)
    if (Sec.Flags == llvm::MachO::S_SYMBOL_STUBS ||
        Sec.Name == section_names::macho::ObjCStubs ||
        Sec.Name == section_names::macho::StubHelper)
      Img.recordImportStubRange(Sec.Addr, Sec.Size);

  // --- Symbol table ---
  for (auto SI = Obj.symbol_begin(), SE = Obj.symbol_end(); SI != SE; ++SI) {
    llvm::object::DataRefImpl DRI = SI->getRawDataRefImpl();
    uint64_t SymAddr;
    uint8_t NType, NSect;

    if (Is64) {
      auto Entry = Obj.getSymbol64TableEntry(DRI);
      SymAddr = Entry.n_value;
      NType = Entry.n_type;
      NSect = Entry.n_sect;
    } else {
      auto Entry = Obj.getSymbolTableEntry(DRI);
      SymAddr = Entry.n_value;
      NType = Entry.n_type;
      NSect = Entry.n_sect;
    }

    // STABS entries describe debug records, not addressable symbols. N_FUN
    // duplicates ordinary function names; N_OSO carries an object timestamp.
    if ((NType & llvm::MachO::N_STAB) != 0)
      continue;

    // N_UNDF's n_value is a common-symbol size or linker bookkeeping, not an
    // addressable definition. Some linked Swift images retain a nonzero
    // undefined record beside the actual N_SECT metadata symbol. Publishing
    // both as image symbols makes the exact storage identity look ambiguous.
    if ((NType & llvm::MachO::N_TYPE) == llvm::MachO::N_UNDF)
      continue;

    bool IsSect = (NType & llvm::MachO::N_TYPE) == llvm::MachO::N_SECT &&
                  NSect > 0 && NSect <= Img.Sections.size();
    if (SymAddr == 0 && !IsSect)
      continue;

    auto NameOrErr = Obj.getSymbolName(DRI);
    if (!NameOrErr) {
      llvm::consumeError(NameOrErr.takeError());
      continue;
    }
    if (NameOrErr->empty())
      continue;
    // LLVM's local `ltmp<N>` marks where a section of an object starts, for
    // the relocations that name it, at the address of whatever begins there:
    // a function or a datum another symbol names.
    if ((NType & llvm::MachO::N_EXT) == 0 && NameOrErr->starts_with("ltmp"))
      continue;

    if (Img.Arch == Arch::ARM && IsSect)
      SymAddr = clearThumbBit(SymAddr);

    Symbol Sym;
    Sym.Name = NameOrErr->str();
    Sym.Addr = SymAddr;
    if (IsSect) {
      const Section &Sec = Img.Sections[NSect - 1];
      const bool IsInstructions =
          (Sec.Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                       llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) != 0;
      // Mach-O has no ELF-style symbol type for distinguishing functions from
      // labels. Clang/LLVM's `.L` names are assembler-local control-flow labels
      // inside a function and must not shorten the owning function's CFG.
      const bool IsAssemblerLocalLabel = NameOrErr->starts_with(".L");
      Sym.IsFunc =
          IsInstructions && Sec.contains(SymAddr) && !IsAssemblerLocalLabel;
    }
    Img.Symbols.push_back(Sym);

    if (Sym.IsFunc) {
      Export Exp;
      Exp.Name = Sym.Name;
      Exp.Addr = SymAddr;
      Img.Exports.push_back(std::move(Exp));
    }
  }

  // MH_OBJECT nlists have no size field. In a pure-instruction section, the
  // next distinct function symbol or the section end bounds each function.
  // Do not extrapolate through a mixed code/data section.
  if (Obj.getHeader().filetype == MH_OBJECT) {
    for (const Section &Sec : Img.Sections) {
      if ((Sec.Type & S_ATTR_PURE_INSTRUCTIONS) == 0 || Sec.Size == 0 ||
          Sec.Size > InvalidVA - Sec.VA)
        continue;
      std::vector<size_t> Functions;
      for (size_t I = 0; I < Img.Symbols.size(); ++I)
        if (Img.Symbols[I].IsFunc && Sec.contains(Img.Symbols[I].Addr))
          Functions.push_back(I);
      std::sort(Functions.begin(), Functions.end(), [&](size_t A, size_t B) {
        return Img.Symbols[A].Addr < Img.Symbols[B].Addr;
      });
      const va_t SectionEnd = Sec.VA + Sec.Size;
      for (size_t I = 0; I < Functions.size(); ++I) {
        Symbol &Sym = Img.Symbols[Functions[I]];
        va_t End = SectionEnd;
        for (size_t J = I + 1; J < Functions.size(); ++J)
          if (Img.Symbols[Functions[J]].Addr > Sym.Addr) {
            End = Img.Symbols[Functions[J]].Addr;
            break;
          }
        Sym.Size = End - Sym.Addr;
      }
    }
  }

  // --- Apply relocations for .o files ---
  if (Obj.getHeader().filetype == MH_OBJECT) {
    if (llvm::Error Err = macho_loader::applyObjectRelocations(Obj, Img))
      return std::move(Err);
  }

  // --- Stub-to-import mapping via indirect symbol table ---
  macho_loader::parseStubImports(Obj, Sections, BasePtr, FileSize, Is64, Img);

  // --- Pointer-slot (__got / __la_symbol_ptr) -> import mapping.  Names the
  // GOT-indirect stack-probe call (____chkstk_darwin) the rewriter elides. ---
  macho_loader::parseNonLazyPtrImports(Obj, Sections, BasePtr, FileSize, Is64,
                                       Img);

  macho_loader::parseFunctionStarts(BasePtr, FileSize, FuncStarts, TextVMAddr,
                                    Img);
  macho_loader::parseNeededLibraries(Obj, Img);
  macho_loader::parseBindStreams(BasePtr, FileSize, DyldInfo, Img);
  macho_loader::parseRebaseStream(BasePtr, FileSize, DyldInfo, Img);
  macho_loader::parseChainedFixupsImports(BasePtr, FileSize, ChainedFixups,
                                          Img);
  macho_loader::parseChainedFixupsRebases(BasePtr, FileSize, ChainedFixups,
                                          TextVMAddr, Img);
  macho_loader::parseRuntimeFunctionSections(Sections, TextVMAddr, Img);
  macho_loader::parseExportTrie(BasePtr, FileSize, DyldInfo, TextVMAddr, Img);
  macho_loader::parseUUID(Obj, Img);
  macho_loader::parseBuildVersion(Obj, Img);

  parseObjCMethods(Img);
  parseObjCStorage(Img);

  if (llvm::Error Err = applyARMFunctionModeHints(Img, ARMFunctionModes))
    return std::move(Err);
  if (Img.Arch == Arch::ARM)
    if (llvm::Error Err = discoverARMReachableModes(Img))
      return std::move(Err);
  if (llvm::Error Err = verifyARMFunctionModeHints(Img, ARMFunctionModes))
    return std::move(Err);

  runPostLoadDiscovery(
      Img, "macho: loaded " + pathToUTF8(std::filesystem::u8path(
                                             Buffer.getBufferIdentifier().str())
                                             .filename()));
  // Classified before any table is read: a compact-unwind entry names a
  // personality slot, not a language, so what its LSDA means is settled by the
  // image's symbols and sections rather than by the entry.
  Img.ExceptionMetadata.Runtime = detectLanguageRuntime(Img);
  // Personality routines are reached through __got slots bound at load time,
  // so language-table decoding waits until stubs and bindings are known.
  macho_unwind::parseDarwinExceptions(Img);
  go_loader::parseGoExceptions(Img);
  // Rust and Objective-C share the Itanium tables above rather than emitting
  // their own, so their readings of them run last, over records that are
  // already normalized.
  rust_eh::parseRustExceptions(Img);
  objc_eh::parseObjCExceptions(Img);
  return Img;
}

void MachOLoader::identify(llvm::MemoryBufferRef Buffer,
                           std::vector<LoadCandidate> &Rows) {
  const auto malformed = [&](LoadRow Kind, llvm::Error Error) {
    LoadCandidate Row;
    Row.Row = Kind;
    Row.Format = BinaryFormat::MachO;
    Row.Description =
        Kind == LoadRow::MachO
            ? llvm::formatv(getLoadRowText(Kind).data(), fileTypeName(0),
                            cpuName(0))
                  .str()
            : llvm::formatv(getLoadRowText(Kind).data(), 1, cpuName(0)).str();
    Row.Reason = llvm::formatv(getLoadReasonText(LoadReason::Malformed).data(),
                               llvm::toString(std::move(Error)))
                     .str();
    Rows.push_back(std::move(Row));
  };
  auto Binary = llvm::object::createBinary(Buffer);
  if (!Binary)
    return malformed(LoadRow::MachO, Binary.takeError());
  if (auto *Universal =
          llvm::dyn_cast<llvm::object::MachOUniversalBinary>(Binary->get())) {
    // A row for every slice; load() reads the one chooseUniversalSlice picks.
    const auto Chosen = macho_loader::chooseUniversalSlice(*Universal);
    std::string ChosenName;
    size_t Index = 0;
    for (const auto &Slice : Universal->objects())
      if (Index++ == Chosen)
        ChosenName = cpuName(Slice.getCPUType());
    Index = 0;
    for (const auto &Slice : Universal->objects()) {
      LoadCandidate Row;
      Row.Row = LoadRow::FatMachO;
      Row.Format = BinaryFormat::MachO;
      Row.Description = llvm::formatv(getLoadRowText(LoadRow::FatMachO).data(),
                                      Index + 1, cpuName(Slice.getCPUType()))
                            .str();
      if (auto Obj = Slice.getAsObjectFile()) {
        judge(**Obj, Row);
      } else {
        Row.Reason =
            llvm::formatv(getLoadReasonText(LoadReason::Malformed).data(),
                          llvm::toString(Obj.takeError()))
                .str();
      }
      if (Row.Loadable && Chosen != Index) {
        Row.Loadable = false;
        Row.Reason = llvm::formatv(getLoadReasonText(LoadReason::Slice).data(),
                                   ChosenName)
                         .str();
      }
      Rows.push_back(std::move(Row));
      ++Index;
    }
    return;
  }
  auto *Obj = llvm::dyn_cast<llvm::object::MachOObjectFile>(Binary->get());
  if (!Obj)
    return malformed(LoadRow::MachO,
                     llvm::make_error<llvm::StringError>(
                         "not a Mach-O file", llvm::inconvertibleErrorCode()));
  LoadCandidate Row;
  Row.Row = LoadRow::MachO;
  Row.Format = BinaryFormat::MachO;
  const uint32_t FileType =
      Obj->is64Bit() ? Obj->getHeader64().filetype : Obj->getHeader().filetype;
  const uint32_t CpuType =
      Obj->is64Bit() ? Obj->getHeader64().cputype : Obj->getHeader().cputype;
  Row.Description = llvm::formatv(getLoadRowText(LoadRow::MachO).data(),
                                  fileTypeName(FileType), cpuName(CpuType))
                        .str();
  judge(*Obj, Row);
  Rows.push_back(std::move(Row));
}

} // namespace neverd
