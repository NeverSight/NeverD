//===- MedStackAlignment.h - Proven entry-stack alignment -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDSTACKALIGNMENT_H
#define NEVERD_IR_MED_MEDSTACKALIGNMENT_H

#include "neverd/ir/med/MedIR.h"

#include <optional>

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

} // namespace neverd

#endif // NEVERD_IR_MED_MEDSTACKALIGNMENT_H
