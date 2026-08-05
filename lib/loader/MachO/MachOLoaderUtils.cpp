//===- MachOLoaderUtils.cpp - Mach-O loader helpers ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "neverd/Object/MachOLayout.h"
#include "neverd/Support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <set>

#define DEBUG_TYPE "neverd-macho-loader"

namespace neverd {
namespace macho_loader {

using namespace llvm::MachO;

namespace {

/// LC_UNIXTHREAD: load_command header + {flavor, count} + register state.
struct UnixThreadFlavorHeader {
  uint32_t Flavor;
  uint32_t Count;
};
constexpr uint32_t kUnixThreadStateOffset =
    sizeof(llvm::MachO::load_command) + sizeof(UnixThreadFlavorHeader);

va_t readUnixThreadEntry(const uint8_t *StatePtr, size_t StateBytes, Arch A) {
  if (A == Arch::X86 && StateBytes >= sizeof(x86_thread_state32_t)) {
    auto *TS = reinterpret_cast<const x86_thread_state32_t *>(StatePtr);
    return TS->eip;
  }
  if (A == Arch::X64 && StateBytes >= sizeof(x86_thread_state64_t)) {
    auto *TS = reinterpret_cast<const x86_thread_state64_t *>(StatePtr);
    return TS->rip;
  }
  if (A == Arch::ARM && StateBytes >= sizeof(arm_thread_state32_t)) {
    auto *TS = reinterpret_cast<const arm_thread_state32_t *>(StatePtr);
    return clearThumbBit(TS->pc);
  }
  if (A == Arch::AArch64 && StateBytes >= sizeof(arm_thread_state64_t)) {
    auto *TS = reinterpret_cast<const arm_thread_state64_t *>(StatePtr);
    return TS->pc;
  }
  return 0;
}

} // anonymous namespace

llvm::Expected<std::pair<std::unique_ptr<llvm::MemoryBuffer>,
                         std::unique_ptr<llvm::object::MachOObjectFile>>>
openMachOFile(const std::filesystem::path &Path) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path.string());
  if (!BufOrErr)
    return llvm::make_error<llvm::StringError>(
        "macho: cannot open " + Path.string(), llvm::inconvertibleErrorCode());

  auto Buf = std::move(*BufOrErr);
  auto BinaryOr = llvm::object::createBinary(Buf->getMemBufferRef());
  if (!BinaryOr)
    return BinaryOr.takeError();

  std::unique_ptr<llvm::object::Binary> Binary = std::move(*BinaryOr);

  if (auto *Universal =
          llvm::dyn_cast<llvm::object::MachOUniversalBinary>(Binary.get())) {
    llvm::Triple Host(llvm::sys::getProcessTriple());
    std::string ArchName = Host.getArchName().str();
    auto ObjOr = Universal->getMachOObjectForArch(ArchName);
    if (!ObjOr) {
      llvm::consumeError(ObjOr.takeError());
      for (const auto &Slice : Universal->objects()) {
        ObjOr = Slice.getAsObjectFile();
        if (ObjOr)
          break;
        llvm::consumeError(ObjOr.takeError());
      }
    }
    if (!ObjOr)
      return llvm::make_error<llvm::StringError>(
          "macho: no slice in universal binary for host arch",
          llvm::inconvertibleErrorCode());
    return std::make_pair(std::move(Buf), std::move(*ObjOr));
  }

  if (auto *Obj = llvm::dyn_cast<llvm::object::MachOObjectFile>(Binary.get())) {
    auto ObjCopy = llvm::object::MachOObjectFile::create(
        Obj->getMemoryBufferRef(), Obj->isLittleEndian(), Obj->is64Bit());
    if (!ObjCopy)
      return ObjCopy.takeError();
    return std::make_pair(std::move(Buf), std::move(*ObjCopy));
  }

  return llvm::make_error<llvm::StringError>("macho: not a Mach-O file",
                                             llvm::inconvertibleErrorCode());
}

void parseDyldInfoLoadCommands(const llvm::object::MachOObjectFile &Obj,
                               DyldInfoOffsets &Out) {
  for (const auto &LC : Obj.load_commands()) {
    if ((LC.C.cmd != LC_DYLD_INFO && LC.C.cmd != LC_DYLD_INFO_ONLY) ||
        LC.C.cmdsize < sizeof(dyld_info_command))
      continue;
    auto DI = *reinterpret_cast<const dyld_info_command *>(LC.Ptr);
    Out.BindOff = DI.bind_off;
    Out.BindSize = DI.bind_size;
    Out.LazyBindOff = DI.lazy_bind_off;
    Out.LazyBindSize = DI.lazy_bind_size;
    Out.ExportOff = DI.export_off;
    Out.ExportSize = DI.export_size;
    return;
  }
}

