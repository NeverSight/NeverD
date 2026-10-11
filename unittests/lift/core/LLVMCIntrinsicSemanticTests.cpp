//===- LLVMCIntrinsicSemanticTests.cpp - Intrinsic C semantics ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"
#include "neverd/backend/llvm/LLVMX86X87StateAsm.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <vector>

namespace {
using namespace neverd;

std::string emit(llvm::Module &M) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.EmitIncludes = false;
  EXPECT_TRUE(LLVMCEmitter().emit(M, OS, Options));
  return Text;
}

struct StosFixture {
  llvm::LLVMContext Context;
  llvm::Module M{"rep-stos", Context};
  llvm::IRBuilder<> B{Context};
  llvm::Function *Fn;
  llvm::CallInst *Call;

  StosFixture(unsigned Bytes, X86RepStos::Direction Dir,
              llvm::StringRef Name = "fill", bool Live = false,
              bool BadConstraints = false, bool BadTemplate = false) {
    M.setDataLayout("e-p:64:64");
    const char Suffix = Bytes == 1   ? 'b'
                        : Bytes == 2 ? 'w'
                        : Bytes == 4 ? 'l'
                                     : 'q';
    std::string Rep = std::string("rep stos") + Suffix;
    std::string Asm =
        Dir == X86RepStos::Forward ? Rep
        : Dir == X86RepStos::Backward
            ? "std\n\t" + Rep + "\n\tcld"
            : "test $5,$5\n\tje 1f\n\tstd\n\t1:\n\t" + Rep + "\n\tcld";
    if (BadTemplate)
      Asm += "\n\tnop";
    const bool Dynamic = Dir == X86RepStos::Dynamic;
    std::string Constraints = std::string("={di},={cx},0,1,{ax},") +
                              (Dynamic ? "r," : "") +
                              "~{memory},~{dirflag},~{cc}";
    if (BadConstraints)
      Constraints += ",~{flags}";
    auto *Ret =
        llvm::StructType::get(Context, {B.getInt64Ty(), B.getInt64Ty()});
    std::vector<llvm::Type *> Types{B.getInt64Ty(), B.getInt64Ty(),
                                    B.getIntNTy(Bytes * 8)};
    if (Dynamic)
      Types.push_back(B.getInt64Ty());
    Fn = llvm::Function::Create(
        llvm::FunctionType::get(Live ? B.getInt64Ty() : B.getVoidTy(), Types,
                                false),
        llvm::GlobalValue::ExternalLinkage, Name, M);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Fn));
    auto *IA = llvm::InlineAsm::get(llvm::FunctionType::get(Ret, Types, false),
                                    Asm, Constraints, true);
    llvm::SmallVector<llvm::Value *, 4> Args;
    for (auto &Arg : Fn->args())
      Args.push_back(&Arg);
    Call = B.CreateCall(IA, Args);
    if (Live)
      B.CreateRet(B.CreateExtractValue(Call, 0));
    else
      B.CreateRetVoid();
  }
};

// Compile the emitted C itself. It checks both diagnostics and behavior, so a
// renderer that prints a plausible mnemonic but drops the memory effect fails.
void compileAndCheck(const std::string &Source, bool LLVMOnly = false,
                     llvm::ArrayRef<llvm::StringRef> ExpectedIR = {},
                     llvm::StringRef Optimization = "-O1",
                     bool CheckUndefined = false) {
  auto Compiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(Compiler)) << "clang is required";
  llvm::SmallString<128> Input, Output, Errors;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-intrinsic", "c", Input));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-intrinsic", "out", Output));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-intrinsic", "err", Errors));
  llvm::FileRemover RemoveInput(Input), RemoveOutput(Output),
      RemoveErrors(Errors);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(Input, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  llvm::SmallVector<llvm::StringRef, 16> Args{
      *Compiler,
      "-std=c11",
      Optimization,
      "-Werror=int-conversion",
      "-Werror=incompatible-pointer-types",
      "-Werror=uninitialized",
      Input,
      "-o",
      Output};
  if (LLVMOnly)
    Args.append({"-target", "x86_64-pc-windows-msvc", "-S", "-emit-llvm"});
  // Clang lowers the i128 conversion controls to its compiler runtime. The
  // MSVC CRT alone does not supply these builtins; retain execution coverage.
  if (!LLVMOnly &&
      llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSWindows())
    Args.push_back("--rtlib=compiler-rt");
  if (CheckUndefined)
    Args.append({"-fsanitize=undefined", "-fsanitize-trap=undefined"});
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, Errors.str()};
  std::string Error;
  int Status = llvm::sys::ExecuteAndWait(*Compiler, Args, std::nullopt,
                                         Redirects, 30, 0, &Error);
  auto Log = llvm::MemoryBuffer::getFile(Errors);
  ASSERT_EQ(Status, 0) << Error << (Log ? (*Log)->getBuffer().str() : "")
                       << Source;
  if (LLVMOnly) {
    auto IR = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(static_cast<bool>(IR));
    if (ExpectedIR.empty()) {
      EXPECT_TRUE((*IR)->getBuffer().contains("call ptr @llvm.localaddress()"))
          << (*IR)->getBuffer().str();
    } else {
      for (llvm::StringRef Token : ExpectedIR)
        EXPECT_TRUE((*IR)->getBuffer().contains(Token))
            << (*IR)->getBuffer().str();
      EXPECT_TRUE((*IR)->getBuffer().contains("br i1"))
          << (*IR)->getBuffer().str();
    }
  } else {
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}, std::nullopt, {}, 30,
                                        0, &Error),
              0)
        << Error << Source;
  }
}

TEST(LLVMCIntrinsicSemantics, SaturatingConversionsClampBeforeCasting) {
  llvm::LLVMContext Context;
  llvm::Module Module("saturating-conversions", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> Builder(Context);
  auto *Wide = Builder.getInt128Ty();
  auto Producer = Module.getOrInsertFunction(
      "produce_double", Builder.getDoubleTy(), Builder.getDoubleTy());
  std::string Table;
  for (unsigned Bits : {1u, 8u, 16u, 32u, 64u, 128u})
    for (bool Signed : {false, true})
      for (bool Single : {false, true})
        for (bool Assigned : {false, true}) {
          const std::string Name =
              "sat_" + std::to_string(Bits) + "_" + std::to_string(Signed) +
              "_" + std::to_string(Single) + "_" + std::to_string(Assigned);
          Table += Name + ",";
          auto *Function = llvm::Function::Create(
              llvm::FunctionType::get(
                  Wide, {Builder.getDoubleTy(), Builder.getPtrTy()}, false),
              llvm::GlobalValue::ExternalLinkage, Name, Module);
          Builder.SetInsertPoint(
              llvm::BasicBlock::Create(Context, "entry", Function));
          llvm::Value *Input =
              Builder.CreateCall(Producer, {Function->getArg(0)});
          if (Single)
            Input = Builder.CreateFPTrunc(Input, Builder.getFloatTy());
          auto *Intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
              &Module,
              Signed ? llvm::Intrinsic::fptosi_sat
                     : llvm::Intrinsic::fptoui_sat,
              {Builder.getIntNTy(Bits), Input->getType()});
          auto *Result = Builder.CreateCall(Intrinsic, {Input});
          Builder.CreateStore(Assigned
                                  ? Builder.CreateZExtOrBitCast(Result, Wide)
                                  : llvm::ConstantInt::get(Wide, 42),
                              Function->getArg(1));
          Builder.CreateRet(Builder.CreateZExtOrBitCast(Result, Wide));
        }
  const std::string Text = emit(Module);
  EXPECT_EQ(Text.find("llvm_x2E_"), std::string::npos) << Text;
  const std::string Source = "#include <stdint.h>\n#include <stdbool.h>\n" +
                             Text + R"(
typedef unsigned __int128 U128;
static unsigned calls;
double produce_double(double value) { ++calls; return value; }
int main(void) {
  U128 (*functions[])(double, void *) = {)" +
                             Table + R"(};
  const unsigned widths[] = {1, 8, 16, 32, 64, 128};
  for (unsigned w = 0; w < 6; ++w)
    for (unsigned sign = 0; sign < 2; ++sign) {
      const unsigned bits = widths[w];
      const U128 mask = ~(U128)0 >> (128 - bits);
      const U128 max = mask >> sign;
      const U128 min = sign ? (U128)1 << (bits - 1) : 0;
      double bound = 1;
      for (unsigned b = 0; b < bits - sign; ++b) bound *= 2;
      const double inputs[] = {0, -0.0, 0.75, -0.75, 1.75, -1.75,
          __builtin_inf(), -__builtin_inf(), __builtin_nan(""),
          bound, -bound, bound * 2, -bound * 2};
      const U128 expected[] = {0, 0, 0, 0, max ? 1 : 0, sign ? mask : 0,
          max, min, 0, max, min, max, min};
      for (unsigned single = 0; single < 2; ++single)
        for (unsigned assigned = 0; assigned < 2; ++assigned)
          for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
            calls = 0;
            U128 stored = 99;
            if (functions[w * 8 + sign * 4 + single * 2 + assigned](
                    inputs[i], &stored) != expected[i]) return 1;
            if (stored != (assigned ? expected[i] : 42)) return 2;
            if (calls != 1) return 3;
          }
    }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndCheck(Source, false, {}, Optimization, true);
}

TEST(LLVMCIntrinsicSemantics, BitReverseUsesTheExactIntegerWidth) {
  llvm::LLVMContext Context;
  llvm::Module Module("bit-reverse", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> Builder(Context);
  auto *Wide = Builder.getInt128Ty();
  std::string Table;
  for (unsigned Bits : {1u, 3u, 7u, 8u, 16u, 24u, 32u, 64u, 65u, 128u}) {
    const std::string Name = "reverse_" + std::to_string(Bits);
    Table += Name + ",";
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Wide, {Wide, Builder.getPtrTy()}, false),
        llvm::GlobalValue::ExternalLinkage, Name, Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Integer = Builder.getIntNTy(Bits);
    auto *Intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::bitreverse, {Integer});
    auto *Result = Builder.CreateCall(
        Intrinsic,
        {Builder.CreateTruncOrBitCast(Function->getArg(0), Integer)});
    Builder.CreateStore(Builder.CreateZExtOrBitCast(Result, Wide),
                        Function->getArg(1));
    Builder.CreateRet(Builder.CreateZExtOrBitCast(Result, Wide));
  }
  const std::string Text = emit(Module);
  EXPECT_EQ(Text.find("llvm_x2E_"), std::string::npos) << Text;
  const std::string Source = "#include <stdint.h>\n#include <stdbool.h>\n" +
                             Text + R"(
typedef unsigned __int128 U128;
int main(void) {
  U128 (*functions[])(U128, void *) = {)" +
                             Table + R"(};
  const unsigned widths[] = {1, 3, 7, 8, 16, 24, 32, 64, 65, 128};
  for (unsigned w = 0; w < 10; ++w) {
    const unsigned bits = widths[w];
    const U128 mask = ~(U128)0 >> (128 - bits);
    U128 stored = 0;
    if (functions[w](~(U128)0, &stored) != mask || stored != mask) return 1;
    for (unsigned bit = 0; bit < 128; ++bit) {
      const U128 expected = bit < bits ? (U128)1 << (bits - bit - 1) : 0;
      if (functions[w]((U128)1 << bit, &stored) != expected ||
          stored != expected) return 2;
    }
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndCheck(Source, false, {}, Optimization, true);
}

TEST(LLVMCIntrinsicSemantics, ScalarUnaryRejectsUnsupportedShapes) {
  for (const auto Kind :
       {llvm::Intrinsic::bitreverse, llvm::Intrinsic::fptosi_sat,
        llvm::Intrinsic::fptoui_sat}) {
    llvm::LLVMContext Context;
    llvm::Module Module("unsupported-unary", Context);
    llvm::IRBuilder<> Builder(Context);
    llvm::Type *Input = Kind == llvm::Intrinsic::bitreverse
                            ? Builder.getIntNTy(129)
                            : Builder.getHalfTy();
    auto *Output = Builder.getIntNTy(129);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Output, {Input}, false),
        llvm::GlobalValue::ExternalLinkage, "bad_unary", Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    llvm::SmallVector<llvm::Type *, 2> Types{Output};
    if (Kind != llvm::Intrinsic::bitreverse)
      Types.push_back(Input);
    auto *Intrinsic =
        llvm::Intrinsic::getOrInsertDeclaration(&Module, Kind, Types);
    Builder.CreateRet(Builder.CreateCall(Intrinsic, {Function->getArg(0)}));
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    EXPECT_THROW(LLVMCEmitter().emit(Module, Stream, {}), std::runtime_error);
  }
}

