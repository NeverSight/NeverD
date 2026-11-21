//===- MedGoResults.h - Go register ABI call results ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The results a call under Go's internal register ABI returns past its
/// first: which of them a function reads, and the rewrite that returns them.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDGORESULTS_H
#define NEVERD_IR_MED_MEDGORESULTS_H

#include "neverd/Common.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <map>
#include <set>

namespace neverd {

/// The position of \p RegOff among Go's integer results on \p A (RAX, RBX,
/// RCX, RDI, RSI, R8-R11: goIntegerArgumentRegisters), or -1.
int goResultPosition(Arch A, uint64_t RegOff);

/// The results a call returns: the position of the last, and for each the
/// bytes of its register the call's readers take, at most a word.  A
/// position no reader takes, before the last, is a whole word.
struct GoCallShape {
  int Last = -1;
  llvm::SmallVector<uint16_t, 9> Widths;
};

/// The results \p Func reads after each call in \p Sites, before any write:
/// a read of the value the call left in a result register, in any block and
/// through the PHIs it reaches, and, for \p Returned, a value the function
/// returns.  A call that returns some results already counts them.  A read
/// that only keeps the bytes a partial write leaves, a self-zeroing idiom,
/// or a sub-register view takes no result; the view still reads as many of
/// a result's bytes as it covers.  A call none of whose results past RAX is
/// read is absent.
std::map<uint32_t, GoCallShape>
goCallResultsRead(const MedFunc &Func, Arch A, const std::set<uint32_t> &Sites,
                  llvm::ArrayRef<MedVar> Returned = {});

/// What a Go function's return in \p Block hands back in the register at
/// \p RegOff, as the return lowers a result field: the newest write of the
/// whole register, its PHI, the call that last changes it, or the value it
/// entered the function with (\p Value is then the copy that names it, if
/// any).  Every write of a general-purpose register defines the whole of it
/// too; a narrower value (a view copy propagation left on a temporary, the
/// entry copy of the low byte) is a value of its own, which a later write of
/// the whole register leaves stale.  A join without a PHI for the register
/// brings the same value along every edge, so any edge that reaches it will do.
/// \p ById maps the function's block ids to its blocks.
struct GoReturnedValue {
  enum Kind : uint8_t { Unknown, Defined, Phi, Call, Entry } What = Unknown;
  const MedVar *Value = nullptr;
  const MedOp *Caller = nullptr;
};
GoReturnedValue
goReturnedValue(const llvm::DenseMap<int, const MedBlock *> &ById,
                const MedBlock &Block, uint64_t RegOff, Arch A);

/// Make each call of \p Func that \p Shapes names return Go's integer
/// results through its last position, each as wide as its shape says,
/// packed into one value laid out as the record of them (each field
/// naturally aligned), whose fields define each result register the call
/// leaves, at every width it leaves it, so the reads it reaches keep their
/// values.  A width wider than its field takes the field's value in its low
/// bytes: no reader takes more.  A call that returns some already, each a
/// word, takes the rest.
void defineGoCallResults(MedFunc &Func, Arch A,
                         const std::map<uint32_t, GoCallShape> &Shapes);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDGORESULTS_H
