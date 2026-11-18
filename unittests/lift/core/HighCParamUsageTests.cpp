//===- HighCParamUsageTests.cpp - Shared parameter-use queries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve parameter uses while bounding shared expressions and call graphs.

#include "../../../lib/backend/c/HighC/HighCWriter.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"

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

TEST(HighCParamUsage, MIPSFullGPRReadsKeepTheIncomingWordExtension) {
  std::string Source = "#include <stdint.h>\n";
  std::string Checks;
  unsigned Index = 0;
  for (Arch Architecture :
       {Arch::MIPS64, Arch::MIPS64EL, Arch::MIPS64N32, Arch::MIPS64N32EL})
    for (bool Signed : {false, true})
      for (bool DebugType : {false, true})
        for (bool Forwarded : {false, true})
          for (uint16_t ReadBytes : {4, 8}) {
            SCOPED_TRACE(unsigned(Architecture));
            SCOPED_TRACE(Signed);
            SCOPED_TRACE(DebugType);
            SCOPED_TRACE(Forwarded);
            SCOPED_TRACE(ReadBytes);
            const auto Register = getTargetRegInfo(Architecture)
                                      .integerArgumentLayout(BinaryFormat::ELF)
                                      .Registers.front();
            HighFunc Function;
            Function.Entry = 0x1000;
            Function.Name = "entry";
            const auto LogicalType = NdType::makeInt(4, Signed);
            Function.Params.push_back(
                {DebugType ? "arg0" : "value",
                 DebugType ? NdType::makeInt(8) : LogicalType, Register, 0});
            RecordParametersDebug Debug;
            Debug.Function.Addr = Function.Entry;
            Debug.Function.Params = {{"value", LogicalType}};
            CEmitterOptions Options;
            Options.TheArch = Architecture;
            Options.Format = BinaryFormat::ELF;
            std::string Text;
            llvm::raw_string_ostream Out(Text);
            HighCWriter Writer(Out, Options, DebugType ? &Debug : nullptr);
            Writer.CurrentFunc = &Function;
            MedVar Value;
            Value.Kind = Forwarded ? MedVar::Temp : MedVar::Param;
            Value.Id = Forwarded ? 7 : 0;
            Value.Size = ReadBytes;
            Value.TheArch = Architecture;
            if (Forwarded)
              Writer.CopyForward[Writer.varName(Value)] = "value";
            const auto Expression =
                HighExpr::makeVar(Value, NdType::makeInt(ReadBytes, false));
            const std::string Read = Writer.exprStr(*Expression);
            if (ReadBytes == 8) {
              const auto Printed = Writer.printedIntegerType(*Expression);
              ASSERT_TRUE(Printed) << Read;
              EXPECT_EQ(Printed->first, 8);
              EXPECT_TRUE(Printed->second);
            }
            const std::string Name = "probe_" + std::to_string(Index++);
            Source += "uint64_t " + Name + "(" +
                      (Signed ? "int32_t" : "uint32_t") + " value) { return " +
                      Read + "; }\n";
            const std::string Expected = ReadBytes == 8 || Signed
                                             ? "(uint64_t)(int64_t)(int32_t)x"
                                             : "(uint64_t)x";
            Checks += "if (" + Name + "(x) != " + Expected + ") return 1;\n";
          }
  Source += "int main(void) { const uint32_t values[] = "
            "{0, 1, 0x7fffffffU, 0x80000000U, 0x90010001U, 0xffffffffU}; "
            "for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) "
            "{ uint32_t x = values[i];\n" +
            Checks + "} return 0; }\n";
  // Execute the emitted integer expressions on the host. The independent
  // MIPS round-trip also checks actual n32/n64 callers and relocated pointers.
  for (const char *Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(Source, {Level});
}

TEST(HighCParamUsage, MIPSWordExtensionDoesNotInventRegisterOrPointerBits) {
  for (Arch Architecture : {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64,
                            Arch::MIPS64EL, Arch::AArch64})
    for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF})
      for (bool Register : {false, true})
        for (bool Pointer : {false, true}) {
          HighFunc Function;
          Function.Entry = 0x1000;
          Function.Params.push_back(
              {"value", Pointer ? NdType::makePtr() : NdType::makeInt(4, false),
               Register ? getTargetRegInfo(Architecture)
                              .integerArgumentLayout(BinaryFormat::ELF)
                              .Registers.front()
                        : kNoParamReg,
               0});
          CEmitterOptions Options;
          Options.TheArch = Architecture;
          Options.Format = Format;
          std::string Text;
          llvm::raw_string_ostream Out(Text);
          HighCWriter Writer(Out, Options, nullptr);
          Writer.CurrentFunc = &Function;
          MedVar Value;
          Value.Kind = MedVar::Param;
          Value.Id = 0;
          Value.Size = 8;
          const auto Expression = HighExpr::makeVar(Value, NdType::makeInt(8));
          const std::string Read = Writer.exprStr(*Expression);
          const bool Extended = Register && Format == BinaryFormat::ELF &&
                                isMIPS64(Architecture) &&
                                (!Pointer || isMIPSN32(Architecture));
          EXPECT_EQ(Read.find("(int64_t)") != std::string::npos, Extended)
              << unsigned(Architecture) << " " << unsigned(Format) << " "
              << Register << " " << Pointer << " " << Read;
        }
}

