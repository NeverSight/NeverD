//===- HighCIntegerWidthTests.cpp - Executable integer width checks -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>
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

std::string emitFunctions(const std::vector<HighFunc> &Functions) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
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

void compileAndExecute(const std::string &Source, bool CheckShiftUB) {
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

} // namespace
