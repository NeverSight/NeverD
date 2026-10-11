//===- LLVMCValueTests.cpp - Executable LLVM C value semantics
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/LLVMC/LLVMCFrameLayout.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/TargetParser/Host.h"

#include <stdexcept>

namespace {

void compileAndRun(const std::string &Source,
                   llvm::StringRef Optimization = "-O2",
                   const std::string &ReferenceIR = {},
                   bool CheckUndefinedBehavior = false) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  llvm::SmallString<128> ReferencePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-value", "ll",
                                                  ReferencePath));
  llvm::FileRemover RemoveReference(ReferencePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-value", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-value", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-value", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  if (!ReferenceIR.empty()) {
    llvm::raw_fd_ostream OS(ReferencePath, EC);
    ASSERT_FALSE(EC);
    OS << ReferenceIR;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  llvm::SmallVector<llvm::StringRef, 12> Arguments{Compiler,
                                                   "-std=c11",
                                                   Optimization,
                                                   "-Werror=uninitialized",
                                                   "-Werror=return-type",
                                                   SourcePath,
                                                   "-o",
                                                   BinaryPath};
  if (!ReferenceIR.empty())
    Arguments.push_back(ReferencePath);
  if (CheckUndefinedBehavior) {
    Arguments.push_back("-fsanitize=undefined");
    Arguments.push_back("-fsanitize-trap=all");
  }
  std::string Error;
  int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                         Redirects, 30, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Result, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "")
                       << '\n'
                       << Source;
  Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                     Redirects, 30, 0, &Error);
  ASSERT_EQ(Result, 0) << Error << '\n' << Source;
}

TEST(LLVMCValues, DeepForwardedBitAssemblyRetainsEveryDefinition) {
  llvm::LLVMContext Context;
  llvm::Module Module("forwarded-packed-bits", Context);
  Module.setDataLayout("e-p:64:64");
  Module.setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Fn = llvm::Function::Create(llvm::FunctionType::get(I64, {I64}, false),
                                    llvm::GlobalValue::ExternalLinkage,
                                    "forwarded_bits", Module);
  llvm::IRBuilder<llvm::NoFolder> B(
      llvm::BasicBlock::Create(Context, "entry", Fn));
  auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 152),
                               nullptr, "frame");
  auto *Base = B.CreatePtrToInt(
      B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(72)), I64, "rsp_init");
  auto *Slot = B.CreateIntToPtr(B.CreateSub(Base, B.getInt64(8)), B.getPtrTy());
  llvm::Value *Packed = B.getInt64(0);
  for (unsigned Bit = 0; Bit < 64; ++Bit) {
    auto *Value = B.CreateAnd(B.CreateLShr(Fn->getArg(0), B.getInt64(Bit)),
                              B.getInt64(1));
    auto *Shifted = B.CreateShl(Value, B.getInt64(Bit));
    auto *Selected = B.CreateSelect(B.getTrue(), Shifted, B.getInt64(0));
    Packed = B.CreateOr(Packed, Selected);
  }
  B.CreateStore(Packed, Slot);
  B.CreateRet(B.CreateLoad(I64, Slot));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  const std::string Driver = R"(
int main(void) {
  const uint64_t inputs[] = {0, 1, UINT64_MAX, UINT64_C(0x1234567887654321), UINT64_C(1) << 63};
  for (unsigned i = 0; i < 5; ++i)
    if (forwarded_bits(inputs[i]) != inputs[i]) return 1;
  return 0;
}
)";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Driver, Optimization, {}, true);
}

TEST(LLVMCValues, ArrayResultsKeepBothWordsAcrossExternalCalls) {
  llvm::LLVMContext Context;
  llvm::Module Module("array-result", Context);
  Module.setDataLayout("e-p:64:64");
  Module.setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
  if (Module.getTargetTriple().isOSWindows() ||
      !(Module.getTargetTriple().isAArch64() ||
        Module.getTargetTriple().getArch() == llvm::Triple::x86_64))
    GTEST_SKIP()
        << "Raw array/C record ABI comparison requires AArch64 or SysV x64";
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Pair = llvm::ArrayType::get(Word, 2);
  auto *Provider = llvm::Function::Create(
      llvm::FunctionType::get(Pair, {Word}, false),
      llvm::GlobalValue::ExternalLinkage, "external_pair", Module);
  auto *Score = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Word}, false),
      llvm::GlobalValue::ExternalLinkage, "read_pair", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Score));
  auto *Result = B.CreateCall(Provider, {Score->getArg(0)}, "pair");
  B.CreateRet(B.CreateXor(B.CreateExtractValue(Result, {0}),
                          B.CreateExtractValue(Result, {1})));
  auto *Second = llvm::Function::Create(Score->getFunctionType(),
                                        llvm::GlobalValue::ExternalLinkage,
                                        "read_second", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Second));
  B.CreateRet(
      B.CreateExtractValue(B.CreateCall(Provider, {Second->getArg(0)}), {1}));
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  EXPECT_EQ(Source.find("uint64_t* external_pair"), std::string::npos);
  EXPECT_NE(Source.find(".elements[1]"), std::string::npos);
  std::string FunctionSource;
  llvm::raw_string_ostream FunctionOut(FunctionSource);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, FunctionOut, Options, nullptr,
                                          nullptr, Score));
  // The provider is compiled directly from LLVM in a separate translation
  // unit. This catches a pointer-return ABI even if the C were syntactically
  // valid, and makes a repeated external call observable.
  const std::string Reference = R"(
@calls = global i64 0
define [2 x i64] @external_pair(i64 %x) {
  %n = load i64, ptr @calls
  %next = add i64 %n, 1
  store i64 %next, ptr @calls
  %hi = xor i64 %x, -9223372036854775808
  %lo = add i64 %x, 17
  %a = insertvalue [2 x i64] poison, i64 %lo, 0
  %b = insertvalue [2 x i64] %a, i64 %hi, 1
  ret [2 x i64] %b
}
)";
  const std::string Main = R"(
extern uint64_t calls;
int main(void) {
  const uint64_t values[] = {0, 1, 17, UINT64_MAX, UINT64_C(1) << 63};
  for (unsigned i = 0; i < 5; ++i) {
    uint64_t x = values[i], n = calls;
    if (read_pair(x) != ((x + 17) ^ (x ^ (UINT64_C(1) << 63)))) return 1;
    if (calls != n + 1) return 2;
  }
  return 0;
}
)";
  std::string FullMain = Main;
  const std::string CallCheck = "if (calls != n + 1) return 2;";
  FullMain.insert(FullMain.find(CallCheck) + CallCheck.size(), R"(
    if (read_second(x) != (x ^ (UINT64_C(1) << 63))) return 3;
    if (calls != n + 2) return 4;
)");
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    compileAndRun(Source + FullMain, Optimization, Reference);
    compileAndRun(FunctionSource + Main, Optimization, Reference);
  }
}

TEST(LLVMCValues, ArrayArgumentsAndReturnsInteroperateWithLLVMCallers) {
  llvm::LLVMContext Context;
  llvm::Module Module("array-argument", Context);
  Module.setDataLayout("e-p:64:64");
  Module.setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
  if (Module.getTargetTriple().isOSWindows() ||
      !(Module.getTargetTriple().isAArch64() ||
        Module.getTargetTriple().getArch() == llvm::Triple::x86_64))
    GTEST_SKIP()
        << "Raw array/C record ABI comparison requires AArch64 or SysV x64";
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Pair = llvm::ArrayType::get(Word, 2);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(Pair, {Pair, Word}, false),
      llvm::GlobalValue::ExternalLinkage, "c_pair", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Fn));
  auto *First = B.CreateExtractValue(Fn->getArg(0), {0});
  auto *Second = B.CreateExtractValue(Fn->getArg(0), {1});
  auto *A = B.CreateInsertValue(llvm::PoisonValue::get(Pair),
                                B.CreateAdd(Second, Fn->getArg(1)), {0});
  B.CreateRet(B.CreateInsertValue(A, B.CreateXor(First, Fn->getArg(1)), {1}));
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  const std::string Reference = R"(
declare [2 x i64] @c_pair([2 x i64], i64)
define i64 @call_c_pair(i64 %x, i64 %y, i64 %z) {
  %a = insertvalue [2 x i64] poison, i64 %x, 0
  %b = insertvalue [2 x i64] %a, i64 %y, 1
  %r = call [2 x i64] @c_pair([2 x i64] %b, i64 %z)
  %lo = extractvalue [2 x i64] %r, 0
  %hi = extractvalue [2 x i64] %r, 1
  %v = sub i64 %lo, %hi
  ret i64 %v
}
)";
  Source += R"(
extern uint64_t call_c_pair(uint64_t, uint64_t, uint64_t);
int main(void) {
  const uint64_t values[] = {0, 1, UINT64_MAX, UINT64_C(1) << 63};
  for (unsigned i = 0; i < 4; ++i)
    for (unsigned j = 0; j < 4; ++j)
      for (unsigned k = 0; k < 4; ++k)
        if (call_c_pair(values[i], values[j], values[k]) !=
            (values[j] + values[k]) - (values[i] ^ values[k])) return 1;
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization, Reference);
}

TEST(LLVMCValues, NestedAggregateInsertExtractSelectAndPhiHaveValueSemantics) {
  llvm::LLVMContext Context;
  llvm::Module Module("aggregate-values", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Pair = llvm::ArrayType::get(Word, 2);
  auto *Record = llvm::StructType::get(Context, {Word, Pair});
  auto *Array = llvm::ArrayType::get(Record, 2);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Word, Word}, false),
      llvm::GlobalValue::ExternalLinkage, "aggregate_value", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Fn);
  auto *Yes = llvm::BasicBlock::Create(Context, "yes", Fn);
  auto *No = llvm::BasicBlock::Create(Context, "no", Fn);
  auto *Merge = llvm::BasicBlock::Create(Context, "merge", Fn);
  llvm::IRBuilder<llvm::NoFolder> B(Entry);
  auto *A = B.CreateInsertValue(llvm::Constant::getNullValue(Array),
                                Fn->getArg(0), {1, 1, 0}, "first");
  auto *C = B.CreateInsertValue(A, Fn->getArg(1), {1, 1, 1}, "second");
  auto *Flag = B.CreateICmpULT(Fn->getArg(0), Fn->getArg(1));
  B.CreateCondBr(Flag, Yes, No);
  B.SetInsertPoint(Yes);
  B.CreateBr(Merge);
  B.SetInsertPoint(No);
  B.CreateBr(Merge);
  B.SetInsertPoint(Merge);
  auto *Phi = B.CreatePHI(Array, 2, "merged");
  Phi->addIncoming(A, Yes);
  Phi->addIncoming(C, No);
  auto *Selected = B.CreateSelect(Flag, C, Phi, "selected");
  B.CreateRet(B.CreateXor(B.CreateExtractValue(Selected, {1, 1, 0}),
                          B.CreateExtractValue(Selected, {1, 1, 1})));
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  Source += R"(
int main(void) {
  const uint64_t values[] = {0, 1, UINT64_MAX, UINT64_C(1) << 63};
  for (unsigned i = 0; i < 4; ++i)
    for (unsigned j = 0; j < 4; ++j)
      if (aggregate_value(values[i], values[j]) != (values[i] ^ values[j]))
        return 1;
  return 0;
}


)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, UnprovedArrayBoundaryABIsFailBeforeWritingSource) {
  for (unsigned Mode = 0; Mode < 11; ++Mode) {
    llvm::LLVMContext Context;
    llvm::Module Module("unsupported-array-abi", Context);
    Module.setTargetTriple(llvm::Triple(
        Mode == 0 ? "x86_64-pc-windows-msvc" : "aarch64-unknown-linux-gnu"));
    Module.setDataLayout("e-p:64:64");
    llvm::Type *Element = Mode == 1 ? llvm::Type::getInt32Ty(Context)
                                    : llvm::Type::getInt64Ty(Context);
    llvm::Type *Result = llvm::ArrayType::get(Element, Mode == 2 ? 3 : 2);
    if (Mode == 3)
      Result =
          llvm::StructType::get(Context, llvm::ArrayRef<llvm::Type *>{Result});
    if (Mode == 4)
      Module.setTargetTriple(llvm::Triple());
    if (Mode == 5)
      Module.setDataLayout("e-p:32:32");
    llvm::SmallVector<llvm::Type *> Parameters;
    if (Mode >= 7) {
      Parameters.push_back(Result);
      if (Mode == 7)
        Parameters.append(7, llvm::Type::getInt64Ty(Context));
      if (Mode == 8)
        Parameters.push_back(llvm::Type::getDoubleTy(Context));
    }
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(Result, Parameters, Mode == 9),
        llvm::GlobalValue::ExternalLinkage, "external_array", Module);
    if (Mode == 6)
      Fn->setCallingConv(llvm::CallingConv::Fast);
    if (Mode == 10)
      Fn->addParamAttr(0, llvm::Attribute::InReg);
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
                 std::runtime_error);
    EXPECT_TRUE(Source.empty());
  }
}

