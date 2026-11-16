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

#include "../../../lib/ir/high/pass/HighSemanticSimplifier.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/ir/high/HighIntegerProjection.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace neverd {
bool simplifyAllExprs(std::vector<HighStmt> &);
void zeroUndemandedUnknownBytes(HighFunc &, Arch);
void eliminateUnusedValues(HighFunc &);
} // namespace neverd

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

  void makeLarge() {
    NextTemp = 100000;
    for (size_t I = 0; I <= limits::kMaxStructurableMedOps; ++I)
      emit(NdOp::COPY, {constant(I)});
  }

  /// Emit the last operation into the return register and close the function.
  /// Return-value recovery looks for the final definition of that register
  /// rather than at what the RETURN names, so a temporary would leave the
  /// function returning nothing.
  HighFunc finish(NdOp Op, std::vector<MedVar> Inputs, bool Symbolic = true) {
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
    // Exercise the opt-in through the production converter, not a second
    // invocation of the pass after ordinary cleanup.
    MedToHighConverter Converter;
    if (Symbolic)
      Converter.setSymbolicSimplification(true);
    return Converter.convert(Func, Arch::X64);
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

TEST(HighSymSimplify, OrdinaryConversionKeepsOptionalSymbolicWorkDisabled) {
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar Xor = B.emit(NdOp::INT_XOR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Scaled = B.emit(NdOp::INT_MULT, {And, FunctionBuilder::constant(2)});
  const auto Expr =
      returnedExpr(B.finish(NdOp::INT_ADD, {Xor, Scaled}, /*Symbolic=*/false));
  EXPECT_NE(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_NE(Expr.find('&'), std::string::npos) << Expr;
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

TEST(HighSymSimplify, RecoversAdditionFromAConstantLeftShift) {
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar Xor = B.emit(NdOp::INT_XOR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Scaled = B.emit(NdOp::INT_LEFT, {And, FunctionBuilder::constant(1)});

  const std::string Expr = returnedExpr(B.finish(NdOp::INT_ADD, {Xor, Scaled}));
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find("<<"), std::string::npos) << Expr;
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

TEST(HighSymSimplify, FoldedScalarLiteralsRetainNumericProvenance) {
  using P = ConstantAddressProvenance;
  for (P FirstProvenance : {P::Scalar, P::Unknown}) {
    auto Color = HighExpr::makeBinop(
        NdOp::INT_OR,
        HighExpr::makeBinop(NdOp::INT_OR,
                            HighExpr::makeConst(0x3B0000, 8, FirstProvenance),
                            HighExpr::makeConst(0x5B00, 8, P::Scalar)),
        HighExpr::makeConst(0x1B, 8, P::Scalar));
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = std::move(Color);
    std::vector<HighStmt> Body{Return};
    simplifyExprSemantics(Body);
    ASSERT_EQ(Body[0].RetVal->Kind, ExprKind::Const);
    EXPECT_EQ(Body[0].RetVal->ConstVal, 0x3B5B1BU);
    EXPECT_EQ(Body[0].RetVal->ConstProvenance, FirstProvenance);
  }
}

TEST(HighSymSimplify, ExtractionIndexDoesNotEraseScalarLiteralProvenance) {
  using P = ConstantAddressProvenance;
  for (P FragmentProvenance : {P::Scalar, P::Unknown, P::DataAddress}) {
    SCOPED_TRACE(static_cast<int>(FragmentProvenance));
    auto Word = HighExpr::makeBinop(
        NdOp::INT_OR,
        HighExpr::makeBinop(NdOp::INT_OR,
                            HighExpr::makeConst(0xCAA8, 4, FragmentProvenance),
                            HighExpr::makeConst(0x30000, 4, P::Scalar)),
        HighExpr::makeConst(0, 4, P::Scalar));
    auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Word);
    Extended->Type = NdType::makeInt(8, false);
    auto Extract = HighExpr::makeBinop(NdOp::SUBBYTES, Extended,
                                       HighExpr::makeConst(0, 4, P::Unknown));
    Extract->Type = NdType::makeInt(4, false);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = std::move(Extract);
    std::vector<HighStmt> Body{Return};
    simplifyExprSemantics(Body);
    if (FragmentProvenance == P::DataAddress) {
      EXPECT_NE(Body[0].RetVal->Kind, ExprKind::Const);
      continue;
    }
    ASSERT_EQ(Body[0].RetVal->Kind, ExprKind::Const) << Body[0].RetVal->str();
    EXPECT_EQ(Body[0].RetVal->ConstVal, 0x3CAA8U);
    EXPECT_EQ(Body[0].RetVal->ConstProvenance,
              FragmentProvenance == P::Scalar ? P::Scalar : P::Unknown);
  }
}

TEST(HighSymSimplify, OpaqueSelectDoesNotTaintFoldedScalarOperand) {
  using P = ConstantAddressProvenance;
  for (P FragmentProvenance : {P::Scalar, P::Unknown, P::DataAddress}) {
    SCOPED_TRACE(static_cast<int>(FragmentProvenance));
    auto Word = HighExpr::makeBinop(
        NdOp::INT_OR,
        HighExpr::makeBinop(NdOp::INT_AND,
                            HighExpr::makeConst(0xCAA8, 4, FragmentProvenance),
                            HighExpr::makeConst(0xFFFF, 4, P::Scalar)),
        HighExpr::makeConst(0x30000, 4, P::Scalar));
    auto Select = std::make_shared<HighExpr>();
    Select->Kind = ExprKind::BinOp;
    Select->Op = NdOp::SELECT;
    Select->Type = NdType::makeInt(4, false);
    // Pure integer selections now translate. Keep this selection opaque at
    // an actual effect boundary while checking the independent scalar fold.
    auto Condition = HighExpr::makeCall("choose", 0x401000, {});
    Condition->Type = NdType::makeInt(4, false);
    Select->Operands = {Condition, HighExpr::makeConst(1, 4),
                        HighExpr::makeConst(0, 4)};
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeBinop(NdOp::INT_AND, Word, Select);
    std::vector<HighStmt> Body{Return};
    simplifyExprSemantics(Body);
    ExprPtr Folded;
    std::vector<ExprPtr> Work{Body[0].RetVal};
    while (!Work.empty()) {
      ExprPtr Current = Work.back();
      Work.pop_back();
      if (Current->Kind == ExprKind::Const && Current->ConstVal == 0x3CAA8)
        Folded = Current;
      Work.insert(Work.end(), Current->Operands.begin(),
                  Current->Operands.end());
    }
    if (FragmentProvenance == P::DataAddress) {
      EXPECT_FALSE(Folded);
      continue;
    }
    ASSERT_TRUE(Folded) << Body[0].RetVal->str();
    EXPECT_EQ(Folded->ConstProvenance,
              FragmentProvenance == P::Scalar ? P::Scalar : P::Unknown);
  }
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

TEST(HighSymSimplify, AddressConstantsRetainTheirOriginAcrossNumericRewrites) {
  using P = ConstantAddressProvenance;
  for (P Provenance :
       {P::AddressFragment, P::Address, P::DataAddress, P::CodeAddress}) {
    auto Address = HighExpr::makeConst(0x401234, 8, Provenance, 0x401000);
    MedVar Input;
    Input.Kind = MedVar::Param;
    Input.Size = 8;
    auto X = HighExpr::makeVar(Input);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    auto Sum = HighExpr::makeBinop(
        NdOp::INT_ADD, HighExpr::makeBinop(NdOp::INT_XOR, Address, X),
        HighExpr::makeBinop(NdOp::INT_MULT, HighExpr::makeConst(2, 8),
                            HighExpr::makeBinop(NdOp::INT_AND, Address, X)));
    Return.RetVal = HighExpr::makeBinop(NdOp::INT_SUB, Sum, X);
    std::vector<HighStmt> Body{Return};
    simplifyExprSemantics(Body);
    ASSERT_TRUE(Body[0].RetVal);
    EXPECT_TRUE(Body[0].RetVal->structuralEq(*Address));
    EXPECT_EQ(Body[0].RetVal->ConstProvenance, Provenance);
    EXPECT_EQ(Body[0].RetVal->AddressOwnerVA, 0x401000U);

    // Equal bits with a different meaning may not erase a relocatable input.
    auto Scalar = HighExpr::makeConst(0x401234, 8, P::Scalar);
    Return.RetVal = HighExpr::makeBinop(
        NdOp::INT_SUB, HighExpr::makeBinop(NdOp::INT_ADD, Address, X),
        HighExpr::makeBinop(NdOp::INT_ADD, Scalar, X));
    Body = {Return};
    simplifyExprSemantics(Body);
    std::vector<ExprPtr> Pending{Body[0].RetVal};
    bool RetainedAddress = false;
    while (!Pending.empty()) {
      auto E = Pending.back();
      Pending.pop_back();
      ASSERT_TRUE(E);
      RetainedAddress |= E->Kind == ExprKind::Const &&
                         E->ConstProvenance == Provenance &&
                         E->AddressOwnerVA == 0x401000;
      Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
    }
    EXPECT_TRUE(RetainedAddress);
  }
}

TEST(HighSymSimplify, SynthesizedNumericConstantsKeepScalarIdentity) {
  using P = ConstantAddressProvenance;
  constexpr uint64_t Base = 0x402000;
  MedVar Input;
  Input.Kind = MedVar::Param;
  Input.Size = 8;
  const auto X = HighExpr::makeVar(Input);
  for (bool ConstantResult : {false, true}) {
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    auto Numeric = HighExpr::makeConst(Base, 8, P::Scalar);
    // Reassociation synthesizes an address-shaped number used in arithmetic;
    // cancellation also exercises a wholly constant rewritten expression.
    auto Value = HighExpr::makeBinop(
        NdOp::INT_ADD, HighExpr::makeBinop(NdOp::INT_ADD, X, Numeric),
        HighExpr::makeConst(4, 8, P::Scalar));
    Return.RetVal =
        ConstantResult ? HighExpr::makeBinop(NdOp::INT_SUB, Value, X) : Value;
    std::vector<HighStmt> Body{Return};
    simplifyExprSemantics(Body);
    std::vector<ExprPtr> Pending{Body[0].RetVal};
    bool FoundCombined = false;
    while (!Pending.empty()) {
      auto E = Pending.back();
      Pending.pop_back();
      ASSERT_TRUE(E);
      if (E->Kind == ExprKind::Const) {
        FoundCombined |= E->ConstVal == Base + 4;
        EXPECT_EQ(E->ConstProvenance, P::Scalar);
        EXPECT_EQ(E->AddressOwnerVA, InvalidVA);
      }
      Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
    }
    EXPECT_TRUE(FoundCombined);
  }
}

TEST(HighSymSimplify, KeepsWhatItCannotSeeInsideOf) {
  // A memory read contributes to the sum. It must survive as a snapshot at
  // its original statement while the surrounding arithmetic is simplified.
  FunctionBuilder B;
  MedVar Addr = B.param(0, 0x38);
  MedVar X = B.param(1, 0x30);
  MedVar Y = B.param(2, 0x10);
  MedVar Loaded = B.emit(NdOp::LOAD, {Addr});
  MedVar Xor = B.emit(NdOp::INT_XOR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Scaled = B.emit(NdOp::INT_MULT, {And, FunctionBuilder::constant(2)});
  MedVar Sum = B.emit(NdOp::INT_ADD, {Xor, Scaled});

  HighFunc Func = B.finish(NdOp::INT_ADD, {Sum, Loaded});
  std::string Expr = returnedExpr(Func);
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;

  // The read happens once, no later than the return: kept apart as a
  // snapshot the return reads, or inside the return itself.
  const HighExpr *Read = nullptr;
  const HighStmt *ReadStmt = nullptr;
  const HighStmt *Return = nullptr;
  unsigned ReadCount = 0;
  for (const HighStmt &Stmt : Func.Body) {
    forEachExpr(Stmt, [&](const ExprPtr &Root) {
      std::vector<const HighExpr *> Work{Root.get()};
      while (!Work.empty()) {
        const HighExpr *E = Work.back();
        Work.pop_back();
        if (!E)
          continue;
        if (E->Kind == ExprKind::Load) {
          EXPECT_EQ(Return, nullptr) << "memory read moved after its use";
          Read = E;
          ReadStmt = &Stmt;
          ++ReadCount;
        }
        for (const ExprPtr &Operand : E->Operands)
          Work.push_back(Operand.get());
      }
    });
    if (Stmt.Kind == StmtKind::Return)
      Return = &Stmt;
  }
  ASSERT_EQ(ReadCount, 1U) << Expr;
  ASSERT_NE(Return, nullptr);
  ASSERT_EQ(Read->Operands.size(), 1U);
  ASSERT_EQ(Read->Operands[0]->Kind, ExprKind::Var);
  EXPECT_EQ(Read->Operands[0]->Var.Kind, MedVar::Param);
  EXPECT_EQ(Read->Operands[0]->Var.Id, 0);
  EXPECT_EQ(Read->Operands[0]->Var.RegOff, Addr.RegOff);
  EXPECT_EQ(Read->Operands[0]->Var.Size, Addr.Size);
  ASSERT_TRUE(Read->Type);
  EXPECT_EQ(Read->Type->Size, kWordBytes);
  if (ReadStmt == Return)
    return;
  ASSERT_EQ(ReadStmt->Kind, StmtKind::Assign);
  ASSERT_TRUE(ReadStmt->Dst);
  bool UsesSnapshot = false;
  std::vector<ExprPtr> Worklist{Return->RetVal};
  while (!Worklist.empty()) {
    ExprPtr Current = Worklist.back();
    Worklist.pop_back();
    ASSERT_TRUE(Current);
    UsesSnapshot |= Current->structuralEq(*ReadStmt->Dst);
    Worklist.insert(Worklist.end(), Current->Operands.begin(),
                    Current->Operands.end());
  }
  EXPECT_TRUE(UsesSnapshot) << Expr;
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

ExprPtr integerCast(ExprPtr Operand, uint16_t Bytes, bool Signed = false) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Cast;
  E->Type = E->CastTo = NdType::makeInt(Bytes, Signed);
  E->Operands = {std::move(Operand)};
  return E;
}

ExprPtr extendInteger(ExprPtr Operand, uint16_t Bytes, bool Signed = false) {
  auto E = HighExpr::makeUnary(Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT,
                               std::move(Operand));
  E->Type = NdType::makeInt(Bytes, false);
  return E;
}

ExprPtr integerSelect(const ExprPtr &Condition, const ExprPtr &Yes,
                      const ExprPtr &No) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::BinOp;
  E->Op = NdOp::SELECT;
  E->Type = Yes->Type;
  E->Operands = {Condition, Yes, No};
  return E;
}

ExprPtr carrySum(const ExprPtr &X, const ExprPtr &Y) {
  return HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeBinop(NdOp::INT_XOR, X, Y),
      HighExpr::makeBinop(NdOp::INT_MULT, HighExpr::makeConst(2, X->Type->Size),
                          HighExpr::makeBinop(NdOp::INT_AND, X, Y)));
}

// Evaluate exact bitvector operations independently of the symbolic bridge.
// The small-domain tests below include every input, including all carry and
// sign transitions; wide products retain their full APInt intermediate.
llvm::APInt
integerValue(const ExprPtr &E, uint64_t X, uint64_t Y = 0,
             const std::unordered_map<int, uint64_t> *Bindings = nullptr) {
  const unsigned Width = E->Type->Size * 8;
  if (E->Kind == ExprKind::Const)
    return llvm::APInt(Width, E->ConstVal, false, true);
  if (E->Kind == ExprKind::Var) {
    if (Bindings) {
      auto It = Bindings->find(E->Var.Id);
      if (It == Bindings->end()) {
        ADD_FAILURE() << "read before definition in test interpreter";
        return llvm::APInt(Width, 0);
      }
      return llvm::APInt(Width, It->second, false, true);
    }
    return llvm::APInt(Width, E->Var.Id == 1 ? X : Y, false, true);
  }
  llvm::APInt A = integerValue(E->Operands[0], X, Y, Bindings);
  if (E->Kind == ExprKind::BitCast) {
    EXPECT_EQ(A.getBitWidth(), Width);
    return A;
  }
  if (E->Kind == ExprKind::Cast)
    return E->Operands[0]->Type->IsSigned ? A.sextOrTrunc(Width)
                                          : A.zextOrTrunc(Width);
  if (E->Kind == ExprKind::UnaryOp) {
    switch (E->Op) {
    case NdOp::INT_ZEXT:
      return A.zextOrTrunc(Width);
    case NdOp::INT_SEXT:
      return A.sextOrTrunc(Width);
    case NdOp::INT_NOT:
    case NdOp::INT_NEGATE:
      return ~A;
    case NdOp::INT_NEG2:
      return -A;
    case NdOp::BOOL_NOT:
      return llvm::APInt(Width, A.isZero());
    case NdOp::POPCOUNT:
      return llvm::APInt(Width, A.popcount());
    default:
      ADD_FAILURE() << "unsupported unary test expression";
      return llvm::APInt(Width, 0);
    }
  }
  if (E->Op == NdOp::SUBBYTES)
    return A.extractBits(Width, E->Operands[1]->ConstVal * 8);
  if (E->Op == NdOp::SELECT)
    return integerValue(E->Operands[A.isZero() ? 2 : 1], X, Y, Bindings);
  llvm::APInt B = integerValue(E->Operands[1], X, Y, Bindings);
  switch (E->Op) {
  case NdOp::INT_EQUAL:
    return llvm::APInt(Width, A == B);
  case NdOp::INT_NOTEQUAL:
    return llvm::APInt(Width, A != B);
  case NdOp::INT_LESS:
    return llvm::APInt(Width, A.ult(B));
  case NdOp::INT_LESSEQUAL:
    return llvm::APInt(Width, A.ule(B));
  case NdOp::INT_SLESS:
    return llvm::APInt(Width, A.slt(B));
  case NdOp::INT_SLESSEQUAL:
    return llvm::APInt(Width, A.sle(B));
  case NdOp::BOOL_AND:
    return llvm::APInt(Width, !A.isZero() && !B.isZero());
  case NdOp::BOOL_OR:
    return llvm::APInt(Width, !A.isZero() || !B.isZero());
  case NdOp::BOOL_XOR:
    return llvm::APInt(Width, A.isZero() != B.isZero());
  default:
    break;
  }
  B = B.zextOrTrunc(Width);
  A = A.zextOrTrunc(Width);
  switch (E->Op) {
  case NdOp::INT_ADD:
    return A + B;
  case NdOp::INT_SUB:
    return A - B;
  case NdOp::INT_MULT:
    return A * B;
  case NdOp::INT_AND:
    return A & B;
  case NdOp::INT_OR:
    return A | B;
  case NdOp::INT_XOR:
    return A ^ B;
  case NdOp::INT_LEFT:
    return B.uge(Width) ? llvm::APInt(Width, 0) : A.shl(B.getZExtValue());
  case NdOp::INT_RIGHT:
    return B.uge(Width) ? llvm::APInt(Width, 0) : A.lshr(B.getZExtValue());
  case NdOp::INT_ASHR:
    return A.ashr(B.getLimitedValue(Width - 1));
  default:
    ADD_FAILURE() << "unsupported binary test expression";
    return llvm::APInt(Width, 0);
  }
}

TEST(HighSymSimplify, PreservesSupportedAndRefusedSubByteProjections) {
  uint64_t Random = 0x737562627974;
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (unsigned Bits : {1u, 3u, 7u})
      for (unsigned Low : {0u, 1u, unsigned(Bytes * 8 - Bits)}) {
        SCOPED_TRACE(std::to_string(Bytes) + ":" + std::to_string(Bits) + ":" +
                     std::to_string(Low));
        auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
        auto Y = HighExpr::makeVar(tempOfSize(2, Bytes));
        auto Before = HighExpr::makeBinop(
            NdOp::INT_AND,
            HighExpr::makeBinop(NdOp::INT_RIGHT, carrySum(X, Y),
                                HighExpr::makeConst(Low, Bytes)),
            HighExpr::makeConst((uint64_t(1) << Bits) - 1, Bytes));
        auto After = simplified(Before);
        // Some projections additionally require non-byte-width arithmetic;
        // unsupported interiors must retain the original semantics as well.
        for (unsigned I = 0; I < 512; ++I) {
          Random ^= Random << 13;
          Random ^= Random >> 7;
          Random ^= Random << 17;
          const auto A = Random;
          Random ^= Random << 13;
          Random ^= Random >> 7;
          Random ^= Random << 17;
          const auto B = Random;
          auto Expected = llvm::APInt(Bytes * 8, A) + llvm::APInt(Bytes * 8, B);
          Expected = Expected.lshr(Low) & ((uint64_t(1) << Bits) - 1);
          EXPECT_EQ(integerValue(Before, A, B), Expected);
          EXPECT_EQ(integerValue(After, A, B), Expected);
        }
      }
}

// capstone's printer assembles an operand word from three tables and reads a
// seven-bit field of it: `(uint32_t)(Bits >> 14) & 0x7F` with
// `Bits = op0 | (uint64_t)op1 << 32 | (uint64_t)op2 << 48`. Builders keep the
// source coordinates of a narrowed constant shift, a window at bit 14. The
// bridge must spell that window again, or the unobserved words stay live.
TEST(HighSymSimplify, BitWindowsOfAssembledWordsDropUnobservedWords) {
  auto Op0 = HighExpr::makeVar(tempOfSize(1, 8));
  auto Op1 = HighExpr::makeVar(tempOfSize(2, 2));
  auto Op2 = HighExpr::makeVar(tempOfSize(3, 1));
  auto Placed = [](const ExprPtr &V, unsigned Amount) {
    return HighExpr::makeBinop(NdOp::INT_LEFT, extendInteger(V, 8),
                               HighExpr::makeConst(Amount, 8));
  };
  auto Bits = HighExpr::makeBinop(
      NdOp::INT_OR, HighExpr::makeBinop(NdOp::INT_OR, Op0, Placed(Op1, 32)),
      Placed(Op2, 48));
  auto Before = HighExpr::makeBinop(
      NdOp::INT_AND,
      integerCast(HighExpr::makeBinop(NdOp::INT_RIGHT, Bits,
                                      HighExpr::makeConst(14, 8)),
                  4),
      HighExpr::makeConst(0x7F, 4));
  auto After = simplified(Before);
  std::function<bool(const ExprPtr &, int)> Reads = [&](const ExprPtr &E,
                                                        int Id) {
    if (E->Kind == ExprKind::Var && E->Var.Id == Id)
      return true;
    for (const auto &Operand : E->Operands)
      if (Operand && Reads(Operand, Id))
        return true;
    return false;
  };
  EXPECT_TRUE(Reads(After, 1));
  EXPECT_FALSE(Reads(After, 2));
  EXPECT_FALSE(Reads(After, 3));
  for (uint64_t A : {0x0ull, 0x1FC000ull, ~0ull, 0x123456789ABCDEFull})
    for (uint64_t B : {0x0ull, 0xFFFFull, 0x5A5Aull}) {
      const std::unordered_map<int, uint64_t> Values{
          {1, A}, {2, B}, {3, B & 0xFF}};
      EXPECT_EQ(integerValue(After, 0, 0, &Values).getZExtValue(),
                (A >> 14) & 0x7F);
      EXPECT_EQ(integerValue(Before, 0, 0, &Values).getZExtValue(),
                (A >> 14) & 0x7F);
    }
}

TEST(HighSymSimplify, SourceToggleReachesProvedAffineSynthesis) {
  for (uint16_t Bytes : {1, 2, 4, 8, 16, 32}) {
    SCOPED_TRACE(Bytes);
    auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
    auto Y = HighExpr::makeVar(tempOfSize(2, Bytes));
    auto C = [&](uint64_t V) { return HighExpr::makeConst(V, Bytes); };
    auto Shift = [&](ExprPtr V, unsigned N) {
      return HighExpr::makeBinop(NdOp::INT_RIGHT, std::move(V), C(N));
    };
    auto Expected = HighExpr::makeBinop(
        NdOp::INT_ADD,
        HighExpr::makeBinop(NdOp::INT_ADD,
                            HighExpr::makeBinop(NdOp::INT_MULT, C(182), X),
                            HighExpr::makeBinop(NdOp::INT_MULT, C(73), Y)),
        C(37));
    auto Before = HighExpr::makeBinop(
        NdOp::INT_ADD, Expected,
        HighExpr::makeBinop(NdOp::INT_SUB, Shift(X, 4), Shift(Shift(X, 2), 2)));
    auto After = simplified(Before);
    EXPECT_EQ(After->str().find(">>"), std::string::npos) << After->str();
    EXPECT_NE(After.get(), Before.get());
    for (uint64_t A : {0ull, 1ull, 127ull, 128ull, 255ull, ~0ull})
      for (uint64_t B : {0ull, 1ull, 128ull, 255ull, ~0ull}) {
        EXPECT_EQ(integerValue(Before, A, B), integerValue(Expected, A, B));
        EXPECT_EQ(integerValue(After, A, B), integerValue(Expected, A, B));
      }
  }
}

TEST(HighSymSimplify, SourceSynthesisPreservesNarrowOverflow) {
  auto X = HighExpr::makeVar(tempOfSize(1, 1));
  auto Y = HighExpr::makeVar(tempOfSize(2, 1));
  auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, X, Y);
  auto Before = HighExpr::makeBinop(NdOp::INT_ADD, extendInteger(Sum, 4),
                                    HighExpr::makeConst(37, 4));
  auto After = simplified(Before);
  for (unsigned A = 0; A < 256; ++A)
    for (unsigned B = 0; B < 256; ++B)
      EXPECT_EQ(integerValue(After, A, B).getZExtValue(),
                uint64_t(uint8_t(A + B)) + 37);
}

TEST(HighSymSimplify, SourceToggleReachesQuadraticSynthesisAndTypedRegions) {
  for (uint16_t Bytes : {1, 2, 4, 8, 16, 32}) {
    SCOPED_TRACE(Bytes);
    auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
    auto Y = HighExpr::makeVar(tempOfSize(2, Bytes));
    auto C = [&](uint64_t V) { return HighExpr::makeConst(V, Bytes); };
    auto Add = [](ExprPtr A, ExprPtr B) {
      return HighExpr::makeBinop(NdOp::INT_ADD, A, B);
    };
    auto Mul = [](ExprPtr A, ExprPtr B) {
      return HighExpr::makeBinop(NdOp::INT_MULT, A, B);
    };
    auto Shift = [&](ExprPtr V, unsigned N) {
      return HighExpr::makeBinop(NdOp::INT_RIGHT, V, C(N));
    };
    auto Expected = Add(Add(Mul(C(11), Mul(X, X)), Mul(C(73), Mul(X, Y))),
                        Add(Mul(C(182), Y), C(37)));
    auto Before = Add(Expected, HighExpr::makeBinop(NdOp::INT_SUB, Shift(X, 4),
                                                    Shift(Shift(X, 2), 2)));
    Before->Type = NdType::makeInt(Bytes, false);
    Expected->Type = Before->Type;
    // Exercise an enclosing conversion as well as the complete word. The
    // quadratic must keep its own wrapping width inside that conversion.
    for (bool Cast : {false, true}) {
      SCOPED_TRACE(Cast);
      auto Input = Cast ? integerCast(Before, Bytes == 4 ? 8 : 4) : Before;
      auto Want = Cast ? integerCast(Expected, Bytes == 4 ? 8 : 4) : Expected;
      auto After = simplified(Input);
      EXPECT_EQ(After->str().find(">>"), std::string::npos) << After->str();
      EXPECT_NE(After.get(), Input.get());
      for (uint64_t A : {0ull, 1ull, 127ull, 128ull, 255ull, ~0ull})
        for (uint64_t B : {0ull, 1ull, 128ull, 255ull, ~0ull}) {
          EXPECT_EQ(integerValue(Input, A, B), integerValue(Want, A, B));
          EXPECT_EQ(integerValue(After, A, B), integerValue(Want, A, B));
        }
    }
  }
}

TEST(HighSymSimplify, SourceSynthesisReachesInsideWidthConversions) {
  for (uint16_t FromBytes : {1, 2, 4, 8})
    for (uint16_t ToBytes : {1, 4, 8, 16}) {
      if (FromBytes == ToBytes)
        continue;
      for (bool Signed : {false, true}) {
        SCOPED_TRACE(FromBytes);
        SCOPED_TRACE(ToBytes);
        SCOPED_TRACE(Signed);
        auto X = HighExpr::makeVar(tempOfSize(1, FromBytes));
        auto Y = HighExpr::makeVar(tempOfSize(2, FromBytes));
        auto C = [&](uint64_t V) { return HighExpr::makeConst(V, FromBytes); };
        auto Shift = [&](ExprPtr V, unsigned N) {
          return HighExpr::makeBinop(NdOp::INT_RIGHT, std::move(V), C(N));
        };
        auto Sum = HighExpr::makeBinop(
            NdOp::INT_ADD,
            HighExpr::makeBinop(NdOp::INT_ADD,
                                HighExpr::makeBinop(NdOp::INT_MULT, C(182), X),
                                HighExpr::makeBinop(NdOp::INT_MULT, C(73), Y)),
            C(37));
        auto Hidden =
            HighExpr::makeBinop(NdOp::INT_ADD, Sum,
                                HighExpr::makeBinop(NdOp::INT_SUB, Shift(X, 4),
                                                    Shift(Shift(X, 2), 2)));
        Hidden->Type = NdType::makeInt(FromBytes, Signed);
        auto Before = integerCast(Hidden, ToBytes);
        const auto OriginalText = Before->str();
        auto After = simplified(Before);
        EXPECT_EQ(After->str().find(">>"), std::string::npos) << After->str();
        EXPECT_EQ(After->Type->Size, ToBytes);
        EXPECT_EQ(Before->str(), OriginalText);
        for (uint64_t A : {0ull, 1ull, 127ull, 128ull, 255ull, 0x80000000ull,
                           0xffffffffull, 0x8000000000000000ull, ~0ull})
          for (uint64_t B : {0ull, 1ull, 128ull, 255ull, ~0ull}) {
            const llvm::APInt Narrow =
                llvm::APInt(FromBytes * 8, A, false, true) * 182 +
                llvm::APInt(FromBytes * 8, B, false, true) * 73 + 37;
            const auto Expected = Signed ? Narrow.sextOrTrunc(ToBytes * 8)
                                         : Narrow.zextOrTrunc(ToBytes * 8);
            EXPECT_EQ(integerValue(Before, A, B), Expected);
            EXPECT_EQ(integerValue(After, A, B), Expected);
          }
      }
    }
}

TEST(HighSymSimplify, ReimportsWideConstantsWithoutLosingTheirUpperBits) {
  for (uint16_t Bytes : {16, 32})
    for (uint64_t Value : {0x8000000000000000ull, 0x8000000000000035ull, ~0ull})
      for (NdOp Op : {NdOp::COPY, NdOp::INT_NEG2, NdOp::INT_NOT}) {
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Value);
        SCOPED_TRACE(static_cast<unsigned>(Op));
        auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
        auto Y = HighExpr::makeVar(tempOfSize(2, Bytes));
        auto Tail = extendInteger(HighExpr::makeConst(Value, 8), Bytes);
        llvm::APInt Expected(Bytes * 8, Value);
        if (Op != NdOp::COPY) {
          Tail = HighExpr::makeUnary(Op, Tail);
          Expected = Op == NdOp::INT_NEG2 ? -Expected : ~Expected;
        }
        const auto Folded = simplified(wideConstantTail(X, Y, Tail, Bytes));
        EXPECT_EQ(integerValue(Folded, 0, 0), Expected);
        // A later enclosing region must still recognize the complete constant,
        // rather than treating a high-bit 64-bit payload as an opaque value.
        const auto Outer = HighExpr::makeBinop(
            NdOp::INT_ADD, X, HighExpr::makeBinop(NdOp::INT_SUB, Folded, Tail));
        const auto After = simplified(Outer);
        EXPECT_EQ(After->str(), X->str());
        for (uint64_t A : {0ull, 1ull, 0x8000000000000000ull, ~0ull})
          EXPECT_EQ(integerValue(After, A), llvm::APInt(Bytes * 8, A));
      }
}

// These checks use only ordinary cleanup, with no symbolic bridge or solver.
ExprPtr basicSimplified(const ExprPtr &E) {
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = E;
  std::vector<HighStmt> Body{Return};
  for (unsigned I = 0; I < 8 && simplifyAllExprs(Body); ++I) {
  }
  return Body.front().RetVal;
}

bool containsNode(const ExprPtr &E, const ExprPtr &Needle) {
  if (E == Needle)
    return true;
  for (const auto &Operand : E->Operands)
    if (Operand && containsNode(Operand, Needle))
      return true;
  return false;
}

TEST(HighExprSimplify, MaskedStatusBitsDoNotEvaluateUnknownIncomingBits) {
  auto Unknown = std::make_shared<HighExpr>();
  Unknown->Kind = ExprKind::Undef;
  Unknown->Type = NdType::makeInt(2, false);
  const auto Flags = HighExpr::makeBinop(
      NdOp::INT_OR,
      HighExpr::makeBinop(NdOp::INT_AND, Unknown,
                          HighExpr::makeConst(0xb8ff, 2)),
      HighExpr::makeBinop(NdOp::INT_OR, HighExpr::makeConst(0x4100, 2),
                          HighExpr::makeVar(tempOfSize(1, 2))));
  const auto SharedText = Flags->str();
  const auto After = basicSimplified(HighExpr::makeBinop(
      NdOp::INT_AND, extendInteger(Flags, 4), HighExpr::makeConst(0x4700, 4)));
  EXPECT_FALSE(containsNode(After, Unknown)) << After->str();
  EXPECT_EQ(Flags->str(), SharedText);
  EXPECT_TRUE(containsNode(Flags, Unknown));
  for (uint64_t Bits = 0; Bits < 65536; ++Bits)
    ASSERT_EQ(integerValue(After, Bits).getZExtValue(),
              (0x4100 | Bits) & 0x4700);

  const auto Observed = basicSimplified(HighExpr::makeBinop(
      NdOp::INT_AND, Flags, HighExpr::makeConst(0x4701, 2)));
  EXPECT_TRUE(containsNode(Observed, Unknown));
}

TEST(HighExprSimplify,
     MaskProjectionPreservesBitwiseValuesAndSignedExtensions) {
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (bool Signed : {false, true})
      for (NdOp Op : {NdOp::INT_AND, NdOp::INT_OR, NdOp::INT_XOR})
        for (uint64_t Mask : {0x55ULL, 0x80ULL, 0xff00ULL,
                              0x8000000000000000ULL, 0xffffffffffffffffULL}) {
          const auto X =
              HighExpr::makeVar(tempOfSize(1, 1), NdType::makeInt(1, Signed));
          const auto Narrow =
              HighExpr::makeBinop(Op, X, HighExpr::makeConst(0xa5, 1));
          Narrow->Type = NdType::makeInt(1, Signed);
          const auto Extended = integerCast(Narrow, Bytes);
          const auto Before = HighExpr::makeBinop(
              NdOp::INT_AND, Extended, HighExpr::makeConst(Mask, Bytes));
          // Record the independent expected values before any in-place cleanup.
          std::vector<llvm::APInt> Expected;
          for (uint64_t A = 0; A < 256; ++A)
            Expected.push_back(integerValue(Before, A));
          const auto After = basicSimplified(Before);
          for (uint64_t A = 0; A < 256; ++A)
            ASSERT_EQ(integerValue(After, A), Expected[A])
                << Bytes << " " << Signed << " " << Mask << " " << A;
        }
}

TEST(HighExprSimplify, MaskProjectionKeepsEffectsAndTrappingOperations) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 2));
  auto Call = std::make_shared<HighExpr>();
  Call->Kind = ExprKind::Call;
  Call->Type = NdType::makeInt(2, false);
  Call->CallTarget = "observe";
  auto Load = std::make_shared<HighExpr>();
  Load->Kind = ExprKind::Load;
  Load->Type = NdType::makeInt(2, false);
  Load->Operands = {X};
  const auto Div = HighExpr::makeBinop(NdOp::INT_DIV, X, X);
  auto Ordered = HighExpr::makeUnary(NdOp::INT_NOT, X);
  Ordered->MemoryOrdering = NdMemoryOrdering::Acquire;
  for (const auto &Effect : {Call, Load, Div, Ordered}) {
    const auto Flags =
        HighExpr::makeBinop(NdOp::INT_OR,
                            HighExpr::makeBinop(NdOp::INT_AND, Effect,
                                                HighExpr::makeConst(0xb8ff, 2)),
                            HighExpr::makeConst(0x4100, 2));
    const auto After = basicSimplified(HighExpr::makeBinop(
        NdOp::INT_AND, Flags, HighExpr::makeConst(0x4700, 2)));
    EXPECT_TRUE(containsNode(After, Effect)) << After->str();
  }
}

