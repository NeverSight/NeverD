//===- MIPSHistoricalTLB.h - Historical address translation -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_MIPS_MIPSHISTORICALTLB_H
#define NEVERD_IR_LOW_MIPS_MIPSHISTORICALTLB_H

#include <cstdint>
#include <optional>
#include <span>

namespace neverd::mips {

struct HistoricalCP0;

/// Raw entries sampled from the same MMU at one instruction boundary.
/// R3000A uses Hi and Lo0. R4000 has two page halves and a PageMask.
struct HistoricalTLBEntry {
  uint64_t Hi;
  uint32_t Lo0, Lo1, PageMask;
};

enum class HistoricalMemoryAccess : uint8_t { Fetch, Load, Store };
enum class HistoricalAddressResult : uint8_t {
  Mapped,
  AddressError,
  Refill,
  Invalid,
  Modified
};

struct HistoricalTranslation {
  HistoricalAddressResult Result;
  uint64_t Physical = 0;
};

/// The caller owns the complete table: invalid entries still participate in
/// matching. A partial snapshot cannot establish that a translation missed.
struct HistoricalTLB {
  std::span<const HistoricalTLBEntry> Entries;
  uint8_t ASID;
  std::optional<HistoricalTranslation>
  translate(const HistoricalCP0 &CPU, uint64_t VA,
            HistoricalMemoryAccess Access) const;
};

} // namespace neverd::mips

#endif // NEVERD_IR_LOW_MIPS_MIPSHISTORICALTLB_H
