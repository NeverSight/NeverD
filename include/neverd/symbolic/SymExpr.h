//===- SymExpr.h - Hash-consed symbolic bitvector expressions ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A hash-consed (interned) directed acyclic graph of fixed-width bitvector
/// expressions.  This is the common currency of semantic optimisation:
/// symbolic execution produces these, the simplifier rewrites them, the MBA
/// solver replaces them, and the printer renders them.
///
/// Three properties drive the design:
///
///   1. *Interning.*  Every distinct expression exists exactly once, so
///      structural equality is `SymRef == SymRef` — a 32-bit integer compare
///      rather than a recursive walk or a hash comparison.  Shared subterms
///      cost one word, which matters because obfuscated code produces DAGs
///      that are exponentially larger when viewed as trees.
///
///   2. *Arbitrary width.*  Constants are \c llvm::APInt, so a 256-bit EVM
///      word is as ordinary as an 8-bit byte.  Every operation is modulo
///      2^Width.
///
///   3. *Canonical form on construction.*  The `mk*` builders flatten nested
///      associative operators, sort commutative operands, collect like terms
///      in a sum, and fold constants.  Two expressions that differ only by
///      those rearrangements intern to the same node, so a great deal of
///      obfuscation collapses before any rewrite rule runs.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMEXPR_H
#define NEVERD_SYMBOLIC_SYMEXPR_H

#include "neverd/Common.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace neverd::symbolic {

/// Operators of the bitvector expression language.
///
/// The set is deliberately close to SMT-LIB's QF_BV so that a solver bridge is
/// a direct mapping, but it is flattened: \c Add, \c Mul, \c And, \c Or and
/// \c Xor are n-ary and their operands are kept sorted, which is what makes
/// commutative/associative rearrangement invisible to the rest of the engine.
///
/// Several surface operators have no node of their own because they are
/// canonicalized away on construction:
///   - `a - b`  becomes  `Add(a, Mul(-1, b))`
///   - `-a`     becomes  `Mul(-1, a)`
///   - `a != b` becomes  `Not(Eq(a, b))` at width 1
///   - `a > b`  becomes  `Ult(b, a)` and friends, by swapping operands
enum class SymOp : uint8_t {
  /// Literal.  For Width <= 64 the value lives inline in SymNode::Aux;
  /// wider literals index the context's wide-constant pool.
  Const,
  /// Free variable.  SymNode::Aux is an index into the context's variable
  /// table.
  Var,

  // --- n-ary, associative and commutative, operands sorted ---
  Add,
  Mul,
  And,
  Or,
  Xor,

  // --- unary ---
  /// Bitwise complement.  Kept primitive rather than rewritten to `-x - 1`
  /// because it is a generator of the bitwise algebra the MBA solver works in.
  Not,

  // --- binary, order significant ---
  Shl,
  LShr,
  AShr,
  UDiv,
  SDiv,
  URem,
  SRem,
  Rol,
  Ror,

  // --- structural ---
  /// Bit-field extract.  SymNode::Aux is the index of the lowest extracted
  /// bit; the node's Width gives how many bits are taken.
  Extract,
  /// Concatenation, most significant operand first.  Width is the sum of the
  /// operand widths.
  Concat,
  ZExt,
  SExt,
  /// `Ite(c, t, e)` with `c` of width 1.
  Ite,

  // --- predicates, always width 1 ---
  Eq,
  Ult,
  Ule,
  Slt,
  Sle,
};

/// True for the n-ary operators whose operands are flattened and sorted.
constexpr bool isVariadic(SymOp Op) {
  return Op == SymOp::Add || Op == SymOp::Mul || Op == SymOp::And ||
         Op == SymOp::Or || Op == SymOp::Xor || Op == SymOp::Concat;
}

/// True for operators whose operands may be reordered freely.  \c Concat is
/// variadic but *not* commutative, which is why this is a separate predicate.
constexpr bool isCommutative(SymOp Op) {
  return Op == SymOp::Add || Op == SymOp::Mul || Op == SymOp::And ||
         Op == SymOp::Or || Op == SymOp::Xor;
}

