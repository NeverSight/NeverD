//===- HighCIntegerWidthTests.cpp - Executable integer width checks -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

ExprPtr parameter(unsigned Id, TypeRef Type) {
  MedVar Var;
  Var.Kind = MedVar::Param;
  Var.Id = static_cast<int>(Id);
  Var.Size = Type->Size;
  Var.TheArch = Arch::X64;
  return HighExpr::makeVar(Var, Type);
}

void returnValue(HighFunc &Func, ExprPtr Value) {
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Value);
  Func.Body.push_back(std::move(Return));
}

std::string emitFunctions(const std::vector<HighFunc> &Functions,
                          const BinaryImage *Image = nullptr) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Image = Image;
  EXPECT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  OS.flush();
  return Source;
}

std::string executionHarness(const std::string &Checks) {
  return R"(
#include <inttypes.h>
#include <stdio.h>
static unsigned failures;
static void check_value(const char *name, uint64_t got, uint64_t expected) {
    if (got != expected) {
        fprintf(stderr, "%s: got 0x%016" PRIx64 ", expected 0x%016" PRIx64 "\n",
                name, got, expected);
        ++failures;
    }
}
int main(void) {
)" + Checks +
         "    return failures != 0;\n}\n";
}

std::string argument(const TypeRef &Type, const char *Bits) {
  return "(" + typeToC(Type) + ")UINT64_C(" + Bits + ")";
}

void appendCheck(std::string &Checks, const std::string &Name,
                 const std::string &Arguments, const char *Expected) {
  Checks += "    check_value(\"" + Name + "(" + Arguments + ")\", " +
            "(uint64_t)" + Name + "(" + Arguments + "), UINT64_C(" + Expected +
            "));\n";
}

void compileAndExecute(const std::string &Source, bool CheckShiftUB,
                       bool CheckArithmeticUB = false) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto FoundCompiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(FoundCompiler)) << "clang is required";
  const std::string Compiler = *FoundCompiler;
#endif
  llvm::SmallString<128> SourcePath, ExecutablePath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-integer-width", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-integer-width", "exe",
                                                  ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-integer-width", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC) << EC.message();
    OS << Source;
  }
  llvm::SmallVector<llvm::StringRef, 16> Arguments{
      Compiler, "-std=c11", "-O1", "-fno-inline", "-Werror=return-type"};
  if (CheckShiftUB) {
    // Trap mode requires no platform sanitizer runtime. An unsupported
    // compiler is a real test failure rather than an unreported skip.
    Arguments.push_back("-fsanitize=shift");
    Arguments.push_back("-fsanitize-trap=shift");
  }
  if (CheckArithmeticUB) {
    Arguments.push_back("-fsanitize=signed-integer-overflow");
    Arguments.push_back("-fsanitize-trap=signed-integer-overflow");
  }
  Arguments.append({SourcePath, "-o", ExecutablePath});
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  std::string Error;
  const int CompileStatus = llvm::sys::ExecuteAndWait(
      Compiler, Arguments, std::nullopt, Redirects, 30, 0, &Error);
  auto CompileErrors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(CompileStatus, 0)
      << Error << (CompileErrors ? (*CompileErrors)->getBuffer().str() : "")
      << "\n"
      << Source;
  llvm::SmallVector<llvm::StringRef, 1> RunArguments{ExecutablePath};
  const int RunStatus = llvm::sys::ExecuteAndWait(
      ExecutablePath, RunArguments, std::nullopt, Redirects, 30, 0, &Error);
  auto RunErrors = llvm::MemoryBuffer::getFile(ErrorPath);
  EXPECT_EQ(RunStatus, 0) << Error
                          << (RunErrors ? (*RunErrors)->getBuffer().str() : "")
                          << "\n"
                          << Source;
}

TEST(HighCIntegerWidths, UnsignedMachineWidthArithmeticUsesDeclaredType) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  auto AddOne = [&](const std::string &Name, TypeRef Declared,
                    TypeRef OperandType, TypeRef ResultType, const char *Input,
                    const char *Expected) {
    HighFunc Func;
    Func.Name = Name;
    Func.ReturnType = ResultType;
    Func.Params = {{"arg0", Declared}};
    auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, parameter(0, OperandType),
                                   HighExpr::makeConst(1, 4));
    Sum->Type = ResultType;
    returnValue(Func, Sum);
    appendCheck(Checks, Name, argument(Declared, Input), Expected);
    Functions.push_back(std::move(Func));
  };

  const auto U16 = NdType::makeInt(2, false);
  const auto U32 = NdType::makeInt(4, false);
  const auto S32 = NdType::makeInt(4, true);
  const auto U64 = NdType::makeInt(8, false);
  AddOne("natural_u32_add", U32, U32, U32, "0xffffffff", "0");
  AddOne("natural_u64_add", U64, U64, U64, "0xffffffffffffffff", "0");
  AddOne("natural_u32_to_s32", U32, S32, S32, "0xffffff9c",
         "0xffffffffffffff9d");
  // C promotes uint16_t to int, so the result still needs an explicit
  // machine-width truncation. Signed operands also need unsigned arithmetic
  // to avoid signed-overflow UB, even if HighIR inferred an unsigned result.
  AddOne("narrow_u16_add", U16, U16, U16, "0xffff", "0");
  AddOne("signed_s32_add", S32, S32, S32, "0x7fffffff", "0xffffffff80000000");
  AddOne("signed_decl_unsigned_ir_add", S32, U32, U32, "0xffffffff", "0");

  const std::string Source = emitFunctions(Functions);
  auto Body = [&](const std::string &Name) {
    const size_t Begin = Source.find(Name + "(");
    EXPECT_NE(Begin, std::string::npos) << Source;
    if (Begin == std::string::npos)
      return std::string{};
    const size_t End = Source.find("\n}", Begin);
    EXPECT_NE(End, std::string::npos) << Source;
    return Source.substr(Begin, End == std::string::npos ? End : End - Begin);
  };
  EXPECT_NE(Body("natural_u32_add").find("return arg0 + 1;"), std::string::npos)
      << Source;
  EXPECT_NE(Body("natural_u64_add").find("return arg0 + 1;"), std::string::npos)
      << Source;
  EXPECT_NE(Body("natural_u32_to_s32").find("return (int32_t)(arg0 + 1);"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Body("narrow_u16_add").find("return arg0 + 1;"), std::string::npos)
      << Source;
  EXPECT_EQ(Body("signed_s32_add").find("return arg0 + 1;"), std::string::npos)
      << Source;
  EXPECT_EQ(Body("signed_decl_unsigned_ir_add").find("return arg0 + 1;"),
            std::string::npos)
      << Source;
  compileAndExecute(Source + executionHarness(Checks), false,
                    /*CheckArithmeticUB=*/true);
}

