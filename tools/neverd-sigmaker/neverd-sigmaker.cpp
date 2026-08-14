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
/// A relocation bounds what a signature may assert: the byte holds a
/// placeholder here and an address in the linked image.  Wildcards cover the
/// ones inside the leading pattern and the tail, and the CRC stops at the
/// first one it meets, because a checksum cannot express a wildcard.
///
/// Usage:
///   neverd-sigmaker /path/to/libfoo.a -o foo.pat
///   neverd-sigmaker /usr/lib/libc.a -o libc.pat --name "libc"
///   neverd-sigmaker libgcc_eh.a -o eh.pat --tail 65535
///
/// The last form states every byte of every function, so a match is agreement
/// over the whole routine rather than over its opening run.  A consumer that
/// acts on the name it gets -- naming an exception personality, say -- asks
/// for that; see SignatureMatcher::isFullyVerified.
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
static cl::opt<unsigned>
    TailLen("tail",
            cl::desc("Trailing pattern bytes to emit after the CRC span; "
                     "0 emits none, a value at least as large as the function "
                     "covers it to its end"),
            cl::init(0));

/// The CRC span may not cross a relocation.
///
/// A relocated byte holds a link-time placeholder in the object file and a
/// resolved address in the image the signature is meant to match, so a
/// checksum spanning one can never agree with the very binaries it is for.
/// The pattern bytes state a wildcard there instead; the CRC has no way to
/// express one, so it stops.
static size_t crcSpan(size_t Start, size_t Size,
                      const std::set<uint64_t> &RelocOffsets) {
  size_t End = std::min(Size, Start + 255);
  for (size_t I = Start; I < End; ++I)
    if (RelocOffsets.count(I))
      return I - Start;
  return End - Start;
}

static void emitPatternBytes(raw_ostream &OS, const uint8_t *Data, size_t Begin,
                             size_t End,
                             const std::set<uint64_t> &RelocOffsets) {
  for (size_t I = Begin; I < End; ++I) {
    if (RelocOffsets.count(I))
      OS << "..";
    else
      OS << format("%02X", Data[I]);
  }
}

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

  emitPatternBytes(OS, Data, 0, LeadBytes, RelocOffsets);

  size_t CRCStart = LeadBytes;
  size_t CRCLen = crcSpan(CRCStart, Size, RelocOffsets);
  uint16_t CRC = 0;
  if (CRCLen > 0)
    CRC = neverd::sigs::SignatureMatcher::computeCRC16(Data + CRCStart, CRCLen);

  OS << format(" %02X %04X %04X", static_cast<unsigned>(CRCLen), CRC,
               static_cast<unsigned>(Size));
  OS << " :0000 " << FuncName;

  // Everything the CRC had to stop short of, stated byte by byte so that a
  // wildcard can stand where a relocation does.  This is what lets a match
  // cover a whole function rather than its first invariant run, which is the
  // difference between a name worth displaying and a name worth acting on.
  size_t TailStart = CRCStart + CRCLen;
  size_t TailEnd = std::min(Size, TailStart + static_cast<size_t>(TailLen));
  if (TailEnd > TailStart) {
    OS << " ";
    emitPatternBytes(OS, Data, TailStart, TailEnd, RelocOffsets);
  }

  OS << "\n";
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
