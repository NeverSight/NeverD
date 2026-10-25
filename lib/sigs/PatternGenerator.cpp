//===- PatternGenerator.cpp - Build .pat lines from object files ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/PatternGenerator.h"

#include "X86/X86ImportReference.h"

#include "neverd/loader/COFF/RTTI.h"
#include "neverd/object/SectionNames.h"
#include "neverd/sigs/SignatureMatcher.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/MC/MCLinkerOptimizationHint.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/LEB128.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <map>
#include <tuple>
#include <vector>

using namespace llvm;
using namespace llvm::object;

namespace neverd {
namespace sigs {

namespace {

#define NEVERD_BRANCH_VALUE(Name, Value)                                       \
  [[maybe_unused]] constexpr uint32_t Name = Value;
#include "neverd/support/BranchEncoding.def"

#define NEVERD_SIGS_SYNTAX_CHAR(Name, Value)                                   \
  [[maybe_unused]] constexpr char Name = Value;
#define NEVERD_SIGS_SYNTAX_STRING(Name, Value)                                 \
  [[maybe_unused]] constexpr StringLiteral Name(Value);
#include "neverd/sigs/LinkerSyntax.def"

constexpr StringLiteral LocalLabelPrefixes[] = {
#define NEVERD_SIGS_LOCAL_LABEL_PREFIX(Value) Value,
#include "neverd/sigs/LinkerSyntax.def"
};

constexpr StringLiteral SynthesizedRoutinePrefixes[] = {
#define NEVERD_SIGS_SYNTHESIZED_ROUTINE_PREFIX(Value) Value,
#include "neverd/sigs/LinkerSyntax.def"
};

#define NEVERD_PATTERN_STRING(Name, Value)                                     \
  [[maybe_unused]] constexpr StringLiteral Name(Value);
#define NEVERD_PATTERN_CHAR(Name, Value)                                       \
  [[maybe_unused]] constexpr char Name = Value;
#define NEVERD_PATTERN_VALUE(Name, Value)                                      \
  [[maybe_unused]] constexpr unsigned Name = Value;
#define NEVERD_PATTERN_FORMAT(Name, Value)                                     \
  [[maybe_unused]] constexpr const char *Name = Value;
#include "neverd/sigs/PatternSyntax.def"

#define NEVERD_OTHER_RELOCATION(Width)                                         \
  constexpr unsigned OtherRelocationWidth = Width;
#include "RelocationFootprints.def"

/// What TargetMachine.def says of an architecture.
struct TargetMachineInfo {
  StringLiteral Spelling;
  uint16_t COFFMachine;
  uint16_t ELFMachine;
  unsigned AddressBytes;
  uint32_t MachOCPUType;
};

/// The architectures, in the order of TargetMachine.
constexpr TargetMachineInfo TargetMachines[] = {
#define NEVERD_SIGS_TARGET_MACHINE(Name, Spelling, COFFMachine, ELFMachine,    \
                                   AddressBytes, MachOCPUType)                 \
  {Spelling, COFF::COFFMachine, ELF::ELFMachine, AddressBytes,                 \
   MachO::MachOCPUType},
#include "neverd/sigs/TargetMachine.def"
};

} // anonymous namespace

PatternGeneratorStats &
PatternGeneratorStats::operator+=(const PatternGeneratorStats &Other) {
  Functions += Other.Functions;
  TooSmall += Other.TooSmall;
  TooWeak += Other.TooWeak;
  UnsupportedRelocation += Other.UnsupportedRelocation;
  UnsupportedCOFFRelocations.insert(Other.UnsupportedCOFFRelocations.begin(),
                                    Other.UnsupportedCOFFRelocations.end());
  UnsupportedELFRelocations.insert(Other.UnsupportedELFRelocations.begin(),
                                   Other.UnsupportedELFRelocations.end());
  UnsupportedMachORelocations.insert(Other.UnsupportedMachORelocations.begin(),
                                     Other.UnsupportedMachORelocations.end());
  UnsupportedHint += Other.UnsupportedHint;
  UnsupportedMachOHints.insert(Other.UnsupportedMachOHints.begin(),
                               Other.UnsupportedMachOHints.end());
  UnreadableHints += Other.UnreadableHints;
  Synthesized += Other.Synthesized;
  return *this;
}

bool isSynthesizedRoutineName(StringRef Name) {
  // One reserved `_` in front, as a Mach-O object adds to every name.
  Name.consume_front(StringRef(&ReservedNamePrefix, 1));
  return llvm::any_of(SynthesizedRoutinePrefixes, [&](StringRef Prefix) {
    StringRef Number = Name;
    return Number.consume_front(Prefix) && !Number.empty() &&
           isDigit(Number.front()) && isDigit(Number.back()) &&
           llvm::all_of(Number, [](char C) {
             return isDigit(C) || C == SynthesizedNumberSeparator;
           });
  });
}

std::optional<TargetMachine> parseTargetMachine(StringRef Name) {
  for (size_t I = 0; I < std::size(TargetMachines); ++I)
    if (TargetMachines[I].Spelling == Name)
      return static_cast<TargetMachine>(I);
  return std::nullopt;
}

ArrayRef<StringLiteral> targetMachineNames() {
  static constexpr StringLiteral Names[] = {
#define NEVERD_SIGS_TARGET_MACHINE(Name, Spelling, COFFMachine, ELFMachine,    \
                                   AddressBytes, MachOCPUType)                 \
  Spelling,
#include "neverd/sigs/TargetMachine.def"
  };
  return Names;
}

bool isObjectForMachine(const ObjectFile &Obj, TargetMachine Machine) {
  assert(static_cast<size_t>(Machine) < std::size(TargetMachines) &&
         "not an architecture of TargetMachine.def");
  const TargetMachineInfo &Info = TargetMachines[static_cast<size_t>(Machine)];
  if (const auto *COFF = dyn_cast<COFFObjectFile>(&Obj))
    return COFF->getMachine() == Info.COFFMachine;
  if (const auto *ELFObj = dyn_cast<ELFObjectFileBase>(&Obj))
    return Obj.getBytesInAddress() == Info.AddressBytes &&
           ELFObj->getEMachine() == Info.ELFMachine;
  if (const auto *MachOObj = dyn_cast<MachOObjectFile>(&Obj))
    return MachOObj->getHeader().cputype == Info.MachOCPUType;
  return false;
}

