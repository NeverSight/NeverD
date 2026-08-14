//===- CnfEncoderTests.cpp - Gate definitions and folding -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks the encoder against the truth tables of the gates it claims to
/// build, by forcing every combination of its inputs and reading the result
/// back out of a model.  Also pins the two behaviours the bit-blaster's size
/// depends on — that a constant operand folds a gate away, and that a gate
/// built twice is one gate — and the rule that makes a one-sided definition
/// safe, namely that asking later for the other side adds it.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatSolver.h"
#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <functional>

using namespace neverd::solver;

namespace {

/// Force \p A and \p B to fixed values and report what \p Gate is worth.
bool gateValue(SatSolver &S, SatLit A, bool ValueA, SatLit B, bool ValueB,
               SatLit Gate) {
  const SatLit Assumptions[] = {A.withPolarity(ValueA), B.withPolarity(ValueB)};
  EXPECT_EQ(S.solve(Assumptions), SatResult::Sat);
  return S.modelValue(Gate) == SatValue::True;
}

void checkTwoInputGate(const char *Name,
                       const std::function<SatLit(CnfEncoder &, SatLit,
                                                  SatLit)> &Build,
                       const std::function<bool(bool, bool)> &Expected) {
  SatSolver S;
  CnfEncoder E(S);
  SatLit A = E.freshLit();
  SatLit B = E.freshLit();
  SatLit G = Build(E, A, B);

  for (unsigned Combination = 0; Combination < 4; ++Combination) {
    bool ValueA = (Combination & 1) != 0;
    bool ValueB = (Combination & 2) != 0;
    EXPECT_EQ(gateValue(S, A, ValueA, B, ValueB, G), Expected(ValueA, ValueB))
        << Name << " at " << ValueA << "," << ValueB;
  }
}

TEST(CnfEncoder, TwoInputGatesMatchTheirTruthTables) {
  checkTwoInputGate(
      "and", [](CnfEncoder &E, SatLit A, SatLit B) { return E.mkAnd(A, B); },
      [](bool A, bool B) { return A && B; });
  checkTwoInputGate(
      "or", [](CnfEncoder &E, SatLit A, SatLit B) { return E.mkOr(A, B); },
      [](bool A, bool B) { return A || B; });
  checkTwoInputGate(
      "xor", [](CnfEncoder &E, SatLit A, SatLit B) { return E.mkXor(A, B); },
      [](bool A, bool B) { return A != B; });
  checkTwoInputGate(
      "equiv",
      [](CnfEncoder &E, SatLit A, SatLit B) { return E.mkEquiv(A, B); },
      [](bool A, bool B) { return A == B; });
  checkTwoInputGate(
      "nand",
      [](CnfEncoder &E, SatLit A, SatLit B) { return ~E.mkAnd(A, B); },
      [](bool A, bool B) { return !(A && B); });
}

TEST(CnfEncoder, SelectionAndMajorityMatchTheirTruthTables) {
  SatSolver S;
  CnfEncoder E(S);
  SatLit A = E.freshLit();
  SatLit B = E.freshLit();
  SatLit C = E.freshLit();

  SatLit Selected = E.mkIte(A, B, C);
  SatLit Majority = E.mkMajority(A, B, C);
  SatLit Sum;
  SatLit Carry;
  E.mkFullAdder(A, B, C, Sum, Carry);

  for (unsigned Combination = 0; Combination < 8; ++Combination) {
    bool ValueA = (Combination & 1) != 0;
    bool ValueB = (Combination & 2) != 0;
    bool ValueC = (Combination & 4) != 0;

    const SatLit Assumptions[] = {A.withPolarity(ValueA),
                                  B.withPolarity(ValueB),
                                  C.withPolarity(ValueC)};
    ASSERT_EQ(S.solve(Assumptions), SatResult::Sat);

    unsigned Total = unsigned(ValueA) + unsigned(ValueB) + unsigned(ValueC);
    EXPECT_EQ(S.modelValue(Selected) == SatValue::True,
              ValueA ? ValueB : ValueC);
    EXPECT_EQ(S.modelValue(Majority) == SatValue::True, Total >= 2);
    EXPECT_EQ(S.modelValue(Sum) == SatValue::True, (Total & 1) != 0);
    EXPECT_EQ(S.modelValue(Carry) == SatValue::True, Total >= 2);
  }
}

TEST(CnfEncoder, ConstantOperandsFoldInsteadOfBuildingAGate) {
  SatSolver S;
  CnfEncoder E(S);
  SatLit A = E.freshLit();

  EXPECT_EQ(E.mkAnd(E.trueLit(), A), A);
  EXPECT_EQ(E.mkAnd(E.falseLit(), A), E.falseLit());
  EXPECT_EQ(E.mkOr(E.trueLit(), A), E.trueLit());
  EXPECT_EQ(E.mkOr(E.falseLit(), A), A);
  EXPECT_EQ(E.mkXor(E.falseLit(), A), A);
  EXPECT_EQ(E.mkXor(E.trueLit(), A), ~A);
  EXPECT_EQ(E.mkIte(E.trueLit(), A, ~A), A);
  EXPECT_EQ(E.mkIte(E.falseLit(), A, ~A), ~A);
  EXPECT_EQ(E.mkMajority(E.falseLit(), A, ~A), E.falseLit());

  // The algebraic identities matter as much as the constant ones, because
  // bit-blasting produces both in quantity.
  EXPECT_EQ(E.mkAnd(A, A), A);
  EXPECT_EQ(E.mkAnd(A, ~A), E.falseLit());
  EXPECT_EQ(E.mkOr(A, ~A), E.trueLit());
  EXPECT_EQ(E.mkXor(A, A), E.falseLit());
  EXPECT_EQ(E.mkXor(A, ~A), E.trueLit());

  EXPECT_EQ(E.numGates(), 0u);
}

TEST(CnfEncoder, EqualGatesAreOneGate) {
  SatSolver S;
  CnfEncoder E(S);
  SatLit A = E.freshLit();
  SatLit B = E.freshLit();

  SatLit First = E.mkAnd(A, B);
  EXPECT_EQ(E.numGates(), 1u);

  // Operand order is not part of a conjunction's identity.
  EXPECT_EQ(E.mkAnd(B, A), First);
  EXPECT_EQ(E.numGates(), 1u);

  // A disjunction is the same gate seen through complemented operands, which
  // is what lets a formula written either way share one variable.
  EXPECT_EQ(E.mkOr(~A, ~B), ~First);
  EXPECT_EQ(E.numGates(), 1u);

  // Complements on an exclusive or move to its result, so these are one gate.
  SatLit Parity = E.mkXor(A, B);
  EXPECT_EQ(E.mkXor(~A, B), ~Parity);
  EXPECT_EQ(E.mkXor(~A, ~B), Parity);
  EXPECT_EQ(E.numGates(), 2u);
}

TEST(CnfEncoder, WideExclusiveOrStaysLinear) {
  SatSolver S;
  CnfEncoder E(S);

  llvm::SmallVector<SatLit, 8> Inputs;
  for (unsigned I = 0; I < 8; ++I)
    Inputs.push_back(E.freshLit());

  SatLit Parity = E.mkXor(Inputs);

  // A single node would need one clause per assignment of its inputs, so a
  // wide parity has to become a chain instead.
  EXPECT_LT(S.numClauses(), 64u);

  for (unsigned Combination = 0; Combination < 256; ++Combination) {
    llvm::SmallVector<SatLit, 8> Assumptions;
    unsigned Ones = 0;
    for (unsigned I = 0; I < 8; ++I) {
      bool Bit = ((Combination >> I) & 1) != 0;
      Ones += unsigned(Bit);
      Assumptions.push_back(Inputs[I].withPolarity(Bit));
    }
    ASSERT_EQ(S.solve(Assumptions), SatResult::Sat);
    EXPECT_EQ(S.modelValue(Parity) == SatValue::True, (Ones & 1) != 0);
  }
}

TEST(CnfEncoder, AOneSidedDefinitionIsCompletedWhenTheOtherSideIsAskedFor) {
  SatSolver S;
  CnfEncoder E(S);
  SatLit A = E.freshLit();
  SatLit B = E.freshLit();

  SatLit G = E.mkAnd(A, B, GatePolarity::Positive);

  // With only the half that makes forcing the gate true meaningful, the gate
  // does imply its operands...
  const SatLit Forced[] = {G};
  ASSERT_EQ(S.solve(Forced), SatResult::Sat);
  EXPECT_EQ(S.modelValue(A), SatValue::True);
  EXPECT_EQ(S.modelValue(B), SatValue::True);

  // ...but the operands do not yet imply the gate.
  const SatLit Loose[] = {A, B, ~G};
  EXPECT_EQ(S.solve(Loose), SatResult::Sat);

  // Asking for the same gate with both halves completes the definition rather
  // than building a second gate.
  EXPECT_EQ(E.mkAnd(A, B, GatePolarity::Both), G);
  EXPECT_EQ(E.numGates(), 1u);
  EXPECT_EQ(S.solve(Loose), SatResult::Unsat);
}

TEST(CnfEncoder, TheConstantLiteralBehavesLikeAnyOther) {
  SatSolver S;
  CnfEncoder E(S);

  EXPECT_TRUE(E.isConstant(E.trueLit()));
  EXPECT_TRUE(E.isConstant(E.falseLit()));
  EXPECT_EQ(E.constant(true), E.trueLit());
  EXPECT_EQ(E.constant(false), E.falseLit());
  EXPECT_FALSE(E.isConstant(E.freshLit()));

  ASSERT_EQ(S.solve(), SatResult::Sat);
  EXPECT_EQ(S.modelValue(E.trueLit()), SatValue::True);
  EXPECT_EQ(S.modelValue(E.falseLit()), SatValue::False);

  // Asserting the false literal is how an encoder reports that what it was
  // asked to build cannot hold.
  EXPECT_FALSE(E.assertTrue(E.falseLit()));
  EXPECT_EQ(S.solve(), SatResult::Unsat);
}

} // namespace