void parseNeededLibraries(const llvm::object::MachOObjectFile &Obj,
                          BinaryImage &Img) {
  for (const auto &LC : Obj.load_commands()) {
    if ((LC.C.cmd != LC_LOAD_DYLIB && LC.C.cmd != LC_LOAD_WEAK_DYLIB &&
         LC.C.cmd != LC_REEXPORT_DYLIB && LC.C.cmd != LC_LAZY_LOAD_DYLIB) ||
        LC.C.cmdsize < sizeof(dylib_command))
      continue;

    auto DylibCmd = *reinterpret_cast<const dylib_command *>(LC.Ptr);
    uint32_t NameOff = DylibCmd.dylib.name;
    if (NameOff >= LC.C.cmdsize)
      continue;

    const char *N = reinterpret_cast<const char *>(LC.Ptr) + NameOff;
    size_t MaxLen = LC.C.cmdsize - NameOff;
    std::string DylibName = readFixedName(N, MaxLen);
    if (std::find(Img.DynInfo.NeededLibs.begin(), Img.DynInfo.NeededLibs.end(),
                  DylibName) == Img.DynInfo.NeededLibs.end())
      Img.DynInfo.NeededLibs.push_back(std::move(DylibName));
  }
}

void parseFunctionStarts(const uint8_t *BasePtr, size_t FileSize,
                         const FunctionStartsInfo &Info, uint64_t TextVMAddr,
                         BinaryImage &Img) {
  if (Info.DataOff == 0 || Info.DataSize == 0 ||
      !rangeInBounds(Info.DataOff, Info.DataSize, FileSize) || TextVMAddr == 0)
    return;

  auto Existing = Img.getSymbolAddresses();
  const uint8_t *P = BasePtr + Info.DataOff;
  const uint8_t *End = P + Info.DataSize;
  uint64_t Addr = TextVMAddr;
  [[maybe_unused]] size_t Added = 0;

  while (P < End) {
    unsigned BytesRead = 0;
    const char *Error = nullptr;
    uint64_t Delta = llvm::decodeULEB128(P, &BytesRead, End, &Error);
    if (Error || BytesRead == 0)
      break;
    P += BytesRead;
    if (Delta == 0)
      break;
    if (Delta > InvalidVA - Addr)
      break;
    Addr += Delta;

    va_t FuncAddr = Addr;
    if (Img.Arch == Arch::ARM)
      FuncAddr = clearThumbBit(FuncAddr);

    if (Existing.insert(FuncAddr).second) {
      Img.Symbols.push_back(Symbol::makeFunc(FuncAddr));
      ++Added;
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "macho: LC_FUNCTION_STARTS added " << Added
                          << " functions\n");
}

void parseEntryPoint(const llvm::object::MachOObjectFile &Obj, BinaryImage &Img,
                     uint64_t TextVMAddr) {
  for (const auto &LC : Obj.load_commands()) {
    if (LC.C.cmd == LC_MAIN && LC.C.cmdsize >= sizeof(entry_point_command)) {
      auto EP = *reinterpret_cast<const entry_point_command *>(LC.Ptr);
      if (EP.entryoff > InvalidVA - TextVMAddr)
        continue;
      Img.Entry = TextVMAddr + EP.entryoff;
      if (Img.Arch == Arch::ARM)
        Img.Entry = clearThumbBit(Img.Entry);
      return;
    }

    if (LC.C.cmd != LC_UNIXTHREAD || Img.Entry != 0)
      continue;
    if (LC.C.cmdsize <= kUnixThreadStateOffset)
      continue;

    const uint8_t *StatePtr =
        reinterpret_cast<const uint8_t *>(LC.Ptr) + kUnixThreadStateOffset;
    size_t StateBytes = LC.C.cmdsize - kUnixThreadStateOffset;
    va_t Entry = readUnixThreadEntry(StatePtr, StateBytes, Img.Arch);
    if (Entry != 0)
      Img.Entry = Entry;
  }
}

void parseBindStreams(const uint8_t *BasePtr, size_t FileSize,
                      const DyldInfoOffsets &DyldInfo, BinaryImage &Img) {
  uint32_t BindOff = DyldInfo.BindOff;
  uint32_t BindSize = DyldInfo.BindSize;
  uint32_t LazyBindOff = DyldInfo.LazyBindOff;
  uint32_t LazyBindSize = DyldInfo.LazyBindSize;
  std::map<std::string, size_t> ImportIndex;
  for (size_t I = 0; I < Img.Imports.size(); ++I)
    ImportIndex[Img.Imports[I].Name] = I;

  auto ParseBindStream = [&](uint32_t Off, uint32_t Sz) {
    if (Off == 0 || Sz == 0 || !rangeInBounds(Off, Sz, FileSize))
      return;
    const uint8_t *P = BasePtr + Off;
    const uint8_t *End = P + Sz;

    std::string SymName;
    int64_t LibOrdinal = 0;
    uint8_t SegIdx = 0;
    uint64_t SegOff = 0;

    auto ReadULEB = [&](uint64_t &Val) -> bool {
      if (P >= End)
        return false;
      unsigned BytesRead = 0;
      const char *Error = nullptr;
      Val = llvm::decodeULEB128(P, &BytesRead, End, &Error);
      if (Error || BytesRead == 0)
        return false;
      P += BytesRead;
      return true;
    };
    auto ReadSLEB = [&](int64_t &Val) -> bool {
      if (P >= End)
        return false;
      unsigned BytesRead = 0;
      const char *Error = nullptr;
      Val = llvm::decodeSLEB128(P, &BytesRead, End, &Error);
      if (Error || BytesRead == 0)
        return false;
      P += BytesRead;
      return true;
    };

    while (P < End) {
      uint8_t Byte = *P++;
      uint8_t Opcode = Byte & BIND_OPCODE_MASK;
      uint8_t Imm = Byte & BIND_IMMEDIATE_MASK;

      switch (Opcode) {
      case BIND_OPCODE_DONE:
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
        LibOrdinal = Imm;
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: {
        uint64_t Ordinal = 0;
        if (!ReadULEB(Ordinal) ||
            Ordinal > static_cast<uint64_t>(
                          std::numeric_limits<int64_t>::max()))
          return;
        LibOrdinal = static_cast<int64_t>(Ordinal);
        break;
      }
      case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
        if (Imm == 0)
          LibOrdinal = 0;
        else
          LibOrdinal = static_cast<int8_t>(BIND_OPCODE_MASK | Imm);
        break;
      case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
        size_t MaxLen = static_cast<size_t>(End - P);
        const void *Term = std::memchr(P, 0, MaxLen);
        if (!Term) {
          P = End;
          break;
        }
        const auto *TermPtr = static_cast<const uint8_t *>(Term);
        SymName.assign(reinterpret_cast<const char *>(P),
                       static_cast<size_t>(TermPtr - P));
        P = TermPtr + 1;
        break;
      }
      case BIND_OPCODE_SET_TYPE_IMM:
        break;
      case BIND_OPCODE_SET_ADDEND_SLEB: {
        [[maybe_unused]] int64_t Addend = 0;
        if (!ReadSLEB(Addend))
          return;
        break;
      }
      case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: {
        SegIdx = Imm;
        if (!ReadULEB(SegOff))
          return;
        break;
      }
      case BIND_OPCODE_ADD_ADDR_ULEB: {
        uint64_t Delta = 0;
        if (!ReadULEB(Delta) || Delta > InvalidVA - SegOff)
          return;
        SegOff += Delta;
        break;
      }
      case BIND_OPCODE_DO_BIND:
      case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
      case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
      case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
        uint64_t Count = 1;
        uint64_t Skip = 0;
        if (Opcode == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB) {
          if (!ReadULEB(Count) || !ReadULEB(Skip))
            return;
          if (Count == 0)
            break;
        }

        va_t BindAddr = 0;
        bool HasBindAddr = false;
        if (SegIdx < Img.Segments.size()) {
          const Segment &Seg = Img.Segments[SegIdx];
          if (SegOff < Seg.Size && SegOff <= InvalidVA - Seg.VA) {
            BindAddr = Seg.VA + SegOff;
            HasBindAddr = true;
          }
        }

        if (!SymName.empty() && HasBindAddr) {
          std::string DylibName;
          if (LibOrdinal > 0 &&
              static_cast<size_t>(LibOrdinal) <= Img.DynInfo.NeededLibs.size())
            DylibName =
                Img.DynInfo.NeededLibs[static_cast<size_t>(LibOrdinal - 1)];

          auto It = ImportIndex.find(SymName);
          if (It != ImportIndex.end()) {
            if (!DylibName.empty())
              Img.Imports[It->second].Module = DylibName;
          } else {
            Import Imp;
            Imp.Name = SymName;
            Imp.Module = DylibName.empty() ? kExternModule.str() : DylibName;
            Imp.IATAddr = BindAddr;
            ImportIndex[SymName] = Img.Imports.size();
            Img.Imports.push_back(std::move(Imp));
          }
        }

        uint32_t PtrSz = Img.getPointerSize();
        if (PtrSz > InvalidVA - SegOff)
          return;
        SegOff += PtrSz;
        if (Opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB) {
          uint64_t Delta = 0;
          if (!ReadULEB(Delta) || Delta > InvalidVA - SegOff)
            return;
          SegOff += Delta;
        } else if (Opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED) {
          uint64_t Delta = static_cast<uint64_t>(Imm) * PtrSz;
          if (Delta > InvalidVA - SegOff)
            return;
          SegOff += Delta;
        }
        else if (Opcode == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB) {
          if (Skip > InvalidVA - PtrSz)
            return;
          uint64_t Stride = PtrSz + Skip;
          if (Skip > InvalidVA - SegOff)
            return;
          SegOff += Skip;
          uint64_t Repeats = Count - 1;
          if (Repeats != 0 &&
              Stride > (InvalidVA - SegOff) / Repeats)
            return;
          SegOff += Repeats * Stride;
        }
        break;
      }
      default:
        break;
      }
    }
  };

  ParseBindStream(BindOff, BindSize);
  ParseBindStream(LazyBindOff, LazyBindSize);
}