std::optional<ELFRelocationFootprint> elfRelocationFootprint(uint16_t Machine,
                                                             uint32_t Type) {
  // A range is First to Last; below First, the difference wraps around.
  auto InRange = [Type](uint32_t First, uint32_t Last) {
    return Type - First <= Last - First;
  };
  switch (Machine) {
  case ELF::EM_X86_64:
    switch (Type) {
#define NEVERD_ELF_X86_64_RELOCATION(Name, Before, Width)                      \
  case ELF::Name:                                                              \
    return ELFRelocationFootprint{Before, Width};
#include "RelocationFootprints.def"
    }
    return std::nullopt;

  case ELF::EM_386:
    switch (Type) {
#define NEVERD_ELF_386_RELOCATION(Name, Before, Width)                         \
  case ELF::Name:                                                              \
    return ELFRelocationFootprint{Before, Width};
#include "RelocationFootprints.def"
    }
    return std::nullopt;

  case ELF::EM_AARCH64:
    switch (Type) {
#define NEVERD_ELF_AARCH64_RELOCATION(Name, Before, Width)                     \
  case ELF::Name:                                                              \
    return ELFRelocationFootprint{Before, Width};
#include "RelocationFootprints.def"
    }
#define NEVERD_ELF_AARCH64_RELOCATIONS(First, Last, Before, Width)             \
  if (InRange(ELF::First, ELF::Last))                                          \
    return ELFRelocationFootprint{Before, Width};
#include "RelocationFootprints.def"
    return std::nullopt;

  case ELF::EM_ARM:
    switch (Type) {
#define NEVERD_ELF_ARM_RELOCATION(Name, Before, Width)                         \
  case ELF::Name:                                                              \
    return ELFRelocationFootprint{Before, Width};
#define NEVERD_ELF_ARM_UNSUPPORTED_RELOCATION(Name)                            \
  case ELF::Name:                                                              \
    return std::nullopt;
#include "RelocationFootprints.def"
    }
#define NEVERD_ELF_ARM_RELOCATIONS(First, Last, Before, Width)                 \
  if (InRange(ELF::First, ELF::Last))                                          \
    return ELFRelocationFootprint{Before, Width};
#include "RelocationFootprints.def"
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<RelocationFootprint>
machORelocationFootprint(uint32_t CPUType, uint32_t Type, unsigned Length) {
  // A sized relocation's field is as long as its r_length, a power of two,
  // says.
  const RelocationFootprint Sized{0, 1u << Length};
  switch (CPUType) {
  case MachO::CPU_TYPE_ARM64:
    switch (Type) {
#define NEVERD_MACHO_ARM64_RELOCATION(Name, Before, Width)                     \
  case MachO::Name:                                                            \
    return RelocationFootprint{Before, Width};
#define NEVERD_MACHO_ARM64_SIZED_RELOCATION(Name)                              \
  case MachO::Name:                                                            \
    return Sized;
#include "RelocationFootprints.def"
    }
    return std::nullopt;

  case MachO::CPU_TYPE_X86_64:
    switch (Type) {
#define NEVERD_MACHO_X86_64_RELOCATION(Name, Before, Width)                    \
  case MachO::Name:                                                            \
    return RelocationFootprint{Before, Width};
#define NEVERD_MACHO_X86_64_SIZED_RELOCATION(Name)                             \
  case MachO::Name:                                                            \
    return Sized;
#include "RelocationFootprints.def"
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<unsigned> machOOptimizationHintWidth(uint32_t CPUType,
                                                   uint32_t Kind) {
  // The kinds LLVM's assembler writes, and ld64 and lld fold.
  if (!isValidMCLOHType(Kind))
    return std::nullopt;
  switch (CPUType) {
#define NEVERD_MACHO_OPTIMIZATION_HINT(CPU, Width)                             \
  case MachO::CPU:                                                             \
    return Width;
#include "RelocationFootprints.def"
  }
  return std::nullopt;
}

std::optional<unsigned> coffRelocationWidth(uint16_t Machine, uint16_t Type) {
  switch (Machine) {
  case COFF::IMAGE_FILE_MACHINE_I386:
    switch (Type) {
#define NEVERD_COFF_I386_RELOCATION(Name, Width)                               \
  case COFF::Name:                                                             \
    return Width;
#include "RelocationFootprints.def"
    }
    return std::nullopt;

  case COFF::IMAGE_FILE_MACHINE_AMD64:
    switch (Type) {
#define NEVERD_COFF_AMD64_RELOCATION(Name, Width)                              \
  case COFF::Name:                                                             \
    return Width;
#include "RelocationFootprints.def"
    }
    return std::nullopt;

  case COFF::IMAGE_FILE_MACHINE_ARMNT:
    switch (Type) {
#define NEVERD_COFF_ARM_RELOCATION(Name, Width)                                \
  case COFF::Name:                                                             \
    return Width;
#include "RelocationFootprints.def"
    }
    return std::nullopt;

  case COFF::IMAGE_FILE_MACHINE_ARM64:
  case COFF::IMAGE_FILE_MACHINE_ARM64EC:
  case COFF::IMAGE_FILE_MACHINE_ARM64X:
    switch (Type) {
#define NEVERD_COFF_ARM64_RELOCATION(Name, Width)                              \
  case COFF::Name:                                                             \
    return Width;
#include "RelocationFootprints.def"
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
    const bool ImportField = Machine == COFF::IMAGE_FILE_MACHINE_I386
                                 ? Type == COFF::IMAGE_REL_I386_DIR32
                                 : Type == COFF::IMAGE_REL_AMD64_REL32;
    if (ImportField &&
        isX86ImportReferenceField(Code.drop_front(FunctionOffset), Offset,
                                  Machine == COFF::IMAGE_FILE_MACHINE_AMD64))
      return Offset;
    if (Rel32 &&
        isX86ConditionalReferenceField(Code.drop_front(FunctionOffset), Offset,
                                       Machine ==
                                           COFF::IMAGE_FILE_MACHINE_AMD64) &&
        readLE<uint32_t>(Code.data() + RelocationOffset) == 0)
      return Offset;
    // Otherwise the opcode before the field must be a direct branch.
    if (!Rel32 || Offset == 0)
      return std::nullopt;
    const uint8_t Opcode = Code[RelocationOffset - x86::kRel32DispOffset];
    if (Opcode != x86::kCallRel32 && Opcode != x86::kJmpRel32)
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

std::optional<uint64_t> elfBranchReferenceOffset(uint16_t Machine,
                                                 uint32_t Type,
                                                 ArrayRef<uint8_t> Function,
                                                 uint64_t Offset) {
  // Every branch relocation changes a word: a rel32 field or an instruction.
  if (Offset >= Function.size() || Function.size() - Offset < sizeof(uint32_t))
    return std::nullopt;
  switch (Machine) {
  case ELF::EM_386:
  case ELF::EM_X86_64: {
    const bool Rel32 =
        Machine == ELF::EM_386
            ? Type == ELF::R_386_PC32 || Type == ELF::R_386_PLT32
            : Type == ELF::R_X86_64_PC32 || Type == ELF::R_X86_64_PLT32;
    // As in COFF, the opcode before the field is what makes it a branch.
    if (!Rel32 || Offset == 0)
      return std::nullopt;
    const uint8_t Opcode = Function[Offset - x86::kRel32DispOffset];
    if (Opcode != x86::kCallRel32 && Opcode != x86::kJmpRel32)
      return std::nullopt;
    return Offset;
  }
  case ELF::EM_AARCH64:
    if (Type == ELF::R_AARCH64_CALL26 || Type == ELF::R_AARCH64_JUMP26)
      return Offset;
    return std::nullopt;
  case ELF::EM_ARM:
    if ((Type == ELF::R_ARM_THM_CALL || Type == ELF::R_ARM_THM_JUMP24) &&
        Offset % ThumbHalfwordBytes == 0)
      return Offset;
    // An ARM-state branch is stated one byte past its instruction, at an odd
    // offset no Thumb-2 instruction has.
    if ((Type == ELF::R_ARM_CALL || Type == ELF::R_ARM_JUMP24 ||
         Type == ELF::R_ARM_PLT32 || Type == ELF::R_ARM_PC24) &&
        Offset % ArmInstructionBytes == 0)
      return Offset + ArmStateReferenceMark;
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<uint64_t> machOBranchReferenceOffset(uint32_t CPUType,
                                                   uint32_t Type,
                                                   ArrayRef<uint8_t> Function,
                                                   uint64_t Offset) {
  // As in ELF, every branch relocation changes a word: a rel32 field or an
  // instruction.
  if (Offset >= Function.size() || Function.size() - Offset < sizeof(uint32_t))
    return std::nullopt;
  switch (CPUType) {
  case MachO::CPU_TYPE_ARM64:
    if (Type == MachO::ARM64_RELOC_BRANCH26)
      return Offset;
    return std::nullopt;
  case MachO::CPU_TYPE_X86_64: {
    // The opcode before the field is what makes it a branch.
    if (Type != MachO::X86_64_RELOC_BRANCH || Offset < x86::kRel32DispOffset)
      return std::nullopt;
    const uint8_t Opcode = Function[Offset - x86::kRel32DispOffset];
    if (Opcode != x86::kCallRel32 && Opcode != x86::kJmpRel32)
      return std::nullopt;
    return Offset;
  }
  }
  return std::nullopt;
}

namespace {

void emitPatternBytes(raw_ostream &OS, ArrayRef<uint8_t> Data,
                      ArrayRef<bool> Wildcard, size_t Begin, size_t End) {
  for (size_t I = Begin; I < End; ++I) {
    if (Wildcard[I])
      OS << UnstatedByte;
    else
      OS << format(ByteFormat, Data[I]);
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
  size_t End = std::min(Wildcard.size(), Start + MaxCRCLength);
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
void countOrEmit(raw_ostream &OS, ArrayRef<StringRef> Names,
                 ArrayRef<uint8_t> Data, ArrayRef<bool> Wildcard,
                 const PatternGeneratorOptions &Opts,
                 PatternGeneratorStats &Stats,
                 ArrayRef<FuncRef> References = {}) {
  if (Data.empty() || (!Opts.CollisionEvidence && !Opts.RequireCaller &&
                       Data.size() < Opts.MinFuncSize))
    ++Stats.TooSmall;
  else if (statedByteCount(Wildcard, Opts) < SignatureMatcher::MinStatedBytes &&
           !(Opts.RequireReferences && !References.empty()) &&
           !Opts.CollisionEvidence && !Opts.RequireCaller)
    ++Stats.TooWeak;
  else if (emitPatternLine(OS, Names, Data, Wildcard, Opts, References))
    ++Stats.Functions;
}

/// Whether a relocation target's name can be a reference: a routine's
/// linkage name, not a section (".text$mn"), a label ("$LN5") or a routine
/// a compiler synthesized.
bool isReferenceName(StringRef Name) {
  return !Name.empty() && !Name.starts_with(SectionSymbolPrefix) &&
         !Name.starts_with(LabelSymbolPrefix) &&
         !isSynthesizedRoutineName(Name);
}

/// Whether \p Name is an assembler's local label rather than a routine.
bool isLocalLabel(StringRef Name) {
  return llvm::any_of(LocalLabelPrefixes, [&](StringRef Prefix) {
    return Name.starts_with(Prefix);
  });
}

/// The routine an ELF relocation names, when a reference can name it: an
/// undefined symbol, which the object calls by its name, or a function
/// symbol.  Any other defined symbol -- a section, an object, a label of
/// hand-written assembly -- is no routine a line states.
StringRef elfReferenceName(const ELFObjectFileBase &Obj,
                           const RelocationRef &Rel) {
  const symbol_iterator Sym = Rel.getSymbol();
  if (Sym == Obj.symbol_end())
    return {};
  const ELFSymbolRef Target(*Sym);
  Expected<uint32_t> Flags = Target.getFlags();
  if (!Flags) {
    consumeError(Flags.takeError());
    return {};
  }
  const uint8_t Type = Target.getELFType();
  if (!(*Flags & SymbolRef::SF_Undefined) && Type != ELF::STT_FUNC &&
      Type != ELF::STT_GNU_IFUNC)
    return {};
  Expected<StringRef> Name = Target.getName();
  if (!Name) {
    consumeError(Name.takeError());
    return {};
  }
  return isReferenceName(*Name) ? *Name : StringRef();
}

/// Every symbol's address, per section, sorted: a function ends at the first
/// one after it.
std::map<SectionRef, std::vector<uint64_t>>
symbolAddressesBySection(const ObjectFile &Obj) {
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
  return SymbolAddresses;
}

/// One function symbol of an ELF or Mach-O object: where it starts in its
/// section, and its bytes up to the next symbol of any kind there.
struct GenericFunction {
  StringRef Name;
  SectionRef Section;
  uint64_t Offset;
  ArrayRef<uint8_t> Data;
  /// The size an ELF symbol states (st_size), or 0.
  uint64_t SymbolSize = 0;
};

/// Calls \p Visit for each function symbol of \p Obj, in symbol-table order,
/// but a routine a compiler synthesized, which \p Stats counts instead.
template <typename VisitorT>
void forEachGenericFunction(const ObjectFile &Obj, PatternGeneratorStats &Stats,
                            VisitorT Visit) {
  std::map<SectionRef, std::vector<uint64_t>> SymbolAddresses =
      symbolAddressesBySection(Obj);
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
    if (Name.empty() || isLocalLabel(Name))
      continue;
    if (isSynthesizedRoutineName(Name)) {
      ++Stats.Synthesized;
      continue;
    }

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

    ArrayRef<uint8_t> Data(
        reinterpret_cast<const uint8_t *>(Contents->data()) + Offset, FuncSize);
    uint64_t SymbolSize = 0;
    if (isa<ELFObjectFileBase>(&Obj))
      SymbolSize = ELFSymbolRef(Sym).getSize();
    Visit(GenericFunction{Name, *Sec, Offset, Data, SymbolSize});
  }
}

/// The reading objects of any other format keep: every relocation covers
/// OtherRelocationWidth bytes.
PatternGeneratorStats generateGeneric(const ObjectFile &Obj,
                                      const PatternGeneratorOptions &Opts,
                                      raw_ostream &OS) {
  PatternGeneratorStats Stats;
  forEachGenericFunction(Obj, Stats, [&](const GenericFunction &Fn) {
    SmallVector<bool, 256> Wildcard(Fn.Data.size(), false);
    for (const RelocationRef &Rel : Fn.Section.relocations()) {
      uint64_t RelOffset = Rel.getOffset() - Fn.Offset;
      if (RelOffset < Fn.Data.size())
        markWildcard(Wildcard, RelOffset, OtherRelocationWidth);
    }
    countOrEmit(OS, Fn.Name, Fn.Data, Wildcard, Opts, Stats);
  });
  return Stats;
}

/// An ELF object keeps a section's relocations in a section of their own
/// (SHT_REL or SHT_RELA) that names the section it applies to, so the code
/// section itself lists none. Each relocation leaves its footprint -- the
/// field, and the instruction bytes a linker may rewrite around it -- as
/// wildcards.
PatternGeneratorStats generateELF(const ELFObjectFileBase &Obj,
                                  const PatternGeneratorOptions &Opts,
                                  raw_ostream &OS) {
  PatternGeneratorStats Stats;
  const uint16_t Machine = Obj.getEMachine();

  struct ELFRelocation {
    uint64_t Offset;
    uint32_t Type;
    /// The routine it names, when a reference can name one.
    StringRef Routine;
  };
  std::map<SectionRef, std::vector<ELFRelocation>> Relocations;
  for (const SectionRef &Sec : Obj.sections()) {
    Expected<section_iterator> Target = Sec.getRelocatedSection();
    if (!Target) {
      consumeError(Target.takeError());
      continue;
    }
    if (*Target == Obj.section_end())
      continue;
    std::vector<ELFRelocation> &List = Relocations[**Target];
    for (const RelocationRef &Rel : Sec.relocations())
      List.push_back(
          {Rel.getOffset(), static_cast<uint32_t>(Rel.getType()),
           Opts.EmitReferences ? elfReferenceName(Obj, Rel) : StringRef()});
  }

  // The function symbols that label one address are one routine's names:
  // glibc's `puts` and `_IO_puts`, or a constructor's C1 and C2 symbols.
  struct Routine {
    GenericFunction Fn;
    SmallVector<StringRef, 2> Names;
    uint64_t Size = 0;
  };
  std::vector<Routine> Routines;
  std::map<std::pair<SectionRef, uint64_t>, size_t> ByStart;
  forEachGenericFunction(Obj, Stats, [&](const GenericFunction &Fn) {
    auto [It, Fresh] =
        ByStart.try_emplace({Fn.Section, Fn.Offset}, Routines.size());
    if (Fresh)
      Routines.push_back({Fn, {}});
    Routine &R = Routines[It->second];
    if (!llvm::is_contained(R.Names, Fn.Name))
      R.Names.push_back(Fn.Name);
    R.Size = std::max(R.Size, Fn.SymbolSize);
  });

  for (Routine &R : Routines) {
    llvm::sort(R.Names, [](StringRef A, StringRef B) {
      return preferredAliasOrder(A, B);
    });
    // A function ends where its symbol's size says, before the padding that
    // aligns what follows it: builds of a library pad the same code
    // differently, and a line that took the padding in would give the same
    // routine a different claim in each. With no size, or a symbol inside
    // it, the next symbol still ends it.
    if (R.Size && R.Size < R.Fn.Data.size())
      R.Fn.Data = R.Fn.Data.take_front(R.Size);
    const GenericFunction &Fn = R.Fn;
    const uint64_t Begin = Fn.Offset;
    const uint64_t End = Fn.Offset + Fn.Data.size();
    SmallVector<bool, 256> Wildcard(Fn.Data.size(), false);
    std::vector<FuncRef> References;
    bool Supported = true;
    if (auto It = Relocations.find(Fn.Section); It != Relocations.end()) {
      for (const ELFRelocation &Rel : It->second) {
        const std::optional<ELFRelocationFootprint> Footprint =
            elfRelocationFootprint(Machine, Rel.Type);
        if (!Footprint) {
          if (Rel.Offset >= Begin && Rel.Offset < End) {
            Supported = false;
            Stats.UnsupportedELFRelocations.insert({Machine, Rel.Type});
          }
          continue;
        }
        // The footprint may reach back past the function's start, or begin
        // in the function for a field that starts past its end.
        const uint64_t From =
            Rel.Offset > Footprint->Before ? Rel.Offset - Footprint->Before : 0;
        const uint64_t To = Rel.Offset + Footprint->Width;
        if (To <= Begin || From >= End)
          continue;
        const uint64_t Start = std::max(From, Begin);
        markWildcard(Wildcard, Start - Begin, std::min(To, End) - Start);
        // A branch to one of the routine's own names is recursion, not a
        // reference to anything the image has to confirm.
        if (Rel.Routine.empty() || Rel.Offset < Begin ||
            llvm::is_contained(R.Names, Rel.Routine))
          continue;
        if (const std::optional<uint64_t> At = elfBranchReferenceOffset(
                Machine, Rel.Type, Fn.Data, Rel.Offset - Begin))
          References.push_back({static_cast<uint32_t>(*At), Rel.Routine.str()});
      }
    }
    if (!Supported) {
      ++Stats.UnsupportedRelocation;
      continue;
    }
    // A call whose opcode another relocation's footprint covers is one the
    // linker may rewrite: `__tls_get_addr` in a general-dynamic TLS sequence
    // is gone once the sequence relaxes.  Only a branch the image keeps can
    // confirm anything.
    const bool OpcodeBeforeField =
        Machine == ELF::EM_386 || Machine == ELF::EM_X86_64;
    llvm::erase_if(References, [&](const FuncRef &Ref) {
      return OpcodeBeforeField && Wildcard[Ref.Offset - x86::kRel32DispOffset];
    });
    llvm::sort(References, [](const FuncRef &A, const FuncRef &B) {
      return A.Offset < B.Offset;
    });
    countOrEmit(OS, R.Names, Fn.Data, Wildcard, Opts, Stats, References);
  }
  return Stats;
}

/// The routine a Mach-O relocation names, when a reference can name it: an
/// undefined symbol, which the object calls by its name, or a function
/// symbol.  A relocation to a section offset names no symbol, and a local
/// label is no routine a line states.
StringRef machOReferenceName(const MachOObjectFile &Obj,
                             const RelocationRef &Rel) {
  const symbol_iterator Sym = Rel.getSymbol();
  if (Sym == Obj.symbol_end())
    return {};
  Expected<uint32_t> Flags = Sym->getFlags();
  if (!Flags) {
    consumeError(Flags.takeError());
    return {};
  }
  if (!(*Flags & SymbolRef::SF_Undefined)) {
    Expected<SymbolRef::Type> Type = Sym->getType();
    if (!Type) {
      consumeError(Type.takeError());
      return {};
    }
    if (*Type != SymbolRef::ST_Function)
      return {};
  }
  Expected<StringRef> Name = Sym->getName();
  if (!Name) {
    consumeError(Name.takeError());
    return {};
  }
  return isReferenceName(*Name) && !isLocalLabel(*Name) ? *Name : StringRef();
}

/// One linker optimization hint of a Mach-O object: its kind, and the
/// addresses of the instructions it names.
struct MachOOptimizationHint {
  uint32_t Kind;
  SmallVector<uint64_t, 3> Addresses;
};

/// The linker optimization hints of \p Obj, none when it has no
/// LC_LINKER_OPTIMIZATION_HINT, or std::nullopt when they cannot be read: a
/// ULEB128 runs past the data or overflows, or a hint of a known kind names
/// another number of instructions than the kind does.  A kind of 0 is the
/// padding after the last hint.
std::optional<std::vector<MachOOptimizationHint>>
readMachOOptimizationHints(const MachOObjectFile &Obj) {
  const MachO::linkedit_data_command Command = Obj.getLinkOptHintsLoadCommand();
  const StringRef Data =
      Obj.getData().substr(Command.dataoff, Command.datasize);
  if (Data.size() != Command.datasize)
    return std::nullopt;
  const uint8_t *Cursor = Data.bytes_begin();
  const uint8_t *End = Data.bytes_end();
  const char *Error = nullptr;
  auto Read = [&] { return decodeULEB128AndInc(Cursor, End, &Error); };
  std::vector<MachOOptimizationHint> Hints;
  while (Cursor != End) {
    const uint64_t Kind = Read();
    if (Error)
      return std::nullopt;
    if (Kind == 0)
      break;
    const uint64_t Count = Read();
    if (Error || Kind > UINT32_MAX ||
        (isValidMCLOHType(Kind) &&
         Count != static_cast<uint64_t>(
                      MCLOHIdToNbArgs(static_cast<MCLOHType>(Kind)))))
      return std::nullopt;
    MachOOptimizationHint Hint{static_cast<uint32_t>(Kind), {}};
    for (uint64_t I = 0; I < Count; ++I) {
      Hint.Addresses.push_back(Read());
      if (Error)
        return std::nullopt;
    }
    Hints.push_back(std::move(Hint));
  }
  return Hints;
}

/// A Mach-O object keeps a section's relocations with the section.  Each
/// relocation leaves its footprint as wildcards, and a function that holds
/// one machORelocationFootprint does not know -- a scattered one, which no
/// arm64 or x86-64 object has, or any of another CPU type -- is left out.
/// So does each instruction a linker optimization hint names, which the
/// linker may rewrite whole, relocated or not; a function holding one of a
/// hint machOOptimizationHintWidth does not know, or any function of an
/// object whose hints cannot be read, is left out.
PatternGeneratorStats generateMachO(const MachOObjectFile &Obj,
                                    const PatternGeneratorOptions &Opts,
                                    raw_ostream &OS) {
  PatternGeneratorStats Stats;
  const uint32_t CPUType = Obj.getHeader().cputype;

  // The instructions the hints name, by section.  A hint states addresses in
  // the object's address space, not offsets in a section as a relocation
  // does.
  struct HintedInstruction {
    uint64_t Offset;
    uint32_t Kind;
  };
  std::map<SectionRef, std::vector<HintedInstruction>> Hinted;
  const std::optional<std::vector<MachOOptimizationHint>> Hints =
      readMachOOptimizationHints(Obj);
  if (!Hints)
    ++Stats.UnreadableHints;
  else
    for (const MachOOptimizationHint &Hint : *Hints)
      for (const uint64_t Address : Hint.Addresses)
        for (const SectionRef &Sec : Obj.sections())
          if (Address >= Sec.getAddress() &&
              Address - Sec.getAddress() < Sec.getSize()) {
            Hinted[Sec].push_back({Address - Sec.getAddress(), Hint.Kind});
            break;
          }

  struct MachORelocation {
    uint64_t Offset;
    uint32_t Type;
    std::optional<RelocationFootprint> Footprint;
    /// The routine it names, when a reference can name one.
    StringRef Routine;
  };
  std::map<SectionRef, std::vector<MachORelocation>> Relocations;
  for (const SectionRef &Sec : Obj.sections()) {
    std::vector<MachORelocation> &List = Relocations[Sec];
    for (const RelocationRef &Rel : Sec.relocations()) {
      const MachO::any_relocation_info Info =
          Obj.getRelocation(Rel.getRawDataRefImpl());
      const uint32_t Type = Obj.getAnyRelocationType(Info);
      std::optional<RelocationFootprint> Footprint;
      if (!Obj.isRelocationScattered(Info))
        Footprint = machORelocationFootprint(CPUType, Type,
                                             Obj.getAnyRelocationLength(Info));
      const StringRef Routine = Opts.EmitReferences && Footprint
                                    ? machOReferenceName(Obj, Rel)
                                    : StringRef();
      List.push_back({Rel.getOffset(), Type, Footprint, Routine});
    }
  }

  // The function symbols that label one address are one routine's names.
  struct Routine {
    GenericFunction Fn;
    SmallVector<StringRef, 2> Names;
  };
  std::vector<Routine> Routines;
  std::map<std::pair<SectionRef, uint64_t>, size_t> ByStart;
  forEachGenericFunction(Obj, Stats, [&](const GenericFunction &Fn) {
    auto [It, Fresh] =
        ByStart.try_emplace({Fn.Section, Fn.Offset}, Routines.size());
    if (Fresh)
      Routines.push_back({Fn, {}});
    Routine &R = Routines[It->second];
    if (!llvm::is_contained(R.Names, Fn.Name))
      R.Names.push_back(Fn.Name);
  });

  for (Routine &R : Routines) {
    llvm::sort(R.Names, [](StringRef A, StringRef B) {
      return preferredAliasOrder(A, B);
    });
    const GenericFunction &Fn = R.Fn;
    const uint64_t Begin = Fn.Offset;
    const uint64_t End = Fn.Offset + Fn.Data.size();
    SmallVector<bool, 256> Wildcard(Fn.Data.size(), false);
    std::vector<FuncRef> References;
    bool Supported = true;
    for (const MachORelocation &Rel : Relocations[Fn.Section]) {
      if (!Rel.Footprint) {
        if (Rel.Offset >= Begin && Rel.Offset < End) {
          Supported = false;
          Stats.UnsupportedMachORelocations.insert({CPUType, Rel.Type});
        }
        continue;
      }
      // The footprint may reach back past the function's start, or begin in
      // the function for a field that starts past its end.
      const uint64_t From = Rel.Offset > Rel.Footprint->Before
                                ? Rel.Offset - Rel.Footprint->Before
                                : 0;
      const uint64_t To = Rel.Offset + Rel.Footprint->Width;
      if (To <= Begin || From >= End)
        continue;
      const uint64_t Start = std::max(From, Begin);
      markWildcard(Wildcard, Start - Begin, std::min(To, End) - Start);
      // A branch to one of the routine's own names is recursion, not a
      // reference to anything the image has to confirm.
      if (Rel.Routine.empty() || Rel.Offset < Begin ||
          llvm::is_contained(R.Names, Rel.Routine))
        continue;
      if (const std::optional<uint64_t> At = machOBranchReferenceOffset(
              CPUType, Rel.Type, Fn.Data, Rel.Offset - Begin))
        References.push_back({static_cast<uint32_t>(*At), Rel.Routine.str()});
    }
    if (!Supported) {
      ++Stats.UnsupportedRelocation;
      continue;
    }
    bool HintsKnown = Hints.has_value();
    if (auto It = Hinted.find(Fn.Section); HintsKnown && It != Hinted.end()) {
      for (const HintedInstruction &Instruction : It->second) {
        if (Instruction.Offset < Begin || Instruction.Offset >= End)
          continue;
        const std::optional<unsigned> Width =
            machOOptimizationHintWidth(CPUType, Instruction.Kind);
        if (!Width) {
          HintsKnown = false;
          Stats.UnsupportedMachOHints.insert({CPUType, Instruction.Kind});
          continue;
        }
        markWildcard(Wildcard, Instruction.Offset - Begin, *Width);
      }
    }
    if (!HintsKnown) {
      ++Stats.UnsupportedHint;
      continue;
    }
    // As in ELF, only a call whose opcode no other relocation's footprint
    // covers is one the image keeps.
    const bool OpcodeBeforeField = CPUType == MachO::CPU_TYPE_X86_64;
    llvm::erase_if(References, [&](const FuncRef &Ref) {
      return OpcodeBeforeField && Wildcard[Ref.Offset - x86::kRel32DispOffset];
    });
    llvm::sort(References, [](const FuncRef &A, const FuncRef &B) {
      return std::tie(A.Offset, A.Name) < std::tie(B.Offset, B.Name);
    });
    countOrEmit(OS, R.Names, Fn.Data, Wildcard, Opts, Stats, References);
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

bool hasCOFFFunctionType(const COFFSymbolRef &Sym) {
  if (Sym.getComplexType() != COFF::IMAGE_SYM_DTYPE_FUNCTION)
    return false;
  uint8_t Class = Sym.getStorageClass();
  if (Class != COFF::IMAGE_SYM_CLASS_EXTERNAL &&
      Class != COFF::IMAGE_SYM_CLASS_STATIC)
    return false;
  return true;
}

bool isCOFFFunctionSymbol(const COFFSymbolRef &Sym) {
  return hasCOFFFunctionType(Sym) &&
         !COFF::isReservedSectionNumber(Sym.getSectionNumber());
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
    SmallVector<StringRef, 2> Names;
  };
  std::vector<FunctionSymbol> Functions;
  DenseMap<std::pair<int32_t, uint32_t>, size_t> ByStart;
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
    // A routine a compiler synthesized still ends the function before it.
    It->second.FunctionStarts.push_back(Sym.getValue());
    if (isSynthesizedRoutineName(*NameOrErr)) {
      ++Stats.Synthesized;
      continue;
    }
    auto [At, Fresh] =
        ByStart.try_emplace({SectionNumber, Sym.getValue()}, Functions.size());
    if (Fresh)
      Functions.push_back({SectionNumber, Sym.getValue(), {}});
    auto &Names = Functions[At->second].Names;
    if (!llvm::is_contained(Names, *NameOrErr))
      Names.push_back(*NameOrErr);
  }
  for (auto &Entry : Sections) {
    std::vector<uint32_t> &Starts = Entry.second.FunctionStarts;
    llvm::sort(Starts);
    Starts.erase(std::unique(Starts.begin(), Starts.end()), Starts.end());
  }

  // Pass 2: symbols at the same section and offset are aliases, as with
  // memcpy/memmove in the x64 runtime. Keep the object's alias evidence in
  // one line, rather than turning it into competing byte-identical claims.
  // Each function runs to the next function in its section, or to
  // the section's end. Labels -- MSVC's `$LN` jump and handler targets --
  // are not function symbols, so they do not cut a function short.
  for (FunctionSymbol &Fn : Functions) {
    llvm::sort(Fn.Names, [](StringRef A, StringRef B) {
      return preferredAliasOrder(A, B);
    });
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
      std::optional<uint64_t> At = coffBranchReferenceOffset(
          Machine, Rel.Type, Sec.Contents.take_front(End), Rel.Offset,
          Fn.Offset);
      const uint64_t Field = Rel.Offset - Fn.Offset;
      const bool AddressReference =
          !At && Machine == COFF::IMAGE_FILE_MACHINE_AMD64 &&
          Rel.Type == COFF::IMAGE_REL_AMD64_REL32 &&
          isX64AddressReferenceField(Sec.Contents.slice(Fn.Offset, Size),
                                     Field) &&
          readLE<uint32_t>(Sec.Contents.data() + Rel.Offset) == 0;
      if (!At && !AddressReference)
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
      if (At &&
          (Machine == COFF::IMAGE_FILE_MACHINE_I386 ||
           Machine == COFF::IMAGE_FILE_MACHINE_AMD64) &&
          isX86ConditionalReferenceField(
              Sec.Contents.slice(Fn.Offset, Size), *At,
              Machine == COFF::IMAGE_FILE_MACHINE_AMD64) &&
          (!hasCOFFFunctionType(*TargetOrErr) ||
           TargetOrErr->getSectionNumber() < COFF::IMAGE_SYM_UNDEFINED))
        continue;
      if (AddressReference) {
        if (!coff_loader::primaryVTableTypeName(*TargetName) &&
            !(hasCOFFFunctionType(*TargetOrErr) &&
              TargetOrErr->getSectionNumber() >= COFF::IMAGE_SYM_UNDEFINED))
          continue;
        At = Field;
      }
      // Only a pointer symbol supplied by a COFF import can name the slot
      // of a memory-indirect call. A global function pointer is not an
      // import merely because its instruction has the same encoding.
      if ((Machine == COFF::IMAGE_FILE_MACHINE_I386 ||
           Machine == COFF::IMAGE_FILE_MACHINE_AMD64) &&
          isX86ImportReferenceField(Sec.Contents.slice(Fn.Offset, Size), *At,
                                    Machine ==
                                        COFF::IMAGE_FILE_MACHINE_AMD64) &&
          !TargetName->starts_with(COFFImportPointerPrefix))
        continue;
      // A branch to the function's own start is recursion, not a reference
      // to anything the image has to confirm.
      if (!isReferenceName(*TargetName) ||
          llvm::is_contained(Fn.Names, *TargetName))
        continue;
      References.push_back({static_cast<uint32_t>(*At), TargetName->str()});
      // Where no object defines the symbol, the link resolves it to its
      // alternate name, which the branch then reaches instead.
      if (const auto It = Opts.AlternateNames.find(*TargetName);
          It != Opts.AlternateNames.end())
        for (const std::string &Alternate : It->second)
          if (isReferenceName(Alternate) &&
              !llvm::is_contained(Fn.Names, StringRef(Alternate)))
            References.push_back({static_cast<uint32_t>(*At), Alternate});
    }
    if (!Supported) {
      ++Stats.UnsupportedRelocation;
      continue;
    }
    llvm::sort(References, [](const FuncRef &A, const FuncRef &B) {
      return std::tie(A.Offset, A.Name) < std::tie(B.Offset, B.Name);
    });
    References.erase(std::unique(References.begin(), References.end(),
                                 [](const FuncRef &A, const FuncRef &B) {
                                   return A.Offset == B.Offset &&
                                          A.Name == B.Name;
                                 }),
                     References.end());

    ArrayRef<uint8_t> Data = Sec.Contents.slice(Fn.Offset, Size);
    countOrEmit(OS, Fn.Names, Data, Wildcard, Opts, Stats, References);
  }
  return Stats;
}

} // anonymous namespace

void collectAlternateNames(
    const ObjectFile &Obj,
    std::map<std::string, std::vector<std::string>, std::less<>> &Names) {
  const auto *COFFObj = dyn_cast<COFFObjectFile>(&Obj);
  if (!COFFObj)
    return;
  for (const SectionRef &Sec : COFFObj->sections()) {
    Expected<StringRef> SecName = Sec.getName();
    if (!SecName) {
      consumeError(SecName.takeError());
      continue;
    }
    if (*SecName != section_names::coff::Drectve)
      continue;
    Expected<StringRef> Contents = Sec.getContents();
    if (!Contents) {
      consumeError(Contents.takeError());
      continue;
    }
    // See LinkerSyntax.def for the syntax of the directives.
    StringRef Rest = *Contents;
    Rest.consume_front(DirectiveByteOrderMark);
    while (!Rest.empty()) {
      Rest = Rest.ltrim(DirectiveSeparators);
      if (Rest.empty())
        break;
      StringRef Directive;
      if (Rest.front() == DirectiveQuote) {
        const size_t Close = Rest.find(DirectiveQuote, sizeof(DirectiveQuote));
        Directive = Rest.slice(sizeof(DirectiveQuote), Close);
        Rest = Close == StringRef::npos
                   ? StringRef()
                   : Rest.drop_front(Close + sizeof(DirectiveQuote));
      } else {
        const size_t End = Rest.find_first_of(DirectiveSeparators);
        Directive = Rest.take_front(End);
        Rest = End == StringRef::npos ? StringRef() : Rest.drop_front(End);
      }
      Directive = Directive.trim(DirectivePadding);
      if (Directive.empty() ||
          !DirectiveOptionPrefixes.contains(Directive.front()))
        continue;
      Directive = Directive.drop_front();
      if (!Directive.consume_front_insensitive(AlternateNameOption))
        continue;
      const auto [Symbol, Alternate] = Directive.split(AlternateNameSeparator);
      if (Symbol.empty() || Alternate.empty())
        continue;
      std::vector<std::string> &List = Names[Symbol.str()];
      if (!llvm::is_contained(List, Alternate))
        List.push_back(Alternate.str());
    }
  }
}

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

bool emitPatternLine(raw_ostream &OS, ArrayRef<StringRef> Names,
                     ArrayRef<uint8_t> Data, ArrayRef<bool> Wildcard,
                     const PatternGeneratorOptions &Opts,
                     ArrayRef<FuncRef> References) {
  const size_t Size = Data.size();
  const bool RequireReferences = Opts.RequireReferences && !References.empty();
  if (Names.empty() || Size == 0 ||
      (!Opts.CollisionEvidence && !Opts.RequireCaller &&
       Size < Opts.MinFuncSize) ||
      Wildcard.size() != Size ||
      (!RequireReferences && !Opts.CollisionEvidence && !Opts.RequireCaller &&
       statedByteCount(Wildcard, Opts) < SignatureMatcher::MinStatedBytes))
    return false;

  const size_t LeadBytes = std::min(static_cast<size_t>(Opts.LeadingLen), Size);
  emitPatternBytes(OS, Data, Wildcard, 0, LeadBytes);

  const size_t CRCStart = LeadBytes;
  const size_t CRCLen = crcSpan(Wildcard, CRCStart);
  uint16_t CRC = 0;
  if (CRCLen > 0)
    CRC = SignatureMatcher::computeCRC16(Data.data() + CRCStart, CRCLen);

  OS << format(CRCFieldsFormat, static_cast<unsigned>(CRCLen), CRC,
               static_cast<unsigned>(Size));
  if (Opts.CollisionEvidence)
    OS << FieldSeparator << CollisionEvidenceOnly;
  else if (RequireReferences)
    OS << FieldSeparator << RequiredReferences;
  else if (Opts.RequireCaller)
    OS << FieldSeparator << RequiredCaller;
  // Every name labels the function's start.
  for (StringRef Name : Names)
    OS << FieldSeparator << PublicNamePrefix << format(OffsetFormat, 0u)
       << FieldSeparator << Name;
  for (const FuncRef &Ref : References)
    OS << FieldSeparator << ReferencePrefix
       << format(OffsetFormat, static_cast<unsigned>(Ref.Offset))
       << FieldSeparator << Ref.Name;

  // Everything the CRC had to stop short of, stated byte by byte so that a
  // wildcard can stand where a relocation does.  This is what lets a match
  // cover a whole function rather than its first invariant run, which is the
  // difference between a name worth displaying and a name worth acting on.
  const size_t TailStart = CRCStart + CRCLen;
  const size_t TailEnd =
      RequireReferences || Opts.CollisionEvidence || Opts.RequireCaller
          ? Size
          : std::min(Size, TailStart + static_cast<size_t>(Opts.TailLen));
  if (TailEnd > TailStart) {
    OS << FieldSeparator;
    emitPatternBytes(OS, Data, Wildcard, TailStart, TailEnd);
  }

  OS << LineEnd;
  return true;
}

PatternGeneratorStats generatePatterns(const ObjectFile &Obj,
                                       const PatternGeneratorOptions &Opts,
                                       raw_ostream &OS) {
  if (const auto *COFF = dyn_cast<COFFObjectFile>(&Obj))
    return generateCOFF(*COFF, Opts, OS);
  if (const auto *ELFObj = dyn_cast<ELFObjectFileBase>(&Obj))
    return generateELF(*ELFObj, Opts, OS);
  if (const auto *MachOObj = dyn_cast<MachOObjectFile>(&Obj))
    return generateMachO(*MachOObj, Opts, OS);
  return generateGeneric(Obj, Opts, OS);
}

} // namespace sigs
} // namespace neverd
