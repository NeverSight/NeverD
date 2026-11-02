//===- AArch64_CallAbiTests.cpp - AAPCS64 call contracts ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Floating register transport, native scalar interfaces and outgoing calls.
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"

#include <fstream>
#include <iterator>
#include <regex>

class AArch64_CallAbi : public NeverDLiftTest {};

namespace {
#define CALL_ABI_VALUE(Type, Name, Value) inline constexpr Type Name = Value;
#define CALL_ABI_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64CallAbiFixtures.def"

// Follow only transformations that preserve the low double-width bits.
// This accepts both vector extraction and integer carrier legalization while
// refusing arithmetic, another lane, or an unrelated incoming value.
llvm::Value *callABILowDoubleOrigin(llvm::Value *Value) {
  while (auto *Instruction = llvm::dyn_cast<llvm::Instruction>(Value)) {
    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Instruction)) {
      const auto SourceBits =
          Cast->getOperand(0)->getType()->getPrimitiveSizeInBits();
      const auto ResultBits = Cast->getType()->getPrimitiveSizeInBits();
      if ((!llvm::isa<llvm::BitCastInst, llvm::TruncInst, llvm::ZExtInst>(
              Cast)) ||
          SourceBits.isScalable() || ResultBits.isScalable() ||
          SourceBits.getFixedValue() < kCallABIDoubleBits ||
          ResultBits.getFixedValue() < kCallABIDoubleBits)
        break;
      Value = Cast->getOperand(0);
      continue;
    }
    if (auto *Extract = llvm::dyn_cast<llvm::ExtractElementInst>(Instruction)) {
      const auto *Index =
          llvm::dyn_cast<llvm::ConstantInt>(Extract->getIndexOperand());
      if (!Index || !Index->isZero() ||
          Extract->getType()->getPrimitiveSizeInBits() < kCallABIDoubleBits)
        break;
      Value = Extract->getVectorOperand();
      continue;
    }
    if (auto *Insert = llvm::dyn_cast<llvm::InsertElementInst>(Instruction)) {
      const auto *Index =
          llvm::dyn_cast<llvm::ConstantInt>(Insert->getOperand(2));
      if (!Index || !Index->isZero() ||
          Insert->getOperand(1)->getType()->getPrimitiveSizeInBits() <
              kCallABIDoubleBits)
        break;
      Value = Insert->getOperand(1);
      continue;
    }
    break;
  }
  return Value;
}
} // namespace

static fs::path callAbiObj() {
  return fs::path(TEST_OBJ_DIR) / "test_call_abi_a64_macho";
}

static fs::path callArgPhiMachOObj() {
  return fs::path(TEST_OBJ_DIR) / "test_call_arg_phi_a64_macho";
}

static fs::path callArgPhiELFObj() {
  return fs::path(TEST_OBJ_DIR) / "test_call_arg_phi_a64_elf.so";
}

static fs::path callArgPhiPEObj() {
  return fs::path(TEST_OBJ_DIR) / "test_call_arg_phi_a64_pe.exe";
}

static std::string callAbiFunctionText(llvm::StringRef IR,
                                       llvm::StringRef Name) {
  std::string Needle = "@" + Name.str() + "(";
  size_t Begin = 0;
  while ((Begin = IR.find("define ", Begin)) != llvm::StringRef::npos) {
    size_t HeaderEnd = IR.find('\n', Begin);
    size_t NameAt = IR.find(Needle, Begin);
    if (NameAt != llvm::StringRef::npos &&
        (HeaderEnd == llvm::StringRef::npos || NameAt < HeaderEnd))
      break;
    Begin += sizeof("define ") - 1;
  }
  if (Begin == llvm::StringRef::npos)
    return {};
  size_t End = IR.find("\n}", Begin);
  if (End == llvm::StringRef::npos)
    return {};
  return IR.slice(Begin, End + 2).str();
}