TEST(LLVMCIntrinsicSemantics, FunnelShiftHelpersAvoidFunctionNameCollisions) {
  for (bool IntrinsicFirst : {false, true}) {
    SCOPED_TRACE(IntrinsicFirst);
    llvm::LLVMContext Context;
    llvm::Module Module("funnel-helper-identities", Context);
    llvm::IRBuilder<> Builder(Context);
    auto *Wide = Builder.getInt128Ty();
    std::string Table;
    for (unsigned Bits : {8u, 16u, 32u, 64u, 128u}) {
      auto *Ty = Builder.getIntNTy(Bits);
      for (bool Left : {true, false}) {
        const auto ID = Left ? llvm::Intrinsic::fshl : llvm::Intrinsic::fshr;
        const std::string Suffix =
            std::string(Left ? "fshl_i" : "fshr_i") + std::to_string(Bits);
        llvm::Function *Intrinsic = nullptr;
        if (IntrinsicFirst)
          Intrinsic =
              llvm::Intrinsic::getOrInsertDeclaration(&Module, ID, {Ty});
        // A normal two-argument function occupies the intrinsic helper's
        // preferred C spelling. Both identities must survive either order.
        auto *Collision =
            llvm::Function::Create(llvm::FunctionType::get(Ty, {Ty, Ty}, false),
                                   llvm::GlobalValue::ExternalLinkage,
                                   "neverd_llvm_" + Suffix, Module);
        Builder.SetInsertPoint(
            llvm::BasicBlock::Create(Context, "entry", Collision));
        Builder.CreateRet(
            Builder.CreateXor(Collision->getArg(0), Collision->getArg(1)));
        if (!IntrinsicFirst)
          Intrinsic =
              llvm::Intrinsic::getOrInsertDeclaration(&Module, ID, {Ty});
        const std::string Name = "check_" + Suffix;
        Table += Name + ",";
        auto *Fn = llvm::Function::Create(
            llvm::FunctionType::get(Wide, {Wide, Wide, Wide}, false),
            llvm::GlobalValue::ExternalLinkage, Name, Module);
        Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Fn));
        auto *A = Builder.CreateTruncOrBitCast(Fn->getArg(0), Ty);
        auto *B = Builder.CreateTruncOrBitCast(Fn->getArg(1), Ty);
        auto *Count = Builder.CreateTruncOrBitCast(Fn->getArg(2), Ty);
        auto *Shift = Builder.CreateCall(Intrinsic, {A, B, Count});
        auto *Other = Builder.CreateCall(Collision, {A, B});
        Builder.CreateRet(
            Builder.CreateZExtOrBitCast(Builder.CreateXor(Shift, Other), Wide));
      }
    }
    const std::string Source = "#include <stdint.h>\n" + emit(Module) + R"(
typedef unsigned __int128 U128;
int main(void) {
  U128 (*functions[])(U128, U128, U128) = {)" +
                               Table + R"(};
  const unsigned widths[] = {8, 16, 32, 64, 128};
  const U128 values[] = {0, 1, 0x89abcdefu, ~(U128)0,
                        ((U128)1 << 127) | ((U128)1 << 63)};
  for (unsigned w = 0; w < 5; ++w) {
    const unsigned width = widths[w];
    const U128 mask = ~(U128)0 >> (128 - width);
    for (unsigned i = 0; i < 5; ++i)
      for (unsigned j = 0; j < 5; ++j)
        for (unsigned count = 0; count < 2 * width + 1; ++count) {
          const U128 a = values[i] & mask, b = values[j] & mask;
          const unsigned shift = (count & (unsigned)mask) % width;
          const U128 left = shift ? ((a << shift) | (b >> (width - shift))) & mask : a;
          const U128 right = shift ? ((a << (width - shift)) | (b >> shift)) & mask : b;
          if (functions[2*w](a, b, count) != (left ^ a ^ b)) return 1;
          if (functions[2*w+1](a, b, count) != (right ^ a ^ b)) return 2;
        }
  }
  return 0;
}
)";
    for (llvm::StringRef Optimization : {"-O0", "-O2"})
      compileAndCheck(Source, false, {}, Optimization, true);
  }
}

TEST(LLVMCIntrinsicSemantics, IntegerMinMaxKeepsWidthsSignednessAndProducers) {
  llvm::LLVMContext Context;
  llvm::Module Module("integer-minmax", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> Builder(Context);
  auto *Wide = Builder.getInt128Ty();
  auto Producer = Module.getOrInsertFunction("produce128", Wide, Wide);
  auto *Collision = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getInt8Ty(),
                              {Builder.getInt8Ty(), Builder.getInt8Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "neverd_llvm_umin_i8", Module);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Collision));
  Builder.CreateRet(
      Builder.CreateXor(Collision->getArg(0), Collision->getArg(1)));
  const llvm::Intrinsic::ID Kinds[] = {
      llvm::Intrinsic::umin, llvm::Intrinsic::umax, llvm::Intrinsic::smin,
      llvm::Intrinsic::smax};
  std::string Table;
  for (unsigned Bits : {1u, 8u, 16u, 32u, 64u, 128u}) {
    for (unsigned Kind = 0; Kind != 4; ++Kind) {
      for (bool Assigned : {false, true}) {
        const std::string Name = "minmax_" + std::to_string(Bits) + "_" +
                                 std::to_string(Kind) + "_" +
                                 std::to_string(Assigned);
        Table += Name + ",";
        auto *Function = llvm::Function::Create(
            llvm::FunctionType::get(Wide, {Wide, Wide, Builder.getPtrTy()},
                                    false),
            llvm::GlobalValue::ExternalLinkage, Name, Module);
        Builder.SetInsertPoint(
            llvm::BasicBlock::Create(Context, "entry", Function));
        auto *Left = Builder.CreateCall(Producer, {Function->getArg(0)});
        auto *Right = Builder.CreateCall(Producer, {Function->getArg(1)});
        auto *Integer = Builder.getIntNTy(Bits);
        auto *Intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
            &Module, Kinds[Kind], {Integer});
        auto *Chosen = Builder.CreateCall(
            Intrinsic, {Builder.CreateTruncOrBitCast(Left, Integer),
                        Builder.CreateTruncOrBitCast(Right, Integer)});
        // Two direct uses exercise assigned calls; one exercises inline calls.
        Builder.CreateStore(Assigned ? Builder.CreateZExtOrBitCast(Chosen, Wide)
                                     : Left,
                            Function->getArg(2));
        Builder.CreateRet(Builder.CreateZExtOrBitCast(Chosen, Wide));
      }
    }
  }
  const std::string Text = emit(Module);
  EXPECT_EQ(Text.find("llvm_x2E_"), std::string::npos) << Text;
  // Runtime compilers may happen to evaluate C arguments left to right. Also
  // reject an expression that leaves these LLVM-ordered calls unsequenced.
  llvm::SmallVector<llvm::StringRef, 64> Lines;
  llvm::StringRef(Text).split(Lines, '\n');
  for (llvm::StringRef Line : Lines) {
    const auto First = Line.find("produce128(");
    if (First != llvm::StringRef::npos)
      EXPECT_EQ(Line.find("produce128(", First + 1), llvm::StringRef::npos)
          << Line.str();
  }
  const std::string Source = "#include <stdint.h>\n#include <stdbool.h>\n" +
                             Text + R"(
typedef unsigned __int128 U128;
static unsigned calls;
static U128 observed[2];
U128 produce128(U128 value) {
  if (calls < 2) observed[calls] = value;
  ++calls;
  return value;
}
int main(void) {
  if (neverd_llvm_umin_i8(0xf0, 0x33) != 0xc3) return 4;
  U128 (*functions[])(U128, U128, void *) = {)" +
                             Table + R"(};
  const unsigned widths[] = {1, 8, 16, 32, 64, 128};
  for (unsigned width = 0; width < 6; ++width) {
    const unsigned bits = widths[width];
    const U128 mask = ~(U128)0 >> (128 - bits);
    const U128 sign = (U128)1 << (bits - 1);
    const U128 values[] = {0, 1, sign - 1, sign, sign + 1, mask,
                          ~(U128)0, ((U128)1 << 100) | 0xabcdef};
    for (unsigned a = 0; a < 8; ++a)
      for (unsigned b = 0; b < 8; ++b)
        for (unsigned kind = 0; kind < 4; ++kind)
          for (unsigned assigned = 0; assigned < 2; ++assigned) {
            const U128 left = values[a] & mask, right = values[b] & mask;
            const int different_signs = !!(left & sign) != !!(right & sign);
            const int less = kind >= 2 && different_signs
                               ? !!(left & sign) : left < right;
            const U128 expected = (kind & 1) ? (less ? right : left)
                                             : (less ? left : right);
            U128 stored = 0;
            calls = 0;
            if (functions[width * 8 + kind * 2 + assigned](
                    values[a], values[b], &stored) != expected) return 1;
            if (stored != (assigned ? expected : values[a])) return 2;
            if (calls != 2 || observed[0] != values[a] ||
                observed[1] != values[b]) return 3;
          }
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndCheck(Source, false, {}, Optimization);
}

TEST(LLVMCIntrinsicSemantics, IntegerMinMaxRejectsUnsupportedScalarWidths) {
  for (unsigned Bits : {2u, 7u, 24u, 65u, 129u}) {
    SCOPED_TRACE(Bits);
    llvm::LLVMContext Context;
    llvm::Module Module("unsupported-minmax", Context);
    llvm::IRBuilder<> Builder(Context);
    auto *Integer = Builder.getIntNTy(Bits);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Integer, {Integer, Integer}, false),
        llvm::GlobalValue::ExternalLinkage, "bad_minmax", Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::umin, {Integer});
    Builder.CreateRet(Builder.CreateCall(
        Intrinsic, {Function->getArg(0), Function->getArg(1)}));
    std::string Text;
    llvm::raw_string_ostream Output(Text);
    EXPECT_THROW(LLVMCEmitter().emit(Module, Output, {}), std::runtime_error);
  }
}

