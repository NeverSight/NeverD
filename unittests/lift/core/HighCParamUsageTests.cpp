//===- HighCParamUsageTests.cpp - Shared parameter-use queries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve parameter uses while bounding shared expressions and call graphs.

#include "../../../lib/backend/c/HighC/HighCWriter.h"
#include "gtest/gtest.h"

using namespace neverd;

namespace {
ExprPtr makeParam(int Id) {
  MedVar P;
  P.Kind = MedVar::Param;
  P.Id = Id;
  P.Size = 8;
  P.TheArch = Arch::X64;
  return HighExpr::makeVar(P, NdType::makeInt(8));
}

HighFunc makeFunction(size_t Index) {
  HighFunc F;
  F.Name = "fan_" + std::to_string(Index);
  F.Entry = 0x401000 + Index * 0x100;
  F.ReturnType = NdType::makeInt(8);
  for (int I = 0; I != 6; ++I)
    F.Params.push_back({"arg" + std::to_string(I), NdType::makeInt(8)});
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeConst(0, 8);
  F.Body.push_back(Ret);
  return F;
}

void addCall(HighFunc &F, const HighFunc &Callee, bool ReadParameters) {
  std::vector<ExprPtr> Arguments;
  for (size_t I = 0; I != F.Params.size(); ++I)
    Arguments.push_back(ReadParameters ? makeParam(static_cast<int>(I))
                                       : HighExpr::makeConst(7, 8));
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall(Callee.Name, Callee.Entry, Arguments);
  F.Body.insert(F.Body.begin(), std::move(Call));
}

class RecordParametersDebug : public DebugContext {
public:
  FunctionSym Function;
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    return Address == Function.Addr ? std::optional(Function) : std::nullopt;
  }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return std::nullopt;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return {Function}; }
  bool hasInfo() const override { return true; }
};
} // namespace

TEST(HighCParamUsage, RecordDeclarationsSurviveScalarFunctionSwitches) {
  HighFunc Scalar = makeFunction(0);
  HighFunc Records = makeFunction(1);
  HighFunc DebugOnly = makeFunction(2);
  const TypeRef Record = NdType::makeNamedRecord("Payload", 8);
  Scalar.Params[0].Name = "t40";
  Records.Params[0] = {"t40", NdType::makePtr(Record)};
  RecordParametersDebug Debug;
  Debug.Function.Addr = DebugOnly.Entry;
  Debug.Function.Params = {{"t40", NdType::makePtr(Record)}};
  Debug.Function.PlatformConvention = false;
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.Id = 40;
  Value.Size = 8;
  const auto Base = HighExpr::makeVar(Value, NdType::makeInt(8));
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  HighCWriter Writer(OS, Options, &Debug);
  // Each function has the same parameter count; both the current function's
  // declarations and a debug-only declaration retain their own authority.
  for (const HighFunc *Function :
       {&Scalar, &Records, &Scalar, &DebugOnly, &Scalar, &Records}) {
    Writer.CurrentFunc = Function;
    EXPECT_EQ(Writer.declaredRecordPointee(*Base),
              Function == &Scalar ? nullptr : Record)
        << Function->Name;
  }
}

TEST(HighCParamUsage, SharedExpressionVisitsRetainTheActuallyReadParameter) {
  auto F = makeFunction(0);
  ExprPtr Value = makeParam(0);
  for (unsigned I = 0; I != 28; ++I)
    Value = HighExpr::makeBinop(NdOp::INT_ADD, Value, Value);
  F.Body.front().RetVal = Value;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  HighCWriter Writer(OS, Options, nullptr);
  EXPECT_EQ(Writer.emittedParamIndices(F), std::vector<size_t>({0}));
}

TEST(HighCParamUsage, RepeatedCallsReuseCompleteDefaultParameterQueries) {
  std::vector<HighFunc> Functions;
  for (size_t I = 0; I != 28; ++I)
    Functions.push_back(makeFunction(I));
  for (size_t I = 0; I + 1 != Functions.size(); ++I) {
    addCall(Functions[I], Functions[I + 1], false);
    addCall(Functions[I], Functions[I + 1], false);
  }
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  HighCWriter Writer(OS, Options, nullptr);
  Writer.writeAll(Functions);
  for (const auto &F : Functions) {
    EXPECT_EQ(Writer.emittedParamIndices(F),
              std::vector<size_t>({0, 1, 2, 3, 4, 5}));
    EXPECT_NE(Text.find(F.Name + "("), std::string::npos);
  }
}

TEST(HighCParamUsage, RecursiveCallsKeepEveryReadOperand) {
  std::vector<HighFunc> Functions{makeFunction(0), makeFunction(1)};
  addCall(Functions[0], Functions[1], true);
  addCall(Functions[1], Functions[0], true);
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  HighCWriter Writer(OS, Options, nullptr);
  Writer.writeAll(Functions);
  for (const auto &F : Functions)
    EXPECT_EQ(Writer.emittedParamIndices(F),
              std::vector<size_t>({0, 1, 2, 3, 4, 5}));
}

TEST(HighCParamUsage, SharedCalleesKeepTheirDeclaredOperandProjection) {
  std::vector<HighFunc> Functions;
  for (size_t I = 0; I != 28; ++I) {
    Functions.push_back(makeFunction(I));
    Functions.back().Body.front().RetVal = makeParam(0);
  }
  for (size_t I = 0; I + 1 != Functions.size(); ++I) {
    addCall(Functions[I], Functions[I + 1], true);
    addCall(Functions[I], Functions[I + 1], true);
  }
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  HighCWriter Writer(OS, Options, nullptr);
  Writer.writeAll(Functions);
  for (const auto &F : Functions) {
    EXPECT_EQ(Writer.emittedParamIndices(F), std::vector<size_t>({0}));
    for (const auto &S : F.Body)
      if (S.Kind == StmtKind::Call) {
        ASSERT_NE(S.CallExpr, nullptr);
        EXPECT_EQ(Writer.printedCallOperands(*S.CallExpr),
                  std::vector<size_t>({0}));
      }
  }
}
