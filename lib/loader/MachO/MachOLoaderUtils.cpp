//===- MachOLoaderUtils.cpp - Core Mach-O loader helpers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "neverd/object/MachOLayout.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstddef>
#include <cstring>
#include <optional>
#include <set>

#define DEBUG_TYPE "neverd-macho-loader"

namespace neverd {
namespace macho_loader {

using namespace llvm::MachO;

namespace {

/// LC_THREAD / LC_UNIXTHREAD: load_command header followed by one or more
/// {flavor, count, register state} records.
struct UnixThreadFlavorHeader {
  uint32_t Flavor;
  uint32_t Count;
};

va_t normalizeMachOFunctionAddress(va_t Addr, const BinaryImage &Img) {
  return Img.Arch == Arch::ARM ? clearThumbBit(Addr) : Addr;
}

template <typename StateT, typename GetPC>
std::optional<va_t> readThreadState(const uint8_t *StatePtr, size_t StateBytes,
                                    GetPC GetProgramCounter) {
  if (StateBytes < sizeof(StateT))
    return std::nullopt;
  StateT State;
  std::memcpy(&State, StatePtr, sizeof(State));
  return static_cast<va_t>(GetProgramCounter(State));
}

std::optional<va_t> readThreadCommandEntry(const uint8_t *Command,
                                           size_t CommandSize, Arch A) {
  size_t Offset = sizeof(llvm::MachO::load_command);
  while (rangeInBounds(Offset, sizeof(UnixThreadFlavorHeader), CommandSize)) {
    UnixThreadFlavorHeader Header;
    std::memcpy(&Header, Command + Offset, sizeof(Header));
    Offset += sizeof(Header);
    if (Header.Count > (CommandSize - Offset) / sizeof(uint32_t))
      return std::nullopt;
    size_t StateBytes = static_cast<size_t>(Header.Count) * sizeof(uint32_t);
    const uint8_t *StatePtr = Command + Offset;

    if (A == Arch::X86 && Header.Flavor == x86_THREAD_STATE32)
      if (auto Entry = readThreadState<x86_thread_state32_t>(
              StatePtr, StateBytes,
              [](const x86_thread_state32_t &State) { return State.eip; }))
        return Entry;
    if (A == Arch::X64 && Header.Flavor == x86_THREAD_STATE64)
      if (auto Entry = readThreadState<x86_thread_state64_t>(
              StatePtr, StateBytes,
              [](const x86_thread_state64_t &State) { return State.rip; }))
        return Entry;
    if (A == Arch::ARM && Header.Flavor == ARM_THREAD_STATE)
      if (auto Entry = readThreadState<arm_thread_state32_t>(
              StatePtr, StateBytes,
              [](const arm_thread_state32_t &State) { return State.pc; }))
        return clearThumbBit(*Entry);
    if (A == Arch::AArch64 && Header.Flavor == ARM_THREAD_STATE64)
      if (auto Entry = readThreadState<arm_thread_state64_t>(
              StatePtr, StateBytes,
              [](const arm_thread_state64_t &State) { return State.pc; }))
        return Entry;

    Offset += StateBytes;
  }
  return std::nullopt;
}

void appendRuntimeFunction(va_t Addr, std::vector<va_t> *Out,
                           BinaryImage &Img) {
  Addr = normalizeMachOFunctionAddress(Addr, Img);
  if (!Img.recordRuntimeFunction(Addr))
    return;
  if (Out && std::find(Out->begin(), Out->end(), Addr) == Out->end())
    Out->push_back(Addr);
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

va_t getMachHeaderVA(const BinaryImage &Img) {
  const Segment *Fallback = nullptr;
  for (const Segment &Seg : Img.Segments) {
    if (Seg.FileOff != 0 || Seg.FileSz == 0)
      continue;
    if (Seg.Name == "__TEXT")
      return Seg.VA;
    if (!Fallback)
      Fallback = &Seg;
  }
  return Fallback ? Fallback->VA : Img.Base;
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
  // LC_MAIN is main(), not a loader scaffold, and takes precedence regardless
  // of command ordering.
  for (const auto &LC : Obj.load_commands()) {
    if (LC.C.cmd == LC_MAIN && LC.C.cmdsize >= sizeof(entry_point_command)) {
      entry_point_command EP;
      std::memcpy(&EP, LC.Ptr, sizeof(EP));
      if (EP.entryoff > InvalidVA - TextVMAddr)
        continue;
      Img.Entry = TextVMAddr + EP.entryoff;
      if (Img.Arch == Arch::ARM)
        Img.Entry = clearThumbBit(Img.Entry);
      return;
    }
  }

  auto FindThreadEntry = [&](uint32_t CommandType) -> std::optional<va_t> {
    for (const auto &LC : Obj.load_commands()) {
      if (LC.C.cmd != CommandType ||
          LC.C.cmdsize < sizeof(llvm::MachO::load_command))
        continue;
      auto Entry = readThreadCommandEntry(
          reinterpret_cast<const uint8_t *>(LC.Ptr), LC.C.cmdsize, Img.Arch);
      if (Entry)
        return normalizeMachOFunctionAddress(*Entry, Img);
    }
    return std::nullopt;
  };

  std::optional<va_t> Entry = FindThreadEntry(LC_UNIXTHREAD);
  if (!Entry)
    Entry = FindThreadEntry(LC_THREAD);
  if (Entry) {
    Img.Entry = *Entry;
    Img.recordRuntimeFunction(*Entry);
  }
}

void parseRuntimeLoadCommands(const llvm::object::MachOObjectFile &Obj,
                              BinaryImage &Img) {
  for (const auto &LC : Obj.load_commands()) {
    va_t InitAddress = 0;
    if (LC.C.cmd == LC_ROUTINES && LC.C.cmdsize >= sizeof(routines_command)) {
      routines_command Command;
      std::memcpy(&Command, LC.Ptr, sizeof(Command));
      InitAddress = Command.init_address;
    } else if (LC.C.cmd == LC_ROUTINES_64 &&
               LC.C.cmdsize >= sizeof(routines_command_64)) {
      routines_command_64 Command;
      std::memcpy(&Command, LC.Ptr, sizeof(Command));
      InitAddress = Command.init_address;
    } else
      continue;
    if (InitAddress == 0)
      continue;
    InitAddress = normalizeMachOFunctionAddress(InitAddress, Img);
    if (Img.recordRuntimeFunction(InitAddress) && Img.DynInfo.InitAddr == 0)
      Img.DynInfo.InitAddr = InitAddress;
  }
}

void parseRuntimeFunctionSections(const std::vector<SectionInfo> &Sections,
                                  uint64_t TextVMAddr, BinaryImage &Img) {
  for (const SectionInfo &Sec : Sections) {
    std::vector<va_t> *Out = nullptr;
    uint32_t EntrySize = Img.getPointerSize();
    if (Sec.Flags == S_MOD_INIT_FUNC_POINTERS ||
        Sec.Flags == S_THREAD_LOCAL_INIT_FUNCTION_POINTERS)
      Out = &Img.DynInfo.InitArray;
    else if (Sec.Flags == S_MOD_TERM_FUNC_POINTERS)
      Out = &Img.DynInfo.FiniArray;
    else if (Sec.Flags == S_INIT_FUNC_OFFSETS) {
      Out = &Img.DynInfo.InitArray;
      EntrySize = sizeof(uint32_t);
    } else {
      continue;
    }

    const Segment *DataSeg = Img.getSegmentFor(Sec.Addr);
    if (!DataSeg || !DataSeg->isReadable())
      continue;

    uint64_t Count = Sec.Size / EntrySize;
    for (uint64_t I = 0; I < Count; ++I) {
      if (I > (InvalidVA - Sec.Addr) / EntrySize)
        break;
      const uint8_t *P = Img.readVA(Sec.Addr + I * EntrySize, EntrySize);
      if (!P)
        break;
      va_t Addr = 0;
      if (Sec.Flags == S_INIT_FUNC_OFFSETS) {
        uint32_t Offset = readLE<uint32_t>(P);
        if (Offset > InvalidVA - TextVMAddr)
          continue;
        Addr = TextVMAddr + Offset;
      } else {
        Addr = static_cast<va_t>(readPtr(P, Img.is64Bit()));
        if (Addr == 0)
          continue;
      }
      appendRuntimeFunction(Addr, Out, Img);
    }
  }
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
        if (!ReadULEB(TermP, TermEnd, Addr) || Addr > InvalidVA - TextVMAddr)
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
        Stack.push_back({static_cast<size_t>(ChildOff), Prefix + EdgeLabel});
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "macho: export trie added " << Added
                          << " exports\n");
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
