//===- CnfEncoder.h - Circuits to conjunctive normal form -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Turns a boolean circuit into clauses.
///
/// The formulas NeverD asks about are circuits — an adder, a comparator, a
/// multiplexer chain — and a circuit written out as a formula in conjunctive
/// normal form by distributing its operators is exponentially larger than the
/// circuit.  The way out is definitional: give every gate its own variable and
/// state, in a handful of clauses, that the variable equals the gate.  The
/// result is linear in the size of the circuit and equisatisfiable with it,
/// which is all a satisfiability question needs.  Reading a model back is
/// unchanged, because a gate variable is true in the model exactly when the
/// gate it names is.
///
/// Two properties do most of the work in practice.
///
/// *Structural sharing.*  A gate is looked up by operator and operand list
/// before it is created, so the shared subterms of a hash-consed expression
/// stay shared all the way down to the clauses.  Bit-blasting an expression
/// DAG whose tree expansion is exponential therefore stays proportional to the
/// DAG.
///
/// *Constant folding.*  One variable is held true, giving the encoder a true
/// and a false literal that behave like any other.  Every builder recognises
/// them, so a constant operand collapses its gate instead of encoding it.
/// That matters more than it sounds: bit-blasting is full of constants —
/// the carry into the low bit of an adder, the zero fill of an extension, the
/// bits of a literal operand — and folding them removes most of the clauses a
/// naive encoding of the same circuit would emit.
///
/// A gate's definition has two halves, and a caller that only ever forces the
/// gate one way needs only one of them.  \c GatePolarity says which halves to
/// emit; asking later for the other half adds it, so narrowing the polarity is
/// an optimisation rather than a commitment.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_CNFENCODER_H
#define NEVERD_SOLVER_CNFENCODER_H

#include "neverd/solver/SatSolver.h"
#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace neverd::solver {

/// Which half of a gate's definition a caller needs.
///
/// A gate output \c g standing for a formula \c f is defined by two
/// implications.  `g -> f` is what makes forcing \c g true meaningful, and
/// `f -> g` is what makes forcing it false meaningful.  A gate that is only
/// ever asserted true — the root of an assertion, or an operand of an
/// and-gate that is itself only asserted true — needs the first alone, and
/// emitting the second only gives the search more clauses to walk.
///
/// Getting this wrong is not a performance mistake, so the default is both
/// halves and a narrower choice has to be asked for.  A caller that cannot
/// show which way a gate will be forced should not narrow it.
enum class GatePolarity : uint8_t {
  /// The gate may be forced true, so its definition must follow from it.
  Positive,
  /// The gate may be forced false, so it must follow from its definition.
  Negative,
  /// Both, which is what any gate under a non-monotone parent needs.
  Both,
};

/// Builds gates over a \c SatSolver's variables and emits their definitions.
///
/// The encoder borrows the solver and adds variables and clauses to it.  Any
/// number of encoders may share one solver, though a single one is what keeps
/// structural sharing effective.
class CnfEncoder {
public:
  explicit CnfEncoder(SatSolver &Solver);

  SatSolver &solver() { return Solver; }
  const SatSolver &solver() const { return Solver; }

  //===--------------------------------------------------------------------===//
  // Constants and fresh variables
  //===--------------------------------------------------------------------===//

  /// A literal that is true in every model, and its complement.  Both are
  /// ordinary literals: they may be stored in a bit vector, handed to a gate
  /// builder, or asserted, and the builders recognise them and fold.
  SatLit trueLit() const { return True; }
  SatLit falseLit() const { return ~True; }
  SatLit constant(bool Value) const { return True.withPolarity(Value); }

  bool isConstant(SatLit L) const { return L.var() == True.var(); }
  bool isTrueLit(SatLit L) const { return L == True; }
  bool isFalseLit(SatLit L) const { return L == ~True; }

  /// A variable nothing yet constrains, as a positive literal.  Used for the
  /// bits of a free bitvector variable, and for any value the caller intends
  /// to define with its own clauses.
  SatLit freshLit();

