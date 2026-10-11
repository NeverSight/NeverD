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
  // Parameter loads and private ECX/EDX spills can precede SavedESP. They do
  // not change ESP/EBP or any runtime cell. Preserve their real instructions
  // for the state solver; this only establishes the registration coordinates.
  while (Cursor + 3 <= Size) {
    const uint8_t Opcode = P[Cursor], ModRM = P[Cursor + 1];
    const unsigned Register = (ModRM >> 3) & 7;
    const unsigned Mod = ModRM >> 6;
    const unsigned Width = Mod == 1 ? 1 : Mod == 2 ? 4 : 0;
    if (!Width || (ModRM & 7) != 5 || Cursor + 2 + Width > Size)
      break;
    const int32_t Offset =
        Width == 1 ? int8_t(P[Cursor + 2]) : readLE<int32_t>(P + Cursor + 2);
    const bool ArgumentLoad =
        Opcode == 0x8b && Register != 4 && Register != 5 && Offset >= 8;
    const bool ArgumentSpill =
        Opcode == 0x89 && (Register == 1 || Register == 2) && Offset <= -32 &&
        int64_t(Offset) >= -12 - int64_t(Allocation);
    if (!ArgumentLoad && !ArgumentSpill)
      break;
    Cursor += 2 + Width;
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
  // Unoptimized Clang first copies ESP to EAX. Admit only the adjacent copy
  // and matching store; the later node LEA then replaces this EAX definition.
  if (Cursor + 2 <= Size && P[Cursor] == 0x89 && P[Cursor + 1] == 0xe0) {
    Cursor += 2;
    if (!Operand(0x89, 0, -28))
      return false;
  } else if (!Operand(0x89, 4, -28)) {
    return false;
  }
  if (!Operand(0xc7, 0, -16) || !Immediate(UINT32_MAX))
    return false;
  const bool EarlyNode = Operand(0x8d, 0, -24);
  if (!Operand(0xc7, 0, -20) || !Immediate(Site.HandlerVA) ||
      Site.InstallVA != Site.Range.Begin + Cursor)
    return false;
  uint8_t LinkRegister = 0;
  if (EarlyNode) {
    if (Cursor + 7 > Size || P[Cursor] != 0x64 || P[Cursor + 1] != 0x8b ||
        (P[Cursor + 2] & 0xc7) != 0x05 || readLE<uint32_t>(P + Cursor + 3) != 0)
      return false;
    LinkRegister = (P[Cursor + 2] >> 3) & 7;
    // EAX holds the node; ESP and EBP retain their frame coordinates.
    if (LinkRegister == 0 || LinkRegister == 4 || LinkRegister == 5)
      return false;
    Cursor += 7;
  } else {
    // Optimized Clang reads the old chain into EAX before computing the node.
    if (Cursor + 6 > Size || P[Cursor] != 0x64 || P[Cursor + 1] != 0xa1 ||
        readLE<uint32_t>(P + Cursor + 2) != 0)
      return false;
    Cursor += 6;
  }
  if (!Operand(0x89, LinkRegister, -24) ||
      (!EarlyNode && !Operand(0x8d, 0, -24)) || Cursor + 6 > Size ||
      P[Cursor] != 0x64 || P[Cursor + 1] != 0xa3 ||
      readLE<uint32_t>(P + Cursor + 2) != 0)
    return false;
  Chain.RegistrationOffset = -24;
  Chain.TryLevelOffset = -16;
  Chain.ChainInstallVA = Site.Range.Begin + Cursor;
  return true;
}

} // namespace neverd::coff_loader::registration_detail