TEST(LLVMCValues, AggregateCopiesPreserveUnalignedMemoryAndArrayStorage) {
  llvm::LLVMContext Context;
  llvm::Module Module("aggregate-memory", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Ptr = llvm::PointerType::getUnqual(Context);
  auto *Pair = llvm::ArrayType::get(Word, 2);
  auto *Global = new llvm::GlobalVariable(
      Module, Pair, false, llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantArray::get(Pair, {llvm::ConstantInt::get(Word, 5),
                                      llvm::ConstantInt::get(Word, 11)}),
      "array_storage");
  auto *Touch = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {Ptr}, false),
      llvm::GlobalValue::ExternalLinkage, "touch_words", Module);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Ptr, Ptr, Word}, false),
      llvm::GlobalValue::ExternalLinkage, "copy_aggregate", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Fn));
  auto *Local = B.CreateAlloca(Pair, nullptr, "local_array");
  auto *Input = B.CreateAlignedLoad(Pair, Fn->getArg(1), llvm::Align(1));
  auto *Updated = B.CreateInsertValue(Input, Fn->getArg(2), {1});
  B.CreateStore(Updated, Local);
  B.CreateCall(Touch, {Local});
  auto *Observed = B.CreateLoad(Pair, Local);
  B.CreateAlignedStore(Observed, Fn->getArg(0), llvm::Align(1));
  auto *Known = B.CreateLoad(Pair, Global);
  B.CreateRet(B.CreateXor(B.CreateExtractValue(Known, {1}),
                          B.CreateExtractValue(Observed, {0})));
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  Source += R"(
#include <string.h>
void touch_words(void *p) {
  uint64_t x;
  memcpy(&x, p, 8); x ^= UINT64_C(0x0102030405060708); memcpy(p, &x, 8);
}
int main(void) {
  for (unsigned offset = 1; offset < 8; ++offset) {
    uint8_t src[32], dst[32], expected[32];
    memset(src, 0x73, sizeof(src)); memset(dst, 0xa5, sizeof(dst));
    memcpy(expected, dst, sizeof(dst));
    uint64_t a = UINT64_C(0xfedcba9876543210), b = UINT64_MAX - offset;
    memcpy(src + offset, &a, 8);
    uint64_t changed = a ^ UINT64_C(0x0102030405060708);
    memcpy(expected + offset, &changed, 8);
    memcpy(expected + offset + 8, &b, 8);
    if (copy_aggregate(dst + offset, src + offset, b) != (changed ^ 11)) return 1;
    if (memcmp(dst, expected, sizeof(dst))) return 2;
    uint64_t unchanged; memcpy(&unchanged, src + offset, 8);
    if (unchanged != a) return 3;
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, CompoundComparisonsPreserveConstantsAndPolarity) {
  llvm::LLVMContext Context;
  llvm::Module Module("compound-comparisons", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Signature = llvm::FunctionType::get(Word, {Word, Word}, false);
  std::string Main = R"(
int main(void) {
  const uint64_t values[] = {0, 1, 3, 5, 7, UINT64_MAX, UINT64_MAX / 2,
                            UINT64_MAX / 2 + 1};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
      uint64_t x = values[i], y = values[j];
)";
  for (unsigned Mode = 0; Mode != 6; ++Mode)
    for (unsigned Negated = 0; Negated != 4; ++Negated) {
      const std::string Name =
          "compare_" + std::to_string(Mode) + "_" + std::to_string(Negated);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
      auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
      auto *Yes = llvm::BasicBlock::Create(Context, "yes", Function);
      auto *No = llvm::BasicBlock::Create(Context, "no", Function);
      llvm::IRBuilder<llvm::NoFolder> B(Entry);
      llvm::Value *X = Function->getArg(0), *Y = Function->getArg(1);
      llvm::Value *Left = nullptr, *Right = nullptr;
      std::string LeftText, RightText;
      if (Mode == 0) {
        Left = B.CreateICmpEQ(X, B.getInt64(5));
        Right = B.CreateICmpEQ(B.CreateAnd(Y, B.getInt64(3)), B.getInt64(0));
        LeftText = "x == 5";
        RightText = "(y & 3) == 0";
      } else if (Mode == 1) {
        Left = B.CreateICmpULT(X, Y);
        Right = B.CreateICmpEQ(B.CreateSub(X, Y), B.getInt64(0));
        LeftText = "x < y";
        RightText = "x == y";
      } else if (Mode < 4) {
        Left = Mode == 2 ? B.CreateICmpSLT(X, B.getInt64(0))
                         : B.CreateICmpSLT(B.getInt64(0), X);
        Right = B.CreateICmpEQ(X, B.getInt64(0));
        LeftText = Mode == 2 ? "(x >> 63) != 0" : "(x >> 63) == 0 && x != 0";
        RightText = "x == 0";
      } else {
        auto *P = B.CreateICmpEQ(X, B.getInt64(0));
        auto *Q = B.CreateICmpEQ(Y, B.getInt64(0));
        auto *Flag = Mode == 4 ? B.CreateAnd(P, Q) : B.CreateXor(P, Q);
        auto *Wide = B.CreateZExt(Flag, Word);
        Left = B.CreateICmpSLT(Wide, B.getInt64(0));
        Right = B.CreateICmpEQ(Wide, B.getInt64(0));
        LeftText = "0"; // The widened Boolean is always nonnegative.
        RightText =
            Mode == 4 ? "!((x == 0) && (y == 0))" : "!((x == 0) != (y == 0))";
      }
      if (Negated & 1) {
        Left = B.CreateICmpEQ(Left, B.getFalse());
        LeftText = "!(" + LeftText + ")";
      }
      if (Negated & 2) {
        Right = B.CreateICmpEQ(Right, B.getFalse());
        RightText = "!(" + RightText + ")";
      }
      B.CreateCondBr(Mode >= 2 ? B.CreateOr(Left, Right)
                               : B.CreateAnd(Left, Right),
                     Yes, No);
      B.SetInsertPoint(Yes);
      B.CreateRet(B.getInt64(7));
      B.SetInsertPoint(No);
      B.CreateRet(B.getInt64(11));
      Main += "if (" + Name + "(x, y) != ((" + LeftText + ")" +
              (Mode >= 2 ? " || " : " && ") + "(" + RightText +
              ") ? 7 : 11)) return " + std::to_string(Mode * 4 + Negated + 1) +
              ";\n";
    }
  Main += "} return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, EscapedByteStringsPreserveAdjacentDigitsWhenExecuted) {
  std::string Bytes;
  for (unsigned Byte = 0; Byte < 256; ++Byte)
    for (char Digit : llvm::StringRef("0123456789abcdefABCDEF")) {
      Bytes += static_cast<char>(Byte);
      Bytes += Digit;
    }
  Bytes += "?"
           "?/"
           "?"
           "?=";
  std::string Source = "static const unsigned char actual[] = \"" +
                       neverd::escapeCString(Bytes) + "\";\n";
  Source += "static const unsigned char expected[] = {";
  for (unsigned char Byte : Bytes)
    Source += std::to_string(Byte) + ",";
  Source += "};\nint main(void) {\n"
            "if (sizeof(actual) != sizeof(expected) + 1) return 1;\n"
            "for (unsigned i = 0; i < sizeof(expected); ++i)\n"
            "  if (actual[i] != expected[i]) return 2;\n"
            "return actual[sizeof(expected)] != 0;\n}\n";
  compileAndRun(Source);
}

TEST(LLVMCValues, CrossBlockStoredExpressionsKeepExpansionBounded) {
  llvm::LLVMContext Context;
  llvm::Module Module("cross-block-home-expansion", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Signature = llvm::FunctionType::get(Word, {Word}, false);
  std::string Main = R"(
int main(void) {
  const uint64_t values[] = {0, 1, 7, UINT64_MAX, UINT64_MAX / 3};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
)";
  for (bool Reverse : {false, true})
    for (bool Derived : {false, true}) {
      const std::string Name = std::string("cross_block_") +
                               (Reverse ? "reverse_" : "forward_") +
                               (Derived ? "derived" : "direct");
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
      auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
      llvm::IRBuilder<llvm::NoFolder> B(Entry);
      auto *Slot = B.CreateAlloca(Word, nullptr, "home");
      llvm::Value *Value = B.CreateAdd(Function->getArg(0), B.getInt64(0));
      for (unsigned I = 0; I < 32; ++I) {
        auto *Next = llvm::BasicBlock::Create(Context, "next", Function);
        if (Reverse)
          Next->moveAfter(Entry);
        B.CreateBr(Next);
        B.SetInsertPoint(Next);
        // Cover both a stored foreign definition and a local expression
        // whose operand has not been measured by this block's index.
        B.CreateStore(Derived ? B.CreateAdd(Value, B.getInt64(1)) : Value,
                      Slot);
        auto *Read = B.CreateLoad(Word, Slot);
        Value = B.CreateAdd(Read, Read);
      }
      B.CreateRet(Value);
      Main += "{ uint64_t expected = values[i];\n"
              "for (unsigned n = 0; n < 32; ++n) expected = (expected + " +
              std::to_string(Derived ? 1 : 0) +
              ") * UINT64_C(2);\n"
              "if (" +
              Name + "(values[i]) != expected) return 1; }\n";
    }
  Main += "} return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  ASSERT_LT(Source.size(), 65536u);
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, MutableHomesKeepBranchValuesAndEarlierReads) {
  llvm::LLVMContext Context;
  llvm::Module Module("mutable-home-joins", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Signature = llvm::FunctionType::get(Word, {Word, Word}, false);
  for (bool Reverse : {false, true}) {
    const auto Name = Reverse ? "join_reverse" : "join_forward";
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
    auto *Left = llvm::BasicBlock::Create(Context, "left", Function);
    auto *Join = llvm::BasicBlock::Create(Context, "join", Function);
    auto *Right = llvm::BasicBlock::Create(Context, "right", Function);
    if (Reverse)
      Right->moveBefore(Left);
    llvm::IRBuilder<llvm::NoFolder> B(Entry);
    auto *Slot = B.CreateAlloca(Word, nullptr, "home");
    B.CreateStore(Function->getArg(0), Slot);
    auto *Saved = B.CreateLoad(Word, Slot, "before_branch");
    B.CreateCondBr(B.CreateICmpNE(Function->getArg(1), B.getInt64(0)), Left,
                   Right);
    B.SetInsertPoint(Left);
    B.CreateStore(B.getInt64(9), Slot);
    B.CreateBr(Join);
    B.SetInsertPoint(Right);
    B.CreateStore(B.CreateAdd(Saved, B.getInt64(1)), Slot);
    B.CreateBr(Join);
    B.SetInsertPoint(Join);
    auto *Selected = B.CreateLoad(Word, Slot, "selected");
    B.CreateStore(B.getInt64(99), Slot);
    B.CreateRet(B.CreateAdd(Selected, Saved));
  }
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  Source += R"(
int main(void) {
  const uint64_t values[] = {0, 1, 7, 99, UINT64_MAX, UINT64_MAX - 8};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned branch = 0; branch < 2; ++branch) {
      uint64_t value = values[i];
      uint64_t expected = value + (branch ? 9 : value + 1);
      if (join_forward(value, branch) != expected) return 1;
      if (join_reverse(value, branch) != expected) return 2;
    }
  return 0;
}
)";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, PartialAliasStoresCannotForwardAWholeSlotLoad) {
  llvm::LLVMContext Context;
  llvm::Module Module("mutable-home-partial-alias", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Word}, false),
      llvm::GlobalValue::ExternalLinkage, "partial_alias", Module);
  llvm::IRBuilder<llvm::NoFolder> B(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Slot = B.CreateAlloca(Word, nullptr, "home");
  B.CreateStore(Function->getArg(0), Slot);
  auto *Before = B.CreateLoad(Word, Slot, "before");
  auto *Byte = B.CreateGEP(B.getInt8Ty(), Slot, B.getInt64(1));
  B.CreateStore(B.getInt8(0x5a), Byte)->setAlignment(llvm::Align(1));
  auto *After = B.CreateLoad(Word, Slot, "after");
  B.CreateRet(B.CreateXor(Before, After));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  Source += R"(
int main(void) {
  for (unsigned byte = 0; byte < 256; ++byte) {
    uint64_t input = UINT64_C(0x123456789abc00ef) | ((uint64_t)byte << 8);
    uint64_t changed = input;
    ((unsigned char *)&changed)[1] = 0x5a;
    if (partial_alias(input) != (input ^ changed)) return 1;
  }
  return 0;
}
)";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, SelfStoredAddressPreventsLocalLoadForwarding) {
  llvm::LLVMContext Context;
  llvm::Module Module("self-stored-address", Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Pointer, {}, false),
      llvm::GlobalValue::ExternalLinkage, "self_alias", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Slot = B.CreateAlloca(Pointer, nullptr, "home");
  B.CreateStore(Slot, Slot);
  auto *Alias = B.CreateLoad(Pointer, Slot);
  B.CreateStore(llvm::ConstantPointerNull::get(Pointer), Alias);
  B.CreateRet(B.CreateLoad(Pointer, Slot));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source + "\nint main(void) { return self_alias() != 0; }\n",
                  Optimization);
}

TEST(LLVMCValues, IntegerAddressStoresPreservePointerBits) {
  llvm::LLVMContext Context;
  llvm::Module Module("integer-address-stores", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Signature = llvm::FunctionType::get(Word, {Word, Pointer}, false);
  std::string Main = R"(
int main(void) {
  _Alignas(8) uint32_t slot32;
  uint64_t slot64;
  const uint64_t values[] = {0, 1, UINT64_C(0x123456789abcdef0), UINT64_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
)";
  for (unsigned Width : {32u, 64u}) {
    auto *Integer = llvm::Type::getIntNTy(Context, Width);
    for (unsigned Alignment : {1u, 8u}) {
      const std::string Name = "address_store_" + std::to_string(Width) + "_" +
                               std::to_string(Alignment);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
      llvm::IRBuilder<llvm::NoFolder> B(
          llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64));
      Frame->setAlignment(llvm::Align(16));
      auto *Data = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(32));
      // Cover both aligned external storage and an unaligned byte backing;
      // neither proves a C effective type for these pointer-bit stores.
      llvm::Value *Slot =
          Alignment == 8 ? static_cast<llvm::Value *>(Function->getArg(1))
                         : B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(3));
      B.CreateStore(Function->getArg(0), Data)->setAlignment(llvm::Align(8));
      auto *Address = B.CreatePtrToInt(Data, Integer);
      B.CreateStore(Address, Slot)->setAlignment(llvm::Align(Alignment));
      auto *Bits = B.CreateLoad(Integer, Slot);
      Bits->setAlignment(llvm::Align(Alignment));
      if (Width == 64) {
        auto *Value = B.CreateLoad(Word, B.CreateIntToPtr(Bits, Pointer));
        Value->setAlignment(llvm::Align(8));
        B.CreateRet(Value);
      } else {
        B.CreateRet(B.CreateSelect(B.CreateICmpEQ(Bits, Address),
                                   Function->getArg(0),
                                   B.CreateNot(Function->getArg(0))));
      }
      Main += "if (" + Name + "(values[i], &slot" + std::to_string(Width) +
              ") != values[i]) return 1;\n";
    }
  }
  Main += "} return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, ByteBackingArraysRetainExplicitAlignment) {
  llvm::LLVMContext Context;
  llvm::Module Module("aligned-byte-backing", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Signature = llvm::FunctionType::get(Word, {}, false);
  std::string Main = "int main(void) {\n";
  for (unsigned Alignment : {16u, 64u, 4096u}) {
    const std::string Name = "aligned_bytes_" + std::to_string(Alignment);
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<llvm::NoFolder> B(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 33));
    Frame->setAlignment(llvm::Align(Alignment));
    B.CreateRet(
        B.CreateAnd(B.CreatePtrToInt(Frame, Word), B.getInt64(Alignment - 1)));
    Main += "if (" + Name + "() != 0) return 1;\n";
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, WideIntegerAddressStoresRetainExtendedBits) {
  llvm::LLVMContext Context;
  llvm::Module Module("wide-integer-address-stores", Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Signature = llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                            {Pointer, Pointer}, false);
  for (unsigned Alignment : {1u, 16u}) {
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage,
        "wide_address_" + std::to_string(Alignment), Module);
    llvm::IRBuilder<llvm::NoFolder> B(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Low = B.CreatePtrToInt(Function->getArg(0), B.getInt32Ty());
    auto *Extended = B.CreateSExt(Low, B.getInt128Ty());
    B.CreateStore(Extended, Function->getArg(1))
        ->setAlignment(llvm::Align(Alignment));
    B.CreateRetVoid();
  }
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  Source += R"(
int main(void) {
  const uintptr_t values[] = {0, 1, UINT32_C(0x7fffffff),
                              UINT32_C(0x80000000), UINT32_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    _Alignas(16) __uint128_t aligned = 0;
    unsigned char storage[17];
    __uint128_t copied = 0;
    wide_address_16((void *)values[i], &aligned);
    wide_address_1((void *)values[i], storage + 1);
    __builtin_memcpy(&copied, storage + 1, sizeof(copied));
    const uint64_t high = values[i] & UINT32_C(0x80000000) ? UINT64_MAX : 0;
    const uint64_t low = high ? (uint64_t)values[i] | UINT64_C(0xffffffff00000000)
                              : (uint64_t)values[i];
    if ((uint64_t)(aligned >> 64) != high || (uint64_t)aligned != low) return 1;
    if ((uint64_t)(copied >> 64) != high || (uint64_t)copied != low) return 2;
  }
  return 0;
}
)";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, ByteBackingArraysPreserveBaseAndPartialAccesses) {
  llvm::LLVMContext Context;
  llvm::Module Module("byte-backing-accesses", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Word}, false),
      llvm::GlobalValue::ExternalLinkage, "byte_backing", Module);
  llvm::IRBuilder<llvm::NoFolder> B(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 16));
  Frame->setAlignment(llvm::Align(8));
  B.CreateStore(Function->getArg(0), Frame)->setAlignment(llvm::Align(8));
  auto *Before = B.CreateLoad(Word, Frame);
  Before->setAlignment(llvm::Align(8));
  auto *Byte = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(3));
  B.CreateStore(B.getInt8(0x5a), Byte)->setAlignment(llvm::Align(1));
  auto *After = B.CreateLoad(Word, Frame);
  After->setAlignment(llvm::Align(8));
  B.CreateRet(B.CreateXor(Before, After));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  Source += R"(