TEST(HighCParamUsage, UnreadFloatingSuffixKeepsItsDeclaredInterface) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool HasReadParameter : {false, true}) {
        SCOPED_TRACE(unsigned(Architecture));
        SCOPED_TRACE(unsigned(Format));
        SCOPED_TRACE(HasReadParameter);
        HighFunc Function;
        Function.Name = "tick";
        Function.Entry = 0x401000;
        Function.ReturnType = NdType::makeVoid();
        RecordParametersDebug Debug;
        Debug.Function.Addr = Function.Entry;
        Debug.Function.Name = Function.Name;
        Debug.Function.ReturnType = Function.ReturnType;
        if (HasReadParameter) {
          Function.Params.push_back({"context", NdType::makePtr(),
                                     getTargetRegInfo(Architecture)
                                         .integerArgumentLayout(Format)
                                         .Registers.front(),
                                     0});
          Debug.Function.Params.push_back({"context", NdType::makePtr()});
        }
        Debug.Function.Params.push_back({"elapsed", NdType::makeFloat(4)});
        Debug.Function.Params.push_back({"scale", NdType::makeFloat(8)});
        Debug.Function.Params.push_back({"flags", NdType::makeInt(4, false)});
        CEmitterOptions Options;
        Options.TheArch = Architecture;
        Options.Format = Format;
        std::string Text;
        llvm::raw_string_ostream Out(Text);
        HighCWriter Writer(Out, Options, &Debug);
        const size_t First = HasReadParameter ? 1 : 0;
        EXPECT_EQ(Writer.unreadDeclaredParams(Function),
                  std::vector<size_t>({First, First + 1, First + 2}));
        Writer.writeAll({Function});
        EXPECT_NE(Text.find("float elapsed, double scale, uint32_t flags"),
                  std::string::npos)
            << Text;
      }
}

TEST(HighCParamUsage, UnplacedSignatureDoesNotInventUnreadFloatingParameters) {
  HighFunc Function;
  Function.Name = "custom_tick";
  Function.Entry = 0x401000;
  Function.ReturnType = NdType::makeVoid();
  RecordParametersDebug Debug;
  Debug.Function.Addr = Function.Entry;
  Debug.Function.PlatformConvention = false;
  Debug.Function.Params = {{"elapsed", NdType::makeFloat(4)}};
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::ELF;
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  HighCWriter Writer(Out, Options, &Debug);
  EXPECT_TRUE(Writer.unreadDeclaredParams(Function).empty());
}

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
