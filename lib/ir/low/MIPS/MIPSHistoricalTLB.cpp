//===- MIPSHistoricalTLB.cpp - R3000A and R4000 address translation -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/MIPS/MIPSHistoricalTLB.h"

#include "neverd/ir/low/MIPS/MIPSHistoricalCP0.h"

namespace neverd::mips {

std::optional<HistoricalTranslation>
HistoricalTLB::translate(const HistoricalCP0 &CPU, uint64_t VA,
                         HistoricalMemoryAccess Access) const {
  const bool R3000 = CPU.Processor == HistoricalProcessor::R30xx;
  if (!R3000 && CPU.Processor != HistoricalProcessor::R4000)
    return std::nullopt;
  if (R3000 && Access != HistoricalMemoryAccess::Fetch &&
      (CPU.Status & (1u << 16)))
    return std::nullopt; // Isolated D-cache accesses do not address ordinary
                         // RAM.
  const uint32_t Address = static_cast<uint32_t>(VA);
  if (R3000 && VA != Address)
    return std::nullopt;
  const bool Kernel =
      R3000 ? !(CPU.Status & 2) : (CPU.Status & 6) || !(CPU.Status & 0x18);
  const bool Supervisor =
      !R3000 && !(CPU.Status & 6) && (CPU.Status & 0x18) == 8;
  if (!Kernel && !Supervisor && Access != HistoricalMemoryAccess::Fetch &&
      (CPU.Status & (1u << 25)))
    return std::nullopt; // User-mode reverse-endian data uses different byte
                         // lanes.
  if (R3000 || VA == uint64_t(int64_t(int32_t(Address)))) {
    if (Address >= 0x80000000u && !Kernel &&
        !(Supervisor && Address >= 0xc0000000u && Address < 0xe0000000u))
      return HistoricalTranslation{HistoricalAddressResult::AddressError};
    if (Address >= 0x80000000u && Address < 0xc0000000u)
      return HistoricalTranslation{HistoricalAddressResult::Mapped,
                                   Address & 0x1fffffffu};
    if (!R3000 && (CPU.Status & 4) && Address < 0x80000000u)
      return HistoricalTranslation{HistoricalAddressResult::Mapped, Address};
  } else {
    // R4000 manual, tables 4-1, 4-2 and 4-4. A non-sign-extended address
    // produced in 32-bit addressing mode has no guaranteed translation.
    if (!CPU.usesExtendedAddressing())
      return std::nullopt;
    const auto Region = VA >> 62;
    const HistoricalTranslation Error{HistoricalAddressResult::AddressError};
    if (Region == 2) { // xkphys carries CCA in [61:59] and PA in [35:0].
      if (!Kernel || (VA & UINT64_C(0x07fffff000000000)))
        return Error;
      const auto CCA = (VA >> 59) & 7;
      if (CCA < 2 || CCA == 7)
        return std::nullopt;
      return HistoricalTranslation{HistoricalAddressResult::Mapped,
                                   VA & UINT64_C(0xfffffffff)};
    }
    // xuseg and xsseg span 1 TiB each. The upper 2 GiB of xkseg are
    // accessible only through the sign-extended compatibility segments.
    if ((VA & UINT64_C(0x3fffff0000000000)) ||
        (Region == 1 && !Kernel && !Supervisor) ||
        (Region == 3 && (!Kernel || VA > UINT64_C(0xc00000ff7fffffff))))
      return Error;
    if (Region == 0 && (CPU.Status & 4))
      return std::nullopt; // Only the low 2 GiB are specified under ERL.
  }

  if (Entries.size() != (R3000 ? 64u : 48u) || (R3000 && ASID >= 64))
    return std::nullopt;
  if (CPU.Status & (1u << 21))
    return std::nullopt; // Translation is unavailable with TS set.
  std::optional<HistoricalTranslation> Match;
  for (const auto &Entry : Entries) {
    uint64_t PageBytes = 4096;
    uint32_t Lo = Entry.Lo0;
    if (R3000) {
      if ((Address >> 12) != (uint32_t(Entry.Hi) >> 12) ||
          (!(Lo & 0x100) && ((Entry.Hi >> 6) & 63) != ASID))
        continue;
    } else {
      // R4000 supports seven powers of four from 4 KiB through 16 MiB.
      switch (Entry.PageMask) {
      case 0:
      case 0x6000:
      case 0x1e000:
      case 0x7e000:
      case 0x1fe000:
      case 0x7fe000:
      case 0x1ffe000:
        break;
      default:
        return std::nullopt;
      }
      PageBytes = (uint64_t(Entry.PageMask) + 0x2000) / 2;
      const uint64_t Key =
          UINT64_C(0xc00000ffffffe000) & ~uint64_t(Entry.PageMask);
      if (((Entry.Hi ^ VA) & Key) ||
          (!(Lo & Entry.Lo1 & 1) && uint8_t(Entry.Hi) != ASID))
        continue;
      if (VA & PageBytes)
        Lo = Entry.Lo1;
    }
    if (Match)
      return std::nullopt; // Multiple matches are not a usable translation.
    if (!(Lo & (R3000 ? 0x200u : 2u)))
      Match = HistoricalTranslation{HistoricalAddressResult::Invalid};
    else if (Access == HistoricalMemoryAccess::Store &&
             !(Lo & (R3000 ? 0x400u : 4u)))
      Match = HistoricalTranslation{HistoricalAddressResult::Modified};
    else {
      const uint64_t Frame =
          R3000 ? Lo & 0xfffff000u : uint64_t((Lo >> 6) & 0xffffffu) << 12;
      Match = HistoricalTranslation{HistoricalAddressResult::Mapped,
                                    (Frame & ~(PageBytes - 1)) |
                                        (VA & (PageBytes - 1))};
    }
  }
  return Match.value_or(HistoricalTranslation{HistoricalAddressResult::Refill});
}

} // namespace neverd::mips