TEST(LLVMCIntrinsicSemantics, IntegerMinMaxRejectsMismatchedOperands) {
  llvm::LLVMContext Context;
  llvm::Module Module("malformed-minmax", Context);
  llvm::IRBuilder<> Builder(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getInt8Ty(),
                              {Builder.getInt8Ty(), Builder.getInt16Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "bad_minmax", Module);
  auto *Malformed = llvm::Function::Create(Function->getFunctionType(),
                                           llvm::GlobalValue::ExternalLinkage,
                                           "llvm.umin.i8", Module);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRet(Builder.CreateCall(
      Malformed, {Function->getArg(0), Function->getArg(1)}));
  std::string Text;
  llvm::raw_string_ostream Output(Text);
  EXPECT_THROW(LLVMCEmitter().emit(Module, Output, {}), std::runtime_error);
}

TEST(LLVMCIntrinsicSemantics, IntegerMinMaxRejectsMismatchedCallsite) {
  llvm::LLVMContext Context;
  llvm::Module Module("mismatched-minmax-callsite", Context);
  llvm::IRBuilder<> Builder(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getInt16Ty(),
                              {Builder.getInt8Ty(), Builder.getInt8Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "bad_minmax", Module);
  auto *Intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::umin, {Builder.getInt8Ty()});
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  // Opaque pointers permit constructing this malformed call, for which
  // getCalledFunction() returns null rather than the known declaration.
  auto *Call = Builder.CreateCall(Function->getFunctionType(), Intrinsic,
                                  {Function->getArg(0), Function->getArg(1)});
  ASSERT_EQ(Call->getCalledFunction(), nullptr);
  Builder.CreateRet(Call);
  std::string Text;
  llvm::raw_string_ostream Output(Text);
  EXPECT_THROW(LLVMCEmitter().emit(Module, Output, {}), std::runtime_error);
}

TEST(LLVMCIntrinsicSemantics, SingleUseCallsKeepEffectsAndExecutionPaths) {
  llvm::LLVMContext Context;
  llvm::Module Module("ordered-calls", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> Builder(Context);
  auto *Word = Builder.getInt64Ty();
  auto Producer = Module.getOrInsertFunction("next_value", Word, Word);
  auto Consumer =
      Module.getOrInsertFunction("combine_values", Word, Word, Word);
  auto Read = Module.getOrInsertFunction("read_cell", Word, Builder.getPtrTy());

  auto *Pair = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Word, Word}, false),
      llvm::GlobalValue::ExternalLinkage, "ordered_pair", Module);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Pair));
  auto *Left = Builder.CreateCall(Producer, {Pair->getArg(0)});
  auto *Right = Builder.CreateCall(Producer, {Pair->getArg(1)});
  Builder.CreateRet(Builder.CreateAdd(
      Builder.CreateCall(Consumer, {Left, Right}), Builder.getInt64(1)));

  auto *Memory = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Builder.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "read_before_store", Module);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Memory));
  auto *ReadBefore = Builder.CreateCall(Read, {Memory->getArg(0)});
  Builder.CreateStore(Builder.getInt64(7), Memory->getArg(0));
  Builder.CreateRet(Builder.CreateAdd(ReadBefore, Builder.getInt64(1)));

  auto *Conditional = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Word}, false),
      llvm::GlobalValue::ExternalLinkage, "call_before_branch", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Conditional);
  auto *Then = llvm::BasicBlock::Create(Context, "then", Conditional);
  auto *Else = llvm::BasicBlock::Create(Context, "else", Conditional);
  Builder.SetInsertPoint(Entry);
  auto *Always = Builder.CreateCall(Producer, {Conditional->getArg(0)});
  Builder.CreateCondBr(
      Builder.CreateICmpNE(Conditional->getArg(0), Builder.getInt64(0)), Then,
      Else);
  Builder.SetInsertPoint(Then);
  Builder.CreateRet(Builder.CreateAdd(Always, Builder.getInt64(1)));
  Builder.SetInsertPoint(Else);
  Builder.CreateRet(Builder.getInt64(99));

  const std::string Text = emit(Module);
  llvm::SmallVector<llvm::StringRef, 64> Lines;
  llvm::StringRef(Text).split(Lines, '\n');
  for (llvm::StringRef Line : Lines) {
    const auto First = Line.find("next_value(");
    if (First != llvm::StringRef::npos)
      EXPECT_EQ(Line.find("next_value(", First + 1), llvm::StringRef::npos)
          << Line.str();
  }
  const std::string Source =
      "#include <stdint.h>\n#include <stdbool.h>\n" + Text + R"(
static unsigned calls;
static uint64_t observed[2];
uint64_t next_value(uint64_t value) {
  if (calls < 2) observed[calls] = value;
  ++calls;
  return value;
}
uint64_t combine_values(uint64_t left, uint64_t right) {
  if (calls != 2 || observed[0] != left || observed[1] != right) return 999;
  return left * 10 + right;
}
uint64_t read_cell(void *pointer) { return *(uint64_t *)pointer; }
int main(void) {
  calls = 0;
  if (ordered_pair(3, 5) != 36 || calls != 2) return 1;
  uint64_t cell = 41;
  if (read_before_store(&cell) != 42 || cell != 7) return 2;
  for (uint64_t value = 0; value < 2; ++value) {
    calls = 0;
    if (call_before_branch(value) != (value ? value + 1 : 99) ||
        calls != 1 || observed[0] != value) return 3;
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndCheck(Source, false, {}, Optimization);
}

TEST(LLVMCIntrinsicSemantics, RepStosFrameBufferSharesBackingWithIndexedLoads) {
  std::string Source = "#include <stdint.h>\n";
  for (bool Backward : {false, true}) {
    llvm::LLVMContext C;
    llvm::Module M("frame-stos", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<llvm::NoFolder> B(C);
    const std::string Name = Backward ? "frame_fill_backward" : "frame_fill";
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(),
                                {B.getInt64Ty(), B.getInt64Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, Name, M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
    auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 160),
                                 nullptr, "frame");
    auto Addr = [&](uint64_t Offset) {
      return B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(Offset));
    };
    B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(144), "frame_end");
    B.CreateStore(B.getInt8(0xCC), Addr(79));
    B.CreateStore(B.getInt8(0xDD), Addr(144));
    B.CreateStore(B.getInt8(0), Addr(80));
    auto *RetTy = llvm::StructType::get(C, {B.getInt64Ty(), B.getInt64Ty()});
    auto *AsmTy = llvm::FunctionType::get(
        RetTy, {B.getInt64Ty(), B.getInt64Ty(), B.getInt8Ty()}, false);
    auto *IA = llvm::InlineAsm::get(
        AsmTy, Backward ? "std\n\trep stosb\n\tcld" : "rep stosb",
        "={di},={cx},0,1,{ax},~{memory},~{dirflag},~{cc}", true);
    B.CreateCall(IA,
                 {B.CreatePtrToInt(Addr(Backward ? 143 : 80), B.getInt64Ty()),
                  F->getArg(1), B.getInt8(0x5A)});
    auto *Indexed = B.CreateGEP(B.getInt8Ty(), Frame,
                                B.CreateAdd(F->getArg(0), B.getInt64(80)));
    auto Load = [&](llvm::Value *Ptr) {
      return B.CreateZExt(B.CreateLoad(B.getInt8Ty(), Ptr), B.getInt32Ty());
    };
    auto *Result =
        B.CreateOr(Load(Indexed), B.CreateShl(Load(Addr(80)), B.getInt32(8)));
    Result = B.CreateOr(Result, B.CreateShl(Load(Addr(79)), B.getInt32(16)));
    Result = B.CreateOr(Result, B.CreateShl(Load(Addr(144)), B.getInt32(24)));
    B.CreateRet(Result);
    const std::string Text = emit(M);
    EXPECT_EQ(Text.find("&var_"), std::string::npos) << Text;
    Source += Text;
  }
  Source += "int main(void) { for(uint64_t i=0;i<64;++i) { "
            "if(frame_fill(i,64)!=0xDDCC5A5Au) return 1; "
            "if(frame_fill_backward(i,64)!=0xDDCC5A5Au) return 2; "
            "} return 0; }\n";
  compileAndCheck(Source);
}

TEST(LLVMCIntrinsicSemantics, RepStosForwardBackwardDynamicAndZeroCount) {
  std::string Source = "#include <stdint.h>\n";
  for (unsigned Bytes : {1u, 2u, 4u, 8u}) {
    for (auto Dir :
         {X86RepStos::Forward, X86RepStos::Backward, X86RepStos::Dynamic}) {
      const std::string Name =
          "fill" + std::to_string(Bytes) + "_" + std::to_string(Dir);
      StosFixture F(Bytes, Dir, Name);
      ASSERT_TRUE(classifyX86RepStos(Arch::X64, *F.Call));
      Source += emit(F.M);
    }
  }
  Source += "int main(void) { unsigned char data[80];\n";
  for (unsigned Bytes : {1u, 2u, 4u, 8u}) {
    for (auto Dir :
         {X86RepStos::Forward, X86RepStos::Backward, X86RepStos::Dynamic}) {
      for (bool Backward : {false, true}) {
        if (Dir != X86RepStos::Dynamic &&
            Backward != (Dir == X86RepStos::Backward))
          continue;
        const std::string Name =
            "fill" + std::to_string(Bytes) + "_" + std::to_string(Dir);
        const std::string Df =
            Dir == X86RepStos::Dynamic ? (Backward ? ",7" : ",0") : "";
        const unsigned Start = 8 + (Backward ? 3 * Bytes : 0);
        Source += "for (unsigned i=0;i<80;++i) data[i]=0xCC;\n";
        Source += Name + "((uintptr_t)&data[" + std::to_string(Start) +
                  "],4,(uint" + std::to_string(Bytes * 8) +
                  "_t)0x8877665544332211ULL" + Df + ");\n";
        Source += "for (unsigned i=0;i<80;++i) { unsigned char expected=0xCC; "
                  "if(i>=8 && i<" +
                  std::to_string(8 + 4 * Bytes) +
                  ") "
                  "expected=(unsigned char)(0x8877665544332211ULL >> (((i-8)%" +
                  std::to_string(Bytes) +
                  ")*8)); if(data[i]!=expected)return 1; }\n";
        Source += Name + "(0,0,0" + Df + ");\n";
      }
    }
  }
  Source += "return 0;}\n";
  compileAndCheck(Source);
}

TEST(LLVMCIntrinsicSemantics, RepStosRejectsUnprovedContracts) {
  for (unsigned Variant = 0; Variant < 3; ++Variant) {
    StosFixture F(1, X86RepStos::Dynamic, "bad_fill", Variant == 0,
                  Variant == 1, Variant == 2);
    EXPECT_FALSE(classifyX86RepStos(Arch::X64, *F.Call));
    EXPECT_DEATH((void)emit(F.M),
                 "unsupported REP STOS inline assembly contract");
  }
}

TEST(LLVMCIntrinsicSemantics,
     LocalAddressKeepsTargetIntrinsicAndInitializesResult) {
  llvm::LLVMContext C;
  llvm::Module M("localaddress", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *Fn =
      llvm::Function::Create(llvm::FunctionType::get(B.getVoidTy(), false),
                             llvm::GlobalValue::ExternalLinkage, "parent", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Address = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
      &M, llvm::Intrinsic::localaddress));
  auto Consume =
      M.getOrInsertFunction("use_parent", B.getVoidTy(), B.getPtrTy());
  B.CreateCall(Consume, {Address});
  B.CreateRetVoid();
  const auto Text = emit(M);
  EXPECT_NE(Text.find("__asm__(\"llvm.localaddress\")"), std::string::npos)
      << Text;
  compileAndCheck(Text, true);
}

TEST(LLVMCIntrinsicSemantics, IntegerAddressAdditionUsesIntegerArithmetic) {
  llvm::LLVMContext C;
  llvm::Module M("address-add", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy(), B.getInt64Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "add_address", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Address = B.CreatePtrToInt(Fn->getArg(0), B.getInt64Ty());
  auto *Added = B.CreateAdd(Address, Fn->getArg(1));
  // Two uses force the addition to remain a statement in the C output.
  auto Sink =
      M.getOrInsertFunction("use_integer", B.getVoidTy(), B.getInt64Ty());
  B.CreateCall(Sink, {Added});
  B.CreateRet(Added);
  const auto Text = emit(M);
  compileAndCheck("#include <stdint.h>\n" + Text +
                  "void use_integer(uint64_t x) {(void)x;}\n"
                  "int main(void) {char b[8]; return add_address(b, "
                  "UINT64_MAX) != (uintptr_t)b-1;}\n");
}

TEST(LLVMCIntrinsicSemantics, ConstantRotateGuardsUseActualIntegerWidths) {
  llvm::LLVMContext C;
  llvm::Module M("rotate-guards", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> B(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt64Ty(), {B.getInt64Ty()}, false),
      llvm::GlobalValue::ExternalLinkage, "rotate_guard", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto Shift = [&](unsigned Count, bool Left) {
    auto *Home = B.CreateAlloca(B.getInt64Ty());
    B.CreateStore(B.getInt64(Count), Home);
    auto *N = B.CreateLoad(B.getInt64Ty(), Home);
    auto *Masked = B.CreateURem(N, B.getInt64(64));
    auto *Value = Left ? B.CreateShl(Fn->getArg(0), Masked)
                       : B.CreateLShr(Fn->getArg(0), Masked);
    return B.CreateSelect(B.CreateICmpULT(N, B.getInt64(64)), Value,
                          B.getInt64(0));
  };
  auto *Rotated = B.CreateOr(Shift(16, true), Shift(48, false));
  // i8 255 is negative only for signed comparison; zext must retain 255.
  auto *Unsigned = B.CreateICmpULT(B.getInt8(255), B.getInt8(1));
  auto *Signed = B.CreateICmpSLT(B.getInt8(255), B.getInt8(1));
  auto *Extended = B.CreateICmpEQ(B.CreateZExt(B.getInt8(255), B.getInt64Ty()),
                                  B.getInt64(255));
  auto *Wrapped =
      B.CreateICmpEQ(B.CreateAdd(B.getInt8(255), B.getInt8(1)), B.getInt8(0));
  auto *Correct = B.CreateAnd(B.CreateAnd(B.CreateNot(Unsigned), Signed),
                              B.CreateAnd(Extended, Wrapped));
  B.CreateRet(B.CreateSelect(Correct, Rotated, B.getInt64(0)));
  const auto Text = emit(M);
  EXPECT_EQ(Text.find(" % "), std::string::npos) << Text;
  EXPECT_EQ(Text.find(" ? "), std::string::npos) << Text;
  compileAndCheck(
      "#include <stdint.h>\n" + Text +
      "int main(void) { uint64_t a[]={0,1,0x8000000000000001ULL,UINT64_MAX};"
      "for(unsigned i=0;i<4;++i)if(rotate_guard(a[i])!=((a[i]<<16)|(a[i]>>48)))"
      "return 1; return 0;}\n");
}

TEST(LLVMCIntrinsicSemantics, ConstantSelectPreservesCallAndVolatileProducer) {
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    llvm::LLVMContext C;
    llvm::Module M("select-effects", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<llvm::NoFolder> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy(), B.getInt1Ty()},
                                false),
        llvm::GlobalValue::ExternalLinkage, "select_effects", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    auto Callee = M.getOrInsertFunction("tick", B.getInt64Ty());
    auto *Called = B.CreateCall(Callee);
    auto *Loaded = B.CreateLoad(B.getInt64Ty(), Fn->getArg(0));
    Loaded->setVolatile(Mode != 1);
    if (Mode != 0)
      Loaded->setAtomic(Mode == 1
                            ? llvm::AtomicOrdering::Acquire
                            : llvm::AtomicOrdering::SequentiallyConsistent);
    auto *Chosen = B.CreateSelect(B.CreateICmpULT(B.getInt8(255), B.getInt8(1)),
                                  Called, Loaded);
    B.CreateRet(B.CreateSelect(Fn->getArg(1), Chosen, B.getInt64(9)));
    const auto Text = emit(M);
    EXPECT_NE(Text.find("tick()"), std::string::npos) << Text;
    if (Mode != 1)
      EXPECT_NE(Text.find("volatile"), std::string::npos) << Text;
    if (Mode != 0) {
      EXPECT_NE(Text.find("__atomic_load("), std::string::npos) << Text;
      EXPECT_NE(Text.find(Mode == 1 ? "__ATOMIC_ACQUIRE" : "__ATOMIC_SEQ_CST"),
                std::string::npos)
          << Text;
    }
    EXPECT_NE(Text.find(" ? "), std::string::npos) << Text;
    compileAndCheck("#include <stdint.h>\n#include <stdbool.h>\n" + Text +
                    "static int calls; uint64_t tick(void) {++calls;return 3;}"
                    "int main(void) { uint64_t x=17; "
                    "if(select_effects(&x,1)!=17 || calls!=1)"
                    "return 1; if(select_effects(&x,0)!=9 || calls!=2)return "
                    "2;return 0;}\n");
  }
}

