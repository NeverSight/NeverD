//===- LLVMCFunnelShiftTests.cpp - Funnel shift C semantics -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

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
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u}) {
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
  const T values[] = {(T)0, (T)1, (T)0x80000001u, (T)~(T)0}; \
  const unsigned counts[] = {0, 1, 7, W - 1, W, W + 1, 2 * W - 1, 255}; \
  for (unsigned i = 0; i < 4; ++i) \
    for (unsigned j = 0; j < 4; ++j) \
      for (unsigned k = 0; k < 8; ++k) { \
        T a = values[i], b = values[j]; \
        unsigned s = counts[k] % W; \
        T left = s ? (T)((a << s) | (b >> (W - s))) : a; \
        T right = s ? (T)((a << (W - s)) | (b >> s)) : b; \
        if (fshl##W(a, b, (T)counts[k]) != (T)(left ^ (T)0x5a) || \
            fshr##W(a, b, (T)counts[k]) != (T)(right ^ (T)0x5a)) \
          return 1; \
      } \
} while (0)
int main(void) {
  CHECK(8, uint8_t);
  CHECK(16, uint16_t);
  CHECK(32, uint32_t);
  CHECK(64, uint64_t);
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
  const llvm::SmallVector<llvm::StringRef, 12> Arguments{
      *Compiler,
      "-std=c11",
      "-O1",
      "-fsanitize=undefined",
      "-fsanitize-trap=undefined",
      Input,
      "-o",
      Output};
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
