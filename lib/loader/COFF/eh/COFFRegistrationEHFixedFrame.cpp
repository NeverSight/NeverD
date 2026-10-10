//===- COFFRegistrationEHFixedFrame.cpp - x86 fixed C++ frames -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/Limits.h"

#include <algorithm>

namespace neverd::coff_loader::registration_detail {

bool proveFixedCxxRegistrationLayout(const BinaryImage &Img,
                                     const InstallSite &Site,
                                     RegistrationChainInfo &Chain) {
  const size_t Size = std::min<uint64_t>(Site.Range.size(), 96);
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
  if (!Operand(0x89, 4, -28) || !Operand(0xc7, 0, -16) ||
      !Immediate(UINT32_MAX) || !Operand(0x8d, 0, -24) ||
      !Operand(0xc7, 0, -20) || !Immediate(Site.HandlerVA) ||
      Cursor + 7 > Size || Site.InstallVA != Site.Range.Begin + Cursor ||
      P[Cursor] != 0x64 || P[Cursor + 1] != 0x8b ||
      (P[Cursor + 2] != 0x0d && P[Cursor + 2] != 0x15) ||
      readLE<uint32_t>(P + Cursor + 3) != 0)
    return false;
  const uint8_t LinkRegister = (P[Cursor + 2] >> 3) & 7;
  Cursor += 7;
  if (!Operand(0x89, LinkRegister, -24) || Cursor + 6 > Size ||
      P[Cursor] != 0x64 || P[Cursor + 1] != 0xa3 ||
      readLE<uint32_t>(P + Cursor + 2) != 0)
    return false;
  Chain.RegistrationOffset = -24;
  Chain.TryLevelOffset = -16;
  Chain.ChainInstallVA = Site.Range.Begin + Cursor;
  return true;
}

} // namespace neverd::coff_loader::registration_detail