int main(void) {
  for (unsigned byte = 0; byte < 256; ++byte) {
    uint64_t input = UINT64_C(0x1234567800abcdef) | ((uint64_t)byte << 24);
    uint64_t changed = input;
    ((unsigned char *)&changed)[3] = 0x5a;
    if (byte_backing(input) != (input ^ changed)) return 1;
  }
  return 0;
}
)";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, ScalarBitCountsNormalizeConstantsAndDynamicInputs) {
  llvm::LLVMContext Context;
  llvm::Module Module("bit-count-widths", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Signature = llvm::FunctionType::get(Word, {Word}, false);
  std::string Main = "int main(void) {\n";
  for (unsigned Width : {1u, 8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u}) {
    auto *Type = llvm::Type::getIntNTy(Context, Width);
    for (auto ID : {llvm::Intrinsic::ctpop, llvm::Intrinsic::ctlz,
                    llvm::Intrinsic::cttz}) {
      for (bool Constant : {false, true}) {
        const std::string Name = "count_" + std::to_string(ID) + "_" +
                                 std::to_string(Width) +
                                 (Constant ? "_c" : "_v");
        auto *Function = llvm::Function::Create(
            Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
        llvm::IRBuilder<> B(
            llvm::BasicBlock::Create(Context, "entry", Function));
        llvm::Value *Input =
            Constant ? llvm::ConstantInt::getAllOnesValue(Type)
                     : B.CreateTruncOrBitCast(Function->getArg(0), Type);
        auto *Intrinsic =
            llvm::Intrinsic::getOrInsertDeclaration(&Module, ID, {Type});
        llvm::SmallVector<llvm::Value *, 2> Args{Input};
        if (ID != llvm::Intrinsic::ctpop)
          Args.push_back(B.getFalse());
        B.CreateRet(B.CreateZExtOrTrunc(B.CreateCall(Intrinsic, Args), Word));
        Main += "{ const unsigned bits = " + std::to_string(Width) + ";\n";
        Main += "uint64_t mask = UINT64_MAX >> (64 - bits);\n";
        Main += "for (unsigned i = 0; i <= bits + 1; ++i) {\n";
        Main += "uint64_t input = i == bits + 1 ? UINT64_MAX : i == bits ? 0 : "
                "UINT64_C(1) << i;\n";
        Main += Constant ? "uint64_t value = mask;\n"
                         : "uint64_t value = input & mask;\n";
        Main += "unsigned expected = 0;\n";
        if (ID == llvm::Intrinsic::ctpop)
          Main += "for (; value; value >>= 1) expected += value & 1;\n";
        else if (ID == llvm::Intrinsic::ctlz)
          Main += "while (expected < bits && !(value & (UINT64_C(1) << (bits - "
                  "expected - 1)))) ++expected;\n";
        else
          Main += "while (expected < bits && !(value & (UINT64_C(1) << "
                  "expected))) ++expected;\n";
        Main += "if (" + Name + "(input) != expected) return 1;\n} }\n";
      }
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, WideIntegerConstantsRetainBothHalvesWhenExecuted) {
  llvm::LLVMContext Context;
  llvm::Module Module("wide-constants", Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Signature = llvm::FunctionType::get(
      llvm::Type::getVoidTy(Context),
      {llvm::Type::getInt1Ty(Context), Pointer, Pointer}, false);
  std::string Main = "int main(void) { __uint128_t actual, copy;\n";
  unsigned Index = 0;
  for (unsigned Width : {65u, 96u, 128u}) {
    for (uint64_t High :
         {uint64_t(0), uint64_t(1), uint64_t(1) << 63, ~uint64_t(0)}) {
      llvm::APInt Value(128, High);
      Value = ((Value << 64) | llvm::APInt(128, 0xfedcba9876543210ULL))
                  .trunc(Width);
      std::string Name = "constant" + std::to_string(Index++);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      // Keep a nonstandard-width local value live without changing the width
      // of a memory access: both observable stores are exactly 128 bits.
      auto *Chosen = Builder.CreateSelect(
          Function->getArg(0), llvm::ConstantInt::get(Context, Value),
          llvm::ConstantInt::get(Context, llvm::APInt(Width, 0)), "chosen");
      for (unsigned Argument : {1u, 2u})
        Builder.CreateStore(
            Builder.CreateZExtOrTrunc(Chosen, Builder.getInt128Ty()),
            Function->getArg(Argument));
      Builder.CreateRetVoid();
      auto Extended = Value.zextOrTrunc(128);
      Main += "actual = copy = 0; " + Name + "(1, &actual, &copy);\n";
      Main += "if ((uint64_t)actual != " +
              std::to_string(Extended.extractBitsAsZExtValue(64, 0)) +
              "ULL || (uint64_t)(actual >> 64) != " +
              std::to_string(Extended.extractBitsAsZExtValue(64, 64)) +
              "ULL || actual != copy) return " + std::to_string(Index) + ";\n";
      Main += Name + "(0, &actual, &copy);\n";
      Main += "if (actual || copy) return 100;\n";
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + Main);
}

TEST(LLVMCValues, FoldedIntegerViewsAndArithmeticKeepTheirWidths) {
  llvm::LLVMContext Context;
  llvm::Module Module("folded-integer-widths", Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {llvm::PointerType::getUnqual(Context)}, false);
  std::string Main = "int main(void) { __uint128_t actual;\n";
  enum class Case {
    ZeroExtend,
    SignExtend,
    Truncate,
    Add,
    Subtract,
    SignedAdd
  };
  unsigned Index = 0;
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    for (Case Kind : {Case::ZeroExtend, Case::SignExtend, Case::Truncate,
                      Case::Add, Case::Subtract, Case::SignedAdd}) {
      const std::string Name = "folded" + std::to_string(Index++);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
      // Keep the casts and arithmetic as instructions so the C emitter owns
      // their folding; IRBuilder's normal constant folder would hide a bug.
      llvm::IRBuilder<llvm::NoFolder> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Type = Builder.getIntNTy(Width);
      const llvm::APInt Ones = llvm::APInt::getAllOnes(Width);
      auto Constant = [&](const llvm::APInt &Bits) {
        return llvm::ConstantInt::get(Context, Bits);
      };
      llvm::Value *Value = nullptr;
      llvm::APInt Expected(Width, 0);
      bool Signed = false;
      switch (Kind) {
      case Case::ZeroExtend:
        Value = Constant(Ones);
        Expected = Ones;
        break;
      case Case::SignExtend:
        Expected = llvm::APInt::getSignedMinValue(Width);
        Value = Constant(Expected);
        Signed = true;
        break;
      case Case::Truncate: {
        const llvm::APInt Wide(128,
                               {0x1234567812345600ULL, 0xfedcba9876543210ULL});
        Value = Builder.CreateTrunc(Constant(Wide), Type, "narrowed");
        Expected = Wide.trunc(Width);
        break;
      }
      case Case::Add:
        Value = Builder.CreateAdd(
            Constant(Ones), Constant(llvm::APInt(Width, 1)), "wrapped_add");
        break;
      case Case::Subtract:
        Value = Builder.CreateSub(Constant(llvm::APInt(Width, 0)),
                                  Constant(llvm::APInt(Width, 1)),
                                  "wrapped_subtract");
        Expected = Ones;
        break;
      case Case::SignedAdd:
        Value = Builder.CreateAdd(
            Constant(llvm::APInt::getSignedMaxValue(Width)),
            Constant(llvm::APInt(Width, 1)), "signed_boundary");
        Expected = llvm::APInt::getSignedMinValue(Width);
        Signed = true;
        break;
      }
      Value = Signed ? Builder.CreateSExtOrTrunc(Value, Builder.getInt64Ty())
                     : Builder.CreateZExtOrTrunc(Value, Builder.getInt64Ty());
      Value = Signed ? Builder.CreateSExt(Value, Builder.getInt128Ty())
                     : Builder.CreateZExt(Value, Builder.getInt128Ty());
      Builder.CreateStore(Value, Function->getArg(0));
      Builder.CreateRetVoid();
      Expected = Signed ? Expected.sext(128) : Expected.zext(128);
      Main += Name + "(&actual);\n";
      Main += "if ((uint64_t)actual != " +
              std::to_string(Expected.extractBitsAsZExtValue(64, 0)) +
              "ULL || (uint64_t)(actual >> 64) != " +
              std::to_string(Expected.extractBitsAsZExtValue(64, 64)) +
              "ULL) return " + std::to_string(Index) + ";\n";
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + Main);
}

TEST(LLVMCValues, RuntimeIntegerCastChainsKeepTruncationAndSignedness) {
  llvm::LLVMContext Context;
  llvm::Module Module("runtime-integer-widths", Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Wide = llvm::Type::getInt128Ty(Context);
  auto *Signature = llvm::FunctionType::get(
      llvm::Type::getVoidTy(Context), {Wide, Pointer, Pointer, Pointer}, false);
  std::string Main = "int main(void) { __uint128_t zero, sign, copy;\n";
  auto Literal = [](const llvm::APInt &Value) {
    return "(((__uint128_t)" +
           std::to_string(Value.extractBitsAsZExtValue(64, 64)) +
           "ULL << 64) | " +
           std::to_string(Value.extractBitsAsZExtValue(64, 0)) + "ULL)";
  };
  for (unsigned Width :
       {1u, 7u, 8u, 9u, 16u, 31u, 32u, 63u, 64u, 65u, 96u, 127u}) {
    const std::string Name = "cast" + std::to_string(Width);
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<llvm::NoFolder> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Narrowed = Builder.CreateTrunc(Function->getArg(0),
                                         Builder.getIntNTy(Width), "narrowed");
    auto *Zero = Builder.CreateZExt(Narrowed, Wide, "zero_extended");
    auto *Sign = Builder.CreateSExt(Narrowed, Wide, "sign_extended");
    Builder.CreateStore(Zero, Function->getArg(1));
    Builder.CreateStore(Sign, Function->getArg(2));
    Builder.CreateStore(Zero, Function->getArg(3));
    Builder.CreateRetVoid();
    for (const llvm::APInt &Input :
         {llvm::APInt(128, 0), llvm::APInt::getAllOnes(128),
          llvm::APInt::getOneBitSet(128, Width - 1),
          llvm::APInt::getOneBitSet(128, Width)}) {
      const llvm::APInt Bits = Input.trunc(Width);
      Main += Name + "(" + Literal(Input) + ", &zero, &sign, &copy);\n";
      Main += "if (zero != " + Literal(Bits.zext(128)) +
              " || sign != " + Literal(Bits.sext(128)) +
              " || copy != zero) return 1;\n";
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + Main);
}

TEST(LLVMCValues, SwitchCasesPreserveTheirSelectorBitPattern) {
  llvm::LLVMContext Context;
  llvm::Module Module("switch-bit-patterns", Context);
  auto *Wide = llvm::Type::getInt128Ty(Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), {Wide}, false);
  auto Literal = [](const llvm::APInt &Value) {
    const auto WideValue = Value.zextOrTrunc(128);
    return "(((__uint128_t)" +
           std::to_string(WideValue.extractBitsAsZExtValue(64, 64)) +
           "ULL << 64) | " +
           std::to_string(WideValue.extractBitsAsZExtValue(64, 0)) + "ULL)";
  };
  std::string Main = "int main(void) {\n";
  for (unsigned Width :
       {1u, 2u, 7u, 8u, 9u, 16u, 31u, 32u, 63u, 64u, 65u, 96u, 127u, 128u}) {
    const std::string Name = "switch_bits" + std::to_string(Width);
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
    auto *Default = llvm::BasicBlock::Create(Context, "otherwise", Function);
    llvm::IRBuilder<llvm::NoFolder> Builder(Entry);
    auto *Selector = Builder.CreateZExtOrTrunc(
        Function->getArg(0), Builder.getIntNTy(Width), "selector");
    auto *Switch = Builder.CreateSwitch(Selector, Default);
    const auto HighBit = llvm::APInt::getSignedMinValue(Width);
    const auto Ones = llvm::APInt::getAllOnes(Width);
    const auto AddCase = [&](const llvm::APInt &Value, unsigned Result) {
      auto *Block = llvm::BasicBlock::Create(Context, "matched", Function);
      Switch->addCase(llvm::ConstantInt::get(Context, Value), Block);
      Builder.SetInsertPoint(Block);
      Builder.CreateRet(Builder.getInt32(Result));
    };
    AddCase(llvm::APInt(Width, 0), 11);
    AddCase(HighBit, 23);
    if (HighBit != Ones)
      AddCase(Ones, 37);
    Builder.SetInsertPoint(Default);
    Builder.CreateRet(Builder.getInt32(49));

    // Exercise the sign boundary, every i2 value, the default arm, and upper
    // input bits discarded by the selector. Expected tags use APInt equality,
    // independently of the C case-label spelling and integer promotions.
    for (const llvm::APInt &Input :
         {llvm::APInt(128, 0), llvm::APInt(128, 1), llvm::APInt(128, 2),
          llvm::APInt(128, 3), HighBit.zextOrTrunc(128),
          (HighBit - 1).zextOrTrunc(128), (HighBit + 1).zextOrTrunc(128),
          Ones.zextOrTrunc(128), llvm::APInt::getAllOnes(128)}) {
      const auto Bits = Input.zextOrTrunc(Width);
      const unsigned Expected = Bits.isZero()     ? 11
                                : Bits == HighBit ? 23
                                : Bits == Ones    ? 37
                                                  : 49;
      Main += "if (" + Name + "(" + Literal(Input) +
              ") != " + std::to_string(Expected) + ") return 1;\n";
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, EqualityPreservesNarrowBitsAcrossEveryComparisonSurface) {
  llvm::LLVMContext Context;
  llvm::Module Module("comparison-bit-patterns", Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I128 = llvm::Type::getInt128Ty(Context);
  auto *Signature = llvm::FunctionType::get(
      I32, {I128, llvm::PointerType::getUnqual(Context)}, false);
  auto Literal = [](const llvm::APInt &Value) {
    const auto Wide = Value.zextOrTrunc(128);
    return "(((__uint128_t)" +
           std::to_string(Wide.extractBitsAsZExtValue(64, 64)) +
           "ULL << 64) | " +
           std::to_string(Wide.extractBitsAsZExtValue(64, 0)) + "ULL)";
  };
  std::string Main = "int main(void) { uint32_t observed;\n";
  unsigned Index = 0;
  for (unsigned Width : {1u, 2u, 3u, 8u, 16u, 31u, 33u, 65u, 127u}) {
    const auto High = llvm::APInt::getSignedMinValue(Width);
    for (const auto &Constant : {llvm::APInt(Width, 0), High})
      for (bool Equal : {false, true})
        for (bool Reversed : {false, true})
          for (unsigned Surface = 0; Surface != 3; ++Surface) {
            const std::string Name = "compare_bits" + std::to_string(Index++);
            auto *Function = llvm::Function::Create(
                Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
            llvm::IRBuilder<llvm::NoFolder> Builder(
                llvm::BasicBlock::Create(Context, "entry", Function));
            auto *Type = Builder.getIntNTy(Width);
            auto *Narrow = Builder.CreateTrunc(Function->getArg(0), Type);
            // Keep modular wrap visible to comparison spelling, including
            // the EQ/NE-zero condition shortcuts.
            auto *Wrapped = Builder.CreateAdd(
                Narrow, llvm::ConstantInt::get(Type, 1), "wrapped");
            llvm::Value *LHS = Wrapped;
            llvm::Value *RHS = llvm::ConstantInt::get(Context, Constant);
            if (Reversed)
              std::swap(LHS, RHS);
            auto *Compare = Equal ? Builder.CreateICmpEQ(LHS, RHS)
                                  : Builder.CreateICmpNE(LHS, RHS);
            if (Surface == 2) {
              // Only the false edge has an effect. The structured writer
              // renders an inverted condition before the shared return.
              auto *Done = llvm::BasicBlock::Create(Context, "done", Function);
              auto *Miss = llvm::BasicBlock::Create(Context, "miss", Function);
              Builder.CreateCondBr(Compare, Done, Miss);
              Builder.SetInsertPoint(Miss);
              Builder.CreateStore(Builder.getInt32(13), Function->getArg(1));
              Builder.CreateBr(Done);
              Builder.SetInsertPoint(Done);
              Builder.CreateRet(Builder.getInt32(41));
            } else {
              llvm::Value *Selected = Builder.CreateSelect(
                  Compare, Builder.getInt32(41), Builder.getInt32(13));
              if (Surface == 1)
                // Two uses force an assigned ICmp instead of inline text.
                Selected = Builder.CreateAdd(Selected,
                                             Builder.CreateZExt(Compare, I32));
              Builder.CreateRet(Selected);
            }
            for (const auto &Input :
                 {llvm::APInt(128, 0), llvm::APInt(128, 1), llvm::APInt(128, 2),
                  llvm::APInt(128, 3), llvm::APInt(128, 7), High.zext(128),
                  (High - 1).zext(128), (High + 1).zext(128),
                  llvm::APInt::getAllOnes(128)}) {
              const auto Bits = Input.trunc(Width) + 1;
              const bool Matches = (Bits == Constant) == Equal;
              const unsigned Returned = Surface == 2 ? 41
                                        : Matches    ? (Surface == 1 ? 42 : 41)
                                                     : 13;
              const unsigned Stored = Surface == 2 && !Matches ? 13 : 41;
              Main += "observed = 41; if (" + Name + "(" + Literal(Input) +
                      ", &observed) != " + std::to_string(Returned) +
                      " || observed != " + std::to_string(Stored) +
                      ") return 1;\n";
            }
          }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization.str());
    compileAndRun(Source + Main, Optimization);
  }
}

TEST(LLVMCValues, BooleanExtensionDoesNotPeelArithmeticNormalization) {
  llvm::LLVMContext Context;
  llvm::Module Module("boolean-arithmetic-view", Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *Signature = llvm::FunctionType::get(I32, {I32}, false);
  std::string Main = "int main(void) { for (uint32_t x = 0; x < 16; ++x) {\n";
  for (bool Equal : {false, true}) {
    const std::string Name = Equal ? "wrapped_eq" : "wrapped_ne";
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<llvm::NoFolder> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Sum = Builder.CreateAdd(
        Builder.CreateTrunc(Function->getArg(0), Builder.getInt1Ty()),
        Builder.getInt1(true));
    auto *Extended = Builder.CreateZExt(Sum, Builder.getInt8Ty());
    auto *Compare = Equal ? Builder.CreateICmpEQ(Extended, Builder.getInt8(0))
                          : Builder.CreateICmpNE(Extended, Builder.getInt8(0));
    auto *True = llvm::BasicBlock::Create(Context, "true", Function);
    auto *False = llvm::BasicBlock::Create(Context, "false", Function);
    Builder.CreateCondBr(Compare, True, False);
    Builder.SetInsertPoint(True);
    Builder.CreateRet(Builder.getInt32(1));
    Builder.SetInsertPoint(False);
    Builder.CreateRet(Builder.getInt32(0));
    Main += "if (" + Name + "(x) != " + (Equal ? "(x & 1)" : "((x & 1) ^ 1)") +
            ") return 1;\n";
  }
  Main += "} return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, BooleanFlagViewsKeepOneObservableCall) {
  llvm::LLVMContext Context;
  llvm::Module Module("boolean-call-effects", Context);
  auto *I8 = llvm::Type::getInt8Ty(Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Next = llvm::Function::Create(llvm::FunctionType::get(I64, {}, false),
                                      llvm::GlobalValue::ExternalLinkage,
                                      "next_value", Module);
  for (bool Invert : {false, true}) {
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(I32, {}, false),
        llvm::GlobalValue::ExternalLinkage,
        Invert ? "inverted_flags" : "direct_flags", Module);
    llvm::IRBuilder<llvm::NoFolder> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Home = Builder.CreateAlloca(I32);
    Builder.CreateStore(Builder.CreateTrunc(Builder.CreateCall(Next), I32),
                        Home);
    auto *Word = Builder.CreateAnd(Builder.CreateLoad(I32, Home),
                                   Builder.CreateLoad(I32, Home));
    auto *Zero = Builder.CreateICmpEQ(Word, Builder.getInt32(0));
    auto *Sign = Builder.CreateICmpSLT(Word, Builder.getInt32(0));
    auto *Flags = Builder.CreateOr(Builder.CreateZExt(Zero, I8),
                                   Builder.CreateZExt(Sign, I8));
    auto *Test = Invert ? Builder.CreateICmpEQ(Flags, Builder.getInt8(0))
                        : Builder.CreateICmpNE(Flags, Builder.getInt8(0));
    auto *True = llvm::BasicBlock::Create(Context, "true", Function);
    auto *False = llvm::BasicBlock::Create(Context, "false", Function);
    Builder.CreateCondBr(Test, True, False);
    Builder.SetInsertPoint(True);
    Builder.CreateRet(Builder.getInt32(1));
    Builder.SetInsertPoint(False);
    Builder.CreateRet(Builder.getInt32(0));
  }
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  const std::string Main = R"(
static uint64_t supplied;
static unsigned calls;
uint64_t next_value(void) { ++calls; return supplied; }
int main(void) {
  const uint64_t values[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff,
                            0x100000000ULL, 0x180000000ULL};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    supplied = values[i];
    uint32_t bits = (uint32_t)supplied;
    unsigned nonpositive = bits == 0 || (bits & 0x80000000U) != 0;
    calls = 0;
    if (direct_flags() != nonpositive || calls != 1) return 1;
    calls = 0;
    if (inverted_flags() != !nonpositive || calls != 1) return 2;
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, OrderedComparisonsUseTheirLLVMWidthAndSignedness) {
  llvm::LLVMContext Context;
  llvm::Module Module("ordered-comparison-widths", Context);
  auto *I128 = llvm::Type::getInt128Ty(Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), {I128}, false);
  auto Literal = [](const llvm::APInt &Value) {
    const auto Wide = Value.zextOrTrunc(128);
    return "(((__uint128_t)" +
           std::to_string(Wide.extractBitsAsZExtValue(64, 64)) +
           "ULL << 64) | " +
           std::to_string(Wide.extractBitsAsZExtValue(64, 0)) + "ULL)";
  };
  std::string Main = "int main(void) {\n";
  unsigned Index = 0;
  for (unsigned Width :
       {1u, 2u, 3u, 8u, 16u, 31u, 32u, 33u, 64u, 65u, 127u, 128u}) {
    const auto High = llvm::APInt::getSignedMinValue(Width);
    for (auto Predicate : {llvm::CmpInst::ICMP_ULT, llvm::CmpInst::ICMP_UGT,
                           llvm::CmpInst::ICMP_SLT, llvm::CmpInst::ICMP_SGT}) {
      const std::string Name = "ordered_bits" + std::to_string(Index++);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
      llvm::IRBuilder<llvm::NoFolder> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Value = Builder.CreateZExtOrTrunc(Function->getArg(0),
                                              Builder.getIntNTy(Width));
      Builder.CreateRet(Builder.CreateZExt(
          Builder.CreateICmp(Predicate, Value,
                             llvm::ConstantInt::get(Context, High)),
          Builder.getInt32Ty()));
      for (const auto &Input :
           {llvm::APInt(128, 0), llvm::APInt(128, 1), High.zextOrTrunc(128),
            (High - 1).zextOrTrunc(128), (High + 1).zextOrTrunc(128),
            llvm::APInt::getAllOnes(128)}) {
        const auto Bits = Input.zextOrTrunc(Width);
        const bool Expected =
            Predicate == llvm::CmpInst::ICMP_ULT   ? Bits.ult(High)
            : Predicate == llvm::CmpInst::ICMP_UGT ? Bits.ugt(High)
            : Predicate == llvm::CmpInst::ICMP_SLT ? Bits.slt(High)
                                                   : Bits.sgt(High);
        Main += "if (" + Name + "(" + Literal(Input) +
                ") != " + std::to_string(Expected) + ") return 1;\n";
      }
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization.str());
    compileAndRun(Source + Main, Optimization);
  }
}

TEST(LLVMCValues, TruncatedPredicatesTestOnlyTheirLowBits) {
  llvm::LLVMContext Context;
  llvm::Module Module("truncated-predicate", Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                              {llvm::Type::getInt64Ty(Context)}, false);
  auto *Function = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "low_bit", Module);
  llvm::IRBuilder<llvm::NoFolder> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Set = llvm::BasicBlock::Create(Context, "set", Function);
  auto *Clear = llvm::BasicBlock::Create(Context, "clear", Function);
  Builder.CreateCondBr(
      Builder.CreateTrunc(Function->getArg(0), Builder.getInt1Ty()), Set,
      Clear);
  Builder.SetInsertPoint(Set);
  Builder.CreateRet(Builder.getInt32(1));
  Builder.SetInsertPoint(Clear);
  Builder.CreateRet(Builder.getInt32(0));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + "int main(void) { return low_bit(0) != 0 || "
                         "low_bit(1) != 1 || low_bit(2) != 0 || "
                         "low_bit(UINT64_MAX) != 1; }\n");
}

TEST(LLVMCValues, ForwardedBitwiseOperandsRetainComparisonGrouping) {
  llvm::LLVMContext Context;
  llvm::Module Module("forwarded-bitwise-comparisons", Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Signature = llvm::FunctionType::get(I64, {I64, I64}, false);
  const llvm::Instruction::BinaryOps Operators[] = {
      llvm::Instruction::And, llvm::Instruction::Or, llvm::Instruction::Xor};
  unsigned Index = 0;
  for (auto Operator : Operators) {
    for (bool Equal : {false, true}) {
      for (bool Reversed : {false, true}) {
        auto *Function = llvm::Function::Create(
            Signature, llvm::GlobalValue::ExternalLinkage,
            "grouped" + std::to_string(Index++), Module);
        llvm::IRBuilder<llvm::NoFolder> Builder(
            llvm::BasicBlock::Create(Context, "entry", Function));
        auto *Home = Builder.CreateAlloca(I64);
        Builder.CreateStore(
            Builder.CreateBinOp(Operator, Function->getArg(0),
                                llvm::ConstantInt::get(I64, 0x24)),
            Home);
        llvm::Value *Left = Builder.CreateLoad(I64, Home);
        llvm::Value *Right = Function->getArg(1);
        if (Reversed)
          std::swap(Left, Right);
        auto *Compare = Equal ? Builder.CreateICmpEQ(Left, Right)
                              : Builder.CreateICmpNE(Left, Right);
        Builder.CreateRet(Builder.CreateZExt(Compare, I64));
      }
    }
  }
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  std::string Main = R"(
int main(void) {
  const uint64_t inputs[] = {0, 1, 4, 0x20, 0x24, 0x25, UINT64_MAX};
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    uint64_t x = inputs[i];
    for (unsigned j = 0; j < sizeof(inputs) / sizeof(inputs[0]); ++j) {
      uint64_t y = inputs[j];
)";
  Index = 0;
  for (const char *Operator : {"&", "|", "^"})
    for (const char *Compare : {"!=", "=="})
      for (unsigned Reversed = 0; Reversed != 2; ++Reversed)
        Main += "if (grouped" + std::to_string(Index++) + "(x, y) != ((x " +
                Operator + " 0x24) " + Compare + " y)) return 1;\n";
  Main += "} } return 0; }\n";
  compileAndRun(Source + Main);
}

TEST(LLVMCValues, ForwardedHomesKeepNarrowSignAndWideShiftSemantics) {
  llvm::LLVMContext Context;
  llvm::Module Module("forwarded-home-widths", Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Callee = llvm::Function::Create(
      llvm::FunctionType::get(I64, {I64}, false),
      llvm::GlobalValue::ExternalLinkage, "source_bits", Module);
  llvm::IRBuilder<llvm::NoFolder> CalleeBuilder(
      llvm::BasicBlock::Create(Context, "entry", Callee));
  CalleeBuilder.CreateRet(Callee->getArg(0));

  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {I64, I64, Pointer, Pointer, Pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "home_widths", Module);
  llvm::IRBuilder<llvm::NoFolder> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *WideHome = Builder.CreateAlloca(I64, nullptr, "wide_home");
  auto *NarrowHome = Builder.CreateAlloca(I32, nullptr, "narrow_home");
  auto *ReloadHome = Builder.CreateAlloca(I64, nullptr, "reload_home");
  auto *Call = Builder.CreateCall(Callee, {Function->getArg(0)}, "call_bits");
  Builder.CreateStore(Call, WideHome);
  auto *Narrow = Builder.CreateTrunc(Builder.CreateLoad(I64, WideHome), I32);
  Builder.CreateStore(Narrow, NarrowHome);
  auto *Reloaded = Builder.CreateLoad(I32, NarrowHome);
  Builder.CreateStore(Builder.CreateSExt(Reloaded, I64), Function->getArg(2));
  Builder.CreateStore(Builder.CreateZExt(Reloaded, I64), ReloadHome);
  auto *High = Builder.CreateLShr(Builder.CreateLoad(I64, ReloadHome),
                                  llvm::ConstantInt::get(I64, 32));
  Builder.CreateStore(High, Function->getArg(3));
  auto *Other = Builder.CreateTrunc(Function->getArg(1), I32);
  Builder.CreateStore(Builder.CreateAdd(Reloaded, Other), Function->getArg(4));
  Builder.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));

  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
int main(void) {
  const uint64_t inputs[] = {
      0, 0x7fffffffULL, 0x80000000ULL, 0xffffffffULL,
      0x1234567800000001ULL, 0x1234567880000001ULL, UINT64_MAX};
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    uint64_t sign = 0, high = 1;
    uint32_t sum = 0;
    const uint32_t low = (uint32_t)inputs[i];
    const uint64_t expected_sign =
        low & 0x80000000U ? (uint64_t)low | 0xffffffff00000000ULL : low;
    home_widths(inputs[i], 0xfedcba98ffffffffULL, &sign, &high, &sum);
    if (sign != expected_sign || high != 0 || sum != low - 1U)
      return 1;
  }
  return 0;
}
)");
}

TEST(LLVMCValues, WideIntegerPointerConstantKeepsItsPointerWidth) {
  llvm::LLVMContext Context;
  llvm::Module Module("wide-integer-pointer", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Signature = llvm::FunctionType::get(Pointer, false);
  auto *Function = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "wide_pointer", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::APInt Address(128, 1);
  Address = (Address << 96) | llvm::APInt(128, 0x1234);
  Builder.CreateRet(llvm::ConstantExpr::getIntToPtr(
      llvm::ConstantInt::get(Context, Address), Pointer));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(
      Source +
      "int main(void) { return (uintptr_t)wide_pointer() != 0x1234; }\n");
}

TEST(LLVMCValues, WideIntegerConstantsPreserveEveryWordAndStoreExtent) {
  for (unsigned Width : {160U, 256U, 288U, 512U}) {
    llvm::LLVMContext Context;
    llvm::Module Module("wide-constant", Context);
    auto *Signature =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                {llvm::PointerType::getUnqual(Context)}, false);
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, "wide_store", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    llvm::APInt Value(Width, 0);
    for (unsigned Bit : {0U, 65U, 130U, Width - 1})
      Value.setBit(Bit);
    auto *Store = Builder.CreateStore(llvm::ConstantInt::get(Context, Value),
                                      Function->getArg(0));
    Store->setAlignment(llvm::Align(1));
    Builder.CreateRetVoid();
    auto *CopySignature =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                {llvm::PointerType::getUnqual(Context),
                                 llvm::PointerType::getUnqual(Context)},
                                false);
    auto *Copy = llvm::Function::Create(
        CopySignature, llvm::GlobalValue::ExternalLinkage, "wide_copy", Module);
    Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Copy));
    auto *Loaded =
        Builder.CreateLoad(Builder.getIntNTy(Width), Copy->getArg(0));
    Loaded->setAlignment(llvm::Align(1));
    Builder.CreateStore(Loaded, Copy->getArg(1))->setAlignment(llvm::Align(1));
    Builder.CreateRetVoid();
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
    std::string Driver = R"(
#include <string.h>
int main(void) {
  unsigned char storage[96], expected[96];
  memset(storage, 0x59, sizeof(storage));
  memcpy(expected, storage, sizeof(storage));
)";
    Driver += "  memset(expected + 1, 0, " + std::to_string(Width / 8) + ");\n";
    for (unsigned Bit : {0U, 65U, 130U, Width - 1})
      Driver += "  expected[" + std::to_string(1 + Bit / 8) +
                "] |= " + std::to_string(1U << (Bit % 8)) + ";\n";
    Driver += "  wide_store(storage + 1);\n"
              "  if (memcmp(storage, expected, sizeof(storage))) return 1;\n"
              "  unsigned char copy[96], expected_copy[96];\n"
              "  memset(copy, 0xa5, sizeof(copy));\n"
              "  memcpy(expected_copy, copy, sizeof(copy));\n";
    Driver += "  memcpy(expected_copy + 3, expected + 1, " +
              std::to_string(Width / 8) +
              ");\n"
              "  wide_copy(storage + 1, copy + 3);\n"
              "  return memcmp(copy, expected_copy, sizeof(copy)) != 0;\n}\n";
    for (const char *Optimization : {"-O0", "-O2"})
      compileAndRun(Source + Driver, Optimization, {}, true);
  }
}

TEST(LLVMCValues, ConstantsBeyondTheSupportedCarrierAreRejected) {
  llvm::LLVMContext Context;
  llvm::Module Module("unsupported-constant", Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {llvm::PointerType::getUnqual(Context)}, false);
  auto *Function = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "wide_store", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateStore(llvm::ConstantInt::get(Context, llvm::APInt(1024, 17)),
                      Function->getArg(0));
  Builder.CreateRetVoid();
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
               std::invalid_argument);
}

TEST(LLVMCValues, GlobalByteViewsUseObjectAddresses) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
@global_bits = global i64 0
define i64 @global_roundtrip(i64 %value) {
entry:
  store i64 %value, ptr @global_bits
  store i16 43981, ptr @global_bits, align 1
  %after = load i64, ptr @global_bits
  ret i64 %after
}
define i16 @global_low() {
entry:
  %low = load i16, ptr @global_bits, align 1
  ret i16 %low
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module);
  for (bool Unaligned : {false, true}) {
    SCOPED_TRACE(Unaligned);
    neverd::CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    Options.UseUnalignedPointers = Unaligned;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(*Module, OS, Options));
    Source += R"(
#include <string.h>
int main(void) {
  const uint64_t inputs[] = {0, UINT64_MAX, UINT64_C(0x1122334455667788)};
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    uint64_t expected = inputs[i];
    uint16_t low = 0xabcd;
    memcpy(&expected, &low, sizeof(low));
    if (global_roundtrip(inputs[i]) != expected || global_low() != low)
      return 1;
  }
  return 0;
}
)";
    for (const char *Optimization : {"-O0", "-O2"})
      compileAndRun(Source, Optimization, {}, true);
  }
}

