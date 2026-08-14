//===- HighSymSimplifyTests.cpp - Semantic simplification of HighIR -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pins what the symbolic engine is allowed to do to a decompiled function,
/// from the other end: a MedIR function goes in and the C-shaped expression
/// comes out.
///
/// Two things are being tested at once, and both matter.  That the engine
/// reaches an expression at all — it only does once copy propagation has
/// folded the assignments into one tree — and that the translation across is
/// faithful in both directions, because a mistake there would not fail
/// loudly.  It would emit a smaller function that computes something else.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/ir/high/MedToHigh.h"

#include <string>
#include <thread>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t kEntry = 0x400000;
constexpr uint16_t kWordBytes = 4;

/// Builds a single-block function out of a sequence of operations on
/// temporaries, returning whatever the last one defined.
class FunctionBuilder {
public:
  FunctionBuilder() {
    Func.Entry = kEntry;
    Func.Name = "obfuscated";
    Func.ReturnType = NdType::makeInt(kWordBytes, false);
    Block.Id = 0;
    Block.StartAddr = kEntry;
  }

  /// A parameter, passed in a register so the converter names it as one.
  MedVar param(int Index, uint64_t RegOff) {
    MedVar V;
    V.Kind = MedVar::Param;
    V.TheArch = Arch::X64;
    V.Id = 100 + Index;
    V.SSAVer = 1;
    V.Size = kWordBytes;
    V.RegOff = RegOff;
    Func.Params.push_back(V);
    return V;
  }

  static MedVar constant(uint64_t Value) {
    return MedVar::makeConst(Value, kWordBytes);
  }

  MedVar emit(NdOp Op, std::vector<MedVar> Inputs) {
    MedVar Out;
    Out.Kind = MedVar::Temp;
    Out.TheArch = Arch::X64;
    Out.Id = NextTemp++;
    Out.SSAVer = 1;
    Out.Size = kWordBytes;
    return append(Op, std::move(Inputs), Out);
  }

  /// Emit the last operation into the return register and close the function.
  /// Return-value recovery looks for the final definition of that register
  /// rather than at what the RETURN names, so a temporary would leave the
  /// function returning nothing.
  HighFunc finish(NdOp Op, std::vector<MedVar> Inputs) {
    MedVar Out;
    Out.Kind = MedVar::Reg;
    Out.TheArch = Arch::X64;
    Out.Id = 900;
    Out.SSAVer = 1;
    Out.Size = kWordBytes;
    Out.RegOff = 0; // rax
    append(Op, std::move(Inputs), Out);

    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = NextAddr;
    Block.Ops.push_back(std::move(Return));
    Block.EndAddr = NextAddr + 4;
    Func.Blocks.push_back(std::move(Block));
    // convert() runs the production HighIR cleanup, including semantic
    // simplification.  Do not invoke the pass a second time here: these tests
    // must fail if the default decompilation path ever loses that integration.
    return MedToHighConverter().convert(Func, Arch::X64);
  }

private:
  MedVar append(NdOp Op, std::vector<MedVar> Inputs, MedVar Out) {
    MedOp Instr;
    Instr.Opcode = Op;
    Instr.Output = Out;
    Instr.Addr = NextAddr;
    NextAddr += 4;
    for (const MedVar &In : Inputs)
      Instr.addInput(In);
    Block.Ops.push_back(std::move(Instr));
    return Out;
  }

  MedFunc Func;
  MedBlock Block;
  int NextTemp = 1;
  va_t NextAddr = kEntry;
};

/// The returned expression, rendered the way HighIR prints it.
std::string returnedExpr(const HighFunc &Func) {
  for (const HighStmt &S : Func.Body)
    if (S.Kind == StmtKind::Return && S.RetVal)
      return S.RetVal->str();
  return "<no return>";
}

