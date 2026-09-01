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
/// result or a floating-point operation becomes a fresh input, and execution
/// carries on around it.  That is the same bargain the simplifier makes, and
/// it is what keeps a result that mentions an unknown still exactly true.
///
/// The bar for naming rather than modelling is what the expression language
/// can say exactly, not what is convenient.  A population count, a leading
/// zero count and a bit-field insert or extract all have exact models here —
/// costing a node per bit for the first two — because the concrete emulator
/// that shadows this one computes them, and two engines over one operator set
/// that disagree about an opcode are worse than either alone.
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

/// What a complete call contract says about memory not named by an explicit
/// write.  This vocabulary is deliberately independent of any analysis that
/// supplies the contract, so Symbolic never depends on a higher-level client.
enum class SymCallMemoryEffect : uint8_t {
  Preserve,
  Havoc,
};

/// One exact memory write performed by a summarized call.
struct SymCallMemoryWrite {
  SymRef Address;
  SymRef Value;
};

/// Analysis-neutral effects for one exact call occurrence.
///
/// A provider may return this only when it covers the complete contract needed
/// by symbolic execution.  An invalid ReturnValue means an unconstrained
/// return, which is still a known contract; an absent effect retains the
/// ordinary conservative call havoc.
struct SymCallEffect {
  SymCallMemoryEffect Memory = SymCallMemoryEffect::Havoc;
  SymRef ReturnValue;
  std::vector<SymCallMemoryWrite> Writes;
  std::vector<SymRef> Constraints;
};

class SymExec {
public:
  SymExec(SymContext &Ctx, SymState &State) : Ctx(Ctx), State(State) {}

  /// Execute one operation against the state.
  StepResult step(const LowOp &Op);

  /// Execute one operation, applying \p CallEffect when \p Op is a call.  The
  /// effect is ignored for every other opcode.  A null effect preserves the
  /// fail-closed default used by \c step(const LowOp&).
  StepResult step(const LowOp &Op, const SymCallEffect *CallEffect);

  /// Read one operand through the same width, space, and memory semantics used
  /// by \c step.  This may materialise an untouched symbolic input in \c State.
  /// Concrete shadow execution uses it instead of duplicating NdVar semantics.
  SymRef operandValue(const NdVar &V);

  /// Execute operations in order, stopping when one changes control flow.
  /// An unmodelled operation has already written a named unknown and therefore
  /// falls through like an ordinary operation.  Returns how many were executed,
  /// the last of them included.
  size_t run(llvm::ArrayRef<LowOp> Ops);

  /// Where the last control transfer went, as an expression.  Constant for a
  /// direct branch; whatever the code computed for an indirect branch or an
  /// explicitly targeted return.
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

  /// Operations whose value semantics were replaced by a fresh unknown.
  unsigned opaqueOperationCount() const { return OpaqueOperations; }

  /// Calls stepped with conservative register and memory havoc.
  unsigned callHavocCount() const { return CallHavocs; }

  /// Stores modelled at their target region while conservatively forgetting
  /// other regions that may alias it.
  unsigned memoryHavocCount() const { return MemoryHavocs; }

private:
  SymRef read(const NdVar &V);
  void writeResult(const NdVar &Output, SymRef Value);
  /// Fit \p Value to \p Bits, extending with zeros or dropping the top.
  SymRef fit(SymRef Value, uint32_t Bits);

  StepResult stepBinary(const LowOp &Op);
  StepResult stepCompare(const LowOp &Op);
  StepResult stepUnary(const LowOp &Op);
  StepResult stepBoolean(const LowOp &Op);
  StepResult stepBits(const LowOp &Op);
  StepResult stepMemory(const LowOp &Op);
  StepResult stepControl(const LowOp &Op, const SymCallEffect *CallEffect);
  StepResult unmodelled(const LowOp &Op);

  SymContext &Ctx;
  SymState &State;

  SymRef Target;
  SymRef Condition;
  std::vector<SymRef> Constraints;
  std::vector<SymRegisterRange> CallPreserved;
  unsigned Unmodelled = 0;
  unsigned OpaqueOperations = 0;
  unsigned CallHavocs = 0;
  unsigned MemoryHavocs = 0;
};

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMEXEC_H
