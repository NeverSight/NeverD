//===- MedCallReturnReads.h - Source call-stack observations ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reject observations of physical call writes absent from a source frame.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLRETURNREADS_H
#define NEVERD_IR_MED_MEDCALLRETURNREADS_H

#include "neverd/Common.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/med/MedIntegerReturnRange.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct MedCallReturnRead;
struct MedFunc;

/// Check exact entry-relative loads against reaching physical x86 CALL
/// return slots before source cleanup. An ordinary C call does not write the
/// reconstructed frame. A later explicit store kills only its own bytes;
/// reading any remaining call-written byte requires a machine-state source
/// contract. This check does not certify unresolved addresses or callee
/// effects. FreshSource, when supplied, must be the exact LowIR input of this
/// fresh conversion, before source cleanup. Its checked registration-state
/// flow can locate restored x86 SEH stacks; this is not a reusable MedIR fact.
/// Where that proof is incomplete, the EH3/EH4 frame protocol still places
/// reloads of the saved stack pointer and runtime callback stacks.
///
/// With \p Deferred, each such load is recorded there instead of refusing the
/// function: whether its value is observed is known only once call arguments
/// and returns are settled (validateObservedCallReturnReads). Every other
/// refusal remains immediate.
///
/// \p NeedsCalleeFacts, when supplied without \p CallReturns, is set if a
/// CALL's stack position had no local range. Callee return ranges may still
/// bound it, so a load deferred now could be proved once they are known.
bool validateSourceCallReturnReads(
    const MedFunc &Func, const BinaryImage &Image, std::string &Diagnostic,
    size_t WorkLimit = limits::ir::kSourceCallReturnReadWork,
    const LowFunc *FreshSource = nullptr,
    const MedCallReturnRanges *CallReturns = nullptr,
    std::vector<MedCallReturnRead> *Deferred = nullptr,
    bool *NeedsCalleeFacts = nullptr);

/// Refuse a recorded CALL return-slot load (MedFunc::CallReturnReads) whose
/// value the settled function still observes. Copies, byte views and joins
/// only move the value; any other operation, call argument, call clobber or
/// exceptional entry naming it reads it. While a register holds it, so may
/// another variable over that register, a call without settled arguments
/// that takes the register, or a RETURN that reads it. A load no surviving
/// operation performs is unobserved.
/// No pass between the two checks may forward a stored value into such a
/// load, or rewrite it.
///
/// \p CalleeIntegerArity bounds, by entry, the integer argument registers a
/// direct call to a function of this run can pass: its emitted prototype
/// takes no integer parameter at or past that position, which both C routes
/// enforce by passing a defined callee exactly its parameters. A call whose
/// target it does not name may read every argument register.
bool validateObservedCallReturnReads(
    const MedFunc &Func, const BinaryImage &Image, std::string &Diagnostic,
    size_t WorkLimit = limits::ir::kSourceCallReturnReadWork,
    const std::map<va_t, unsigned> *CalleeIntegerArity = nullptr);
/// Retain observed near-CALL return-address writes in the reconstructed frame.
/// Only exact entry-relative slots below entry SP are admitted. Planning is
/// transactional: ambiguous stacks, incomplete graphs and unavailable original
/// encodings leave Func unchanged. This does not prove callee memory effects or
/// authorize a physical stack switch. Invoke before source stack cleanup.
bool preserveSourceCallReturnWrites(
    MedFunc &Func, const BinaryImage &Image, std::string &Diagnostic,
    size_t WorkLimit = limits::ir::kSourceCallReturnReadWork,
    const LowFunc *FreshSource = nullptr,
    const MedCallReturnRanges *CallReturns = nullptr,
    std::vector<MedCallReturnRead> *Deferred = nullptr,
    bool *NeedsCalleeFacts = nullptr);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDCALLRETURNREADS_H