TEST(HighSymSimplify, OrdinaryAndEnhancedConversionPreserveIntegerResults) {
  for (const bool Symbolic : {false, true}) {
    SCOPED_TRACE(Symbolic);
    FunctionBuilder B;
    const auto X = B.param(0, 0x38);
    const auto Y = B.param(1, 0x30);
    const auto Xor = B.emit(NdOp::INT_XOR, {X, Y});
    const auto And = B.emit(NdOp::INT_AND, {X, Y});
    const auto Scaled =
        B.emit(NdOp::INT_MULT, {And, FunctionBuilder::constant(2)});
    const auto F = B.finish(NdOp::INT_ADD, {Xor, Scaled}, Symbolic);
    ASSERT_EQ(F.Params.size(), 2u);
    ASSERT_EQ(F.Params[0].RegOff, X.RegOff);
    ASSERT_EQ(F.Params[1].RegOff, Y.RegOff);
    for (uint32_t A : {0u, 1u, 7u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
      for (uint32_t C : {0u, 1u, 17u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        // HighIR parameter references use their source parameter position,
        // after the converter has bound the original MedIR register inputs.
        std::unordered_map<int, uint64_t> Values{{0, A}, {1, C}};
        unsigned Returns = 0;
        for (const auto &S : F.Body) {
          if (S.Kind == StmtKind::Assign && S.Dst &&
              S.Dst->Kind == ExprKind::Var)
            Values[S.Dst->Var.Id] =
                integerValue(S.Val, 0, 0, &Values).getZExtValue();
          if (S.Kind == StmtKind::Return) {
            ASSERT_NE(S.RetVal, nullptr);
            EXPECT_EQ(integerValue(S.RetVal, 0, 0, &Values).getZExtValue(),
                      static_cast<uint32_t>(A + C));
            ++Returns;
          }
        }
        EXPECT_EQ(Returns, 1u);
      }
  }
}

TEST(HighSymSimplify, RecoversSubtractionThroughWidenedLowProducts) {
  for (uint16_t Bytes : {1, 2, 4, 8}) {
    SCOPED_TRACE(Bytes);
    const auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
    const auto Y = HighExpr::makeVar(tempOfSize(2, Bytes));
    const auto Coefficient = extendInteger(
        HighExpr::makeConst(llvm::APInt::getAllOnes(Bytes * 8).getZExtValue(),
                            Bytes),
        Bytes * 2);
    const auto Product = HighExpr::makeBinop(NdOp::INT_MULT, Coefficient,
                                             extendInteger(Y, Bytes * 2));
    for (bool UseSlice : {false, true}) {
      ExprPtr Low = integerCast(Product, Bytes);
      if (UseSlice) {
        Low = HighExpr::makeBinop(NdOp::SUBBYTES, Product,
                                  HighExpr::makeConst(0, 4));
        Low->Type = NdType::makeInt(Bytes, false);
      }
      const auto Difference = HighExpr::makeBinop(NdOp::INT_ADD, X, Low);
      const auto Before =
          HighExpr::makeBinop(NdOp::INT_AND, Difference,
                              HighExpr::makeBinop(NdOp::INT_OR, X, Difference));
      const auto After = simplified(Before);
      const auto Text = After->str();
      EXPECT_EQ(Text.find('*'), std::string::npos) << Text;
      EXPECT_EQ(Text.find('&'), std::string::npos) << Text;
      EXPECT_EQ(Text.find('|'), std::string::npos) << Text;
      EXPECT_NE(Text.find('-'), std::string::npos) << Text;
      for (uint64_t A : {0ULL, 1ULL, 0x7fULL, 0x80ULL, ~0ULL})
        for (uint64_t B : {0ULL, 1ULL, 0xffULL, 0x8000000000000000ULL, ~0ULL})
          EXPECT_EQ(integerValue(After, A, B), integerValue(Before, A, B));
    }
  }
}

TEST(HighSymSimplify, WideningUsesTheSourceSignedness) {
  for (bool SourceSigned : {false, true}) {
    const auto X =
        HighExpr::makeVar(tempOfSize(1, 1), NdType::makeInt(1, SourceSigned));
    const auto Y =
        HighExpr::makeVar(tempOfSize(2, 1), NdType::makeInt(1, false));
    const auto Before = carrySum(integerCast(X, 2), extendInteger(Y, 2));
    const auto After = simplified(Before);
    EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
    for (uint64_t A = 0; A < 256; ++A)
      for (uint64_t B : {0ULL, 1ULL, 127ULL, 128ULL, 255ULL})
        EXPECT_EQ(integerValue(After, A, B), integerValue(Before, A, B));
    EXPECT_EQ(X->Type->IsSigned, SourceSigned);
  }
}

TEST(HighSymSimplify, NarrowArithmeticKeepsItsCarryBoundaryWhenWidened) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 1));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 1));
  const auto Before = HighExpr::makeBinop(
      NdOp::INT_SUB, extendInteger(carrySum(X, Y), 2),
      HighExpr::makeBinop(NdOp::INT_ADD, extendInteger(X, 2),
                          extendInteger(Y, 2)));
  const auto After = simplified(Before);
  EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
  for (uint64_t A = 0; A < 256; ++A)
    for (uint64_t B = 0; B < 256; ++B)
      ASSERT_EQ(integerValue(After, A, B).getZExtValue(),
                A + B > 255 ? 0xff00U : 0U);
}

TEST(HighSymSimplify, HighProductSliceKeepsCarriesFromTheLowByte) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 1));
  const auto Product = HighExpr::makeBinop(NdOp::INT_MULT, extendInteger(X, 2),
                                           HighExpr::makeConst(255, 2));
  auto High =
      HighExpr::makeBinop(NdOp::SUBBYTES, Product, HighExpr::makeConst(1, 4));
  High->Type = NdType::makeInt(1, false);
  const auto Before = carrySum(High, X);
  const auto After = simplified(Before);
  EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
  for (uint64_t A = 0; A < 256; ++A)
    EXPECT_EQ(integerValue(After, A), integerValue(Before, A));
}

TEST(HighSymSimplify, TruncationBeforeWideningStillDropsUpperBits) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 2));
  const auto Low = integerCast(X, 1, true);
  const auto Before = carrySum(extendInteger(Low, 2, true), X);
  const auto After = simplified(Before);
  EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
  for (uint64_t A = 0; A < 65536; ++A)
    ASSERT_EQ(integerValue(After, A), integerValue(Before, A));
}

TEST(HighSymSimplify, SignedLiteralCastsPreserveTheOriginalSignBit) {
  auto Negative = HighExpr::makeConst(255, 1);
  Negative->Type = NdType::makeInt(1, true);
  const auto X = HighExpr::makeVar(tempOfSize(1, 2));
  const auto Before = carrySum(integerCast(Negative, 2), X);
  const auto After = simplified(Before);
  for (uint64_t A : {0ULL, 1ULL, 255ULL, 32768ULL, 65535ULL})
    EXPECT_EQ(integerValue(After, A).getZExtValue(), (A - 1) & 65535);
}

TEST(HighSymSimplify, CastsDoNotHideEffectsUnknownValuesOrPointerIdentity) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 8));
  auto Call = std::make_shared<HighExpr>();
  Call->Kind = ExprKind::Call;
  Call->Type = NdType::makeInt(4, false);
  Call->CallTarget = "read_value";
  auto Pointer = HighExpr::makeVar(tempOfSize(2, 8));
  Pointer->Type = NdType::makePtr(NdType::makeInt(1, false));
  auto Float = HighExpr::makeConst(0, 4);
  Float->Type = NdType::makeFloat(4);
  const auto Quotient = HighExpr::makeBinop(
      NdOp::INT_DIV, X, HighExpr::makeVar(tempOfSize(2, 8)));
  auto Invalid = integerCast(HighExpr::makeConst(1, 4), 8);
  Invalid->CastTo = NdType::makeInt(2, false);
  auto InvalidExtension = extendInteger(X, 4);
  auto InvalidSlice =
      HighExpr::makeBinop(NdOp::SUBBYTES, X, HighExpr::makeConst(8, 4));
  InvalidSlice->Type = NdType::makeInt(4, false);
  auto InvalidBitCast = std::make_shared<HighExpr>();
  InvalidBitCast->Kind = ExprKind::BitCast;
  InvalidBitCast->Type = NdType::makeInt(4, false);
  InvalidBitCast->Operands = {X};
  for (const auto &Value :
       {Call, HighExpr::makeLoad(X, NdType::makeInt(4, false)),
        HighExpr::makeUndef(4), Pointer, Float, Quotient, Invalid,
        InvalidExtension, InvalidSlice, InvalidBitCast}) {
    const auto Opaque = integerCast(Value, 8);
    const auto Before = carrySum(Opaque, X);
    EXPECT_EQ(simplified(Before).get(), Before.get());
  }
}

TEST(HighSymSimplify, ARecoveredVariableRetainsItsOtherUsesSignedness) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4), NdType::makeInt(4, true));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4), NdType::makeInt(4, true));
  const auto Before = HighExpr::makeBinop(NdOp::INT_SUB, carrySum(X, Y), Y);
  Before->Type = NdType::makeInt(4, false);
  const auto After = simplified(Before);
  ASSERT_EQ(After->Kind, ExprKind::Cast);
  EXPECT_FALSE(After->Type->IsSigned);
  EXPECT_FALSE(After->CastTo->IsSigned);
  EXPECT_EQ(After->Operands[0].get(), X.get());
  EXPECT_TRUE(X->Type->IsSigned);
  EXPECT_TRUE(Y->Type->IsSigned);
}

TEST(HighSymSimplify, ReachesNumericRegionsUnderOpaquePredicateOperations) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto OriginalSum = carrySum(X, Y);
  const auto Population = HighExpr::makeUnary(NdOp::POPCOUNT, OriginalSum);
  const auto Predicate = HighExpr::makeBinop(NdOp::INT_EQUAL, Population,
                                             HighExpr::makeConst(3, 4));
  const auto After = simplified(Predicate);
  EXPECT_EQ(After->Kind, ExprKind::BinOp);
  EXPECT_EQ(After->Op, NdOp::INT_EQUAL);
  ASSERT_EQ(After->Operands.size(), 2U);
  EXPECT_EQ(After->Operands[0]->Op, NdOp::POPCOUNT);
  EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
  EXPECT_EQ(After->str().find('&'), std::string::npos) << After->str();
  EXPECT_NE(OriginalSum->str().find('^'), std::string::npos);
}

TEST(HighSymSimplify, SimplifyingCallArgumentsPreservesTheCallAndOrderedLoad) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto Load = HighExpr::makeLoad(X, NdType::makeInt(4, false),
                                       NdMemoryOrdering::Acquire);
  auto Call = std::make_shared<HighExpr>();
  Call->Kind = ExprKind::Call;
  Call->Type = NdType::makeInt(4, false);
  Call->CallTarget = "observe_values";
  Call->CallAddr = 0x401000;
  Call->Operands = {carrySum(X, Y), Load};
  const auto After = simplified(Call);
  EXPECT_EQ(After->Kind, ExprKind::Call);
  EXPECT_EQ(After->CallTarget, Call->CallTarget);
  EXPECT_EQ(After->CallAddr, Call->CallAddr);
  ASSERT_EQ(After->Operands.size(), 2U);
  EXPECT_EQ(After->Operands[1].get(), Load.get());
  EXPECT_EQ(After->Operands[0]->str().find('^'), std::string::npos);
  EXPECT_EQ(Call->Operands[0]->Op, NdOp::INT_ADD);
  EXPECT_NE(Call->Operands[0]->str().find('^'), std::string::npos);
}

TEST(HighSymSimplify, SimplifyingNumericPointerOperandKeepsThePointerCast) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 8));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 8));
  auto Cast = integerCast(carrySum(X, Y), 8);
  Cast->Type = Cast->CastTo = NdType::makePtr();
  const auto After = simplified(Cast);
  EXPECT_EQ(After->Kind, ExprKind::Cast);
  EXPECT_EQ(After->Type.get(), Cast->Type.get());
  EXPECT_EQ(After->CastTo.get(), Cast->CastTo.get());
  EXPECT_EQ(After->Operands[0]->str().find('^'), std::string::npos);
}

TEST(HighSymSimplify, KeepsRecoveredSliceWidthWhenTheRootHasNoType) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 8));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto Low = integerCast(X, 4);
  auto Sum = carrySum(Low, Y);
  Sum->Type.reset();
  auto Before = HighExpr::makeBinop(NdOp::INT_SUB, Sum, Y);
  const auto After = simplified(Before);
  ASSERT_EQ(After->Kind, ExprKind::Cast);
  ASSERT_TRUE(After->Type);
  ASSERT_TRUE(After->CastTo);
  EXPECT_EQ(After->Type->Size, 4U);
  EXPECT_EQ(After->CastTo->Size, 4U);
  EXPECT_EQ(After->Operands[0].get(), X.get());
}