void parseExportTrie(const uint8_t *BasePtr, size_t FileSize,
                     const DyldInfoOffsets &DyldInfo, uint64_t TextVMAddr,
                     BinaryImage &Img) {
  uint32_t ExportOff = DyldInfo.ExportOff;
  uint32_t ExportSize = DyldInfo.ExportSize;
  if (ExportOff == 0 || ExportSize == 0 ||
      !rangeInBounds(ExportOff, ExportSize, FileSize))
    return;

  const uint8_t *TrieBase = BasePtr + ExportOff;
  size_t TrieSize = ExportSize;

  std::set<va_t> ExistingExports;
  for (const auto &E : Img.Exports)
    ExistingExports.insert(E.Addr);

  struct TrieEntry {
    size_t Offset;
    std::string Prefix;
  };
  std::vector<TrieEntry> Stack;
  Stack.push_back({0, ""});
  std::set<size_t> SeenNodes{0};
  [[maybe_unused]] size_t Added = 0;

  auto ReadULEB = [](const uint8_t *&Cursor, const uint8_t *Limit,
                     uint64_t &Value) {
    if (Cursor >= Limit)
      return false;
    unsigned BytesRead = 0;
    const char *Error = nullptr;
    Value = llvm::decodeULEB128(Cursor, &BytesRead, Limit, &Error);
    if (Error || BytesRead == 0)
      return false;
    Cursor += BytesRead;
    return true;
  };

  while (!Stack.empty()) {
    auto [NodeOff, Prefix] = Stack.back();
    Stack.pop_back();
    if (NodeOff >= TrieSize)
      continue;

    const uint8_t *P = TrieBase + NodeOff;
    const uint8_t *End = TrieBase + TrieSize;

    uint64_t TermSize = 0;
    if (!ReadULEB(P, End, TermSize) ||
        TermSize > static_cast<uint64_t>(End - P))
      continue;
    const uint8_t *TermEnd = P + static_cast<size_t>(TermSize);
    if (TermSize > 0 && !Prefix.empty()) {
      const uint8_t *TermP = P;
      uint64_t Flags = 0;
      uint64_t Addr = 0;
      if (!ReadULEB(TermP, TermEnd, Flags))
        continue;

      // Re-exports encode a library ordinal and import name after the flags,
      // not an address relative to __TEXT.
      if ((Flags & EXPORT_SYMBOL_FLAGS_REEXPORT) == 0) {
        if (!ReadULEB(TermP, TermEnd, Addr) ||
            Addr > InvalidVA - TextVMAddr)
          continue;
        va_t ExportAddr = TextVMAddr + Addr;
        if (ExistingExports.insert(ExportAddr).second) {
          Export Exp;
          Exp.Name = Prefix;
          Exp.Addr = ExportAddr;
          Img.Exports.push_back(std::move(Exp));
          ++Added;
        }
      }
    }
    P = TermEnd;
    if (P >= End)
      continue;

    uint8_t ChildCount = *P++;
    for (uint8_t C = 0; C < ChildCount; ++C) {
      size_t MaxLen = static_cast<size_t>(End - P);
      const void *Term = std::memchr(P, 0, MaxLen);
      if (!Term)
        break;
      const auto *TermPtr = static_cast<const uint8_t *>(Term);
      std::string EdgeLabel(reinterpret_cast<const char *>(P),
                            static_cast<size_t>(TermPtr - P));
      P = TermPtr + 1;

      uint64_t ChildOff = 0;
      if (!ReadULEB(P, End, ChildOff))
        break;
      if (ChildOff < TrieSize &&
          SeenNodes.insert(static_cast<size_t>(ChildOff)).second)
        Stack.push_back(
            {static_cast<size_t>(ChildOff), Prefix + EdgeLabel});
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "macho: export trie added " << Added
                          << " exports\n");
}

void parseStubImports(const llvm::object::MachOObjectFile &Obj,
                      const std::vector<SectionInfo> &Sections,
                      const uint8_t *BasePtr, size_t FileSize, bool Is64,
                      BinaryImage &Img) {
  symtab_command SymtabCmd = Obj.getSymtabLoadCommand();
  dysymtab_command DysymtabCmd = Obj.getDysymtabLoadCommand();
  const size_t NListSize = getMachONListSize(Is64);

  if (SymtabCmd.nsyms == 0 || DysymtabCmd.nindirectsyms == 0)
    return;
  if (!rangeInBounds(SymtabCmd.stroff, SymtabCmd.strsize, FileSize) ||
      !rangeInBounds(SymtabCmd.symoff,
                     static_cast<uint64_t>(SymtabCmd.nsyms) * NListSize,
                     FileSize))
    return;

  const char *StrTab =
      reinterpret_cast<const char *>(BasePtr + SymtabCmd.stroff);

  auto GetSymName = [&](uint32_t SymIdx) -> std::string {
    size_t EntryOff = SymtabCmd.symoff + SymIdx * NListSize;
    if (!rangeInBounds(EntryOff, NListSize, FileSize))
      return {};
    uint32_t NStrx;
    if (Is64) {
      auto *NL = reinterpret_cast<const nlist_64 *>(BasePtr + EntryOff);
      NStrx = NL->n_strx;
    } else {
      auto *NL = reinterpret_cast<const nlist *>(BasePtr + EntryOff);
      NStrx = NL->n_strx;
    }
    if (NStrx == 0 || NStrx >= SymtabCmd.strsize)
      return {};
    return readFixedName(StrTab + NStrx, SymtabCmd.strsize - NStrx);
  };

  for (const auto &Sect : Sections) {
    if (Sect.Flags != S_SYMBOL_STUBS || Sect.StubSize == 0)
      continue;

    uint32_t NStubs = static_cast<uint32_t>(Sect.Size / Sect.StubSize);
    uint32_t IndirectBase = Sect.Reserved1;

    for (uint32_t SI = 0; SI < NStubs; ++SI) {
      uint32_t ISymIdx = IndirectBase + SI;
      if (ISymIdx >= DysymtabCmd.nindirectsyms)
        continue;

      uint32_t SymIdx = Obj.getIndirectSymbolTableEntry(DysymtabCmd, ISymIdx);
      if (SymIdx == INDIRECT_SYMBOL_LOCAL || SymIdx == INDIRECT_SYMBOL_ABS ||
          SymIdx >= SymtabCmd.nsyms)
        continue;

      std::string SymName = GetSymName(SymIdx);
      if (SymName.empty())
        continue;

      va_t StubAddr = Sect.Addr + SI * Sect.StubSize;

      Import Imp;
      Imp.Name = SymName;
      Imp.IATAddr = StubAddr;
      Img.Imports.push_back(std::move(Imp));

      Symbol Sym = Symbol::makeFunc(StubAddr);
      Sym.Name = SymName;
      Img.Symbols.push_back(std::move(Sym));

      LLVM_DEBUG(llvm::dbgs() << "macho: stub 0x" << llvm::utohexstr(StubAddr)
                              << " -> " << SymName << "\n");
    }
  }
}

void parseNonLazyPtrImports(const llvm::object::MachOObjectFile &Obj,
                            const std::vector<SectionInfo> &Sections,
                            const uint8_t *BasePtr, size_t FileSize, bool Is64,
                            BinaryImage &Img) {
  using namespace llvm::MachO;
  symtab_command SymtabCmd = Obj.getSymtabLoadCommand();
  dysymtab_command DysymtabCmd = Obj.getDysymtabLoadCommand();
  const size_t NListSize = getMachONListSize(Is64);

  if (SymtabCmd.nsyms == 0 || DysymtabCmd.nindirectsyms == 0)
    return;
  if (!rangeInBounds(SymtabCmd.stroff, SymtabCmd.strsize, FileSize) ||
      !rangeInBounds(SymtabCmd.symoff,
                     static_cast<uint64_t>(SymtabCmd.nsyms) * NListSize,
                     FileSize))
    return;

  const char *StrTab =
      reinterpret_cast<const char *>(BasePtr + SymtabCmd.stroff);

  auto GetSymName = [&](uint32_t SymIdx) -> std::string {
    size_t EntryOff = SymtabCmd.symoff + SymIdx * NListSize;
    if (!rangeInBounds(EntryOff, NListSize, FileSize))
      return {};
    uint32_t NStrx;
    if (Is64) {
      auto *NL = reinterpret_cast<const nlist_64 *>(BasePtr + EntryOff);
      NStrx = NL->n_strx;
    } else {
      auto *NL = reinterpret_cast<const nlist *>(BasePtr + EntryOff);
      NStrx = NL->n_strx;
    }
    if (NStrx == 0 || NStrx >= SymtabCmd.strsize)
      return {};
    return readFixedName(StrTab + NStrx, SymtabCmd.strsize - NStrx);
  };

  const uint64_t PtrSize = Is64 ? 8 : 4;
  for (const auto &Sect : Sections) {
    if (Sect.Flags != S_NON_LAZY_SYMBOL_POINTERS &&
        Sect.Flags != S_LAZY_SYMBOL_POINTERS)
      continue;

    uint64_t NSlots = Sect.Size / PtrSize;
    uint32_t IndirectBase = Sect.Reserved1;

    for (uint64_t SI = 0; SI < NSlots; ++SI) {
      uint32_t ISymIdx = IndirectBase + static_cast<uint32_t>(SI);
      if (ISymIdx >= DysymtabCmd.nindirectsyms)
        continue;

      uint32_t SymIdx = Obj.getIndirectSymbolTableEntry(DysymtabCmd, ISymIdx);
      if (SymIdx == INDIRECT_SYMBOL_LOCAL || SymIdx == INDIRECT_SYMBOL_ABS ||
          SymIdx >= SymtabCmd.nsyms)
        continue;

      std::string SymName = GetSymName(SymIdx);
      if (SymName.empty())
        continue;

      va_t SlotAddr = Sect.Addr + SI * PtrSize;
      Img.ImportPtrSlots[SlotAddr] = SymName;

      LLVM_DEBUG(llvm::dbgs()
                 << "macho: ptr-slot 0x" << llvm::utohexstr(SlotAddr) << " -> "
                 << SymName << "\n");
    }
  }
}

void parseChainedFixupsImports(const uint8_t *BasePtr, size_t FileSize,
                               const ChainedFixupsInfo &Info,
                               BinaryImage &Img) {
  using namespace llvm::MachO;
  uint32_t DataOff = Info.DataOff;
  uint32_t DataSize = Info.DataSize;
  if (DataOff == 0 || DataSize < sizeof(dyld_chained_fixups_header) ||
      !rangeInBounds(DataOff, DataSize, FileSize))
    return;

  const auto *Hdr =
      reinterpret_cast<const dyld_chained_fixups_header *>(BasePtr + DataOff);
  if (Hdr->imports_count == 0 || Hdr->imports_offset == 0 ||
      Hdr->symbols_offset == 0)
    return;

  // imports_offset/symbols_offset are untrusted: validate them against DataSize
  // in 64-bit so the derived absolute offsets cannot wrap and StrSize (the gap
  // to the blob end) cannot underflow into a huge length.
  if (Hdr->imports_offset >= DataSize || Hdr->symbols_offset >= DataSize)
    return;
  uint64_t ImportsAbs = static_cast<uint64_t>(DataOff) + Hdr->imports_offset;
  uint64_t SymbolsAbs = static_cast<uint64_t>(DataOff) + Hdr->symbols_offset;
  uint64_t SymbolsEnd = static_cast<uint64_t>(DataOff) + DataSize;

  if (ImportsAbs >= FileSize || SymbolsAbs >= FileSize)
    return;

  const char *StrTab = reinterpret_cast<const char *>(BasePtr + SymbolsAbs);
  size_t StrSize = static_cast<size_t>(SymbolsEnd - SymbolsAbs);

  std::set<std::string> SeenNames;
  for (const auto &Imp : Img.Imports)
    SeenNames.insert(Imp.Name);

  for (uint32_t I = 0; I < Hdr->imports_count; ++I) {
    uint32_t NameOff = 0;
    int32_t LibOrdinal = 0;

    switch (Hdr->imports_format) {
    case DYLD_CHAINED_IMPORT: {
      uint64_t EntOff =
          ImportsAbs + static_cast<uint64_t>(I) * sizeof(dyld_chained_import);
      if (!rangeInBounds(EntOff, sizeof(dyld_chained_import), SymbolsEnd))
        return;
      dyld_chained_import E;
      std::memcpy(&E, BasePtr + static_cast<size_t>(EntOff), sizeof(E));
      NameOff = E.name_offset;
      LibOrdinal = static_cast<int8_t>(E.lib_ordinal);
      break;
    }
    case DYLD_CHAINED_IMPORT_ADDEND: {
      uint64_t EntOff = ImportsAbs + static_cast<uint64_t>(I) *
                                         sizeof(dyld_chained_import_addend);
      if (!rangeInBounds(EntOff, sizeof(dyld_chained_import_addend),
                         SymbolsEnd))
        return;
      dyld_chained_import_addend E;
      std::memcpy(&E, BasePtr + static_cast<size_t>(EntOff), sizeof(E));
      NameOff = E.name_offset;
      LibOrdinal = static_cast<int8_t>(E.lib_ordinal);
      break;
    }
    case DYLD_CHAINED_IMPORT_ADDEND64: {
      uint64_t EntOff = ImportsAbs + static_cast<uint64_t>(I) *
                                         sizeof(dyld_chained_import_addend64);
      if (!rangeInBounds(EntOff, sizeof(dyld_chained_import_addend64),
                         SymbolsEnd))
        return;
      dyld_chained_import_addend64 E;
      std::memcpy(&E, BasePtr + static_cast<size_t>(EntOff), sizeof(E));
      NameOff = E.name_offset;
      LibOrdinal = static_cast<int16_t>(E.lib_ordinal);
      break;
    }
    default:
      return;
    }

    if (NameOff >= StrSize)
      continue;
    std::string SymName = readFixedName(StrTab + NameOff, StrSize - NameOff);
    if (SymName.empty() || !SeenNames.insert(SymName).second)
      continue;

    std::string DylibName;
    if (LibOrdinal > 0 &&
        static_cast<size_t>(LibOrdinal) <= Img.DynInfo.NeededLibs.size())
      DylibName = Img.DynInfo.NeededLibs[static_cast<size_t>(LibOrdinal - 1)];

    Import Imp;
    Imp.Name = SymName;
    Imp.Module = DylibName.empty() ? kExternModule.str() : DylibName;
    Img.Imports.push_back(std::move(Imp));
  }
  LLVM_DEBUG(llvm::dbgs() << "macho: parsed " << Hdr->imports_count
                          << " chained fixups imports\n");
}

void parseChainedFixupsRebases(const uint8_t *BasePtr, size_t FileSize,
                               const ChainedFixupsInfo &Info, va_t TextVMAddr,
                               BinaryImage &Img) {
  using namespace llvm::MachO;
  uint32_t DataOff = Info.DataOff;
  uint32_t DataSize = Info.DataSize;
  if (DataOff == 0 || DataSize < sizeof(dyld_chained_fixups_header) ||
      !rangeInBounds(DataOff, DataSize, FileSize))
    return;
  // The chained-fixups blob is self-contained within [DataOff, DataEnd); every
  // sub-structure offset below is validated against this bound before deref.
  const uint64_t DataEnd = static_cast<uint64_t>(DataOff) + DataSize;

  const auto *Hdr =
      reinterpret_cast<const dyld_chained_fixups_header *>(BasePtr + DataOff);
  if (Hdr->starts_offset == 0)
    return;
  uint64_t StartsAbs = static_cast<uint64_t>(DataOff) + Hdr->starts_offset;
  if (!rangeInBounds(StartsAbs, sizeof(uint32_t), DataEnd))
    return;
  const auto *Starts = reinterpret_cast<const dyld_chained_starts_in_image *>(
      BasePtr + StartsAbs);
  uint32_t SegCount = Starts->seg_count;
  if (StartsAbs + sizeof(uint32_t) +
          static_cast<uint64_t>(SegCount) * sizeof(uint32_t) >
      DataEnd)
    return;

  // A rebase slot at \p SlotVA points at \p TargetVA; classify the target
  // segment exactly as the ELF loader does (executable → code-pointer table
  // entry; read-only non-exec data → data-pointer table entry).
  auto recordSlot = [&](va_t SlotVA, va_t TargetVA) {
    const Segment *TSeg = Img.getSegmentFor(TargetVA);
    if (!TSeg)
      return;
    if (TSeg->isExecutable())
      Img.CodePtrRelocSlots.insert(SlotVA);
    else if (TSeg->isReadable() && !TSeg->isWritable() && !TSeg->Data.empty())
      Img.DataPtrRelocSlots.insert(SlotVA);
  };

  size_t NumRecorded = 0;
  for (uint32_t S = 0; S < SegCount; ++S) {
    uint32_t SegInfoOff = Starts->seg_info_offset[S];
    if (SegInfoOff == 0)
      continue;
    uint64_t SegAbs = StartsAbs + SegInfoOff;
    if (!rangeInBounds(SegAbs, sizeof(dyld_chained_starts_in_segment),
                       DataEnd))
      continue;
    const auto *Seg = reinterpret_cast<const dyld_chained_starts_in_segment *>(
        BasePtr + SegAbs);
    if (Seg->size < offsetof(dyld_chained_starts_in_segment, page_start) ||
        !rangeInBounds(SegAbs, Seg->size, DataEnd))
      continue;
    uint64_t SegEnd = SegAbs + Seg->size;
    uint16_t PtrFormat = Seg->pointer_format;
    // Only the 64-bit pointer formats carry the rebase/bind bitfields decoded
    // below; arm64e (authenticated) and the 32-bit formats are skipped (they
    // never appear for the plain data-pointer tables we care about here).
    if (PtrFormat != DYLD_CHAINED_PTR_64 &&
        PtrFormat != DYLD_CHAINED_PTR_64_OFFSET)
      continue;
    uint32_t PageSize = Seg->page_size ? Seg->page_size : 0x1000;
    uint16_t PageCount = Seg->page_count;
    uint64_t PageStartArr =
        SegAbs + offsetof(dyld_chained_starts_in_segment, page_start);
    if (!rangeInBounds(
            PageStartArr,
            static_cast<uint64_t>(PageCount) * sizeof(uint16_t), SegEnd))
      continue;
    uint64_t SegVMOff = Seg->segment_offset;
    for (uint16_t P = 0; P < PageCount; ++P) {
      uint16_t Start = Seg->page_start[P];
      if (Start == DYLD_CHAINED_PTR_START_NONE)
        continue;
      // Pages with multiple chain starts (overlapping fixups) are rare for the
      // __DATA tables of interest; skip rather than mis-walk the overflow list.
      if (Start & DYLD_CHAINED_PTR_START_MULTI)
        continue;
      uint64_t PageOffset = static_cast<uint64_t>(P) * PageSize + Start;
      if (PageOffset > InvalidVA - SegVMOff)
        continue;
      uint64_t ChainOffset = SegVMOff + PageOffset;
      if (ChainOffset > InvalidVA - TextVMAddr)
        continue;
      va_t ChainVA = TextVMAddr + ChainOffset;
      // Bounded chain walk; a malformed `next` cannot loop forever.
      for (uint32_t Guard = 0; Guard < (1u << 22); ++Guard) {
        const uint8_t *Loc = Img.readVA(ChainVA, sizeof(uint64_t));
        if (!Loc)
          break;
        uint64_t Raw;
        std::memcpy(&Raw, Loc, sizeof(uint64_t));
        uint32_t Next;
        if ((Raw >> 63) & 1) {
          // bind: an imported symbol, not a rebase — only carries the chain
          // link.
          dyld_chained_ptr_64_bind B;
          std::memcpy(&B, &Raw, sizeof(B));
          Next = B.next;
        } else {
          dyld_chained_ptr_64_rebase R;
          std::memcpy(&R, &Raw, sizeof(R));
          Next = R.next;
          va_t TargetVA;
          if (PtrFormat == DYLD_CHAINED_PTR_64_OFFSET) {
            if (R.target > InvalidVA - TextVMAddr)
              break;
            TargetVA = TextVMAddr + R.target;
          } else {
            TargetVA = (static_cast<uint64_t>(R.high8) << 56) | R.target;
          }
          size_t Before =
              Img.CodePtrRelocSlots.size() + Img.DataPtrRelocSlots.size();
          recordSlot(ChainVA, TargetVA);
          NumRecorded +=
              (Img.CodePtrRelocSlots.size() + Img.DataPtrRelocSlots.size()) -
              Before;
          // Apply the rebase in-place: the on-disk slot holds the *encoded*
          // chained-pointer bitfield (target/next/bind), but every consumer of
          // the loaded image (jump-table resolver reading absolute table
          // entries, pointer-table symbolization, data reads) expects the
          // resolved preferred-base VA — exactly what dyld writes at load time
          // and what the ELF loader does when it applies relocations.  Without
          // this the table entries read back as garbage (`target | next<<51`).
          Img.patchPtr(ChainVA, static_cast<uint64_t>(TargetVA));
        }
        if (Next == 0)
          break;
        // For 64-bit chained pointers, `next` counts 4-byte strides.
        va_t Delta = static_cast<va_t>(Next) * 4;
        if (Delta > InvalidVA - ChainVA)
          break;
        ChainVA += Delta;
      }
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "macho: parsed chained-fixup rebases, recorded "
                          << NumRecorded << " code/data pointer slots\n");
  (void)NumRecorded;
}

void parseUUID(const llvm::object::MachOObjectFile &Obj, BinaryImage &Img) {
  for (const auto &LC : Obj.load_commands()) {
    if (LC.C.cmd != LC_UUID || LC.C.cmdsize < sizeof(uuid_command))
      continue;
    auto UUID = *reinterpret_cast<const uuid_command *>(LC.Ptr);
    std::string Hex;
    Hex.reserve(32);
    for (uint8_t B : UUID.uuid)
      Hex += llvm::utohexstr(B, true, 2);
    Img.DynInfo.UUID = Hex;
    LLVM_DEBUG(llvm::dbgs() << "macho: UUID = " << Hex << "\n");
    return;
  }
}

void parseBuildVersion(const llvm::object::MachOObjectFile &Obj,
                       BinaryImage &Img) {
  for (const auto &LC : Obj.load_commands()) {
    if (LC.C.cmd != LC_BUILD_VERSION ||
        LC.C.cmdsize < sizeof(build_version_command))
      continue;
    auto BV = *reinterpret_cast<const build_version_command *>(LC.Ptr);
    Img.DynInfo.MinOSVersion = packed_version::toString(BV.minos);
    LLVM_DEBUG(llvm::dbgs()
               << "macho: min OS = " << Img.DynInfo.MinOSVersion << "\n");
    return;
  }
}

} // namespace macho_loader
} // namespace neverd