TEST_F(AArch64_CallAbi, IndirectFPCallUsesV0ForArgumentAndReturn) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string F = callAbiFunctionText(R.out, kCallABIIndirectDouble);
  ASSERT_FALSE(F.empty()) << R.out;
  size_t Call = F.find(kCallABIVectorCall);
  ASSERT_NE(Call, std::string::npos) << F;
  size_t CallEnd = F.find('\n', Call);
  std::string CallLine = F.substr(Call, CallEnd - Call);
  EXPECT_NE(CallLine.find(kCallABIVectorArgument), std::string::npos)
      << CallLine;
  EXPECT_EQ(CallLine.find(kCallABIPointerArgument), std::string::npos)
      << CallLine;
  EXPECT_NE(F.find(kCallABIScalarReturn), std::string::npos) << F;

  auto Opt = liftToLLVMIR(callAbiObj());
  ASSERT_EQ(Opt.exitCode, 0) << Opt.err;
  const llvm::StringRef Output(Opt.out);
  const size_t ModuleBegin = Output.find(kCallABIModuleStart);
  ASSERT_NE(ModuleBegin, llvm::StringRef::npos) << Opt.out;
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  const auto Module = llvm::parseAssemblyString(Output.drop_front(ModuleBegin),
                                                Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  const auto *Function = Module->getFunction(kCallABIIndirectDouble);
  ASSERT_NE(Function, nullptr);
  EXPECT_TRUE(Function->getReturnType()->isDoubleTy());
  ASSERT_EQ(Function->arg_size(), kCallABIParameters);
  EXPECT_TRUE(Function->getArg(kCallABIFPParameter)->getType()->isDoubleTy());
  unsigned Returns = 0;
  for (const auto &Block : *Function)
    for (const auto &Instruction : Block)
      if (const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(&Instruction)) {
        ++Returns;
        const auto *Producer = llvm::dyn_cast<llvm::CallBase>(
            callABILowDoubleOrigin(Return->getReturnValue()));
        ASSERT_NE(Producer, nullptr);
        EXPECT_TRUE(Producer->isIndirectCall());
        ASSERT_EQ(Producer->arg_size(), kCallABICallbackParameters);
        EXPECT_EQ(callABILowDoubleOrigin(Producer->getArgOperand(0)),
                  Function->getArg(kCallABIFPParameter));
      }
  EXPECT_NE(Returns, 0U);
}

TEST_F(AArch64_CallAbi, ExternalPairCallPreservesX0AndX1) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string F = callAbiFunctionText(R.out, "direct_external_pair_sum");
  ASSERT_FALSE(F.empty()) << R.out;
  EXPECT_NE(F.find("call { i64, i64 } @make_external_pair()"),
            std::string::npos)
      << F;
  EXPECT_NE(F.find("extractvalue { i64, i64 }"), std::string::npos) << F;
  EXPECT_EQ(F.find("X1_call_clobber_unknown"), std::string::npos) << F;
}

TEST_F(AArch64_CallAbi, ExternalDarwinVarargsKeepOutgoingStackValues) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string F = callAbiFunctionText(R.out, "direct_external_varargs");
  ASSERT_FALSE(F.empty()) << R.out;
  EXPECT_NE(F.find("call i64 (i64, ...) @sum_external_varargs(i64 16, i64 32, "
                   "i64 48)"),
            std::string::npos)
      << F;
}

TEST_F(AArch64_CallAbi, DarwinVarargsAtJoinKeepAllOutgoingStackValues) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string F = callAbiFunctionText(R.out, "joined_external_varargs");
  ASSERT_FALSE(F.empty()) << R.out;
  size_t Call = F.find("@printf(");
  ASSERT_NE(Call, std::string::npos) << F;
  size_t LineBegin = F.rfind('\n', Call);
  size_t LineEnd = F.find('\n', Call);
  std::string CallLine = F.substr(LineBegin + 1, LineEnd - LineBegin - 1);
  size_t FirstArg = CallLine.find("i64 %");
  ASSERT_NE(FirstArg, std::string::npos) << CallLine;
  EXPECT_NE(CallLine.find("i64 %", FirstArg + 6), std::string::npos)
      << CallLine;
}

TEST_F(AArch64_CallAbi, DarwinVaListForwarderUsesExactConsumerCertificate) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string F = callAbiFunctionText(R.out, "forward_va_list");
  ASSERT_FALSE(F.empty()) << R.out;
  EXPECT_NE(F.find("@forward_va_list(i64"), std::string::npos) << F;
  EXPECT_NE(F.find("..."), std::string::npos) << F;
  EXPECT_NE(F.find("call i64 @vprintf(ptr"), std::string::npos) << F;
}