TEST(LLVMCValues, ReloadedFieldGuardSurvivesAnInterveningCall) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
declare i32 @IsKind(ptr)
define i32 @reload_guard(i64 %self) {
entry:
  %home = alloca i64
  %first = alloca i64
  %second = alloca i64
  store i64 %self, ptr %home
  %addr1 = add i64 %self, 8
  %ptr1 = inttoptr i64 %addr1 to ptr
  %value1 = load i64, ptr %ptr1
  store i64 %value1, ptr %first
  %snapshot1 = load i64, ptr %first
  %zero1 = icmp eq i64 %snapshot1, 0
  br i1 %zero1, label %done, label %check
check:
  %old = load i64, ptr %first
  %oldptr = inttoptr i64 %old to ptr
  %kind = call i32 @IsKind(ptr %oldptr)
  %is_kind = icmp ne i32 %kind, 0
  br i1 %is_kind, label %retest, label %done
retest:
  %base = load i64, ptr %home
  %addr2 = add i64 %base, 8
  %ptr2 = inttoptr i64 %addr2 to ptr
  %value2 = load i64, ptr %ptr2
  store i64 %value2, ptr %second
  %snapshot2 = load i64, ptr %second
  %zero2 = icmp eq i64 %snapshot2, 0
  br i1 %zero2, label %done, label %again
again:
  %new = load i64, ptr %second
  %newptr = inttoptr i64 %new to ptr
  %kind2 = call i32 @IsKind(ptr %newptr)
  ret i32 %kind2
done:
  ret i32 0
}
define i32 @reload_guard_call_before_test(i64 %self) {
entry:
  %address = add i64 %self, 8
  %pointer = inttoptr i64 %address to ptr
  %first = load i64, ptr %pointer
  %oldptr = inttoptr i64 %first to ptr
  call i32 @IsKind(ptr %oldptr)
  %nonzero = icmp ne i64 %first, 0
  br i1 %nonzero, label %retest, label %done
retest:
  %second = load i64, ptr %pointer
  %nonzero2 = icmp ne i64 %second, 0
  br i1 %nonzero2, label %again, label %done
again:
  %newptr = inttoptr i64 %second to ptr
  %kind = call i32 @IsKind(ptr %newptr)
  ret i32 %kind
done:
  ret i32 0
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(*Module, OS, Options));
  Source += R"(
