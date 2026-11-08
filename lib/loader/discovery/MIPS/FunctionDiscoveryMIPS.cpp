//===- FunctionDiscoveryMIPS.cpp - Classic MIPS ELF import veneers -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recognize the complete GNU/lld absolute PLT entry, including the delay
/// slot that supplies the lazy resolver's cell address. A known import slot
/// supplies the callable identity; matching instruction bytes alone do not.
//===----------------------------------------------------------------------===//

#include "../FunctionDiscoveryDetail.h"

#include "neverd/ArchSupport.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {

std::optional<va_t> mipsImportThunkSlot(const BinaryImage &Img, va_t Address) {
  if (!Img.isELF() || Address % 4)
    return std::nullopt;
  const auto Mode = Img.instructionModeAt(Address);
  if (!Mode ||
      (*Mode != InstructionMode::Default &&
       *Mode != InstructionMode::MIPSClassic) ||
      Img.instructionModeAt(Address + 15, *Mode) != Mode)
    return std::nullopt;
  const uint8_t *Bytes = Img.readVA(Address, 16);
  if (!Bytes)
    return std::nullopt;
  const bool Little = archLittleEndian(Img.Arch);
  const uint32_t High = readTargetEndian<uint32_t>(Bytes, Little);
  const uint32_t Load = readTargetEndian<uint32_t>(Bytes + 4, Little);
  const uint32_t Jump = readTargetEndian<uint32_t>(Bytes + 8, Little);
  const uint32_t Slot = readTargetEndian<uint32_t>(Bytes + 12, Little);
  // lui r15,hi; lw/ld t9,lo(r15); jr t9; addiu/daddiu t8,r15,lo
  const bool Wide = isMIPS64(Img.Arch);
  if ((High & 0xffff0000u) != 0x3c0f0000u ||
      (Load & 0xffff0000u) != (Wide ? 0xddf90000u : 0x8df90000u) ||
      Jump != 0x03200008u ||
      (Slot & 0xffff0000u) != (Wide ? 0x65f80000u : 0x25f80000u) ||
      uint16_t(Load) != uint16_t(Slot) ||
      !Img.hasExecutableCodeOwnerRange(Address, 16))
    return std::nullopt;
  const uint64_t Base = uint64_t(int64_t(int32_t(High << 16)));
  const uint64_t Cell = Base + uint64_t(int64_t(int16_t(Load)));
  return Wide ? Cell : uint32_t(Cell);
}

size_t scanImportThunksMIPS(BinaryImage &Img, const Segment &Seg,
                            const std::map<va_t, size_t> &Targets,
                            std::set<va_t> &Existing,
                            const ImportThunkCandidates &Candidates) {
  size_t Added = 0;
  for (size_t Offset = (4 - Seg.VA % 4) % 4;
       Offset <= Seg.Data.size() && Seg.Data.size() - Offset >= 16;
       Offset += 4) {
    const va_t Address = Seg.VA + Offset;
    const auto Slot = mipsImportThunkSlot(Img, Address);
    if (!Slot)
      continue;
    const auto Owner = Targets.find(*Slot);
    if (Owner == Targets.end() || !Candidates.allows(Address, 16))
      continue;
    Img.recordImportStub(Address, Owner->second);
    if (Existing.insert(Address).second) {
      Img.Symbols.push_back(Symbol::makeFunc(Address, 16));
      ++Added;
    }
  }
  return Added;
}

} // namespace neverd