TEST(HighCIntegerWidths, ArithmeticWrapsBeforeWideningWithoutSignedOverflow) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (uint16_t Width : {1, 2, 4, 8}) {
    const unsigned Bits = Width * 8;
    const uint64_t Max = UINT64_MAX >> (64 - Bits);
    const uint64_t Values[] = {0, 1, Max >> 1, (Max >> 1) + 1, Max - 1, Max};
    for (bool Signed : {false, true})
      for (NdOp Op : {NdOp::INT_ADD, NdOp::INT_SUB, NdOp::INT_MULT}) {
        const auto Type = NdType::makeInt(Width, Signed);
        HighFunc Func;
        Func.Name = "wrap" + std::to_string(Bits) + (Signed ? "_s" : "_u") +
                    std::to_string(static_cast<int>(Op));
        Func.ReturnType = NdType::makeInt(8, false);
        Func.Params = {{"arg0", Type}, {"arg1", Type}};
        auto Value =
            HighExpr::makeBinop(Op, parameter(0, Type), parameter(1, Type));
        Value->Type = Type;
        // Widening after the expression must observe its already wrapped
        // result, including signed reinterpretation at the narrow width.
        auto Widened = std::make_shared<HighExpr>();
        Widened->Kind = ExprKind::Cast;
        Widened->Type = Widened->CastTo = Func.ReturnType;
        Widened->Operands = {Value};
        returnValue(Func, Widened);
        for (uint64_t Left : Values)
          for (uint64_t Right : Values) {
            const llvm::APInt A(Bits, Left), B(Bits, Right);
            const llvm::APInt Result = Op == NdOp::INT_ADD   ? A + B
                                       : Op == NdOp::INT_SUB ? A - B
                                                             : A * B;
            const auto Expected = std::to_string(
                (Signed ? Result.sextOrTrunc(64) : Result.zextOrTrunc(64))
                    .getZExtValue());
            appendCheck(Checks, Func.Name,
                        argument(Type, std::to_string(Left).c_str()) + ", " +
                            argument(Type, std::to_string(Right).c_str()),
                        Expected.c_str());
          }
        Functions.push_back(std::move(Func));
      }
  }
  const auto WideMax = llvm::APInt::getAllOnes(128);
  const llvm::APInt WideValues[] = {llvm::APInt(128, 0), llvm::APInt(128, 1),
                                    WideMax.lshr(1),     WideMax.lshr(1) + 1,
                                    WideMax - 1,         WideMax};
  auto WideLiteral = [](const llvm::APInt &Value) {
    return "(((__uint128_t)UINT64_C(" +
           std::to_string(Value.lshr(64).getZExtValue()) +
           ") << 64) | UINT64_C(" +
           std::to_string(Value.trunc(64).getZExtValue()) + "))";
  };
  for (bool Signed : {false, true})
    for (NdOp Op : {NdOp::INT_ADD, NdOp::INT_SUB, NdOp::INT_MULT}) {
      const auto Type = NdType::makeInt(16, Signed);
      HighFunc Func;
      Func.Name = std::string("wrap128_") + (Signed ? "s" : "u") +
                  std::to_string(static_cast<int>(Op));
      Func.ReturnType = Type;
      Func.Params = {{"arg0", Type}, {"arg1", Type}};
      auto Value =
          HighExpr::makeBinop(Op, parameter(0, Type), parameter(1, Type));
      Value->Type = Type;
      returnValue(Func, Value);
      for (const auto &Left : WideValues)
        for (const auto &Right : WideValues) {
          const llvm::APInt Result = Op == NdOp::INT_ADD   ? Left + Right
                                     : Op == NdOp::INT_SUB ? Left - Right
                                                           : Left * Right;
          auto WideArgument = [&](const llvm::APInt &Bits) {
            return Signed ? "__builtin_bit_cast(__int128_t, " +
                                WideLiteral(Bits) + ")"
                          : WideLiteral(Bits);
          };
          Checks += "    { __uint128_t actual = (__uint128_t)" + Func.Name +
                    "(" + WideArgument(Left) + ", " + WideArgument(Right) +
                    "); check_value(\"" + Func.Name +
                    " low\", (uint64_t)actual, UINT64_C(" +
                    std::to_string(Result.trunc(64).getZExtValue()) +
                    ")); check_value(\"" + Func.Name +
                    " high\", (uint64_t)(actual >> 64), UINT64_C(" +
                    std::to_string(Result.lshr(64).getZExtValue()) + ")); }\n";
        }
      Functions.push_back(std::move(Func));
    }
  compileAndExecute(emitFunctions(Functions) + executionHarness(Checks), false,
                    true);
}