static uint64_t object[2];
static unsigned calls;
uint32_t IsKind(void *p) {
  ++calls;
  object[1] = 0;
  return p != 0 ? 1 : 17;
}
int main(void) {
  object[1] = 0;
  if (reload_guard((uintptr_t)object) || calls) return 1;
  object[1] = (uintptr_t)object;
  if (reload_guard((uintptr_t)object) || calls != 1) return 2;
  object[1] = (uintptr_t)object;
  calls = 0;
  if (reload_guard_call_before_test((uintptr_t)object) || calls != 1) return 3;
  return 0;
}
)";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization, {}, true);
}

TEST(LLVMCValues, SpecializedCallTailUsesTheIncomingObjectOnEachEdge) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
declare void @work_a()
declare void @work_b()
declare void @destroy_one(ptr)
declare void @release_name()
define void @cleanup_tail(i1 %flag) {
entry:
  %slot = alloca ptr
  %object_a = alloca i64
  %object_b = alloca i64
  store i64 11, ptr %object_a
  store i64 22, ptr %object_b
  br i1 %flag, label %arm_a, label %arm_b
arm_a:
  call void @work_a()
  store ptr %object_a, ptr %slot
  br label %tail
arm_b:
  call void @work_b()
  store ptr %object_b, ptr %slot
  br label %tail
tail:
  %selected = load ptr, ptr %slot
  call void @destroy_one(ptr %selected)
  br label %epi
epi:
  call void @release_name()
  ret void
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(*Module, OS, {}));
  Source += R"(
#include <string.h>
static unsigned events;
static uint64_t selected;
void work_a(void) { events = events * 10 + 1; }
void work_b(void) { events = events * 10 + 2; }
void destroy_one(void *p) {
  memcpy(&selected, p, sizeof(selected));
  events = events * 10 + 3;
}
void release_name(void) { events = events * 10 + 4; }
int main(void) {
  cleanup_tail(0);
  if (events != 234 || selected != 22) return 1;
  events = 0;
  cleanup_tail(1);
  if (events != 134 || selected != 11) return 2;
  return 0;
}
)";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization, {}, true);
}

TEST(LLVMCValues, ConstantHomesStayInitializedAcrossRepeatedDiamonds) {
  llvm::LLVMContext Context;
  llvm::Module Module("constant-home-diamonds", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Word}, false),
      llvm::GlobalValue::ExternalLinkage, "constant_diamonds", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Home = B.CreateAlloca(Word);
  B.CreateStore(B.getInt64(7), Home);
  for (unsigned I = 0; I < 20; ++I) {
    auto *Next = B.CreateAlloca(Word);
    auto *Left = llvm::BasicBlock::Create(Context, "left", Function);
    auto *Join = llvm::BasicBlock::Create(Context, "join", Function);
    auto *Right = llvm::BasicBlock::Create(Context, "right", Function);
    B.CreateCondBr(B.CreateICmpNE(Function->getArg(0), B.getInt64(0)), Left,
                   Right);
    for (auto *Arm : {Left, Right}) {
      B.SetInsertPoint(Arm);
      B.CreateStore(B.CreateLoad(Word, Home), Next);
      B.CreateBr(Join);
    }
    B.SetInsertPoint(Join);
    Home = Next;
  }
  B.CreateRet(B.CreateLoad(Word, Home));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, OS, {}));
  Source += "\nint main(void) { return constant_diamonds(0) != 7 || "
            "constant_diamonds(1) != 7; }\n";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization, {}, true);
}