/// True for the purely bitwise operators.  The MBA solver uses this to decide
/// whether a subtree lives in the boolean algebra it can solve directly.
constexpr bool isBitwise(SymOp Op) {
  return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
         Op == SymOp::Not;
}

/// True for the width-1 comparison operators.
constexpr bool isPredicate(SymOp Op) {
  return Op == SymOp::Eq || Op == SymOp::Ult || Op == SymOp::Ule ||
         Op == SymOp::Slt || Op == SymOp::Sle;
}

const char *symOpName(SymOp Op);

/// A handle to an interned node.  Cheap to copy, and equality is structural
/// equality of the expressions themselves.
class SymRef {
public:
  constexpr SymRef() = default;
  constexpr explicit SymRef(uint32_t I) : Index(I) {}

  constexpr uint32_t index() const { return Index; }
  constexpr bool isValid() const { return Index != kInvalid; }
  constexpr explicit operator bool() const { return isValid(); }

  friend constexpr bool operator==(SymRef A, SymRef B) {
    return A.Index == B.Index;
  }
  friend constexpr bool operator!=(SymRef A, SymRef B) {
    return A.Index != B.Index;
  }
  /// Total order used to canonicalize commutative operand lists.  It has no
  /// semantic meaning; it only has to be deterministic for a given context.
  friend constexpr bool operator<(SymRef A, SymRef B) {
    return A.Index < B.Index;
  }

private:
  static constexpr uint32_t kInvalid = ~uint32_t(0);
  uint32_t Index = kInvalid;
};

/// One interned node.  Operands live in the context's shared operand pool so
/// that this stays a small, trivially copyable record.
struct SymNode {
  SymOp Op = SymOp::Const;
  /// Result width in bits.  Never zero for a valid node.
  uint32_t Width = 0;
  /// Offset of the first operand in SymContext's operand pool.
  uint32_t FirstOperand = 0;
  uint32_t NumOperands = 0;
  /// Operator-specific payload: the inline value of a narrow \c Const, the
  /// variable id of a \c Var, or the low bit index of an \c Extract.
  uint64_t Aux = 0;
};

/// A free variable's name and width.
struct SymVarInfo {
  std::string Name;
  uint32_t Width = 0;
};

/// Owner of a set of interned expressions.
///
/// All \c SymRef values are indices into one context; mixing refs from
/// different contexts is a programming error.  A context only grows — nodes
/// are never freed individually — which suits the analyse-then-discard
/// lifetime of a decompilation unit and keeps refs stable.
class SymContext {
public:
  SymContext();

  //===--------------------------------------------------------------------===//
  // Leaves
  //===--------------------------------------------------------------------===//

  SymRef mkConst(const llvm::APInt &Val);
  /// A literal of \p Width bits.  \p Val is taken modulo 2^Width, so a lifter
  /// may hand over a machine word without masking it first.
  SymRef mkConst(uint32_t Width, uint64_t Val);
  SymRef mkZero(uint32_t Width) { return mkConst(Width, 0); }
  SymRef mkOne(uint32_t Width) { return mkConst(Width, 1); }
  /// The all-ones value, i.e. -1 and also the neutral element of \c And.
  SymRef mkOnes(uint32_t Width);
  SymRef mkTrue() { return mkConst(1, 1); }
  SymRef mkFalse() { return mkConst(1, 0); }

  /// Intern a variable by name.  Re-requesting an existing name returns the
  /// same node; the width must match the original declaration.
  SymRef mkVar(llvm::StringRef Name, uint32_t Width);
  /// Declare a fresh variable with a generated name that cannot collide with
  /// an existing one.  Used by the MBA solver to stand in for a subtree it
  /// cannot see inside of.
  SymRef mkFreshVar(uint32_t Width, llvm::StringRef Prefix = "t");

  //===--------------------------------------------------------------------===//
  // Arithmetic
  //===--------------------------------------------------------------------===//