TEST(HighCIntegerWidths, SignedOverflowPredicatesCompileAndMatchBoundaries) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (uint16_t Width : {1, 2, 4, 8, 16}) {
    const unsigned Bits = Width * 8;
    const std::vector<llvm::APInt> Values{
        llvm::APInt(Bits, 0),
        llvm::APInt(Bits, 1),
        llvm::APInt::getSignedMaxValue(Bits),
        llvm::APInt::getSignedMaxValue(Bits) - 1,
        llvm::APInt::getSignedMinValue(Bits),
        llvm::APInt::getSignedMinValue(Bits) + 1,
        llvm::APInt::getAllOnes(Bits)};
    for (bool SignedCarrier : {true, false}) {
      const auto Type = NdType::makeInt(Width, SignedCarrier);
      const auto BitArgument = [&](const llvm::APInt &BitsValue) {
        if (Width <= 8)
          return argument(
              Type, std::to_string(BitsValue.getZExtValue()).c_str());
        const auto Bits = "(((__uint128_t)UINT64_C(" +
                          std::to_string(BitsValue.lshr(64).getZExtValue()) +
                          ") << 64) | UINT64_C(" +
                          std::to_string(BitsValue.trunc(64).getZExtValue()) +
                          "))";
        return SignedCarrier
                   ? "__builtin_bit_cast(" + typeToC(Type) + ", " + Bits + ")"
                   : Bits;
      };
      for (const auto Op : {NdOp::INT_SOVF, NdOp::INT_SBOR}) {
        HighFunc Func;
        Func.Name = "signed_overflow_" + std::to_string(Bits) +
                    (SignedCarrier ? "_s" : "_u") +
                    (Op == NdOp::INT_SOVF ? "_add" : "_sub");
        Func.ReturnType = NdType::makeInt(1, false);
        Func.Params = {{"arg0", Type}, {"arg1", Type}};
        auto Value =
            HighExpr::makeBinop(Op, parameter(0, Type), parameter(1, Type));
        Value->Type = Func.ReturnType;
        returnValue(Func, Value);
        for (const auto &Left : Values)
          for (const auto &Right : Values) {
            bool Overflow = false;
            [[maybe_unused]] const auto Result =
                Op == NdOp::INT_SOVF ? Left.sadd_ov(Right, Overflow)
                                      : Left.ssub_ov(Right, Overflow);
            appendCheck(
                Checks, Func.Name, BitArgument(Left) + ", " + BitArgument(Right),
                Overflow ? "1" : "0");
          }
        Functions.push_back(std::move(Func));
      }
    }
  }
  const auto Source = emitFunctions(Functions);
  EXPECT_EQ(Source.find("_overflow_p"), std::string::npos);
  compileAndExecute(Source + executionHarness(Checks), false);
}

TEST(HighCIntegerWidths, WordShapedConstantsKeepTheirFullWidthValue) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (bool Signed : {false, true})
    for (uint64_t Bits : {UINT64_C(0x7fffffff), UINT64_C(0x80000000),
                          UINT64_C(0xfffffffe), UINT64_C(0xffffffff),
                          UINT64_C(0x100000000), UINT64_MAX - 1, UINT64_MAX}) {
      const auto Type = NdType::makeInt(8, Signed);
      HighFunc F;
      F.Name = std::string("constant_") + (Signed ? "s_" : "u_") +
               std::to_string(Bits);
      F.ReturnType = Type;
      auto Constant = HighExpr::makeConst(Bits, 8);
      Constant->Type = Type;
      returnValue(F, Constant);
      appendCheck(Checks, F.Name, "", std::to_string(Bits).c_str());
      Functions.push_back(F);
      F.Name += "_mask";
      F.Params = {{"arg0", Type}};
      F.Body.clear();
      auto Masked =
          HighExpr::makeBinop(NdOp::INT_AND, parameter(0, Type), Constant);
      Masked->Type = Type;
      returnValue(F, Masked);
      for (uint64_t Input : {UINT64_C(0), UINT64_C(0xffffffff),
                             UINT64_C(0x8000000100000001), UINT64_MAX})
        appendCheck(Checks, F.Name,
                    argument(Type, std::to_string(Input).c_str()),
                    std::to_string(Input & Bits).c_str());
      Functions.push_back(F);
    }
  for (uint16_t Width : {1, 2, 4})
    for (bool Signed : {false, true}) {
      HighFunc F;
      F.Name =
          "narrow_constant_" + std::to_string(Width) + (Signed ? "s" : "u");
      F.ReturnType = NdType::makeInt(8, Signed);
      const uint64_t Bits = (UINT64_C(1) << (Width * 8)) - 1;
      auto Constant = HighExpr::makeConst(Bits, Width);
      Constant->Type = NdType::makeInt(Width, Signed);
      returnValue(F, Constant);
      appendCheck(Checks, F.Name, "",
                  std::to_string(Signed ? UINT64_MAX : Bits).c_str());
      Functions.push_back(F);
    }
  compileAndExecute(emitFunctions(Functions) + executionHarness(Checks), false);
}

TEST(HighCIntegerWidths, NegationWrapsAndNestedNegativeConstantsCompile) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  auto Literal = [](const llvm::APInt &Value) {
    const auto Wide = Value.zextOrTrunc(128);
    return "(((__uint128_t)UINT64_C(" +
           std::to_string(Wide.lshr(64).getZExtValue()) +
           ") << 64) | UINT64_C(" +
           std::to_string(Wide.trunc(64).getZExtValue()) + "))";
  };
  for (uint16_t Width : {1, 2, 4, 8, 16}) {
    const unsigned Bits = Width * 8;
    const auto Max = llvm::APInt::getAllOnes(Bits);
    const llvm::APInt Values[] = {llvm::APInt(Bits, 0), llvm::APInt(Bits, 1),
                                  Max.lshr(1), Max.lshr(1) + 1, Max};
    for (bool Signed : {false, true}) {
      const auto Type = NdType::makeInt(Width, Signed);
      HighFunc Func;
      Func.Name = "neg" + std::to_string(Bits) + (Signed ? "s" : "u");
      Func.ReturnType = Type;
      Func.Params = {{"arg0", Type}};
      auto Value = HighExpr::makeUnary(NdOp::INT_NEG2, parameter(0, Type));
      Value->Type = Type;
      returnValue(Func, Value);
      for (const auto &Input : Values) {
        const auto Expected = (-Input).zextOrTrunc(128);
        const auto Argument = "__builtin_bit_cast(" + typeToC(Type) + ", (" +
                              typeToC(NdType::makeInt(Width, false)) + ")" +
                              Literal(Input) + ")";
        Checks += "    if ((__uint128_t)(" +
                  typeToC(NdType::makeInt(Width, false)) + ")" + Func.Name +
                  "(" + Argument + ") != " + Literal(Expected) +
                  ") ++failures;\n";
      }
      Functions.push_back(std::move(Func));
    }
  }
  HighFunc Constant;
  Constant.Name = "neg_constant";
  Constant.ReturnType = NdType::makeInt(8);
  returnValue(
      Constant,
      HighExpr::makeUnary(NdOp::INT_NEG2, HighExpr::makeConst(UINT64_MAX, 8)));
  Functions.push_back(std::move(Constant));
  Checks += "    if (neg_constant() != 1) ++failures;\n";
  compileAndExecute(emitFunctions(Functions) + executionHarness(Checks), false,
                    true);
}

