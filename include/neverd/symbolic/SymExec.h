//===- SymExec.h - Symbolic execution of NeverD IR --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runs lifted code without running it, producing an expression for every
/// value the code computes and a condition for every branch it could take.
///
/// It executes \c NdOp, which is what makes it worth having.  Writing a
/// symbolic engine against a machine means writing that machine's instruction
/// semantics, which is a five-figure line count per architecture and the
/// reason engines exist for x86 and almost nowhere else.  NeverD has already
/// paid that cost in its lifters and collapsed six targets — x86, x86-64,
/// ARM32, AArch64, EVM and SBF — onto one small operator set.  Executing that
/// set symbolically covers all six from the code below, and covers whatever is
/// lifted next for free.
///
/// Widths are exact.  Each operand is read at the width it declares and the
/// operation happens there, rather than being widened to a machine word and
/// masked afterwards — a shortcut that is fine for constant folding and wrong
/// for a signed comparison of narrow operands.
///
/// What the engine cannot model it names rather than guesses at: a call's
/// result, a floating-point operation, a population count all become fresh
/// inputs, and execution carries on around them.  That is the same bargain the
/// simplifier makes, and it is what keeps a result that mentions an unknown
/// still exactly true.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMEXEC_H
#define NEVERD_SYMBOLIC_SYMEXEC_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/symbolic/SymState.h"

#include <vector>

namespace neverd::symbolic {

/// What executing an operation did to the flow of control.
enum class StepResult : uint8_t {
  /// Control fell through; keep going.
  Continue,
  /// An unconditional branch.  \c SymExec::branchTarget holds where to.
  Branch,
  /// A conditional branch.  \c SymExec::branchCondition holds the predicate
  /// and \c branchTarget the address taken when it holds.
  CondBranch,
  /// A branch through a computed address, which \c branchTarget holds as an
  /// expression rather than a number.
  IndirectBranch,
  Return,
  /// An operation whose effect the engine declined to model.  Its output, if
  /// any, has been given a fresh input, so the state is still consistent and
  /// execution may continue.
  Unmodelled,
};

class SymExec {
public:
  SymExec(SymContext &Ctx, SymState &State) : Ctx(Ctx), State(State) {}

  /// Execute one operation against the state.
  StepResult step(const LowOp &Op);

  /// Execute operations in order, stopping when one changes control flow.
  /// An unmodelled operation has already written a named unknown and therefore
  /// falls through like an ordinary operation.  Returns how many were executed,
  /// the last of them included.
  size_t run(llvm::ArrayRef<LowOp> Ops);

  /// Where the last branch went, as an expression.  Constant for a direct
  /// branch; whatever the code computed for an indirect one.
  SymRef branchTarget() const { return Target; }

  /// The predicate of the last conditional branch, one bit wide.
  SymRef branchCondition() const { return Condition; }

  /// The conditions assumed along the way, in the order they were assumed.
  /// Empty until \c assumeBranch is called: stepping a conditional branch
  /// reports the predicate and takes no side, because which side to take is
  /// the caller's decision.
  llvm::ArrayRef<SymRef> pathConstraints() const { return Constraints; }

  /// Record that the last conditional branch went one way.  \p Taken selects
  /// the branch's own direction.
  void assumeBranch(bool Taken);
  void assume(SymRef Condition);

  /// The conjunction of everything assumed, or the constant true when nothing
  /// has been.
  SymRef pathPredicate() const;

  /// Registers a call leaves alone.  Until this is set a call forgets every
  /// register, which is correct and loses more than it has to.
  void setCallPreservedRegisters(std::vector<SymRegisterRange> Ranges) {
    CallPreserved = std::move(Ranges);
  }

  /// How many operations required unknown values or conservative havoc rather
  /// than exact execution.
  ///
  /// Worth asking before believing an absence.  That a value came out not
  /// mentioning something is only evidence it does not depend on it if
  /// everything in between was actually carried out; one unmodelled operation
  /// severs every dependence running through it, and the result looks exactly
  /// like independence.
  unsigned unmodelledCount() const { return Unmodelled; }

private:
  SymRef read(const NdVar &V);
  void writeResult(const NdVar &Output, SymRef Value);
  /// Fit \p Value to \p Bits, extending with zeros or dropping the top.
  SymRef fit(SymRef Value, uint32_t Bits);

  StepResult stepBinary(const LowOp &Op);
  StepResult stepCompare(const LowOp &Op);
  StepResult stepUnary(const LowOp &Op);
  StepResult stepBoolean(const LowOp &Op);
  StepResult stepMemory(const LowOp &Op);
  StepResult stepControl(const LowOp &Op);
  StepResult unmodelled(const LowOp &Op);

  SymContext &Ctx;
  SymState &State;

  SymRef Target;
  SymRef Condition;
  std::vector<SymRef> Constraints;
  std::vector<SymRegisterRange> CallPreserved;
  unsigned Unmodelled = 0;
};

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMEXEC_H