TEST(HighSymSimplify, InfersWidthsOnceAcrossAnUnannotatedDeepDAG) {
  std::vector<ExprPtr> Nodes;
  auto Unannotated = [&](ExprPtr E) {
    E->Type.reset();
    Nodes.push_back(E);
    return E;
  };
  const auto X = Unannotated(HighExpr::makeVar(tempOfSize(1, 4)));
  const auto Y = Unannotated(HighExpr::makeVar(tempOfSize(2, 4)));
  const auto Xor = Unannotated(HighExpr::makeBinop(NdOp::INT_XOR, X, Y));
  const auto And = Unannotated(HighExpr::makeBinop(NdOp::INT_AND, X, Y));
  const auto Scaled = Unannotated(
      HighExpr::makeBinop(NdOp::INT_MULT, And, HighExpr::makeConst(2, 4)));
  auto Before = Unannotated(HighExpr::makeBinop(NdOp::INT_ADD, Xor, Scaled));
  for (unsigned I = 0; I < 8192; ++I)
    Before = Unannotated(
        HighExpr::makeBinop(NdOp::INT_ADD, Before, HighExpr::makeConst(0, 4)));
  const auto After = simplified(Before);
  EXPECT_EQ(After->str().find('^'), std::string::npos);
  EXPECT_EQ(After->str().find('&'), std::string::npos);
  EXPECT_NE(After->str().find('+'), std::string::npos);
  // Teardown must not itself recurse down the synthetic stress chain.
  for (auto &E : Nodes)
    E->Operands.clear();
}

HighStmt assignValue(const ExprPtr &Destination, const ExprPtr &Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = Destination;
  S.Val = Value;
  return S;
}

HighStmt returnValue(const ExprPtr &Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = Value;
  return S;
}

ExprPtr sharedMaskUse(const ExprPtr &X, const ExprPtr &Y, const ExprPtr &Mask) {
  return HighExpr::makeBinop(NdOp::INT_ADD, X,
                             HighExpr::makeBinop(NdOp::INT_OR, Mask, Y));
}

TEST(HighSymSimplify, UsesVisiblePureDefinitionsWithoutExpandingTheOutput) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  const auto Mask = HighExpr::makeBinop(NdOp::INT_AND, X, Y);
  std::vector<HighStmt> Body{assignValue(T, Mask),
                             returnValue(sharedMaskUse(X, Y, T))};
  simplifyExprSemantics(Body);
  EXPECT_EQ(Body.front().Val.get(), Mask.get());
  const auto Text = Body.back().RetVal->str();
  EXPECT_EQ(Text.find('|'), std::string::npos) << Text;
  EXPECT_EQ(Text.find('&'), std::string::npos) << Text;
  EXPECT_NE(Text.find('+'), std::string::npos) << Text;
}

TEST(HighSymSimplify, EmptyAnchorsPreserveAvailableDefinitions) {
  for (uint16_t Bytes : {1, 2, 4, 8}) {
    SCOPED_TRACE(Bytes);
    const auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
    const auto Y = HighExpr::makeVar(tempOfSize(2, Bytes));
    const auto T = HighExpr::makeVar(tempOfSize(3, Bytes));
    HighStmt Anchor;
    Anchor.Kind = StmtKind::Block;
    Anchor.Addr = 0x402000;
    std::vector<HighStmt> Body{
        assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)), Anchor,
        returnValue(sharedMaskUse(X, Y, T))};
    simplifyExprSemantics(Body);
    EXPECT_EQ(Body[1].Kind, StmtKind::Block);
    EXPECT_EQ(Body[1].Addr, Anchor.Addr);
    const auto &After = Body.back().RetVal;
    EXPECT_EQ(After->str().find('|'), std::string::npos) << After->str();
    for (uint64_t A : {0ULL, 1ULL, 127ULL, 0x80000000ULL, ~0ULL})
      for (uint64_t B : {0ULL, 17ULL, 0x8000000000000000ULL, ~0ULL})
        EXPECT_EQ(integerValue(After, A, B),
                  llvm::APInt(Bytes * 8, A, false, true) +
                      llvm::APInt(Bytes * 8, B, false, true));
  }
}

TEST(HighSymSimplify, AnchorsKeepControlFlowAndEffectBoundaries) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  for (unsigned Boundary = 0; Boundary < 7; ++Boundary) {
    SCOPED_TRACE(Boundary);
    HighStmt Anchor;
    Anchor.Kind = StmtKind::Block;
    Anchor.Addr = 0x402000;
    std::vector<HighStmt> Body;
    if (Boundary == 0) {
      HighStmt Jump;
      Jump.Kind = StmtKind::Goto;
      Jump.GotoTarget = Anchor.Addr;
      Body.push_back(Jump);
    } else if (Boundary == 1) {
      HighStmt Try;
      Try.Kind = StmtKind::ItaniumTry;
      Try.EHClauses.emplace_back();
      Try.EHClauses.back().HandlerVA = Anchor.Addr;
      Body.push_back(Try);
    } else if (Boundary == 2) {
      Anchor.Body.push_back(assignValue(T, X));
    } else if (Boundary == 3) {
      Anchor.Val = HighExpr::makeConst(1, 4);
    } else if (Boundary == 4) {
      Anchor.Dst = T;
    } else if (Boundary == 5) {
      Anchor.MemoryOrdering = NdMemoryOrdering::Acquire;
    } else {
      Anchor.IsPhiCopy = true;
    }
    Body.push_back(assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)));
    Body.push_back(Anchor);
    Body.push_back(returnValue(sharedMaskUse(X, Y, T)));
    simplifyExprSemantics(Body);
    EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos)
        << Body.back().RetVal->str();
  }
}

TEST(HighSymSimplify, InvalidatesDefinitionsAcrossRenamedLocalWrites) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  X->Var.RenameTag = 7;
  const auto Alias = HighExpr::makeVar(tempOfSize(90, 4));
  Alias->Var.RenameTag = 7;
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  const auto U = HighExpr::makeVar(tempOfSize(4, 4));
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)),
      assignValue(U, T), assignValue(Alias, HighExpr::makeConst(17, 4)),
      returnValue(sharedMaskUse(X, Y, U))};
  simplifyExprSemantics(Body);
  EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos)
      << Body.back().RetVal->str();
}

TEST(HighSymSimplify, ReassigningATemporaryDoesNotReuseItsOldDefinition) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)),
      assignValue(T, HighExpr::makeBinop(NdOp::INT_ADD, T, X)),
      returnValue(sharedMaskUse(X, Y, T))};
  simplifyExprSemantics(Body);
  EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos)
      << Body.back().RetVal->str();
}

TEST(HighSymSimplify, AvailableDefinitionsNeverGrowTheOriginalUse) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  const auto Before = HighExpr::makeBinop(NdOp::INT_AND, T, X);
  std::vector<HighStmt> Body{assignValue(T, carrySum(X, Y)),
                             returnValue(Before)};
  simplifyExprSemantics(Body);
  EXPECT_EQ(Body.back().RetVal.get(), Before.get());
  EXPECT_EQ(Body.back().RetVal->Operands[0].get(), T.get());
}

TEST(HighSymSimplify, DoesNotCarryAvailableDefinitionsAcrossControlFlow) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  const auto Definition =
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y));
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x402000;
  auto Return = returnValue(sharedMaskUse(X, Y, T));
  Return.Addr = Jump.GotoTarget;
  std::vector<HighStmt> WithLabel{Jump, Definition, Return};
  simplifyExprSemantics(WithLabel);
  EXPECT_NE(WithLabel.back().RetVal->str().find('|'), std::string::npos);

  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = sharedMaskUse(X, Y, T);
  Loop.Body.push_back(assignValue(T, X));
  std::vector<HighStmt> WithLoop{Definition, Loop};
  simplifyExprSemantics(WithLoop);
  EXPECT_NE(WithLoop.back().Cond->str().find('|'), std::string::npos);

  auto PhiDefinition = Definition;
  PhiDefinition.IsPhiCopy = true;
  std::vector<HighStmt> WithPhi{PhiDefinition, Return};
  simplifyExprSemantics(WithPhi);
  EXPECT_NE(WithPhi.back().RetVal->str().find('|'), std::string::npos);
}

TEST(HighSymSimplify, AddressTakenLocalsNeverBecomeSymbolicDefinitions) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  auto Address = std::make_shared<HighExpr>();
  Address->Kind = ExprKind::Addr;
  Address->Type = NdType::makePtr();
  Address->Operands = {T};
  HighStmt Escape;
  Escape.Kind = StmtKind::ExprStmt;
  Escape.Val = Address;
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)),
      returnValue(sharedMaskUse(X, Y, T)), Escape};
  simplifyExprSemantics(Body);
  EXPECT_NE(Body[1].RetVal->str().find('|'), std::string::npos);
  EXPECT_EQ(Body.back().Val.get(), Address.get());
  EXPECT_EQ(Address->Operands[0].get(), T.get());
}

TEST(HighSymSimplify, EffectBoundariesInvalidateAvailableDefinitions) {
  for (bool Mutable : {false, true}) {
    SCOPED_TRACE(Mutable);
    auto Input = tempOfSize(1, 4);
    if (Mutable)
      Input.Kind = MedVar::Reg;
    const auto X = HighExpr::makeVar(Input);
    const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
    const auto T = HighExpr::makeVar(tempOfSize(3, 4));
    auto Call = std::make_shared<HighExpr>();
    Call->Kind = ExprKind::Call;
    Call->Type = NdType::makeInt(4);
    Call->CallTarget = "update_state";
    HighStmt Effect;
    Effect.Kind = StmtKind::ExprStmt;
    Effect.CallExpr = Call;
    std::vector<HighStmt> Body{
        assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)), Effect,
        returnValue(sharedMaskUse(X, Y, T))};
    simplifyExprSemantics(Body);
    EXPECT_EQ(Body[1].CallExpr.get(), Call.get());
    // An unescaped C temporary survives the call; an uncaptured register
    // read and every definition depending on it must be invalidated.
    EXPECT_EQ(Body.back().RetVal->str().find('|') != std::string::npos,
              Mutable);
  }
}

TEST(HighSymSimplify, EmitsThePackedParityOfAFullWordProduct) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto Z = HighExpr::makeVar(tempOfSize(3, 4));
  const auto W = HighExpr::makeVar(tempOfSize(4, 4));
  auto C = [](uint32_t V) {
    return HighExpr::makeConst(V, 4, ConstantAddressProvenance::Scalar);
  };
  auto Bin = HighExpr::makeBinop;
  const auto XY = Bin(NdOp::INT_XOR, X, Y);
  const std::array<ExprPtr, 6> Words{
      Bin(NdOp::INT_OR, Y, Z),
      Bin(NdOp::INT_MULT, Bin(NdOp::INT_AND, X, Z), Bin(NdOp::INT_OR, Y, W)),
      XY,
      Bin(NdOp::INT_ADD, X, Bin(NdOp::INT_XOR, W, Y)),
      Bin(NdOp::INT_AND, Z, W),
      HighExpr::makeUnary(NdOp::INT_NOT, Bin(NdOp::INT_OR, W, X))};
  struct Factor {
    unsigned Offset;
    int Weight;
    unsigned Word;
  };
  const Factor Factors[]{{8, 819200, 0},  {14, 507904, 1},  {6, 188416, 2},
                         {12, 950272, 3}, {1, 278528, 3},   {5, 57344, 1},
                         {9, 90112, 2},   {1, -16384, 4},   {3, 81920, 3},
                         {1, -507904, 5}, {4, -622592, 5},  {2, 311296, 1},
                         {3, 1146880, 2}, {10, -409600, 4}, {11, 106496, 0},
                         {13, -53248, 4}, {7, -114688, 5}};
  auto Product = C(768);
  for (const auto &[Offset, Weight, Index] : Factors)
    Product = Bin(NdOp::INT_MULT, Product,
                  Bin(NdOp::INT_ADD, C(Offset),
                      Bin(NdOp::INT_MULT, C(Weight), Words[Index])));
  auto Input = Bin(NdOp::INT_ADD, Bin(NdOp::INT_ADD, Product, XY),
                   Bin(NdOp::INT_MULT, C(2), Bin(NdOp::INT_AND, X, Y)));
  std::vector<HighStmt> Body{returnValue(Input)};
  simplifyExprSemantics(Body);
  const auto &Output = Body.back().RetVal;
  ASSERT_NE(Output->str().find("<<"), std::string::npos) << Output->str();
  ASSERT_EQ(Output->str().find('*'), std::string::npos) << Output->str();
  for (uint32_t I = 0; I < 128; ++I) {
    const std::unordered_map<int, uint64_t> Values{{1, I * 0x9e3779b9u},
                                                   {2, ~I},
                                                   {3, (I + 11) * 0xd1b54a33u},
                                                   {4, I ^ 0x80000000u}};
    EXPECT_EQ(integerValue(Input, 0, 0, &Values),
              integerValue(Output, 0, 0, &Values));
  }
}

TEST(HighSymSimplify, ReusesLiveAffineWordInNonzeroResidueLookups) {
  constexpr uint64_t Units[] = {0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D, 0x27D4EB2F,
                                0x165667B1, 0xD3A2646D, 0xA24BAED5, 0x9FB21C65};
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (unsigned Extended : {0u, 1u, 2u}) {
      if (Extended && Bytes == 8)
        continue;
      SCOPED_TRACE(std::to_string(Bytes) + ":" + std::to_string(Extended));
      auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
      auto Y = HighExpr::makeVar(tempOfSize(2, Bytes));
      auto T = HighExpr::makeVar(tempOfSize(3, Extended ? 8 : Bytes));
      auto View = Extended ? integerCast(T, Bytes) : T;
      auto C = [&](uint64_t V) { return HighExpr::makeConst(V, Bytes); };
      auto Base = HighExpr::makeBinop(
          NdOp::INT_ADD, X, HighExpr::makeBinop(NdOp::INT_MULT, C(3), Y));
      auto Product = C(uint64_t(1) << (8 * Bytes - 7));
      for (unsigned I = 0; I < 8; ++I) {
        auto Factor = HighExpr::makeBinop(
            NdOp::INT_ADD,
            HighExpr::makeBinop(NdOp::INT_MULT, C(Units[I]), View),
            C(Units[I] * (I == 7 ? 6 : I)));
        Product = HighExpr::makeBinop(NdOp::INT_MULT, Product, Factor);
      }
      auto Original = HighExpr::makeBinop(
          NdOp::INT_ADD, Product, HighExpr::makeBinop(NdOp::INT_ADD, X, Y));
      std::vector<HighStmt> Body{
          assignValue(T,
                      Extended ? extendInteger(Base, 8, Extended == 2) : Base),
          returnValue(Original)};
      simplifyExprSemantics(Body);
      EXPECT_NE(Body.back().RetVal->str().find(">>"), std::string::npos)
          << Body.back().RetVal->str();
      EXPECT_NE(Body.back().RetVal->str().find(T->str()), std::string::npos);
      for (uint64_t I = 0; I < 512; ++I) {
        const unsigned Width = Bytes * 8;
        auto A = llvm::APInt(Width, I * 0x9e3779b97f4a7c15ULL);
        auto B = llvm::APInt(Width, (I + 17) * 0xd1b54a32d192ed03ULL);
        auto Word = A + 3 * B;
        auto Want = llvm::APInt(Width, 1).shl(Width - 7);
        for (unsigned J = 0; J < 8; ++J)
          Want *= Units[J] * (Word + (J == 7 ? 6 : J));
        Want += A + B;
        std::unordered_map<int, uint64_t> Values{
            {1, A.getZExtValue()},
            {2, B.getZExtValue()},
            {3, (Extended == 2 ? Word.sextOrTrunc(64) : Word.zextOrTrunc(64))
                    .getZExtValue()}};
        EXPECT_EQ(integerValue(Original, 0, 0, &Values), Want);
        EXPECT_EQ(integerValue(Body.back().RetVal, 0, 0, &Values), Want);
      }
    }
}

TEST(HighSymSimplify, DefinitionBindingsPreserveAssignmentWidthAndSignedness) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  for (uint16_t Bytes : {4, 8}) {
    const auto T =
        HighExpr::makeVar(tempOfSize(3, Bytes), NdType::makeInt(Bytes, false));
    const auto Use = Bytes == 4 ? T : integerCast(T, 4);
    std::vector<HighStmt> Body{
        assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)),
        returnValue(sharedMaskUse(X, Y, Use))};
    simplifyExprSemantics(Body);
    const auto Text = Body.back().RetVal->str();
    // The known assignment conversion also preserves the low prefix of a
    // widened capture. The wider store no longer has to block this identity.
    EXPECT_EQ(Text.find('|'), std::string::npos) << Text;
    EXPECT_EQ(Text.find('&'), std::string::npos) << Text;
    for (uint64_t A : {0ULL, 1ULL, 7ULL, 0x80000000ULL, 0xffffffffULL})
      for (uint64_t B : {0ULL, 1ULL, 17ULL, 0x80000000ULL, 0xffffffffULL})
        EXPECT_EQ(integerValue(Body.back().RetVal, A, B),
                  llvm::APInt(32, A + B));
  }
}

TEST(HighSymSimplify, MixedSignednessDefinitionKeepsItsWideningMeaning) {
  for (bool DestinationSigned : {false, true}) {
    const auto X = HighExpr::makeVar(tempOfSize(1, 1),
                                     NdType::makeInt(1, !DestinationSigned));
    const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
    const auto T = HighExpr::makeVar(tempOfSize(3, 1),
                                     NdType::makeInt(1, DestinationSigned));
    const auto Before =
        HighExpr::makeBinop(NdOp::INT_SUB, carrySum(integerCast(T, 4), Y), Y);
    std::vector<HighStmt> Body{assignValue(T, X), returnValue(Before)};
    simplifyExprSemantics(Body);
    EXPECT_FALSE(containsNode(Body.back().RetVal, T));
    EXPECT_EQ(X->Type->IsSigned, !DestinationSigned);
    for (uint64_t Value = 0; Value != 256; ++Value) {
      const auto Byte = llvm::APInt(8, Value);
      const auto Expected = DestinationSigned ? Byte.sext(32) : Byte.zext(32);
      EXPECT_EQ(integerValue(Body.back().RetVal, Value, 0xdeadbeef), Expected);
    }
  }
}

TEST(HighSymSimplify, RelatesDisjointFrameLocalsAndRejectsAliasingBoundaries) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  auto Frame = [](int64_t Offset, uint16_t Size = 4) {
    auto V = tempOfSize(3, Size);
    V.Kind = MedVar::Stack;
    V.StackOff = Offset;
    return HighExpr::makeVar(V);
  };
  for (unsigned Boundary = 0; Boundary != 6; ++Boundary) {
    SCOPED_TRACE(Boundary);
    const auto T = Frame(-16);
    std::vector<HighStmt> Body{
        assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y))};
    if (Boundary == 1) {
      // Adjacent frame storage is independent.
      Body.push_back(assignValue(Frame(-12), X));
    } else if (Boundary == 2 || Boundary == 3) {
      // A partial overlap or a differently sized view at the same offset
      // must not leave the earlier whole-word value available.
      Body.push_back(
          assignValue(Frame(Boundary == 2 ? -14 : -16, 2), integerCast(X, 2)));
    } else if (Boundary == 4) {
      HighStmt Store;
      Store.Kind = StmtKind::Store;
      Store.StoreAddr = HighExpr::makeConst(0x700000, 8);
      Store.StoreVal = X;
      Body.push_back(Store);
    } else if (Boundary == 5) {
      // Taking a peer's address can expose T through pointer arithmetic.
      auto Address = std::make_shared<HighExpr>();
      Address->Kind = ExprKind::Addr;
      Address->Type = NdType::makePtr();
      Address->Operands = {Frame(-12)};
      HighStmt Escape;
      Escape.Kind = StmtKind::ExprStmt;
      Escape.Val = Address;
      Body.insert(Body.begin(), Escape);
    }
    Body.push_back(returnValue(sharedMaskUse(X, Y, T)));
    simplifyExprSemantics(Body);
    const auto Text = Body.back().RetVal->str();
    if (Boundary < 2) {
      EXPECT_EQ(Text.find('|'), std::string::npos) << Text;
      EXPECT_EQ(Text.find('&'), std::string::npos) << Text;
    } else {
      EXPECT_NE(Text.find('|'), std::string::npos) << Text;
    }
  }
}

TEST(HighSymSimplify, MemoryWritesKillTransitiveFrameDependencies) {
  auto V = tempOfSize(3, 4);
  V.Kind = MedVar::Stack;
  V.StackOff = -16;
  const auto Frame = HighExpr::makeVar(V);
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto T = HighExpr::makeVar(tempOfSize(4, 4));
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeConst(0x700000, 8);
  Store.StoreVal = X;
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Frame)), Store,
      returnValue(sharedMaskUse(X, Frame, T))};
  simplifyExprSemantics(Body);
  EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos);
}

TEST(HighSymSimplify, ImplicitOutputsInvalidateAvailableDefinitions) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  auto ImplicitWrite = HighExpr::makeUnary(NdOp::INT_NOT, X);
  ImplicitWrite->IntrinsicOutputs.push_back(X->Var);
  HighStmt Effect;
  Effect.Kind = StmtKind::ExprStmt;
  Effect.Val = ImplicitWrite;
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)), Effect,
      returnValue(sharedMaskUse(X, Y, T))};
  simplifyExprSemantics(Body);
  EXPECT_EQ(Body[1].Val.get(), ImplicitWrite.get());
  EXPECT_EQ(Body[1].Val->IntrinsicOutputs.size(), 1U);
  EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos);
}

TEST(HighSymSimplify, DefinitionSnapshotsKeepValuesBeforeInputReassignment) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_ADD, X, Y)),
      assignValue(X, HighExpr::makeConst(7, 4)),
      returnValue(HighExpr::makeBinop(
          NdOp::INT_SUB, HighExpr::makeBinop(NdOp::INT_SUB, T, X), Y))};
  simplifyExprSemantics(Body);
  for (uint64_t A : {0ULL, 1ULL, 7ULL, 127ULL, 0x80000000ULL, 0xffffffffULL})
    for (uint64_t B : {0ULL, 17ULL, 0xffffffffULL}) {
      std::unordered_map<int, uint64_t> Values{{1, A}, {2, B}};
      for (const auto &S : Body) {
        if (S.Kind == StmtKind::Assign)
          Values[S.Dst->Var.Id] =
              integerValue(S.Val, 0, 0, &Values).getZExtValue();
        else if (S.Kind == StmtKind::Return)
          EXPECT_EQ(integerValue(S.RetVal, 0, 0, &Values).getZExtValue(),
                    (A - 7) & 0xffffffffU);
      }
    }
}

ExprPtr parameterView(int Id, uint16_t Bytes) {
  auto V = tempOfSize(Id, Bytes);
  V.Kind = MedVar::Param;
  return HighExpr::makeVar(V, NdType::makeInt(Bytes, false));
}

TEST(HighSymSimplify, RelatesNarrowParameterViewsToTheObservedWideValue) {
  const auto Wide = parameterView(1, 8);
  const auto Low = parameterView(1, 4);
  const auto Y = parameterView(2, 4);
  const auto Difference = integerCast(
      HighExpr::makeBinop(NdOp::INT_SUB, Wide, HighExpr::makeConst(1, 8)), 4);
  const auto Before = HighExpr::makeBinop(
      NdOp::INT_SUB, Difference,
      HighExpr::makeBinop(NdOp::INT_OR, HighExpr::makeUnary(NdOp::INT_NOT, Y),
                          Low));
  const auto After = simplified(Before);
  EXPECT_EQ(After->str().find('-'), std::string::npos) << After->str();
  EXPECT_EQ(After->str().find('~'), std::string::npos) << After->str();
  EXPECT_NE(After->str().find('|'), std::string::npos) << After->str();
  for (uint64_t A : {0ULL, 0xffffffffULL, 0x1234567880000000ULL, ~0ULL})
    for (uint64_t B : {0ULL, 1ULL, 0x80000000ULL, 0xffffffffULL})
      EXPECT_EQ(integerValue(After, A, B).getZExtValue(),
                (A | B) & 0xffffffffU);
}

TEST(HighSymSimplify, ParameterViewRelationsNeverInventUpperBits) {
  const auto Wide = parameterView(1, 8);
  const auto Low = parameterView(1, 4);
  for (bool Signed : {false, true}) {
    const auto Before =
        HighExpr::makeBinop(NdOp::INT_SUB, Wide, extendInteger(Low, 8, Signed));
    const auto After = simplified(Before);
    for (uint64_t A : {0ULL, 0x80000000ULL, 0x1234567880000000ULL,
                       0xffffffff00000000ULL, ~0ULL})
      EXPECT_EQ(integerValue(After, A), integerValue(Before, A));
    EXPECT_NE(integerValue(After, 0x1234567800000000ULL).getZExtValue(), 0U);
  }
}