TEST_F(AArch64_CallAbi, DarwinVaListWrapperKeepsFixedRegisterPrefix) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string Wrapper =
      callAbiFunctionText(R.out, "forward_va_list_with_context");
  ASSERT_FALSE(Wrapper.empty()) << R.out;
  EXPECT_NE(Wrapper.find("@forward_va_list_with_context(i32 %arg0, i64 %arg1"),
            std::string::npos)
      << Wrapper;

  std::string Caller =
      callAbiFunctionText(R.out, "call_forward_va_list_with_context");
  ASSERT_FALSE(Caller.empty()) << R.out;
  size_t Call = Caller.find("@forward_va_list_with_context(");
  ASSERT_NE(Call, std::string::npos) << Caller;
  size_t LineBegin = Caller.rfind('\n', Call);
  size_t LineEnd = Caller.find('\n', Call);
  std::string CallLine = Caller.substr(LineBegin + 1, LineEnd - LineBegin - 1);
  EXPECT_NE(CallLine.find("call i64 (i32, i64, i64, ...)"), std::string::npos)
      << CallLine;
  size_t ContextArg = CallLine.find("i32 %");
  ASSERT_NE(ContextArg, std::string::npos) << CallLine;
  size_t FixedArg = CallLine.find("i64 ", ContextArg);
  ASSERT_NE(FixedArg, std::string::npos) << CallLine;
  EXPECT_NE(CallLine.find("i64 ", FixedArg + 4), std::string::npos) << CallLine;
}

