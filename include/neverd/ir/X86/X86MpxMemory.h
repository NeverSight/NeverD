//===- X86MpxMemory.h - MPX memory completion boundary ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Memory-provider contract for BNDMOV and bounds-table transactions. Requests
/// describe architectural fields, not a chosen microarchitectural access order.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86MPXMEMORY_H
#define NEVERD_IR_X86_X86MPXMEMORY_H

#include "neverd/ir/X86/X86MpxContext.h"

#include <array>
#include <cstdint>

namespace neverd {

enum class X86MpxMemoryOperation : uint8_t {
  LoadBounds,
  StoreBounds,
  LoadDirectory,
  LoadTable,
  StoreTable
};

struct X86MpxMemoryAccess {
  X86MpxMemoryOperation Operation;
  X86MpxMode Mode;
  /// BNDMOV: operand offset in the provider's original segment environment.
  /// Directory/table: computed BDE/BTE address. The provider must preserve
  /// the architectural address interpretation and exception provenance.
  uint64_t Address;
  /// Only StoreBounds can be locked. Never split this into scalar writes.
  bool Locked = false;
  /// Stores supply lower, upper, then pointer-value fields. This is layout,
  /// not an execution order. The fourth BTE field is never part of a request.
  std::array<uint64_t, 3> Words{};
  /// BNDMOV retains the selected segment, including default SS. Table requests
  /// use the already translated address and the table access's DS contract.
  X86SegmentRegister Segment = X86SegmentRegister::DS;
  /// The original BNDMOV base is SP/BP (not R12/R13). Long mode ignores
  /// CS/DS/ES/SS overrides for fault classification, so Segment alone cannot
  /// distinguish a stack-default #SS from a data-default #GP. FS/GS still
  /// override that default. Table accesses are not stack references.
  bool DefaultStack = false;

  unsigned wordBytes() const { return Mode == X86MpxMode::Bits64 ? 8 : 4; }
  unsigned wordCount() const {
    if (Operation == X86MpxMemoryOperation::LoadDirectory)
      return 1;
    return Operation == X86MpxMemoryOperation::LoadTable ||
                   Operation == X86MpxMemoryOperation::StoreTable
               ? 3
               : 2;
  }
  bool writesMemory() const {
    return Operation == X86MpxMemoryOperation::StoreBounds ||
           Operation == X86MpxMemoryOperation::StoreTable;
  }
  /// Whether a segment/canonicality failure uses stack-reference provenance.
  /// Page permissions and other fault causes remain the provider's decisions.
  bool usesStackSegment() const {
    if (Mode == X86MpxMode::Bits32)
      return Segment == X86SegmentRegister::SS;
    return DefaultStack && Segment != X86SegmentRegister::FS &&
           Segment != X86SegmentRegister::GS;
  }
};

enum class X86MpxMemoryOutcome : uint8_t { Completed, Fault, Unavailable };

struct X86MpxMemoryCompletion {
  X86MpxMemoryOutcome Outcome = X86MpxMemoryOutcome::Unavailable;
  /// Completed reads return all requested fields in their memory-layout order.
  /// Faulted reads publish no successful architectural bounds value here.
  std::array<uint64_t, 3> Words{};
};

class X86MpxMemoryProvider {
public:
  virtual ~X86MpxMemoryProvider() = default;
  /// The provider owns the complete memory transaction, including its actual
  /// access order, partial stores, LOCK atomicity, faults and diagnostics.
  /// Fault preserves those effects; the context never rolls them back.
  /// Unavailable certifies no effects from this request and must not be used
  /// after performing a partial transaction. Earlier completed requests, such
  /// as a BDE read, still stand. Missing semantics are not a simulated #PF.
  /// Do not mutate the MPX configuration or bounds bank from this callback.
  virtual X86MpxMemoryCompletion access(const X86MpxMemoryAccess &Access) = 0;
};

} // namespace neverd

#endif // NEVERD_IR_X86_X86MPXMEMORY_H
