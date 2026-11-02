//===- SEHFinallyBodiesDetail.h - Shared __finally projection --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The architecture-neutral steps of projecting a termination handler into its
/// parent's `__finally`, shared by each architecture's projection.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_STRUCTURE_SEHFINALLYBODIESDETAIL_H
#define NEVERD_IR_HIGH_STRUCTURE_SEHFINALLYBODIESDETAIL_H

#include "neverd/ir/high/HighIR.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace neverd::seh_finally {

/// A deep copy with fresh expression nodes, so a projection never changes the
/// handler's own body, which other parents may also take.
ExprPtr cloneExpr(const ExprPtr &E);
HighStmt cloneStmt(const HighStmt &S);
std::vector<HighStmt> cloneStmts(const std::vector<HighStmt> &Stmts);

/// Rewrite every expression of \p Stmts through \p Fn, children first.
void mapExprs(std::vector<HighStmt> &Stmts,
              const std::function<ExprPtr(ExprPtr)> &Fn);

/// A pure integer expression over constants, evaluated; nullopt otherwise.
std::optional<uint64_t> foldToConstant(const HighExpr &E, unsigned Depth = 0);

/// Simplify a projected handler body in one forward pass: uses read the
/// values copies give them, integer computations over numbers fold, and an
/// `if` whose condition folds keeps only the branch it takes.
void simplifyProjectedBody(std::vector<HighStmt> &Body);

/// The variables \p Body defines, by HighIR identity.
std::set<VarKey> definedVariables(const std::vector<HighStmt> &Body);

/// Whether \p V is a register's value on entry to the handler: version 0
/// that the handler does not define (\p Defined).  Version 0 of a register
/// written before it is read is that first write.
bool isEntryValue(const MedVar &V, const std::set<VarKey> &Defined);

/// Whether every variable of a handler body can move into its parent: its
/// own temporaries, parameters up to \p MaxParam, constants, and the entry
/// registers \p EntryRegisterKnown accepts.  Its own stack pointer, frame
/// slots, return value and exception state cannot.
bool funcletVariablesMovable(
    const std::vector<HighStmt> &Body, Arch TargetArch, int MaxParam,
    const std::function<bool(const MedVar &)> &EntryRegisterKnown);

/// The names a parent leaves free: the first rename tag past its own, and
/// the variable numbers neither it nor a body attached so far uses.
struct FuncletNames {
  int RenameTag = 0;
  std::shared_ptr<const std::set<int>> ParentIds =
      std::make_shared<const std::set<int>>();
  std::set<int> Taken;
  int Cursor = 0;
};
FuncletNames firstFreeNames(const std::vector<HighStmt> &Stmts);

/// The parent's entry stack pointer, which the writer spells `frame_base`:
/// the variable its body reads, or a fresh one numbered from \p Names.
MedVar entryStackPointer(const HighFunc &Parent, Arch TargetArch,
                         FuncletNames &Names);

/// Move the handler's locals in \p Body past \p Names, and \p Names past
/// them: a named local keeps the sharing its name gives, and each unnamed one
/// keeps its kind under a fresh number, since its spelling comes from the
/// handler's own numbering.  False, changing nothing, when the names would
/// overflow.
bool renameFuncletLocals(std::vector<HighStmt> &Body, FuncletNames &Names);

/// `AbnormalTermination()`, the <excpt.h> intrinsic.
ExprPtr makeAbnormalTermination();

/// The largest rename tag \p Stmts use, or -1.
int maxRenameTag(const std::vector<HighStmt> &Stmts);

/// Whether \p Stmts call a routine that unwinds a frame to a local target.
bool callsLocalUnwind(const std::vector<HighStmt> &Stmts);

/// Where a parent's HighIR prints its local-unwind exits
/// (HighFunc::SEHLocalUnwindExits).  A copy of an exit is the run of
/// statements that call the termination handlers it still calls, in its
/// order, ended in the same statement list by a `goto` to its target or a
/// `return`.  An exit with a handler call outside such a run is not
/// structured: its handlers stay explicit calls.
class UnwindExits {
public:
  explicit UnwindExits(HighFunc &Parent);

  /// Whether \p S is the jump that ends a copy of an exit.
  bool endsCopy(const HighStmt &S) const;

