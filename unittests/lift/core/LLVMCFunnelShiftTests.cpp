//===- LLVMCFunnelShiftTests.cpp - Funnel shift C semantics -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../HostCompiler.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>
#include <utility>

namespace {
using namespace neverd;

TEST(LLVMCFunnelShift, GuardsZeroAndModuloCountsAtIntegerWidths) {
  llvm::LLVMContext Context;
  llvm::Module Module("funnel-shifts", Context);
  llvm::IRBuilder<> Builder(Context);
  for (unsigned Width :
       {7u, 8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u, 72u, 128u}) {
    auto *Ty = Builder.getIntNTy(Width);
    for (const auto &[Prefix, ID] :
         {std::pair{"fshl", llvm::Intrinsic::fshl},
          std::pair{"fshr", llvm::Intrinsic::fshr}}) {
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(Ty, {Ty, Ty, Ty}, false),
          llvm::GlobalValue::ExternalLinkage,
          std::string(Prefix) + std::to_string(Width), Module);
      Builder.SetInsertPoint(
          llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Intrinsic =
          llvm::Intrinsic::getOrInsertDeclaration(&Module, ID, {Ty});
      auto *Shifted = Builder.CreateCall(
          Intrinsic,
          {Function->getArg(0), Function->getArg(1), Function->getArg(2)});
      Builder.CreateRet(
          Builder.CreateXor(Shifted, llvm::ConstantInt::get(Ty, 0x5a)));
    }
  }

  std::string Emitted;
  llvm::raw_string_ostream Stream(Emitted);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
  Stream.flush();
  EXPECT_NE(Emitted.find("neverd_llvm_fshl_i32"), std::string::npos) << Emitted;
  EXPECT_NE(Emitted.find("neverd_llvm_fshr_i32"), std::string::npos) << Emitted;

  const std::string Source = "#include <stdint.h>\n" + Emitted + R"(
#define CHECK(W, T) do { \
  const T mask = (T)((T)~(T)0 >> (sizeof(T) * 8 - W)); \
  const T values[] = {(T)0, (T)1, (T)0x80000001u, (T)~(T)0}; \
  const unsigned counts[] = {0, 1, 7, W - 1, W, W + 1, 2 * W - 1, 255}; \
  for (unsigned i = 0; i < 4; ++i) \
    for (unsigned j = 0; j < 4; ++j) \
      for (unsigned k = 0; k < 8; ++k) { \
        T a = values[i] & mask, b = values[j] & mask; \
        unsigned s = ((T)counts[k] & mask) % W; \
        T left = s ? (T)((a << s) | (b >> (W - s))) : a; \
        T right = s ? (T)((a << (W - s)) | (b >> s)) : b; \
        if (fshl##W(values[i], values[j], (T)counts[k]) != ((left ^ (T)0x5a) & mask) || \
            fshr##W(values[i], values[j], (T)counts[k]) != ((right ^ (T)0x5a) & mask)) \
          return 1; \
      } \
} while (0)
int main(void) {
  CHECK(7, uint8_t);
  CHECK(8, uint8_t);
  CHECK(16, uint16_t);
  CHECK(24, uint32_t);
  CHECK(32, uint32_t);
  CHECK(40, uint64_t);
  CHECK(48, uint64_t);
  CHECK(56, uint64_t);
  CHECK(64, uint64_t);
  CHECK(72, __uint128_t);
  CHECK(128, unsigned __int128);
  return 0;
}
)";

  auto Compiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(Compiler)) << "clang is required";
  llvm::SmallString<128> Input, Output, Errors;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-funnel", "c", Input));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-funnel", "out", Output));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-funnel", "err", Errors));
  llvm::FileRemover RemoveInput(Input), RemoveOutput(Output),
      RemoveErrors(Errors);
  std::error_code ErrorCode;
  {
    llvm::raw_fd_ostream File(Input, ErrorCode);
    ASSERT_FALSE(ErrorCode);
    File << Source;
  }
  llvm::SmallVector<llvm::StringRef, 12> Arguments{*Compiler,
                                                   "-std=c11",
                                                   "-O1",
                                                   "-fsanitize=undefined",
                                                   "-fsanitize-trap=undefined",
                                                   Input,
                                                   "-o",
                                                   Output};
  neverd::test::appendHostCLinkFlags(Arguments);
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, Errors.str()};
  std::string Error;
  const int Compiled = llvm::sys::ExecuteAndWait(
      *Compiler, Arguments, std::nullopt, Redirects, 30, 0, &Error);
  auto Log = llvm::MemoryBuffer::getFile(Errors);
  ASSERT_EQ(Compiled, 0) << Error << (Log ? (*Log)->getBuffer().str() : "")
                         << Source;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}, std::nullopt, {}, 30, 0,
                                      &Error),
            0)
      << Error << Source;
}
} // namespace