TEST(LLVMCValues, PreservedIndirectCallsKeepSignatureAndABI) {
  for (auto Convention : {llvm::CallingConv::C, llvm::CallingConv::Win64,
                          llvm::CallingConv::X86_64_SysV}) {
    SCOPED_TRACE(Convention);
    llvm::LLVMContext C;
    llvm::Module M("indirect-call-abi", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> B(C);
    auto *Type =
        llvm::FunctionType::get(B.getInt64Ty(),
                                {B.getInt8Ty(), B.getInt32Ty(), B.getInt64Ty(),
                                 B.getInt64Ty(), B.getInt64Ty()},
                                false);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy()}, false),
        llvm::GlobalValue::ExternalLinkage, "call_pointer", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    auto *Call =
        B.CreateCall(Type, Fn->getArg(0),
                     {B.getInt8(0xf1), B.getInt32(0x87654321),
                      B.getInt64(UINT64_C(0x8000000000000017)), B.getInt64(29),
                      B.getInt64(UINT64_C(0xfedcba9876543210))});
    Call->setCallingConv(Convention);
    B.CreateRet(Call);
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    neverd::CEmitterOptions Options;
    Options.TheArch = neverd::Arch::X64;
    Options.PreserveLLVMFunctionTypes = true;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(M, OS, Options));
    const char *Attribute = Convention == llvm::CallingConv::Win64
                                ? "__attribute__((ms_abi)) "
                            : Convention == llvm::CallingConv::X86_64_SysV
                                ? "__attribute__((sysv_abi)) "
                                : "";
    if (*Attribute)
      EXPECT_NE(Text.find(Attribute), std::string::npos) << Text;
    // An independently written callee makes wrong widths, register assignment,
    // stack arguments and repeated calls observable. The fifth Win64 argument
    // is passed on the stack, while SysV passes it in a register.
    const std::string Program = Text + "\nstatic unsigned calls;\nuint64_t " +
                                Attribute +
                                R"(target(uint8_t a, uint32_t b, uint64_t c,
                              uint64_t d, uint64_t e) {
  ++calls;
  if (a != 0xf1 || b != UINT32_C(0x87654321) ||
      c != UINT64_C(0x8000000000000017) || d != 29 ||
      e != UINT64_C(0xfedcba9876543210)) return 0;
  return UINT64_C(0xdeadbeef80000001);
}
int main(void) {
  return call_pointer((void *)target) != UINT64_C(0xdeadbeef80000001) ||
         calls != 1;
}
)";
    if (llvm::Triple(llvm::sys::getDefaultTargetTriple()).getArch() ==
        llvm::Triple::x86_64)
      for (const char *Level : {"-O0", "-O2"})
        compileAndRun(Program, Level);
  }
}

TEST(LLVMCValues, PreservedIndirectVoidCallHasExplicitEmptyPrototype) {
  llvm::LLVMContext C;
  llvm::Module M("indirect-void-call", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "call_void_pointer", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  B.CreateCall(llvm::FunctionType::get(B.getVoidTy(), false), Fn->getArg(0));
  B.CreateRetVoid();
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(M, OS, Options));
  EXPECT_NE(Text.find("(*)(void)"), std::string::npos) << Text;
  const auto Program = Text + R"(
static unsigned calls;
void target(void) { ++calls; }
int main(void) { call_void_pointer((void *)target); return calls != 1; }
)";
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Program, Level);
}

TEST(LLVMCValues, PreservedIndirectCallRejectsUnknownConvention) {
  llvm::LLVMContext C;
  llvm::Module M("indirect-unsupported-abi", C);
  llvm::IRBuilder<> B(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "call_unknown_abi", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Call = B.CreateCall(llvm::FunctionType::get(B.getInt64Ty(), false),
                            Fn->getArg(0));
  Call->setCallingConv(llvm::CallingConv::Fast);
  B.CreateRet(Call);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  EXPECT_THROW(neverd::LLVMCEmitter().emit(M, OS, Options), std::runtime_error);
}

TEST(LLVMCValues, PreservedIndirectCallsKeepNarrowReturns) {
  for (unsigned Bits : {8u, 16u, 32u}) {
    SCOPED_TRACE(Bits);
    llvm::LLVMContext C;
    llvm::Module M("indirect-narrow-return", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy(), B.getInt64Ty()},
                                false),
        llvm::GlobalValue::ExternalLinkage, "call_narrow", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    auto *Call = B.CreateCall(
        llvm::FunctionType::get(B.getIntNTy(Bits), {B.getInt64Ty()}, false),
        Fn->getArg(0), {Fn->getArg(1)});
    B.CreateRet(B.CreateZExt(Call, B.getInt64Ty()));
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    neverd::CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(M, OS, Options));
    const auto Program = Text + "\nuint" + std::to_string(Bits) +
                         "_t target(uint64_t x) { return (uint" +
                         std::to_string(Bits) +
                         "_t)(x ^ UINT64_C(0x87654321fedcba98)); }\n" + R"(
int main(void) {
  const uint64_t values[] = {0, 1, UINT64_MAX, UINT64_C(1) << 63};
  for (unsigned i = 0; i < 4; ++i)
    if (call_narrow((void *)target, values[i]) != (uint64_t)target(values[i]))
      return 1;
  return 0;
}
)";
    for (const char *Level : {"-O0", "-O2"})
      compileAndRun(Program, Level);
  }
}

TEST(LLVMCValues, PreservedIndirectVariadicCallKeepsFixedPrototype) {
  llvm::LLVMContext C;
  llvm::Module M("indirect-variadic-call", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt64Ty(),
                              {B.getPtrTy(), B.getInt64Ty(), B.getInt64Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "call_variadic", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Call = B.CreateCall(
      llvm::FunctionType::get(B.getInt64Ty(), {B.getInt32Ty()}, true),
      Fn->getArg(0), {B.getInt32(2), Fn->getArg(1), Fn->getArg(2)});
  B.CreateRet(Call);
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(M, OS, Options));
  EXPECT_NE(Text.find("uint32_t, ..."), std::string::npos) << Text;
  const auto Program = "#include <stdarg.h>\n" + Text + R"(
static unsigned calls;
uint64_t target(uint32_t n, ...) {
  ++calls;
  va_list args;
  va_start(args, n);
  uint64_t first = va_arg(args, uint64_t);
  uint64_t second = va_arg(args, uint64_t);
  va_end(args);
  return n == 2 ? first ^ second : 0;
}
int main(void) {
  return call_variadic((void *)target, UINT64_C(0xfedcba9876543210),
                       UINT64_C(0x80000000abcdef71)) !=
      (UINT64_C(0xfedcba9876543210) ^ UINT64_C(0x80000000abcdef71)) || calls != 1;
}
)";
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Program, Level);
}

TEST(LLVMCValues, PreservedIndirectVariadicCallNeedsFixedParameter) {
  llvm::LLVMContext C;
  llvm::Module M("indirect-variadic-without-fixed-parameter", C);
  llvm::IRBuilder<> B(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "call_no_fixed_parameter", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  B.CreateRet(B.CreateCall(llvm::FunctionType::get(B.getInt64Ty(), {}, true),
                           Fn->getArg(0)));
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  EXPECT_THROW(neverd::LLVMCEmitter().emit(M, OS, Options), std::runtime_error);
}

} // namespace

TEST(LLVMCValues, ImageStringsRejectOverlappingRelocationStorage) {
  for (bool Wide : {false, true}) {
    neverd::BinaryImage Image;
    Image.Arch = neverd::Arch::X64;
    Image.Bits = neverd::Bitness::Bits64;
    neverd::Segment Segment;
    Segment.VA = 0x1000;
    Segment.Size = Segment.FileSz = 64;
    Segment.Flags = neverd::SegmentFlags::Readable;
    Segment.Data.resize(64);
    constexpr neverd::va_t Address = 0x1010;
    const unsigned Stride = Wide ? 2 : 1;
    for (unsigned I = 0; I < 5; ++I)
      Segment.Data[16 + I * Stride] = "hello"[I];
    Image.Segments.push_back(std::move(Segment));
    const std::optional<std::string> Expected =
        Wide ? "L\"hello\"" : "\"hello\"";
    ASSERT_EQ(neverd::imageStringLiteral(&Image, Address), Expected);

    // A fixup can overlap the first byte from the preceding pointer slot,
    // start inside the text, or alter its terminator. A following slot does
    // not invalidate the complete preceding string.
    for (neverd::va_t Slot :
         {Address - 7, Address, Address + 2, Address + 5 * Stride}) {
      Image.CodePtrRelocSlots = {Slot};
      EXPECT_FALSE(neverd::imageStringLiteral(&Image, Address));
    }
    Image.CodePtrRelocSlots = {Address + 6 * Stride};
    EXPECT_EQ(neverd::imageStringLiteral(&Image, Address), Expected);
    Image.CodePtrRelocSlots.clear();
    neverd::BaseRelocation Fixup;
    Fixup.Address = Address + 1;
    Image.BaseRelocations.push_back(Fixup);
    EXPECT_FALSE(neverd::imageStringLiteral(&Image, Address));
  }
}

TEST(LLVMCValues, GenericFrameBuiltinsDoNotRequireForeignISAHeaders) {
  for (auto Arch : {neverd::Arch::AArch64, neverd::Arch::X64}) {
    llvm::LLVMContext Context;
    llvm::Module Module("generic-frame-builtins", Context);
    auto *Pointer = llvm::PointerType::getUnqual(Context);
    for (auto ID :
         {llvm::Intrinsic::returnaddress, llvm::Intrinsic::frameaddress}) {
      const char *Name =
          ID == llvm::Intrinsic::returnaddress ? "return_ptr" : "frame_ptr";
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(Pointer, {}, false),
          llvm::GlobalValue::ExternalLinkage, Name, Module);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Intrinsic =
          llvm::Intrinsic::getOrInsertDeclaration(&Module, ID, {Pointer});
      Builder.CreateRet(Builder.CreateCall(Intrinsic, {Builder.getInt32(0)}));
    }
    neverd::CEmitterOptions Options;
    Options.TheArch = Arch;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
    EXPECT_EQ(Source.find("arm_acle.h"), std::string::npos);
    EXPECT_EQ(Source.find("immintrin.h"), std::string::npos);
    for (const char *Optimization : {"-O0", "-O2"})
      compileAndRun(Source + "\nint main(void) { return !return_ptr() || "
                             "!frame_ptr(); }\n",
                    Optimization);
  }
}

TEST(LLVMCValues, DeadAddressComputationsKeepEffectsAndTheOriginalIR) {
  llvm::LLVMContext Context;
  llvm::Module Module("dead-source-addresses", Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Table = new llvm::GlobalVariable(
      Module, llvm::ArrayType::get(I64, 4), true,
      llvm::GlobalValue::ExternalLinkage, nullptr, "__nd_codeptr_1000");
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I64, {Pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "read_effects", Module);
  auto *Observe = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {Pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "record_event", Module);
  llvm::IRBuilder<llvm::NoFolder> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateAdd(Builder.CreatePtrToInt(Table, I64), Builder.getInt64(8));
  auto *Volatile = Builder.CreateLoad(I64, Function->getArg(0));
  Volatile->setVolatile(true);
  auto *Atomic = Builder.CreateLoad(I64, Function->getArg(0));
  Atomic->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
  Atomic->setAlignment(llvm::Align(8));
  Builder.CreateCall(Observe, {Function->getArg(0)});
  Builder.CreateRet(Builder.getInt64(17));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Before;
  llvm::raw_string_ostream BeforeOut(Before);
  Module.print(BeforeOut, nullptr);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  EXPECT_EQ(Source.find("codeptr_1000"), std::string::npos);
  EXPECT_NE(Source.find("volatile"), std::string::npos);
  EXPECT_NE(Source.find("__atomic_load"), std::string::npos);
  std::string After;
  llvm::raw_string_ostream AfterOut(After);
  Module.print(AfterOut, nullptr);
  EXPECT_EQ(Before, After);
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
static unsigned events;
void record_event(void *pointer) { ++events; *(uint64_t *)pointer += 1; }
int main(void) {
  uint64_t value = 71;
  return read_effects(&value) != 17 || events != 1 || value != 72;
}
)",
                  Optimization);
}

TEST(LLVMCValues, RemaindersWithInlineOperandsPublishTheirResult) {
  llvm::LLVMContext Context;
  llvm::Module Module("remainder-inline-operands", Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  std::string Main = "int main(void) { uint64_t first, second;\n";
  for (bool Signed : {false, true}) {
    const std::string Name = Signed ? "signed_rem" : "unsigned_rem";
    auto *Signature = llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                              {llvm::Type::getInt64Ty(Context),
                                               llvm::Type::getInt64Ty(Context),
                                               Pointer, Pointer},
                                              false);
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Divisor = Builder.CreateOr(Function->getArg(1), Builder.getInt64(1));
    auto *Remainder = Signed ? Builder.CreateSRem(Function->getArg(0), Divisor)
                             : Builder.CreateURem(Function->getArg(0), Divisor);
    Builder.CreateStore(Remainder, Function->getArg(2));
    Builder.CreateStore(Builder.CreateAdd(Remainder, Builder.getInt64(17)),
                        Function->getArg(3));
    Builder.CreateRetVoid();
    for (int64_t Input : {int64_t(0), int64_t(71), int64_t(-71)}) {
      const uint64_t Expected =
          Signed ? uint64_t(Input % 9) : uint64_t(Input) % 9;
      Main += Name + "(" + std::to_string(uint64_t(Input)) +
              "ULL, 8, &first, &second);\n";
      Main += "if (first != " + std::to_string(Expected) +
              "ULL || second != " + std::to_string(Expected + 17) +
              "ULL) return 1;\n";
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, LongExpressionBoundariesSurviveReorderedBlocks) {
  for (bool Reverse : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module Module("expression_boundaries", Context);
    auto *Word = llvm::Type::getInt64Ty(Context);
    auto *Signature = llvm::FunctionType::get(Word, {Word, Word}, false);
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, "chain", Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
    auto *Middle = llvm::BasicBlock::Create(Context, "middle", Function);
    auto *Tail = llvm::BasicBlock::Create(Context, "tail", Function);
    llvm::IRBuilder<llvm::NoFolder> B(Entry);
    B.CreateBr(Middle);
    B.SetInsertPoint(Middle);
    llvm::Value *Value = Function->getArg(0);
    for (unsigned I = 0; I != 512; ++I) {
      if (I == 256) {
        B.CreateBr(Tail);
        B.SetInsertPoint(Tail);
      }
      Value = B.CreateAdd(
          B.CreateXor(B.CreateMul(Value, B.getInt64(33)), Function->getArg(1)),
          B.getInt64(I));
    }
    B.CreateRet(Value);
    if (Reverse)
      Tail->moveAfter(Entry);
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
    ASSERT_LT(Source.size(), 200000u);
    const char *Main = R"(
int main(void) {
  for (uint64_t n = 0; n != 24; ++n) {
    uint64_t k = n * UINT64_C(0x9e3779b97f4a7c15), expected = n;
    for (uint64_t i = 0; i != 512; ++i)
      expected = ((expected * 33) ^ k) + i;
    if (chain(n, k) != expected) return 1;
  }
  return 0;
}
)";
    for (const char *Optimization : {"-O0", "-O2"})
      compileAndRun(Source + Main, Optimization);
  }
}

TEST(LLVMCValues, RelocatableConstantAddressArithmeticUsesTheObjectAddress) {
  llvm::LLVMContext Context;
  llvm::Module Module("relocatable_address_arithmetic", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Data =
      llvm::ConstantDataArray::getString(Context, "0123456789abcdef", false);
  auto *Global = new llvm::GlobalVariable(Module, Data->getType(), true,
                                          llvm::GlobalValue::ExternalLinkage,
                                          Data, "address_bytes");
  auto *Element = llvm::ConstantExpr::getGetElementPtr(
      Data->getType(), Global, llvm::ConstantInt::get(Word, 1));
  auto *Base = llvm::ConstantExpr::getPtrToInt(Element, Word);
  auto *Address =
      llvm::ConstantExpr::getAdd(Base, llvm::ConstantInt::getSigned(Word, -8));
  auto *Function = llvm::Function::Create(llvm::FunctionType::get(Word, false),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "address_arithmetic", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
  B.CreateRet(Address);
  auto *Direct = llvm::Function::Create(llvm::FunctionType::get(Word, false),
                                        llvm::GlobalValue::ExternalLinkage,
                                        "direct_object_address", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Direct));
  B.CreateRet(llvm::ConstantExpr::getPtrToInt(Global, Word));
  auto *Subtract = llvm::Function::Create(llvm::FunctionType::get(Word, false),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "subtract_object_address", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Subtract));
  B.CreateRet(llvm::ConstantExpr::getSub(B.getInt64(37), Base));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  const char *Main = R"(
int main(void) {
  uintptr_t expected = (uintptr_t)((const unsigned char *)&address_bytes + 8);
  if (direct_object_address() != (uintptr_t)&address_bytes) return 2;
  if (subtract_object_address() != UINT64_C(37) - (expected + 8)) return 3;
  if (address_arithmetic() != expected) return 1;
  return *(const unsigned char *)(uintptr_t)address_arithmetic() != '8';
}
)";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization, {}, true);
}

