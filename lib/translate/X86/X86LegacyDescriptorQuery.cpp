//===- X86LegacyDescriptorQuery.cpp - Faultable descriptor queries -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Invalid selectors clear ZF; descriptor page/backing failures still fault.
//===----------------------------------------------------------------------===//

#include "X86LegacyDescriptorQuery.h"

#include <array>
#include <cstdint>
#include <utility>

namespace neverd::translate::detail {

llvm::Expected<X86LegacyDescriptorQueryResult>
queryLegacyDescriptor(const X86LegacySegmentState &S, X86PagedMemory &M,
                      X86LegacyDescriptorQuery Query, uint16_t Selector) {
  X86LegacyDescriptorQueryResult Result;
  const bool Local = Selector & 4;
  const uint32_t Index = Selector & 0xfff8;
  const uint32_t Limit = Local ? S.LDTR.Limit : S.GDTRLimit;
  if (!(Selector & 0xfffc) || (Local && S.LDTR.Unusable) || Index + 7 > Limit)
    return Result;
  const uint32_t Address = (Local ? S.LDTR.Base : S.GDTRBase) + Index;
  std::array<uint8_t, 8> Bytes{};
  auto Read = M.load(
      {Address, MemoryAccessKind::Read, S.CPL, true, false, true}, Bytes);
  if (!Read)
    return Read.takeError();
  static_cast<X86MemoryAccessResult &>(Result) = std::move(*Read);
  if (Result.Status != X86PageWalkStatus::Completed)
    return Result;
  uint64_t Descriptor = 0;
  for (unsigned I = 0; I != Bytes.size(); ++I)
    Descriptor |= uint64_t(Bytes[I]) << (I * 8);
  const unsigned Type = (Descriptor >> 40) & 15;
  const unsigned DPL = (Descriptor >> 45) & 3;
  const bool Segment = Descriptor & (uint64_t{1} << 44);
  const bool Conforming = Segment && (Type & 12) == 12;
  if (!Conforming && (S.CPL > DPL || (Selector & 3) > DPL))
    return Result;
  using QueryKind = X86LegacyDescriptorQuery;
  if (Query == QueryKind::Read || Query == QueryKind::Write) {
    Result.Visible =
        Segment && (Query == QueryKind::Write ? (Type & 10) == 2
                                              : !(Type & 8) || (Type & 2));
    return Result;
  }
  // LAR additionally admits call/task gates; neither operation admits
  // interrupt/trap gates. Presence and accessed bits are only queried.
  constexpr uint16_t LimitTypes = 0x0a0e;               // 1, 2, 3, 9, 11.
  constexpr uint16_t RightsTypes = LimitTypes | 0x1030; // Also 4, 5, 12.
  if (!Segment &&
      !((Query == QueryKind::Limit ? LimitTypes : RightsTypes) & (1u << Type)))
    return Result;
  Result.Visible = true;
  if (Query == QueryKind::AccessRights) {
    // The bound TableImage profile retains the otherwise undefined limit
    // nibble in LAR[19:16], independently of the architectural rights bits.
    Result.Value = uint32_t(Descriptor >> 32) & 0x00ffff00;
  } else {
    Result.Value =
        uint32_t(Descriptor & 0xffff) | uint32_t((Descriptor >> 32) & 0xf0000);
    if (Descriptor & (uint64_t{1} << 55))
      Result.Value = (Result.Value << 12) | 0xfff;
  }
  return Result;
}

} // namespace neverd::translate::detail
