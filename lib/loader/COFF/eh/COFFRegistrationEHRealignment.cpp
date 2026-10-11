//===- COFFRegistrationEHRealignment.cpp - x86 aligned frame decoding ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include <algorithm>

namespace neverd::coff_loader::registration_detail {

/// Authenticate LLVM's realigned ESI frame and the complete publication of
/// its C++ registration node. This proves coordinates, not CFG transfers.
bool proveRealignedCxxRegistrationLayout(const BinaryImage &Img,
                                         const InstallSite &Site,
                                         RegistrationChainInfo &Chain) {
  const size_t Size = std::min<uint64_t>(Site.Range.size(), 128);
  const uint8_t *P = Img.readVA(Site.Range.Begin, Size);
  if (!P || Size < 17 || P[0] != 0x55 || P[1] != 0x89 || P[2] != 0xe5 ||
      P[3] != 0x53 || P[4] != 0x57 || P[5] != 0x56 || P[6] != 0x83 ||
      P[7] != 0xe4 || P[9] != 0x81 || P[10] != 0xec || P[15] != 0x89 ||
      P[16] != 0xe6)
    return false;
  const uint32_t Alignment = uint32_t(-int32_t(int8_t(P[8])));
  const uint32_t Allocation = readLE<uint32_t>(P + 11);
  if (Alignment < 4 || Alignment > 128 || (Alignment & (Alignment - 1)) ||
      Allocation < 20 || Allocation > limits::kMaxRegistrationEHStateWork)
    return false;

  size_t Cursor = 17;
  auto Operand = [&](uint8_t Opcode,
                     uint8_t Register) -> std::optional<int32_t> {
    if (Cursor + 2 > Size || P[Cursor] != Opcode)
      return std::nullopt;
    const uint8_t ModRM = P[Cursor + 1];
    const unsigned Mod = ModRM >> 6;
    const size_t DispBytes = Mod == 1 ? 1 : Mod == 2 ? 4 : 0;
    if (!DispBytes || (ModRM & 7) != 6 || ((ModRM >> 3) & 7) != Register ||
        Cursor + 2 + DispBytes > Size)
      return std::nullopt;
    const int32_t Offset = DispBytes == 1 ? int8_t(P[Cursor + 2])
                                          : readLE<int32_t>(P + Cursor + 2);
    Cursor += 2 + DispBytes;
    return Offset;
  };
  auto Immediate = [&]() -> std::optional<uint32_t> {
    if (Cursor + 4 > Size)
      return std::nullopt;
    const uint32_t Value = readLE<uint32_t>(P + Cursor);
    Cursor += 4;
    return Value;
  };
  const auto SavedParent = Operand(0x89, 5);
  if (!SavedParent)
    return false;
  // Register allocation can schedule incoming argument loads between the
  // parent-frame spill and node initialization, and spill those values into
  // private locals. ESI, EBP and ESP retain their distinct frame coordinates;
  // the state solver replays every actual access.
  while (Cursor + 3 <= Size && (P[Cursor] == 0x8b || P[Cursor] == 0x89)) {
    const bool Load = P[Cursor] == 0x8b;
    const uint8_t ModRM = P[Cursor + 1];
    const unsigned Register = (ModRM >> 3) & 7;
    const unsigned Mod = ModRM >> 6;
    const unsigned Width = Mod == 1 ? 1 : Mod == 2 ? 4 : 0;
    if (!Width || (ModRM & 7) != (Load ? 5 : 6) || Register == 4 ||
        Register == 5 || Register == 6 || Cursor + 2 + Width > Size)
      break;
    const int32_t Offset =
        Width == 1 ? int8_t(P[Cursor + 2]) : readLE<int32_t>(P + Cursor + 2);
    if (Load ? Offset < 8 : Offset < 0 || int64_t(Offset) + 4 > *SavedParent)
      break;
    Cursor += 2 + Width;
  }
  const auto SavedSP = Operand(0x89, 4);
  const auto State = Operand(0xc7, 0);
  const auto Seed = Immediate();
  if (Cursor + 2 > Size)
    return false;
  const uint8_t NodeRegister = (P[Cursor + 1] >> 3) & 7;
  if (NodeRegister == 4 || NodeRegister == 5 || NodeRegister == 6)
    return false;
  const auto Link = Operand(0x8d, NodeRegister);
  const auto Handler = Operand(0xc7, 0);
  const auto HandlerVA = Immediate();
  if (!SavedParent || !SavedSP || !State || !Seed || !Link || !Handler ||
      !HandlerVA || *Seed != UINT32_MAX || *HandlerVA != Site.HandlerVA ||
      *Link < 8 || int64_t(*Link) + 12 > Allocation ||
      int64_t(*SavedParent) != int64_t(*Link) - 8 ||
      int64_t(*SavedSP) != int64_t(*Link) - 4 ||
      int64_t(*Handler) != int64_t(*Link) + 4 ||
      int64_t(*State) != int64_t(*Link) + 8 || Cursor + 6 > Size ||
      Site.InstallVA != Site.Range.Begin + Cursor || P[Cursor] != 0x64)
    return false;
  uint8_t LinkRegister = 0;
  if (P[Cursor + 1] == 0xa1 && readLE<uint32_t>(P + Cursor + 2) == 0)
    Cursor += 6;
  else if (Cursor + 7 <= Size && P[Cursor + 1] == 0x8b &&
           (P[Cursor + 2] & 0xc7) == 0x05 &&
           readLE<uint32_t>(P + Cursor + 3) == 0) {
    LinkRegister = (P[Cursor + 2] >> 3) & 7;
    Cursor += 7;
  } else
    return false;
  if (LinkRegister == NodeRegister || LinkRegister == 4 || LinkRegister == 5 ||
      LinkRegister == 6)
    return false;
  const auto Previous = Operand(0x89, LinkRegister);
  const auto InstallSize =
      getX86RegistrationChainStoreSize(Img, Site.Range.Begin + Cursor);
  if (!Previous || *Previous != *Link || !InstallSize ||
      Cursor + *InstallSize > Size ||
      (*InstallSize == 6 && NodeRegister != 0) ||
      (*InstallSize == 7 && ((P[Cursor + 2] >> 3) & 7) != NodeRegister))
    return false;

  Chain.RealignedFrame = RegistrationRealignedFrame{
      6, Site.Range.Begin + 15, Alignment, Allocation, -(*Link + 12), -20};
  Chain.RegistrationOffset = -12;
  Chain.TryLevelOffset = -4;
  Chain.ChainInstallVA = Site.Range.Begin + Cursor;
  return true;
}

} // namespace neverd::coff_loader::registration_detail
