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
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
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
  UnsupportedELFRelocations.insert(Other.UnsupportedELFRelocations.begin(),
                                   Other.UnsupportedELFRelocations.end());
  return *this;
}

std::optional<TargetMachine> parseTargetMachine(StringRef Name) {
  if (Name == "x86")
    return TargetMachine::X86;
  if (Name == "x64")
    return TargetMachine::X64;
  if (Name == "arm")
    return TargetMachine::ARM;
  if (Name == "arm64")
    return TargetMachine::ARM64;
  return std::nullopt;
}

bool isObjectForMachine(const ObjectFile &Obj, TargetMachine Machine) {
  if (const auto *COFF = dyn_cast<COFFObjectFile>(&Obj)) {
    switch (Machine) {
    case TargetMachine::X86:
      return COFF->getMachine() == COFF::IMAGE_FILE_MACHINE_I386;
    case TargetMachine::X64:
      return COFF->getMachine() == COFF::IMAGE_FILE_MACHINE_AMD64;
    case TargetMachine::ARM:
      return COFF->getMachine() == COFF::IMAGE_FILE_MACHINE_ARMNT;
    case TargetMachine::ARM64:
      return COFF->getMachine() == COFF::IMAGE_FILE_MACHINE_ARM64;
    }
    return false;
  }
  if (const auto *ELFObj = dyn_cast<ELFObjectFileBase>(&Obj)) {
    const bool Is64 = Obj.getBytesInAddress() == 8;
    switch (Machine) {
    case TargetMachine::X86:
      return !Is64 && ELFObj->getEMachine() == ELF::EM_386;
    case TargetMachine::X64:
      return Is64 && ELFObj->getEMachine() == ELF::EM_X86_64;
    case TargetMachine::ARM:
      return !Is64 && ELFObj->getEMachine() == ELF::EM_ARM;
    case TargetMachine::ARM64:
      return Is64 && ELFObj->getEMachine() == ELF::EM_AARCH64;
    }
  }
  return false;
}