  /// Whether \p Try's `__finally`, which \p Handler terminates, can run it
  /// for every exit that leaves it: each such exit is structured, every copy
  /// of it lies in the protected body and leaves it, and no handler the exit
  /// still calls after this one, outside the try, would then run first.
  bool canRunInClause(const HighStmt &Try, va_t Handler) const;

  /// Delete \p Handler's calls from the copies of the exits that leave
  /// \p Try, whose `__finally` now runs it, and keep their gotos jumps.
  void runInClause(const HighStmt &Try, va_t Handler);

  /// Find the copies again after the parent's statements changed.
  void rebuild();

  /// The exit whose handler calls end \p Stmts before \p End, followed by
  /// nothing but the values its jump's block moves: a copy whose jump
  /// structuring folded into falling off the list.
  const MedSEHLocalUnwindExit *fallingCopy(const std::vector<HighStmt> &Stmts,
                                           size_t End) const;

private:
  struct Copy {
    std::vector<HighStmt> *List = nullptr;
    /// The positions of the handler calls, in order, then of the jump.
    std::vector<size_t> Calls;
    size_t Jump = 0;
    const MedSEHLocalUnwindExit *Exit = nullptr;
  };

  /// The handler a statement calls for an exit, as the whole statement.
  const MedSEHLocalUnwindExit::Handler *
  handlerCall(const HighStmt &S, const MedSEHLocalUnwindExit *&Exit) const;
  void collect(std::vector<HighStmt> &Stmts);

  HighFunc &Parent;
  std::vector<Copy> Copies;
  /// Exits with a handler call that no copy holds.
  std::set<va_t> Unstructured;
  /// The jump of each copy, and its exit's call address.
  std::map<const HighStmt *, va_t> Jumps;
};

/// The addresses control reaches when it falls past \p Stmts[\p Index]: the
/// statements after it up to the first that prints anything.
std::set<va_t> fallthroughAddresses(const std::vector<HighStmt> &Stmts,
                                    size_t Index);

/// Remove the gotos that end \p Try's protected body only to land where the
/// try falls through anyway, at an address of \p Continuation, unless a jump
/// lands on one (\p Labels) or it ends a local unwind (\p Exits).  The
/// writer leaves such a goto out; with a filled `__finally`, C would read it
/// as a jump out of the try, an abnormal termination, where the machine
/// leaves normally.
void dropFallthroughExits(HighStmt &Try, const std::set<va_t> &Continuation,
                          const std::set<va_t> &Labels,
                          const UnwindExits &Exits);

/// Give back the jump of each local-unwind copy (\p Exits) that ends \p Try's
/// protected body by falling out of it to its target, an address of
/// \p Continuation.  Structuring folds a jump to what follows into falling
/// through, the same thing while the `__finally` is empty; a filled one runs
/// for the jump, an abnormal termination, as the machine runs the handler.
void restoreFallingUnwindJumps(HighStmt &Try,
                               const std::set<va_t> &Continuation,
                               const UnwindExits &Exits);

/// Whether a filled `__finally` keeps \p Try's meaning: its protected body
/// leaves the try by a jump only at the end of a local-unwind exit
/// (\p Exits), nests no exception region but a `__finally` or an `__except`
/// whose handler runs in its clause, and does not unwind locally otherwise.
bool protectedBodyQualifies(const HighStmt &Try, const UnwindExits &Exits);

/// What a dropped `return` still does: a call in its value, as a statement.
std::optional<HighStmt> returnedWork(const HighStmt &Return);

/// A reducible try whose one clause is a `__finally` with no body yet.
bool isEmptyFinallyRegion(const HighStmt &S);

/// `if (AbnormalTermination()) { Abnormal }`: the handler's unwind work, which
/// leaves the compiler's normal-path code to run once on a normal exit.
HighStmt guardAbnormalWork(std::vector<HighStmt> Abnormal);

/// Each architecture's projection (X86/X64SEHFinallyBodies.cpp,
/// ARM/ARMSEHFinallyBodies.cpp).
void attachX64(std::vector<HighFunc> &Funcs, std::set<va_t> *Merged);
void attachARM(std::vector<HighFunc> &Funcs, Arch TargetArch,
               std::set<va_t> *Merged);

} // namespace neverd::seh_finally

#endif // NEVERD_IR_HIGH_STRUCTURE_SEHFINALLYBODIESDETAIL_H
