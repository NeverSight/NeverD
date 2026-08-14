//===- CnfEncoder.cpp - Gate definitions as clauses -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements gate construction, the normalisation that makes structurally
/// equal gates share one variable, and the clauses that define each gate.
///
/// Most of the code here is normalisation rather than clause emission, and
/// that is the right proportion.  A gate that is folded away costs nothing to
/// solve, and a gate that is recognised as one already built costs nothing
/// either; only what survives both reaches the clause database.  Bit-blasting
/// produces enormous numbers of gates that are one of those two things —
/// carries into a constant, multiplexers on a condition that is already
/// decided, the same comparison reached along two paths — so the folding rules
/// below decide the size of the formula far more than the clause count of any
/// single gate does.
///
/// The normal forms are chosen so that terms which differ only by algebra
/// arrive at the same node.  And-gates absorb or-gates by complementing their
/// operands, exclusive-or pulls every complement out to its result, and a
/// multiplexer's condition and true-arm are made positive.  Each of those
/// turns a family of equivalent spellings into one.
///
//===----------------------------------------------------------------------===//

#include "neverd/solver/CnfEncoder.h"

#include "neverd/solver/SatSolver.h"
#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>

namespace neverd::solver {

namespace {

/// Mix one word into a running hash.  The constant is the odd integer nearest
/// the golden ratio scaled to 64 bits, which is the usual choice for this and
/// is what the expression context hashes with.
inline uint64_t mixHash(uint64_t H, uint64_t V) {
  H ^= V + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
  return H;
}

/// The complementary polarity request.  Storing an or-gate as an and-gate over
/// complemented operands complements which half of the definition is needed
/// along with it.
GatePolarity flip(GatePolarity P) {
  switch (P) {
  case GatePolarity::Positive:
    return GatePolarity::Negative;
  case GatePolarity::Negative:
    return GatePolarity::Positive;
  case GatePolarity::Both:
    break;
  }
  return GatePolarity::Both;
}

/// Beyond this many operands an exclusive-or is built as a chain of two-input
/// gates.  A single node would need one clause per assignment of its inputs,
/// which is fine at the two and three inputs an adder wants and is a mistake
/// at any width a caller might hand over.
constexpr size_t kMaxXorFanIn = 3;

} // namespace

CnfEncoder::CnfEncoder(SatSolver &Solver) : Solver(Solver) {
  // One variable is held true so that the builders have a literal to fold
  // against.  It is never branched on: a unit clause fixes it before the
  // search makes its first decision, and it stays fixed for the whole run.
  True = SatLit::positive(Solver.newVar(/*Decision=*/false));
  Solver.addClause(True);
}

SatLit CnfEncoder::freshLit() {
  SatVar V = Solver.newVar(/*Decision=*/true);
  assert(V != kInvalidSatVar && "the solver ran out of variables");
  return SatLit::positive(V);
}

//===----------------------------------------------------------------------===//
// Conjunction and disjunction
//===----------------------------------------------------------------------===//

SatLit CnfEncoder::mkAnd(SatLit A, SatLit B, GatePolarity P) {
  const SatLit Ins[] = {A, B};
  return mkAnd(Ins, P);
}

SatLit CnfEncoder::mkAnd(llvm::ArrayRef<SatLit> Ins, GatePolarity P) {
  llvm::SmallVector<SatLit, 8> Terms(Ins.begin(), Ins.end());

  // Sorting puts duplicates and complementary pairs next to each other, and
  // fixes one spelling for operands that arrived in a different order.
  llvm::sort(Terms);

  size_t Kept = 0;
  SatLit Prev;
  for (SatLit L : Terms) {
    if (isTrueLit(L))
      continue;
    if (isFalseLit(L))
      return falseLit();
    if (Prev.isValid() && L == Prev)
      continue;
    if (Prev.isValid() && L == ~Prev)
      return falseLit();
    Terms[Kept++] = L;
    Prev = L;
  }
  Terms.truncate(Kept);

  if (Terms.empty())
    return trueLit();
  if (Terms.size() == 1)
    return Terms[0];
  return gate(GateKind::And, Terms, P);
}

SatLit CnfEncoder::mkOr(SatLit A, SatLit B, GatePolarity P) {
  const SatLit Ins[] = {A, B};
  return mkOr(Ins, P);
}

SatLit CnfEncoder::mkOr(llvm::ArrayRef<SatLit> Ins, GatePolarity P) {
  llvm::SmallVector<SatLit, 8> Terms;
  Terms.reserve(Ins.size());
  for (SatLit L : Ins)
    Terms.push_back(~L);
  return ~mkAnd(Terms, flip(P));
}

//===----------------------------------------------------------------------===//
// Exclusive or
//===----------------------------------------------------------------------===//

SatLit CnfEncoder::mkXor(SatLit A, SatLit B) {
  const SatLit Ins[] = {A, B};
  return mkXor(Ins);
}

SatLit CnfEncoder::mkXor(llvm::ArrayRef<SatLit> Ins) {
  // Complements and true constants only change the parity of the result, so
  // they are counted and stripped.  What is left is a set of positive
  // literals, which is the normal form the gate table keys on.
  bool Complement = false;
  llvm::SmallVector<SatLit, 8> Terms;
  Terms.reserve(Ins.size());

  for (SatLit L : Ins) {
    if (isConstant(L)) {
      Complement ^= isTrueLit(L);
      continue;
    }
    if (L.isNegated()) {
      Complement = !Complement;
      L = ~L;
    }
    Terms.push_back(L);
  }

  llvm::sort(Terms);

  // A term appearing twice contributes nothing to a parity.
  size_t Kept = 0;
  for (size_t I = 0, E = Terms.size(); I < E;) {
    if (I + 1 < E && Terms[I] == Terms[I + 1]) {
      I += 2;
      continue;
    }
    Terms[Kept++] = Terms[I];
    ++I;
  }
  Terms.truncate(Kept);

  if (Terms.empty())
    return constant(Complement);
  if (Terms.size() == 1)
    return Terms[0].withPolarity(!Complement);

  if (Terms.size() > kMaxXorFanIn) {
    SatLit Acc = Terms[0];
    for (size_t I = 1, E = Terms.size(); I < E; ++I)
      Acc = mkXor(Acc, Terms[I]);
    return Acc.withPolarity(!Complement);
  }

  return gate(GateKind::Xor, Terms, GatePolarity::Both)
      .withPolarity(!Complement);
}

//===----------------------------------------------------------------------===//
// Selection
//===----------------------------------------------------------------------===//

SatLit CnfEncoder::mkIte(SatLit C, SatLit T, SatLit E) {
  if (isTrueLit(C))
    return T;
  if (isFalseLit(C))
    return E;
  if (T == E)
    return T;
  // Selecting between a literal and its complement is a parity, and saying so
  // matters: an exclusive-or gate is half the clauses of a multiplexer and is
  // what a shift or a conditional negation is really made of.
  if (T == ~E)
    return mkXor(C, E);

  if (isTrueLit(T))
    return mkOr(C, E);
  if (isFalseLit(T))
    return mkAnd(~C, E);
  if (isTrueLit(E))
    return mkOr(~C, T);
  if (isFalseLit(E))
    return mkAnd(C, T);

  // An arm that repeats the condition is already known inside its own branch.
  if (T == C)
    return mkOr(C, E);
  if (T == ~C)
    return mkAnd(~C, E);
  if (E == C)
    return mkAnd(C, T);
  if (E == ~C)
    return mkOr(~C, T);

  // Normalise the two spellings of the same selection: complementing the
  // condition swaps the arms, and complementing both arms complements the
  // result.
  if (C.isNegated()) {
    C = ~C;
    std::swap(T, E);
  }
  if (T.isNegated()) {
    const SatLit Ins[] = {C, ~T, ~E};
    return ~gate(GateKind::Ite, Ins, GatePolarity::Both);
  }

  const SatLit Ins[] = {C, T, E};
  return gate(GateKind::Ite, Ins, GatePolarity::Both);
}

SatLit CnfEncoder::mkMajority(SatLit A, SatLit B, SatLit C) {
  // Fold a constant operand: with one input decided, a majority of three is a
  // conjunction or a disjunction of the other two.  Adders feed a constant
  // carry into their lowest bit, so this fires on every addition.
  if (isConstant(A))
    return isTrueLit(A) ? mkOr(B, C) : mkAnd(B, C);
  if (isConstant(B))
    return isTrueLit(B) ? mkOr(A, C) : mkAnd(A, C);
  if (isConstant(C))
    return isTrueLit(C) ? mkOr(A, B) : mkAnd(A, B);

  // Two equal inputs decide the outcome; two complementary inputs cancel and
  // leave the third.
  if (A == B)
    return A;
  if (A == C)
    return A;
  if (B == C)
    return B;
  if (A == ~B)
    return C;
  if (A == ~C)
    return B;
  if (B == ~C)
    return A;

  SatLit Ins[] = {A, B, C};
  llvm::sort(Ins);
  return gate(GateKind::Majority, Ins, GatePolarity::Both);
}

void CnfEncoder::mkFullAdder(SatLit A, SatLit B, SatLit CarryIn, SatLit &Sum,
                             SatLit &CarryOut) {
  const SatLit Ins[] = {A, B, CarryIn};
  Sum = mkXor(Ins);
  CarryOut = mkMajority(A, B, CarryIn);
}

//===----------------------------------------------------------------------===//
// Assertions
//===----------------------------------------------------------------------===//

bool CnfEncoder::assertTrue(SatLit L) { return Solver.addClause(L); }

bool CnfEncoder::assertEquiv(SatLit A, SatLit B) {
  // Both clauses go in even if the first already made the formula
  // contradictory, so that the database says the same thing either way.
  bool First = Solver.addClause(~A, B);
  bool Second = Solver.addClause(A, ~B);
  return First && Second;
}

bool CnfEncoder::assertImplies(SatLit A, SatLit B) {
  return Solver.addClause(~A, B);
}

//===----------------------------------------------------------------------===//
// The gate table
//===----------------------------------------------------------------------===//

SatLit CnfEncoder::gate(GateKind Kind, llvm::ArrayRef<SatLit> Ins,
                        GatePolarity P) {
  uint64_t H = mixHash(0x51ed270bULL, static_cast<uint64_t>(Kind));
  for (SatLit L : Ins)
    H = mixHash(H, L.index());

  auto &Bucket = GateTable[H];
  for (uint32_t Index : Bucket) {
    const Gate &G = Gates[Index];
    if (G.Kind != Kind || G.NumOperands != Ins.size())
      continue;
    if (!std::equal(Ins.begin(), Ins.end(), operandsOf(G).begin()))
      continue;
    emit(Index, P);
    return Gates[Index].Out;
  }

  Gate G;
  G.Kind = Kind;
  G.Emitted = 0;
  G.FirstOperand = static_cast<uint32_t>(OperandPool.size());
  G.NumOperands = static_cast<uint32_t>(Ins.size());
  G.Out = freshLit();
  OperandPool.insert(OperandPool.end(), Ins.begin(), Ins.end());

  auto Index = static_cast<uint32_t>(Gates.size());
  Gates.push_back(G);
  Bucket.push_back(Index);

  emit(Index, P);
  return G.Out;
}

void CnfEncoder::emit(uint32_t Index, GatePolarity P) {
  uint8_t Wanted = 0;
  if (P != GatePolarity::Negative)
    Wanted |= kPositiveEmitted;
  if (P != GatePolarity::Positive)
    Wanted |= kNegativeEmitted;

  const uint8_t Missing = Wanted & ~Gates[Index].Emitted;
  if (Missing == 0)
    return;

  switch (Gates[Index].Kind) {
  case GateKind::And:
    Gates[Index].Emitted |= Missing;
    if (Missing & kPositiveEmitted)
      emitAnd(Gates[Index], /*Positive=*/true);
    if (Missing & kNegativeEmitted)
      emitAnd(Gates[Index], /*Positive=*/false);
    return;

  case GateKind::Xor:
    // A parity constraint has no half to leave out: every clause of it is
    // needed to pin the result in one direction or the other.
    Gates[Index].Emitted = kPositiveEmitted | kNegativeEmitted;
    emitXor(Gates[Index]);
    return;

  case GateKind::Ite:
    Gates[Index].Emitted |= Missing;
    if (Missing & kPositiveEmitted)
      emitIte(Gates[Index], /*Positive=*/true);
    if (Missing & kNegativeEmitted)
      emitIte(Gates[Index], /*Positive=*/false);
    return;

  case GateKind::Majority:
    Gates[Index].Emitted |= Missing;
    if (Missing & kPositiveEmitted)
      emitMajority(Gates[Index], /*Positive=*/true);
    if (Missing & kNegativeEmitted)
      emitMajority(Gates[Index], /*Positive=*/false);
    return;
  }
}

void CnfEncoder::emitAnd(const Gate &G, bool Positive) {
  llvm::ArrayRef<SatLit> Ins = operandsOf(G);

  if (Positive) {
    // The gate implies each operand.
    for (SatLit In : Ins)
      Solver.addClause(~G.Out, In);
    return;
  }

  // All the operands together imply the gate.
  llvm::SmallVector<SatLit, 8> Clause;
  Clause.reserve(Ins.size() + 1);
  Clause.push_back(G.Out);
  for (SatLit In : Ins)
    Clause.push_back(~In);
  Solver.addClause(Clause);
}

void CnfEncoder::emitXor(const Gate &G) {
  llvm::ArrayRef<SatLit> Ins = operandsOf(G);
  const uint32_t N = G.NumOperands;

  // A parity over the operands and the result is defined by ruling out every
  // assignment that breaks it, one clause each.  That is exponential in the
  // number of operands, which is why the builder keeps this to three.
  llvm::SmallVector<SatLit, 8> Clause;
  for (uint32_t Assignment = 0, End = 1u << N; Assignment < End;
       ++Assignment) {
    Clause.clear();
    bool Parity = false;
    for (uint32_t I = 0; I < N; ++I) {
      bool Bit = ((Assignment >> I) & 1) != 0;
      Parity ^= Bit;
      Clause.push_back(Ins[I].withPolarity(!Bit));
    }
    Clause.push_back(G.Out.withPolarity(Parity));
    Solver.addClause(Clause);
  }
}

void CnfEncoder::emitIte(const Gate &G, bool Positive) {
  llvm::ArrayRef<SatLit> Ins = operandsOf(G);
  SatLit C = Ins[0];
  SatLit T = Ins[1];
  SatLit E = Ins[2];

  if (Positive) {
    Solver.addClause(~G.Out, ~C, T);
    Solver.addClause(~G.Out, C, E);
    // Implied by the two above, and worth stating anyway: it lets the search
    // decide the gate from the arms alone, without waiting for the condition.
    Solver.addClause(~G.Out, T, E);
    return;
  }

  Solver.addClause(G.Out, ~C, ~T);
  Solver.addClause(G.Out, C, ~E);
  Solver.addClause(G.Out, ~T, ~E);
}

void CnfEncoder::emitMajority(const Gate &G, bool Positive) {
  llvm::ArrayRef<SatLit> Ins = operandsOf(G);
  SatLit A = Ins[0];
  SatLit B = Ins[1];
  SatLit C = Ins[2];

  if (Positive) {
    // A majority implies that no two of the three are false.
    Solver.addClause(~G.Out, A, B);
    Solver.addClause(~G.Out, A, C);
    Solver.addClause(~G.Out, B, C);
    return;
  }

  // Any two true operands imply the majority.
  Solver.addClause(G.Out, ~A, ~B);
  Solver.addClause(G.Out, ~A, ~C);
  Solver.addClause(G.Out, ~B, ~C);
}

} // namespace neverd::solver