  SymRef mkAdd(llvm::ArrayRef<SymRef> Ops);
  SymRef mkAdd(SymRef A, SymRef B) { return mkAdd({A, B}); }
  SymRef mkSub(SymRef A, SymRef B);
  SymRef mkNeg(SymRef A);
  SymRef mkMul(llvm::ArrayRef<SymRef> Ops);
  SymRef mkMul(SymRef A, SymRef B) { return mkMul({A, B}); }
  SymRef mkUDiv(SymRef A, SymRef B);
  SymRef mkSDiv(SymRef A, SymRef B);
  SymRef mkURem(SymRef A, SymRef B);
  SymRef mkSRem(SymRef A, SymRef B);

  //===--------------------------------------------------------------------===//
  // Bitwise
  //===--------------------------------------------------------------------===//

  SymRef mkAnd(llvm::ArrayRef<SymRef> Ops);
  SymRef mkAnd(SymRef A, SymRef B) { return mkAnd({A, B}); }
  SymRef mkOr(llvm::ArrayRef<SymRef> Ops);
  SymRef mkOr(SymRef A, SymRef B) { return mkOr({A, B}); }
  SymRef mkXor(llvm::ArrayRef<SymRef> Ops);
  SymRef mkXor(SymRef A, SymRef B) { return mkXor({A, B}); }
  SymRef mkNot(SymRef A);
  SymRef mkShl(SymRef A, SymRef B);
  SymRef mkLShr(SymRef A, SymRef B);
  SymRef mkAShr(SymRef A, SymRef B);
  SymRef mkRol(SymRef A, SymRef B);
  SymRef mkRor(SymRef A, SymRef B);

  //===--------------------------------------------------------------------===//
  // Structural
  //===--------------------------------------------------------------------===//

  /// Extract \p Width bits starting at bit \p Low of \p A.
  SymRef mkExtract(SymRef A, uint32_t Low, uint32_t Width);
  /// Concatenate, most significant first.
  SymRef mkConcat(llvm::ArrayRef<SymRef> Ops);
  SymRef mkConcat(SymRef Hi, SymRef Lo) { return mkConcat({Hi, Lo}); }
  SymRef mkZExt(SymRef A, uint32_t Width);
  SymRef mkSExt(SymRef A, uint32_t Width);
  /// Widen or narrow \p A to \p Width, zero-extending or truncating as needed.
  SymRef mkZExtOrTrunc(SymRef A, uint32_t Width);
  SymRef mkIte(SymRef C, SymRef T, SymRef E);

  //===--------------------------------------------------------------------===//
  // Predicates (result width 1)
  //===--------------------------------------------------------------------===//

  SymRef mkEq(SymRef A, SymRef B);
  SymRef mkNe(SymRef A, SymRef B);
  SymRef mkUlt(SymRef A, SymRef B);
  SymRef mkUle(SymRef A, SymRef B);
  SymRef mkUgt(SymRef A, SymRef B) { return mkUlt(B, A); }
  SymRef mkUge(SymRef A, SymRef B) { return mkUle(B, A); }
  SymRef mkSlt(SymRef A, SymRef B);
  SymRef mkSle(SymRef A, SymRef B);
  SymRef mkSgt(SymRef A, SymRef B) { return mkSlt(B, A); }
  SymRef mkSge(SymRef A, SymRef B) { return mkSle(B, A); }

  //===--------------------------------------------------------------------===//
  // Inspection
  //===--------------------------------------------------------------------===//

  const SymNode &node(SymRef R) const { return Nodes[R.index()]; }
  SymOp op(SymRef R) const { return Nodes[R.index()].Op; }
  uint32_t width(SymRef R) const { return Nodes[R.index()].Width; }

  llvm::ArrayRef<SymRef> operands(SymRef R) const {
    const SymNode &N = Nodes[R.index()];
    return llvm::ArrayRef<SymRef>(OperandPool.data() + N.FirstOperand,
                                  N.NumOperands);
  }
  SymRef operand(SymRef R, unsigned I) const { return operands(R)[I]; }
  unsigned numOperands(SymRef R) const { return Nodes[R.index()].NumOperands; }