TEST(HighCIntegerWidths, ExtensionsFollowOpcodeAndSourceWidthAtRuntime) {
  struct Sample {
    uint16_t Width;
    const char *Input;
    const char *SignExtended;
  };
  // Expected bit patterns are independent constants, not the casts being
  // tested. Include both sides of every sign boundary and all-one values.
  const Sample Samples[] = {{1, "0x00", "0x0000000000000000"},
                            {1, "0x7f", "0x000000000000007f"},
                            {1, "0x80", "0xffffffffffffff80"},
                            {1, "0xff", "0xffffffffffffffff"},
                            {2, "0x0000", "0x0000000000000000"},
                            {2, "0x7fff", "0x0000000000007fff"},
                            {2, "0x8000", "0xffffffffffff8000"},
                            {2, "0xffff", "0xffffffffffffffff"},
                            {4, "0x00000000", "0x0000000000000000"},
                            {4, "0x7fffffff", "0x000000007fffffff"},
                            {4, "0x80000001", "0xffffffff80000001"},
                            {4, "0xffffffff", "0xffffffffffffffff"}};
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (uint16_t Width : {1, 2, 4}) {
    for (bool DeclaredSigned : {false, true}) {
      for (bool ExprSigned : {false, true}) {
        for (bool SignExtend : {false, true}) {
          const auto DeclaredType = NdType::makeInt(Width, DeclaredSigned);
          const auto ExprType = NdType::makeInt(Width, ExprSigned);
          HighFunc Func;
          Func.Name = std::string(SignExtend ? "sext" : "zext") +
                      std::to_string(Width * 8) +
                      (DeclaredSigned ? "_decl_s" : "_decl_u") +
                      (ExprSigned ? "_expr_s" : "_expr_u");
          Func.ReturnType = NdType::makeInt(8, SignExtend);
          Func.Params = {{"arg0", DeclaredType}};
          auto Extended =
              HighExpr::makeUnary(SignExtend ? NdOp::INT_SEXT : NdOp::INT_ZEXT,
                                  parameter(0, ExprType));
          Extended->Type = Func.ReturnType;
          returnValue(Func, Extended);
          for (const auto &Sample : Samples)
            if (Sample.Width == Width)
              appendCheck(Checks, Func.Name,
                          argument(DeclaredType, Sample.Input),
                          SignExtend ? Sample.SignExtended : Sample.Input);
          Functions.push_back(std::move(Func));
        }
      }
    }
  }
  compileAndExecute(emitFunctions(Functions) + executionHarness(Checks), false);
}

