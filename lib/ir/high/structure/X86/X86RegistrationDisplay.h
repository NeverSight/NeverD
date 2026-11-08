//===- X86RegistrationDisplay.h - x86 SEH in the decompile view -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The structure of an x86 `_except_handler3` or `_except_handler4` frame in
/// the decompile view, from what the frame's own code says
/// (RegistrationDisplayFacts): each scope's `__try` is the code at its try
/// level and the levels nested in it, its `__finally` the one body the function
/// holds, and its filter code that runs in the function's frame.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_HIGH_STRUCTURE_X86_X86REGISTRATIONDISPLAY_H
#define NEVERD_LIB_IR_HIGH_STRUCTURE_X86_X86REGISTRATIONDISPLAY_H

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

class RegistrationDisplay {
public:
  /// The view of \p Med, or nullopt when it is no such frame.
  static std::optional<RegistrationDisplay> build(const MedFunc &Med,
                                                  const ExceptionFunction &EH);

  /// Print the scopes of \p Func as `__try` statements, each holding what its
  /// try level guards, with its clause holding its handler's code.  It keeps
  /// the counts of the regions that print and those that could not.
  void structure(HighFunc &Func) const;

private:
  RegistrationDisplay(const MedFunc &Med, const ExceptionFunction &EH,
                      const RegistrationDisplayFacts &Facts);

  /// The code only the runtime's entry at the handler or filter \p Root runs,
  /// in address order.  Nullopt when none does: other code shares it.
  std::optional<std::vector<ExceptionAddressRange>> bodyAt(va_t Root) const;
  /// Remove the local unwinds: a jump to what follows each leaves the scopes
  /// the call would have unwound.
  void dropLocalUnwinds(HighFunc &Func) const;
  /// Take each filter's code out of the listing, and say what one expression
  /// can of what it returns.
  std::map<va_t, ExprPtr> takeFilters(HighFunc &Func) const;
  /// The value of the filter at \p Root whose code is \p Body as one
  /// expression, or null when it takes more than C can say in one.
  ExprPtr filterValue(const std::vector<HighStmt> &Body, va_t Root) const;
  /// Print the reads of the exception pointers the runtime leaves in the frame
  /// as what they are in C.
  void nameExceptionReads(HighFunc &Func) const;
  /// Where the runtime leaves the exception pointers: the word below the
  /// registration record, as a distance from the entry stack pointer.
  int64_t exceptionPointersSlot() const;
  /// The handler or filter only whose runtime entry runs \p Address, or zero.
  va_t ownerOf(va_t Address) const;
  /// Whether the frame word at \p Slot, a distance from the entry stack
  /// pointer, is one only the filter at \p Root and, with \p OrExceptArms, the
  /// `__except` arms read.  Only filters write it, and nothing takes its
  /// address.
  bool isFilterTemporary(int64_t Slot, va_t Root, bool OrExceptArms) const;
  /// What a finished `__finally` clause says of its body.
  struct SettledFinally {
    std::vector<ExceptionAddressRange> Ranges;
    /// Whether it comes back to its `call`.
    bool Returns;
    /// What it leaves in the register the call returns, when one name says.
    ExprPtr Returned;
  };
  std::optional<SettledFinally>
  settleClause(HighStmt &Try, const std::set<va_t> &Leaving) const;
  void takeNormalPath(std::vector<HighStmt> &List, size_t Index,
                      const SettledFinally &Finally,
                      const std::set<va_t> &Labels,
                      const std::set<VarKey> &Frame) const;
  void dropBodyCalls(HighFunc &Func) const;
  void settleLists(std::vector<HighStmt> &List, const std::set<va_t> &Labels,
                   const std::set<va_t> &Leaving,
                   const std::set<VarKey> &Frame) const;

  const MedFunc *Med;
  const ExceptionFunction *EH;
  const RegistrationDisplayFacts *Facts;
  /// The scope of each handler and filter.
  std::map<va_t, size_t> ScopeOfRoot;
  /// The frame pointer at each runtime entry, by the name it goes by, as its
  /// distance from the function's entry stack pointer.
  std::map<VarKey, int64_t> Frames;
  /// The frame words a filter saves GetExceptionCode() in for its arm, as
  /// distances from the entry stack pointer: reads of them are that call.
  mutable std::set<int64_t> CodeSlots;
  /// What each settled `__finally` body leaves in the register its `call`
  /// returns, when one name or constant says.
  mutable std::map<va_t, ExprPtr> BodyResults;
};

} // namespace neverd

#endif // NEVERD_LIB_IR_HIGH_STRUCTURE_X86_X86REGISTRATIONDISPLAY_H
