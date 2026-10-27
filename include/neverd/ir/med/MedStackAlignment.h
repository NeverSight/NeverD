//===- MedStackAlignment.h - Proven entry-stack alignment -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDSTACKALIGNMENT_H
#define NEVERD_IR_MED_MEDSTACKALIGNMENT_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

/// How execution reaches \p Func when the image says the kernel enters its
/// address as \p AtEntry.  The kernel leaves no return address at a process
/// entry point, so code there that returns was called instead: a test harness
/// or a loader can call an image's entry point as a function.
StackEntryKind functionEntryKind(const MedFunc &Func, StackEntryKind AtEntry);

/// A synthetic PE32 runtime-entry definition, with its full register carrier.
bool hasValidRegistrationRootShape(const MedOp &Op);

/// A direct established frame and its continuation have exact entry-stack
/// offsets. Realigned frames and private callback stacks deliberately do not.
std::optional<int64_t> registrationRootEntryStackOffset(const MedOp &Op);

/// In source mode, simplify an alignment mask only when its operand is an
/// exact offset from an authenticated entry stack pointer and the target ABI
/// fixes every bit discarded by the mask for a function entered as \p Entry.
void simplifyProvenStackAlignment(MedFunc &Func, Arch Architecture,
                                  BinaryFormat Format,
                                  StackEntryKind Entry = StackEntryKind::Call);

/// The offset of \p Value from the authenticated entry stack pointer, when
/// copies, constant additions and proven alignment masks define it from that
/// pointer.
std::optional<int64_t>
entryStackOffset(const MedFunc &Func, const MedVar &Value, Arch Architecture,
                 BinaryFormat Format,
                 StackEntryKind Entry = StackEntryKind::Call);

/// entryStackOffset of each of \p Values, by one proof over \p Func.
std::vector<std::optional<int64_t>>
entryStackOffsets(const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
                  Arch Architecture, BinaryFormat Format,
                  StackEntryKind Entry = StackEntryKind::Call);

/// Inclusive bounds on a pointer's signed displacement from entry SP. A
/// dynamic allocation can have bounds without having an exact stack offset.
struct EntryStackOffsetRange {
  int64_t Min;
  int64_t Max;
};

/// Bounded dynamic displacements and PHI alternatives rooted at entry SP.
/// Unanchored cycles, unbounded arithmetic and offset overflow stay unknown.
std::vector<std::optional<EntryStackOffsetRange>>
entryStackOffsetRanges(const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
                       Arch Architecture, BinaryFormat Format,
                       StackEntryKind Entry = StackEntryKind::Call);

/// The largest displacement from entry SP each of \p Values can have, where
/// copies, bounded displacements, alignment masks and joins define it and no
/// cycle among them can raise it. A dynamic allocation in a loop has such a
/// bound without a lower one.
std::vector<std::optional<int64_t>>
entryStackOffsetUpperBounds(const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
                            Arch Architecture, BinaryFormat Format,
                            StackEntryKind Entry = StackEntryKind::Call);

/// Full-width SP definitions in the entry block that align an authenticated
/// entry-stack offset. Each definition is a distinct frame origin; this does
/// not establish a constant displacement from the unaligned entry SP.
std::vector<MedVar> entryStackRealignmentRoots(const MedFunc &Func,
                                               Arch Architecture,
                                               BinaryFormat Format);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDSTACKALIGNMENT_H
