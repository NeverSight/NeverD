#include "MachOLocalFunction.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>

namespace neverd {
bool isMachOLocalFunctionRange(const BinaryImage &Image, va_t Entry,
                               uint64_t Size) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 || !Size ||
      Entry > InvalidVA - Size || Image.Segments.size() > 64)
    return false;
  const auto Inside = [&](va_t Address) {
    return Address >= Entry && Address - Entry < Size;
  };
  // Reuse the format parser, rather than interpreting linkage from the
  // loader's compatibility name inventory. All bytes the parser can access
  // must come from complete, disjoint, contiguous file-backed segments.
  std::vector<const Segment *> Segments;
  for (const auto &Segment : Image.Segments)
    if (Segment.FileSz)
      Segments.push_back(&Segment);
  std::sort(Segments.begin(), Segments.end(), [](auto Left, auto Right) {
    return Left->FileOff < Right->FileOff;
  });
  uint64_t FileSize = 0;
  for (const auto *Segment : Segments) {
    if (Segment->FileOff != FileSize ||
        Segment->FileSz > Segment->Data.size() ||
        Segment->FileSz > 64 * 1024 * 1024 - FileSize)
      return false;
    FileSize += Segment->FileSz;
  }
  if (FileSize < sizeof(llvm::MachO::mach_header_64) || Segments.empty())
    return false;
  std::vector<uint8_t> Bytes(FileSize);
  for (const auto *Segment : Segments)
    std::copy_n(Segment->Data.begin(), Segment->FileSz,
                Bytes.begin() + Segment->FileOff);
  const llvm::StringRef Contents(reinterpret_cast<const char *>(Bytes.data()),
                                 Bytes.size());
  auto Parsed = llvm::object::createBinary(
      llvm::MemoryBufferRef(Contents, "<current-mach-o-image>"));
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return false;
  }
  const auto *Object =
      llvm::dyn_cast<llvm::object::MachOObjectFile>(Parsed->get());
  if (!Object || !Object->is64Bit() || !Object->isLittleEndian() ||
      Object->getHeader64().cputype != llvm::MachO::CPU_TYPE_ARM64 ||
      (Object->getHeader64().filetype != llvm::MachO::MH_EXECUTE &&
       Object->getHeader64().filetype != llvm::MachO::MH_DYLIB))
    return false;
  unsigned SymbolTables = 0;
  unsigned ExportTries = 0;
  size_t SegmentIndex = 0;
  for (const auto &Command : Object->load_commands()) {
    if (Command.C.cmd == llvm::MachO::LC_SEGMENT_64) {
      const auto Record = Object->getSegment64LoadCommand(Command);
      if (SegmentIndex >= Image.Segments.size())
        return false;
      const auto &Mapped = Image.Segments[SegmentIndex++];
      if (Record.vmaddr != Mapped.VA || Record.vmsize != Mapped.Size ||
          Record.fileoff != Mapped.FileOff ||
          Record.filesize != Mapped.FileSz ||
          bool(Record.initprot & llvm::MachO::VM_PROT_READ) !=
              Mapped.isReadable() ||
          bool(Record.initprot & llvm::MachO::VM_PROT_WRITE) !=
              Mapped.isWritable() ||
          bool(Record.initprot & llvm::MachO::VM_PROT_EXECUTE) !=
              Mapped.isExecutable() ||
          Record.filesize > Record.vmsize)
        return false;
    }
    SymbolTables += Command.C.cmd == llvm::MachO::LC_SYMTAB;
    ExportTries += Command.C.cmd == llvm::MachO::LC_DYLD_EXPORTS_TRIE;
    if (Command.C.cmd == llvm::MachO::LC_DYLD_INFO ||
        Command.C.cmd == llvm::MachO::LC_DYLD_INFO_ONLY)
      ExportTries += Object->getDyldInfoLoadCommand(Command).export_size != 0;
  }
  if (SymbolTables != 1 || ExportTries > 1 ||
      SegmentIndex != Image.Segments.size())
    return false;
  size_t SectionIndex = 0;
  for (const auto &Section : Object->sections()) {
    if (SectionIndex >= Image.Sections.size())
      return false;
    const auto Record = Object->getSection64(Section.getRawDataRefImpl());
    const auto &Mapped = Image.Sections[SectionIndex++];
    const auto Kind = Record.flags & llvm::MachO::SECTION_TYPE;
    const bool ZeroFill = Kind == llvm::MachO::S_ZEROFILL ||
                          Kind == llvm::MachO::S_GB_ZEROFILL ||
                          Kind == llvm::MachO::S_THREAD_LOCAL_ZEROFILL;
    if (Record.addr != Mapped.VA || Record.size != Mapped.Size ||
        Record.offset != Mapped.FileOff || Record.flags != Mapped.Type ||
        (ZeroFill ? 0 : Record.size) != Mapped.FileSz)
      return false;
  }
  if (SectionIndex != Image.Sections.size())
    return false;
  unsigned Matches = 0;
  for (const auto &Symbol : Object->symbols()) {
    const auto Record =
        Object->getSymbol64TableEntry(Symbol.getRawDataRefImpl());
    if (Record.n_type & llvm::MachO::N_STAB)
      continue;
    if (!Inside(Record.n_value))
      continue;
    if (Record.n_value != Entry || Record.n_type != llvm::MachO::N_SECT ||
        !Record.n_sect || Record.n_sect > Image.Sections.size() ||
        ++Matches != 1)
      return false;
    const auto &Section = Image.Sections[Record.n_sect - 1];
    if (!Section.isExecutable() || !Section.contains(Entry) ||
        Size > Section.Size - (Entry - Section.VA))
      return false;
    auto Name = Symbol.getName();
    if (!Name) {
      llvm::consumeError(Name.takeError());
      return false;
    }
    if (Name->empty())
      return false;
  }
  if (Matches != 1)
    return false;
  bool Exported = false;
  size_t Budget = 1024 * 1024;
  llvm::Error Error = llvm::Error::success();
  for (const auto &Export : Object->exports(Error)) {
    if (!Budget--) {
      Exported = true;
      break;
    }
    const auto Flags = Export.flags();
    if ((Flags & ~uint64_t(0x1f)) ||
        (Flags & llvm::MachO::EXPORT_SYMBOL_FLAGS_KIND_MASK) == 3) {
      Exported = true;
      break;
    }
    if (Flags & llvm::MachO::EXPORT_SYMBOL_FLAGS_REEXPORT)
      continue;
    const va_t Base = (Flags & llvm::MachO::EXPORT_SYMBOL_FLAGS_KIND_MASK) ==
                              llvm::MachO::EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE
                          ? 0
                          : Segments.front()->VA;
    if (Export.address() > InvalidVA - Base || Inside(Base + Export.address()))
      Exported = true;
    if (Flags & llvm::MachO::EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER)
      if (Export.other() > InvalidVA - Base || Inside(Base + Export.other()))
        Exported = true;
  }
  if (Error) {
    llvm::consumeError(std::move(Error));
    return false;
  }
  return !Exported;
}
} // namespace neverd
