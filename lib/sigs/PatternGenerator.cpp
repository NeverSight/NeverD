//===- PatternGenerator.cpp - Build .pat lines from object files ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/PatternGenerator.h"

#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Format.h"

#include <algorithm>
#include <map>
#include <vector>

using namespace llvm;
using namespace llvm::object;

namespace neverd {
namespace sigs {

PatternGeneratorStats &
PatternGeneratorStats::operator+=(const PatternGeneratorStats &Other) {
  Functions += Other.Functions;
  TooSmall += Other.TooSmall;
  TooWeak += Other.TooWeak;
  UnsupportedRelocation += Other.UnsupportedRelocation;
  UnsupportedCOFFRelocations.insert(Other.UnsupportedCOFFRelocations.begin(),
                                    Other.UnsupportedCOFFRelocations.end());
  return *this;
}

std::optional<unsigned> coffRelocationWidth(uint16_t Machine, uint16_t Type) {
  switch (Machine) {
  case COFF::IMAGE_FILE_MACHINE_I386:
    switch (Type) {
    case COFF::IMAGE_REL_I386_ABSOLUTE:
      return 0;
    case COFF::IMAGE_REL_I386_DIR16:
    case COFF::IMAGE_REL_I386_REL16:
    case COFF::IMAGE_REL_I386_SEG12:
    case COFF::IMAGE_REL_I386_SECTION:
      return 2;
    case COFF::IMAGE_REL_I386_SECREL7:
      return 1;
    case COFF::IMAGE_REL_I386_DIR32:
    case COFF::IMAGE_REL_I386_DIR32NB:
    case COFF::IMAGE_REL_I386_SECREL:
    case COFF::IMAGE_REL_I386_TOKEN:
    case COFF::IMAGE_REL_I386_REL32:
      return 4;
    }
    return std::nullopt;

  case COFF::IMAGE_FILE_MACHINE_AMD64:
    switch (Type) {
    case COFF::IMAGE_REL_AMD64_ABSOLUTE:
    case COFF::IMAGE_REL_AMD64_PAIR:
      return 0;
    case COFF::IMAGE_REL_AMD64_SECREL7:
      return 1;
    case COFF::IMAGE_REL_AMD64_SECTION:
      return 2;
    case COFF::IMAGE_REL_AMD64_ADDR32:
    case COFF::IMAGE_REL_AMD64_ADDR32NB:
    case COFF::IMAGE_REL_AMD64_REL32:
    case COFF::IMAGE_REL_AMD64_REL32_1:
    case COFF::IMAGE_REL_AMD64_REL32_2:
    case COFF::IMAGE_REL_AMD64_REL32_3:
    case COFF::IMAGE_REL_AMD64_REL32_4:
    case COFF::IMAGE_REL_AMD64_REL32_5:
    case COFF::IMAGE_REL_AMD64_SECREL:
    case COFF::IMAGE_REL_AMD64_TOKEN:
    case COFF::IMAGE_REL_AMD64_SREL32:
    case COFF::IMAGE_REL_AMD64_SSPAN32:
      return 4;
    case COFF::IMAGE_REL_AMD64_ADDR64:
      return 8;
    }
    return std::nullopt;

  case COFF::IMAGE_FILE_MACHINE_ARMNT:
    switch (Type) {
    case COFF::IMAGE_REL_ARM_ABSOLUTE:
    case COFF::IMAGE_REL_ARM_PAIR:
      return 0;
    case COFF::IMAGE_REL_ARM_SECTION:
      return 2;
    case COFF::IMAGE_REL_ARM_ADDR32:
    case COFF::IMAGE_REL_ARM_ADDR32NB:
    case COFF::IMAGE_REL_ARM_BRANCH24:
    case COFF::IMAGE_REL_ARM_BRANCH11:
    case COFF::IMAGE_REL_ARM_TOKEN:
    case COFF::IMAGE_REL_ARM_BLX24:
    case COFF::IMAGE_REL_ARM_BLX11:
    case COFF::IMAGE_REL_ARM_REL32:
    case COFF::IMAGE_REL_ARM_SECREL:
    case COFF::IMAGE_REL_ARM_BRANCH20T:
    case COFF::IMAGE_REL_ARM_BRANCH24T:
    case COFF::IMAGE_REL_ARM_BLX23T:
      return 4;
    // A MOVW/MOVT pair: the low half of the address in the first
    // instruction and the high half in the second.
    case COFF::IMAGE_REL_ARM_MOV32A:
    case COFF::IMAGE_REL_ARM_MOV32T:
      return 8;
    }
    return std::nullopt;

  case COFF::IMAGE_FILE_MACHINE_ARM64:
  case COFF::IMAGE_FILE_MACHINE_ARM64EC:
  case COFF::IMAGE_FILE_MACHINE_ARM64X:
    switch (Type) {
    case COFF::IMAGE_REL_ARM64_ABSOLUTE:
      return 0;
    case COFF::IMAGE_REL_ARM64_SECTION:
      return 2;
    case COFF::IMAGE_REL_ARM64_ADDR32:
    case COFF::IMAGE_REL_ARM64_ADDR32NB:
    case COFF::IMAGE_REL_ARM64_BRANCH26:
    case COFF::IMAGE_REL_ARM64_PAGEBASE_REL21:
    case COFF::IMAGE_REL_ARM64_REL21:
    case COFF::IMAGE_REL_ARM64_PAGEOFFSET_12A:
    case COFF::IMAGE_REL_ARM64_PAGEOFFSET_12L:
    case COFF::IMAGE_REL_ARM64_SECREL:
    case COFF::IMAGE_REL_ARM64_SECREL_LOW12A:
    case COFF::IMAGE_REL_ARM64_SECREL_HIGH12A:
    case COFF::IMAGE_REL_ARM64_SECREL_LOW12L:
    case COFF::IMAGE_REL_ARM64_TOKEN:
    case COFF::IMAGE_REL_ARM64_BRANCH19:
    case COFF::IMAGE_REL_ARM64_BRANCH14:
    case COFF::IMAGE_REL_ARM64_REL32:
      return 4;
    case COFF::IMAGE_REL_ARM64_ADDR64:
      return 8;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<uint64_t> coffBranchReferenceOffset(uint16_t Machine,
                                                  uint16_t Type,
                                                  ArrayRef<uint8_t> Code,
                                                  uint64_t RelocationOffset,
                                                  uint64_t FunctionOffset) {
  if (RelocationOffset < FunctionOffset || RelocationOffset >= Code.size())
    return std::nullopt;
  const uint64_t Offset = RelocationOffset - FunctionOffset;
  switch (Machine) {
  case COFF::IMAGE_FILE_MACHINE_I386:
  case COFF::IMAGE_FILE_MACHINE_AMD64: {
    const bool Rel32 = Machine == COFF::IMAGE_FILE_MACHINE_I386
                           ? Type == COFF::IMAGE_REL_I386_REL32
                           : Type == COFF::IMAGE_REL_AMD64_REL32;
    // The opcode before the field is what makes the field a branch target;
    // a REL32 after anything else is a data reference.
    if (!Rel32 || Offset == 0)
      return std::nullopt;
    const uint8_t Opcode = Code[RelocationOffset - 1];
    if (Opcode != 0xE8 && Opcode != 0xE9)
      return std::nullopt;
    return Offset;
  }
  case COFF::IMAGE_FILE_MACHINE_ARM64:
  case COFF::IMAGE_FILE_MACHINE_ARM64EC:
  case COFF::IMAGE_FILE_MACHINE_ARM64X:
    if (Type == COFF::IMAGE_REL_ARM64_BRANCH26)
      return Offset;
    return std::nullopt;
  case COFF::IMAGE_FILE_MACHINE_ARMNT:
    if (Type == COFF::IMAGE_REL_ARM_BRANCH24T ||
        Type == COFF::IMAGE_REL_ARM_BLX23T)
      return Offset;
    return std::nullopt;
  }
  return std::nullopt;
}

namespace {

void emitPatternBytes(raw_ostream &OS, ArrayRef<uint8_t> Data,
                      ArrayRef<bool> Wildcard, size_t Begin, size_t End) {
  for (size_t I = Begin; I < End; ++I) {
    if (Wildcard[I])
      OS << "..";
    else
      OS << format("%02X", Data[I]);
  }
}

/// The CRC span may not cross a relocation.
///
/// A relocated byte holds a link-time placeholder in the object file and a
/// resolved address in the image the signature is meant to match, so a
/// checksum spanning one can never agree with the very binaries it is for.
/// The pattern bytes state a wildcard there instead; the CRC has no way to
/// express one, so it stops.
size_t crcSpan(ArrayRef<bool> Wildcard, size_t Start) {
  size_t End = std::min(Wildcard.size(), Start + 255);
  for (size_t I = Start; I < End; ++I)
    if (Wildcard[I])
      return I - Start;
  return End - Start;
}

/// Marks \p Width bytes from \p Offset, clipped to the function.
void markWildcard(MutableArrayRef<bool> Wildcard, uint64_t Offset,
                  uint64_t Width) {
  for (uint64_t I = Offset; I < Offset + Width && I < Wildcard.size(); ++I)
    Wildcard[I] = true;
}

/// Writes a function's line, or counts why it has none.
void countOrEmit(raw_ostream &OS, StringRef Name, ArrayRef<uint8_t> Data,
                 ArrayRef<bool> Wildcard, const PatternGeneratorOptions &Opts,
                 PatternGeneratorStats &Stats,
                 ArrayRef<FuncRef> References = {}) {
  if (Data.size() < Opts.MinFuncSize)
    ++Stats.TooSmall;
  else if (statedByteCount(Wildcard, Opts) < SignatureMatcher::MinStatedBytes)
    ++Stats.TooWeak;
  else if (emitPatternLine(OS, Name, Data, Wildcard, Opts, References))
    ++Stats.Functions;
}

/// Whether a relocation target's name can be a reference: a routine's
/// linkage name, not a section (".text$mn") or a label ("$LN5").
bool isReferenceName(StringRef Name) {
  return !Name.empty() && !Name.starts_with(".") && !Name.starts_with("$");
}

/// The reading every object format shares: each function symbol ends at the
/// next symbol of any kind in its section, and every relocation covers four
/// bytes. ELF and Mach-O signatures have always been made this way.
PatternGeneratorStats generateGeneric(const ObjectFile &Obj,
                                      const PatternGeneratorOptions &Opts,
                                      raw_ostream &OS) {
  PatternGeneratorStats Stats;

  // Every symbol's address, per section, sorted: a function ends at the
  // first one after it.
  std::map<SectionRef, std::vector<uint64_t>> SymbolAddresses;
  for (const SymbolRef &Sym : Obj.symbols()) {
    Expected<uint64_t> Addr = Sym.getAddress();
    if (!Addr) {
      consumeError(Addr.takeError());
      continue;
    }
    Expected<section_iterator> Sec = Sym.getSection();
    if (!Sec) {
      consumeError(Sec.takeError());
      continue;
    }
    if (*Sec == Obj.section_end())
      continue;
    SymbolAddresses[**Sec].push_back(*Addr);
  }
  for (auto &Entry : SymbolAddresses)
    llvm::sort(Entry.second);

  for (const SymbolRef &Sym : Obj.symbols()) {
    Expected<SymbolRef::Type> Type = Sym.getType();
    if (!Type) {
      consumeError(Type.takeError());
      continue;
    }
    if (*Type != SymbolRef::ST_Function)
      continue;

    Expected<StringRef> NameOrErr = Sym.getName();
    if (!NameOrErr) {
      consumeError(NameOrErr.takeError());
      continue;
    }
    StringRef Name = *NameOrErr;
    if (Name.empty() || Name.starts_with("ltmp") || Name.starts_with("L_") ||
        Name.starts_with(".L"))
      continue;

    Expected<uint64_t> AddrOrErr = Sym.getAddress();
    if (!AddrOrErr) {
      consumeError(AddrOrErr.takeError());
      continue;
    }
    uint64_t Addr = *AddrOrErr;

    Expected<section_iterator> SecOrErr = Sym.getSection();
    if (!SecOrErr) {
      consumeError(SecOrErr.takeError());
      continue;
    }
    section_iterator Sec = *SecOrErr;
    if (Sec == Obj.section_end())
      continue;

    Expected<StringRef> Contents = Sec->getContents();
    if (!Contents) {
      consumeError(Contents.takeError());
      continue;
    }

    uint64_t Offset = Addr - Sec->getAddress();
    if (Offset >= Contents->size())
      continue;

    uint64_t FuncSize = Contents->size() - Offset;
    const std::vector<uint64_t> &Addresses = SymbolAddresses[*Sec];
    auto Next = std::upper_bound(Addresses.begin(), Addresses.end(), Addr);
    if (Next != Addresses.end() && *Next - Addr < FuncSize)
      FuncSize = *Next - Addr;

    SmallVector<bool, 256> Wildcard(FuncSize, false);
    for (const RelocationRef &Rel : Sec->relocations()) {
      uint64_t RelOffset = Rel.getOffset() - Offset;
      if (RelOffset < FuncSize)
        markWildcard(Wildcard, RelOffset, 4);
    }

    ArrayRef<uint8_t> Data(
        reinterpret_cast<const uint8_t *>(Contents->data()) + Offset, FuncSize);
    countOrEmit(OS, Name, Data, Wildcard, Opts, Stats);
  }
  return Stats;
}

/// One code section of a COFF object, read once.
struct COFFCodeSection {
  ArrayRef<uint8_t> Contents;
  /// Offsets at which a function starts, sorted and unique.
  std::vector<uint32_t> FunctionStarts;
  /// Section-relative offset, width, and type of every relocation. A width
  /// of std::nullopt is a type coffRelocationWidth does not know.
  struct Relocation {
    uint64_t Offset;
    std::optional<unsigned> Width;
    uint16_t Type;
    uint32_t Symbol;
  };
  std::vector<Relocation> Relocations;
};

bool isCOFFFunctionSymbol(const COFFSymbolRef &Sym) {
  if (Sym.getComplexType() != COFF::IMAGE_SYM_DTYPE_FUNCTION)
    return false;
  uint8_t Class = Sym.getStorageClass();
  if (Class != COFF::IMAGE_SYM_CLASS_EXTERNAL &&
      Class != COFF::IMAGE_SYM_CLASS_STATIC)
    return false;
  return !COFF::isReservedSectionNumber(Sym.getSectionNumber());
}

PatternGeneratorStats generateCOFF(const COFFObjectFile &Obj,
                                   const PatternGeneratorOptions &Opts,
                                   raw_ostream &OS) {
  PatternGeneratorStats Stats;
  const uint16_t Machine = Obj.getMachine();

  // Pass 1: the function symbols, in symbol-table order, and the code
  // sections they live in.
  struct FunctionSymbol {
    int32_t Section;
    uint32_t Offset;
    StringRef Name;
  };
  std::vector<FunctionSymbol> Functions;
  DenseMap<int32_t, COFFCodeSection> Sections;
  for (uint32_t I = 0, N = Obj.getNumberOfSymbols(); I < N; ++I) {
    Expected<COFFSymbolRef> SymOrErr = Obj.getSymbol(I);
    if (!SymOrErr) {
      consumeError(SymOrErr.takeError());
      break;
    }
    COFFSymbolRef Sym = *SymOrErr;
    I += Sym.getNumberOfAuxSymbols();
    if (!isCOFFFunctionSymbol(Sym))
      continue;

    int32_t SectionNumber = Sym.getSectionNumber();
    auto [It, Inserted] = Sections.try_emplace(SectionNumber);
    if (Inserted) {
      Expected<const coff_section *> SecOrErr = Obj.getSection(SectionNumber);
      if (!SecOrErr) {
        consumeError(SecOrErr.takeError());
        continue;
      }
      const coff_section *Sec = *SecOrErr;
      if (!(Sec->Characteristics &
            (COFF::IMAGE_SCN_CNT_CODE | COFF::IMAGE_SCN_MEM_EXECUTE)))
        continue;
      if (Error E = Obj.getSectionContents(Sec, It->second.Contents)) {
        consumeError(std::move(E));
        It->second.Contents = {};
        continue;
      }
      for (const coff_relocation &Rel : Obj.getRelocations(Sec)) {
        uint64_t Offset = uint64_t(Rel.VirtualAddress) - Sec->VirtualAddress;
        It->second.Relocations.push_back(
            {Offset, coffRelocationWidth(Machine, Rel.Type), Rel.Type,
             Rel.SymbolTableIndex});
      }
    }
    if (It->second.Contents.empty() ||
        Sym.getValue() >= It->second.Contents.size())
      continue;

    Expected<StringRef> NameOrErr = Obj.getSymbolName(Sym);
    if (!NameOrErr) {
      consumeError(NameOrErr.takeError());
      continue;
    }
    if (NameOrErr->empty())
      continue;
    It->second.FunctionStarts.push_back(Sym.getValue());
    Functions.push_back({SectionNumber, Sym.getValue(), *NameOrErr});
  }
  for (auto &Entry : Sections) {
    std::vector<uint32_t> &Starts = Entry.second.FunctionStarts;
    llvm::sort(Starts);
    Starts.erase(std::unique(Starts.begin(), Starts.end()), Starts.end());
  }

  // Pass 2: each function runs to the next function in its section, or to
  // the section's end. Labels -- MSVC's `$LN` jump and handler targets --
  // are not function symbols, so they do not cut a function short.
  for (const FunctionSymbol &Fn : Functions) {
    const COFFCodeSection &Sec = Sections.find(Fn.Section)->second;
    uint64_t End = Sec.Contents.size();
    auto Next = std::upper_bound(Sec.FunctionStarts.begin(),
                                 Sec.FunctionStarts.end(), Fn.Offset);
    if (Next != Sec.FunctionStarts.end())
      End = *Next;
    const uint64_t Size = End - Fn.Offset;

    SmallVector<bool, 256> Wildcard(Size, false);
    std::vector<FuncRef> References;
    bool Supported = true;
    for (const COFFCodeSection::Relocation &Rel : Sec.Relocations) {
      if (Rel.Offset < Fn.Offset || Rel.Offset >= End)
        continue;
      if (!Rel.Width) {
        Supported = false;
        Stats.UnsupportedCOFFRelocations.insert({Machine, Rel.Type});
        continue;
      }
      markWildcard(Wildcard, Rel.Offset - Fn.Offset, *Rel.Width);
      if (!Opts.EmitReferences)
        continue;
      const std::optional<uint64_t> At = coffBranchReferenceOffset(
          Machine, Rel.Type, Sec.Contents, Rel.Offset, Fn.Offset);
      if (!At)
        continue;
      Expected<COFFSymbolRef> TargetOrErr = Obj.getSymbol(Rel.Symbol);
      if (!TargetOrErr) {
        consumeError(TargetOrErr.takeError());
        continue;
      }
      Expected<StringRef> TargetName = Obj.getSymbolName(*TargetOrErr);
      if (!TargetName) {
        consumeError(TargetName.takeError());
        continue;
      }
      // A branch to the function's own start is recursion, not a reference
      // to anything the image has to confirm.
      if (isReferenceName(*TargetName) && *TargetName != Fn.Name)
        References.push_back({static_cast<uint32_t>(*At), TargetName->str()});
    }
    if (!Supported) {
      ++Stats.UnsupportedRelocation;
      continue;
    }
    llvm::sort(References, [](const FuncRef &A, const FuncRef &B) {
      return A.Offset < B.Offset;
    });

    ArrayRef<uint8_t> Data = Sec.Contents.slice(Fn.Offset, Size);
    countOrEmit(OS, Fn.Name, Data, Wildcard, Opts, Stats, References);
  }
  return Stats;
}

} // anonymous namespace

size_t statedByteCount(ArrayRef<bool> Wildcard,
                       const PatternGeneratorOptions &Opts) {
  const size_t Size = Wildcard.size();
  const size_t LeadBytes = std::min(static_cast<size_t>(Opts.LeadingLen), Size);
  const size_t CRCLen = crcSpan(Wildcard, LeadBytes);
  const size_t TailStart = LeadBytes + CRCLen;
  const size_t TailEnd =
      std::min(Size, TailStart + static_cast<size_t>(Opts.TailLen));
  size_t Stated = CRCLen;
  for (size_t I = 0; I < LeadBytes; ++I)
    Stated += Wildcard[I] ? 0 : 1;
  for (size_t I = TailStart; I < TailEnd; ++I)
    Stated += Wildcard[I] ? 0 : 1;
  return Stated;
}

bool emitPatternLine(raw_ostream &OS, StringRef Name, ArrayRef<uint8_t> Data,
                     ArrayRef<bool> Wildcard,
                     const PatternGeneratorOptions &Opts,
                     ArrayRef<FuncRef> References) {
  const size_t Size = Data.size();
  if (Size < Opts.MinFuncSize || Wildcard.size() != Size ||
      statedByteCount(Wildcard, Opts) < SignatureMatcher::MinStatedBytes)
    return false;

  const size_t LeadBytes = std::min(static_cast<size_t>(Opts.LeadingLen), Size);
  emitPatternBytes(OS, Data, Wildcard, 0, LeadBytes);

  const size_t CRCStart = LeadBytes;
  const size_t CRCLen = crcSpan(Wildcard, CRCStart);
  uint16_t CRC = 0;
  if (CRCLen > 0)
    CRC = SignatureMatcher::computeCRC16(Data.data() + CRCStart, CRCLen);

  OS << format(" %02X %04X %04X", static_cast<unsigned>(CRCLen), CRC,
               static_cast<unsigned>(Size));
  OS << " :0000 " << Name;
  for (const FuncRef &Ref : References)
    OS << format(" ^%04X ", static_cast<unsigned>(Ref.Offset)) << Ref.Name;

  // Everything the CRC had to stop short of, stated byte by byte so that a
  // wildcard can stand where a relocation does.  This is what lets a match
  // cover a whole function rather than its first invariant run, which is the
  // difference between a name worth displaying and a name worth acting on.
  const size_t TailStart = CRCStart + CRCLen;
  const size_t TailEnd =
      std::min(Size, TailStart + static_cast<size_t>(Opts.TailLen));
  if (TailEnd > TailStart) {
    OS << " ";
    emitPatternBytes(OS, Data, Wildcard, TailStart, TailEnd);
  }

  OS << "\n";
  return true;
}

PatternGeneratorStats generatePatterns(const ObjectFile &Obj,
                                       const PatternGeneratorOptions &Opts,
                                       raw_ostream &OS) {
  if (const auto *COFF = dyn_cast<COFFObjectFile>(&Obj))
    return generateCOFF(*COFF, Opts, OS);
  return generateGeneric(Obj, Opts, OS);
}

} // namespace sigs
} // namespace neverd
