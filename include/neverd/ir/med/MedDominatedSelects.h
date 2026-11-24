//===- MedDominatedSelects.h - Selections a branch decides ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Fold a selection whose condition a dominating conditional branch decides.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDDOMINATEDSELECTS_H
#define NEVERD_IR_MED_MEDDOMINATEDSELECTS_H

namespace neverd {
struct MedFunc;

/// Replace each SELECT of \p Func whose condition a dominating conditional
/// branch decides by a copy of the arm it takes.  A branch decides its
/// condition in a successor that only its edge enters: true where it jumps,
/// false where it falls through, and so in every block that successor
/// dominates.  A selection's condition is decided where it is the branch's
/// value, or the same comparison of the same values, or the negation of
/// either.  `test rdx, rdx; je skip; ...; bsf rcx, rdx` selects RCX's old
/// value where RDX is zero, which the branch has ruled out.  Requires SSA;
/// returns whether any selection was folded.
bool foldDominatedSelects(MedFunc &Func);
} // namespace neverd

#endif // NEVERD_IR_MED_MEDDOMINATEDSELECTS_H