TEST(LLVMCIntrinsicSemantics, VolatileAndAtomicPointerLoadsQualifyTheSlot) {
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    llvm::LLVMContext C;
    llvm::Module M("pointer-load", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getPtrTy(), {B.getPtrTy()}, false),
        llvm::GlobalValue::ExternalLinkage, "read_pointer", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    auto *Loaded = B.CreateLoad(B.getPtrTy(), Fn->getArg(0));
    Loaded->setVolatile(Mode != 1);
    if (Mode != 0)
      Loaded->setAtomic(Mode == 1
                            ? llvm::AtomicOrdering::Acquire
                            : llvm::AtomicOrdering::SequentiallyConsistent);
    B.CreateRet(Loaded);
    const auto Text = emit(M);
    if (Mode != 1)
      EXPECT_NE(Text.find("void* volatile*"), std::string::npos) << Text;
    compileAndCheck(
        "#include <stdint.h>\n" + Text +
        "int main(void) {int x=7;void *p=&x;return read_pointer(&p)!=&x;}\n");
  }
}

TEST(LLVMCIntrinsicSemantics, UnusedObservableLoadKeepsConditionalBlock) {
  for (bool Atomic : {false, true}) {
    SCOPED_TRACE(Atomic);
    llvm::LLVMContext C;
    llvm::Module M("unused-observable-load", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), {B.getPtrTy(), B.getInt1Ty()},
                                false),
        llvm::GlobalValue::ExternalLinkage, "conditional_read", M);
    auto *Entry = llvm::BasicBlock::Create(C, "entry", Fn);
    auto *Probe = llvm::BasicBlock::Create(C, "probe", Fn);
    auto *Join = llvm::BasicBlock::Create(C, "join", Fn);
    B.SetInsertPoint(Entry);
    B.CreateCondBr(Fn->getArg(1), Probe, Join);
    B.SetInsertPoint(Probe);
    auto *Loaded = B.CreateLoad(B.getInt32Ty(), Fn->getArg(0), "observed");
    if (Atomic)
      Loaded->setAtomic(llvm::AtomicOrdering::Acquire);
    else
      Loaded->setVolatile(true);
    B.CreateBr(Join);
    B.SetInsertPoint(Join);
    B.CreateRet(B.getInt32(7));
    const auto Text = "#include <stdint.h>\n" + emit(M);
    // Windows C may strengthen a volatile read to an atomic acquire read.
    compileAndCheck(
        Text, true,
        Atomic ? std::vector<llvm::StringRef>{"load atomic", " acquire"}
               : std::vector<llvm::StringRef>{"volatile i32"});
    compileAndCheck(
        Text + "int main(void){uint32_t x=1;return "
               "conditional_read(0,0)!=7 || conditional_read(&x,1)!=7;}\n");
  }
}

struct ObservableFieldDebug final : NullDebugContext {
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    if (Address != 0x1000)
      return std::nullopt;
    auto Record = NdType::makeNamedRecord("ObservedRecord", 8);
    Record->FieldDisplayNames = {"bits"};
    Record->FieldDisplayOffsets = {4};
    Record->FieldDisplayTypes = {NdType::makeInt(4, false)};
    FunctionSym F;
    F.Name = "read_then_clear";
    F.Addr = Address;
    F.Params = {{"this", NdType::makePtr(Record)}};
    return F;
  }
  bool hasInfo() const override { return true; }
};

TEST(LLVMCIntrinsicSemantics, ObservableFieldCastUsesCapturedValue) {
  for (bool Atomic : {false, true}) {
    SCOPED_TRACE(Atomic);
    llvm::LLVMContext C;
    llvm::Module M("observable-field", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<llvm::NoFolder> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy()}, false),
        llvm::GlobalValue::ExternalLinkage, "read_then_clear", M);
    rewrite_source::setOriginalVA(*Fn, 0x1000);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    auto *Ptr = B.CreateGEP(B.getInt8Ty(), Fn->getArg(0), B.getInt64(4));
    auto *Loaded = B.CreateLoad(B.getInt32Ty(), Ptr, "observed_field");
    if (Atomic)
      Loaded->setAtomic(llvm::AtomicOrdering::Acquire);
    else
      Loaded->setVolatile(true);
    B.CreateStore(B.getInt32(0), Ptr);
    B.CreateRet(B.CreateSExt(Loaded, B.getInt64Ty()));
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    CEmitterOptions Options;
    Options.EmitIncludes = false;
    ObservableFieldDebug Debug;
    ASSERT_TRUE(LLVMCEmitter().emit(M, OS, Options, &Debug));
    compileAndCheck(
        "#include <stdint.h>\n"
        "typedef struct {uint32_t pad,bits;} ObservedRecord;\n" +
        Text +
        "int main(void){ObservedRecord r={0,0x80000001u};return "
        "read_then_clear(&r)!=UINT64_C(0xffffffff80000001) || r.bits!=0;}\n");
  }
}