TEST(LLVMCArithmeticShift, PreservesSignAtTheLLVMOperandWidth) {
  llvm::LLVMContext Context;
  llvm::Module Module("arithmetic-shifts", Context);
  llvm::IRBuilder<> Builder(Context);
  auto *Wide = Builder.getInt128Ty();
  std::string Table;
  for (unsigned Width : {1u, 8u, 16u, 32u, 64u, 128u}) {
    for (bool Assigned : {false, true}) {
      const std::string Name =
          "ashr_" + std::to_string(Width) + "_" + std::to_string(Assigned);
      Table += Name + ",";
      auto *Fn = llvm::Function::Create(
          llvm::FunctionType::get(Wide, {Wide, Wide, Builder.getPtrTy()},
                                  false),
          llvm::GlobalValue::ExternalLinkage, Name, Module);
      Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Fn));
      auto *Ty = Builder.getIntNTy(Width);
      auto *Shift =
          Builder.CreateAShr(Builder.CreateTruncOrBitCast(Fn->getArg(0), Ty),
                             Builder.CreateTruncOrBitCast(Fn->getArg(1), Ty));
      auto *Result = Builder.CreateZExtOrBitCast(Shift, Wide);
      Builder.CreateStore(Assigned ? Result : Fn->getArg(0), Fn->getArg(2));
      Builder.CreateRet(Result);
    }
  }
  // A direct odd-width return must not expose sign-extension carrier bits.
  // Also keep following arithmetic unsigned, even when AShr itself is signed.
  for (unsigned Width : {1u, 7u, 17u, 32u, 64u, 65u}) {
    auto *Ty = Builder.getIntNTy(Width);
    auto *Fn =
        llvm::Function::Create(llvm::FunctionType::get(Ty, {Ty, Ty}, false),
                               llvm::GlobalValue::ExternalLinkage,
                               "direct_" + std::to_string(Width), Module);
    Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Fn));
    auto *Value = Builder.CreateAShr(Fn->getArg(0), Fn->getArg(1));
    if (Width == 32 || Width == 64)
      Value = Builder.CreateAdd(Value, llvm::ConstantInt::get(Ty, 1));
    Builder.CreateRet(Value);
  }
  std::string Emitted;
  llvm::raw_string_ostream Stream(Emitted);
  neverd::CEmitterOptions Options;
  Options.TheArch = neverd::Arch::X64;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Stream, Options));
  const std::string Source = Emitted + R"(
typedef unsigned __int128 U128;
int main(void) {
  U128 (*functions[])(U128, U128, void *) = {)" +
                             Table + R"(};
  if (direct_1(1, 0) != 1 || direct_7(64, 6) != 127 ||
      direct_17(65536, 16) != 131071 ||
      direct_65((U128)1 << 64, 64) != (((U128)1 << 65) - 1)) return 3;
  if (direct_32(0x7fffffff, 0) != UINT32_C(0x80000000) ||
      direct_64(UINT64_C(0x7fffffffffffffff), 0) !=
          UINT64_C(0x8000000000000000)) return 4;
  const unsigned widths[] = {1, 8, 16, 32, 64, 128};
  for (unsigned w = 0; w < 6; ++w) {
    const unsigned width = widths[w];
    const U128 mask = ~(U128)0 >> (128 - width);
    const U128 sign = (U128)1 << (width - 1);
    const U128 edges[] = {0, 1, sign - 1, sign, sign + 1, mask,
                          ((U128)1 << 100) | 0x12345678};
    const unsigned values = width == 8 ? 256 : 7;
    for (unsigned v = 0; v < values; ++v)
      for (unsigned count = 0; count < width; ++count)
        for (unsigned assigned = 0; assigned < 2; ++assigned) {
          const U128 input = width == 8 ? v : edges[v];
          U128 expected = input & mask;
          for (unsigned i = 0; i < count; ++i)
            expected = (expected >> 1) | (expected & sign);
          U128 stored = 0;
          if (functions[w * 2 + assigned](input, count, &stored) != expected)
            return 1;
          if (stored != (assigned ? expected : input)) return 2;
        }
  }
  return 0;
}
)";
  auto Compiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(Compiler)) << "clang is required";
  llvm::SmallString<128> Input, Output, Errors;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-ashr", "c", Input));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-ashr", "out", Output));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-ashr", "err", Errors));
  llvm::FileRemover RemoveInput(Input), RemoveOutput(Output),
      RemoveErrors(Errors);
  std::error_code EC;
  {
    llvm::raw_fd_ostream File(Input, EC);
    ASSERT_FALSE(EC);
    File << Source;
  }
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization.str());
    llvm::SmallVector<llvm::StringRef, 12> Args{*Compiler,
                                                "-std=c11",
                                                Optimization,
                                                "-fsanitize=undefined",
                                                "-fsanitize-trap=undefined",
                                                Input,
                                                "-o",
                                                Output};
    neverd::test::appendHostCLinkFlags(Args);
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Errors.str()};
    std::string Error;
    const int Compiled = llvm::sys::ExecuteAndWait(
        *Compiler, Args, std::nullopt, Redirects, 30, 0, &Error);
    auto Log = llvm::MemoryBuffer::getFile(Errors);
    ASSERT_EQ(Compiled, 0) << Error << (Log ? (*Log)->getBuffer().str() : "")
                           << Source;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}, std::nullopt, {}, 30,
                                        0, &Error),
              0)
        << Error << Source;
  }
}
