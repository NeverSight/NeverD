//===- RegistrationCallStacks.h - Protocol CALL stacks ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Place PE32 EH3/EH4 CALL stacks by the frame protocol where the
/// registration-state proof is incomplete.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_REGISTRATIONCALLSTACKS_H
#define NEVERD_IR_MED_X86_REGISTRATIONCALLSTACKS_H

#include "neverd/ir/med/MedProvenStackOffsets.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace neverd {
struct BinaryImage;
struct LowFunc;

namespace x86_registration {
/// Seeds for the reloads of an EH3/EH4 parent's saved stack pointer. The
/// prologue pushes the record's handler, scope table and seeded level above
/// the node FS:[0] links and saves ESP in the SavedESP word two words below
/// that node; a handler entry reloads it. When every exact store to that word
/// saves one entry stack offset, each exact pointer-wide reload of it is
/// seeded with that offset. Only a write outside every source object could
/// change the word otherwise: it corrupts the runtime's record.
/// \p Accesses holds the entry offset of each exact LOAD and STORE address.
std::vector<ProvenStackOffset> savedStackReloads(
    const MedFunc &Func, const LowFunc &Source, const BinaryImage &Image,
    const std::map<const MedOp *, int64_t> &Accesses, size_t &Remaining);

/// The CALLs among \p Calls, by their pre-call stack pointers, whose stack
/// derives from one SEH callback's runtime stack root alone and stays within
/// its allocation below the return PC. The runtime enters filter and finally
/// callbacks on the dispatcher's stack below the parent frame, so these
/// return slots never reach a parent frame byte.
std::set<const MedOp *>
privateCallbackCalls(const MedFunc &Func, const BinaryImage &Image,
                     const std::map<const MedOp *, MedVar> &Calls,
                     size_t &Remaining);
} // namespace x86_registration
} // namespace neverd

#endif // NEVERD_IR_MED_X86_REGISTRATIONCALLSTACKS_H
