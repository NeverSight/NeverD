//===- neverd-sigmaker.cpp - Generate .pat signatures from libraries ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tool to generate .pat signature files from static libraries (.a / .lib).
/// Extracts function byte patterns with wildcard masks for relocations,
/// computes CRC16 over trailing bytes, and outputs FLIRT-compatible .pat
/// format.
///
/// Usage:
///   neverd-sigmaker --input /path/to/libfoo.a --output foo.pat
///   neverd-sigmaker --input /usr/lib/libc.a --output libc.pat --name "libc"
///
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <set>

using namespace llvm;
using namespace llvm::object;

static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<library>"),
                                      cl::Required);
static cl::opt<std::string> OutputFile("o", cl::desc("Output .pat file"),
                                       cl::Required);
static cl::opt<std::string> LibName("name", cl::desc("Library name tag"),
                                    cl::init(""));
static cl::opt<unsigned>
    LeadingLen("leading", cl::desc("Leading pattern bytes"), cl::init(32));
static cl::opt<unsigned>
    MinFuncSize("min-size", cl::desc("Minimum function size"), cl::init(4));

static bool emitPatLine(raw_ostream &OS, StringRef FuncName,
                        const uint8_t *Data, size_t Size,
                        ArrayRef<std::pair<uint64_t, uint64_t>> Relocs) {
  if (Size < MinFuncSize)
    return false;

  size_t LeadBytes = std::min(static_cast<size_t>(LeadingLen), Size);

  // Build relocation set for fast lookup.
  std::set<uint64_t> RelocOffsets;
  for (auto &[Off, Len] : Relocs) {
    for (uint64_t I = Off; I < Off + Len && I < Size; ++I)
      RelocOffsets.insert(I);
  }

  // Emit leading bytes with wildcards at relocation positions.
  for (size_t I = 0; I < LeadBytes; ++I) {
    if (RelocOffsets.count(I))
      OS << "..";
    else
      OS << format("%02X", Data[I]);
  }

  // CRC16 over bytes after leading pattern.
  size_t CRCStart = LeadBytes;
  size_t CRCLen = std::min(Size - CRCStart, static_cast<size_t>(255));
  uint16_t CRC = 0;
  if (CRCLen > 0)
    CRC = neverd::sigs::SignatureMatcher::computeCRC16(Data + CRCStart, CRCLen);

  OS << format(" %02X %04X %04X", static_cast<unsigned>(CRCLen), CRC,
               static_cast<unsigned>(Size));
  OS << " :0000 " << FuncName << "\n";
  return true;
}

static int processObject(ObjectFile &Obj, raw_ostream &OS) {
  int Count = 0;
  for (const auto &Sym : Obj.symbols()) {
    auto TypeOrErr = Sym.getType();
    if (!TypeOrErr) {
      consumeError(TypeOrErr.takeError());
      continue;
    }
    if (*TypeOrErr != SymbolRef::ST_Function)
      continue;

    auto NameOrErr = Sym.getName();
    if (!NameOrErr) {
      consumeError(NameOrErr.takeError());
      continue;
    }
    StringRef Name = *NameOrErr;
    if (Name.empty() || Name.starts_with("ltmp") || Name.starts_with("L_") ||
        Name.starts_with(".L"))
      continue;

    auto AddrOrErr = Sym.getAddress();
    if (!AddrOrErr) {
      consumeError(AddrOrErr.takeError());
      continue;
    }
    uint64_t Addr = *AddrOrErr;

    auto SecOrErr = Sym.getSection();
    if (!SecOrErr) {
      consumeError(SecOrErr.takeError());
      continue;
    }
    auto Sec = *SecOrErr;
    if (Sec == Obj.section_end())
      continue;

    auto DataOrErr = Sec->getContents();
    if (!DataOrErr) {
      consumeError(DataOrErr.takeError());
      continue;
    }

    uint64_t SecAddr = Sec->getAddress();
    uint64_t Offset = Addr - SecAddr;
    if (Offset >= DataOrErr->size())
      continue;

    const auto *Data =
        reinterpret_cast<const uint8_t *>(DataOrErr->data() + Offset);

    // Estimate function size (distance to next symbol in same section).
    uint64_t FuncSize = DataOrErr->size() - Offset;
    for (const auto &Other : Obj.symbols()) {
      auto OAddr = Other.getAddress();
      if (!OAddr) {
        consumeError(OAddr.takeError());
        continue;
      }
      auto OSec = Other.getSection();
      if (!OSec) {
        consumeError(OSec.takeError());
        continue;
      }
      if (*OSec != Sec)
        continue;
      if (*OAddr > Addr && *OAddr - Addr < FuncSize)
        FuncSize = *OAddr - Addr;
    }

    // Collect relocations targeting this function's range.
    std::vector<std::pair<uint64_t, uint64_t>> Relocs;
    for (const auto &Rel : Sec->relocations()) {
      uint64_t ROff = Rel.getOffset() - Offset;
      if (ROff < FuncSize)
        Relocs.push_back({ROff, 4});
    }

    if (emitPatLine(OS, Name, Data, static_cast<size_t>(FuncSize), Relocs))
      ++Count;
  }
  return Count;
}

int main(int Argc, char *Argv[]) {
  InitLLVM X(Argc, Argv);
  cl::ParseCommandLineOptions(Argc, Argv,
                              "NeverD Signature Maker\n\n"
                              "  Generate .pat files from static libraries.\n");

  std::error_code EC;
  raw_fd_ostream OS(OutputFile, EC);
  if (EC) {
    WithColor::error() << "cannot open output: " << EC.message() << "\n";
    return 1;
  }

  auto BufOrErr = MemoryBuffer::getFile(InputFile);
  if (!BufOrErr) {
    WithColor::error() << "cannot open: " << InputFile << "\n";
    return 1;
  }

  int TotalFuncs = 0;
  auto MemRef = (*BufOrErr)->getMemBufferRef();

  StringRef Magic = MemRef.getBuffer().substr(0, 8);
  if (Magic.starts_with("!<arch>") || Magic.starts_with("!<thin>")) {
    auto ArchOrErr = Archive::create(MemRef);
    if (!ArchOrErr) {
      WithColor::error() << "invalid archive: "
                         << toString(ArchOrErr.takeError()) << "\n";
      return 1;
    }

    Error Err = Error::success();
    for (auto &Child : (*ArchOrErr)->children(Err)) {
      auto ObjOrErr = Child.getAsBinary();
      if (!ObjOrErr) {
        consumeError(ObjOrErr.takeError());
        continue;
      }
      if (auto *Obj = dyn_cast<ObjectFile>(ObjOrErr->get()))
        TotalFuncs += processObject(*Obj, OS);
    }
    if (Err)
      consumeError(std::move(Err));
  } else {
    auto ObjOrErr =
        ObjectFile::createObjectFile((*BufOrErr)->getMemBufferRef());
    if (!ObjOrErr) {
      WithColor::error() << "not a valid object/archive: "
                         << toString(ObjOrErr.takeError()) << "\n";
      return 1;
    }
    TotalFuncs = processObject(**ObjOrErr, OS);
  }

  outs() << "Generated " << TotalFuncs << " signatures → " << OutputFile
         << "\n";
  return 0;
}