TEST(LLVMCValues, UnsupportedConstantExpressionCannotBecomeZero) {
  llvm::LLVMContext Context;
  llvm::Module Module("constant_expression", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Byte = llvm::Type::getInt8Ty(Context);
  auto *Global = new llvm::GlobalVariable(
      Module, Byte, false, llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(Byte, 0), "cell");
  auto *Function = llvm::Function::Create(llvm::FunctionType::get(Word, false),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "address_bits", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
  B.CreateRet(llvm::ConstantExpr::getXor(
      llvm::ConstantExpr::getPtrToInt(Global, Word), B.getInt64(17)));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
               std::runtime_error);
}

TEST(LLVMCValues, LongPointerCastChainsKeepNamedBoundaries) {
  llvm::LLVMContext Context;
  llvm::Module Module("pointer_cast_chain", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "pointer_chain", Module);
  llvm::IRBuilder<llvm::NoFolder> B(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Value = Function->getArg(0);
  for (unsigned I = 0; I != 16384; ++I)
    Value = B.CreateIntToPtr(B.CreatePtrToInt(Value, Word), Pointer);
  B.CreateRet(B.CreatePtrToInt(Value, Word));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  ASSERT_LT(Source.size(), 4000000u);
  const char *Main = R"(
int main(void) {
  uint64_t cell = 17;
  return pointer_chain(&cell) != (uintptr_t)&cell;
}
)";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, DeepConstantExpressionsExhaustABoundedFold) {
  llvm::LLVMContext Context;
  llvm::Module Module("deep_constant_expression", Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Byte = llvm::Type::getInt8Ty(Context);
  auto *Global = new llvm::GlobalVariable(
      Module, Byte, false, llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(Byte, 0), "cell");
  llvm::Constant *Value = llvm::ConstantExpr::getPtrToInt(Global, Word);
  for (unsigned I = 0; I != 256; ++I) {
    Value = llvm::ConstantExpr::getAdd(Value, llvm::ConstantInt::get(Word, 13));
    Value = llvm::ConstantExpr::getXor(Value, llvm::ConstantInt::get(Word, 17));
  }
  auto *Function = llvm::Function::Create(llvm::FunctionType::get(Word, false),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "deep_address_bits", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
  B.CreateRet(Value);
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  try {
    (void)neverd::LLVMCEmitter().emit(Module, Out, {});
    FAIL() << "deep constant expression unexpectedly emitted";
  } catch (const std::runtime_error &Error) {
    EXPECT_STREQ(Error.what(), "LLVM C constant-fold budget exceeded");
  }
}

// Exercise the typed expression projection inside an admitted scalar loop.
// The independent reference is the same LLVM function compiled directly by
// Clang, not another generated C expression with the same promotion rules.
std::string checkScalarLoopExpression(llvm::StringRef Body) {
  const std::string IR = R"(
define i64 @expression_loop(i64 %a, i64 %b, i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [0, %entry], [%next, %body]
  %state = phi i64 [%a, %entry], [%result, %body]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %body, label %exit
body:
)" + Body.str() + R"(
  %next = add i32 %i, 1
  br label %header
exit:
  ret i64 %state
}
)";
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(IR, Error, Context);
  if (!Module || llvm::verifyModule(*Module, &llvm::errs())) {
    ADD_FAILURE() << Error.getMessage().str();
    return {};
  }
  std::string Source, Before, After, Reference;
  llvm::raw_string_ostream BeforeOut(Before);
  Module->print(BeforeOut, nullptr);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  llvm::raw_string_ostream Out(Source);
  EXPECT_TRUE(neverd::LLVMCEmitter().emit(*Module, Out, Options));
  llvm::raw_string_ostream AfterOut(After);
  Module->print(AfterOut, nullptr);
  EXPECT_EQ(Before, After);
  EXPECT_NE(Source.find("for ("), std::string::npos) << Source;
  Module->getFunction("expression_loop")->setName("reference_loop");
  llvm::raw_string_ostream ReferenceOut(Reference);
  Module->print(ReferenceOut, nullptr);
  const std::string Main = R"(
extern uint64_t reference_loop(uint64_t, uint64_t, uint32_t);
int main(void) {
  const uint64_t values[] = {
    0, 1, 2, 7, 31, 32, 40, 63, 127, 128, 255, 256, 32767, 32768,
    65535, 65536, UINT32_MAX, UINT64_C(1) << 32,
    UINT64_C(1) << 63, UINT64_MAX, UINT64_C(0xfedcba9876543210)
  };
  for (unsigned x = 0; x < sizeof(values) / sizeof(values[0]); ++x)
    for (unsigned y = 0; y < sizeof(values) / sizeof(values[0]); ++y)
      for (unsigned n = 0; n < 5; ++n)
        if (expression_loop(values[x], values[y], n) !=
            reference_loop(values[x], values[y], n)) return 1;
  uint64_t random = 0x82439b7du;
  for (unsigned k = 0; k < 4096; ++k) {
    random ^= random << 13; random ^= random >> 7; random ^= random << 17;
    uint64_t a = random;
    random ^= random << 13; random ^= random >> 7; random ^= random << 17;
    if (expression_loop(a, random, k % 5) !=
        reference_loop(a, random, k % 5)) return 2;
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization, Reference,
                  /*CheckUndefinedBehavior=*/true);
  return Source;
}

TEST(LLVMCScalarExpressions, NarrowProductKeepsUnsignedPromotionAndWrapping) {
  const auto Source = checkScalarLoopExpression(R"(
  %x = trunc i64 %state to i16
  %y = trunc i64 %b to i16
  %product = mul i16 %x, %y
  %result = zext i16 %product to i64
)");
  EXPECT_NE(Source.find("(uint32_t)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("(uint16_t)"), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions, WidenedProductExecutesAtItsLLVMWidth) {
  checkScalarLoopExpression(R"(
  %x = trunc i64 %state to i32
  %y = trunc i64 %b to i32
  %wide_x = zext i32 %x to i64
  %wide_y = zext i32 %y to i64
  %result = mul i64 %wide_x, %wide_y
)");
}

TEST(LLVMCScalarExpressions, NarrowArithmeticNormalizesBeforeRightShift) {
  checkScalarLoopExpression(R"(
  %x = trunc i64 %state to i8
  %y = trunc i64 %b to i8
  %difference = sub i8 %x, %y
  %sum = add i8 %difference, 123
  %shifted = shl i8 %sum, 1
  %divided = lshr i8 %shifted, 2
  %result = zext i8 %divided to i64
)");
}

TEST(LLVMCScalarExpressions, WidenedLogicalShiftAcceptsTheFullShiftDomain) {
  checkScalarLoopExpression(R"(
  %x = trunc i64 %state to i32
  %wide = zext i32 %x to i64
  %amount = and i64 %b, 63
  %result = lshr i64 %wide, %amount
)");
}

TEST(LLVMCScalarExpressions, LowBitFromWideValueCannotWidenWordArithmetic) {
  checkScalarLoopExpression(R"(
  %bit = trunc i64 %state to i1
  %word = zext i1 %bit to i32
  %wrapped = add i32 %word, -1
  %result = zext i32 %wrapped to i64
)");
}

TEST(LLVMCScalarExpressions, SignedComparisonsRetainSignInterpretation) {
  for (unsigned Bits : {8, 16, 32}) {
    SCOPED_TRACE(Bits);
    const std::string Type = "i" + std::to_string(Bits);
    checkScalarLoopExpression(
        "  %x = trunc i64 %state to " + Type + "\n  %y = trunc i64 %b to " +
        Type + "\n  %less = icmp slt " + Type + " %x, %y\n" +
        "  %result = select i1 %less, i64 %b, i64 %state\n");
  }
  checkScalarLoopExpression(R"(
  %less = icmp slt i64 %state, %b
  %result = select i1 %less, i64 %b, i64 %state
)");
  checkScalarLoopExpression(R"(
  %less = icmp sgt i64 %state, -9223372036854775808
  %result = select i1 %less, i64 %b, i64 %state
)");
}

TEST(LLVMCScalarExpressions, SignedExtensionsKeepNegativeBitPatterns) {
  for (unsigned Bits : {8, 16, 32}) {
    SCOPED_TRACE(Bits);
    const std::string Type = "i" + std::to_string(Bits);
    checkScalarLoopExpression("  %x = trunc i64 %state to " + Type +
                              "\n  %wide = sext " + Type + " %x to i64\n" +
                              "  %result = xor i64 %wide, %b\n");
  }
}

TEST(LLVMCScalarExpressions, PrecedencePreservesNestedArithmeticAndSelections) {
  const auto Source = checkScalarLoopExpression(R"(
  %inner = sub i64 %b, 7
  %difference = sub i64 %state, %inner
  %other = add i64 %b, 9
  %product = mul i64 %difference, %other
  %bits = and i64 %state, 5
  %choose = icmp eq i64 %bits, 1
  %result = select i1 %choose, i64 %product, i64 %b
)");
  EXPECT_EQ(Source.find("(uint64_t)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("(b - 7ull)"), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions, BooleanArithmeticNormalizesAfterEveryOperation) {
  checkScalarLoopExpression(R"(
  %x = trunc i64 %state to i1
  %y = trunc i64 %b to i1
  %sum = add i1 %x, %y
  %result = zext i1 %sum to i64
)");
}

TEST(LLVMCScalarExpressions, UnsupportedOperationsUseTheExistingWriter) {
  checkScalarLoopExpression(R"(
  %amount = and i64 %b, 63
  %shifted = ashr i64 %state, %amount
  %divisor = or i64 %b, 1
  %result = udiv i64 %shifted, %divisor
)");
}

TEST(LLVMCScalarExpressions, DeepExpressionKeepsMaterializedBoundaries) {
  std::string Body;
  std::string Previous = "%state";
  for (unsigned I = 0; I != 96; ++I) {
    const std::string Name = I == 95 ? "%result" : "%v" + std::to_string(I);
    Body += "  " + Name + " = " + (I % 2 ? "xor" : "add") + " i64 " + Previous +
            ", %b\n";
    Previous = Name;
  }
  const auto Source = checkScalarLoopExpression(Body);
  EXPECT_LT(Source.size(), 20000u);
  EXPECT_NE(Source.find("uint64_t v"), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions, CommutedAdditionCanUseACompoundUpdate) {
  const auto Source = checkScalarLoopExpression(R"(
  %result = add i64 %b, %state
)");
  EXPECT_NE(Source.find(" += "), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions, ReversedSubtractionCannotUseACompoundUpdate) {
  const auto Source = checkScalarLoopExpression(R"(
  %result = sub i64 %b, %state
)");
  EXPECT_EQ(Source.find(" -= "), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions, NeutralSelectionsBecomeConditionalUpdates) {
  for (const std::string Op : {"add", "sub", "xor", "or", "and"})
    for (bool IdentityWhenTrue : {false, true}) {
      SCOPED_TRACE(Op + (IdentityWhenTrue ? " true" : " false"));
      const std::string Identity = Op == "and" ? "-1" : "0";
      const std::string Selected = IdentityWhenTrue ? Identity + ", i64 %term"
                                                    : "%term, i64 " + Identity;
      const std::string Operands =
          Op == "add" || Op == "xor" ? "%selected, %base" : "%base, %selected";
      const auto Source = checkScalarLoopExpression(R"(
  %base = xor i64 %state, %b
  %bit = and i64 %b, 1
  %condition = icmp eq i64 %bit, 0
  %term = add i64 %b, 3
  %selected = select i1 %condition, i64 )" + Selected +
                                                    "\n  %result = " + Op +
                                                    " i64 " + Operands + "\n");
      EXPECT_NE(Source.find("if ("), std::string::npos) << Source;
      EXPECT_EQ(Source.find(" ? "), std::string::npos) << Source;
      const auto At = Source.find("    uint64_t result");
      ASSERT_NE(At, std::string::npos) << Source;
      const auto Declaration = Source.substr(At, Source.find('\n', At) - At);
      EXPECT_NE(Declaration.find(" = a;"), std::string::npos) << Source;
    }
}

TEST(LLVMCScalarExpressions, ConditionalUpdateKeepsOldDestinationCondition) {
  const auto Source = checkScalarLoopExpression(R"(
  %base = add i64 %state, 1
  %bit = and i64 %state, 1
  %condition = icmp eq i64 %bit, 0
  %selected = select i1 %condition, i64 %b, i64 0
  %result = add i64 %base, %selected
)");
  EXPECT_NE(Source.find(" ? "), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions, UnchangedBaseNeedsNoAssignmentOrSnapshot) {
  const auto Source = checkScalarLoopExpression(R"(
  %bit = and i64 %state, 1
  %condition = icmp eq i64 %bit, 0
  %term = xor i64 %state, %b
  %selected = select i1 %condition, i64 %term, i64 0
  %result = add i64 %state, %selected
)");
  EXPECT_NE(Source.find("if ("), std::string::npos) << Source;
  EXPECT_NE(Source.find(" += "), std::string::npos) << Source;
  EXPECT_EQ(Source.find(" ? "), std::string::npos) << Source;
  const auto At = Source.find("    uint64_t result");
  ASSERT_NE(At, std::string::npos) << Source;
  const auto NameBegin = At + std::string("    uint64_t ").size();
  const auto Name =
      Source.substr(NameBegin, Source.find(' ', NameBegin) - NameBegin);
  EXPECT_EQ(Source.find(Name + " = " + Name + ";"), std::string::npos)
      << Source;
}

TEST(LLVMCScalarExpressions, ConditionalUpdateKeepsOldDestinationContribution) {
  const auto Source = checkScalarLoopExpression(R"(
  %base = add i64 %state, 1
  %term = xor i64 %state, %b
  %condition = icmp eq i64 %b, 0
  %selected = select i1 %condition, i64 0, i64 %term
  %result = add i64 %base, %selected
)");
  EXPECT_NE(Source.find(" ? "), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions,
     SharedInlinedConditionStillReadsTheOldDestination) {
  const auto Source = checkScalarLoopExpression(R"(
  %condition = icmp eq i64 %state, 0
  %selected = select i1 %condition, i64 %b, i64 0
  %increment = select i1 %condition, i64 1, i64 2
  %base = add i64 %state, %increment
  %result = add i64 %base, %selected
)");
  EXPECT_NE(Source.find(" ? "), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions,
     ConditionalUpdateUsesMaterializedConditionSnapshot) {
  // Exceed the expression-depth bound at the comparison itself. Multiple
  // uses alone do not keep a comparison from being inlined.
  std::string Body, Previous = "%state";
  for (unsigned I = 0; I != 16; ++I) {
    const std::string Name = "%v" + std::to_string(I);
    Body += "  " + Name + " = add i64 " + Previous + ", %b\n";
    Previous = Name;
  }
  Body += "  %condition = icmp eq i64 " + Previous + R"(, 0
  %selected = select i1 %condition, i64 7, i64 0
  %increment = select i1 %condition, i64 1, i64 2
  %base = add i64 %state, %increment
  %result = add i64 %base, %selected
)";
  const auto Source = checkScalarLoopExpression(Body);
  EXPECT_NE(Source.find("uint8_t condition"), std::string::npos) << Source;
  EXPECT_NE(Source.find("if (condition"), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions, ConditionalTruthTestRetainsNarrowNormalization) {
  const auto Source = checkScalarLoopExpression(R"(
  %x = trunc i64 %a to i8
  %y = trunc i64 %b to i8
  %wrapped = add i8 %x, %y
  %condition = icmp eq i8 %wrapped, 0
  %selected = select i1 %condition, i64 7, i64 0
  %base = add i64 %state, 1
  %result = add i64 %base, %selected
)");
  EXPECT_NE(Source.find("if ("), std::string::npos) << Source;
  EXPECT_NE(Source.find("(uint8_t)"), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions, NonIdentitySelectionKeepsBothArms) {
  const auto Source = checkScalarLoopExpression(R"(
  %condition = icmp eq i64 %b, 0
  %selected = select i1 %condition, i64 5, i64 1
  %base = xor i64 %state, %b
  %result = add i64 %base, %selected
)");
  EXPECT_NE(Source.find(" ? "), std::string::npos) << Source;
}

TEST(LLVMCScalarExpressions, SharedSelectionKeepsItsMaterializedValue) {
  const auto Source = checkScalarLoopExpression(R"(
  %condition = icmp eq i64 %b, 0
  %selected = select i1 %condition, i64 5, i64 0
  %base = xor i64 %state, %selected
  %result = add i64 %base, %selected
)");
  const auto Select = Source.find(" ? ");
  ASSERT_NE(Select, std::string::npos) << Source;
  EXPECT_EQ(Source.find(" ? ", Select + 1), std::string::npos) << Source;
}

TEST(LLVMCValues, CrossBlockHomesKeepConditionalAndWidthChangingStores) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(R"ir(
declare void @change_home(ptr)
define i64 @conditional_home(i32 %flag) {
entry:
  %home = alloca i64
  store i64 0, ptr %home
  %take = icmp ne i32 %flag, 0
  br i1 %take, label %set, label %exit
set:
  store i64 7, ptr %home
  br label %exit
exit:
  %value = load i64, ptr %home
  ret i64 %value
}
define i64 @narrowed_home(i32 %flag) {
entry:
  %home = alloca i64
  store i64 65535, ptr %home
  %take = icmp ne i32 %flag, 0
  br i1 %take, label %narrow, label %exit
narrow:
  %old = load i64, ptr %home
  %byte = trunc i64 %old to i8
  %wide = zext i8 %byte to i64
  store i64 %wide, ptr %home
  br label %exit
exit:
  %value = load i64, ptr %home
  ret i64 %value
}
define i64 @escaped_home() {
entry:
  %home = alloca i64
  store i64 7, ptr %home
  call void @change_home(ptr %home)
  br label %exit
exit:
  %value = load i64, ptr %home
  ret i64 %value
}
define i64 @snapshot_home(ptr %address) {
entry:
  %home = alloca i64
  %old = load i64, ptr %address
  store i64 %old, ptr %home
  call void @change_home(ptr %address)
  br label %exit
exit:
  %value = load i64, ptr %home
  ret i64 %value
}
)ir",
                                          Error, Context);
  ASSERT_TRUE(Module) << Error.getMessage().str();
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(*Module, Out, Options));
  Source += R"c(
void change_home(void *address) { *(uint64_t *)address = 9; }
int main(void) {
    uint64_t value = 7;
    return conditional_home(0) != 0 || conditional_home(1) != 7 ||
           narrowed_home(0) != 65535 || narrowed_home(1) != 255 ||
           escaped_home() != 9 || snapshot_home(&value) != 7 || value != 9;
}
)c";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization, {}, true);
}

TEST(LLVMCValues, PointerFactsKeepObservationWidthAndInterveningWrites) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(R"ir(
declare void @clear_field(ptr)
define i32 @call_before_branch(ptr %field) {
entry:
  %old = load i64, ptr %field
  %nonzero = icmp ne i64 %old, 0
  call void @clear_field(ptr %field)
  br i1 %nonzero, label %retest, label %empty
retest:
  %now = load i64, ptr %field
  %still = icmp ne i64 %now, 0
  br i1 %still, label %present, label %changed
present: ret i32 1
changed: ret i32 2
empty: ret i32 3
}
define i32 @alias_before_retest(ptr %field, ptr %alias) {
entry:
  %old = load i64, ptr %field
  %nonzero = icmp ne i64 %old, 0
  br i1 %nonzero, label %write, label %empty
write:
  store i64 0, ptr %alias
  br label %retest
retest:
  %now = load i64, ptr %field
  %still = icmp ne i64 %now, 0
  br i1 %still, label %present, label %changed
present: ret i32 1
changed: ret i32 2
empty: ret i32 3
}
define i32 @narrow_home_retest(i64 %input) {
entry:
  %home = alloca i64
  store i64 %input, ptr %home
  %old = load i64, ptr %home
  %nonzero = icmp ne i64 %old, 0
  br i1 %nonzero, label %retest, label %empty
retest:
  %now = load i64, ptr %home
  %byte = trunc i64 %now to i8
  %still = icmp ne i8 %byte, 0
  br i1 %still, label %present, label %changed
present: ret i32 1
changed: ret i32 2
empty: ret i32 3
}
define i32 @narrow_field_retest(ptr %field) {
entry:
  %old = load i64, ptr %field
  %nonzero = icmp ne i64 %old, 0
  br i1 %nonzero, label %retest, label %empty
retest:
  %now = load i8, ptr %field
  %still = icmp ne i8 %now, 0
  br i1 %still, label %present, label %changed
present: ret i32 1
changed: ret i32 2
empty: ret i32 3
}
)ir",
                                          Error, Context);
  ASSERT_TRUE(Module) << Error.getMessage().str();
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(*Module, Out, Options));
  Source += R"c(
void clear_field(void *address) { *(uint64_t *)address = 0; }
int main(void) {
    const uint64_t inputs[] = {0, 1, 256, 257, UINT64_C(0x8000000000000000)};
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        uint64_t value = inputs[i];
        unsigned changed = value ? 2 : 3;
        if (call_before_branch(&value) != changed || value != 0) return 1;
        value = inputs[i];
        if (alias_before_retest(&value, &value) != changed || value != 0) return 2;
        value = inputs[i];
        unsigned narrowed = !value ? 3 : (uint8_t)value ? 1 : 2;
        if (narrow_home_retest(value) != narrowed) return 3;
        if (narrow_field_retest(&value) != narrowed) return 4;
    }
    return 0;
}
)c";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization, {}, true);
}

TEST(LLVMCValues, BulkFrameMemoryKeepsContiguousBackingStorage) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(R"ir(
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)
define i64 @bulk_frame(ptr %input, ptr %output, i64 %length) {
entry:
  %frame = alloca [64 x i8], align 8
  %frame_end = getelementptr i8, ptr %frame, i64 64
  %begin = getelementptr i8, ptr %frame, i64 32
  %next = getelementptr i8, ptr %begin, i64 4
  call void @llvm.memset.p0.i64(ptr %begin, i8 90, i64 16, i1 false)
  store i32 7, ptr %begin, align 4
  call void @llvm.memcpy.p0.p0.i64(ptr %begin, ptr %input, i64 %length, i1 false)
  call void @llvm.memmove.p0.p0.i64(ptr %next, ptr %begin, i64 12, i1 false)
  call void @llvm.memcpy.p0.p0.i64(ptr %output, ptr %begin, i64 16, i1 false)
  %result = load i64, ptr %begin, align 8
  ret i64 %result
}
)ir",
                                          Error, Context);
  ASSERT_TRUE(Module) << Error.getMessage().str();
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(*Module, Out, Options));
  Source += R"c(
int main(void) {
    unsigned char input[16], output[16], expected[16];
    for (unsigned i = 0; i != 16; ++i) input[i] = i * 13;
    for (unsigned length = 0; length <= 16; ++length) {
        uint32_t seed = 7;
        uint64_t result;
        memset(expected, 90, sizeof(expected));
        memcpy(expected, &seed, sizeof(seed));
        memcpy(expected, input, length);
        memmove(expected + 4, expected, 12);
        memcpy(&result, expected, sizeof(result));
        if (bulk_frame(input, output, length) != result ||
            memcmp(output, expected, sizeof(output))) return 1;
    }
    return 0;
}
)c";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization, {}, true);
}