TEST(HighCIntegerWidths, ConcatAndLeftShiftExecuteWithoutSignedShiftUB) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  struct JoinedSample {
    uint16_t HalfWidth;
    const char *High;
    const char *Low;
    const char *Expected;
  };
  const JoinedSample JoinedSamples[] = {
      {2, "0x1122", "0x8001", "0x11228001"},
      {2, "0x8000", "0xffff", "0x8000ffff"},
      {2, "0xffff", "0xffff", "0xffffffff"},
      {4, "0x11223344", "0x80000001", "0x1122334480000001"},
      {4, "0x80000000", "0xffffffff", "0x80000000ffffffff"},
      {4, "0xffffffff", "0xffffffff", "0xffffffffffffffff"}};
  for (uint16_t HalfWidth : {2, 4}) {
    for (bool HighSigned : {false, true}) {
      for (bool LowSigned : {false, true}) {
        auto HighType = NdType::makeInt(HalfWidth, HighSigned);
        auto LowType = NdType::makeInt(HalfWidth, LowSigned);
        HighFunc Func;
        Func.Name = "join" + std::to_string(HalfWidth * 8) +
                    (HighSigned ? "_s" : "_u") + (LowSigned ? "s" : "u");
        Func.ReturnType = NdType::makeInt(8, false);
        Func.Params = {{"arg0", HighType}, {"arg1", LowType}};
        auto Joined = HighExpr::makeBinop(NdOp::CONCAT, parameter(0, HighType),
                                          parameter(1, LowType));
        // A signed HighIR result must not make the internal left shift signed.
        Joined->Type = NdType::makeInt(HalfWidth * 2, true);
        auto Unsigned = std::make_shared<HighExpr>();
        Unsigned->Kind = ExprKind::Cast;
        Unsigned->Type = Unsigned->CastTo =
            NdType::makeInt(HalfWidth * 2, false);
        Unsigned->Operands.push_back(Joined);
        returnValue(Func, Unsigned);
        for (const auto &Sample : JoinedSamples)
          if (Sample.HalfWidth == HalfWidth)
            appendCheck(Checks, Func.Name,
                        argument(HighType, Sample.High) + ", " +
                            argument(LowType, Sample.Low),
                        Sample.Expected);
        Functions.push_back(std::move(Func));
      }
    }
  }

  struct ShiftSample {
    uint16_t Width;
    const char *Input;
    const char *Amount;
    const char *Expected;
  };
  const ShiftSample ShiftSamples[] = {
      {1, "0x80", "0", "0x80"},
      {1, "0x80", "1", "0x00"},
      {1, "0xff", "1", "0xfe"},
      {1, "0x01", "7", "0x80"},
      {2, "0x8000", "0", "0x8000"},
      {2, "0x8000", "1", "0x0000"},
      {2, "0xffff", "1", "0xfffe"},
      {2, "0x0001", "15", "0x8000"},
      {4, "0x80000001", "0", "0x80000001"},
      {4, "0x80000001", "1", "0x00000002"},
      {4, "0xffffffff", "1", "0xfffffffe"},
      {4, "0x00000001", "31", "0x80000000"},
      {8, "0x8000000000000001", "0", "0x8000000000000001"},
      {8, "0x8000000000000001", "1", "0x0000000000000002"},
      {8, "0xffffffffffffffff", "1", "0xfffffffffffffffe"},
      {8, "0x0000000000000001", "63", "0x8000000000000000"}};
  for (uint16_t Width : {1, 2, 4, 8}) {
    for (bool Signed : {false, true}) {
      auto Type = NdType::makeInt(Width, Signed);
      auto ShiftType = NdType::makeInt(4, false);
      HighFunc Func;
      Func.Name = "left" + std::to_string(Width * 8) + (Signed ? "_s" : "_u");
      Func.ReturnType = NdType::makeInt(8, false);
      Func.Params = {{"arg0", Type}, {"arg1", ShiftType}};
      auto Shifted = HighExpr::makeBinop(NdOp::INT_LEFT, parameter(0, Type),
                                         parameter(1, ShiftType));
      auto Unsigned = std::make_shared<HighExpr>();
      Unsigned->Kind = ExprKind::Cast;
      Unsigned->Type = Unsigned->CastTo = NdType::makeInt(Width, false);
      Unsigned->Operands.push_back(Shifted);
      returnValue(Func, Unsigned);
      for (const auto &Sample : ShiftSamples)
        if (Sample.Width == Width)
          appendCheck(Checks, Func.Name,
                      argument(Type, Sample.Input) + ", " + Sample.Amount,
                      Sample.Expected);
      Functions.push_back(std::move(Func));
    }
  }
  for (uint16_t Width : {1, 2}) {
    // Observe the shift inside a comparison, before a return/store cast can
    // hide C's promoted 510/131070 result instead of the narrow 254/65534.
    const auto Type = NdType::makeInt(Width, false);
    HighFunc Func;
    Func.Name =
        "left" + std::to_string(Width * 8) + "_truncates_before_compare";
    Func.ReturnType = NdType::makeInt(4);
    Func.Params = {{"arg0", Type}};
    auto Shifted = HighExpr::makeBinop(NdOp::INT_LEFT, parameter(0, Type),
                                       HighExpr::makeConst(1, 4));
    auto Equal = HighExpr::makeBinop(
        NdOp::INT_EQUAL, Shifted,
        HighExpr::makeConst(Width == 1 ? 0xfe : 0xfffe, Width));
    Equal->Type = Func.ReturnType;
    returnValue(Func, Equal);
    appendCheck(Checks, Func.Name,
                argument(Type, Width == 1 ? "0xff" : "0xffff"), "1");
    Functions.push_back(std::move(Func));
  }
  compileAndExecute(emitFunctions(Functions) + executionHarness(Checks), true);
}

TEST(HighCIntegerWidths, CarryUsesUnsignedOperandWidthAtRuntime) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (uint16_t Width : {1, 2, 4, 8}) {
    const uint64_t Max = UINT64_MAX >> (64 - Width * 8);
    const uint64_t Sign = (Max >> 1) + 1;
    struct Sample {
      uint64_t Left;
      uint64_t Right;
      bool Carry;
    };
    const Sample Samples[] = {{0, 0, false},        {Max, 0, false},
                              {Max, 1, true},       {1, Max, true},
                              {Sign - 1, 1, false}, {Sign, Sign, true},
                              {Max, Max, true},     {Sign, 1, false}};
    for (bool LeftSigned : {false, true}) {
      for (bool RightSigned : {false, true}) {
        auto LeftType = NdType::makeInt(Width, LeftSigned);
        auto RightType = NdType::makeInt(Width, RightSigned);
        HighFunc Func;
        Func.Name = "carry" + std::to_string(Width * 8) +
                    (LeftSigned ? "_s" : "_u") + (RightSigned ? "s" : "u");
        Func.ReturnType = NdType::makeInt(1, false);
        Func.Params = {{"arg0", LeftType}, {"arg1", RightType}};
        auto Carry = HighExpr::makeBinop(
            NdOp::INT_CARRY, parameter(0, LeftType), parameter(1, RightType));
        Carry->Type = Func.ReturnType;
        returnValue(Func, Carry);
        for (const auto &Sample : Samples) {
          const auto Left = std::to_string(Sample.Left);
          const auto Right = std::to_string(Sample.Right);
          appendCheck(Checks, Func.Name,
                      argument(LeftType, Left.c_str()) + ", " +
                          argument(RightType, Right.c_str()),
                      Sample.Carry ? "1" : "0");
        }
        Functions.push_back(std::move(Func));
      }
    }
  }
  compileAndExecute(emitFunctions(Functions) + executionHarness(Checks), false);
}