TEST(LLVMCIntrinsicSemantics, ObservableReadonlyImageLoadKeepsDeclaration) {
  for (bool Only : {false, true}) {
    for (bool Atomic : {false, true}) {
      SCOPED_TRACE(Only);
      SCOPED_TRACE(Atomic);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      Image.Base = 0x1000;
      Segment Data;
      Data.VA = 0x2000;
      Data.Size = 4;
      Data.FileSz = 4;
      Data.Flags = SegmentFlags::Readable;
      Data.Data = {17, 0, 0, 0};
      Image.Segments.push_back(Data);
      llvm::LLVMContext C;
      llvm::Module M("readonly-observable", C);
      M.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(C);
      auto *Global = new llvm::GlobalVariable(
          M, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage, nullptr,
          "__nd_data_2000");
      auto *Fn = llvm::Function::Create(
          llvm::FunctionType::get(B.getInt32Ty(), false),
          llvm::GlobalValue::ExternalLinkage, "read_image", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
      auto *Loaded = B.CreateLoad(B.getInt32Ty(), Global);
      if (Atomic)
        Loaded->setAtomic(llvm::AtomicOrdering::Acquire);
      else
        Loaded->setVolatile(true);
      B.CreateRet(Loaded);
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      CEmitterOptions Options;
      Options.EmitIncludes = false;
      ASSERT_TRUE(LLVMCEmitter().emit(M, OS, Options, nullptr, &Image,
                                      Only ? Fn : nullptr));
      EXPECT_NE(Text.find("extern uint32_t dword_2000;"), std::string::npos)
          << Text;
      // The snapshot says 17, but the retained read must observe the actual
      // C object's value rather than silently substituting that snapshot.
      compileAndCheck(
          "#include <stdint.h>\n" + Text +
          "uint32_t dword_2000=29;int main(void){return read_image()!=29;}\n");
    }
  }
}

TEST(LLVMCIntrinsicSemantics, X87FpremKeepsTenByteOperandsAndC2) {
  llvm::LLVMContext C;
  llvm::Module M("x87-fprem-c2", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *I80 = B.getIntNTy(80);
  auto *F80 = llvm::Type::getX86_FP80Ty(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(
          B.getInt32Ty(),
          {B.getPtrTy(), B.getPtrTy(), B.getPtrTy(), B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "fprem_c2", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto Arg = Fn->arg_begin();
  llvm::Value *Large = &*Arg++;
  llvm::Value *Small = &*Arg++;
  llvm::Value *Remainder = &*Arg++;
  llvm::Value *StatusOut = &*Arg;
  auto *Init = llvm::InlineAsm::get(
      llvm::FunctionType::get(B.getVoidTy(), {}, false), "fninit",
      x87StateConstraints(X87StateEffect::Reset), true);
  B.CreateCall(Init);
  auto *LargeBits = B.CreateLoad(I80, Large, "large_bits");
  LargeBits->setVolatile(true);
  auto *SmallBits = B.CreateLoad(I80, Small, "small_bits");
  SmallBits->setVolatile(true);
  auto *Fprem = llvm::InlineAsm::get(
      llvm::FunctionType::get(F80, {F80, F80}, false), "fprem",
      "=&{st},0,{st(1)},~{dirflag},~{fpsr},~{flags}", true);
  auto *Reduced = B.CreateCall(
      Fprem, {B.CreateBitCast(LargeBits, F80), B.CreateBitCast(SmallBits, F80)},
      "partial_remainder");
  // A pure bitcast between FPREM and FNSTSW can make C compilation spill and
  // pop the x87 stack.  The status must be captured in the same asm as FPREM.
  auto *RemainderBits = B.CreateBitCast(Reduced, I80);
  auto *Fnstsw = llvm::InlineAsm::get(
      llvm::FunctionType::get(B.getInt16Ty(), {}, false), "fnstsw $0",
      "={ax},~{dirflag},~{fpsr},~{flags}", true);
  auto *Status = B.CreateCall(Fnstsw, {}, "x87_status");
  B.CreateStore(Status, StatusOut)->setVolatile(true);
  B.CreateStore(RemainderBits, Remainder)->setVolatile(true);
  auto *C2 = B.CreateAnd(Status, B.getInt16(0x0400));
  B.CreateRet(B.CreateSelect(B.CreateICmpNE(C2, B.getInt16(0)), B.getInt32(0),
                             B.getInt32(1)));

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("__uint128_t"), std::string::npos) << Text;
  EXPECT_NE(Text.find("long double"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__builtin_memcpy"), std::string::npos) << Text;
  EXPECT_NE(Text.find("fprem\\n\\tfnstsw %%ax"), std::string::npos) << Text;
  EXPECT_NE(Text.find("fnstsw %%ax"), std::string::npos) << Text;
  EXPECT_NE(Text.find("1024"), std::string::npos) << Text;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck(
      "#include <stdint.h>\n" + Text +
      "int main(void) {\n"
      "  unsigned char large[10] = {1,0,0,0,0,0,0,0x80,0xfe,0x7f};\n"
      "  unsigned char small[10] = {3,0,0,0,0,0,0,0x80,0xbe,0xff};\n"
      "  unsigned char remainder[10] = {0};\n"
      "  uint16_t status = 0;\n"
      "  int result = fprem_c2(large, small, remainder, &status);\n"
      "  return result || (status & 0x3c00) != 0x3400;\n"
      "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, X87FusedFpremStatusKeepsTop) {
  llvm::LLVMContext C;
  llvm::Module M("x87-fused-fprem-status", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *I80 = B.getIntNTy(80);
  auto *F80 = llvm::Type::getX86_FP80Ty(C);
  auto *Pair = llvm::StructType::create(C, "struct.neverd.x87.fprem_result");
  Pair->setBody({F80, B.getInt16Ty()});
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(
          B.getVoidTy(), {B.getPtrTy(), B.getPtrTy(), B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "fused_fprem", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto Arg = Fn->arg_begin();
  llvm::Value *Large = &*Arg++;
  llvm::Value *Small = &*Arg++;
  llvm::Value *StatusOut = &*Arg;
  auto *Init = llvm::InlineAsm::get(
      llvm::FunctionType::get(B.getVoidTy(), {}, false), "fninit",
      x87StateConstraints(X87StateEffect::Reset), true);
  B.CreateCall(Init);
  auto *LargeBits = B.CreateLoad(I80, Large, "large_bits");
  LargeBits->setVolatile(true);
  auto *SmallBits = B.CreateLoad(I80, Small, "small_bits");
  SmallBits->setVolatile(true);
  auto *Fprem = llvm::InlineAsm::get(
      llvm::FunctionType::get(Pair, {F80, F80}, false), "fprem\n\tfnstsw $1",
      "=&{st},={ax},0,{st(1)},~{dirflag},~{fpsr},~{flags}", true);
  auto *Reduced = B.CreateCall(
      Fprem, {B.CreateBitCast(LargeBits, F80), B.CreateBitCast(SmallBits, F80)},
      "partial_remainder_and_status");
  auto *RemainderBits = B.CreateBitCast(B.CreateExtractValue(Reduced, 0), I80);
  B.CreateStore(B.CreateExtractValue(Reduced, 1), StatusOut)->setVolatile(true);
  (void)RemainderBits;
  B.CreateRetVoid();

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("fprem\\n\\tfnstsw %%ax"), std::string::npos) << Text;
  EXPECT_NE(Text.find("field_0"), std::string::npos) << Text;
  EXPECT_NE(Text.find("field_1"), std::string::npos) << Text;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck(
      "#include <stdint.h>\n" + Text +
      "int main(void) {\n"
      "  unsigned char large[10] = {1,0,0,0,0,0,0,0x80,0xfe,0x7f};\n"
      "  unsigned char small[10] = {3,0,0,0,0,0,0,0x80,0xbe,0xff};\n"
      "  uint16_t status = 0;\n"
      "  fused_fprem(large, small, &status);\n"
      "  return (status & 0x3c00) != 0x3400;\n"
      "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, LinuxX64SyscallUsesRegisterABI) {
  llvm::LLVMContext C;
  llvm::Module M("linux-x64-syscall", C);
  M.setDataLayout("e-p:64:64");
  M.setTargetTriple(llvm::Triple("x86_64-unknown-linux-gnu"));
  llvm::IRBuilder<> B(C);
  auto *I64 = B.getInt64Ty();
  auto *Pair = llvm::StructType::create(C, "neverd.x64.syscall_result");
  Pair->setBody({I64, I64});
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(I64, {B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "neverd_test_getpid", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Syscall = llvm::InlineAsm::get(
      llvm::FunctionType::get(Pair, {I64, I64, I64, I64, I64, I64, I64}, false),
      "syscall",
      "={ax},={r11},0,{di},{si},{dx},{r10},{r8},{r9},~{rcx},~{memory},"
      "~{dirflag},~{fpsr},~{flags}",
      true);
  auto *PairValue = B.CreateCall(
      Syscall, {B.getInt64(39), B.getInt64(0), B.getInt64(0), B.getInt64(0),
                B.getInt64(0), B.getInt64(0), B.getInt64(0)});
  B.CreateStore(B.CreateExtractValue(PairValue, 1), &*Fn->arg_begin());
  B.CreateRet(B.CreateExtractValue(PairValue, 0));

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("__asm__(\"r10\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__(\"r8\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__(\"r9\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__(\"r11\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__ volatile(\"syscall\""), std::string::npos)
      << Text;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck("#include <stdint.h>\n#include <unistd.h>\n" + Text +
                  "int main(void) {\n"
                  "  uint64_t flags = 0;\n"
                  "  uint64_t pid = neverd_test_getpid(&flags);\n"
                  "  return pid == (uint64_t)getpid() && (flags & 2) ? 0 : 1;\n"
                  "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, WindowsX64SyscallUsesTheServiceConvention) {
  llvm::LLVMContext C;
  llvm::Module M("windows-x64-syscall", C);
  M.setDataLayout("e-p:64:64");
  M.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::IRBuilder<> B(C);
  auto *I64 = B.getInt64Ty();
  auto *Regs = llvm::StructType::create(C, "neverd.x64.windows_syscall_result");
  Regs->setBody({I64, I64, I64, I64, I64, I64});
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(I64, {B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "neverd_test_nt_service", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Syscall = llvm::InlineAsm::get(
      llvm::FunctionType::get(Regs, {I64, I64, I64, I64, I64}, false),
      "syscall",
      "={ax},={r11},={dx},={r8},={r9},={r10},0,2,3,4,5,~{rcx},~{memory},"
      "~{dirflag},~{fpsr},~{flags}",
      true);
  auto *Result =
      B.CreateCall(Syscall, {B.getInt64(0x55), B.getInt64(2), B.getInt64(3),
                             B.getInt64(4), B.getInt64(1)});
  B.CreateStore(B.CreateExtractValue(Result, 2), &*Fn->arg_begin());
  B.CreateRet(B.CreateExtractValue(Result, 0));

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("requires Windows x86-64"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__(\"r10\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__(\"r8\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__(\"r9\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__(\"r11\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("\"+d\"("), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__ volatile(\"syscall\""), std::string::npos)
      << Text;
}

TEST(LLVMCIntrinsicSemantics, X87I80BitcastMaterializesExpressionSource) {
  llvm::LLVMContext C;
  llvm::Module M("x87-i80-bitcast-expression", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> B(C);
  auto *I80 = B.getIntNTy(80);
  auto *F80 = llvm::Type::getX86_FP80Ty(C);
  auto *Fn = llvm::Function::Create(llvm::FunctionType::get(I80, {I80}, false),
                                    llvm::GlobalValue::ExternalLinkage,
                                    "x87_bits_roundtrip", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Value = B.CreateAdd(&*Fn->arg_begin(), llvm::ConstantInt::get(I80, 1));
  B.CreateRet(B.CreateBitCast(B.CreateBitCast(Value, F80), I80));

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("__uint128_t"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__builtin_memcpy"), std::string::npos) << Text;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck("#include <stdint.h>\n" + Text +
                  "int main(void) {\n"
                  "  __uint128_t bits = (((__uint128_t)0x7ffeULL << 64) | "
                  "0x8000000000000001ULL);\n"
                  "  return x87_bits_roundtrip(bits) == bits + 1 ? 0 : 1;\n"
                  "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, FloatingBitcastsKeepSignedZeroAndNaNPayloads) {
  llvm::LLVMContext Context;
  llvm::Module Module("floating-bitcast-representations", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> Builder(Context);
  for (unsigned Bits : {32u, 64u}) {
    auto *Integer = Builder.getIntNTy(Bits);
    auto *Float = Bits == 32 ? Builder.getFloatTy() : Builder.getDoubleTy();
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Integer, {Integer}, false),
        llvm::GlobalValue::ExternalLinkage, "bits" + std::to_string(Bits),
        Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Builder.CreateRet(Builder.CreateBitCast(
        Builder.CreateBitCast(Function->getArg(0), Float), Integer));
  }
  compileAndCheck("#include <stdint.h>\n" + emit(Module) + R"(
int main(void) {
  const uint32_t small[] = {0, UINT32_C(0x80000000), UINT32_C(0x7f800000),
                           UINT32_C(0x7fc12345), UINT32_C(0xffc54321)};
  const uint64_t large[] = {0, UINT64_C(0x8000000000000000),
      UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8123456789abc),
      UINT64_C(0xfff8fedcba987654)};
  for (unsigned i = 0; i < sizeof(small) / sizeof(small[0]); ++i)
    if (bits32(small[i]) != small[i] || bits64(large[i]) != large[i])
      return (int)i + 1;
  return 0;
}
)");
}

TEST(LLVMCIntrinsicSemantics, FloatingComparisonsReadBitcastValue) {
  llvm::LLVMContext Context;
  llvm::Module Module("floating-bitcast-comparison", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> Builder(Context);
  for (unsigned Bits : {32U, 64U}) {
    auto *Integer = Builder.getIntNTy(Bits);
    auto *Float = Bits == 32 ? Builder.getFloatTy() : Builder.getDoubleTy();
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getInt32Ty(), {Integer}, false),
        llvm::GlobalValue::ExternalLinkage, "classify" + std::to_string(Bits),
        Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Value = Builder.CreateBitCast(Function->getArg(0), Float);
    auto *Zero = llvm::ConstantFP::get(Float, 0.0);
    auto *Equal = Builder.CreateZExt(Builder.CreateFCmpOEQ(Value, Zero),
                                     Builder.getInt32Ty());
    auto *Less = Builder.CreateZExt(Builder.CreateFCmpOLT(Value, Zero),
                                    Builder.getInt32Ty());
    auto *NaN = Builder.CreateZExt(Builder.CreateFCmpUNO(Value, Zero),
                                   Builder.getInt32Ty());
    Builder.CreateRet(
        Builder.CreateOr(Builder.CreateOr(Equal, Builder.CreateShl(Less, 1)),
                         Builder.CreateShl(NaN, 2)));
  }
  const auto Source = "#include <stdint.h>\n" + emit(Module) + R"(
int main(void) {
  const uint32_t small[] = {0, UINT32_C(0x80000000), UINT32_C(0x3f800000),
    UINT32_C(0xbf800000), UINT32_C(0x7f800000), UINT32_C(0xff800000),
    UINT32_C(0x7fc12345), UINT32_C(0xffc54321), 1, UINT32_C(0x80000001)};
  const uint64_t large[] = {0, UINT64_C(0x8000000000000000),
    UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000000),
    UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
    UINT64_C(0x7ff8123456789abc), UINT64_C(0xfff8fedcba987654),
    1, UINT64_C(0x8000000000000001)};
  const unsigned expected[] = {1, 1, 0, 2, 0, 2, 4, 4, 0, 2};
  for (unsigned i = 0; i != 10; ++i)
    if (classify32(small[i]) != expected[i] ||
        classify64(large[i]) != expected[i]) return (int)i + 1;
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    compileAndCheck(Source, false, {}, Level);
}

TEST(LLVMCIntrinsicSemantics, FloatingPredicatesKeepNaNsAndProducerEffects) {
  llvm::LLVMContext Context;
  llvm::Module Module("floating-predicates", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> Builder(Context);
  auto *Double = Builder.getDoubleTy();
  auto *Producer = llvm::Function::Create(
      llvm::FunctionType::get(Double, {Double}, false),
      llvm::GlobalValue::ExternalLinkage, "produce", Module);
  for (unsigned Index = 0; Index <= llvm::CmpInst::FCMP_TRUE; ++Index) {
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getInt32Ty(), {Double, Double}, false),
        llvm::GlobalValue::ExternalLinkage, "compare" + std::to_string(Index),
        Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Left = Builder.CreateCall(Producer, {Function->getArg(0)});
    auto *Right = Builder.CreateCall(Producer, {Function->getArg(1)});
    auto *Compare = Builder.CreateFCmp(
        static_cast<llvm::CmpInst::Predicate>(Index), Left, Right);
    Builder.CreateRet(Builder.CreateZExt(Compare, Builder.getInt32Ty()));
  }
  compileAndCheck("#include <stdint.h>\n" + emit(Module) + R"(
static unsigned calls;
static double observed[2];
double produce(double value) { observed[calls++] = value; return value; }
int main(void) {
  uint32_t (*functions[])(double, double) = {
    compare0, compare1, compare2, compare3, compare4, compare5, compare6,
    compare7, compare8, compare9, compare10, compare11, compare12,
    compare13, compare14, compare15};
  const double values[] = {-1, 0, 1, __builtin_nan("")};
  for (unsigned a = 0; a < 4; ++a)
    for (unsigned b = 0; b < 4; ++b)
      for (unsigned predicate = 0; predicate < 16; ++predicate) {
        const double left = values[a], right = values[b];
        const int unordered = __builtin_isnan(left) || __builtin_isnan(right);
        const int ordered = !unordered;
        const int expected[] = {
          0, ordered && left == right, ordered && left > right,
          ordered && left >= right, ordered && left < right,
          ordered && left <= right, ordered && left != right, ordered,
          unordered, unordered || left == right, unordered || left > right,
          unordered || left >= right, unordered || left < right,
          unordered || left <= right, unordered || left != right, 1};
        calls = 0;
        if (functions[predicate](left, right) != (unsigned)expected[predicate] ||
            calls != 2) return (int)predicate + 1;
        if (!(observed[0] == left ||
              (__builtin_isnan(observed[0]) && __builtin_isnan(left))) ||
            !(observed[1] == right ||
              (__builtin_isnan(observed[1]) && __builtin_isnan(right)))) return 17;
      }
  return 0;
}
)");
}

TEST(LLVMCIntrinsicSemantics, X87SaturatingConversionsGuardUndefinedCasts) {
  llvm::LLVMContext Context;
  llvm::Module Module("x87-saturating-conversions", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> Builder(Context);
  auto *Float = llvm::Type::getX86_FP80Ty(Context);
  for (unsigned Bits : {32u, 64u}) {
    auto *Integer = Builder.getIntNTy(Bits);
    auto *Function =
        llvm::Function::Create(llvm::FunctionType::get(Integer, {Float}, false),
                               llvm::GlobalValue::ExternalLinkage,
                               "saturate" + std::to_string(Bits), Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::fptosi_sat, {Integer, Float});
    Builder.CreateRet(Builder.CreateCall(Intrinsic, {Function->getArg(0)}));
  }
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck("#include <stdint.h>\n" + emit(Module) + R"(
int main(void) {
  const long double nan = __builtin_nanl(""), infinity = __builtin_infl();
  if (saturate32(nan) || saturate64(nan)) return 1;
  if (saturate32(infinity) != INT32_MAX ||
      saturate32(-infinity) != UINT32_C(0x80000000) ||
      saturate64(infinity) != INT64_MAX ||
      saturate64(-infinity) != UINT64_C(0x8000000000000000)) return 2;
  if (saturate32(0x1p31L) != INT32_MAX ||
      saturate64(0x1p63L) != INT64_MAX ||
      saturate32(-0x1p31L) != UINT32_C(0x80000000) ||
      saturate64(-0x1p63L) != UINT64_C(0x8000000000000000)) return 3;
  if (saturate32(17.9L) != 17 || saturate32(-17.9L) != (uint32_t)-17 ||
      saturate64(17.9L) != 17 || saturate64(-17.9L) != (uint64_t)-17) return 4;
  return 0;
}
)");
#else
  GTEST_SKIP() << "native x87 C execution requires Linux x86-64";
#endif
}

TEST(LLVMCIntrinsicSemantics, VolatileI80PointerCopyKeepsTenByteAccesses) {
  llvm::LLVMContext C;
  llvm::Module M("volatile-i80-pointer-copy", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getPtrTy()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "copy_volatile_i80", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto Arg = Fn->arg_begin();
  auto *Bits = B.CreateLoad(B.getIntNTy(80), &*Arg++, "bits");
  Bits->setVolatile(true);
  B.CreateStore(Bits, &*Arg)->setVolatile(true);
  B.CreateRetVoid();

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("((const volatile uint8_t*)"), std::string::npos) << Text;
  EXPECT_NE(Text.find("((volatile uint8_t*)"), std::string::npos) << Text;
  EXPECT_NE(Text.find("x87_byte"), std::string::npos) << Text;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck("#include <stdint.h>\n" + Text +
                  "int main(void) {\n"
                  "  uint8_t source[16] = {1,2,3,4,5,6,7,8,9,10,0xee};\n"
                  "  uint8_t destination[16] = {0};\n"
                  "  destination[10] = 0x77;\n"
                  "  copy_volatile_i80(source, destination);\n"
                  "  for (unsigned i = 0; i < 10; ++i)\n"
                  "    if (destination[i] != source[i]) return 1;\n"
                  "  return source[10] != 0xee || destination[10] != 0x77;\n"
                  "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, X87OperandsShareOverlappingImageByteArray) {
  llvm::LLVMContext C;
  llvm::Module M("x87-overlapping-image-data", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *Bytes = llvm::ArrayType::get(B.getInt8Ty(), 32);
  auto *ImageData = new llvm::GlobalVariable(
      M, Bytes, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantAggregateZero::get(Bytes), "__nd_data_402000.data");
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getPtrTy()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "copy_x87_operands", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto At = [&](uint64_t Offset) {
    return B.CreateGEP(B.getInt8Ty(), ImageData, B.getInt64(Offset));
  };
  B.CreateStore(B.getInt64(0x8000000000000001ULL), At(0))->setVolatile(true);
  B.CreateStore(B.getInt16(0x7ffe), At(8))->setVolatile(true);
  B.CreateStore(B.getInt64(0x8000000000000003ULL), At(10))->setVolatile(true);
  B.CreateStore(B.getInt16(0xffbe), At(18))->setVolatile(true);
  auto *Large = B.CreateLoad(B.getIntNTy(80), At(0), "large_bits");
  Large->setVolatile(true);
  auto *Small = B.CreateLoad(B.getIntNTy(80), At(10), "small_bits");
  Small->setVolatile(true);
  auto Arg = Fn->arg_begin();
  B.CreateStore(Large, &*Arg++)->setVolatile(true);
  B.CreateStore(Small, &*Arg)->setVolatile(true);
  B.CreateStore(Small, At(20))->setVolatile(true);
  B.CreateStore(B.getInt16(0x0400), At(30))->setVolatile(true);
  B.CreateRetVoid();

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("uint8_t unk_402000[32] = {0};"), std::string::npos)
      << Text;
  EXPECT_EQ(Text.find("_402008"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("_402012"), std::string::npos) << Text;
  EXPECT_NE(Text.find("unk_402000 + 10"), std::string::npos) << Text;
  EXPECT_NE(Text.find("unk_402000 + 30"), std::string::npos) << Text;
  std::string OnlyText;
  llvm::raw_string_ostream OnlyOS(OnlyText);
  CEmitterOptions OnlyOptions;
  OnlyOptions.TheArch = Arch::X64;
  OnlyOptions.EmitIncludes = false;
  EXPECT_TRUE(
      LLVMCEmitter().emit(M, OnlyOS, OnlyOptions, nullptr, nullptr, Fn));
  EXPECT_NE(OnlyText.find("uint8_t unk_402000[32] = {0};"), std::string::npos)
      << OnlyText;
  EXPECT_EQ(OnlyText.find("_402008"), std::string::npos) << OnlyText;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck(
      "#include <stdint.h>\n" + Text +
      "int main(void) {\n"
      "  unsigned char large[10] = {0}, small[10] = {0};\n"
      "  const unsigned char want_large[10] = "
      "{1,0,0,0,0,0,0,0x80,0xfe,0x7f};\n"
      "  const unsigned char want_small[10] = "
      "{3,0,0,0,0,0,0,0x80,0xbe,0xff};\n"
      "  copy_x87_operands(large, small);\n"
      "  for (unsigned i = 0; i < 10; ++i) {\n"
      "    if (large[i] != want_large[i] || "
      "unk_402000[i] != want_large[i]) return 1;\n"
      "    if (small[i] != want_small[i] || "
      "unk_402000[10 + i] != want_small[i] || "
      "unk_402000[20 + i] != want_small[i]) return 2;\n"
      "  }\n"
      "  return unk_402000[30] == 0 && unk_402000[31] == 4 ? 0 : 3;\n"
      "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, AssumeEvaluatesItsConditionAndRefusesBundles) {
  for (bool Bundle : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module M("assume-condition", Context);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> B(Context);
    auto *Pointer = B.getPtrTy();
    auto *Predicate = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt1Ty(), {B.getInt32Ty(), Pointer},
                                false),
        llvm::GlobalValue::ExternalLinkage, "predicate", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Predicate));
    auto *Count = B.CreateLoad(B.getInt32Ty(), Predicate->getArg(1));
    B.CreateStore(B.CreateAdd(Count, B.getInt32(1)), Predicate->getArg(1));
    B.CreateRet(B.CreateICmpULT(Predicate->getArg(0), B.getInt32(16)));
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), {B.getInt32Ty(), Pointer},
                                false),
        llvm::GlobalValue::ExternalLinkage, "assuming", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
    auto *Condition = B.CreateCall(Predicate, {F->getArg(0), F->getArg(1)});
    if (Bundle)
      B.CreateAssumption({llvm::OperandBundleDef("nonnull", F->getArg(1))});
    else
      B.CreateAssumption(Condition);
    B.CreateRet(B.CreateAdd(F->getArg(0), B.getInt32(9)));
    if (Bundle) {
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      EXPECT_THROW(LLVMCEmitter().emit(M, OS, {}), std::runtime_error);
      continue;
    }
    const auto Text = emit(M);
    EXPECT_NE(Text.find("__builtin_unreachable()"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("llvm_x2E_assume"), std::string::npos) << Text;
    const auto Program =
        "#include <stdint.h>\n#include <stdbool.h>\n" + Text + R"(
int main(void) {
 uint32_t calls=0;
 for (uint32_t x=0;x<16;++x)
   if (assuming(x,&calls)!=x+9 || calls!=x+1) return 1;
 return 0;
}
)";
    for (const char *Level : {"-O0", "-O2"})
      compileAndCheck(Program, false, {}, Level, true);
  }
}

TEST(LLVMCIntrinsicSemantics, VolatileSegmentLoadsKeepDefinitionsAndReadOrder) {
  for (bool GS : {false, true}) {
    for (unsigned Bits : {8u, 16u, 32u, 64u}) {
      SCOPED_TRACE(GS);
      SCOPED_TRACE(Bits);
      llvm::LLVMContext C;
      llvm::Module M("volatile-segment-loads", C);
      M.setDataLayout("e-p:64:64");
      llvm::IRBuilder<llvm::NoFolder> B(C);
      const unsigned AS = GS ? kLLVMX86GSAddressSpace : kLLVMX86FSAddressSpace;
      auto *Fn = llvm::Function::Create(
          llvm::FunctionType::get(B.getInt64Ty(), {B.getInt64Ty()}, false),
          llvm::GlobalValue::ExternalLinkage, "read_segment_twice", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
      auto *Pointer = B.CreateIntToPtr(Fn->getArg(0), B.getPtrTy(AS));
      auto *First = B.CreateLoad(B.getIntNTy(Bits), Pointer, "first");
      First->setVolatile(true);
      auto *Second = B.CreateLoad(B.getIntNTy(Bits), Pointer, "second");
      Second->setVolatile(true);
      B.CreateRet(
          B.CreateXor(B.CreateZExtOrTrunc(First, B.getInt64Ty()),
                      B.CreateShl(B.CreateZExtOrTrunc(Second, B.getInt64Ty()),
                                  B.getInt64(1))));
      auto *Unused = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), {B.getInt64Ty()}, false),
          llvm::GlobalValue::ExternalLinkage, "read_segment_unused", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Unused));
      auto *UnusedLoad =
          B.CreateLoad(B.getIntNTy(Bits),
                       B.CreateIntToPtr(Unused->getArg(0), B.getPtrTy(AS)));
      UnusedLoad->setVolatile(true);
      B.CreateRetVoid();
      ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
      const auto Text = emit(M);
      const char *Suffix = Bits == 8    ? "byte"
                           : Bits == 16 ? "word"
                           : Bits == 32 ? "dword"
                                        : "qword";
      const std::string Intrinsic =
          std::string("__read") + (GS ? "gs" : "fs") + Suffix;
      EXPECT_NE(Text.find(Intrinsic + "("), std::string::npos) << Text;
      // A declared segment environment permits execution on every host without
      // reading the host's thread or kernel structures. Each read produces a
      // distinct value, records its address and advances an observable counter.
      const std::string Program =
          "#include <stdint.h>\nstatic unsigned reads;\n"
          "static uint64_t addresses[3];\nstatic uint64_t observe(uint64_t p) "
          "{\n"
          "  if (reads < 3) addresses[reads] = p; return (++reads == 1 ? "
          "UINT64_C(0xfedcba9876543210) : UINT64_C(0x80000000abcdef71));\n}\n"
          "#define " +
          Intrinsic + "(p) ((uint" + std::to_string(Bits) + "_t)observe(p))\n" +
          Text + R"(
int main(void) {
  uint64_t first = (uintBITS_t)UINT64_C(0xfedcba9876543210);
  uint64_t second = (uintBITS_t)UINT64_C(0x80000000abcdef71);
  if (read_segment_twice(0x188) != (first ^ (second << 1)) ||
      reads != 2 || addresses[0] != 0x188 || addresses[1] != 0x188) return 1;
  read_segment_unused(0x1a0);
  return reads != 3 || addresses[2] != 0x1a0;
}
)";
      std::string Executable = Program;
      for (size_t At = 0;
           (At = Executable.find("BITS", At)) != std::string::npos;)
        Executable.replace(At, 4, std::to_string(Bits));
      for (const char *Level : {"-O0", "-O2"})
        compileAndCheck(Executable, false, {}, Level);
    }
  }
}

TEST(LLVMCIntrinsicSemantics, AtomicSegmentLoadsFailClearly) {
  for (unsigned AS : {kLLVMX86GSAddressSpace, kLLVMX86FSAddressSpace}) {
    SCOPED_TRACE(AS);
    llvm::LLVMContext C;
    llvm::Module M("atomic-segment-load", C);
    llvm::IRBuilder<> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy(AS)}, false),
        llvm::GlobalValue::ExternalLinkage, "read_atomic_segment", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    auto *Load = B.CreateLoad(B.getInt64Ty(), Fn->getArg(0));
    Load->setAtomic(llvm::AtomicOrdering::Acquire);
    Load->setAlignment(llvm::Align(8));
    B.CreateRet(Load);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    EXPECT_THROW(LLVMCEmitter().emit(M, OS, Options), std::runtime_error);
  }
}

TEST(LLVMCIntrinsicSemantics, VolatileSegmentPointerLoadKeepsItsPointerType) {
  for (bool GS : {false, true}) {
    SCOPED_TRACE(GS);
    llvm::LLVMContext C;
    llvm::Module M("volatile-segment-pointer", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getPtrTy(), {B.getInt64Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, "read_segment_pointer", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    const unsigned AS = GS ? kLLVMX86GSAddressSpace : kLLVMX86FSAddressSpace;
    auto *Load = B.CreateLoad(B.getPtrTy(),
                              B.CreateIntToPtr(Fn->getArg(0), B.getPtrTy(AS)));
    Load->setVolatile(true);
    B.CreateRet(Load);
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.EmitIncludes = false;
    Options.PreserveLLVMFunctionTypes = true;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    ASSERT_TRUE(LLVMCEmitter().emit(M, OS, Options));
    const auto Program = std::string(R"(
#include <stdint.h>
static uint64_t cell, address;
static unsigned reads;
static uint64_t observe(uint64_t offset) {
  ++reads; address = offset; return (uintptr_t)&cell;
}
)") + "#define __read" + (GS ? "gs" : "fs") +
                         "qword(p) observe(p)\n" + Text + R"(
int main(void) {
  return read_segment_pointer(0x188) != &cell || reads != 1 || address != 0x188;
}
)";
    for (const char *Level : {"-O0", "-O2"})
      compileAndCheck(Program, false, {}, Level);
  }
}

TEST(LLVMCIntrinsicSemantics, OrdinarySegmentLoadKeepsItsExistingProjection) {
  for (bool GS : {false, true}) {
    SCOPED_TRACE(GS);
    llvm::LLVMContext C;
    llvm::Module M("ordinary-segment-load", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<llvm::NoFolder> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt64Ty(), {B.getInt64Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, "read_ordinary_segment", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    const unsigned AS = GS ? kLLVMX86GSAddressSpace : kLLVMX86FSAddressSpace;
    auto *Load = B.CreateLoad(B.getInt64Ty(),
                              B.CreateIntToPtr(Fn->getArg(0), B.getPtrTy(AS)));
    B.CreateRet(B.CreateXor(Load, B.CreateShl(Load, B.getInt64(1))));
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    const auto Text = emit(M);
    const auto Program = std::string("#include <stdint.h>\n#define __read") +
                         (GS ? "gs" : "fs") +
                         "qword(p) UINT64_C(0xfedcba9876543210)\n" + Text + R"(
int main(void) {
  const uint64_t value = UINT64_C(0xfedcba9876543210);
  return read_ordinary_segment(0x188) != (value ^ (value << 1));
}
)";
    for (const char *Level : {"-O0", "-O2"})
      compileAndCheck(Program, false, {}, Level);
  }
}

llvm::CallInst *cpuQuery(llvm::IRBuilder<> &B, bool XCR, llvm::Value *Leaf,
                         llvm::Value *Subleaf = nullptr) {
  auto *I32 = B.getInt32Ty();
  auto *Result = llvm::StructType::get(
      B.getContext(), XCR ? llvm::ArrayRef<llvm::Type *>({I32, I32})
                          : llvm::ArrayRef<llvm::Type *>({I32, I32, I32, I32}));
  auto *Type =
      llvm::FunctionType::get(Result,
                              XCR ? llvm::ArrayRef<llvm::Type *>({I32})
                                  : llvm::ArrayRef<llvm::Type *>({I32, I32}),
                              false);
  auto *Asm = llvm::InlineAsm::get(
      Type, XCR ? "xgetbv" : "cpuid",
      XCR ? "={eax},={edx},{ecx},~{memory}"
          : "={eax},={ebx},={ecx},={edx},{eax},{ecx},~{memory}",
      true);
  return XCR ? B.CreateCall(Asm, {Leaf}) : B.CreateCall(Asm, {Leaf, Subleaf});
}

TEST(LLVMCIntrinsicSemantics, ParameterNamesAreStableBeforeNarrowViews) {
  class Names : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      FunctionSym F;
      F.Addr = Address;
      F.Name = "cached_parameters";
      F.ReturnType = NdType::makeInt(8);
      F.Params = {{"input-r8", NdType::makeInt(8)},
                  {"input_r8", NdType::makeInt(8)},
                  {"cpuInfo", NdType::makeInt(8)}};
      return F;
    }
    bool hasInfo() const override { return true; }
  } Debug;
  for (bool UseDebug : {false, true}) {
    for (bool PreserveTypes : {false, true}) {
      SCOPED_TRACE(UseDebug);
      SCOPED_TRACE(PreserveTypes);
      llvm::LLVMContext C;
      llvm::Module M("cached-parameters", C);
      M.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(C);
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(
              B.getInt64Ty(), {B.getInt64Ty(), B.getInt64Ty(), B.getInt64Ty()},
              false),
          llvm::GlobalValue::ExternalLinkage, "cached_parameters", M);
      F->getArg(0)->setName("input-r8");
      F->getArg(1)->setName("input_r8");
      F->getArg(2)->setName("cpuInfo");
      rewrite_source::setOriginalVA(*F, 0x1000);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
      auto Consume =
          M.getOrInsertFunction("consume_byte", B.getInt64Ty(), B.getInt8Ty());
      llvm::Value *Result = B.getInt64(0);
      for (auto &Arg : F->args())
        Result = B.CreateXor(
            Result,
            B.CreateCall(Consume, {B.CreateTrunc(&Arg, B.getInt8Ty())}));
      B.CreateRet(Result);
      ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      Options.PreserveLLVMFunctionTypes = PreserveTypes;
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      ASSERT_TRUE(
          LLVMCEmitter().emit(M, OS, Options, UseDebug ? &Debug : nullptr));
      const std::string Program = Text + R"(
uint64_t consume_byte(uint8_t x) { return (uint64_t)x * 1234567u; }
int main(void) {
  const uint64_t values[] = {0, 1, 255, 256, UINT64_MAX,
                            UINT64_C(0xfedcba98765432a1)};
  for (unsigned i = 0; i < 6; ++i)
    for (unsigned j = 0; j < 6; ++j) {
      uint64_t a = values[i], b = values[j], c = ~a;
      uint64_t expected = consume_byte((uint8_t)a) ^ consume_byte((uint8_t)b) ^
                          consume_byte((uint8_t)c);
      if (cached_parameters(a, b, c) != expected) return 1;
    }
  return 0;
}
)";
      for (const char *Level : {"-O0", "-O2"})
        compileAndCheck(Program, false, {}, Level);
    }
  }
}

TEST(LLVMCIntrinsicSemantics, CachedDebugParameterFieldsKeepSignatureNames) {
  class FieldNames : public NullDebugContext {
  public:
    TypeRef Record = NdType::makeStruct({NdType::makeInt(8)});
    FieldNames() {
      Record->SourceName = "ParameterRecord";
      Record->FieldDisplayNames = {"value"};
      Record->FieldDisplayOffsets = {0};
      Record->FieldDisplayTypes = {NdType::makeInt(8)};
    }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      FunctionSym F;
      F.Addr = Address;
      F.Name = "read_parameter_field";
      F.ReturnType = NdType::makeInt(8);
      F.Params = {{"record-input", NdType::makePtr(Record)},
                  {"record_input", NdType::makeInt(8)}};
      return F;
    }
    bool hasInfo() const override { return true; }
  } Debug;
  llvm::LLVMContext C;
  llvm::Module M("cached-parameter-field", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy(), B.getInt64Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "read_parameter_field", M);
  rewrite_source::setOriginalVA(*F, 0x1000);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
  B.CreateRet(
      B.CreateXor(B.CreateLoad(B.getInt64Ty(), F->getArg(0)), F->getArg(1)));
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  ASSERT_TRUE(LLVMCEmitter().emit(M, OS, Options, &Debug));
  const std::string Main = R"(
int main(void) {
  ParameterRecord values[] = {{0}, {1}, {UINT64_MAX}, {UINT64_C(0xfedcba9876543210)}};
  for (unsigned i = 0; i < 4; ++i)
    for (unsigned j = 0; j < 4; ++j)
      if (read_parameter_field(&values[i], values[j].value) !=
          (values[i].value ^ values[j].value)) return 1;
  return 0;
}
)";
  for (const char *Level : {"-O0", "-O2"})
    compileAndCheck("#include <stdint.h>\n"
                    "typedef struct { uint64_t value; } ParameterRecord;\n" +
                        Text + Main,
                    false, {}, Level);
}

void checkCPUArrays(bool XCR) {
  {
    SCOPED_TRACE(XCR);
    llvm::LLVMContext C;
    llvm::Module M("cpu-query-arrays", C);
    M.setDataLayout("e-p:64:64");
    M.setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
    llvm::IRBuilder<> B(C);
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(
            B.getVoidTy(),
            {B.getInt32Ty(), B.getInt32Ty(), B.getPtrTy(), B.getInt32Ty()},
            false),
        llvm::GlobalValue::ExternalLinkage, "query_cpu", M);
    F->getArg(3)->setName(XCR ? "xcr" : "cpuInfo");
    auto *Entry = llvm::BasicBlock::Create(C, "entry", F);
    auto *First = llvm::BasicBlock::Create(C, "first", F);
    auto *Second = llvm::BasicBlock::Create(C, "second", F);
    auto *Join = llvm::BasicBlock::Create(C, "join", F);
    B.SetInsertPoint(Entry);
    auto *Initial = cpuQuery(B, XCR, F->getArg(0), F->getArg(1));
    B.CreateCondBr(B.CreateICmpEQ(F->getArg(3), B.getInt32(0)), First, Second);
    B.SetInsertPoint(First);
    auto *A =
        B.CreateExtractValue(cpuQuery(B, XCR, F->getArg(0), F->getArg(1)), 0);
    B.CreateBr(Join);
    B.SetInsertPoint(Second);
    auto *D = B.CreateExtractValue(cpuQuery(B, XCR, F->getArg(0), F->getArg(1)),
                                   XCR ? 1 : 3);
    B.CreateBr(Join);
    B.SetInsertPoint(Join);
    auto *Selected = B.CreatePHI(B.getInt32Ty(), 2);
    Selected->addIncoming(A, First);
    Selected->addIncoming(D, Second);
    const unsigned Fields = XCR ? 2 : 4;
    for (unsigned I = 0; I < Fields; ++I)
      B.CreateStore(B.CreateExtractValue(Initial, I),
                    B.CreateGEP(B.getInt32Ty(), F->getArg(2), B.getInt32(I)));
    B.CreateStore(Selected, B.CreateGEP(B.getInt32Ty(), F->getArg(2),
                                        B.getInt32(Fields)));
    B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.PreserveLLVMFunctionTypes = true;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    ASSERT_TRUE(LLVMCEmitter().emit(M, OS, Options));
    const std::string Main = XCR ? R"(
int main(void) {
  unsigned a,b,c,d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
  if (!(c & (1u << 27))) return 2; /* The host gate requires OSXSAVE. */
  __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
  for (unsigned mode = 0; mode < 2; ++mode) {
    uint32_t actual[3] = {0};
    query_cpu(0, 0, actual, mode);
    if (actual[0] != a || actual[1] != d || actual[2] != (mode ? d : a)) return 1;
  }
  return 0;
}
)"
                                 : R"(
int main(void) {
  const unsigned queries[][2] = {{0,0}, {7,0}, {7,1}, {0x80000000,0}};
  for (unsigned i = 0; i < 4; ++i) {
    unsigned a,b,c,d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(queries[i][0]), "c"(queries[i][1]));
    for (unsigned mode = 0; mode < 2; ++mode) {
      uint32_t actual[5] = {0};
      query_cpu(queries[i][0], queries[i][1], actual, mode);
      if (actual[0] != a || actual[1] != b || actual[2] != c ||
          actual[3] != d || actual[4] != (mode ? d : a)) return 1;
    }
  }
  return 0;
}
)";
    for (const char *Level : {"-O0", "-O2"})
      compileAndCheck(Text + Main, false, {}, Level);
  }
}

TEST(LLVMCIntrinsicSemantics, CPUArraysKeepDistinctNamesAcrossBlocks) {
  if (llvm::Triple(llvm::sys::getDefaultTargetTriple()).getArch() !=
      llvm::Triple::x86_64)
    GTEST_SKIP() << "Native CPU queries require an x64 host";
  checkCPUArrays(false);
}

TEST(LLVMCIntrinsicSemantics, XGETBVArraysKeepDistinctNamesAcrossBlocks) {
  if (llvm::Triple(llvm::sys::getDefaultTargetTriple()).getArch() !=
      llvm::Triple::x86_64)
    GTEST_SKIP() << "Native CPU queries require an x64 host";
  const auto Features = llvm::sys::getHostCPUFeatures();
  if (!Features.lookup("xsave"))
    GTEST_SKIP() << "Native XGETBV requires host OSXSAVE support";
  checkCPUArrays(true);
}

TEST(LLVMCIntrinsicSemantics, CPUQueriesRejectChangedAssemblyContracts) {
  for (bool XCR : {false, true})
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      SCOPED_TRACE(XCR);
      SCOPED_TRACE(Mutation);
      llvm::LLVMContext C;
      llvm::Module M("changed-cpu-query", C);
      M.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(C);
      const unsigned Words = XCR ? 2 : 4;
      llvm::SmallVector<llvm::Type *, 4> Fields(
          Mutation == 0 ? Words - 1 : Words,
          Mutation == 1 ? B.getInt64Ty() : B.getInt32Ty());
      auto *Result = llvm::StructType::get(C, Fields, Mutation == 2);
      llvm::SmallVector<llvm::Type *, 2> Inputs(
          XCR ? 1 : 2, Mutation == 3 ? B.getInt64Ty() : B.getInt32Ty());
      const std::string Mnemonic = XCR ? "xgetbv" : "cpuid";
      std::string Constraints =
          XCR ? "={eax},={edx},{ecx},~{memory}"
              : "={eax},={ebx},={ecx},={edx},{eax},{ecx},~{memory}";
      if (Mutation == 4)
        Constraints += ",~{cc}";
      auto *IA = llvm::InlineAsm::get(
          llvm::FunctionType::get(Result, Inputs, false),
          Mutation == 5 ? Mnemonic + "\n\tnop" : Mnemonic, Constraints,
          Mutation != 6, Mutation == 7,
          Mutation == 8 ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT);
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), Inputs, false),
          llvm::GlobalValue::ExternalLinkage, "query_cpu", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
      llvm::SmallVector<llvm::Value *, 2> Args;
      for (auto &Arg : F->args())
        Args.push_back(&Arg);
      B.CreateCall(IA, Args);
      B.CreateRetVoid();
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      EXPECT_THROW(LLVMCEmitter().emit(M, OS, Options), std::runtime_error);
    }
}