  bool isConst(SymRef R) const { return op(R) == SymOp::Const; }
  bool isVar(SymRef R) const { return op(R) == SymOp::Var; }
  /// The value of a \c Const node.  Only valid when \c isConst.
  llvm::APInt constValue(SymRef R) const;
  /// The value of \p R when it is a constant, otherwise nothing.
  std::optional<llvm::APInt> asConst(SymRef R) const;
  bool isConstZero(SymRef R) const;
  bool isConstOnes(SymRef R) const;

  uint32_t varId(SymRef R) const { return uint32_t(Nodes[R.index()].Aux); }
  const SymVarInfo &varInfo(uint32_t Id) const { return Vars[Id]; }
  size_t numVars() const { return Vars.size(); }

  /// The id of the variable named \p Name, if one has been declared.  Callers
  /// that would otherwise hit \c mkVar's width assertion — a parser reading
  /// untrusted text, say — use this to check first.
  std::optional<uint32_t> findVar(llvm::StringRef Name) const;

  /// Number of distinct nodes reachable from \p R, i.e. the size of the DAG.
  /// Used to account structural traversal work; shared subterms count once.
  size_t dagSize(SymRef R) const;

  /// Cost of rendering the expression tree rooted at \p R.
  ///
  /// A shared node is charged once per appearance because a textual expression
  /// prints it once per path.  The all-ones literal is free: it represents the
  /// sign of a negation or the mask of a complement rather than a quantity.
  /// Costs are cached as nodes are appended, so asking repeatedly while a
  /// solver builds candidates remains linear in the total number of nodes
  /// interned in this context.
  size_t readabilityCost(SymRef R) const;

  /// Every variable reachable from \p R, in ascending variable-id order.
  void collectVars(SymRef R, llvm::SmallVectorImpl<uint32_t> &Out) const;

  /// Total nodes interned in this context.  Diagnostic only.
  size_t numNodes() const { return Nodes.size(); }

  //===--------------------------------------------------------------------===//
  // Evaluation
  //===--------------------------------------------------------------------===//

  /// Evaluate \p R with \p VarVals supplying a value per variable id.  Each
  /// supplied value is interpreted modulo the variable's declared width.
  llvm::APInt eval(SymRef R, llvm::ArrayRef<llvm::APInt> VarVals) const;

  /// Evaluate \p R when every width involved is at most 64 bits.  The result
  /// is masked to the width of \p R.  This is the hot path for MBA truth-table
  /// construction, where the same expression is evaluated 2^t times.
  uint64_t evalU64(SymRef R, llvm::ArrayRef<uint64_t> VarVals) const;

  /// True when \p R and every node beneath it is at most 64 bits wide, so
  /// \c evalU64 applies.
  bool fitsU64(SymRef R) const;

  //===--------------------------------------------------------------------===//
  // Substitution
  //===--------------------------------------------------------------------===//

  /// Rebuild \p R with each node in \p Map replaced by its image.  Rebuilding
  /// runs through the `mk*` builders, so the result is canonical.
  SymRef substitute(SymRef R, const std::unordered_map<uint32_t, SymRef> &Map);

  /// Replace variable \p VarIdx with \p Val throughout \p R.
  SymRef substituteVar(SymRef R, uint32_t VarIdx, SymRef Val);

  /// Re-apply \p Orig's operator to \p NewOps through the canonicalizing
  /// builders.  Used wherever a node's children have been rewritten and the
  /// parent must be rebuilt: substitution, the simplifier, and the MBA
  /// solver's re-expansion of hidden subterms.
  SymRef rebuild(SymRef Orig, llvm::ArrayRef<SymRef> NewOps);

  //===--------------------------------------------------------------------===//
  // Printing
  //===--------------------------------------------------------------------===//

  /// Render \p R in the infix syntax that \c parseSymExpr accepts.
  std::string toString(SymRef R) const;

private:
  /// Intern a node, returning an existing ref when an identical node exists.
  SymRef intern(SymOp Op, uint32_t Width, llvm::ArrayRef<SymRef> Ops,
                uint64_t Aux);

  /// Split \p R into a coefficient and a base so that `R == Coeff * Base`.
  /// A node that is not a constant multiple yields a coefficient of one.
  /// This is what lets \c mkAdd collect `x + 2*x` into `3*x`.
  void splitCoefficient(SymRef R, llvm::APInt &Coeff, SymRef &Base) const;

