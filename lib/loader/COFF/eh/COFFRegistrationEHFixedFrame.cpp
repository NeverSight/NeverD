//===- COFFRegistrationEHFixedFrame.cpp - x86 fixed C++ frames -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove fixed EBP coordinates and exact publication of a C++ runtime node.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include <algorithm>

namespace neverd::coff_loader::registration_detail {

bool proveFixedCxxRegistrationLayout(const BinaryImage &Img,
                                     const InstallSite &Site,
                                     RegistrationChainInfo &Chain) {
  const size_t Size = std::min<uint64_t>(Site.Range.size(), 256);
  const uint8_t *P = Img.readVA(Site.Range.Begin, Size);
  if (!P || Site.HandlerVA > UINT32_MAX || Size < 9 || P[0] != 0x55 ||
      P[1] != 0x89 || P[2] != 0xe5 || P[3] != 0x53 || P[4] != 0x57 ||
      P[5] != 0x56 || P[7] != 0xec)
    return false;
  size_t Cursor = 9;
  uint32_t Allocation = 0;
  if (P[6] == 0x83 && int8_t(P[8]) > 0)
    Allocation = P[8];
  else if (P[6] == 0x81 && Size >= 12) {
    Allocation = readLE<uint32_t>(P + 8);
    Cursor = 12;
  }
  if (Allocation < 16 || Allocation > limits::kMaxRegistrationEHStateWork)
    return false;
  // Parameter copies, loads and private ECX/EDX spills can precede SavedESP.
  // They do not change ESP/EBP or any runtime cell. Preserve their real
  // instructions for the state solver; this only proves frame coordinates.
  while (Cursor + 2 <= Size) {
    const size_t Prefix = P[Cursor] == 0x66 ? 1 : 0;
    const size_t Instruction = Cursor + Prefix;
    if (Instruction + 2 > Size)
      break;
    const uint8_t Opcode = P[Instruction], ModRM = P[Instruction + 1];
    const bool Byte = Opcode == 0x8a || Opcode == 0x88;
    if (Prefix && Byte)
      break;
    const unsigned AccessWidth = Byte ? 1 : Prefix ? 2 : 4;
    const unsigned Register = (ModRM >> 3) & 7;
    const unsigned Mod = ModRM >> 6;
    if (Mod == 3 && (Opcode == 0x88 || Opcode == 0x89 || Opcode == 0x8a ||
                     Opcode == 0x8b)) {
      const bool Load = Opcode == 0x8a || Opcode == 0x8b;
      const unsigned Source = Load ? ModRM & 7 : Register;
      const unsigned Destination = Load ? Register : ModRM & 7;
      if ((Source != 1 && Source != 2) ||
          (!Byte && (Destination == 4 || Destination == 5)))
        break;
      Cursor = Instruction + 2;
      continue;
    }
    const unsigned DisplacementWidth = Mod == 1 ? 1 : Mod == 2 ? 4 : 0;
    if (!DisplacementWidth || (ModRM & 7) != 5 ||
        Instruction + 2 + DisplacementWidth > Size)
      break;
    const int32_t Offset = DisplacementWidth == 1
                               ? int8_t(P[Instruction + 2])
                               : readLE<int32_t>(P + Instruction + 2);
    // In a byte MOV, register encodings 4/5 name AH/CH, not SP/BP.
    const bool ArgumentLoad = (Opcode == 0x8b || Opcode == 0x8a) &&
                              (Byte || (Register != 4 && Register != 5)) &&
                              Offset >= 8 &&
                              int64_t(Offset) + AccessWidth <= INT32_MAX;
    const bool ArgumentSpill = (Opcode == 0x89 || Opcode == 0x88) &&
                               (Register == 1 || Register == 2) &&
                               int64_t(Offset) + AccessWidth <= -28 &&
                               int64_t(Offset) >= -12 - int64_t(Allocation);
    if (!ArgumentLoad && !ArgumentSpill)
      break;
    Cursor = Instruction + 2 + DisplacementWidth;
  }
  auto Operand = [&](uint8_t Opcode, uint8_t Register, int8_t Offset) {
    if (Cursor + 3 > Size || P[Cursor] != Opcode ||
        P[Cursor + 1] != (0x45 | (Register << 3)) ||
        int8_t(P[Cursor + 2]) != Offset)
      return false;
    Cursor += 3;
    return true;
  };
  auto Immediate = [&](uint32_t Value) {
    if (Cursor + 4 > Size || readLE<uint32_t>(P + Cursor) != Value)
      return false;
    Cursor += 4;
    return true;
  };
  // SavedESP, state, node address and handler must all precede publication.
  // A handler literal or an FS read in unrelated code proves none of these.
  // Unoptimized Clang may copy ESP to a temporary. Admit only a full-width
  // adjacent copy and matching store, preserving both frame coordinates.
  if (Cursor + 2 <= Size && P[Cursor] == 0x89 &&
      (P[Cursor + 1] & 0xf8) == 0xe0) {
    const uint8_t Register = P[Cursor + 1] & 7;
    if (Register == 4 || Register == 5)
      return false;
    Cursor += 2;
    if (!Operand(0x89, Register, -28))
      return false;
  } else if (!Operand(0x89, 4, -28)) {
    return false;
  }
  if (!Operand(0xc7, 0, -16) || !Immediate(UINT32_MAX))
    return false;
  auto NodeAddress = [&]() -> std::optional<uint8_t> {
    if (Cursor + 3 > Size)
      return std::nullopt;
    const uint8_t Register = (P[Cursor + 1] >> 3) & 7;
    if (Register == 4 || Register == 5 || !Operand(0x8d, Register, -24))
      return std::nullopt;
    return Register;
  };
  auto NodeRegister = NodeAddress();
  const bool EarlyNode = NodeRegister.has_value();
  if (!Operand(0xc7, 0, -20) || !Immediate(Site.HandlerVA) ||
      Site.InstallVA != Site.Range.Begin + Cursor)
    return false;
  uint8_t LinkRegister = 0;
  if (Cursor + 6 > Size || P[Cursor] != 0x64)
    return false;
  if (P[Cursor + 1] == 0xa1 && readLE<uint32_t>(P + Cursor + 2) == 0)
    Cursor += 6;
  else if (Cursor + 7 <= Size && P[Cursor + 1] == 0x8b &&
           (P[Cursor + 2] & 0xc7) == 0x05 &&
           readLE<uint32_t>(P + Cursor + 3) == 0) {
    LinkRegister = (P[Cursor + 2] >> 3) & 7;
    Cursor += 7;
  } else
    return false;
  if (LinkRegister == 4 || LinkRegister == 5 ||
      (EarlyNode && LinkRegister == *NodeRegister) ||
      !Operand(0x89, LinkRegister, -24))
    return false;
  if (!EarlyNode)
    NodeRegister = NodeAddress();
  const auto InstallSize =
      getX86RegistrationChainStoreSize(Img, Site.Range.Begin + Cursor);
  if (!NodeRegister || !InstallSize || Cursor + *InstallSize > Size ||
      (*InstallSize == 6 && *NodeRegister != 0) ||
      (*InstallSize == 7 && ((P[Cursor + 2] >> 3) & 7) != *NodeRegister))
    return false;
  Chain.RegistrationOffset = -24;
  Chain.TryLevelOffset = -16;
  Chain.ChainInstallVA = Site.Range.Begin + Cursor;
  return true;
}

} // namespace neverd::coff_loader::registration_detail