TEST(HighSymSimplify, DoesNotMergeUnrelatedOrMalformedParameterViews) {
  const auto Wide = parameterView(1, 8);
  auto DifferentId = parameterView(2, 4);
  auto DifferentVersion = parameterView(1, 4);
  ++DifferentVersion->Var.SSAVer;
  auto OrdinaryLocal = HighExpr::makeVar(tempOfSize(1, 4));
  auto Malformed = parameterView(1, 8);
  Malformed->Type = NdType::makeInt(4, false);
  for (const auto &Other :
       {DifferentId, DifferentVersion, OrdinaryLocal, Malformed}) {
    const auto After = simplified(
        HighExpr::makeBinop(NdOp::INT_SUB, integerCast(Wide, 4), Other));
    EXPECT_NE(After->str(), "0") << After->str();
  }
}

TEST(HighExprSimplify, WalksDeepExpressionGraphsOnAWorkerStack) {
  const auto Input = HighExpr::makeVar(tempOfSize(1, kWordBytes));
  for (bool Ordered : {false, true}) {
    SCOPED_TRACE(Ordered);
    auto Value = Input;
    if (Ordered) {
      Value = HighExpr::makeUnary(NdOp::INT_NOT, Input);
      Value->MemoryOrdering = NdMemoryOrdering::Acquire;
    }
    auto Deep = Value;
    for (unsigned I = 0; I < 8192; ++I)
      Deep = HighExpr::makeBinop(NdOp::INT_SUB, std::move(Deep),
                                 HighExpr::makeConst(0, kWordBytes));
    std::vector<HighStmt> Body{returnValue(Deep)};
    bool Changed = false;
    std::thread Worker([&] { Changed = simplifyAllExprs(Body); });
    Worker.join();
    EXPECT_EQ(Changed, !Ordered);
    EXPECT_EQ(Body.front().RetVal, Ordered ? Deep : Input);

    // Keep shared-pointer teardown outside the worker-stack assertion. The
    // ordered graph deliberately remains deep because its accesses survive.
    Body.clear();
    auto Current = std::move(Deep);
    while (Current && Current != Value && !Current->Operands.empty()) {
      auto Next = std::move(Current->Operands[0]);
      Current->Operands.clear();
      Current = std::move(Next);
    }
  }
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

struct ScopedSemanticThreads {
  unsigned Previous = workerThreadOverride().load();
  ScopedSemanticThreads() { setWorkerThreadCount(4); }
  ~ScopedSemanticThreads() { setWorkerThreadCount(Previous); }
};

std::vector<std::string>
statementExpressions(const std::vector<HighStmt> &Body) {
  std::vector<std::string> Text;
  walkStmts(Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) { Text.push_back(E->str()); });
  });
  return Text;
}

TEST(HighSymSimplify,
     ParallelSegmentsKeepSharedNodesAndScopeDefinitionsSeparate) {
  ScopedSemanticThreads Threads;
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  const auto SharedUse = sharedMaskUse(X, Y, T);
  const auto OriginalUse = SharedUse->str();
  auto Arm = [&](NdOp Op) {
    HighStmt Anchor;
    Anchor.Kind = StmtKind::Block;
    return std::vector<HighStmt>{assignValue(T, HighExpr::makeBinop(Op, X, Y)),
                                 Anchor, returnValue(SharedUse)};
  };
  std::vector<HighStmt> Serial;
  for (unsigned I = 0; I < 256; ++I) {
    HighStmt S;
    switch (I % 3) {
    case 0:
      S.Kind = StmtKind::IfElse;
      S.Cond = X;
      S.Body = Arm(NdOp::INT_AND);
      S.ElseBody = Arm(NdOp::INT_XOR);
      break;
    case 1:
      S.Kind = StmtKind::Switch;
      S.SwitchExpr = X;
      S.Cases.push_back({0, Arm(NdOp::INT_AND)});
      S.DefaultBody = Arm(NdOp::INT_XOR);
      break;
    default:
      S.Kind = StmtKind::ItaniumTry;
      S.Body = Arm(NdOp::INT_AND);
      S.EHClauses.emplace_back();
      S.EHClauses.back().Kind = HighEHClauseKind::ItaniumCatch;
      S.EHClauseBodies.push_back(Arm(NdOp::INT_XOR));
      break;
    }
    Serial.push_back(std::move(S));
  }
  // Statement vectors are independent, but both runs deliberately share all
  // input expression DAGs. A parallel rewrite must replace roots privately.
  auto Parallel = Serial;
  simplifyExprSemantics(Serial);
  simplifyExprSemantics(Parallel, 4);
  EXPECT_EQ(statementExpressions(Parallel), statementExpressions(Serial));
  EXPECT_EQ(SharedUse->str(), OriginalUse);
  EXPECT_EQ(SharedUse->Operands[1]->Operands[0].get(), T.get());

  auto CheckArm = [&](const std::vector<HighStmt> &Body, bool Xor) {
    ASSERT_EQ(Body.size(), 3U);
    EXPECT_EQ(Body[1].Kind, StmtKind::Block);
    if (!Xor)
      EXPECT_EQ(Body.back().RetVal->str().find('|'), std::string::npos);
    for (uint64_t A : {0ULL, 1ULL, 7ULL, 0x80000000ULL, 0xffffffffULL})
      for (uint64_t B : {0ULL, 1ULL, 17ULL, 0xffffffffULL}) {
        std::unordered_map<int, uint64_t> Values{{1, A}, {2, B}};
        Values[3] =
            integerValue(Body.front().Val, 0, 0, &Values).getZExtValue();
        const uint64_t Expected =
            (A + ((Xor ? A ^ B : A & B) | B)) & 0xffffffffU;
        EXPECT_EQ(
            integerValue(Body.back().RetVal, 0, 0, &Values).getZExtValue(),
            Expected);
      }
  };
  for (const auto &S : Parallel) {
    if (S.Kind == StmtKind::IfElse) {
      CheckArm(S.Body, false);
      CheckArm(S.ElseBody, true);
    } else if (S.Kind == StmtKind::Switch) {
      CheckArm(S.Cases.front().Body, false);
      CheckArm(S.DefaultBody, true);
    } else {
      CheckArm(S.Body, false);
      CheckArm(S.EHClauseBodies.front(), true);
    }
  }
}

TEST(HighSymSimplify,
     ParallelSegmentsPreserveDefinitionKillsAndOrdinaryStores) {
  ScopedSemanticThreads Threads;
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  std::vector<HighStmt> Serial;
  for (unsigned I = 0; I < 512; ++I) {
    auto Input = tempOfSize(1, 4);
    if (I & 8)
      Input.Kind = MedVar::Reg;
    const auto X = HighExpr::makeVar(Input);
    const auto T = HighExpr::makeVar(tempOfSize(3 + I, 4));
    auto Definition = assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y));
    auto Return = returnValue(sharedMaskUse(X, Y, T));
    Return.Addr = kEntry + 16 * I;
    HighStmt Scope;
    Scope.Kind = StmtKind::Block;
    HighStmt Boundary;
    bool HasBoundary = true;
    switch (I % 8) {
    case 0:
      HasBoundary = false;
      break;
    case 1:
      Boundary.Kind = StmtKind::ExprStmt;
      Boundary.CallExpr = std::make_shared<HighExpr>();
      Boundary.CallExpr->Kind = ExprKind::Call;
      Boundary.CallExpr->Type = NdType::makeInt(4);
      Boundary.CallExpr->CallTarget = "update_state";
      break;
    case 2:
      Boundary.Kind = StmtKind::Goto;
      Boundary.GotoTarget = Return.Addr;
      Scope.Body.push_back(Boundary);
      HasBoundary = false;
      break;
    case 3:
      Definition.IsPhiCopy = true;
      HasBoundary = false;
      break;
    case 4:
      Boundary.Kind = StmtKind::ExprStmt;
      Boundary.Val = std::make_shared<HighExpr>();
      Boundary.Val->Kind = ExprKind::Addr;
      Boundary.Val->Type = NdType::makePtr();
      Boundary.Val->Operands = {T};
      break;
    case 5:
      Scope.Kind = StmtKind::ItaniumTry;
      Scope.EHClauses.emplace_back();
      Scope.EHClauses.back().Kind = HighEHClauseKind::ItaniumCatch;
      Scope.EHClauses.back().ContinuationVAs.push_back(Return.Addr);
      HasBoundary = false;
      break;
    case 6:
      Definition.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      HasBoundary = false;
      break;
    case 7:
      Boundary.Kind = StmtKind::Store;
      Boundary.StoreAddr = HighExpr::makeConst(0x700000, 8);
      Boundary.StoreVal = X;
      break;
    }
    Scope.Body.push_back(std::move(Definition));
    if (HasBoundary)
      Scope.Body.push_back(std::move(Boundary));
    Scope.Body.push_back(std::move(Return));
    Serial.push_back(std::move(Scope));
  }
  auto Parallel = Serial;
  simplifyExprSemantics(Serial);
  simplifyExprSemantics(Parallel, 4);
  EXPECT_EQ(statementExpressions(Parallel), statementExpressions(Serial));
  for (size_t I = 0; I < Parallel.size(); ++I) {
    const auto Text = Parallel[I].Body.back().RetVal->str();
    const bool Kill = I % 8 != 0 && I % 8 != 7 && (I % 8 != 1 || (I & 8));
    EXPECT_EQ(Text.find('|') != std::string::npos, Kill) << I << ": " << Text;
  }
}

TEST(HighSymSimplify, SharedBitwiseSumIsSimplifiedInIndirectCallTargets) {
  for (bool Register : {false, true}) {
    SCOPED_TRACE(Register);
    auto Input = tempOfSize(1, 8);
    if (Register)
      Input.Kind = MedVar::Reg;
    auto X = HighExpr::makeVar(Input, NdType::makeInt(8, false));
    auto A = HighExpr::makeVar(tempOfSize(3, 8), X->Type);
    auto B = HighExpr::makeVar(tempOfSize(4, 8), X->Type);
    auto Mask = HighExpr::makeConst(0x1357, 8);
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall("indirect", 0, {});
    Call.CallExpr->IsIndirectCall = true;
    Call.CallExpr->Type = NdType::makeVoid();
    Call.CallExpr->IndirectTarget = HighExpr::makeBinop(NdOp::INT_ADD, A, B);
    std::vector<HighStmt> Body{
        assignValue(A, HighExpr::makeBinop(NdOp::INT_OR, Mask, X)),
        assignValue(B, HighExpr::makeBinop(NdOp::INT_AND, Mask, X)), Call};
    simplifyExprSemantics(Body);
    const auto &Target = Body.back().CallExpr->IndirectTarget;
    const auto Text = Target->str();
    EXPECT_FALSE(containsNode(Target, A)) << Text;
    EXPECT_FALSE(containsNode(Target, B)) << Text;
    EXPECT_EQ(Text.find('|'), std::string::npos) << Text;
    EXPECT_EQ(Text.find('&'), std::string::npos) << Text;
    for (uint64_t Value :
         {0ULL, 1ULL, 0x1357ULL, 0x8000000000000000ULL, 0xffffffffffffffffULL})
      EXPECT_EQ(integerValue(Target, Value).getZExtValue(), Value + 0x1357);
    EXPECT_EQ(Body.back().Kind, StmtKind::Call);
    EXPECT_EQ(Body.back().CallExpr->Kind, ExprKind::Call);
  }
}

TEST(HighSymSimplify, CallInputFactsRetireBeforeNestedEffectsAndLaterCalls) {
  for (bool Nested : {false, true}) {
    SCOPED_TRACE(Nested);
    auto Input = tempOfSize(1, 8);
    Input.Kind = MedVar::Reg;
    auto X = HighExpr::makeVar(Input, NdType::makeInt(8, false));
    auto A = HighExpr::makeVar(tempOfSize(3, 8), X->Type);
    auto B = HighExpr::makeVar(tempOfSize(4, 8), X->Type);
    auto Mask = HighExpr::makeConst(0x1357, 8);
    HighStmt Effect;
    Effect.Kind = StmtKind::Call;
    Effect.CallExpr = HighExpr::makeCall("mutate", 0x401000, {});
    Effect.CallExpr->Type = X->Type;
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall("indirect", 0, {});
    Call.CallExpr->IsIndirectCall = true;
    Call.CallExpr->Type = NdType::makeVoid();
    Call.CallExpr->IndirectTarget = HighExpr::makeBinop(NdOp::INT_ADD, A, B);
    std::vector<HighStmt> Body{
        assignValue(A, HighExpr::makeBinop(NdOp::INT_OR, Mask, X)),
        assignValue(B, HighExpr::makeBinop(NdOp::INT_AND, Mask, X))};
    if (Nested)
      Call.CallExpr->Operands = {Effect.CallExpr};
    else
      Body.push_back(Effect);
    Body.push_back(Call);
    simplifyExprSemantics(Body);
    EXPECT_TRUE(containsNode(Body.back().CallExpr->IndirectTarget, A));
    EXPECT_TRUE(containsNode(Body.back().CallExpr->IndirectTarget, B));
    if (Nested)
      EXPECT_EQ(Body.back().CallExpr->Operands.front(), Effect.CallExpr);
    else
      EXPECT_EQ(Body[2].CallExpr, Effect.CallExpr);
  }
}

TEST(HighSymSimplify, CapturedCallResultsKeepTheirValueAcrossLaterCalls) {
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    SCOPED_TRACE(Variant);
    auto Input = tempOfSize(1, 8);
    Input.Kind = MedVar::Reg;
    auto X = HighExpr::makeVar(Input, NdType::makeInt(8, false));
    auto A = HighExpr::makeVar(tempOfSize(3, 8), X->Type);
    auto B = HighExpr::makeVar(tempOfSize(4, 8), X->Type);
    auto Read = HighExpr::makeCall("read_word", 0x401000, {});
    Read->Type = X->Type;
    auto Mask = HighExpr::makeConst(0x1357, 8);
    HighStmt Effect;
    Effect.Kind = StmtKind::Call;
    Effect.CallExpr = HighExpr::makeCall("mutate", 0x402000, {});
    Effect.CallExpr->Type = NdType::makeVoid();
    if (Variant == 1)
      Effect.CallExpr->IntrinsicOutputs = {X->Var};
    if (Variant == 3) {
      auto Address = std::make_shared<HighExpr>();
      Address->Kind = ExprKind::Addr;
      Address->Type = NdType::makePtr(X->Type);
      Address->Operands = {X};
      Effect.CallExpr->Operands = {Address};
    }
    std::vector<HighStmt> Body{
        assignValue(X, Read),
        assignValue(A, HighExpr::makeBinop(NdOp::INT_OR, Mask, X)),
        assignValue(B, HighExpr::makeBinop(NdOp::INT_AND, Mask, X)), Effect};
    if (Variant == 2)
      Body.push_back(assignValue(X, HighExpr::makeConst(17, 8)));
    Body.push_back(returnValue(HighExpr::makeBinop(NdOp::INT_ADD, A, B)));
    simplifyExprSemantics(Body);
    const auto &Result = Body.back().RetVal;
    if (Variant) {
      EXPECT_TRUE(containsNode(Result, A));
      EXPECT_TRUE(containsNode(Result, B));
    } else {
      EXPECT_FALSE(containsNode(Result, A)) << Result->str();
      EXPECT_FALSE(containsNode(Result, B)) << Result->str();
      for (uint64_t Value : {0ULL, 1ULL, 0x1357ULL, 0x8000000000000000ULL,
                             0xffffffffffffffffULL})
        EXPECT_EQ(integerValue(Result, Value).getZExtValue(), Value + 0x1357);
    }
    EXPECT_EQ(Body.front().Val, Read);
    EXPECT_EQ(Body[3].CallExpr, Effect.CallExpr);
  }
}

