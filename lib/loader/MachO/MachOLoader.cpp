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
#include "neverd/Support/BinaryEncoding.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/Go/GoRuntimeEH.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/MachO/MachOExceptions.h"
#include "neverd/loader/Rust/RustEH.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

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

} // anonymous namespace

llvm::Expected<BinaryImage>
MachOLoader::load(const std::filesystem::path &Path) {
  auto OpenOr = macho_loader::openMachOFile(Path);
  if (!OpenOr)
    return OpenOr.takeError();

  auto Buf = std::move(OpenOr->first);
  auto MachOObj = std::move(OpenOr->second);
  const auto &Obj = *MachOObj;

  bool Is64 = Obj.is64Bit();
  bool IsLE = Obj.isLittleEndian();
  if (!IsLE)
    return llvm::make_error<llvm::StringError>(
        "macho: big-endian not supported", llvm::inconvertibleErrorCode());

  BinaryImage Img;
  Img.Format = BinaryFormat::MachO;
  Img.IsRelocatable = Obj.getHeader().filetype == MH_OBJECT;
  llvm::StringRef ObjBytes = Obj.getData();
  Img.Raw.assign(reinterpret_cast<const uint8_t *>(ObjBytes.data()),
                 reinterpret_cast<const uint8_t *>(ObjBytes.data()) +
                     ObjBytes.size());
  (void)Buf;

  uint32_t CpuType = Is64 ? Obj.getHeader64().cputype : Obj.getHeader().cputype;
  Img.Arch = cpuTypeToArch(CpuType, Is64);
  Img.Bits = Is64 ? Bitness::Bits64 : Bitness::Bits32;
  if (Img.Arch == Arch::Unknown)
    return llvm::make_error<llvm::StringError>(
        "macho: unsupported cpu type 0x" + llvm::utohexstr(CpuType),
        llvm::inconvertibleErrorCode());

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
                        uint64_t FileOff, uint64_t FileSz, uint32_t Prot) {
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
                 SegCmd.filesize, SegCmd.initprot);
      for (uint32_t SI = 0; SI < SegCmd.nsects; ++SI) {
        auto S = Obj.getSection64(LC, SI);
        AddSection(S.sectname, S.segname, S.addr, S.size, S.offset, S.align,
                   S.flags, S.reserved1, S.reserved2, SegCmd.initprot);
      }
    }

    if (LC.C.cmd == LC_SEGMENT && !Is64) {
      auto SegCmd = Obj.getSegmentLoadCommand(LC);
      AddSegment(SegCmd.segname, SegCmd.vmaddr, SegCmd.vmsize, SegCmd.fileoff,
                 SegCmd.filesize, SegCmd.initprot);
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

    if (Img.Arch == Arch::ARM && IsSect)
      SymAddr = clearThumbBit(SymAddr);

    Symbol Sym;
    Sym.Name = NameOrErr->str();
    Sym.Addr = SymAddr;
    Sym.IsFunc = IsSect && Img.Sections[NSect - 1].isExecutable();
    Img.Symbols.push_back(Sym);

    if (Sym.IsFunc) {
      Export Exp;
      Exp.Name = Sym.Name;
      Exp.Addr = SymAddr;
      Img.Exports.push_back(std::move(Exp));
    }
  }

  // --- Apply relocations for .o files ---
  if (Obj.getHeader().filetype == MH_OBJECT)
    macho_loader::applyObjectRelocations(Obj, Img);

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
  macho_loader::parseChainedFixupsImports(BasePtr, FileSize, ChainedFixups,
                                          Img);
  macho_loader::parseChainedFixupsRebases(BasePtr, FileSize, ChainedFixups,
                                          TextVMAddr, Img);
  macho_loader::parseRuntimeFunctionSections(Sections, TextVMAddr, Img);
  macho_loader::parseExportTrie(BasePtr, FileSize, DyldInfo, TextVMAddr, Img);
  macho_loader::parseUUID(Obj, Img);
  macho_loader::parseBuildVersion(Obj, Img);

  runPostLoadDiscovery(Img, "macho: loaded " + Path.filename().string());
  // Classified before any table is read: a compact-unwind entry names a
  // personality slot, not a language, so what its LSDA means is settled by the
  // image's symbols and sections rather than by the entry.
  Img.ExceptionMetadata.Runtime = detectLanguageRuntime(Img);
  // Personality routines are reached through __got slots bound at load time,
  // so language-table decoding waits until stubs and bindings are known.
  macho_unwind::parseDarwinExceptions(Img);
  go_loader::parseGoExceptions(Img);
  // Rust shares the Itanium tables above rather than emitting its own, so its
  // reading of them runs last, over records that are already normalized.
  rust_eh::parseRustExceptions(Img);
  return Img;
}

} // namespace neverd