  //===--------------------------------------------------------------------===//
  // Gates
  //===--------------------------------------------------------------------===//

  SatLit mkAnd(SatLit A, SatLit B, GatePolarity P = GatePolarity::Both);
  SatLit mkAnd(llvm::ArrayRef<SatLit> Ins, GatePolarity P = GatePolarity::Both);
  SatLit mkOr(SatLit A, SatLit B, GatePolarity P = GatePolarity::Both);
  SatLit mkOr(llvm::ArrayRef<SatLit> Ins, GatePolarity P = GatePolarity::Both);

  /// Exclusive or.  Both halves of the definition are always emitted: an
  /// exclusive or is monotone in neither operand, so there is no half to drop.
  SatLit mkXor(SatLit A, SatLit B);
  SatLit mkXor(llvm::ArrayRef<SatLit> Ins);

  /// True when the operands agree.
  SatLit mkEquiv(SatLit A, SatLit B) { return ~mkXor(A, B); }

  /// `C ? T : E`.
  SatLit mkIte(SatLit C, SatLit T, SatLit E);

  /// True when at least two of three operands are.  This is the carry out of
  /// a full adder, and the reason it is named is that the adder wants it
  /// shared with nothing else while the sum wants the same three operands.
  SatLit mkMajority(SatLit A, SatLit B, SatLit C);

  /// The sum and carry of three bits.  Every adder in the bit-blaster is a
  /// chain of these, so it is worth one entry point rather than two lookups.
  void mkFullAdder(SatLit A, SatLit B, SatLit CarryIn, SatLit &Sum,
                   SatLit &CarryOut);

  //===--------------------------------------------------------------------===//
  // Assertions
  //===--------------------------------------------------------------------===//

  /// Constrain \p L to hold.  Returns false once the formula is contradictory,
  /// matching \c SatSolver::addClause.
  bool assertTrue(SatLit L);
  bool assertFalse(SatLit L) { return assertTrue(~L); }
  bool assertEquiv(SatLit A, SatLit B);
  bool assertImplies(SatLit A, SatLit B);

  /// Gates created, shared ones counted once.  Diagnostic only.
  size_t numGates() const { return Gates.size(); }

private:
  /// The operators a gate node can carry.  Or-gates are stored as and-gates
  /// over complemented operands, so one shape covers both and a formula
  /// written either way finds the same node.
  enum class GateKind : uint8_t { And, Xor, Ite, Majority };

  struct Gate {
    GateKind Kind;
    /// Which halves of the definition have been emitted, as a mask of
    /// \c kPositiveEmitted and \c kNegativeEmitted.
    uint8_t Emitted;
    uint32_t FirstOperand;
    uint32_t NumOperands;
    /// The literal this gate's definition constrains.
    SatLit Out;
  };

  static constexpr uint8_t kPositiveEmitted = 1;
  static constexpr uint8_t kNegativeEmitted = 2;

  /// Find or create a gate, emitting whichever halves \p P asks for and are
  /// not already there.
  SatLit gate(GateKind Kind, llvm::ArrayRef<SatLit> Ins, GatePolarity P);
  void emit(uint32_t Index, GatePolarity P);
  void emitAnd(const Gate &G, bool Positive);
  void emitXor(const Gate &G);
  void emitIte(const Gate &G, bool Positive);
  void emitMajority(const Gate &G, bool Positive);

  llvm::ArrayRef<SatLit> operandsOf(const Gate &G) const {
    return llvm::ArrayRef<SatLit>(OperandPool.data() + G.FirstOperand,
                                  G.NumOperands);
  }

  SatSolver &Solver;
  SatLit True;

  std::vector<Gate> Gates;
  std::vector<SatLit> OperandPool;

  /// Identity hash of a gate to the gates carrying it.  Collisions are
  /// resolved by comparing candidates, and the map is never iterated, so
  /// nothing here can make the encoding order-dependent.
  std::unordered_map<uint64_t, llvm::SmallVector<uint32_t, 2>> GateTable;
};

} // namespace neverd::solver

#endif // NEVERD_SOLVER_CNFENCODER_H