TEST(LLVMCValues, FrameBaseOffsetsUseThePointerIndexWidth) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(R"ir(
target datalayout = "e-p:32:32"
define void @frame_offset() {
entry:
  %frame = alloca [64 x i8], align 8
  %frame_end = getelementptr i8, ptr %frame, i64 4294967328
  store i8 7, ptr %frame_end
  ret void
}
)ir",
                                          Error, Context);
  ASSERT_TRUE(Module) << Error.getMessage().str();
  const auto &Frame = llvm::cast<llvm::AllocaInst>(
      Module->getFunction("frame_offset")->getEntryBlock().front());
  // GEP truncates the index before pointer arithmetic: 2^32 + 32 is 32
  // in the guest's 32-bit address domain, and outside the frame at 64 bits.
  EXPECT_EQ(
      neverd::llvmc::syntheticFrameBaseOffset(Frame, Module->getDataLayout()),
      32u);
  EXPECT_FALSE(neverd::llvmc::syntheticFrameBaseOffset(
      Frame, llvm::DataLayout("e-p:64:64")));
  EXPECT_FALSE(neverd::llvmc::syntheticFrameBaseOffset(
      Frame, llvm::DataLayout("e-p:128:128")));
}

TEST(LLVMCValues, SelectedScalarCallsDeclareExternalAndUnemittedProviders) {
  llvm::LLVMContext Context;
  llvm::Module Module("scalar-fragment", Context);
  Module.setDataLayout("e-p:64:64");
  Module.setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Signature = llvm::FunctionType::get(Word, {Word}, false);
  auto *External =
      llvm::Function::Create(Signature, llvm::GlobalValue::ExternalLinkage,
                             "fragment_external", Module);
  auto *Caller = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "fragment_call", Module);
  auto *Defined =
      llvm::Function::Create(Signature, llvm::GlobalValue::ExternalLinkage,
                             "fragment_defined", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Defined));
  B.CreateRet(B.CreateAdd(Defined->getArg(0), B.getInt64(11)));
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Caller));
  auto *First = B.CreateCall(External, {Caller->getArg(0)});
  auto *Second = B.CreateCall(Defined, {Caller->getArg(0)});
  B.CreateRet(B.CreateXor(First, Second));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options, nullptr,
                                          nullptr, Caller));
  EXPECT_NE(Source.find("fragment_external(uint64_t);"), std::string::npos);
  EXPECT_NE(Source.find("fragment_defined(uint64_t);"), std::string::npos);
  const std::string Reference = R"(
define i64 @fragment_external(i64 %x) {
  %value = xor i64 %x, 2305843009213693952
  ret i64 %value
}
define i64 @fragment_defined(i64 %x) {
  %value = add i64 %x, 11
  ret i64 %value
}
)";
  const std::string Main = R"(
int main(void) {
  const uint64_t inputs[] = {0, 1, 77, UINT64_MAX, UINT64_C(1) << 63};
  for (unsigned i = 0; i < 5; ++i) {
    uint64_t x = inputs[i];
    if (fragment_call(x) != ((x ^ (UINT64_C(1) << 61)) ^ (x + 11)))
      return 1;
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization, Reference);
}
