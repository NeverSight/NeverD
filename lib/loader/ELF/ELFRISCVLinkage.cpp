//===- ELFRISCVLinkage.cpp - RISC-V ELF jump slots and PLT entries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The default psABI PLT loads a pointer from a PC-relative jump slot. Its
// resolver also derives a relocation index from the return address in t1.
// Both routes must select the same import before a caller can name it.
//
//===----------------------------------------------------------------------===//

#include "ELFIntegerProfile.h"

#include "neverd/loader/BinaryImageModel.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/AddressRanges.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>

namespace neverd::elf_loader {
namespace {
#include "RISCVPLT.inc"

va_t pcRelative(va_t PC, uint32_t Upper, uint32_t Load, bool Wide) {
  const va_t VA = PC + llvm::SignExtend64<32>(Upper & ~UFormMask) +
                  llvm::SignExtend64<LowBits>(Load >> (32 - LowBits));
  return Wide ? VA : uint32_t(VA);
}

bool isHeader(const BinaryImage &Image, va_t VA) {
  const auto *P = Image.readVA(VA, HeaderSize);
  const auto *Seg = Image.getSegmentFor(VA);
  if (!P || !Seg || !Seg->isExecutable() || VA > InvalidVA - HeaderSize ||
      VA % (Image.RISCV.hasCompressed() ? Word / 2 : Word) ||
      !Image.isCodeRange(VA, HeaderSize))
    return false;
  const bool Wide = Image.is64Bit();
  auto Insn = [&](unsigned N) { return readLE<uint32_t>(P + N * Word); };
  const uint32_t Upper = Insn(0), Load = Insn(2);
  return (Upper & UFormMask) == AuipcT2 && Insn(1) == SubT1T1T3 &&
         (Load & IFormMask) == (Wide ? LdT3T2 : LwT3T2) &&
         pcRelative(VA, Upper, Load, Wide) == Image.DynInfo.PltGotAddr &&
         Insn(3) == AdjustT1 && Insn(4) == ((Load & ~IFormMask) | AddiT0T2) &&
         Insn(5) == (Wide ? SrliT1RV64 : SrliT1RV32) &&
         Insn(6) == (Wide ? LdMap : LwMap) && Insn(7) == JumpT3;
}

std::optional<va_t> findHeader(const BinaryImage &Image, va_t FirstSlot) {
  // Both GNU ld and LLD put PLT0 in unbound slots. A section or a pointer
  // only locates a candidate. The code must compute the runtime DT_PLTGOT.
  if (const Section *Sec = Image.getPltSection();
      Sec && isHeader(Image, Sec->VA))
    return Sec->VA;
  if (const auto *P = Image.readVA(FirstSlot, Image.getPointerSize())) {
    const va_t Target = readPtr(P, Image.is64Bit());
    if (Target && isHeader(Image, Target))
      return Target;
  }
  // Eager binding does not require the file to initialize the jump slots.
  // A sectionless file can still identify its resolver by its instructions.
  for (const auto &Seg : Image.Segments) {
    if (!Seg.isExecutable())
      continue;
    const size_t Size = std::min<uint64_t>(Seg.FileSz, Seg.Data.size());
    if (Size < HeaderSize)
      continue;
    const unsigned Align = Image.RISCV.hasCompressed() ? Word / 2 : Word;
    // Compare a batch of fixed AUIPC/SUB prefixes before checking any GOT
    // address. Repeated bytes in compressed instructions are common.
    auto Matches = [&](size_t Pos) {
      return (readLE<uint64_t>(Seg.Data.data() + Pos) & HeaderPrefixMask) ==
             HeaderPrefix;
    };
    const size_t Last = Size - HeaderSize;
    size_t Off = -Seg.VA % Align;
    for (; Off <= Last && Last - Off >= (ScanStarts - 1) * Align;
         Off += ScanStarts * Align) {
      bool Found = false;
      for (unsigned I = 0; I < ScanStarts; ++I)
        Found |= Matches(Off + I * Align);
      if (!Found)
        continue;
      for (unsigned I = 0; I < ScanStarts; ++I) {
        const size_t Pos = Off + I * Align;
        if (Matches(Pos) && isHeader(Image, Seg.VA + Pos))
          return Seg.VA + Pos;
      }
    }
    for (; Off <= Last; Off += Align)
      if (Matches(Off) && isHeader(Image, Seg.VA + Off))
        return Seg.VA + Off;
  }
  return std::nullopt;
}

// Other dynamic writes can affect more than their first byte. COPY holds
// the symbol's full extent, and a TLS descriptor holds two XLEN words.
uint64_t writeSize(const RelocationEntry &R, unsigned PtrSize) {
  using namespace llvm::ELF;
  switch (R.Type) {
  case R_RISCV_NONE:
    return 0;
  case R_RISCV_COPY:
    return R.ELF->Symbol ? R.ELF->Symbol->Size : 0;
  case R_RISCV_TLSDESC:
    return 2 * PtrSize;
#define NEVERD_RISCV_FIXED_WRITE(Type, Bytes)                                  \
  case Type:                                                                   \
    return Bytes;
#include "ELFDynamicRelocations.def"
  default:
    return PtrSize;
  }
}
} // namespace

llvm::Error readRISCVLinkage(BinaryImage &Image,
                             llvm::ArrayRef<va_t> RelrSlots) {
  using namespace llvm::ELF;
  const unsigned PtrSize = Image.getPointerSize();
  const bool Wide = Image.is64Bit();
  llvm::SmallDenseSet<va_t, 8> Slots;
  llvm::SmallVector<std::pair<uint64_t, size_t>> Entries;
  for (const auto &R : Image.Relocations) {
    if (!R.ELF || R.ELF->Source != ELFRelocationSource::ProgramDynamicTable ||
        R.SectionName != section_names::elf::RelaPlt ||
        R.Type != R_RISCV_JUMP_SLOT)
      continue;
    const Segment *Seg = Image.getSegmentFor(R.Address);
    if (R.Address % PtrSize || R.Address > InvalidVA - PtrSize || !Seg ||
        !Seg->isWritable() ||
        !rangeInBounds(R.Address - Seg->VA, PtrSize, Seg->Size) ||
        !Image.isDataAddress(R.Address))
      return llvm::createStringError("elf: invalid RISC-V jump slot extent");
    if (!Slots.insert(R.Address).second)
      return llvm::createStringError("elf: duplicate RISC-V jump slot");
    const auto &Sym = R.ELF->Symbol;
    if (!Sym || R.SymbolName.empty())
      continue;
    const size_t I = Image.Imports.size();
    Image.Imports.push_back({kExternModule.str(), R.SymbolName, 0, R.Address});
    if ((Sym->isFunction() || Sym->Type == STT_NOTYPE) &&
        (Sym->Binding == STB_GLOBAL || Sym->Binding == STB_WEAK) &&
        Sym->Other == STV_DEFAULT)
      Entries.emplace_back(R.ELF->Ordinal, I);
  }
  if (Entries.empty() || !Image.DynInfo.PltGotAddr)
    return llvm::Error::success();

  llvm::SmallVector<llvm::AddressRange> Fields;
  for (const auto &R : Image.Relocations) {
    if (!R.ELF || R.ELF->Source != ELFRelocationSource::ProgramDynamicTable ||
        (R.Type == R_RISCV_JUMP_SLOT &&
         R.SectionName == section_names::elf::RelaPlt))
      continue;
    const uint64_t Width = writeSize(R, PtrSize);
    if (Width > InvalidVA - R.Address)
      return llvm::createStringError("elf: RISC-V relocation extent overflows");
    if (Width)
      Fields.emplace_back(R.Address, R.Address + Width);
  }
  for (va_t Slot : RelrSlots) {
    if (Slot > InvalidVA - PtrSize)
      return llvm::createStringError("elf: RISC-V RELR extent overflows");
    Fields.emplace_back(Slot, Slot + PtrSize);
  }
  llvm::sort(Fields);
  llvm::AddressRanges Writes;
  for (const auto &Field : Fields)
    Writes.insert(Field);
  auto Overlaps = [&](va_t VA, unsigned Size) {
    auto It = llvm::partition_point(
        Writes, [=](const llvm::AddressRange &R) { return R.end() <= VA; });
    return It != Writes.end() && It->start() < VA + Size;
  };
  for (va_t Slot : Slots)
    if (Overlaps(Slot, PtrSize))
      return llvm::createStringError(
          "elf: conflicting writer to RISC-V jump slot");
  auto Header =
      findHeader(Image, Image.Imports[Entries.front().second].IATAddr);
  if (!Header || Overlaps(*Header, HeaderSize))
    return llvm::Error::success();

  for (auto [Ordinal, Index] : Entries) {
    if (Ordinal >= (InvalidVA - *Header - HeaderSize) / EntrySize)
      continue;
    const va_t VA = *Header + HeaderSize + Ordinal * EntrySize;
    const uint8_t *P = Image.readVA(VA, EntrySize);
    if (!P || !Image.isCodeRange(VA, EntrySize) || Overlaps(VA, EntrySize))
      continue;
    const uint32_t Upper = readLE<uint32_t>(P);
    const uint32_t Load = readLE<uint32_t>(P + Word);
    if ((Upper & UFormMask) != AuipcT3 ||
        (Load & IFormMask) != (Wide ? LdT3T3 : LwT3T3) ||
        readLE<uint32_t>(P + 2 * Word) != LinkT1T3 ||
        readLE<uint32_t>(P + 3 * Word) != Nop ||
        pcRelative(VA, Upper, Load, Wide) != Image.Imports[Index].IATAddr)
      continue;
    Image.recordImportStub(VA, Index);
  }
  return llvm::Error::success();
}
} // namespace neverd::elf_loader