TEST(HighSymSimplify, RecoversAdditionFromItsBitwiseRewriting) {
  // `x + y`, written the way an obfuscator writes it.
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38); // rdi
  MedVar Y = B.param(1, 0x30); // rsi
  MedVar Xor = B.emit(NdOp::INT_XOR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Scaled = B.emit(NdOp::INT_MULT, {And, FunctionBuilder::constant(2)});

  std::string Expr = returnedExpr(B.finish(NdOp::INT_ADD, {Xor, Scaled}));
  // Whichever way the operands come out ordered, neither the exclusive-or nor
  // the doubled conjunction may survive.
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;
  EXPECT_NE(Expr.find('+'), std::string::npos) << Expr;
}

TEST(HighSymSimplify, ReachesTheSmallestNontrivialIdentity) {
  // Four DAG nodes: x, ~x, 1 and the addition.  A size gate above four would
  // silently leave this supported identity out of the production HighIR path.
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar NotX = B.emit(NdOp::INT_NOT, {X});

  std::string Expr = returnedExpr(
      B.finish(NdOp::INT_ADD, {NotX, FunctionBuilder::constant(1)}));
  EXPECT_EQ(Expr.find('~'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('+'), std::string::npos) << Expr;
  EXPECT_NE(Expr.find('-'), std::string::npos) << Expr;
}

TEST(HighSymSimplify, RecoversExclusiveOrFromItsArithmeticRewriting) {
  // `(x | y) - (x & y)`.
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar Or = B.emit(NdOp::INT_OR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});

  std::string Expr = returnedExpr(B.finish(NdOp::INT_SUB, {Or, And}));
  EXPECT_NE(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('|'), std::string::npos) << Expr;
}

TEST(HighSymSimplify, CollapsesAnExpressionThatIsSecretlyConstant) {
  // `(x & y) + (x | y) - x - y` is zero for every input.
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Or = B.emit(NdOp::INT_OR, {X, Y});
  MedVar Sum = B.emit(NdOp::INT_ADD, {And, Or});
  MedVar LessX = B.emit(NdOp::INT_SUB, {Sum, X});

  EXPECT_EQ(returnedExpr(B.finish(NdOp::INT_SUB, {LessX, Y})), "0");
}

TEST(HighSymSimplify, LeavesAnOrdinaryExpressionAlone) {
  // Nothing here is hiding anything, and the pass must not churn it.
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar Sum = B.emit(NdOp::INT_ADD, {X, Y});

  std::string Expr = returnedExpr(
      B.finish(NdOp::INT_LEFT, {Sum, FunctionBuilder::constant(3)}));
  EXPECT_NE(Expr.find('+'), std::string::npos) << Expr;
}

