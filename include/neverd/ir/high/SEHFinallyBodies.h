//===- SEHFinallyBodies.h - __finally termination bodies --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows calls a `__finally` termination handler out of line, with an
/// abnormal-termination flag as the first argument.  A C `__finally` body runs
/// on both the normal exit and an unwind, so the compiler emits the body
/// twice: the funclet (unwind) and an inline copy on the normal path with the
/// flag folded false.  This projects the funclet's logic into the parent's
/// empty `__finally` clause without running it twice.  An x64 funclet reads
/// the parent's frame through its EstablisherFrame argument; an ARM or AArch64
/// funclet through the parent's callee-saved registers, which the unwinder
/// restores before calling it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_SEHFINALLYBODIES_H
#define NEVERD_IR_HIGH_SEHFINALLYBODIES_H

#include "neverd/ir/high/HighIR.h"

#include <set>
#include <vector>

namespace neverd {

/// Fill the empty `__finally` clauses of every SEH function in \p Funcs with
/// its termination handler's logic, at any depth of its statements and whatever
/// the handler's control flow; a handler's own clauses fill first, so a
/// `__try` nested in a `__finally` prints inside it.  On x64 the handler reads
/// the parent's locals through `EstablisherFrame + k`; the certified frame
/// bytes (`HighFunc::SEHTerminationFrameBytes`) rebase that onto the parent's
/// entry stack pointer, as a catch funclet's `RDX` is rebased.  On ARM and
/// AArch64 each callee-saved register the handler reads becomes the parent's
/// value of it throughout the protected ranges
/// (`HighFunc::SEHFuncletRegisters`).  The abnormal-termination argument
/// becomes `AbnormalTermination()`, and a local unwind in the handler the
/// `goto` it makes.
///
/// A C `__finally` runs on the normal exit too, so the compiler's inline
/// normal-path copy must not run a second time.  When the parent's statements
/// after the `__try` are that copy, they are left in place and the clause
/// holds only the abnormal work under `if (AbnormalTermination())`; when the
/// clause can instead be proved equal to that copy with the flag false, the
/// copy is removed and the clause holds the whole body.  A handler with nothing
/// to do while unwinding is an empty clause.  C also runs a filled clause for a
/// jump out of the `__try`, which the machine leaves normally: such a jump to
/// where the try falls through becomes `__leave`.  A handler whose frame or
/// registers are not known or whose body is unavailable keeps its address
/// comment.
///
/// A filter callback reads the parent the same way and is spelled in it the
/// same way, its exception pointers being `GetExceptionInformation()`; when
/// one expression says what it does in its order, that expression becomes the
/// clause's `__except` expression (HighEHClause::FilterValue) in place of a
/// call of the callback.  A write it makes to the parent's frame stays in the
/// expression unless nothing else can read those bytes: no statement of the
/// parent, no other filter and no handler still out of line touches them, and
/// the frame's address goes nowhere else, as with the temporaries a compiler
/// keeps for a filter in its parent's frame.
///
/// \p Funcs is the whole translation unit so the callbacks can be read.  Each
/// funclet whose body is attached into a parent is added to \p Merged when
/// given, so a single-function view need not also print it standalone.
void attachSEHFinallyBodies(std::vector<HighFunc> &Funcs, Arch TargetArch,
                            std::set<va_t> *Merged = nullptr);

} // namespace neverd

#endif // NEVERD_IR_HIGH_SEHFINALLYBODIES_H