TEST(HighSymSimplify, CapturedCallTargetExecutesAfterAnInterveningCall) {
  auto Value = tempOfSize(1, 8);
  Value.Kind = MedVar::Reg;
  auto X = HighExpr::makeVar(Value, NdType::makeInt(8, true));
  auto A = HighExpr::makeVar(tempOfSize(3, 8), X->Type);
  auto B = HighExpr::makeVar(tempOfSize(4, 8), X->Type);
  auto Read = HighExpr::makeCall("read_base", 0x401000, {});
  Read->Type = X->Type;
  auto Mask = HighExpr::makeConst(0x1357, 8);
  HighStmt Effect;
  Effect.Kind = StmtKind::Call;
  Effect.CallExpr = HighExpr::makeCall("mutate_registers", 0x402000, {});
  Effect.CallExpr->Type = NdType::makeInt(4, true);
  auto Call = HighExpr::makeCall("indirect", 0, {});
  Call->IsIndirectCall = true;
  Call->Type = X->Type;
  Call->IndirectTarget = HighExpr::makeBinop(NdOp::INT_ADD, A, B);
  HighFunc Function;
  Function.Entry = kEntry;
  Function.Name = "captured_target";
  Function.ReturnType = X->Type;
  Function.Body = {assignValue(X, Read),
                   assignValue(A, HighExpr::makeBinop(NdOp::INT_OR, Mask, X)),
                   assignValue(B, HighExpr::makeBinop(NdOp::INT_AND, Mask, X)),
                   Effect, returnValue(Call)};
  simplifyExprSemantics(Function.Body);
  const auto &Target = Function.Body.back().RetVal->IndirectTarget;
  EXPECT_FALSE(containsNode(Target, A)) << Target->str();
  EXPECT_FALSE(containsNode(Target, B)) << Target->str();
  std::string C;
  llvm::raw_string_ostream Out(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({Function}, Out, Options));
  Out.flush();
  const std::string Program = C + R"C(
static unsigned calls;
static int64_t target(void) { ++calls; return 0x1234; }
int64_t read_base(void) { ++calls; return (int64_t)((uintptr_t)&target - 0x1357); }
int mutate_registers(void) { ++calls; return 0x6789; }
int main(void) {
  return captured_target() != 0x1234 || calls != 3;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighSymSimplify, ReciprocalArithmeticKeepsItsUnprototypedCall) {
  auto V = tempOfSize(0, 4);
  V.Kind = MedVar::Param;
  const auto X = HighExpr::makeVar(V, NdType::makeInt(4, false));
  const auto Input =
      HighExpr::makeBinop(NdOp::INT_ADD, X, HighExpr::makeConst(17, 4));
  const auto Product =
      HighExpr::makeBinop(NdOp::INT_MULT, HighExpr::makeConst(0x51eb851f, 8),
                          extendInteger(Input, 8, false));
  auto High =
      HighExpr::makeBinop(NdOp::SUBBYTES, Product, HighExpr::makeConst(4, 4));
  High->Type = NdType::makeInt(4, false);
  auto Quotient =
      HighExpr::makeBinop(NdOp::INT_ASHR, High, HighExpr::makeConst(5, 4));
  Quotient->Type = NdType::makeInt(4, false);
  auto Remainder = HighExpr::makeBinop(
      NdOp::INT_SUB, Input,
      HighExpr::makeBinop(NdOp::INT_MULT, HighExpr::makeConst(100, 4),
                          Quotient));
  HighStmt Observe;
  Observe.Kind = StmtKind::Call;
  Observe.CallExpr = HighExpr::makeCall("observe_word", 0x401000, {Quotient});
  Observe.CallExpr->Type = NdType::makeInt(4, false);
  HighFunc F;
  F.Name = "reciprocal_remainder";
  F.Entry = kEntry;
  F.ReturnType = X->Type;
  F.Params = {{"x", X->Type, kNoParamReg, 0}};
  F.Body = {Observe, returnValue(Remainder)};
  simplifyExprSemantics(F.Body);
  ASSERT_EQ(F.Body.size(), 2u);
  ASSERT_TRUE(F.Body.front().CallExpr);
  EXPECT_EQ(F.Body.front().CallExpr->CallTarget, "observe_word");
  ASSERT_EQ(F.Body.front().CallExpr->Operands.size(), 1u);
  EXPECT_NE(F.Body.front().CallExpr->Operands.front()->str().find('/'),
            std::string::npos);
  EXPECT_NE(F.Body.back().RetVal->str().find('%'), std::string::npos);
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  OS.flush();
  EXPECT_EQ(C.find("0x51EB851F"), std::string::npos) << C;
  const std::string Program = C + R"C(
static unsigned calls;
static uint32_t observed;
int observe_word(unsigned int word) { ++calls; observed = word; return 0; }
int main(void) {
  uint32_t x = 0;
  for (unsigned i = 0; i != 100000; ++i) {
    const uint32_t input = x + 17u;
    calls = 0;
    if (reciprocal_remainder(x) != input % 100u ||
        observed != input / 100u || calls != 1)
      return 1;
    x = x * 1664525u + 1013904223u;
  }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighSymSimplify, RepackedByteFlagsBecomeOneMaskedWord) {
  auto V = tempOfSize(0, 8);
  V.Kind = MedVar::Param;
  const auto X = HighExpr::makeVar(V, NdType::makeInt(8, false));
  auto Packed =
      HighExpr::makeBinop(NdOp::INT_AND, X, HighExpr::makeConst(0x3c7300, 8));
  for (unsigned Bit : {0u, 2u, 4u, 6u, 7u, 11u}) {
    const auto Shifted =
        HighExpr::makeBinop(NdOp::INT_RIGHT, X, HighExpr::makeConst(Bit, 8));
    const auto Byte = integerCast(Shifted, 1);
    const auto Flag =
        HighExpr::makeBinop(NdOp::INT_AND, Byte, HighExpr::makeConst(1, 1));
    Packed = HighExpr::makeBinop(
        NdOp::INT_OR, Packed,
        HighExpr::makeBinop(NdOp::INT_MULT, integerCast(Flag, 8),
                            HighExpr::makeConst(uint64_t(1) << Bit, 8)));
  }
  HighFunc F;
  F.Name = "repacked_flags";
  F.Entry = kEntry;
  F.ReturnType = X->Type;
  F.Params = {{"x", X->Type, kNoParamReg, 0}};
  F.Body = {returnValue(
      HighExpr::makeBinop(NdOp::INT_OR, Packed, HighExpr::makeConst(2, 8)))};
  simplifyExprSemantics(F.Body);
  EXPECT_EQ(F.Body.front().RetVal->str().find(" >> "), std::string::npos);
  EXPECT_EQ(F.Body.front().RetVal->str().find('*'), std::string::npos);
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  OS.flush();
  const std::string Program = C + R"C(
int main(void) {
  uint64_t x = 0;
  for (unsigned i = 0; i != 100000; ++i) {
    if (repacked_flags(x) != ((x & 0x3c7bd5u) | 2u))
      return 1;
    x = x * UINT64_C(6364136223846793005) + 1442695040888963407u;
  }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighSymSimplify, ConstantDivisorProofKeepsTrapsAndOperandEffects) {
  const auto X = HighExpr::makeVar(tempOfSize(0, 4), NdType::makeInt(4, false));
  const auto Dynamic = HighExpr::makeVar(tempOfSize(1, 4), X->Type);
  for (const auto Op : {NdOp::INT_DIV, NdOp::INT_REM}) {
    const auto Safe = HighExpr::makeBinop(Op, X, HighExpr::makeConst(100, 4));
    const auto SafeDifference = HighExpr::makeBinop(NdOp::INT_SUB, Safe, Safe);
    const auto Reduced = simplified(SafeDifference);
    ASSERT_EQ(Reduced->Kind, ExprKind::Const) << Reduced->str();
    EXPECT_EQ(Reduced->ConstVal, 0u);
    for (const auto Divisor : {Dynamic, HighExpr::makeConst(0, 4),
                               HighExpr::makeConst(0x100000000, 8)}) {
      const auto Unsafe = HighExpr::makeBinop(Op, X, Divisor);
      const auto Difference =
          HighExpr::makeBinop(NdOp::INT_SUB, Unsafe, Unsafe);
      EXPECT_TRUE(containsNode(simplified(Difference), Unsafe));
    }
    const auto Call = HighExpr::makeCall("read_word", 0x401000, {});
    Call->Type = X->Type;
    const auto Effect =
        HighExpr::makeBinop(Op, Call, HighExpr::makeConst(100, 4));
    const auto Difference = HighExpr::makeBinop(NdOp::INT_SUB, Effect, Effect);
    EXPECT_TRUE(containsNode(simplified(Difference), Call));
  }
  for (const auto Op : {NdOp::INT_SDIV, NdOp::INT_SREM}) {
    const auto Unsafe =
        HighExpr::makeBinop(Op, X, HighExpr::makeConst(0xffffffff, 4));
    const auto Difference = HighExpr::makeBinop(NdOp::INT_SUB, Unsafe, Unsafe);
    EXPECT_TRUE(containsNode(simplified(Difference), Unsafe));
  }
}

TEST(HighSymSimplify, NegatedPackedComparisonKeepsOnlyObservedFlags) {
  auto Parameter = [](int Id) {
    auto V = tempOfSize(Id, 4);
    V.Kind = MedVar::Param;
    return HighExpr::makeVar(V, NdType::makeInt(4, false));
  };
  const auto X = Parameter(0), Y = Parameter(1);
  const auto Captured = HighExpr::makeVar(tempOfSize(10, 4), X->Type);
  const auto Call = HighExpr::makeCall("capture_noise", 0x401000, {});
  Call->Type = X->Type;
  const auto A =
      HighExpr::makeBinop(NdOp::INT_EQUAL, X, HighExpr::makeConst(7, 4));
  const auto B =
      HighExpr::makeBinop(NdOp::INT_LESS, Y, HighExpr::makeConst(13, 4));
  auto Packed = HighExpr::makeBinop(NdOp::INT_AND, Captured,
                                    HighExpr::makeConst(0xfffff77f, 4));
  for (auto [Flag, Bit] : {std::pair{A, 0x80}, std::pair{B, 0x800}})
    Packed = HighExpr::makeBinop(
        NdOp::INT_OR, Packed,
        HighExpr::makeBinop(NdOp::INT_MULT, integerCast(Flag, 4),
                            HighExpr::makeConst(Bit, 4)));
  const auto Encoded = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeConst(0, 4),
      HighExpr::makeBinop(NdOp::INT_OR, HighExpr::makeConst(0xfffff77f, 4),
                          HighExpr::makeUnary(NdOp::INT_NOT, Packed)));
  HighFunc F;
  F.Name = "packed_condition";
  F.Entry = kEntry;
  F.ReturnType = X->Type;
  F.Params = {{"x", X->Type, kNoParamReg, 0}, {"y", Y->Type, kNoParamReg, 1}};
  F.Body = {assignValue(Captured, Call),
            returnValue(HighExpr::makeBinop(NdOp::INT_EQUAL, Encoded,
                                            HighExpr::makeConst(0x801, 4)))};
  simplifyExprSemantics(F.Body);
  EXPECT_FALSE(containsNode(F.Body.back().RetVal, Captured))
      << F.Body.back().RetVal->str();
  EXPECT_EQ(F.Body.front().Val.get(), Call.get());
  EXPECT_EQ(F.Body.back().RetVal->str().find('*'), std::string::npos);
  EXPECT_EQ(F.Body.back().RetVal->str().find('~'), std::string::npos);
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  OS.flush();
  const std::string Program = C + R"C(
static unsigned calls;
static uint32_t noise;
int capture_noise(void) { ++calls; return (int)noise; }
int main(void) {
  for (unsigned n = 0; n != 16; ++n) {
    noise = noise * 1664525u + 1013904223u;
    for (uint32_t x = 0; x != 32; ++x)
      for (uint32_t y = 0; y != 32; ++y) {
        calls = 0;
        const uint32_t flags = (noise & 0xfffff77fu) |
                               (0x80u * (x == 7)) | (0x800u * (y < 13));
        const uint32_t encoded = 0u - (0xfffff77fu | ~flags);
        if (packed_condition(x, y) != (encoded == 0x801u) || calls != 1)
          return 1;
      }
  }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighSymSimplify, RecoveredPackedPredicatesExecuteAtTheirOriginalWidths) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (NdOp Op : {NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL, NdOp::INT_LESS,
                    NdOp::INT_LESSEQUAL, NdOp::INT_SLESS, NdOp::INT_SLESSEQUAL})
      for (bool Invert : {false, true})
        for (bool MaskResult : {false, true}) {
          auto V = tempOfSize(0, Bytes);
          V.Kind = MedVar::Param;
          auto X = HighExpr::makeVar(V, NdType::makeInt(Bytes, false));
          auto Predicate =
              HighExpr::makeBinop(Op, X, HighExpr::makeConst(17, Bytes));
          auto Packed = HighExpr::makeBinop(
              NdOp::INT_OR, HighExpr::makeConst(0x100, 4),
              HighExpr::makeBinop(NdOp::INT_MULT, extendInteger(Predicate, 4),
                                  HighExpr::makeConst(0x40, 4)));
          auto Result =
              extendInteger(HighExpr::makeBinop(
                                NdOp::INT_EQUAL, Packed,
                                HighExpr::makeConst(Invert ? 0x100 : 0x140, 4)),
                            4);
          if (MaskResult)
            Result = HighExpr::makeBinop(NdOp::INT_SUB,
                                         HighExpr::makeConst(0, 4), Result);
          HighFunc F;
          F.Name = "predicate_" + std::to_string(Functions.size());
          F.Entry = kEntry + Functions.size() * 16;
          F.ReturnType = NdType::makeInt(4, false);
          F.Params = {{"x", X->Type, kNoParamReg, 0}};
          F.Body = {returnValue(Result)};
          simplifyExprSemantics(F.Body);
          EXPECT_EQ(F.Body.back().RetVal->str().find('*'), std::string::npos)
              << F.Name << ": " << F.Body.back().RetVal->str();
          const bool Signed =
              Op == NdOp::INT_SLESS || Op == NdOp::INT_SLESSEQUAL;
          const std::string Type =
              (Signed ? "int" : "uint") + std::to_string(Bytes * 8) + "_t";
          const char *Compare = Op == NdOp::INT_EQUAL      ? "=="
                                : Op == NdOp::INT_NOTEQUAL ? "!="
                                : Op == NdOp::INT_LESS || Op == NdOp::INT_SLESS
                                    ? "<"
                                    : "<=";
          std::string Expected = "((" + Type + ")x " + Compare + " 17)";
          if (Invert)
            Expected = "!(" + Expected + ")";
          if (MaskResult)
            Expected = "(0u - (uint32_t)(" + Expected + "))";
          Checks += "if (" + F.Name + "(x) != (uint32_t)(" + Expected +
                    ")) return 1;\n";
          Functions.push_back(std::move(F));
        }
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  OS.flush();
  const auto Program = C + R"C(
int main(void) {
  const uint64_t values[] = {0, 1, 16, 17, 18, 127, 128, 255, 32767, 32768,
      65535, 0x7fffffff, 0x80000000, 0xffffffff, 0x7fffffffffffffffULL,
      0x8000000000000000ULL, ~0ULL};
  for (unsigned i = 0; i != sizeof(values) / sizeof(*values); ++i) {
    uint64_t x = values[i];
)C" + Checks + "}\nreturn 0;\n}\n";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighSymSimplify, PeeledComparisonsKeepTheirValueAtEverySignedness) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (bool Signed : {false, true}) {
      const unsigned Bits = Bytes * 8;
      const uint64_t Mask =
          Bytes == 8 ? ~uint64_t(0) : (uint64_t(1) << Bits) - 1;
      const uint64_t Sign = uint64_t(1) << (Bits - 1);
      const std::string Word = "uint" + std::to_string(Bits) + "_t";
      for (uint64_t Literal :
           {uint64_t(5), Sign - 1, Sign, Sign + 1, -uint64_t(5) & Mask, Mask})
        for (unsigned Shape = 0; Shape != 5; ++Shape) {
          auto V = tempOfSize(0, Bytes);
          V.Kind = MedVar::Param;
          auto X = HighExpr::makeVar(V, NdType::makeInt(Bytes, Signed));
          const auto Zero = HighExpr::makeConst(0, Bytes);
          const auto L = HighExpr::makeConst(Literal, Bytes);
          const auto Minus = HighExpr::makeBinop(NdOp::INT_SUB, Zero, X);
          ExprPtr Predicate;
          std::string Expected;
          const std::string U = "(" + Word + ")v";
          const std::string Lit =
              "(" + Word + ")UINT64_C(" + std::to_string(Literal) + ")";
          switch (Shape) {
          case 0:
            Predicate = HighExpr::makeBinop(NdOp::INT_EQUAL, Minus, L);
            Expected = "(" + Word + ")(0u - " + U + ") == " + Lit;
            break;
          case 1:
            Predicate = HighExpr::makeBinop(
                NdOp::INT_EQUAL, HighExpr::makeUnary(NdOp::INT_NOT, Minus), L);
            Expected =
                "(" + Word + ")~(" + Word + ")(0u - " + U + ") == " + Lit;
            break;
          case 2:
            Predicate = HighExpr::makeBinop(
                NdOp::INT_NOTEQUAL, HighExpr::makeUnary(NdOp::INT_NOT, Minus),
                L);
            Expected =
                "(" + Word + ")~(" + Word + ")(0u - " + U + ") != " + Lit;
            break;
          case 3:
            Predicate = HighExpr::makeBinop(
                NdOp::INT_EQUAL,
                HighExpr::makeBinop(NdOp::INT_SUB, Zero,
                                    HighExpr::makeUnary(NdOp::INT_NOT, Minus)),
                L);
            Expected = "(" + Word + ")(0u - (" + Word + ")~(" + Word +
                       ")(0u - " + U + ")) == " + Lit;
            break;
          default:
            // The literal's top bit must survive a sign-extending promotion of
            // the operand when the mask is peeled away.
            Predicate = HighExpr::makeBinop(
                NdOp::INT_EQUAL,
                HighExpr::makeBinop(NdOp::INT_AND,
                                    HighExpr::makeConst(Sign | 0xf, Bytes),
                                    HighExpr::makeUnary(NdOp::INT_NOT, Minus)),
                L);
            Expected = "(" + Word + ")(" + Word + ")(UINT64_C(" +
                       std::to_string(Sign | 0xf) + ") & ~(" + Word +
                       ")(0u - " + U + ")) == " + Lit;
            break;
          }
          HighFunc F;
          F.Name = "peeled_" + std::to_string(Functions.size());
          F.Entry = kEntry + Functions.size() * 16;
          F.ReturnType = NdType::makeInt(4, false);
          F.Params = {{"x", X->Type, kNoParamReg, 0}};
          F.Body = {returnValue(extendInteger(Predicate, 4))};
          simplifyExprSemantics(F.Body);
          Checks += "    if (" + F.Name + "((" + (Signed ? "int" : "uint") +
                    std::to_string(Bits) + "_t)v) != (uint32_t)(" + Expected +
                    ")) return 1;\n";
          Functions.push_back(std::move(F));
        }
    }
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  OS.flush();
  const auto Program = C + R"C(
int main(void) {
  uint64_t values[1024];
  unsigned count = 0;
  for (unsigned i = 0; i != 256; ++i) {
    values[count++] = i;
    values[count++] = i << 8 | i;
    values[count++] = UINT64_C(0x8000000000000000) >> (i % 64);
    values[count++] = ~UINT64_C(0) - i;
  }
  for (unsigned i = 0; i != count; ++i) {
    const uint64_t v = values[i];
)C" + Checks + "  }\n  return 0;\n}\n";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighSymSimplify, NegatedTermKeepsItsAvailableDefinition) {
  auto Parameter = [](int Id) {
    auto V = tempOfSize(Id, 4);
    V.Kind = MedVar::Param;
    return HighExpr::makeVar(V, NdType::makeInt(4, false));
  };
  const auto X = Parameter(0), Y = Parameter(1);
  const auto Negated = HighExpr::makeVar(tempOfSize(10, 4), X->Type);
  HighFunc F;
  F.Name = "negated_definition";
  F.Entry = kEntry;
  F.ReturnType = X->Type;
  F.Params = {{"x", X->Type, kNoParamReg, 0}, {"y", Y->Type, kNoParamReg, 1}};
  F.Body = {
      assignValue(Negated, HighExpr::makeBinop(NdOp::INT_SUB,
                                               HighExpr::makeConst(0, 4), X)),
      returnValue(HighExpr::makeBinop(NdOp::INT_ADD, Y, Negated))};
  simplifyExprSemantics(F.Body);
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  OS.flush();
  const std::string Program = C + R"C(
int main(void) {
  const uint32_t values[] = {0, 1, 2, 0x7fffffffu, 0x80000000u, 0xffffffffu};
  for (unsigned i = 0; i != 6; ++i)
    for (unsigned j = 0; j != 6; ++j)
      if (negated_definition(values[i], values[j]) != values[j] - values[i])
        return 1;
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighCTruthValue, InvertedEqualityHasAZeroOrOneValue) {
  std::vector<HighFunc> Functions;
  auto Make = [&](const std::string &Name, std::vector<HighStmt> Body,
                  const ExprPtr &X, TypeRef Return) {
    HighFunc F;
    F.Name = Name;
    F.Entry = kEntry + Functions.size() * 16;
    F.ReturnType = Return;
    F.Params = {{"x", X->Type, kNoParamReg, 0}};
    F.Body = std::move(Body);
    Functions.push_back(std::move(F));
  };
  auto V = tempOfSize(0, 4);
  V.Kind = MedVar::Param;
  const auto X = HighExpr::makeVar(V, NdType::makeInt(4, false));
  const auto Zero = HighExpr::makeConst(0, 4);
  const auto Zero1 = HighExpr::makeConst(0, 1);
  const auto One = HighExpr::makeConst(1, 4);
  const auto Five = HighExpr::makeConst(5, 4);
  const auto IsZero = HighExpr::makeBinop(NdOp::INT_EQUAL, X, Zero);
  const auto IsFive = HighExpr::makeBinop(NdOp::INT_EQUAL, X, Five);
  const auto Less = HighExpr::makeBinop(NdOp::INT_LESS, X, Five);
  const auto U32 = NdType::makeInt(4, false);
  const auto U8 = NdType::makeInt(1, false);
  auto Not = [](const ExprPtr &P) {
    return HighExpr::makeUnary(NdOp::BOOL_NOT, P);
  };
  auto IsFalse = [&](const ExprPtr &P) {
    return HighExpr::makeBinop(NdOp::INT_EQUAL, P, Zero1);
  };
  auto Plus1 = [&](const ExprPtr &P) {
    return HighExpr::makeBinop(NdOp::INT_ADD, extendInteger(P, 4), One);
  };
  // Values.
  Make("not_zero_wide", {returnValue(extendInteger(Not(IsZero), 4))}, X, U32);
  Make("not_zero_byte", {returnValue(Not(IsZero))}, X, U8);
  Make("not_zero_sum", {returnValue(Plus1(Not(IsZero)))}, X, U32);
  Make("false_zero_wide", {returnValue(extendInteger(IsFalse(IsZero), 4))}, X,
       U32);
  Make("false_zero_sum", {returnValue(Plus1(IsFalse(IsZero)))}, X, U32);
  Make("false_five_sum", {returnValue(Plus1(IsFalse(IsFive)))}, X, U32);
  Make("false_less_sum", {returnValue(Plus1(IsFalse(Less)))}, X, U32);
  Make("not_less_sum", {returnValue(Plus1(Not(Less)))}, X, U32);
  const auto T = HighExpr::makeVar(tempOfSize(10, 1), U8);
  Make("not_local",
       {assignValue(T, IsZero), returnValue(extendInteger(Not(T), 4))}, X, U32);
  // Conditions read only the truth of the operand.
  auto Branch = [&](const ExprPtr &Condition) {
    HighStmt If;
    If.Kind = StmtKind::If;
    If.Cond = Condition;
    If.Body = {returnValue(One)};
    return std::vector<HighStmt>{std::move(If), returnValue(Zero)};
  };
  Make("if_not_zero", Branch(Not(IsZero)), X, U32);
  Make("if_false_zero", Branch(IsFalse(IsZero)), X, U32);
  Make("if_both",
       Branch(HighExpr::makeBinop(NdOp::BOOL_AND, Not(IsZero), Not(Less))), X,
       U32);
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  OS.flush();
  // The shortest test of a value stays the value itself.
  EXPECT_NE(C.find("if (x) {"), std::string::npos) << C;
  const auto Program = C + R"C(
#define CHECK(call, want) \
  if ((uint32_t)(call) != (uint32_t)(want)) return __LINE__ % 200 + 1;
int main(void) {
  const uint32_t values[] = {0, 1, 2, 3, 4, 5, 6, 256, 0x80000000u, 0xffffffffu};
  for (unsigned i = 0; i != 10; ++i) {
    const uint32_t x = values[i];
    CHECK(not_zero_wide(x), x != 0)
    CHECK((uint8_t)not_zero_byte(x), x != 0)
    CHECK(not_zero_sum(x), (x != 0) + 1)
    CHECK(false_zero_wide(x), x != 0)
    CHECK(false_zero_sum(x), (x != 0) + 1)
    CHECK(false_five_sum(x), (x != 5) + 1)
    CHECK(false_less_sum(x), (x >= 5) + 1)
    CHECK(not_less_sum(x), (x >= 5) + 1)
    CHECK(not_local(x), x != 0)
    CHECK(if_not_zero(x), x != 0)
    CHECK(if_false_zero(x), x != 0)
    CHECK(if_both(x), x >= 5)
  }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighCTruthValue, GreaterThanZeroComparesTheWidthTheTestsRead) {
  auto Parameter = [](uint16_t Bytes, bool Signed) {
    auto V = tempOfSize(0, Bytes);
    V.Kind = MedVar::Param;
    return HighExpr::makeVar(V, NdType::makeInt(Bytes, Signed));
  };
  auto IsZero = [](const ExprPtr &X) {
    return HighExpr::makeBinop(NdOp::INT_EQUAL, X,
                               HighExpr::makeConst(0, X->Type->Size));
  };
  auto NotZero = [](const ExprPtr &X) {
    return HighExpr::makeBinop(NdOp::INT_NOTEQUAL, X,
                               HighExpr::makeConst(0, X->Type->Size));
  };
  auto Below = [](const ExprPtr &X) {
    return HighExpr::makeBinop(NdOp::INT_SLESS, X,
                               HighExpr::makeConst(0, X->Type->Size));
  };
  auto Not = [](const ExprPtr &X) {
    return HighExpr::makeUnary(NdOp::BOOL_NOT, X);
  };
  auto Or = [](const ExprPtr &A, const ExprPtr &B) {
    return HighExpr::makeBinop(NdOp::BOOL_OR, A, B);
  };
  auto And = [](const ExprPtr &A, const ExprPtr &B) {
    return HighExpr::makeBinop(NdOp::BOOL_AND, A, B);
  };
  auto AtLeastZero = [](const ExprPtr &X) {
    return HighExpr::makeBinop(NdOp::INT_SLESSEQUAL,
                               HighExpr::makeConst(0, X->Type->Size), X);
  };
  auto AtMostZero = [](const ExprPtr &X) {
    return HighExpr::makeBinop(NdOp::INT_SLESSEQUAL, X,
                               HighExpr::makeConst(0, X->Type->Size));
  };

  std::vector<HighFunc> Functions;
  std::string Checks;
  // Each shape prints as a condition, and as a value where it means one. The
  // C expression states what the binary's flags decide, over `uint64_t v`.
  const auto Add = [&](const std::string &Name, const ExprPtr &X,
                       const ExprPtr &Test, const std::string &Meaning) {
    const auto Word = NdType::makeInt(4, false);
    const std::string Argument =
        std::string(X->Type->IsSigned ? "(int" : "(uint") +
        std::to_string(X->Type->Size * 8) + "_t)v";
    HighFunc AsCondition;
    AsCondition.Name = Name + "_if";
    AsCondition.Entry = kEntry + Functions.size() * 16;
    AsCondition.ReturnType = Word;
    AsCondition.Params = {{"x", X->Type, kNoParamReg, 0}};
    HighStmt If;
    If.Kind = StmtKind::If;
    If.Cond = Test;
    If.Body = {returnValue(HighExpr::makeConst(1, 4))};
    AsCondition.Body = {std::move(If), returnValue(HighExpr::makeConst(0, 4))};
    Functions.push_back(std::move(AsCondition));
    Checks += "    CHECK(" + Name + "_if(" + Argument + "), " + Meaning + ")\n";

    HighFunc AsValue;
    AsValue.Name = Name + "_value";
    AsValue.Entry = kEntry + Functions.size() * 16;
    AsValue.ReturnType = Word;
    AsValue.Params = {{"x", X->Type, kNoParamReg, 0}};
    AsValue.Body = {returnValue(extendInteger(Test, 4))};
    Functions.push_back(std::move(AsValue));
    Checks +=
        "    CHECK(" + Name + "_value(" + Argument + "), " + Meaning + ")\n";
  };

  const auto S32 = Parameter(4, true), U32 = Parameter(4, false);
  const auto U64 = Parameter(8, false), S64 = Parameter(8, true);
  const auto U16 = Parameter(2, false);
  const auto Low = integerCast(U64, 4, true), LowU = integerCast(U64, 4, false);
  const auto Low16 = integerCast(U32, 2, true);
  const auto Low16U = integerCast(U32, 2, false);
  const std::string S("(int32_t)v"), W("(int64_t)v"), H("(int16_t)v");

  // `test; jle` at each width: the test keeps every byte it reads.
  Add("s32_ge", S32, And(NotZero(S32), AtLeastZero(S32)), S + " > 0");
  Add("s32_above", S32, And(NotZero(S32), Not(Below(S32))), S + " > 0");
  Add("s32_equal", S32, And(NotZero(S32), IsZero(Below(S32))), S + " > 0");
  Add("u32_ge", U32, And(NotZero(U32), AtLeastZero(U32)), S + " > 0");
  Add("u32_above", U32, And(NotZero(U32), Not(Below(U32))), S + " > 0");
  Add("u64_ge", U64, And(NotZero(U64), AtLeastZero(U64)), W + " > 0");
  Add("u64_above", U64, And(NotZero(U64), Not(Below(U64))), W + " > 0");
  Add("u64_equal", U64, And(NotZero(U64), IsZero(Below(U64))), W + " > 0");
  Add("s64_above", S64, And(NotZero(S64), Not(Below(S64))), W + " > 0");
  Add("u64_not_either", U64, Not(Or(IsZero(U64), Below(U64))), W + " > 0");
  Add("u64_not_most", U64, Not(AtMostZero(U64)), W + " > 0");
  Add("s32_not_ge", S32, Not(AtLeastZero(S32)), S + " < 0");
  Add("u64_not_ge", U64, Not(AtLeastZero(U64)), W + " < 0");
  Add("u64_either", U64, Or(IsZero(U64), Below(U64)), W + " <= 0");
  Add("s32_not_either", S32, Not(Or(IsZero(S32), Below(S32))), S + " > 0");
  Add("u16_not_either", U16, Not(Or(IsZero(U16), Below(U16))), H + " > 0");

  // The tests read the same low bytes through views of a wider variable.
  Add("view32_not_either", U64, Not(Or(IsZero(LowU), Below(Low))), S + " > 0");
  Add("view32_either", U64, Or(IsZero(LowU), Below(Low)), S + " <= 0");
  Add("view32_above", U64, And(NotZero(LowU), Not(Below(Low))), S + " > 0");
  Add("view16_not_either", U32, Not(Or(IsZero(Low16U), Below(Low16))),
      H + " > 0");
  Add("view16_above", U32, And(NotZero(Low16U), Not(Below(Low16))), H + " > 0");

  // An extension above the view stays in the comparison: a zero-extended word
  // is never negative, and the signed test takes the signed operand's value.
  const auto ZeroExtended = extendInteger(LowU, 8);
  const auto SignExtended = extendInteger(Low, 8, true);
  Add("zext_not_most", U64, Not(AtMostZero(ZeroExtended)), "(uint32_t)v != 0");
  Add("sext_not_most", U64, Not(AtMostZero(SignExtended)), S + " > 0");
  Add("extended_not_either", U64,
      Not(Or(IsZero(ZeroExtended), Below(SignExtended))), S + " > 0");

  Add("extended_either", U64, Or(IsZero(ZeroExtended), Below(SignExtended)),
      S + " <= 0");

  // A parameter declared unsigned reads signed in the IR: C compares the
  // declaration, so its comparison needs the signed view.
  Add("declared_unsigned_not_most", U32, Not(AtMostZero(S32)), S + " > 0");
  Add("declared_unsigned_not_either", U32, Not(Or(IsZero(S32), Below(S32))),
      S + " > 0");
  Add("declared_signed_not_most", S32, Not(AtMostZero(U32)), S + " > 0");

  // The inverted spelling of a chain reads its signed pair as one comparison.
  Add("s32_chain", S32, Not(Or(Or(IsZero(S32), Below(S32)), IsZero(S32))),
      S + " > 0");

  // A parameter declared wider than what the tests read: C reads every byte of
  // the declaration, so its name alone is not the value tested.
  Add("declared_wide_truth", U64, NotZero(S32), "(uint32_t)v != 0");
  Add("declared_wide_zero", U64, IsZero(S32), "(uint32_t)v == 0");
  Add("declared_wide_above", U64, And(NotZero(S32), Not(Below(S32))),
      S + " > 0");
  Add("declared_wide_not_either", U64, Not(Or(IsZero(S32), Below(S32))),
      S + " > 0");
  // The inverted spelling of a chain prints each term on its own.
  Add("declared_wide_chain", U64,
      Not(Or(Or(IsZero(S32), Below(S32)), IsZero(S32))), S + " > 0");

  // A zero narrower than the variable states the test's width.
  const auto ZeroAt4 = HighExpr::makeConst(0, 4);
  Add("narrow_not_either", U64,
      Not(Or(HighExpr::makeBinop(NdOp::INT_EQUAL, U64, ZeroAt4),
             HighExpr::makeBinop(NdOp::INT_SLESS, U64, ZeroAt4))),
      S + " > 0");

  // A view read above a wider test: the signed view does not decide zero, so
  // no one comparison means this.
  const std::string Mixed = "v != 0 && " + S + " >= 0";
  Add("mixed_ge", U64, And(NotZero(U64), AtLeastZero(Low)), Mixed);
  Add("mixed_above", U64, And(NotZero(U64), Not(Below(Low))), Mixed);
  Add("mixed_equal", U64, And(NotZero(U64), IsZero(Below(Low))), Mixed);
  Add("mixed_not_either", U64, Not(Or(IsZero(U64), Below(Low))), Mixed);
  Add("mixed_either", U64, Or(IsZero(U64), Below(Low)),
      "v == 0 || " + S + " < 0");

  // A call result prints its declared type, which the signed test can't rely
  // on: `uint32_t f()` compared bare is `f() != 0`.
  const auto AddCall = [&](const std::string &Name, const TypeRef &Returned,
                           const std::string &Meaning) {
    const auto Argument = NdType::makeInt(8, false);
    HighFunc Callee;
    Callee.Name = Name + "_source";
    Callee.Entry = kEntry + Functions.size() * 16;
    Callee.ReturnType = Returned;
    Callee.Params = {{"x", Argument, kNoParamReg, 0}};
    Callee.Body = {returnValue(
        integerCast(Parameter(8, false), Returned->Size, Returned->IsSigned))};
    const va_t Target = Callee.Entry;
    Functions.push_back(std::move(Callee));
    auto Call =
        HighExpr::makeCall(Name + "_source", Target, {Parameter(8, false)});
    Call->Type = Returned;
    HighFunc Caller;
    Caller.Name = Name;
    Caller.Entry = kEntry + Functions.size() * 16;
    Caller.ReturnType = NdType::makeInt(4, false);
    Caller.Params = {{"x", Argument, kNoParamReg, 0}};
    HighStmt If;
    If.Kind = StmtKind::If;
    If.Cond = Not(AtMostZero(Call));
    If.Body = {returnValue(HighExpr::makeConst(1, 4))};
    Caller.Body = {std::move(If), returnValue(HighExpr::makeConst(0, 4))};
    Functions.push_back(std::move(Caller));
    Checks += "    CHECK(" + Name + "(v), " + Meaning + ")\n";
  };
  AddCall("call_s32", NdType::makeInt(4, true), S + " > 0");
  AddCall("call_u32", NdType::makeInt(4, false), S + " > 0");
  AddCall("call_s64", NdType::makeInt(8, true), W + " > 0");
  AddCall("call_u64", NdType::makeInt(8, false), W + " > 0");

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  OS.flush();
  // The readable spelling survives wherever it is correct.
  EXPECT_EQ(C.find("x && x >= 0"), std::string::npos) << C;
  EXPECT_EQ(C.find("!(x >= 0)"), std::string::npos) << C;
  EXPECT_EQ(llvm::StringRef(C).count("if (x > 0) {"), 6u) << C;
  EXPECT_EQ(llvm::StringRef(C).count("if ((int64_t)x > 0) {"), 5u) << C;
  EXPECT_NE(C.find("if (x > 0 && x) {"), std::string::npos) << C;
  const auto Program = C + R"C(
#define CHECK(call, want) \
  if ((uint32_t)(call) != (uint32_t)(want)) return __LINE__ % 200 + 1;
int main(void) {
  const uint64_t values[] = {0, 1, 2, 0x7fffu, 0x8000u, 0xffffu, 0x10000u,
      0x18000u, 0x7fffffffu, 0x80000000u, 0xffffffffu,
      UINT64_C(0x100000000), UINT64_C(0x100000001), UINT64_C(0x180000000),
      UINT64_C(0xffffffff00000001), UINT64_C(0x8000000000000000), ~UINT64_C(0)};
  for (unsigned i = 0; i != sizeof(values) / sizeof(values[0]); ++i) {
    const uint64_t v = values[i];
)C" + Checks + R"C(  }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

} // namespace

TEST(HighSymSimplify, LargeConversionHonorsSymbolicOptInAndKeepsLoads) {
  for (bool Symbolic : {false, true})
    for (bool ReadMemory : {false, true}) {
      SCOPED_TRACE(Symbolic);
      SCOPED_TRACE(ReadMemory);
      FunctionBuilder B;
      const auto X = B.param(0, 0x38);
      const auto Y = ReadMemory
                         ? B.emit(NdOp::LOAD, {MedVar::makeConst(0x8000, 8)})
                         : B.param(1, 0x30);
      const auto And = B.emit(NdOp::INT_AND, {X, Y});
      const auto Or = B.emit(NdOp::INT_OR, {X, Y});
      const auto Sum = B.emit(NdOp::INT_ADD, {And, Or});
      const auto LessX = B.emit(NdOp::INT_SUB, {Sum, X});
      B.makeLarge();
      const auto High = B.finish(NdOp::INT_SUB, {LessX, Y}, Symbolic);
      if (Symbolic)
        EXPECT_EQ(returnedExpr(High), "0");
      else
        EXPECT_NE(returnedExpr(High), "0");
      unsigned Loads = 0;
      walkStmts(High.Body, [&](const HighStmt &S) {
        forEachExpr(S, [&](const ExprPtr &Root) {
          auto Visit = [&](auto &&Self, const ExprPtr &E) -> void {
            if (!E)
              return;
            Loads += E->Kind == ExprKind::Load;
            E->forEachChildExpr(
                [&](const ExprPtr &Child) { Self(Self, Child); });
          };
          Visit(Visit, Root);
        });
      });
      EXPECT_EQ(Loads != 0, ReadMemory);
    }
}

TEST(HighExprSimplify, NamingCapturedReadsCleansTheirDeadValueConsumers) {
  auto Address = HighExpr::makeVar(tempOfSize(1, 8));
  auto Read = HighExpr::makeLoad(Address, NdType::makeInt(4, false));
  auto Value = Read;
  for (unsigned I = 0; I < 12; ++I) {
    Value = HighExpr::makeBinop(NdOp::INT_ADD, Value, Value);
    Value->Type = Read->Type;
  }
  HighFunc F;
  F.ReturnType = Read->Type;
  F.Body = {returnValue(
      HighExpr::makeBinop(NdOp::INT_AND, Value, HighExpr::makeConst(0, 4)))};
  simplifyAllExprs(F.Body);
  ASSERT_NE(F.Body.back().RetVal->Kind, ExprKind::Const);
  ASSERT_TRUE(nameRepeatedValues(F));
  ASSERT_EQ(F.Body.back().RetVal->Kind, ExprKind::Const);
  EXPECT_EQ(F.Body.back().RetVal->ConstVal, 0u);
  // Capturing a read makes its subsequent value disposable, not its memory
  // observation. The access must still execute and can still fault.
  unsigned Reads = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    forEachRhsExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Work{Root};
      HighExprSet Seen;
      while (!Work.empty()) {
        auto E = Work.back();
        Work.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (E->Kind == ExprKind::Load) {
          ++Reads;
          EXPECT_EQ(E->Operands.front()->Var, Address->Var);
        }
        E->forEachChildExpr(
            [&](const ExprPtr &Child) { Work.push_back(Child); });
      }
    });
  });
  EXPECT_GT(Reads, 0u);
}