TEST(HighCIntegerWidths, SignedComparisonsFollowOpcodeAtRuntime) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (uint16_t Width : {1, 2, 4, 8}) {
    const unsigned Bits = Width * 8;
    const uint64_t Max = UINT64_MAX >> (64 - Bits);
    const uint64_t Values[] = {0, 1, Max >> 1, (Max >> 1) + 1, Max};
    for (bool LeftSigned : {false, true}) {
      for (bool RightSigned : {false, true}) {
        for (NdOp Op : {NdOp::INT_SLESS, NdOp::INT_SLESSEQUAL}) {
          auto LeftType = NdType::makeInt(Width, LeftSigned);
          auto RightType = NdType::makeInt(Width, RightSigned);
          HighFunc Func;
          Func.Name = "signed_cmp" + std::to_string(Bits) +
                      (LeftSigned ? "_s" : "_u") + (RightSigned ? "s" : "u") +
                      (Op == NdOp::INT_SLESS ? "_lt" : "_le");
          Func.ReturnType = NdType::makeInt(1, false);
          Func.Params = {{"arg0", LeftType}, {"arg1", RightType}};
          auto Comparison = HighExpr::makeBinop(Op, parameter(0, LeftType),
                                                parameter(1, RightType));
          Comparison->Type = Func.ReturnType;
          returnValue(Func, Comparison);
          for (uint64_t Left : Values) {
            for (uint64_t Right : Values) {
              const llvm::APInt A(Bits, Left), B(Bits, Right);
              const bool Expected = Op == NdOp::INT_SLESS ? A.slt(B) : A.sle(B);
              appendCheck(
                  Checks, Func.Name,
                  argument(LeftType, std::to_string(Left).c_str()) + ", " +
                      argument(RightType, std::to_string(Right).c_str()),
                  Expected ? "1" : "0");
            }
          }
          Functions.push_back(std::move(Func));
        }
      }
    }
  }
  compileAndExecute(emitFunctions(Functions) + executionHarness(Checks), false);
}

TEST(HighCIntegerWidths, DivisionAndRemainderFollowOpcodeAtRuntime) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (uint16_t Width : {1, 2, 4, 8}) {
    const unsigned Bits = Width * 8;
    const uint64_t Max = UINT64_MAX >> (64 - Bits);
    const uint64_t Values[] = {0, 1, Max >> 1, (Max >> 1) + 1, Max};
    for (bool LeftSigned : {false, true}) {
      for (bool RightSigned : {false, true}) {
        for (NdOp Op :
             {NdOp::INT_SDIV, NdOp::INT_SREM, NdOp::INT_DIV, NdOp::INT_REM}) {
          const bool SignedOp = Op == NdOp::INT_SDIV || Op == NdOp::INT_SREM;
          const bool Remainder = Op == NdOp::INT_SREM || Op == NdOp::INT_REM;
          auto LeftType = NdType::makeInt(Width, LeftSigned);
          auto RightType = NdType::makeInt(Width, RightSigned);
          HighFunc Func;
          Func.Name = "divrem" + std::to_string(Bits) +
                      (LeftSigned ? "_s" : "_u") + (RightSigned ? "s" : "u") +
                      (SignedOp ? "_signed" : "_unsigned") +
                      (Remainder ? "_rem" : "_div");
          Func.ReturnType = NdType::makeInt(Width, false);
          Func.Params = {{"arg0", LeftType}, {"arg1", RightType}};
          auto Comparison = HighExpr::makeBinop(Op, parameter(0, LeftType),
                                                parameter(1, RightType));
          Comparison->Type = Func.ReturnType;
          returnValue(Func, Comparison);
          for (uint64_t Left : Values) {
            for (uint64_t Right : Values) {
              const llvm::APInt A(Bits, Left), B(Bits, Right);
              if (B.isZero() || (A.isMinSignedValue() && B.isAllOnes()))
                continue;
              const auto Expected = std::to_string(
                  (SignedOp ? (Remainder ? A.srem(B) : A.sdiv(B))
                            : (Remainder ? A.urem(B) : A.udiv(B)))
                      .getZExtValue());
              appendCheck(
                  Checks, Func.Name,
                  argument(LeftType, std::to_string(Left).c_str()) + ", " +
                      argument(RightType, std::to_string(Right).c_str()),
                  Expected.c_str());
            }
          }
          Functions.push_back(std::move(Func));
        }
      }
    }
  }
  compileAndExecute(emitFunctions(Functions) + executionHarness(Checks), false);
}

TEST(HighCIntegerWidths, RightShiftsFollowOpcodeAndBoundCountsAtRuntime) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (uint16_t Width : {1, 2, 4, 8}) {
    const unsigned Bits = Width * 8;
    const uint64_t Max = UINT64_MAX >> (64 - Bits);
    const uint64_t Values[] = {0, Max >> 1, (Max >> 1) + 1, Max};
    const unsigned Counts[] = {0, 1, Bits - 1, Bits, Bits + 1, 256};
    for (bool Signed : {false, true}) {
      for (bool Arithmetic : {false, true}) {
        for (bool Constant : {false, true}) {
          for (unsigned Count : Counts) {
            auto Type = NdType::makeInt(Width, Signed);
            auto CountType = NdType::makeInt(4, false);
            HighFunc Func;
            Func.Name = std::string(Arithmetic ? "ashr" : "lshr") +
                        std::to_string(Bits) + (Signed ? "_s" : "_u") +
                        (Constant ? "_const" : "_var") + std::to_string(Count);
            Func.ReturnType = NdType::makeInt(Width, false);
            Func.Params = {{"arg0", Type}, {"arg1", CountType}};
            returnValue(Func, HighExpr::makeBinop(
                                  Arithmetic ? NdOp::INT_ASHR : NdOp::INT_RIGHT,
                                  parameter(0, Type),
                                  Constant ? HighExpr::makeConst(Count, 4)
                                           : parameter(1, CountType)));
            for (uint64_t Value : Values) {
              const llvm::APInt Input(Bits, Value);
              const auto Expected =
                  Arithmetic ? Input.ashr(Count < Bits ? Count : Bits - 1)
                             : (Count < Bits ? Input.lshr(Count)
                                             : llvm::APInt(Bits, 0));
              const auto InputText = std::to_string(Value);
              const auto ExpectedText = std::to_string(Expected.getZExtValue());
              appendCheck(Checks, Func.Name,
                          argument(Type, InputText.c_str()) + ", " +
                              std::to_string(Count),
                          ExpectedText.c_str());
            }
            Functions.push_back(std::move(Func));
          }
        }
      }
    }
  }
  compileAndExecute(emitFunctions(Functions) + executionHarness(Checks), true);
}