TEST_F(AArch64_CallAbi, FixedPointerArgumentUsesFullWidthPhiAlias) {
  if (!fs::exists(callArgPhiMachOObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callArgPhiMachOObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string F = callAbiFunctionText(R.out, "body");
  ASSERT_FALSE(F.empty()) << R.out;
  size_t Call = F.find("@snprintf(");
  ASSERT_NE(Call, std::string::npos) << F;
  size_t LineBegin = F.rfind('\n', Call);
  size_t LineEnd = F.find('\n', Call);
  std::string CallLine = F.substr(LineBegin + 1, LineEnd - LineBegin - 1);
  EXPECT_NE(CallLine.find("call i32 (ptr, i64, ptr, ...) @snprintf"),
            std::string::npos)
      << CallLine;
  // Mach-O ADRP+ADD arms are occurrence-symbolized before the PHI.  The
  // all-arm owner must consume that already-relocated merge directly instead
  // of applying a second run-relative base.
  EXPECT_NE(F.find(" = phi i64 [ ptrtoint"), std::string::npos) << F;
  EXPECT_NE(F.find("%selmrgrawptr = inttoptr i64 %X2."), std::string::npos)
      << F;
  EXPECT_NE(CallLine.find("ptr %selmrgrawptr"), std::string::npos) << CallLine;
}

TEST_F(AArch64_CallAbi, ELFFixedPointerArgumentUsesFullWidthPhiAlias) {
  if (!fs::exists(callArgPhiELFObj()))
    GTEST_SKIP() << "AArch64 ELF ABI fixture requires ld.lld";
  auto R = liftToLLVMIRUnopt(callArgPhiELFObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  EXPECT_NE(R.out.find(" = phi i64 [ ptrtoint"), std::string::npos) << R.out;
  EXPECT_NE(R.out.find("%selmrgrawptr = inttoptr i64 %X2."), std::string::npos)
      << R.out;
  const size_t Call = R.out.find("@snprintf(");
  ASSERT_NE(Call, std::string::npos) << R.out;
  const size_t LineBegin = R.out.rfind('\n', Call);
  const size_t LineEnd = R.out.find('\n', Call);
  const std::string CallLine =
      R.out.substr(LineBegin + 1, LineEnd - LineBegin - 1);
  EXPECT_NE(CallLine.find("ptr %selmrgrawptr"), std::string::npos) << CallLine;
}

TEST_F(AArch64_CallAbi, PEFixedPointerArgumentUsesFullWidthPhiAlias) {
  if (!fs::exists(callArgPhiPEObj()))
    GTEST_SKIP() << "AArch64 PE ABI fixture requires lld-link";
  auto R = liftToLLVMIRUnopt(callArgPhiPEObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  // PE's direct PHI constants retain the original-VA model, so the same owner
  // rebases the merged offset into the reconstructed rodata run.
  EXPECT_NE(R.out.find("%selmrgoff = sub i64 %X2."), std::string::npos)
      << R.out;
  EXPECT_NE(R.out.find("%selmrgptr = getelementptr"), std::string::npos)
      << R.out;
  size_t Call = R.out.find("@snprintf(");
  ASSERT_NE(Call, std::string::npos) << R.out;
  size_t LineBegin = R.out.rfind('\n', Call);
  size_t LineEnd = R.out.find('\n', Call);
  std::string CallLine = R.out.substr(LineBegin + 1, LineEnd - LineBegin - 1);
  EXPECT_NE(CallLine.find("call i32 (ptr, i64, ptr, ...) @snprintf"),
            std::string::npos)
      << CallLine;
  EXPECT_NE(CallLine.find("ptr %selmrgptr"), std::string::npos) << CallLine;
}

TEST_F(AArch64_CallAbi, ExternalZeroArgPrototypeIsStableAcrossCallSites) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string Compute = callAbiFunctionText(R.out, "external_error_compute");
  std::string Simple = callAbiFunctionText(R.out, "external_error_simple");
  ASSERT_FALSE(Compute.empty()) << R.out;
  ASSERT_FALSE(Simple.empty()) << R.out;
  EXPECT_NE(Compute.find("@__error()"), std::string::npos) << Compute;
  EXPECT_NE(Simple.find("@__error()"), std::string::npos) << Simple;
  EXPECT_EQ(R.out.find("@__error(i"), std::string::npos) << R.out;
}

TEST_F(AArch64_CallAbi, InternalNoReturnCallTerminatesFailingEdge) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string Caller = callAbiFunctionText(R.out, "neverd_after_fail");
  std::string Callee = callAbiFunctionText(R.out, "neverd_fail");
  ASSERT_FALSE(Caller.empty()) << R.out;
  ASSERT_FALSE(Callee.empty()) << R.out;
  size_t Call = Caller.find("@neverd_fail()");
  ASSERT_NE(Call, std::string::npos) << Caller;
  EXPECT_NE(Caller.find("unreachable", Call), std::string::npos) << Caller;
  EXPECT_NE(Callee.find("call void @llvm.trap()"), std::string::npos) << Callee;
  EXPECT_NE(Callee.find("unreachable"), std::string::npos) << Callee;
  EXPECT_EQ(Callee.find("ret i64"), std::string::npos) << Callee;
}

TEST_F(AArch64_CallAbi, AllStagesPass) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  verifyAllStages(callAbiObj());
  verifyLLVMIRNoVerifierErrors(callAbiObj());
}

// AAPCS64 passes floating-point arguments in S and D registers numbered apart
// from the integer X registers, and an optimizing compiler reduces a
// forwarder to a branch that passes its own incoming registers on.  The C
// route must recover both: compile the kernels for AArch64, decompile them
// to C, and run that C on the host against the same source built natively.
TEST_F(AArch64_CallAbi, HighCPassesFloatAndForwardedArguments) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target execution requires Clang";
  // Every product and sum stays exact, so a fused multiply-add rounds alike.
  const std::string Kernels = R"C(
__attribute__((noinline)) float hf(float a, float b) { return a * b - b; }
long ff(long a) { float x = (float)a; return (long)hf(x * 1.5f, x - 2.0f); }
__attribute__((noinline)) double hm(double a, int k, double b) {
  return a * k - b;
}
long mix(long a) {
  double x = (double)a;
  return (long)hm(x * 1.25, (int)a & 7, x + 3.0);
}
__attribute__((noinline)) long g2(long a, long b) { return a * 3 + b; }
long fwd2(long a, long b) { return g2(a, b); }
)C";
  const auto Source = tmpFile("call-args-a64.c");
  const auto Object = tmpFile("call-args-a64.o");
  std::ofstream(Source) << Kernels;
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-target", "aarch64-linux-gnu", "-O2", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;

  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(Input.good());
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  ASSERT_EQ(C.find("unknown value"), std::string::npos) << C;
  EXPECT_NE(C.find("float hf(float "), std::string::npos) << C;

  std::string Reference = Kernels;
  for (const char *Name : {"hf", "ff", "hm", "mix", "g2", "fwd2"})
    Reference = std::regex_replace(
        Reference, std::regex(std::string("\\b") + Name + "\\("),
        std::string("ref_") + Name + "(");
  const auto Program = tmpFile("call-args-a64.c.host");
  std::ofstream(tmpFile("call-args-a64-host.c")) << C << "\n"
                                                 << Reference << R"C(
int main(void) {
  static const long Values[] = {-5, 0, 3, 7, 1000};
  for (unsigned I = 0; I != sizeof(Values) / sizeof(Values[0]); ++I) {
    const long A = Values[I];
    if (ff(A) != ref_ff(A))
      return 1;
    if (mix(A) != ref_mix(A))
      return 2;
    if (fwd2(A, A + 11) != ref_fwd2(A, A + 11))
      return 3;
  }
  return 0;
}
)C";
  const auto Built =
      exec(NEVERD_TEST_CLANG,
           {"-std=gnu11", "-O2", tmpFile("call-args-a64-host.c").string(),
            "-lm", "-o", Program.string()});
  ASSERT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
  const auto Run = exec(Program.string(), {});
  EXPECT_EQ(Run.exitCode, 0) << C;
}