size_t selectionCount(const ExprPtr &Root) {
  size_t Count = 0;
  std::vector<ExprPtr> Work{Root};
  while (!Work.empty()) {
    auto E = Work.back();
    Work.pop_back();
    Count += E->Kind == ExprKind::BinOp && E->Op == NdOp::SELECT;
    Work.insert(Work.end(), E->Operands.begin(), E->Operands.end());
  }
  return Count;
}

TEST(HighSymSimplify, IntegerBitcastsPreserveWordIdentityThroughTheBridge) {
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (bool Signed : {false, true}) {
      auto X = HighExpr::makeVar(tempOfSize(1, Bytes),
                                 NdType::makeInt(Bytes, Signed));
      auto View = std::make_shared<HighExpr>();
      View->Kind = ExprKind::BitCast;
      View->Type = NdType::makeInt(Bytes, !Signed);
      View->Operands = {X};
      const auto Before = HighExpr::makeBinop(NdOp::INT_XOR, X, View);
      const auto After = simplified(Before);
      ASSERT_EQ(After->Kind, ExprKind::Const) << After->str();
      EXPECT_EQ(After->ConstVal, 0U);
      EXPECT_EQ(View->Type->IsSigned, !Signed);
      EXPECT_EQ(X->Type->IsSigned, Signed);
    }
}

TEST(HighSymSimplify, ModularOperationsReadExactLowPrefixesOfWiderInputs) {
  for (uint16_t Bytes : {1, 2, 4})
    for (NdOp Op : {NdOp::INT_ADD, NdOp::INT_SUB, NdOp::INT_MULT, NdOp::INT_AND,
                    NdOp::INT_OR, NdOp::INT_XOR}) {
      auto X = HighExpr::makeVar(tempOfSize(1, 8));
      auto Y = HighExpr::makeVar(tempOfSize(2, 8));
      auto WideInputs = HighExpr::makeBinop(Op, X, Y);
      WideInputs->Type = NdType::makeInt(Bytes, false);
      auto ExactInputs =
          HighExpr::makeBinop(Op, integerCast(X, Bytes), integerCast(Y, Bytes));
      auto Difference =
          HighExpr::makeBinop(NdOp::INT_XOR, WideInputs, ExactInputs);
      Difference->Type = WideInputs->Type;
      auto After = simplified(Difference);
      ASSERT_EQ(After->Kind, ExprKind::Const) << After->str();
      EXPECT_EQ(After->ConstVal, 0U);
      for (uint64_t A : {0ULL, 1ULL, 255ULL, 0x7fffffffffffffffULL, ~0ULL})
        EXPECT_EQ(integerValue(WideInputs, A, ~A),
                  integerValue(ExactInputs, A, ~A));
    }
}

TEST(HighSymSimplify, PrefixReadingDoesNotChangeShiftOrWideningSemantics) {
  auto X = HighExpr::makeVar(tempOfSize(1, 8));
  for (NdOp Op : {NdOp::INT_LEFT, NdOp::INT_RIGHT, NdOp::INT_ASHR}) {
    auto Shift = HighExpr::makeBinop(Op, X, HighExpr::makeConst(1, 4));
    Shift->Type = NdType::makeInt(4, false);
    auto Narrow =
        HighExpr::makeBinop(Op, integerCast(X, 4), HighExpr::makeConst(1, 4));
    auto Before = HighExpr::makeBinop(NdOp::INT_XOR, Shift, Narrow);
    EXPECT_TRUE(containsNode(simplified(Before), Shift));
  }
  auto Byte = HighExpr::makeVar(tempOfSize(2, 1));
  auto Ambiguous = HighExpr::makeBinop(NdOp::INT_ADD, X, Byte);
  auto Explicit = HighExpr::makeBinop(NdOp::INT_ADD, X, integerCast(Byte, 8));
  auto Before = HighExpr::makeBinop(NdOp::INT_XOR, Ambiguous, Explicit);
  EXPECT_TRUE(containsNode(simplified(Before), Ambiguous));
}

TEST(HighSymSimplify, IntegerBitcastWideningKeepsSignedInterpretation) {
  for (uint16_t Bytes : {1, 2, 4})
    for (bool Signed : {false, true}) {
      auto X = HighExpr::makeVar(tempOfSize(1, Bytes),
                                 NdType::makeInt(Bytes, !Signed));
      auto Y = HighExpr::makeVar(tempOfSize(2, 8));
      auto View = std::make_shared<HighExpr>();
      View->Kind = ExprKind::BitCast;
      View->Type = NdType::makeInt(Bytes, Signed);
      View->Operands = {X};
      auto Before = carrySum(integerCast(View, 8), Y);
      auto After = simplified(Before);
      EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
      for (uint64_t A = 0; A != 256; ++A)
        for (uint64_t Value : {A, ~A})
          EXPECT_EQ(integerValue(After, Value, 0x7fffffffffffffffULL),
                    integerValue(Before, Value, 0x7fffffffffffffffULL));
    }
}

TEST(HighSymSimplify, UsesIncomingDefinitionsOnlyForOneShotConditions) {
  for (StmtKind Kind : {StmtKind::If, StmtKind::IfElse, StmtKind::Switch}) {
    const bool Switch = Kind == StmtKind::Switch;
    auto X = HighExpr::makeVar(tempOfSize(1, 4));
    auto Y = HighExpr::makeVar(tempOfSize(2, 4));
    auto T = HighExpr::makeVar(tempOfSize(3, 4));
    HighStmt Branch;
    Branch.Kind = Kind;
    auto &Condition = Switch ? Branch.SwitchExpr : Branch.Cond;
    Condition = sharedMaskUse(X, Y, T);
    if (Switch) {
      Branch.DefaultBody = {returnValue(sharedMaskUse(X, Y, T))};
    } else {
      Branch.Body = {returnValue(sharedMaskUse(X, Y, T))};
    }
    std::vector<HighStmt> Body{
        assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)), Branch,
        returnValue(sharedMaskUse(X, Y, T))};
    simplifyExprSemantics(Body);
    const auto &Result = Switch ? Body[1].SwitchExpr : Body[1].Cond;
    EXPECT_EQ(Result->str().find('|'), std::string::npos) << Result->str();
    EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos);
    const auto &Child = Switch ? Body[1].DefaultBody : Body[1].Body;
    EXPECT_NE(Child.front().RetVal->str().find('|'), std::string::npos);
    for (uint64_t A = 0; A != 32; ++A)
      for (uint64_t B = 0; B != 32; ++B)
        EXPECT_EQ(integerValue(Result, A, B).getZExtValue(), A + B);

    Branch.Addr = 0x402000;
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = Branch.Addr;
    std::vector<HighStmt> WithLabel{Jump, Body.front(), Branch};
    simplifyExprSemantics(WithLabel);
    const auto &Joined = Switch ? WithLabel[2].SwitchExpr : WithLabel[2].Cond;
    EXPECT_NE(Joined->str().find('|'), std::string::npos);
  }
}

TEST(HighSymSimplify, OneShotPredicateDoesNotRepackFlags) {
  auto X = HighExpr::makeVar(tempOfSize(1, 4));
  auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  auto T = HighExpr::makeVar(tempOfSize(3, 1));
  auto Predicate = HighExpr::makeBinop(
      NdOp::BOOL_AND,
      HighExpr::makeUnary(NdOp::BOOL_NOT,
                          HighExpr::makeBinop(NdOp::INT_LESS, X, Y)),
      HighExpr::makeBinop(NdOp::INT_NOTEQUAL, X, Y));
  auto Packed = HighExpr::makeBinop(
      NdOp::INT_OR, extendInteger(T, 4),
      HighExpr::makeBinop(
          NdOp::INT_MULT, HighExpr::makeConst(64, 4),
          extendInteger(HighExpr::makeUnary(NdOp::BOOL_NOT, T), 4)));
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = HighExpr::makeBinop(
      NdOp::INT_AND, HighExpr::makeConst(1, 4),
      HighExpr::makeBinop(NdOp::INT_RIGHT, Packed, HighExpr::makeConst(6, 1)));
  Branch.Body = {returnValue(HighExpr::makeConst(1, 4))};
  std::vector<HighStmt> Body{assignValue(T, Predicate), Branch};
  simplifyExprSemantics(Body);
  EXPECT_EQ(Body[1].Cond->str().find(" | "), std::string::npos)
      << Body[1].Cond->str();
  EXPECT_EQ(Body[1].Cond->str().find(">>"), std::string::npos)
      << Body[1].Cond->str();
  for (uint64_t A = 0; A != 32; ++A)
    for (uint64_t B = 0; B != 32; ++B) {
      const std::unordered_map<int, uint64_t> Values{
          {1, A}, {2, B}, {3, A > B}};
      EXPECT_EQ(integerValue(Body[1].Cond, 0, 0, &Values).getZExtValue(),
                A <= B);
    }
}