TEST(LLVMCIntrinsicSemantics,
     CPUQueryAggregateABIIsRefusedBeforeArrayProjection) {
  for (bool XCR : {false, true}) {
    llvm::LLVMContext C;
    llvm::Module M("cpu-aggregate-return", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> B(C);
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), {}, false),
        llvm::GlobalValue::ExternalLinkage, "query_cpu", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
    auto *Query = cpuQuery(B, XCR, B.getInt32(0), B.getInt32(0));
    auto Consume = M.getOrInsertFunction("consume_cpu_aggregate", B.getVoidTy(),
                                         Query->getType());
    B.CreateCall(Consume, {Query});
    B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.PreserveLLVMFunctionTypes = true;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    EXPECT_THROW(LLVMCEmitter().emit(M, OS, Options), std::runtime_error);
  }
}

TEST(LLVMCIntrinsicSemantics,
     HiddenResultAndThisNamesStayBoundToPhysicalArguments) {
  class ResultNames : public NullDebugContext {
  public:
    bool Member = false;
    TypeRef Record =
        NdType::makeStruct({NdType::makeInt(8), NdType::makeInt(8)});
    ResultNames() { Record->SourceName = "ResultRecord"; }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      FunctionSym F;
      F.Name = "write_result";
      F.Addr = Address;
      F.ReturnType = Record;
      if (Member)
        F.Params.emplace_back("this", NdType::makePtr(NdType::makeInt(8)));
      F.Params.emplace_back("result", NdType::makeInt(8));
      return F;
    }
    bool hasInfo() const override { return true; }
  } Debug;
  for (bool Member : {false, true})
    for (bool Preserve : {false, true}) {
      SCOPED_TRACE(Member);
      SCOPED_TRACE(Preserve);
      Debug.Member = Member;
      llvm::LLVMContext C;
      llvm::Module M("hidden-result-names", C);
      M.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(C);
      llvm::SmallVector<llvm::Type *, 3> Inputs;
      if (Member)
        Inputs.push_back(B.getPtrTy());
      Inputs.append({B.getPtrTy(), B.getInt64Ty()});
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(B.getPtrTy(), Inputs, false),
          llvm::GlobalValue::ExternalLinkage, "write_result", M);
      rewrite_source::setOriginalVA(*F, 0x1000);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
      auto *Output = F->getArg(Member ? 1 : 0);
      auto *Value = F->getArg(Member ? 2 : 1);
      B.CreateStore(Value, Output);
      B.CreateStore(B.CreateNot(Value),
                    B.CreateGEP(B.getInt64Ty(), Output, B.getInt64(1)));
      B.CreateRet(Output);
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      Options.Format = BinaryFormat::COFF;
      Options.PreserveLLVMFunctionTypes = Preserve;
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      ASSERT_TRUE(LLVMCEmitter().emit(M, OS, Options, &Debug));
      std::string Main = R"(