  llvm::APInt maskToWidth(const llvm::APInt &V, uint32_t Width) const;

  std::vector<SymNode> Nodes;
  std::vector<SymRef> OperandPool;
  std::vector<SymVarInfo> Vars;
  /// One readability cost per prefix of \c Nodes, filled lazily.
  mutable std::vector<size_t> ReadabilityCosts;
  /// Literals wider than 64 bits, referenced by index from SymNode::Aux.
  std::vector<llvm::APInt> WideConsts;

  /// Hash of a node's identity to the nodes carrying that hash.  Collisions
  /// are resolved by comparing the candidate against each bucket entry.
  std::unordered_map<uint64_t, llvm::SmallVector<uint32_t, 2>> InternTable;
  std::unordered_map<std::string, uint32_t> VarByName;
  std::unordered_map<std::string, uint32_t> WideConstByKey;
  uint32_t FreshCounter = 0;
};

/// Apply \p R's operator to already-evaluated operand values.
///
/// Exposed because both the evaluation plan and the MBA solver's verification
/// need the operator semantics without re-deriving them, and because having
/// exactly one definition of "what does this operator mean" is what keeps the
/// simplifier's folding and the solver's checking from drifting apart.
llvm::APInt evalNodeAP(const SymContext &Ctx, SymRef R,
                       llvm::ArrayRef<llvm::APInt> Args);

/// A compiled plan for evaluating one expression many times.
///
/// MBA solving evaluates a single expression at all 2^t points of the boolean
/// cube, so evaluation is the inner loop of the whole engine.  Walking the
/// expression afresh for each point would visit a shared subterm once per
/// occurrence — the exact cost that makes obfuscated expressions expensive,
/// since obfuscation multiplies occurrences.  Instead this lowers the DAG once
/// into a flat, topologically ordered instruction list whose operands are
/// direct slot indices.  Each evaluation is then a linear sweep with no graph
/// traversal, no hashing and no allocation, which is how the truth-table pass
/// reaches millions of points per second without a JIT.
///
/// The plan borrows its context.  A context only grows, so no operation on it
/// can invalidate a plan.
class SymEvalPlan {
public:
  SymEvalPlan(const SymContext &Ctx, SymRef Root);

  /// Evaluate with a value per variable id, using 64-bit arithmetic.  Requires
  /// \c fitsU64 — every node at most 64 bits wide.
  uint64_t evalU64(llvm::ArrayRef<uint64_t> VarVals);

  /// Evaluate at arbitrary width.
  llvm::APInt eval(llvm::ArrayRef<llvm::APInt> VarVals);

  /// Variable ids referenced by the expression, ascending.
  llvm::ArrayRef<uint32_t> vars() const { return Vars; }

  /// True when every node fits in 64 bits, so \c evalU64 is usable.
  bool fitsU64() const { return FitsU64; }

  /// Steps one evaluation performs — the cost model the solver budgets with.
  size_t numSteps() const { return Steps.size(); }

  SymRef root() const { return Root; }

private:
  /// One node lowered to its operator, width and operand slots.  Slots are
  /// positions in the scratch buffer, which is also the step order, so a
  /// step's operands are always at strictly smaller indices.
  struct Step {
    SymOp Op;
    uint32_t Width;
    uint64_t Aux;
    uint32_t FirstArg;
    uint32_t NumArgs;
    /// The node this step came from, needed to recover a wide literal and to
    /// hand the node back to \c evalNodeAP for the general cases.
    SymRef Node;
  };

  const SymContext &Ctx;
  SymRef Root;
  std::vector<Step> Steps;
  /// Concatenated operand slot lists, indexed by Step::FirstArg.
  std::vector<uint32_t> ArgSlots;
  std::vector<uint32_t> Vars;
  std::vector<uint64_t> ScratchU64;
  std::vector<llvm::APInt> ScratchAP;
  uint32_t RootSlot = 0;
  bool FitsU64 = true;
};

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMEXPR_H