TEST(HighCIntegerWidths, ArithmeticRightShiftRestoresTypeBeforeComparison) {
  std::vector<HighFunc> Functions;
  std::string Checks;
  for (uint16_t Width : {1, 2, 4, 8}) {
    for (bool Constant : {false, true}) {
      auto Type = NdType::makeInt(Width, false);
      auto CountType = NdType::makeInt(4, false);
      HighFunc Func;
      Func.Name = "ashr_compare" + std::to_string(Width * 8) +
                  (Constant ? "_const" : "_var");
      Func.ReturnType = NdType::makeInt(1, false);
      Func.Params = {{"arg0", Type}, {"arg1", CountType}};
      const uint64_t Sign = UINT64_C(1) << (Width * 8 - 1);
      auto Shifted = HighExpr::makeBinop(NdOp::INT_ASHR, parameter(0, Type),
                                         Constant ? HighExpr::makeConst(1, 4)
                                                  : parameter(1, CountType));
      auto Equal =
          HighExpr::makeBinop(NdOp::INT_EQUAL, Shifted,
                              HighExpr::makeConst(Sign | (Sign >> 1), Width));
      Equal->Type = Func.ReturnType;
      returnValue(Func, Equal);
      const auto Input = std::to_string(Sign);
      appendCheck(Checks, Func.Name, argument(Type, Input.c_str()) + ", 1",
                  "1");
      Functions.push_back(std::move(Func));
    }
  }
  compileAndExecute(emitFunctions(Functions) + executionHarness(Checks), true);
}

TEST(HighCIntegerWidths, OverlappingX87ImageStoresUpdateOne80BitValue) {
  constexpr va_t Base = 0x140002000;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Segment Data;
  Data.Name = ".data";
  Data.VA = Base;
  Data.Data = {3, 0, 0, 0, 0, 0, 0, 0, 0x34, 0x12};
  Data.Size = Data.Data.size();
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Data));

  HighFunc Write;
  Write.Name = "write_x87_image";
  Write.ReturnType = NdType::makeVoid();
  auto Store = [&](va_t Addr, uint64_t Value, uint16_t Size) {
    HighStmt Stmt;
    Stmt.Kind = StmtKind::Store;
    Stmt.StoreAddr = HighExpr::makeConst(Addr, 8);
    Stmt.StoreVal = HighExpr::makeConst(Value, Size);
    Write.Body.push_back(std::move(Stmt));
  };
  Store(Base, UINT64_C(0x8000000000000001), 8);
  Store(Base + 8, 0x7ffe, 2);

  HighFunc Read;
  Read.Name = "read_x87_image";
  Read.ReturnType = NdType::makeInt(10, false);
  returnValue(
      Read, HighExpr::makeLoad(HighExpr::makeConst(Base, 8), Read.ReturnType));

  const std::string Source = emitFunctions({Write, Read}, &Image);
  EXPECT_NE(Source.find("unsigned char g_140002000_bytes[10]"),
            std::string::npos)
      << Source;
  const std::string Checks = R"(
    check_value("initial significand", (uint64_t)read_x87_image(), 3);
    check_value("initial exponent", (uint64_t)(read_x87_image() >> 64), 0x1234);
    write_x87_image();
    check_value("written significand", (uint64_t)read_x87_image(),
                UINT64_C(0x8000000000000001));
    check_value("written exponent", (uint64_t)(read_x87_image() >> 64),
                0x7ffe);
)";
  compileAndExecute(Source + executionHarness(Checks), false);
}