int main(void) {
  const uint64_t values[] = {0, 1, UINT64_MAX, UINT64_C(0xfedcba9876543210)};
  for (unsigned i = 0; i < 4; ++i) {
    ResultRecord actual = {0, 0};
)";
      Main += "    if (write_result(" + std::string(Member ? "0, " : "") +
              "&actual, values[i]) != &actual || actual.first != values[i] || "
              "actual.second != ~values[i]) return 1;\n  }\n return 0;\n}\n";
      for (const char *Level : {"-O0", "-O2"})
        compileAndCheck(
            "#include <stdint.h>\n"
            "typedef struct { uint64_t first, second; } ResultRecord;\n" +
                Text + Main,
            false, {}, Level);
    }
}

TEST(LLVMCIntrinsicSemantics, DeadCPUIDRetainsNativeQueryAndBreakpointOrder) {
  const llvm::Triple Host(llvm::sys::getDefaultTargetTriple());
  if (!Host.isOSWindows() || Host.getArch() != llvm::Triple::x86_64)
    GTEST_SKIP()
        << "This native event observer uses Windows x64 single stepping";
  llvm::LLVMContext C;
  llvm::Module M("ordered-cpu-query", C);
  M.setDataLayout("e-p:64:64");
  M.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::IRBuilder<> B(C);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt32Ty(), {}, false),
      llvm::GlobalValue::ExternalLinkage, "query_with_breakpoint", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
  cpuQuery(B, false, B.getInt32(0), B.getInt32(0));
  auto *Break =
      llvm::InlineAsm::get(llvm::FunctionType::get(B.getVoidTy(), {}, false),
                           "int3", "~{memory}", true);
  B.CreateCall(Break);
  B.CreateRet(B.CreateExtractValue(
      cpuQuery(B, false, B.getInt32(7), B.getInt32(1)), 0));
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  ASSERT_TRUE(LLVMCEmitter().emit(M, OS, Options));
  const std::string Main = R"(
