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

#include "neverd/ir/high/MedToHigh.h"

#include <string>
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
    HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
    // Applied here rather than reached through the pipeline, because it is not
    // part of the standard one yet.
    simplifyExprSemantics(High.Body);
    return High;
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

  std::string Expr =
      returnedExpr(B.finish(NdOp::INT_LEFT, {Sum, FunctionBuilder::constant(3)}));
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

} // namespace
