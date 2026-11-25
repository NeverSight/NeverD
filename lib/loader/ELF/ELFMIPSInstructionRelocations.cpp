//===- ELFMIPSInstructionRelocations.cpp - Retained MIPS fields ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate retained HI/LO/HIGHER/HIGHEST fields against linked bytes.
//===----------------------------------------------------------------------===//

#include "ELFMIPSInstructionRelocations.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ELF/MIPSCodeMode.h"

#include "llvm/Support/Endian.h"

#include <map>
#include <tuple>
#include <vector>

namespace neverd::elf_loader {

void collectMIPSInstructionRelocations(BinaryImage &Image) {
  if (!Image.MIPS || Image.IsRelocatable || Image.Relocations.empty())
    return;
  using namespace llvm::ELF;
  std::map<va_t, unsigned> Claims;
  for (const auto &R : Image.Relocations)
    if (R.Type != R_MIPS_NONE)
      ++Claims[R.Address];

  std::map<va_t, MIPSInstructionRelocation> Fields;
  using SymbolKey = std::tuple<uint64_t, uint32_t, uint32_t>;
  std::map<SymbolKey, std::vector<va_t>> PendingHigh;
  for (const auto &R : Image.Relocations) {
    const auto GPPart = mipsPackedGPDisplacement(R.Type);
    std::optional<MIPSImmediatePart> Part = GPPart;
    if (!GPPart) {
      switch (R.Type) {
#define NEVERD_MIPS_IMMEDIATE_RELOCATION(Name, Type, Shift, Carry)             \
  case Type:                                                                   \
    Part = MIPSImmediatePart::Name;                                            \
    break;
#include "neverd/loader/ELF/MIPSImmediateRelocations.def"
      default:
        continue;
      }
    }
    const bool FunctionSymbol =
        R.ELF && R.ELF->Symbol && R.ELF->Symbol->isFunction();
    if (!GPPart && !FunctionSymbol)
      continue;
    auto &Field = Fields[R.Address];
    Field.Address = R.Address;
    if (!FunctionSymbol ||
        (GPPart && (!Image.MIPS->PICGP || !R.HasExplicitAddend)))
      continue;
    const auto &Symbol = *R.ELF->Symbol;
    const auto *Bytes = Image.readVA(R.Address, 4);
    if (Claims[R.Address] != 1 || !Symbol.isDefined() || !Bytes ||
        extractionOf(*Part).Shift >= 8 * Image.getPointerSize() ||
        R.Address % 4 || !Image.isCodeRange(R.Address, 4) ||
        !mips::codeModeRangeMatches(Image, R.Address, 4,
                                    InstructionMode::MIPSClassic))
      continue;
    const uint32_t Word = archLittleEndian(Image.Arch)
                              ? llvm::support::endian::read32le(Bytes)
                              : llvm::support::endian::read32be(Bytes);
    const unsigned Opcode = Word >> 26;
    if ((Opcode != 15 && Opcode != 9 && Opcode != 25 && Opcode != 13) ||
        (Opcode == 15 && ((Word >> 21) & 31)) ||
        (Opcode == 25 && !isMIPS64(Image.Arch)))
      continue;
    const bool MIPS16 = (Symbol.Other & 0xf0) == STO_MIPS_MIPS16;
    const bool Micro = !MIPS16 && (Symbol.Other & STO_MIPS_MICROMIPS);
    const auto Mode = MIPS16  ? InstructionMode::MIPS16
                      : Micro ? InstructionMode::MicroMIPS
                              : InstructionMode::MIPSClassic;
    const va_t Entry = MIPS16 || Micro ? Symbol.Value & ~va_t{1} : Symbol.Value;
    if (!Image.isCodeAddress(Entry) ||
        !mips::codeModeRangeMatches(Image, Entry, 1, Mode))
      continue;
    const va_t Value = serializeCodePointer(Entry, Image.Arch, Mode);
    auto Candidate =
        MIPSSymbolImmediate{Value, R.Addend, Word, *Part, GPPart.has_value()};
    if (R.HasExplicitAddend) {
      const uint64_t Absolute = Value + uint64_t(R.Addend);
      const uint64_t Expression =
          GPPart ? *Image.MIPS->PICGP - Absolute : Absolute;
      if (extractionOf(*Part).apply(Expression) == uint16_t(Word))
        Field.Value = Candidate;
      continue;
    }
    // ELF32 REL has already consumed its addend. LO16 needs only the modular
    // low displacement. HI16 additionally needs the matching following LO16
    // in relocation-table order, not instruction/address order. Several HI16
    // records can share that LO16. Never invent an absent pair.
    if (Image.getPointerSize() != 4)
      continue;
    const SymbolKey Key{R.ELF->TableFileOffset, Symbol.TableSectionIndex,
                        R.SymbolIndex};
    if (*Part == MIPSImmediatePart::Hi16) {
      Field.Value = Candidate;
      PendingHigh[Key].push_back(R.Address);
    } else if (*Part == MIPSImmediatePart::Lo16) {
      Candidate.Addend = int16_t(uint16_t(Word) - uint16_t(Value));
      Field.Value = Candidate;
      const auto Pending = PendingHigh.find(Key);
      if (Pending == PendingHigh.end())
        continue;
      for (va_t Address : Pending->second) {
        auto &High = Fields[Address].Value;
        const uint32_t Linked = (High->Instruction << 16) + int16_t(Word);
        High->Addend = int32_t(Linked - uint32_t(Value));
      }
      PendingHigh.erase(Pending);
    }
  }
  for (const auto &[Key, Addresses] : PendingHigh)
    for (va_t Address : Addresses)
      Fields[Address].Value.reset();
  auto &Output = Image.MIPS->InstructionRelocations;
  Output.reserve(Fields.size());
  for (auto &[Address, Field] : Fields)
    Output.push_back(std::move(Field));
}

} // namespace neverd::elf_loader