#include <windows.h>
static volatile unsigned queries, breaks, queries_at_break, bad, steps, active;
static unsigned leaves[8], subleaves[8];
static LONG CALLBACK observe(EXCEPTION_POINTERS *info) {
  CONTEXT *c = info->ContextRecord;
  DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_BREAKPOINT) {
    ++breaks; queries_at_break = queries;
    if (*(const unsigned char *)(uintptr_t)c->Rip == 0xcc) ++c->Rip;
  } else if (code != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  if (!active) { c->EFlags &= ~256u; return EXCEPTION_CONTINUE_EXECUTION; }
  /* Windows clears TF when delivering an exception. Rearm each continuation. */
  if (++steps > 100000) { bad = 1; c->EFlags &= ~256u; }
  else c->EFlags |= 256u;
  const unsigned char *pc = (const unsigned char *)(uintptr_t)c->Rip;
  if (pc[0] == 0x0f && pc[1] == 0xa2) {
    if (queries < 8) { leaves[queries] = (unsigned)c->Rax;
                       subleaves[queries] = (unsigned)c->Rcx; }
    ++queries;
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}
__attribute__((noinline)) static unsigned trace(unsigned (*fn)(void)) {
  active = 1;
  __asm__ volatile("pushfq; orq $256,(%%rsp); popfq" ::: "memory", "cc");
  unsigned result = fn();
  active = 0;
  __asm__ volatile("pushfq; andq $-257,(%%rsp); popfq" ::: "memory", "cc");
  return result;
}
int main(void) {
  /* push rbx; CPUID(0,0); INT3; CPUID(7,1); pop rbx; ret */
  const unsigned char bytes[] = {0x53,0x31,0xc0,0x31,0xc9,0x0f,0xa2,0xcc,
    0xb8,7,0,0,0,0xb9,1,0,0,0,0x0f,0xa2,0x5b,0xc3};
  void *native = VirtualAlloc(0, sizeof(bytes), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!native) return 10;
  __builtin_memcpy(native, bytes, sizeof(bytes));
  FlushInstructionCache(GetCurrentProcess(), native, sizeof(bytes));
  void *handler = AddVectoredExceptionHandler(1, observe);
  if (!handler) return 11;
  unsigned reference = trace((unsigned (*)(void))native);
  if (bad || queries != 2 || breaks != 1 || queries_at_break != 1 ||
      leaves[0] != 0 || subleaves[0] != 0 || leaves[1] != 7 || subleaves[1] != 1) return 1;
  queries = breaks = queries_at_break = bad = steps = 0;
  unsigned actual = trace(query_with_breakpoint);
  RemoveVectoredExceptionHandler(handler);
  VirtualFree(native, 0, MEM_RELEASE);
  if (bad || queries != 2 || breaks != 1 || queries_at_break != 1 ||
      leaves[0] != 0 || subleaves[0] != 0 || leaves[1] != 7 || subleaves[1] != 1) return 2;
  return actual != reference;
}
)";
  for (const char *Level : {"-O0", "-O2"})
    compileAndCheck(Text + Main, false, {}, Level);
}

} // namespace