TEST(HighSymSimplify, IntegerBitcastSourceExecutesWithItsSignedWidening) {
  auto Param = [](int Id) {
    auto V = tempOfSize(Id, 8);
    V.Kind = MedVar::Param;
    return HighExpr::makeVar(V, NdType::makeInt(8, false));
  };
  auto X = Param(0), Y = Param(1);
  auto View = std::make_shared<HighExpr>();
  View->Kind = ExprKind::BitCast;
  View->Type = NdType::makeInt(1, true);
  View->Operands = {integerCast(X, 1)};
  HighFunc F;
  F.Name = "bitcast_sum";
  F.Entry = kEntry;
  F.ReturnType = X->Type;
  F.Params = {{"x", X->Type, kNoParamReg, 0}, {"y", Y->Type, kNoParamReg, 1}};
  F.Body = {returnValue(carrySum(integerCast(View, 8), Y))};
  simplifyExprSemantics(F.Body);
  EXPECT_EQ(F.Body.front().RetVal->str().find('^'), std::string::npos);
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  OS.flush();
  const std::string Program = C + R"C(
int main(void) {
  const uint64_t values[] = {0, 1, 0x7fffffffffffffffull, 0x8000000000000000ull,
                            0xffffffffffffffffull};
  for (unsigned x = 0; x != 256; ++x)
    for (unsigned j = 0; j != 5; ++j) {
      int64_t signed_byte = x < 128 ? (int64_t)x : (int64_t)x - 256;
      uint64_t expected = (uint64_t)signed_byte + values[j];
      if (bitcast_sum(x, values[j]) != expected)
        return 1;
    }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighSymSimplify, IntegerPrefixSourceKeepsTruncationBeforeWidening) {
  auto Param = [](int Id) {
    auto V = tempOfSize(Id, 8);
    V.Kind = MedVar::Param;
    return HighExpr::makeVar(V, NdType::makeInt(8, false));
  };
  auto X = Param(0), Y = Param(1);
  auto Narrow = HighExpr::makeBinop(NdOp::INT_ADD, X, Y);
  Narrow->Type = NdType::makeInt(4, false);
  HighFunc F;
  F.Name = "prefix_sum";
  F.Entry = kEntry;
  F.ReturnType = X->Type;
  F.Params = {{"x", X->Type, kNoParamReg, 0}, {"y", Y->Type, kNoParamReg, 1}};
  F.Body = {returnValue(carrySum(integerCast(Narrow, 8), Y))};
  simplifyExprSemantics(F.Body);
  EXPECT_EQ(F.Body.front().RetVal->str().find('^'), std::string::npos);
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  OS.flush();
  const std::string Program = C + R"C(
int main(void) {
  const uint64_t values[] = {0, 1, 0x7fffffffull, 0xffffffffull, 0x100000001ull,
                            0x7fffffffffffffffull, 0xffffffffffffffffull};
  for (unsigned i = 0; i != 7; ++i)
    for (unsigned j = 0; j != 7; ++j) {
      uint64_t expected = (uint64_t)(uint32_t)(values[i] + values[j]) + values[j];
      if (prefix_sum(values[i], values[j]) != expected)
        return 1;
    }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighSymSimplify, WidenedCapturedFlagsRetainTheirAssignmentConversion) {
  auto Param = [](int Id) {
    auto V = tempOfSize(Id, 4);
    V.Kind = MedVar::Param;
    return HighExpr::makeVar(V, NdType::makeInt(4, false));
  };
  auto X = Param(0), Y = Param(1);
  auto Captured = HighExpr::makeVar(tempOfSize(10, 8));
  auto Equal = HighExpr::makeBinop(NdOp::INT_EQUAL, X, Y);
  auto Less = HighExpr::makeBinop(NdOp::INT_LESS, X, Y);
  auto Packed = HighExpr::makeBinop(
      NdOp::INT_OR,
      HighExpr::makeBinop(NdOp::INT_MULT, HighExpr::makeConst(64, 4),
                          integerCast(Equal, 4)),
      integerCast(Less, 4));
  auto Bit = [&](unsigned Shift) {
    auto Field = HighExpr::makeBinop(NdOp::INT_RIGHT, Captured,
                                     HighExpr::makeConst(Shift, 8));
    return HighExpr::makeBinop(
        NdOp::INT_EQUAL,
        HighExpr::makeBinop(NdOp::INT_AND, Field, HighExpr::makeConst(1, 8)),
        HighExpr::makeConst(0, 8));
  };
  HighFunc F;
  F.Name = "widened_order";
  F.Entry = kEntry;
  F.ReturnType = X->Type;
  F.Params = {{"x", X->Type, kNoParamReg, 0}, {"y", Y->Type, kNoParamReg, 1}};
  F.Body = {assignValue(Captured, Packed),
            returnValue(HighExpr::makeBinop(NdOp::BOOL_AND, Bit(6), Bit(0)))};
  simplifyExprSemantics(F.Body);
  EXPECT_FALSE(containsNode(F.Body.back().RetVal, Captured))
      << F.Body.back().RetVal->str();
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  OS.flush();
  const std::string Program = C + R"C(
int main(void) {
  const uint32_t values[] = {0, 1, 39, 64, 255, 0x80000000u, 0xffffffffu};
  for (unsigned i = 0; i != 7; ++i)
    for (unsigned j = 0; j != 7; ++j)
      if (widened_order(values[i], values[j]) != (values[i] > values[j]))
        return 1;
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighSymSimplify, PhiCopyReadsIncomingDefinitionsBeforeRetiringThem) {
  auto X = HighExpr::makeVar(tempOfSize(1, 8));
  auto Y = HighExpr::makeVar(tempOfSize(2, 8));
  auto Flags = HighExpr::makeVar(tempOfSize(3, 8));
  auto Out = HighExpr::makeVar(tempOfSize(4, 4));
  auto Packed = HighExpr::makeBinop(
      NdOp::INT_OR,
      HighExpr::makeBinop(
          NdOp::INT_MULT, HighExpr::makeConst(64, 4),
          integerCast(HighExpr::makeBinop(NdOp::INT_EQUAL, X, Y), 4)),
      integerCast(HighExpr::makeBinop(NdOp::INT_LESS, X, Y), 4));
  auto Bit = [&](unsigned Shift) {
    return HighExpr::makeBinop(
        NdOp::INT_EQUAL,
        HighExpr::makeBinop(NdOp::INT_AND, HighExpr::makeConst(1, 8),
                            HighExpr::makeBinop(NdOp::INT_RIGHT, Flags,
                                                HighExpr::makeConst(Shift, 8))),
        HighExpr::makeConst(0, 8));
  };
  auto Copy = assignValue(
      Out, integerCast(HighExpr::makeBinop(NdOp::INT_AND, Bit(6), Bit(0)), 4));
  Copy.IsPhiCopy = true;
  std::vector<HighStmt> Body{assignValue(Flags, integerCast(Packed, 8)), Copy,
                             returnValue(Bit(0))};
  simplifyExprSemantics(Body);
  const auto &After = Body[1].Val;
  EXPECT_FALSE(containsNode(After, Flags)) << After->str();
  EXPECT_TRUE(Body[1].IsPhiCopy);
  EXPECT_TRUE(containsNode(Body.back().RetVal, Flags));
  const uint64_t Values[] = {0, 1, 63, 64, 255, 0x8000000000000000ULL, ~0ULL};
  for (uint64_t A : Values)
    for (uint64_t B : Values)
      EXPECT_EQ(integerValue(After, A, B).getZExtValue(), A > B);

  auto Labeled = Copy;
  Labeled.Addr = kEntry + 16;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = Labeled.Addr;
  std::vector<HighStmt> Entered{Jump, Body.front(), Labeled};
  simplifyExprSemantics(Entered);
  EXPECT_TRUE(containsNode(Entered.back().Val, Flags));
}

TEST(HighSymSimplify, IntegerSelectionsCarryTheirConditionIntoComparisons) {
  for (uint16_t Bytes : {1, 2, 4, 8}) {
    auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
    auto Select = integerSelect(X, HighExpr::makeConst(17, 4),
                                HighExpr::makeConst(33, 4));
    auto Before = HighExpr::makeBinop(NdOp::INT_EQUAL, Select,
                                      HighExpr::makeConst(17, 4));
    auto After = simplified(Before);
    EXPECT_EQ(selectionCount(After), 0U) << After->str();
    for (uint64_t A :
         {0ULL, 1ULL, 2ULL, 0x80ULL, 0x100ULL, 0x8000000000000000ULL, ~0ULL}) {
      EXPECT_EQ(integerValue(After, A), integerValue(Before, A));
      EXPECT_EQ(integerValue(After, A).getZExtValue(),
                !llvm::APInt(Bytes * 8, A, false, true).isZero());
    }
  }
}

TEST(HighSymSimplify, SelectionBridgePreservesEffectsAndAmbiguousWidths) {
  auto X = HighExpr::makeVar(tempOfSize(1, 4));
  auto Call = HighExpr::makeCall("choose", 0x401000, {});
  Call->Type = NdType::makeInt(8, false);
  auto Before =
      HighExpr::makeBinop(NdOp::INT_EQUAL,
                          integerSelect(Call, HighExpr::makeConst(17, 4),
                                        HighExpr::makeConst(33, 4)),
                          HighExpr::makeConst(17, 4));
  auto After = simplified(Before);
  EXPECT_TRUE(containsNode(After, Call));
  for (uint16_t Bytes : {1, 8}) {
    auto Ambiguous = integerSelect(X, HighExpr::makeConst(17, 4),
                                   HighExpr::makeConst(17, Bytes));
    Before = HighExpr::makeBinop(NdOp::INT_EQUAL, Ambiguous,
                                 HighExpr::makeConst(17, 4));
    After = simplified(Before);
    EXPECT_TRUE(containsNode(After, Ambiguous)) << After->str();
  }
}

TEST(HighSymSimplify, AssignmentDefinitionsPreserveEveryIntegerConversion) {
  for (uint16_t SourceBytes : {1, 2, 4, 8})
    for (uint16_t StoredBytes : {1, 2, 4, 8})
      for (bool SourceSigned : {false, true})
        for (bool StoredSigned : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << SourceBytes << ' ' << StoredBytes << ' '
                       << SourceSigned << ' ' << StoredSigned);
          auto X =
              HighExpr::makeVar(tempOfSize(1, SourceBytes),
                                NdType::makeInt(SourceBytes, SourceSigned));
          auto Stored =
              HighExpr::makeVar(tempOfSize(2, StoredBytes),
                                NdType::makeInt(StoredBytes, StoredSigned));
          auto Expected = integerCast(X, StoredBytes, StoredSigned);
          std::vector<HighStmt> Body{
              assignValue(Stored, X),
              returnValue(HighExpr::makeBinop(NdOp::INT_EQUAL,
                                              integerCast(Stored, 8),
                                              integerCast(Expected, 8)))};
          simplifyExprSemantics(Body);
          const auto &Result = Body.back().RetVal;
          ASSERT_EQ(Result->Kind, ExprKind::Const) << Result->str();
          EXPECT_EQ(Result->ConstVal, 1U);
        }
}

TEST(HighSymSimplify, BooleanInputsReadFullWidthNonzeroTruth) {
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (NdOp Op : {NdOp::BOOL_AND, NdOp::BOOL_OR, NdOp::BOOL_XOR}) {
      auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
      auto Y = HighExpr::makeVar(tempOfSize(2, Bytes));
      auto Truth = [&](const ExprPtr &V) {
        return HighExpr::makeBinop(NdOp::INT_NOTEQUAL, V,
                                   HighExpr::makeConst(0, Bytes));
      };
      auto Before = HighExpr::makeBinop(Op, X, Y);
      auto Explicit = HighExpr::makeBinop(Op, Truth(X), Truth(Y));
      auto Difference =
          simplified(HighExpr::makeBinop(NdOp::INT_XOR, Before, Explicit));
      ASSERT_EQ(Difference->Kind, ExprKind::Const) << Difference->str();
      EXPECT_EQ(Difference->ConstVal, 0u);
      auto After = simplified(Before);
      for (uint64_t A :
           {0ULL, 1ULL, 2ULL, 0x80ULL, 0x100ULL, 0x8000000000000000ULL, ~0ULL})
        for (uint64_t B : {0ULL, 1ULL, 2ULL, 0x80ULL, 0x100ULL,
                           0x8000000000000000ULL, ~0ULL})
          EXPECT_EQ(integerValue(After, A, B), integerValue(Before, A, B));
    }
}

TEST(HighSymSimplify, ExpandedByteParityKeepsEveryBitAndItsInputWidth) {
  for (const uint16_t Bytes : {1, 4, 8})
    for (const bool Corrupt : {false, true}) {
      auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
      auto Word =
          HighExpr::makeBinop(NdOp::INT_ADD, X, HighExpr::makeConst(73, Bytes));
      ExprPtr Fold = HighExpr::makeConst(0, Bytes);
      for (unsigned Bit = 0; Bit < 8; ++Bit) {
        const auto SourceBit = Corrupt && Bit == 3 ? 2 : Bit;
        auto Part = HighExpr::makeBinop(
            NdOp::INT_AND,
            HighExpr::makeBinop(NdOp::INT_RIGHT, Word,
                                HighExpr::makeConst(SourceBit, 1)),
            HighExpr::makeConst(1, Bytes));
        Fold = HighExpr::makeBinop(NdOp::INT_XOR, Fold, Part);
      }
      const auto Before = HighExpr::makeBinop(NdOp::INT_EQUAL, Fold,
                                              HighExpr::makeConst(0, Bytes));
      const auto After = simplified(Before);
      bool HasPopcount = false;
      std::vector<ExprPtr> Work{After};
      while (!Work.empty()) {
        const auto E = Work.back();
        Work.pop_back();
        HasPopcount |= E->Kind == ExprKind::UnaryOp && E->Op == NdOp::POPCOUNT;
        Work.insert(Work.end(), E->Operands.begin(), E->Operands.end());
      }
      EXPECT_EQ(HasPopcount, !Corrupt) << After->str();
      for (unsigned Value = 0; Value < 256; ++Value) {
        EXPECT_EQ(integerValue(After, Value), integerValue(Before, Value));
        EXPECT_EQ(integerValue(After, ~uint64_t(Value)),
                  integerValue(Before, ~uint64_t(Value)));
      }
    }
}

TEST(HighSymSimplify, RecoveredParitySourceCountsOnlyItsUnsignedByte) {
  auto Parameter = [](int Id) {
    auto V = tempOfSize(Id, 1);
    V.Kind = MedVar::Param;
    return HighExpr::makeVar(V, NdType::makeInt(1, true));
  };
  const auto X = Parameter(0), Y = Parameter(1);
  const auto Byte = HighExpr::makeBinop(NdOp::INT_SUB, X, Y);
  auto Sum = HighExpr::makeConst(0, 1);
  for (unsigned Bit = 0; Bit < 8; ++Bit)
    Sum = HighExpr::makeBinop(
        NdOp::INT_ADD, Sum,
        HighExpr::makeBinop(NdOp::INT_AND,
                            HighExpr::makeBinop(NdOp::INT_RIGHT, Byte,
                                                HighExpr::makeConst(Bit, 1)),
                            HighExpr::makeConst(1, 1)));
  HighFunc F;
  F.Name = "byte_parity_count";
  F.Entry = kEntry;
  F.ReturnType = NdType::makeInt(1, false);
  F.Params = {{"x", X->Type, kNoParamReg, 0}, {"y", Y->Type, kNoParamReg, 1}};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeBinop(
      NdOp::INT_EQUAL,
      HighExpr::makeBinop(NdOp::INT_AND, Sum, HighExpr::makeConst(1, 1)),
      HighExpr::makeConst(0, 1));
  F.Body.push_back(std::move(Return));
  simplifyExprSemantics(F.Body);
  std::string C;
  llvm::raw_string_ostream Out(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  Out.flush();
  ASSERT_NE(C.find("__builtin_popcount"), std::string::npos) << C;
  const auto Program = C + R"C(
int main(void) {
  for (unsigned x = 0; x < 256; ++x)
    for (unsigned y = 0; y < 256; ++y) {
      unsigned byte = (x - y) & 255u, odd = 0;
      for (unsigned bit = 0; bit < 8; ++bit)
        odd ^= (byte >> bit) & 1u;
      if (byte_parity_count((int8_t)x, (int8_t)y) != !odd)
        return 1;
    }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighExprSimplify, SharedObservableSeedsDoNotSpendBudgetPerRoot) {
  for (const bool IndirectCall : {false, true}) {
    SCOPED_TRACE(IndirectCall);
    auto Unread = HighExpr::makeUndef(8);
    auto Address = HighExpr::makeUndef(8);
    auto Effect = HighExpr::makeLoad(Address, Unread->Type);
    if (IndirectCall) {
      Effect = HighExpr::makeCall("indirect", 0, {});
      Effect->IsIndirectCall = true;
      Effect->IndirectTarget = Address;
      Effect->Type = Unread->Type;
    }
    auto Shared = HighExpr::makeBinop(NdOp::INT_OR, Unread, Effect);
    const auto Zero = HighExpr::makeConst(0, 8);
    for (unsigned I = 0; I < 512; ++I)
      Shared = HighExpr::makeBinop(NdOp::INT_XOR, Shared, Zero);
    HighFunc F;
    for (unsigned I = 0; I < 4096; ++I)
      F.Body.push_back(
          assignValue(HighExpr::makeVar(tempOfSize(I, 8)), Shared));
    // None of these temporary results is read, but the memory address or
    // indirect target is observed at every evaluation of the shared graph.
    zeroUndemandedUnknownBytes(F, Arch::X64);
    EXPECT_FALSE(containsNode(F.Body.front().Val, Unread));
    EXPECT_TRUE(containsNode(F.Body.front().Val, Effect));
    EXPECT_EQ(IndirectCall ? Effect->IndirectTarget : Effect->Operands.front(),
              Address);
    EXPECT_EQ(Address->Kind, ExprKind::Undef);
    for (const auto &Stmt : F.Body)
      EXPECT_EQ(Stmt.Val, Shared);
  }
}

TEST(HighExprSimplify, SharedSeedsStillDemandEachRootsValue) {
  for (const bool Reverse : {false, true}) {
    auto Unknown = HighExpr::makeUndef(8);
    auto Shared =
        HighExpr::makeBinop(NdOp::INT_OR, Unknown, HighExpr::makeConst(0, 8));
    auto Unread = assignValue(HighExpr::makeVar(tempOfSize(1, 8)), Shared);
    auto Read = returnValue(Shared);
    HighFunc F;
    F.ReturnType = Unknown->Type;
    F.Body = Reverse ? std::vector<HighStmt>{Read, Unread}
                     : std::vector<HighStmt>{Unread, Read};
    zeroUndemandedUnknownBytes(F, Arch::X64);
    EXPECT_TRUE(containsNode(Shared, Unknown));
    EXPECT_EQ(Unknown->Kind, ExprKind::Undef);
  }
}

TEST(HighExprSimplify, ByteDemandKeepsBothSidesOfTheInlineMaskBoundary) {
  for (uint16_t Bytes : {8, 16, 64})
    for (bool ReadHigh : {false, true}) {
      auto Low = HighExpr::makeUndef(Bytes / 2);
      auto High = HighExpr::makeUndef(Bytes / 2);
      auto Joined = HighExpr::makeBinop(NdOp::CONCAT, High, Low);
      Joined->Type = NdType::makeInt(Bytes, false);
      auto Read =
          HighExpr::makeBinop(NdOp::SUBBYTES, Joined,
                              HighExpr::makeConst(ReadHigh ? Bytes - 4 : 0, 4));
      Read->Type = NdType::makeInt(4, false);
      HighFunc F;
      F.ReturnType = Read->Type;
      F.Body = {returnValue(Read)};
      zeroUndemandedUnknownBytes(F, Arch::X64);
      EXPECT_TRUE(containsNode(F.Body.front().RetVal, ReadHigh ? High : Low));
      EXPECT_FALSE(containsNode(F.Body.front().RetVal, ReadHigh ? Low : High));
    }
}

TEST(HighExprSimplify, SharedObservableSeedsTerminateCycles) {
  auto Address = HighExpr::makeUndef(8);
  const auto Load = HighExpr::makeLoad(Address, Address->Type);
  auto Cycle =
      HighExpr::makeBinop(NdOp::INT_OR, HighExpr::makeConst(0, 8), Load);
  Cycle->Operands.front() = Cycle;
  HighFunc F;
  F.Body = {assignValue(HighExpr::makeVar(tempOfSize(1, 8)), Cycle)};
  zeroUndemandedUnknownBytes(F, Arch::X64);
  EXPECT_EQ(Load->Operands.front(), Address);
  EXPECT_EQ(Address->Kind, ExprKind::Undef);
  Cycle->Operands.front().reset();
}

TEST(HighExprSimplify, CrossDefinitionFlagDemandDoesNotKeepTheWholeByte) {
  auto X = HighExpr::makeVar(tempOfSize(1, 4));
  auto Noise = HighExpr::makeVar(tempOfSize(2, 4));
  auto Flags = HighExpr::makeVar(tempOfSize(3, 4));
  auto Copy = HighExpr::makeVar(tempOfSize(4, 4));
  auto Packed = HighExpr::makeBinop(
      NdOp::INT_OR,
      HighExpr::makeBinop(NdOp::INT_AND, X, HighExpr::makeConst(0x41, 4)),
      HighExpr::makeBinop(NdOp::INT_AND, Noise,
                          HighExpr::makeConst(~uint64_t(0x41), 4)));
  const auto Text = Packed->str();
  HighFunc F;
  F.ReturnType = NdType::makeInt(4, false);
  F.Body = {assignValue(Flags, Packed), assignValue(Copy, Flags),
            returnValue(HighExpr::makeBinop(NdOp::INT_AND, Copy,
                                            HighExpr::makeConst(0x41, 4)))};
  zeroUndemandedUnknownBytes(F, Arch::X64);
  EXPECT_FALSE(containsNode(F.Body[0].Val, Noise)) << F.Body[0].Val->str();
  EXPECT_EQ(Packed->str(), Text);
  for (uint64_t A = 0; A != 256; ++A)
    EXPECT_EQ(integerValue(F.Body[0].Val, A).getZExtValue() & 0x41, A & 0x41);
}

TEST(HighExprSimplify, ScaledCapturedPredicateDoesNotKeepUnobservedLowBits) {
  for (bool Swap : {false, true}) {
    auto X = HighExpr::makeVar(tempOfSize(1, 4));
    auto Noise = HighExpr::makeVar(tempOfSize(2, 1));
    auto Predicate = HighExpr::makeVar(tempOfSize(3, 1));
    auto Flags = HighExpr::makeVar(tempOfSize(4, 4));
    auto Test = HighExpr::makeUnary(NdOp::BOOL_NOT, Noise);
    auto Wide = extendInteger(Predicate, 4);
    auto Scale = HighExpr::makeConst(4, 4);
    auto Packed = HighExpr::makeBinop(NdOp::INT_OR, X,
                                      HighExpr::makeBinop(NdOp::INT_MULT,
                                                          Swap ? Scale : Wide,
                                                          Swap ? Wide : Scale));
    HighFunc F;
    F.ReturnType = NdType::makeInt(4, false);
    F.Body = {assignValue(Predicate, Test), assignValue(Flags, Packed),
              returnValue(HighExpr::makeBinop(
                  NdOp::INT_AND,
                  HighExpr::makeBinop(NdOp::INT_RIGHT, Flags,
                                      HighExpr::makeConst(8, 1)),
                  HighExpr::makeConst(0x77, 4)))};
    zeroUndemandedUnknownBytes(F, Arch::X64);
    EXPECT_FALSE(containsNode(F.Body[0].Val, Noise)) << F.Body[0].Val->str();
    EXPECT_EQ(F.Body[0].Val->Kind, ExprKind::Const);
    EXPECT_EQ(F.Body[0].Val->ConstVal, 0u);
    EXPECT_TRUE(containsNode(Test, Noise));

    // A second reader observes the actual predicate and must keep its input.
    F.Body[0].Val = Test;
    F.Body[1].Val = Packed;
    F.Body.push_back(returnValue(
        HighExpr::makeBinop(NdOp::INT_AND, Flags, HighExpr::makeConst(4, 4))));
    zeroUndemandedUnknownBytes(F, Arch::X64);
    EXPECT_TRUE(containsNode(F.Body[0].Val, Noise));
  }
}

TEST(HighExprSimplify, ScaledBitDemandKeepsImplicitPromotionsAndEffects) {
  auto X = HighExpr::makeVar(tempOfSize(1, 1), NdType::makeInt(1, true));
  auto Narrow = HighExpr::makeVar(tempOfSize(2, 1), X->Type);
  auto Wide = HighExpr::makeVar(tempOfSize(3, 4), NdType::makeInt(4, false));
  auto Product =
      HighExpr::makeBinop(NdOp::INT_MULT, Narrow, HighExpr::makeConst(4, 4));
  Product->Type = Wide->Type;
  HighFunc F;
  F.ReturnType = Wide->Type;
  F.Body = {assignValue(Narrow, X), assignValue(Wide, Product),
            returnValue(HighExpr::makeBinop(NdOp::INT_RIGHT, Wide,
                                            HighExpr::makeConst(16, 1)))};
  zeroUndemandedUnknownBytes(F, Arch::X64);
  EXPECT_EQ(F.Body[0].Val, X);

  for (bool Load : {false, true}) {
    auto Predicate = HighExpr::makeVar(tempOfSize(4, 1));
    auto Flags = HighExpr::makeVar(tempOfSize(5, 4));
    auto Effect = Load ? HighExpr::makeLoad(HighExpr::makeConst(0x1000, 8),
                                            NdType::makeInt(1, false))
                       : HighExpr::makeCall("observe", 0x401000, {});
    Effect->Type = NdType::makeInt(1, false);
    auto Test = HighExpr::makeUnary(NdOp::BOOL_NOT, Effect);
    F.Body = {
        assignValue(Predicate, Test),
        assignValue(Flags, HighExpr::makeBinop(NdOp::INT_MULT,
                                               extendInteger(Predicate, 4),
                                               HighExpr::makeConst(4, 4))),
        returnValue(HighExpr::makeBinop(NdOp::INT_RIGHT, Flags,
                                        HighExpr::makeConst(8, 1)))};
    zeroUndemandedUnknownBytes(F, Arch::X64);
    EXPECT_TRUE(containsNode(F.Body[0].Val, Effect));
  }
}

TEST(HighExprSimplify, ScaledPredicateSourceRetainsItsProviderInvocation) {
  auto V = tempOfSize(0, 4);
  V.Kind = MedVar::Param;
  auto X = HighExpr::makeVar(V, NdType::makeInt(4, false));
  auto Predicate = HighExpr::makeVar(tempOfSize(1, 1));
  auto Flags = HighExpr::makeVar(tempOfSize(2, 4));
  auto Call = HighExpr::makeCall("next_value", 0x401000, {});
  Call->Type = NdType::makeInt(4, true);
  auto Test = HighExpr::makeUnary(NdOp::BOOL_NOT, Call);
  Test->Type = Predicate->Type;
  HighFunc F;
  F.Name = "high_fields";
  F.Entry = kEntry;
  F.ReturnType = X->Type;
  F.Params = {{"x", X->Type, kNoParamReg, 0}};
  F.Body = {
      assignValue(Predicate, Test),
      assignValue(Flags, HighExpr::makeBinop(
                             NdOp::INT_OR, X,
                             HighExpr::makeBinop(NdOp::INT_MULT,
                                                 extendInteger(Predicate, 4),
                                                 HighExpr::makeConst(4, 4)))),
      returnValue(
          HighExpr::makeBinop(NdOp::INT_AND,
                              HighExpr::makeBinop(NdOp::INT_RIGHT, Flags,
                                                  HighExpr::makeConst(8, 1)),
                              HighExpr::makeConst(0x77, 4)))};
  zeroUndemandedUnknownBytes(F, Arch::X64);
  EXPECT_TRUE(containsNode(F.Body.front().Val, Call));
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  OS.flush();
  const std::string Program = C + R"C(
static unsigned calls;
static uint32_t noise;
int next_value(void) { ++calls; return noise; }
int main(void) {
  const uint32_t values[] = {0, 1, 4, 256, 65536, 0x80000000u, 0xffffffffu};
  for (noise = 0; noise != 256; ++noise)
    for (unsigned i = 0; i != 7; ++i) {
      calls = 0;
      if (high_fields(values[i]) != ((values[i] >> 8) & 0x77) || calls != 1)
        return 1;
    }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighExprSimplify, CrossDefinitionDemandIncludesEveryReaderAndLoopArrival) {
  auto X = HighExpr::makeVar(tempOfSize(1, 4));
  auto Noise = HighExpr::makeVar(tempOfSize(2, 4));
  auto Flags = HighExpr::makeVar(tempOfSize(3, 4));
  auto Packed = HighExpr::makeBinop(
      NdOp::INT_OR,
      HighExpr::makeBinop(NdOp::INT_AND, X, HighExpr::makeConst(0x41, 4)),
      HighExpr::makeBinop(NdOp::INT_AND, Noise, HighExpr::makeConst(4, 4)));
  HighFunc F;
  F.ReturnType = NdType::makeInt(4, false);
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, Flags, HighExpr::makeConst(4, 4));
  Loop.Body = {assignValue(Flags, Packed)};
  F.Body = {assignValue(Flags, Packed), Loop,
            returnValue(HighExpr::makeBinop(NdOp::INT_AND, Flags,
                                            HighExpr::makeConst(0x41, 4)))};
  zeroUndemandedUnknownBytes(F, Arch::X64);
  EXPECT_TRUE(containsNode(F.Body[0].Val, Noise));
  EXPECT_TRUE(containsNode(F.Body[1].Body[0].Val, Noise));

  F.Body.push_back(returnValue(Flags));
  const auto Before = F.Body[0].Val;
  zeroUndemandedUnknownBytes(F, Arch::X64);
  EXPECT_EQ(F.Body[0].Val, Before);
}

TEST(HighExprSimplify, BitDemandUnifiesAllRenamedViewsOfTheSameLocal) {
  auto X = HighExpr::makeVar(tempOfSize(1, 4));
  auto Noise = HighExpr::makeVar(tempOfSize(2, 4));
  auto Defined = HighExpr::makeVar(tempOfSize(3, 4));
  auto Alias = HighExpr::makeVar(tempOfSize(93, 4));
  Defined->Var.RenameTag = Alias->Var.RenameTag = 12;
  auto Packed = HighExpr::makeBinop(
      NdOp::INT_OR,
      HighExpr::makeBinop(NdOp::INT_AND, X, HighExpr::makeConst(0x41, 4)),
      HighExpr::makeBinop(NdOp::INT_AND, Noise, HighExpr::makeConst(4, 4)));
  HighFunc F;
  F.ReturnType = NdType::makeInt(4, false);
  F.Body = {assignValue(Defined, Packed),
            returnValue(HighExpr::makeBinop(
                NdOp::INT_OR,
                HighExpr::makeBinop(NdOp::INT_AND, Defined,
                                    HighExpr::makeConst(1, 4)),
                HighExpr::makeBinop(NdOp::INT_AND, Alias,
                                    HighExpr::makeConst(64, 4))))};
  zeroUndemandedUnknownBytes(F, Arch::X64);
  EXPECT_FALSE(containsNode(F.Body[0].Val, Noise));
  for (uint64_t A = 0; A != 256; ++A)
    EXPECT_EQ(integerValue(F.Body[0].Val, A).getZExtValue() & 0x41, A & 0x41);
}

TEST(HighExprSimplify, CrossDefinitionBitProjectionKeepsLoadsAndDivision) {
  for (bool Load : {false, true}) {
    auto X = HighExpr::makeVar(tempOfSize(1, 4));
    auto T = HighExpr::makeVar(tempOfSize(2, 4));
    auto Effect = std::make_shared<HighExpr>();
    Effect->Type = NdType::makeInt(4, false);
    if (Load) {
      Effect->Kind = ExprKind::Load;
      Effect->Operands = {HighExpr::makeConst(0x1000, 8)};
    } else {
      Effect = HighExpr::makeBinop(NdOp::INT_DIV, HighExpr::makeConst(1, 4), X);
    }
    auto Packed = HighExpr::makeBinop(
        NdOp::INT_OR, X,
        HighExpr::makeBinop(NdOp::INT_LEFT, Effect, HighExpr::makeConst(8, 1)));
    HighFunc F;
    F.ReturnType = NdType::makeInt(4, false);
    F.Body = {assignValue(T, Packed),
              returnValue(HighExpr::makeBinop(NdOp::INT_AND, T,
                                              HighExpr::makeConst(1, 4)))};
    zeroUndemandedUnknownBytes(F, Arch::X64);
    EXPECT_TRUE(containsNode(F.Body[0].Val, Effect));
  }
}

TEST(HighExprSimplify, BitDemandRetainsImplicitAssignmentConversions) {
  auto X = HighExpr::makeVar(tempOfSize(1, 4));
  auto Narrow = HighExpr::makeVar(tempOfSize(2, 4), NdType::makeInt(4, true));
  auto Wide = HighExpr::makeVar(tempOfSize(3, 8), NdType::makeInt(8, true));
  auto Sign =
      HighExpr::makeBinop(NdOp::INT_AND, X, HighExpr::makeConst(0x80000000, 4));
  Sign->Type = Narrow->Type;
  HighFunc F;
  F.ReturnType = NdType::makeInt(8, true);
  F.Body = {assignValue(Narrow, Sign), assignValue(Wide, Narrow),
            returnValue(HighExpr::makeBinop(NdOp::INT_RIGHT, Wide,
                                            HighExpr::makeConst(32, 1)))};
  zeroUndemandedUnknownBytes(F, Arch::X64);
  EXPECT_EQ(F.Body[0].Val, Sign);
  EXPECT_EQ(F.Body[1].Val, Narrow);
  EXPECT_TRUE(containsNode(F.Body[0].Val, X));
}

TEST(HighExprSimplify, PackedPredicatesKeepOnlyTheirOwnObservedBit) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto Predicate = HighExpr::makeBinop(NdOp::INT_LESS, X, Y);
  for (unsigned Bit = 0; Bit != 32; ++Bit) {
    auto Flag = HighExpr::makeBinop(NdOp::INT_MULT,
                                    HighExpr::makeConst(uint64_t{1} << Bit, 4),
                                    extendInteger(Predicate, 4));
    auto After = basicSimplified(projectHighIntegerBits(Flag, 0x41));
    if (Bit != 0 && Bit != 6)
      EXPECT_FALSE(containsNode(After, X)) << After->str();
    for (uint64_t A = 0; A != 32; ++A)
      for (uint64_t B = 0; B != 32; ++B)
        EXPECT_EQ(integerValue(After, A, B).getZExtValue() & 0x41,
                  (A < B ? uint64_t{1} << Bit : 0) & 0x41);
  }

  auto Call = std::make_shared<HighExpr>();
  Call->Kind = ExprKind::Call;
  Call->CallTarget = "observe";
  Call->Type = NdType::makeInt(4, false);
  auto Observed = HighExpr::makeBinop(NdOp::INT_EQUAL, Call, X);
  auto Flag = HighExpr::makeBinop(NdOp::INT_MULT, HighExpr::makeConst(4, 4),
                                  extendInteger(Observed, 4));
  EXPECT_TRUE(containsNode(projectHighIntegerBits(Flag, 0x41), Call));
}

TEST(HighExprSimplify, EqualityWithMaskedCapturedWordReadsOnlyChangedBits) {
  for (uint16_t Bytes : {1, 2, 4, 8}) {
    auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
    for (NdOp MaskOp : {NdOp::INT_AND, NdOp::INT_OR})
      for (NdOp Compare : {NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL})
        for (uint64_t Mask :
             {0ull, 1ull, 0x41ull, 0x80ull, 0xfffffffeull, ~0ull}) {
          auto Selected =
              HighExpr::makeBinop(MaskOp, X, HighExpr::makeConst(Mask, Bytes));
          auto Before = HighExpr::makeBinop(Compare, X, Selected);
          auto After = basicSimplified(Before);
          for (uint64_t A = 0; A != 256; ++A) {
            EXPECT_EQ(integerValue(After, A), integerValue(Before, A));
            EXPECT_EQ(integerValue(After, ~A), integerValue(Before, ~A));
          }
        }
  }
  auto X = HighExpr::makeVar(tempOfSize(1, 4));
  auto Flags = HighExpr::makeVar(tempOfSize(2, 4));
  auto Noise = HighExpr::makeVar(tempOfSize(3, 4));
  auto Packed = HighExpr::makeBinop(
      NdOp::INT_OR,
      HighExpr::makeBinop(NdOp::INT_AND, X, HighExpr::makeConst(1, 4)),
      HighExpr::makeBinop(NdOp::INT_AND, Noise,
                          HighExpr::makeConst(0xfffffffe, 4)));
  HighFunc F;
  F.ReturnType = NdType::makeInt(1, false);
  F.Body = {assignValue(Flags, Packed),
            returnValue(HighExpr::makeBinop(
                NdOp::INT_EQUAL, Flags,
                HighExpr::makeBinop(NdOp::INT_AND, Flags,
                                    HighExpr::makeConst(0xfffffffe, 4))))};
  simplifyAllExprs(F.Body);
  zeroUndemandedUnknownBytes(F, Arch::X64);
  EXPECT_FALSE(containsNode(F.Body[0].Val, Noise));

  auto Load = std::make_shared<HighExpr>();
  Load->Kind = ExprKind::Load;
  Load->Type = NdType::makeInt(4, false);
  Load->Operands = {HighExpr::makeConst(0x1000, 8)};
  auto Before = HighExpr::makeBinop(
      NdOp::INT_EQUAL, Load,
      HighExpr::makeBinop(NdOp::INT_AND, Load,
                          HighExpr::makeConst(0xfffffffe, 4)));
  auto After = basicSimplified(Before);
  EXPECT_EQ(After->Operands[0], Load);
  EXPECT_TRUE(containsNode(After->Operands[1], Load));
}

TEST(HighExprSimplify,
     CrossDefinitionMasksProjectSelectionsAndKeepTheirCondition) {
  auto X = HighExpr::makeVar(tempOfSize(1, 4));
  auto Noise = HighExpr::makeVar(tempOfSize(2, 4));
  auto T = HighExpr::makeVar(tempOfSize(3, 4));
  auto Condition = std::make_shared<HighExpr>();
  Condition->Kind = ExprKind::Call;
  Condition->CallTarget = "choose";
  Condition->Type = NdType::makeInt(1, false);
  auto Select = std::make_shared<HighExpr>();
  Select->Kind = ExprKind::BinOp;
  Select->Op = NdOp::SELECT;
  Select->Type = NdType::makeInt(4, false);
  auto Padding =
      HighExpr::makeBinop(NdOp::INT_LEFT, Noise, HighExpr::makeConst(8, 1));
  Select->Operands = {Condition, HighExpr::makeBinop(NdOp::INT_OR, X, Padding),
                      HighExpr::makeBinop(NdOp::INT_XOR, X, Padding)};
  HighFunc F;
  F.ReturnType = NdType::makeInt(4, false);
  F.Body = {assignValue(T, Select),
            returnValue(HighExpr::makeBinop(NdOp::INT_AND, T,
                                            HighExpr::makeConst(0x41, 4)))};
  zeroUndemandedUnknownBytes(F, Arch::X64);
  EXPECT_FALSE(containsNode(F.Body[0].Val, Noise));
  EXPECT_TRUE(containsNode(F.Body[0].Val, Condition));
  EXPECT_TRUE(containsNode(Select, Noise));
}

TEST(HighExprSimplify, ProjectedFlagSourceExecutesWithItsOriginalCall) {
  auto Param = [](int Id) {
    auto V = tempOfSize(Id, 4);
    V.Kind = MedVar::Param;
    return HighExpr::makeVar(V, NdType::makeInt(4, false));
  };
  auto X = Param(0), Y = Param(1);
  auto T = HighExpr::makeVar(tempOfSize(10, 4));
  auto CF = HighExpr::makeBinop(NdOp::INT_LESS, X, Y);
  auto ZF = HighExpr::makeBinop(
      NdOp::INT_MULT, HighExpr::makeConst(64, 4),
      extendInteger(HighExpr::makeBinop(NdOp::INT_EQUAL, X, Y), 4));
  auto PF = HighExpr::makeBinop(
      NdOp::INT_MULT, HighExpr::makeConst(4, 4),
      extendInteger(
          HighExpr::makeBinop(NdOp::INT_EQUAL, Y, HighExpr::makeConst(17, 4)),
          4));
  auto Packed = HighExpr::makeBinop(NdOp::INT_OR, extendInteger(CF, 4),
                                    HighExpr::makeBinop(NdOp::INT_OR, ZF, PF));
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall("observe", 0x401000, {});
  Call.CallExpr->Type = NdType::makeVoid();
  HighFunc F;
  F.Name = "flag_test";
  F.Entry = kEntry;
  F.ReturnType = NdType::makeInt(4, false);
  F.Params = {{"x", X->Type, kNoParamReg, 0}, {"y", Y->Type, kNoParamReg, 1}};
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = HighExpr::makeBinop(
      NdOp::INT_EQUAL,
      HighExpr::makeBinop(NdOp::INT_AND, T, HighExpr::makeConst(0x41, 4)),
      HighExpr::makeConst(0, 4));
  Branch.Body = {returnValue(HighExpr::makeConst(1, 4))};
  F.Body = {assignValue(T, Packed), Call, Branch,
            returnValue(HighExpr::makeConst(0, 4))};
  zeroUndemandedUnknownBytes(F, Arch::X64);
  simplifyAllExprs(F.Body);
  simplifyExprSemantics(F.Body);
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  OS.flush();
  const std::string Program = C + R"C(
static unsigned calls;
int observe(void) { ++calls; return 0; }
int main(void) {
  const uint32_t values[] = {0, 1, 16, 17, 18, 255, 0x80000000u, 0xffffffffu};
  for (unsigned i = 0; i != 8; ++i)
    for (unsigned j = 0; j != 8; ++j) {
      calls = 0;
      if (flag_test(values[i], values[j]) != (values[i] > values[j]) || calls != 1)
        return 1;
    }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(HighExprSimplify, DiscardsDeepPureValuesWithoutKeepingTheirLocalsLive) {
  for (NdOp Op : {NdOp::INT_AND, NdOp::INT_MULT}) {
    auto Value = HighExpr::makeVar(tempOfSize(1, kWordBytes));
    for (int I = 2; I < 258; ++I)
      Value = HighExpr::makeBinop(NdOp::INT_XOR, Value,
                                  HighExpr::makeVar(tempOfSize(I, kWordBytes)));
    std::vector<HighStmt> Body{returnValue(
        HighExpr::makeBinop(Op, Value, HighExpr::makeConst(0, kWordBytes)))};
    simplifyAllExprs(Body);
    ASSERT_EQ(Body.front().RetVal->Kind, ExprKind::Const);
    EXPECT_EQ(Body.front().RetVal->ConstVal, 0U);
    EXPECT_EQ(Body.front().RetVal->Type->Size, kWordBytes);
  }
}

TEST(HighExprSimplify, DeepZeroMasksKeepObservationsAndPossibleTraps) {
  auto X = HighExpr::makeVar(tempOfSize(1, kWordBytes));
  auto Call = HighExpr::makeCall("observe", kEntry, {X});
  Call->Type = X->Type;
  auto Load = HighExpr::makeLoad(X, X->Type);
  auto Divide = HighExpr::makeBinop(NdOp::INT_DIV, X, X);
  auto Ordered = HighExpr::makeUnary(NdOp::INT_NOT, X);
  Ordered->MemoryOrdering = NdMemoryOrdering::Acquire;
  for (const auto &Effect : {Call, Load, Divide, Ordered}) {
    for (NdOp Op : {NdOp::INT_AND, NdOp::INT_MULT}) {
      auto Value = Effect;
      for (int I = 2; I < 258; ++I)
        Value = HighExpr::makeBinop(
            NdOp::INT_XOR, Value, HighExpr::makeVar(tempOfSize(I, kWordBytes)));
      std::vector<HighStmt> Body{returnValue(
          HighExpr::makeBinop(Op, Value, HighExpr::makeConst(0, kWordBytes)))};
      simplifyAllExprs(Body);
      EXPECT_TRUE(containsNode(Body.front().RetVal, Effect));
    }
  }
}

TEST(HighExprSimplify, ZeroMasksDiscardOnlyPureCompleteSelections) {
  auto X = HighExpr::makeVar(tempOfSize(1, kWordBytes));
  auto Call = HighExpr::makeCall("observe", kEntry, {X});
  Call->Type = X->Type;
  auto Divide = HighExpr::makeBinop(NdOp::INT_DIV, X, X);
  const auto Select = [&](const ExprPtr &Condition, const ExprPtr &Then,
                          const ExprPtr &Else) {
    auto E = std::make_shared<HighExpr>();
    E->Kind = ExprKind::BinOp;
    E->Op = NdOp::SELECT;
    E->Type = X->Type;
    E->Operands = {Condition, Then, Else};
    return E;
  };
  for (NdOp Op : {NdOp::INT_AND, NdOp::INT_MULT}) {
    auto Value = Select(X, HighExpr::makeConst(17, kWordBytes), X);
    auto After = basicSimplified(
        HighExpr::makeBinop(Op, Value, HighExpr::makeConst(0, kWordBytes)));
    ASSERT_EQ(After->Kind, ExprKind::Const);
    EXPECT_EQ(After->ConstVal, 0U);
    for (const auto &Effect : {Call, Divide})
      for (unsigned I = 0; I < 3; ++I) {
        auto Inputs = std::vector<ExprPtr>{X, X, X};
        Inputs[I] = Effect;
        auto Kept = basicSimplified(
            HighExpr::makeBinop(Op, Select(Inputs[0], Inputs[1], Inputs[2]),
                                HighExpr::makeConst(0, kWordBytes)));
        EXPECT_TRUE(containsNode(Kept, Effect));
      }
    auto Malformed = Select(X, X, HighExpr::makeConst(17, 1));
    After = basicSimplified(
        HighExpr::makeBinop(Op, Malformed, HighExpr::makeConst(0, kWordBytes)));
    EXPECT_TRUE(containsNode(After, Malformed));
  }
}

ExprPtr selectedBytes(const ExprPtr &Condition, const ExprPtr &Yes,
                      const ExprPtr &No, unsigned YesOffset,
                      uint16_t ResultBytes, NdOp Combine,
                      bool SignExtend = false) {
  ExprPtr Result;
  for (unsigned I = 0; I != 4; ++I) {
    auto Slice = [&](const ExprPtr &Source, unsigned Offset) {
      auto E = HighExpr::makeBinop(NdOp::SUBBYTES, Source,
                                   HighExpr::makeConst(Offset, 4));
      E->Type = NdType::makeInt(1, true);
      return E;
    };
    auto Select = std::make_shared<HighExpr>();
    Select->Kind = ExprKind::BinOp;
    Select->Op = NdOp::SELECT;
    Select->Type = NdType::makeInt(1, true);
    Select->Operands = {Condition, Slice(Yes, YesOffset + I), Slice(No, I)};
    auto Piece = extendInteger(Select, ResultBytes, SignExtend);
    if (I)
      Piece = HighExpr::makeBinop(NdOp::INT_LEFT, Piece,
                                  HighExpr::makeConst(8 * I, ResultBytes));
    Result = Result ? HighExpr::makeBinop(Combine, Result, Piece) : Piece;
  }
  return Result;
}

TEST(HighExprSimplify, AdjacentSelectedBytesRecoverOneWordSelection) {
  auto Condition = HighExpr::makeVar(tempOfSize(0, 8));
  auto Yes = HighExpr::makeVar(tempOfSize(1, 16));
  auto No = HighExpr::makeVar(tempOfSize(2, 4));
  for (auto Combine : {NdOp::INT_OR, NdOp::INT_ADD})
    for (uint16_t Bytes : {4, 8}) {
      auto Before = selectedBytes(Condition, Yes, No, 4, Bytes, Combine);
      EXPECT_EQ(selectionCount(Before), 4U);
      auto After = basicSimplified(Before);
      EXPECT_EQ(selectionCount(After), 1U) << After->str();
      EXPECT_EQ(After->Type->Size, Bytes);
    }
}

TEST(HighExprSimplify, SelectedByteRecoveryPreservesGuardsAndExtensions) {
  auto Condition = HighExpr::makeVar(tempOfSize(0, 8));
  auto Yes = HighExpr::makeVar(tempOfSize(1, 16));
  auto No = HighExpr::makeVar(tempOfSize(2, 4));
  for (auto Combine : {NdOp::INT_OR, NdOp::INT_ADD}) {
    auto Signed = selectedBytes(Condition, Yes, No, 4, 4, Combine, true);
    EXPECT_GT(selectionCount(basicSimplified(Signed)), 1U);
    auto Guards = selectedBytes(Condition, Yes, No, 4, 4, Combine);
    auto Last = Guards->Operands[1]->Operands[0]->Operands[0];
    ASSERT_EQ(Last->Op, NdOp::SELECT);
    Last->Operands[0] = HighExpr::makeVar(tempOfSize(3, 8));
    EXPECT_GT(selectionCount(basicSimplified(Guards)), 1U);
    auto Load = HighExpr::makeLoad(Condition, Yes->Type);
    auto Reads = selectedBytes(Condition, Load, No, 4, 4, Combine);
    EXPECT_TRUE(containsNode(basicSimplified(Reads), Load));
    auto Effect = HighExpr::makeCall("next_guard", 0x401000, {});
    Effect->Type = Condition->Type;
    auto Effects = selectedBytes(Effect, Yes, No, 4, 4, Combine);
    EXPECT_EQ(selectionCount(basicSimplified(Effects)), 4U);
    auto Unknown =
        selectedBytes(HighExpr::makeUndef(8), Yes, No, 4, 4, Combine);
    EXPECT_EQ(selectionCount(basicSimplified(Unknown)), 4U);
  }
}

TEST(HighExprSimplify, SelectedByteGuardsRetainTheirSignedWidening) {
  auto Narrow = HighExpr::makeVar(tempOfSize(0, 1), NdType::makeInt(1, true));
  auto Unsigned = HighExpr::makeVar(Narrow->Var, NdType::makeInt(1, false));
  auto Guard = [&](const ExprPtr &Input) {
    return HighExpr::makeBinop(NdOp::INT_EQUAL, integerCast(Input, 8),
                               HighExpr::makeConst(uint64_t{0} - 128, 8));
  };
  auto Yes = HighExpr::makeVar(tempOfSize(1, 16));
  auto No = HighExpr::makeVar(tempOfSize(2, 4));
  auto Before = selectedBytes(Guard(Narrow), Yes, No, 4, 4, NdOp::INT_OR);
  auto Last = Before->Operands[1]->Operands[0]->Operands[0];
  ASSERT_EQ(Last->Op, NdOp::SELECT);
  Last->Operands[0] = Guard(Unsigned);
  EXPECT_GT(selectionCount(basicSimplified(Before)), 1U);
}

TEST(HighExprSimplify, RecoveredSelectedWordExecutesAtBothSourceWidths) {
  auto Param = [](int Id, uint16_t Bytes) {
    auto V = tempOfSize(Id, Bytes);
    V.Kind = MedVar::Param;
    return HighExpr::makeVar(V, NdType::makeInt(Bytes, false));
  };
  auto Condition = Param(0, 8), Yes = Param(1, 16), No = Param(2, 4);
  for (uint16_t Bytes : {4, 8}) {
    HighFunc F;
    F.Name = "selected_word";
    F.Entry = kEntry;
    F.ReturnType = NdType::makeInt(Bytes, false);
    F.Params = {{"guard", Condition->Type, kNoParamReg, 0},
                {"yes", Yes->Type, kNoParamReg, 1},
                {"no", No->Type, kNoParamReg, 2}};
    F.Body = {returnValue(basicSimplified(
        selectedBytes(Condition, Yes, No, 4, Bytes,
                      Bytes == 4 ? NdOp::INT_OR : NdOp::INT_ADD)))};
    ASSERT_EQ(selectionCount(F.Body.front().RetVal), 1U);
    std::string C;
    llvm::raw_string_ostream OS(C);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
    OS.flush();
    const std::string Program = C + R"C(
int main(void) {
  const uint64_t guards[] = {0, 1, 2, 0x100, 0x8000000000000000ULL, ~0ULL};
  const uint64_t words[] = {0, 1, 0x80ff00ffULL, 0x8000000000000000ULL, ~0ULL};
  for (unsigned g = 0; g != 6; ++g)
    for (unsigned i = 0; i != 5; ++i)
      for (unsigned j = 0; j != 5; ++j) {
        const unsigned __int128 value =
            ((unsigned __int128)words[i] << 64) | words[j];
        const uint32_t no = (uint32_t)words[i];
        const uint32_t want = guards[g] ? (uint32_t)(value >> 32) : no;
        if (selected_word(guards[g], value, no) != (uint64_t)want)
          return 1;
      }
  return 0;
}
)C";
    source_call_execution_test::compileAndRun(Program, {"-O0"});
    source_call_execution_test::compileAndRun(Program, {"-O2"});
  }
}

TEST(HighSymSimplify, RepeatedQueriesRecomputeDefinitionsWidthsAndAliases) {
  // The same source expression is first read with an unhelpful shift, then
  // with the shift that cancels its residue. Reusing search work must not
  // reuse the old reaching definition, a different width, or input aliasing.
  for (unsigned Threads : {1u, 4u}) {
    for (uint16_t Bytes : {1, 4, 8, 16})
      for (bool Alias : {false, true}) {
        SCOPED_TRACE(Threads);
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Alias);
        auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
        auto Y = Alias ? X : HighExpr::makeVar(tempOfSize(2, Bytes));
        auto ShiftCount = HighExpr::makeVar(tempOfSize(3, Bytes));
        auto C = [&](uint64_t V) { return HighExpr::makeConst(V, Bytes); };
        auto Expected = HighExpr::makeBinop(
            NdOp::INT_ADD,
            HighExpr::makeBinop(NdOp::INT_ADD,
                                HighExpr::makeBinop(NdOp::INT_MULT, C(182), X),
                                HighExpr::makeBinop(NdOp::INT_MULT, C(73), Y)),
            C(37));
        auto Source = HighExpr::makeBinop(
            NdOp::INT_ADD, Expected,
            HighExpr::makeBinop(
                NdOp::INT_SUB,
                HighExpr::makeBinop(NdOp::INT_RIGHT, X, ShiftCount),
                HighExpr::makeBinop(
                    NdOp::INT_RIGHT,
                    HighExpr::makeBinop(NdOp::INT_RIGHT, X, C(2)), C(2))));
        const auto Original = Source->str();
        for (uint64_t Shift : {3, 4, 3, 4}) {
          SCOPED_TRACE(Shift);
          HighStmt Definition;
          Definition.Kind = StmtKind::Assign;
          Definition.Dst = ShiftCount;
          Definition.Val = C(Shift);
          HighStmt Return;
          Return.Kind = StmtKind::Return;
          Return.RetVal = Source;
          std::vector<HighStmt> Body{Definition, Return};
          auto Fresh = Body;
          simplifyExprSemantics(Body, Threads);
          simplifyExprSemantics(Fresh, Threads);
          const auto &After = Body.back().RetVal;
          EXPECT_EQ(After->str(), Fresh.back().RetVal->str());
          EXPECT_EQ(Source->str(), Original);
          if (Shift == 4)
            EXPECT_EQ(After->str().find(">>"), std::string::npos)
                << After->str();
          for (uint64_t A : {0ull, 1ull, 127ull, 128ull, 255ull, ~0ull})
            for (uint64_t B : {0ull, 1ull, 128ull, 255ull, ~0ull}) {
              const std::unordered_map<int, uint64_t> Bindings{
                  {1, A}, {2, B}, {3, Shift}};
              EXPECT_EQ(integerValue(After, A, B, &Bindings),
                        integerValue(Source, A, B, &Bindings));
            }
        }
      }
  }
}

TEST(HighSymSimplify, ScopedSearchRecomputesDefinitionsWidthsAndAliases) {
  // The same source expression is first read with an unhelpful shift, then
  // with the shift that cancels its residue. Reusing search work must not
  // reuse the old reaching definition, a different width, or input aliasing.
  for (unsigned Threads : {1u, 4u}) {
    detail::HighSemanticSimplifier Semantics;
    for (uint16_t Bytes : {1, 4, 8, 16})
      for (bool Alias : {false, true}) {
        SCOPED_TRACE(Threads);
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Alias);
        auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
        auto Y = Alias ? X : HighExpr::makeVar(tempOfSize(2, Bytes));
        auto ShiftCount = HighExpr::makeVar(tempOfSize(3, Bytes));
        auto C = [&](uint64_t V) { return HighExpr::makeConst(V, Bytes); };
        auto Expected = HighExpr::makeBinop(
            NdOp::INT_ADD,
            HighExpr::makeBinop(NdOp::INT_ADD,
                                HighExpr::makeBinop(NdOp::INT_MULT, C(182), X),
                                HighExpr::makeBinop(NdOp::INT_MULT, C(73), Y)),
            C(37));
        auto Source = HighExpr::makeBinop(
            NdOp::INT_ADD, Expected,
            HighExpr::makeBinop(
                NdOp::INT_SUB,
                HighExpr::makeBinop(NdOp::INT_RIGHT, X, ShiftCount),
                HighExpr::makeBinop(
                    NdOp::INT_RIGHT,
                    HighExpr::makeBinop(NdOp::INT_RIGHT, X, C(2)), C(2))));
        const auto Original = Source->str();
        for (uint64_t Shift : {3, 4, 3, 4}) {
          SCOPED_TRACE(Shift);
          HighStmt Definition;
          Definition.Kind = StmtKind::Assign;
          Definition.Dst = ShiftCount;
          Definition.Val = C(Shift);
          HighStmt Return;
          Return.Kind = StmtKind::Return;
          Return.RetVal = Source;
          std::vector<HighStmt> Body{Definition, Return};
          auto Fresh = Body;
          Semantics.simplify(Body, Threads);
          simplifyExprSemantics(Fresh, Threads);
          const auto &After = Body.back().RetVal;
          EXPECT_EQ(After->str(), Fresh.back().RetVal->str());
          EXPECT_EQ(Source->str(), Original);
          if (Shift == 4)
            EXPECT_EQ(After->str().find(">>"), std::string::npos)
                << After->str();
          for (uint64_t A : {0ull, 1ull, 127ull, 128ull, 255ull, ~0ull})
            for (uint64_t B : {0ull, 1ull, 128ull, 255ull, ~0ull}) {
              const std::unordered_map<int, uint64_t> Bindings{
                  {1, A}, {2, B}, {3, Shift}};
              EXPECT_EQ(integerValue(After, A, B, &Bindings),
                        integerValue(Source, A, B, &Bindings));
            }
        }
      }
  }
}