std::optional<ELFRelocationFootprint> elfRelocationFootprint(uint16_t Machine,
                                                             uint32_t Type) {
  auto Field = [](unsigned Width) { return ELFRelocationFootprint{0, Width}; };
  switch (Machine) {
  case ELF::EM_X86_64:
    switch (Type) {
    case ELF::R_X86_64_NONE:
      return Field(0);
    case ELF::R_X86_64_8:
    case ELF::R_X86_64_PC8:
      return Field(1);
    case ELF::R_X86_64_16:
    case ELF::R_X86_64_PC16:
      return Field(2);
    case ELF::R_X86_64_PC32:
    case ELF::R_X86_64_GOT32:
    case ELF::R_X86_64_PLT32:
    case ELF::R_X86_64_GOTPCREL:
    case ELF::R_X86_64_32:
    case ELF::R_X86_64_32S:
    case ELF::R_X86_64_DTPOFF32:
    case ELF::R_X86_64_TPOFF32:
    case ELF::R_X86_64_GOTPC32:
    case ELF::R_X86_64_SIZE32:
      return Field(4);
    case ELF::R_X86_64_64:
    case ELF::R_X86_64_DTPMOD64:
    case ELF::R_X86_64_DTPOFF64:
    case ELF::R_X86_64_TPOFF64:
    case ELF::R_X86_64_PC64:
    case ELF::R_X86_64_GOTOFF64:
    case ELF::R_X86_64_GOT64:
    case ELF::R_X86_64_GOTPCREL64:
    case ELF::R_X86_64_GOTPC64:
    case ELF::R_X86_64_GOTPLT64:
    case ELF::R_X86_64_PLTOFF64:
    case ELF::R_X86_64_SIZE64:
      return Field(8);
    // A relaxable GOT load: the opcode and ModRM ahead of the field become
    // those of `lea`, a direct `call`/`jmp` (with a prefix or a trailing
    // `nop`) or an immediate form. The REX variants reach one byte further
    // back to the REX prefix, the CODE_4 ones to a two-byte REX2 prefix.
    case ELF::R_X86_64_GOTPCRELX:
      return ELFRelocationFootprint{2, 4};
    case ELF::R_X86_64_REX_GOTPCRELX:
    // Initial-exec and descriptor loads become immediate moves when the
    // variable is the executable's own.
    case ELF::R_X86_64_GOTTPOFF:
    case ELF::R_X86_64_GOTPC32_TLSDESC:
      return ELFRelocationFootprint{3, 4};
    case ELF::R_X86_64_CODE_4_GOTPCRELX:
    case ELF::R_X86_64_CODE_4_GOTTPOFF:
    case ELF::R_X86_64_CODE_4_GOTPC32_TLSDESC:
      return ELFRelocationFootprint{4, 4};
    case ELF::R_X86_64_CODE_6_GOTTPOFF:
      return ELFRelocationFootprint{6, 4};
    // The descriptor call `call *(%rax)` becomes a two-byte no-op.
    case ELF::R_X86_64_TLSDESC_CALL:
      return Field(2);
    // General dynamic: `.byte 0x66; lea x@tlsgd(%rip),%rdi` and a call to
    // __tls_get_addr, 16 bytes with the field at 4, all rewritten.
    case ELF::R_X86_64_TLSGD:
      return ELFRelocationFootprint{4, 12};
    // Local dynamic: `lea x@tlsld(%rip),%rdi` and the call, 12 bytes (13
    // with `call *__tls_get_addr@GOTPCREL(%rip)`) with the field at 3.
    case ELF::R_X86_64_TLSLD:
      return ELFRelocationFootprint{3, 10};
    }
    return std::nullopt;

  case ELF::EM_386:
    switch (Type) {
    case ELF::R_386_NONE:
      return Field(0);
    case ELF::R_386_8:
    case ELF::R_386_PC8:
      return Field(1);
    case ELF::R_386_16:
    case ELF::R_386_PC16:
      return Field(2);
    case ELF::R_386_32:
    case ELF::R_386_PC32:
    case ELF::R_386_GOT32:
    case ELF::R_386_PLT32:
    case ELF::R_386_GOTOFF:
    case ELF::R_386_GOTPC:
    case ELF::R_386_32PLT:
    case ELF::R_386_TLS_TPOFF:
    case ELF::R_386_TLS_LE:
    case ELF::R_386_TLS_LDO_32:
    case ELF::R_386_TLS_LE_32:
    case ELF::R_386_TLS_DTPMOD32:
    case ELF::R_386_TLS_DTPOFF32:
    case ELF::R_386_TLS_TPOFF32:
      return Field(4);
    // Relaxable GOT loads and initial-exec or descriptor loads: the opcode
    // and ModRM ahead of the field change with the instruction.
    case ELF::R_386_GOT32X:
    case ELF::R_386_TLS_IE:
    case ELF::R_386_TLS_GOTIE:
    case ELF::R_386_TLS_IE_32:
    case ELF::R_386_TLS_GOTDESC:
      return ELFRelocationFootprint{2, 4};
    case ELF::R_386_TLS_DESC_CALL:
      return Field(2);
    // General dynamic: `lea x@tlsgd(,%ebx,1),%eax` (the field at 3) and a
    // call to ___tls_get_addr, direct or through the GOT.
    case ELF::R_386_TLS_GD:
      return ELFRelocationFootprint{3, 10};
    // Local dynamic: `lea x@tlsldm(%ebx),%eax` (the field at 2) and the call.
    case ELF::R_386_TLS_LDM:
      return ELFRelocationFootprint{2, 10};
    }
    return std::nullopt;

  case ELF::EM_AARCH64:
    switch (Type) {
    case ELF::R_AARCH64_NONE:
      return Field(0);
    case ELF::R_AARCH64_ABS16:
    case ELF::R_AARCH64_PREL16:
      return Field(2);
    case ELF::R_AARCH64_ABS32:
    case ELF::R_AARCH64_PREL32:
    case ELF::R_AARCH64_GOTREL32:
    case ELF::R_AARCH64_PLT32:
    case ELF::R_AARCH64_GOTPCREL32:
      return Field(4);
    case ELF::R_AARCH64_ABS64:
    case ELF::R_AARCH64_PREL64:
    case ELF::R_AARCH64_GOTREL64:
    case ELF::R_AARCH64_AUTH_ABS64:
      return Field(8);
    }
    // Every other static relocation patches one instruction, and TLS
    // relaxation rewrites only instructions that carry one of their own
    // (TLSDESC_CALL marks the `blr` that becomes a `nop`).
    if ((Type >= ELF::R_AARCH64_MOVW_UABS_G0 &&
         Type <= ELF::R_AARCH64_LD64_GOTPAGE_LO15) ||
        Type == ELF::R_AARCH64_PATCHINST ||
        (Type >= ELF::R_AARCH64_TLSGD_ADR_PREL21 &&
         Type <= ELF::R_AARCH64_TLSLD_LDST128_DTPREL_LO12_NC) ||
        (Type >= ELF::R_AARCH64_AUTH_MOVW_GOTOFF_G0 &&
         Type <= ELF::R_AARCH64_AUTH_TLSDESC_ADD_LO12))
      return Field(4);
    return std::nullopt;

  case ELF::EM_ARM:
    switch (Type) {
    case ELF::R_ARM_NONE:
    case ELF::R_ARM_GNU_VTENTRY:
    case ELF::R_ARM_GNU_VTINHERIT:
      return Field(0);
    case ELF::R_ARM_ABS8:
      return Field(1);
    // Data halfwords, and the 16-bit Thumb instructions.
    case ELF::R_ARM_ABS16:
    case ELF::R_ARM_THM_ABS5:
    case ELF::R_ARM_THM_PC8:
    case ELF::R_ARM_THM_SWI8:
    case ELF::R_ARM_THM_JUMP6:
    case ELF::R_ARM_THM_JUMP11:
    case ELF::R_ARM_THM_JUMP8:
    case ELF::R_ARM_THM_TLS_DESCSEQ16:
    case ELF::R_ARM_THM_ALU_ABS_G0_NC:
    case ELF::R_ARM_THM_ALU_ABS_G1_NC:
    case ELF::R_ARM_THM_ALU_ABS_G2_NC:
    case ELF::R_ARM_THM_ALU_ABS_G3:
      return Field(2);
    // The dynamic types and the platform-private ones.
    case ELF::R_ARM_TLS_DESC:
    case ELF::R_ARM_TLS_DTPMOD32:
    case ELF::R_ARM_TLS_DTPOFF32:
    case ELF::R_ARM_TLS_TPOFF32:
    case ELF::R_ARM_COPY:
    case ELF::R_ARM_GLOB_DAT:
    case ELF::R_ARM_JUMP_SLOT:
    case ELF::R_ARM_RELATIVE:
    case ELF::R_ARM_IRELATIVE:
      return std::nullopt;
    }
    // Every other static relocation patches a data word, an ARM instruction
    // or a 32-bit Thumb instruction -- including V4BX, whose `bx` a linker
    // for ARMv4 rewrites, and BL, which interworking turns into BLX.
    if (Type <= ELF::R_ARM_TLS_IE12GP || Type == ELF::R_ARM_THM_TLS_DESCSEQ32 ||
        (Type >= ELF::R_ARM_THM_BF16 && Type <= ELF::R_ARM_THM_BF18))
      return Field(4);
    return std::nullopt;
  }
  return std::nullopt;
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
void countOrEmit(raw_ostream &OS, ArrayRef<StringRef> Names,
                 ArrayRef<uint8_t> Data, ArrayRef<bool> Wildcard,
                 const PatternGeneratorOptions &Opts,
                 PatternGeneratorStats &Stats,
                 ArrayRef<FuncRef> References = {}) {
  if (Data.size() < Opts.MinFuncSize)
    ++Stats.TooSmall;
  else if (statedByteCount(Wildcard, Opts) < SignatureMatcher::MinStatedBytes)
    ++Stats.TooWeak;
  else if (emitPatternLine(OS, Names, Data, Wildcard, Opts, References))
    ++Stats.Functions;
}

/// Whether a relocation target's name can be a reference: a routine's
/// linkage name, not a section (".text$mn") or a label ("$LN5").
bool isReferenceName(StringRef Name) {
  return !Name.empty() && !Name.starts_with(".") && !Name.starts_with("$");
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

/// Calls \p Visit for each function symbol of \p Obj, in symbol-table order.
template <typename VisitorT>
void forEachGenericFunction(const ObjectFile &Obj, VisitorT Visit) {
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

    ArrayRef<uint8_t> Data(
        reinterpret_cast<const uint8_t *>(Contents->data()) + Offset, FuncSize);
    uint64_t SymbolSize = 0;
    if (isa<ELFObjectFileBase>(&Obj))
      SymbolSize = ELFSymbolRef(Sym).getSize();
    Visit(GenericFunction{Name, *Sec, Offset, Data, SymbolSize});
  }
}

/// The reading Mach-O objects keep: every relocation covers four bytes.
PatternGeneratorStats generateGeneric(const ObjectFile &Obj,
                                      const PatternGeneratorOptions &Opts,
                                      raw_ostream &OS) {
  PatternGeneratorStats Stats;
  forEachGenericFunction(Obj, [&](const GenericFunction &Fn) {
    SmallVector<bool, 256> Wildcard(Fn.Data.size(), false);
    for (const RelocationRef &Rel : Fn.Section.relocations()) {
      uint64_t RelOffset = Rel.getOffset() - Fn.Offset;
      if (RelOffset < Fn.Data.size())
        markWildcard(Wildcard, RelOffset, 4);
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
      List.push_back({Rel.getOffset(), static_cast<uint32_t>(Rel.getType())});
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
  forEachGenericFunction(Obj, [&](const GenericFunction &Fn) {
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
      }
    }
    if (!Supported) {
      ++Stats.UnsupportedRelocation;
      continue;
    }
    countOrEmit(OS, R.Names, Fn.Data, Wildcard, Opts, Stats);
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

bool emitPatternLine(raw_ostream &OS, ArrayRef<StringRef> Names,
                     ArrayRef<uint8_t> Data, ArrayRef<bool> Wildcard,
                     const PatternGeneratorOptions &Opts,
                     ArrayRef<FuncRef> References) {
  const size_t Size = Data.size();
  if (Names.empty() || Size < Opts.MinFuncSize || Wildcard.size() != Size ||
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
  for (StringRef Name : Names)
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
  if (const auto *ELFObj = dyn_cast<ELFObjectFileBase>(&Obj))
    return generateELF(*ELFObj, Opts, OS);
  return generateGeneric(Obj, Opts, OS);
}

} // namespace sigs
} // namespace neverd
