//===- COFFRegistrationEHRealignment.cpp - x86 aligned frame decoding ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/Limits.h"

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

namespace neverd::coff_loader::registration_detail {

namespace {

/// MSVC retains entry EBP while constructing its three-word node below an
/// independently aligned ESP. Do not infer their relationship from a state
/// literal or an FS read: authenticate the entire straight-line publication.
std::optional<RegistrationInstallation>
decodeStackRealignedCxxRegistrationLayout(const BinaryImage &Img,
                                          const InstallSite &Site) {
  const size_t Size = std::min<uint64_t>(
      Site.Range.size(),
      ::neverd::limits::loader::
          kCOFFRegistrationEHRealignmentProveRealignedCxxRegistrationLayoutSizeMinArgument2);
  const uint8_t *P = Img.readVA(Site.Range.Begin, Size);
  if (!P || Site.HandlerVA > UINT32_MAX)
    return std::nullopt;
  size_t Cursor = 0;
  auto Bytes = [&](std::initializer_list<uint8_t> Expected) {
    if (Cursor > Size || Expected.size() > Size - Cursor ||
        !std::equal(Expected.begin(), Expected.end(), P + Cursor))
      return false;
    Cursor += Expected.size();
    return true;
  };
  if (Size >= 2 && P[0] == 0x8b && P[1] == 0xff)
    Cursor = 2;
  if (!Bytes({0x55}) || Cursor + 2 > Size ||
      !((P[Cursor] == 0x8b && P[Cursor + 1] == 0xec) ||
        (P[Cursor] == 0x89 && P[Cursor + 1] == 0xe5)))
    return std::nullopt;
  Cursor += 2;
  const va_t Definition = Site.Range.Begin + Cursor;
  if (Cursor + 3 > Size || P[Cursor + 1] != 0xe4)
    return std::nullopt;
  uint32_t Mask = 0;
  if (P[Cursor] == 0x83) {
    Mask = uint32_t(int32_t(int8_t(P[Cursor + 2])));
    Cursor += 3;
  } else if (P[Cursor] == 0x81 && Cursor + 6 <= Size) {
    Mask = readLE<uint32_t>(P + Cursor + 2);
    Cursor += 6;
  } else
    return std::nullopt;
  const uint32_t Alignment = uint32_t{0} - Mask;
  if (Alignment < limits::kMinRegistrationFrameAlignment ||
      Alignment > limits::kMaxRegistrationFrameAlignment ||
      (Alignment & (Alignment - 1)) || !Bytes({0x6a, 0xff, 0x68}) ||
      Cursor + 4 > Size || readLE<uint32_t>(P + Cursor) != Site.HandlerVA)
    return std::nullopt;
  const va_t HandlerPointer = Site.Range.Begin + Cursor;
  std::vector<va_t> AddressOperands{HandlerPointer};
  Cursor += 4;
  const va_t ChainRead = Site.Range.Begin + Cursor;
  if (!(Bytes({0x64, 0xa1, 0, 0, 0, 0}) ||
        Bytes({0x64, 0x8b, 0x05, 0, 0, 0, 0})) ||
      !Bytes({0x50}) || Cursor + 3 > Size)
    return std::nullopt;
  uint32_t Allocation = 0;
  if (P[Cursor] == 0x83 && P[Cursor + 1] == 0xec &&
      int8_t(P[Cursor + 2]) >= 0) {
    Allocation = P[Cursor + 2];
    Cursor += 3;
  } else if (Cursor + 6 <= Size && P[Cursor] == 0x81 && P[Cursor + 1] == 0xec) {
    Allocation = readLE<uint32_t>(P + Cursor + 2);
    Cursor += 6;
  } else
    return std::nullopt;
  if (Allocation > limits::kMaxSourceCallBorrowedBytes)
    return std::nullopt;
  unsigned Saved = 0, SavedBytes = 0;
  while (Cursor < Size &&
         (P[Cursor] == 0x53 || P[Cursor] == 0x56 || P[Cursor] == 0x57)) {
    const unsigned Bit = 1u << (P[Cursor] - 0x50);
    if (Saved & Bit)
      return std::nullopt;
    Saved |= Bit;
    SavedBytes += 4;
    ++Cursor;
  }
  unsigned CookieBytes = 0;
  if (Cursor < Size && P[Cursor] == 0xa1) {
    if (Cursor + 8 > Size ||
        !((P[Cursor + 5] == 0x33 && P[Cursor + 6] == 0xc4) ||
          (P[Cursor + 5] == 0x31 && P[Cursor + 6] == 0xe0)) ||
        P[Cursor + 7] != 0x50)
      return std::nullopt;
    AddressOperands.push_back(Site.Range.Begin + Cursor + 1);
    Cursor += 8;
    CookieBytes = 4;
  }
  const uint64_t Depth = uint64_t(Allocation) + 12 + SavedBytes + CookieBytes;
  if (Depth > limits::kMaxSourceCallBorrowedBytes || Depth > INT32_MAX ||
      Cursor + 4 > Size || P[Cursor] != 0x8d || P[Cursor + 2] != 0x24)
    return std::nullopt;
  int32_t Node = -1;
  if (P[Cursor + 1] == 0x44) {
    Node = int8_t(P[Cursor + 3]);
    Cursor += 4;
  } else if (P[Cursor + 1] == 0x84 && Cursor + 7 <= Size) {
    Node = readLE<int32_t>(P + Cursor + 3);
    Cursor += 7;
  }
  const auto Store =
      getX86RegistrationChainStoreSize(Img, Site.Range.Begin + Cursor);
  if (Node < 0 || uint64_t(Node) != Depth - 12 || !Store ||
      *Store > Size - Cursor || (*Store == 7 && P[Cursor + 2] != 0x05) ||
      !readImmutablePE32CodeBytes(Img, Site.Range.Begin, Cursor + *Store,
                                  AddressOperands))
    return std::nullopt;
  return RegistrationInstallation{
      ChainRead,
      HandlerPointer,
      Site.Range.Begin + Cursor,
      -12,
      -4,
      RegistrationRealignedFrame{4, Definition, Alignment, uint32_t(Depth),
                                 -int32_t(Depth), 0},
      std::move(AddressOperands)};
}

} // namespace

/// Authenticate a supported realigned local frame and complete C++ node
/// publication. This proves coordinates, not CFG transfers.
std::optional<RegistrationInstallation>
decodeRealignedCxxRegistrationLayout(const BinaryImage &Img,
                                     const InstallSite &Site) {
  if (auto Stack = decodeStackRealignedCxxRegistrationLayout(Img, Site))
    return Stack;
  const size_t Size = std::min<uint64_t>(
      Site.Range.size(),
      ::neverd::limits::loader::
          kCOFFRegistrationEHRealignmentProveRealignedCxxRegistrationLayoutSizeMinArgument2);
  const uint8_t *P = Img.readVA(Site.Range.Begin, Size);
  if (!P || Size < 17 || P[0] != 0x55 || P[1] != 0x89 || P[2] != 0xe5 ||
      P[3] != 0x53 || P[4] != 0x57 || P[5] != 0x56 || P[6] != 0x83 ||
      P[7] != 0xe4 || P[9] != 0x81 || P[10] != 0xec || P[15] != 0x89 ||
      P[16] != 0xe6)
    return std::nullopt;
  const uint32_t Alignment = uint32_t(-int32_t(int8_t(P[8])));
  const uint32_t Allocation = readLE<uint32_t>(P + 11);
  if (Alignment < limits::kMinRegistrationFrameAlignment ||
      Alignment > limits::kMaxRegistrationFrameAlignment ||
      (Alignment & (Alignment - 1)) || Allocation < 20 ||
      Allocation > limits::kMaxSourceCallBorrowedBytes)
    return std::nullopt;

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
    return std::nullopt;
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
    return std::nullopt;
  const uint8_t NodeRegister = (P[Cursor + 1] >> 3) & 7;
  if (NodeRegister == 4 || NodeRegister == 5 || NodeRegister == 6)
    return std::nullopt;
  const auto Link = Operand(0x8d, NodeRegister);
  const auto Handler = Operand(0xc7, 0);
  const va_t HandlerOperand = Site.Range.Begin + Cursor;
  const auto HandlerVA = Immediate();
  const va_t ChainRead = Site.Range.Begin + Cursor;
  if (!SavedParent || !SavedSP || !State || !Seed || !Link || !Handler ||
      !HandlerVA || *Seed != UINT32_MAX || *HandlerVA != Site.HandlerVA ||
      *Link < 8 || int64_t(*Link) + 12 > Allocation ||
      int64_t(*SavedParent) != int64_t(*Link) - 8 ||
      int64_t(*SavedSP) != int64_t(*Link) - 4 ||
      int64_t(*Handler) != int64_t(*Link) + 4 ||
      int64_t(*State) != int64_t(*Link) + 8 || Cursor + 6 > Size ||
      P[Cursor] != 0x64)
    return std::nullopt;
  uint8_t LinkRegister = 0;
  if (P[Cursor + 1] == 0xa1 && readLE<uint32_t>(P + Cursor + 2) == 0)
    Cursor += 6;
  else if (Cursor + 7 <= Size && P[Cursor + 1] == 0x8b &&
           (P[Cursor + 2] & 0xc7) == 0x05 &&
           readLE<uint32_t>(P + Cursor + 3) == 0) {
    LinkRegister = (P[Cursor + 2] >> 3) & 7;
    Cursor += 7;
  } else
    return std::nullopt;
  if (LinkRegister == NodeRegister || LinkRegister == 4 || LinkRegister == 5 ||
      LinkRegister == 6)
    return std::nullopt;
  const auto Previous = Operand(0x89, LinkRegister);
  const auto InstallSize =
      getX86RegistrationChainStoreSize(Img, Site.Range.Begin + Cursor);
  if (!Previous || *Previous != *Link || !InstallSize ||
      Cursor + *InstallSize > Size ||
      (*InstallSize == 6 && NodeRegister != 0) ||
      (*InstallSize == 7 && ((P[Cursor + 2] >> 3) & 7) != NodeRegister) ||
      !readImmutablePE32CodeBytes(Img, Site.Range.Begin, Cursor + *InstallSize,
                                  {HandlerOperand}))
    return std::nullopt;

  return RegistrationInstallation{
      ChainRead,
      HandlerOperand,
      Site.Range.Begin + Cursor,
      -12,
      -4,
      RegistrationRealignedFrame{6, Site.Range.Begin + 15, Alignment,
                                 Allocation, -(*Link + 12), -20},
      {HandlerOperand}};
}

} // namespace neverd::coff_loader::registration_detail