TEST(HighCIntegerWidths, SegmentedOffsetsDoNotAliasOverlappingImageBacking) {
  constexpr va_t Base = 0x140002000;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Segment Data;
  Data.Name = ".data";
  Data.VA = Base;
  Data.Data = {3, 0, 0, 0, 0, 0, 0, 0, 0x34, 0x12};
  Data.Size = Data.Data.size();
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Data));

  HighFunc DefaultStore;
  DefaultStore.Name = "write_image_backing";
  DefaultStore.ReturnType = NdType::makeVoid();
  for (const auto [Offset, Size] :
       {std::pair<uint64_t, uint16_t>{0, 8}, {8, 2}}) {
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = HighExpr::makeConst(Base + Offset, 8);
    Store.StoreVal = HighExpr::makeConst(1, Size);
    DefaultStore.Body.push_back(std::move(Store));
  }
  HighFunc DefaultRead;
  DefaultRead.Name = "read_image_backing";
  DefaultRead.ReturnType = NdType::makeInt(10, false);
  returnValue(DefaultRead, HighExpr::makeLoad(HighExpr::makeConst(Base, 8),
                                              DefaultRead.ReturnType));

  const auto U64 = NdType::makeInt(8, false);
  HighFunc SegmentedRead;
  SegmentedRead.Name = "read_gs_coincident";
  SegmentedRead.ReturnType = U64;
  returnValue(SegmentedRead, HighExpr::makeLoad(HighExpr::makeConst(Base, 8),
                                                U64, NdMemoryOrdering::None,
                                                NdMemoryAddressSpace::X86GS));

  HighFunc CompoundRead;
  CompoundRead.Name = "read_gs_compound";
  CompoundRead.ReturnType = U64;
  CompoundRead.Params = {{"arg0", U64}};
  returnValue(CompoundRead,
              HighExpr::makeLoad(
                  HighExpr::makeBinop(NdOp::INT_ADD, parameter(0, U64),
                                      HighExpr::makeConst(Base, 8)),
                  U64, NdMemoryOrdering::None, NdMemoryAddressSpace::X86GS));

  HighFunc SegmentedStore;
  SegmentedStore.Name = "write_gs_coincident";
  SegmentedStore.ReturnType = NdType::makeVoid();
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeConst(Base, 8);
  Store.StoreVal = HighExpr::makeConst(7, 8);
  Store.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  SegmentedStore.Body.push_back(std::move(Store));

  HighFunc AssignedStore;
  AssignedStore.Name = "assign_gs_coincident";
  AssignedStore.ReturnType = NdType::makeVoid();
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst =
      HighExpr::makeLoad(HighExpr::makeConst(Base, 8), U64,
                         NdMemoryOrdering::None, NdMemoryAddressSpace::X86GS);
  Assign.Val = HighExpr::makeConst(9, 8);
  AssignedStore.Body.push_back(std::move(Assign));

  HighFunc ExpressionStore;
  ExpressionStore.Name = "expression_gs_coincident";
  ExpressionStore.ReturnType = U64;
  auto StoreExpr = std::make_shared<HighExpr>();
  StoreExpr->Kind = ExprKind::Store;
  StoreExpr->Type = U64;
  StoreExpr->MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  StoreExpr->Operands = {HighExpr::makeConst(Base, 8),
                         HighExpr::makeConst(11, 8)};
  returnValue(ExpressionStore, StoreExpr);

  HighFunc AtomicStore;
  AtomicStore.Name = "atomic_gs_coincident";
  AtomicStore.ReturnType = U64;
  auto Atomic =
      HighExpr::makeBinop(NdOp::ATOMIC_ADD, HighExpr::makeConst(Base, 8),
                          HighExpr::makeConst(1, 8));
  Atomic->Type = U64;
  Atomic->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  Atomic->MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  returnValue(AtomicStore, Atomic);

  const std::string Source = emitFunctions(
      {DefaultStore, DefaultRead, SegmentedRead, CompoundRead, SegmentedStore,
       AssignedStore, ExpressionStore, AtomicStore},
      &Image);
  EXPECT_NE(Source.find("unsigned char g_140002000_bytes[10]"),
            std::string::npos)
      << Source;
  auto Body = [&](const std::string &Name) {
    const size_t Signature = Source.rfind(Name + "(");
    EXPECT_NE(Signature, std::string::npos) << Source;
    if (Signature == std::string::npos)
      return std::string();
    const size_t Open = Source.find('{', Signature);
    const size_t Close = Source.find("\n}", Open);
    EXPECT_NE(Open, std::string::npos) << Source;
    EXPECT_NE(Close, std::string::npos) << Source;
    if (Open == std::string::npos || Close == std::string::npos)
      return std::string();
    return Source.substr(Open, Close - Open);
  };
  for (const std::string &Name :
       {"read_gs_coincident", "read_gs_compound", "write_gs_coincident",
        "assign_gs_coincident", "expression_gs_coincident",
        "atomic_gs_coincident"}) {
    const std::string SegmentBody = Body(Name);
    EXPECT_NE(SegmentBody.find("0x140002000"), std::string::npos)
        << Name << "\n"
        << Source;
    EXPECT_EQ(SegmentBody.find("g_140002000_bytes"), std::string::npos)
        << Name << "\n"
        << Source;
  }
  EXPECT_NE(Body("read_gs_coincident").find("__readgsqword"), std::string::npos)
      << Source;
  EXPECT_NE(Body("read_gs_compound").find(" + 0x140002000"), std::string::npos)
      << Source;
  EXPECT_NE(Body("atomic_gs_coincident").find("__atomic_fetch_add"),
            std::string::npos)
      << Source;
}

TEST(HighCIntegerWidths, LinuxX64ExitOmitsUnusedUnknownRegisterInputs) {
  const auto U64 = NdType::makeInt(8, false);
  auto UnknownRegister = [&](int Id) {
    MedVar Register;
    Register.Kind = MedVar::Reg;
    Register.Id = Id;
    Register.Size = 8;
    Register.TheArch = Arch::X64;
    return HighExpr::makeVar(Register, U64);
  };
  auto Pair = [&](int LowId, int HighId) {
    auto Value = HighExpr::makeBinop(
        NdOp::CONCAT, UnknownRegister(HighId), UnknownRegister(LowId));
    Value->Type = NdType::makeInt(16, false);
    return Value;
  };
  auto MakeCall = [&](uint64_t Number, const std::string &Name) {
    HighFunc Func;
    Func.Name = Name;
    Func.ReturnType = NdType::makeVoid();
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall(
        "neverd_x64_syscall", 0,
        {HighExpr::makeConst(Number, 8), HighExpr::makeConst(0, 8),
         Pair(6, 2), Pair(10, 8), UnknownRegister(9)});
    Call.CallExpr->IntrinsicId = Intrinsic::X64Syscall;
    Call.CallExpr->Type = NdType::makeInt(16, false);
    Func.Body.push_back(std::move(Call));
    return Func;
  };

  const std::string Source = emitFunctions(
      {MakeCall(60, "call_exit"), MakeCall(231, "call_exit_group"),
       MakeCall(39, "call_getpid")});
  EXPECT_NE(Source.find("neverd_x64_syscall(60, 0, 0, 0, 0)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("neverd_x64_syscall(231, 0, 0, 0, 0)"),
            std::string::npos)
      << Source;
  const size_t Getpid = Source.find("call_getpid(");
  ASSERT_NE(Getpid, std::string::npos) << Source;
  EXPECT_NE(Source.find("__builtin_trap()", Getpid), std::string::npos)
      << Source;
#if defined(__x86_64__) && defined(__linux__)
  compileAndExecute(Source + "int main(void) { call_exit(); return 99; }\n",
                    false);
  compileAndExecute(Source +
                        "int main(void) { call_exit_group(); return 99; }\n",
                    false);
#endif
}

} // namespace
