//===- MedCallLeftovers.h - Values calls leave in registers -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The values a function's registers hold only because earlier calls left
/// them there.  The ABI lets a call change a caller-saved register, so what
/// the register holds afterwards is no value the function sets, and no
/// compiler passes it on as an argument.  Call-argument recovery and HighIR's
/// call arguments both end a call's arguments at such a register.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLLEFTOVERS_H
#define NEVERD_IR_MED_MEDCALLLEFTOVERS_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <optional>
#include <set>
#include <tuple>

namespace neverd {

struct TargetRegInfo;

/// Whether \p Call may change the register view \p Size bytes wide at
/// \p RegOff, as the ABI lets a call change each caller-saved register.  It
/// keeps the view when it keeps every caller-saved register
/// (MedOp::PreservesCallerSaved), when the convention \p Format selects
/// preserves all of it, when its callee is known never to write it
/// (MedOp::CallPreservedGPRs), or when the view's register holds the call's
/// result.  The stack, frame and link registers have models of their own.
bool callClobbersRegister(const MedOp &Call, uint64_t RegOff, uint16_t Size,
                          const TargetRegInfo &TRI, BinaryFormat Format);

/// What \p Call leaves in the register view \p Size bytes wide at \p RegOff
/// when it clobbers the view: the clobber SSA gave the view
/// (MedFunc::CallClobbers), or an Unspecified value where the function has
/// no variable for it.  nullopt when the call keeps the view.
std::optional<MedVar> valueLeftByCall(const MedFunc &Func, const MedOp &Call,
                                      uint64_t RegOff, uint16_t Size,
                                      const TargetRegInfo &TRI,
                                      BinaryFormat Format);

/// Each call's clobber of a caller-saved register it may change whole
/// (MedFunc::CallClobbers), and each join of nothing else.  A clobber the
/// struct-return recovery still holds as a possible result field
/// (MedFunc::StructReturnCandidates) is none: its call may return part of
/// its result there.  An Unspecified value, which no variable holds, is one
/// too (valueLeftByCall).
class ValuesLeftByCalls {
public:
  explicit ValuesLeftByCalls(const MedFunc &Func);

  /// Result fields whose scalar call-return recovery has not necessarily run
  /// yet are excluded too. Block order cannot turn a possible result into
  /// evidence that an argument was never supplied.
  ValuesLeftByCalls(const MedFunc &Func,
                    llvm::ArrayRef<MedVar> PossibleResults);

  /// Whether \p V holds only what earlier calls left in its register.
  bool contains(const MedVar &V) const;

private:
  std::set<std::tuple<int, int, int>> Values;
};

} // namespace neverd

#endif // NEVERD_IR_MED_MEDCALLLEFTOVERS_H