TEST(HighSymSimplify, KeepsWhatItCannotSeeInsideOf) {
  // A memory read sits inside the sum.  The engine has nothing to say about
  // one, so it has to stand an opaque input in front of it, measure what
  // surrounds it, and put the read back exactly as it was.  Dropping it would
  // be the worst thing this pass could do, and the quietest.
  FunctionBuilder B;
  MedVar Addr = B.param(0, 0x38);
  MedVar X = B.param(1, 0x30);
  MedVar Y = B.param(2, 0x10);
  MedVar Loaded = B.emit(NdOp::LOAD, {Addr});
  MedVar Xor = B.emit(NdOp::INT_XOR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Scaled = B.emit(NdOp::INT_MULT, {And, FunctionBuilder::constant(2)});
  MedVar Sum = B.emit(NdOp::INT_ADD, {Xor, Scaled});

  std::string Expr = returnedExpr(B.finish(NdOp::INT_ADD, {Sum, Loaded}));
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;
  EXPECT_NE(Expr.find('*'), std::string::npos) << Expr;
}

TEST(HighSymSimplify, ReachesAnIdentityBelowSixtyFourExpressionLevels) {
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar Xor = B.emit(NdOp::INT_XOR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Scaled = B.emit(NdOp::INT_MULT, {And, FunctionBuilder::constant(2)});
  MedVar Wrapped = B.emit(NdOp::INT_ADD, {Xor, Scaled});

  // A hard recursion limit must not decide which parts of a real expression
  // the semantic pass can see.  The symbolic engine itself walks iteratively,
  // so this translator should be able to feed it a deep HighIR tree too.
  for (unsigned I = 0; I < 96; ++I)
    Wrapped =
        B.emit(NdOp::INT_ADD, {Wrapped, FunctionBuilder::constant(I + 1)});

  std::string Expr = returnedExpr(
      B.finish(NdOp::INT_ADD, {Wrapped, FunctionBuilder::constant(97)}));
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;
}

//===----------------------------------------------------------------------===//
// Words wider than the machine's
//===----------------------------------------------------------------------===//

/// A temporary of \p Bytes bytes.  Building HighIR directly is what lets these
/// reach a width no x86 register has.
MedVar tempOfSize(int Id, uint16_t Bytes) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.TheArch = Arch::X64;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = Bytes;
  return V;
}

/// \p E after the production semantic pass has had it.
ExprPtr simplified(ExprPtr E) {
  std::vector<HighStmt> Body;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(E);
  Body.push_back(std::move(Return));
  simplifyExprSemantics(Body);
  return Body.front().RetVal;
}

// The carry-save identity again, at a width no machine register has.  Nothing
// in the engine cares: its literals are arbitrary precision and a 128-bit word
// is measured exactly as a 32-bit one.  The only thing that ever left a
// `__int128` obfuscation standing was this bridge declining to carry one
// across.
TEST(HighSymSimplify, RecoversAdditionAtAWidthWiderThanAMachineWord) {
  constexpr uint16_t kWideBytes = 16;
  ExprPtr X = HighExpr::makeVar(tempOfSize(1, kWideBytes));
  ExprPtr Y = HighExpr::makeVar(tempOfSize(2, kWideBytes));
  ExprPtr Carry = HighExpr::makeBinop(NdOp::INT_MULT,
                                      HighExpr::makeBinop(NdOp::INT_AND, X, Y),
                                      HighExpr::makeConst(2, kWideBytes));
  ExprPtr Result = simplified(HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeBinop(NdOp::INT_XOR, X, Y), Carry));

  const std::string Expr = Result->str();
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;
  EXPECT_NE(Expr.find('+'), std::string::npos) << Expr;
  // What comes back has to be the width that went in, or the backend would
  // print a 128-bit value through a narrower type.
  ASSERT_TRUE(Result->Type);
  EXPECT_EQ(Result->Type->Size, kWideBytes);
}

// The value has to survive the round trip as well as the width.  This is zero
// at every input, and only a measurement carried out in arbitrary precision
// can say so at 256 bits.
TEST(HighSymSimplify, CollapsesAWideExpressionThatIsSecretlyConstant) {
  constexpr uint16_t kWideBytes = 32;
  ExprPtr X = HighExpr::makeVar(tempOfSize(1, kWideBytes));
  ExprPtr Y = HighExpr::makeVar(tempOfSize(2, kWideBytes));
  ExprPtr Sum = HighExpr::makeBinop(NdOp::INT_ADD,
                                    HighExpr::makeBinop(NdOp::INT_AND, X, Y),
                                    HighExpr::makeBinop(NdOp::INT_OR, X, Y));
  ExprPtr Result = simplified(HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeBinop(NdOp::INT_SUB, Sum, X), Y));

  EXPECT_EQ(Result->str(), "0");
}

/// The carry-save spelling of `x + y`, less both of its inputs again, so what
/// the engine is left holding is whatever constant tail was folded in.
ExprPtr wideConstantTail(const ExprPtr &X, const ExprPtr &Y, ExprPtr Tail,
                         uint16_t Bytes) {
  ExprPtr Carry = HighExpr::makeBinop(NdOp::INT_MULT,
                                      HighExpr::makeBinop(NdOp::INT_AND, X, Y),
                                      HighExpr::makeConst(2, Bytes));
  ExprPtr Sum = HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeBinop(NdOp::INT_XOR, X, Y), Carry);
  ExprPtr Whole =
      HighExpr::makeBinop(NdOp::INT_ADD, std::move(Sum), std::move(Tail));
  return HighExpr::makeBinop(NdOp::INT_SUB,
                             HighExpr::makeBinop(NdOp::INT_SUB, Whole, X), Y);
}

// Measuring derives values HighIR has no room to write down: a literal is kept
// in sixty-four bits whatever size stands beside it, and a 128-bit all-ones
// needs all of them.  Keeping the low half would be a smaller expression
// computing something else, so the value is spelled as the negation it is
// instead -- which is exact, and is what a reader wanted to see anyway.
TEST(HighSymSimplify, SpellsAWideConstantTooLargeToStoreAsANegation) {
  // `(x ^ y) + 2 * (x & y) + (-1) - x - y` is -1 for every input.
  constexpr uint16_t kWideBytes = 16;
  ExprPtr X = HighExpr::makeVar(tempOfSize(1, kWideBytes));
  ExprPtr Y = HighExpr::makeVar(tempOfSize(2, kWideBytes));
  ExprPtr MinusOne =
      HighExpr::makeUnary(NdOp::INT_NEG2, HighExpr::makeConst(1, kWideBytes));
  ExprPtr Result =
      simplified(wideConstantTail(X, Y, std::move(MinusOne), kWideBytes));

  EXPECT_EQ(Result->str(), "-1");
  ASSERT_TRUE(Result->Type);
  EXPECT_EQ(Result->Type->Size, kWideBytes);
}

// A value with no exact spelling at all is the case that has to be refused.
// `1 << 100` is not a literal HighIR can hold, and neither its negation nor its
// complement is one either, so there is nothing to write down -- and writing
// its low sixty-four bits would be a shorter expression computing something
// else, which is the one thing this pass may never hand back.  What comes out
// is therefore the very object that went in.
TEST(HighSymSimplify, DeclinesARewriteWhoseConstantHasNoSpelling) {
  constexpr uint16_t kWideBytes = 16;
  ExprPtr X = HighExpr::makeVar(tempOfSize(1, kWideBytes));
  ExprPtr Y = HighExpr::makeVar(tempOfSize(2, kWideBytes));
  ExprPtr Shifted =
      HighExpr::makeBinop(NdOp::INT_LEFT, HighExpr::makeConst(1, kWideBytes),
                          HighExpr::makeConst(100, kWideBytes));
  ExprPtr Before = wideConstantTail(X, Y, std::move(Shifted), kWideBytes);

  EXPECT_EQ(simplified(Before).get(), Before.get()) << Before->str();
}

TEST(HighCDeadStoreAnalysis, WalksDeepExpressionGraphsOnAWorkerStack) {
  MedVar Input;
  Input.Kind = MedVar::Param;
  Input.TheArch = Arch::X64;
  Input.Id = 1;
  Input.SSAVer = 1;
  Input.Size = kWordBytes;

  ExprPtr Deep = HighExpr::makeVar(Input);
  for (unsigned I = 0; I < 8192; ++I)
    Deep = HighExpr::makeBinop(NdOp::INT_ADD, std::move(Deep),
                               HighExpr::makeConst(I, kWordBytes));

  HighFunc Func;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Deep);
  Func.Body.push_back(std::move(Return));

  HighCAnalysisState State;
  std::thread Worker([&] {
    analyzeDeadStores(
        State, Func, [](const MedVar &V) { return V.display(); },
        [](const HighExpr &) { return std::string("expression"); });
  });
  Worker.join();
  EXPECT_TRUE(State.DeadStmts.empty());

  // Shared-pointer destruction follows the same chain recursively.  Dismantle
  // this synthetic stress tree iteratively so the test measures the analysis,
  // not the standard library's control-block teardown.
  ExprPtr Current = std::move(Func.Body.front().RetVal);
  while (Current && Current->Kind == ExprKind::BinOp &&
         !Current->Operands.empty()) {
    ExprPtr Next = std::move(Current->Operands[0]);
    Current->Operands.clear();
    Current = std::move(Next);
  }
}

} // namespace
