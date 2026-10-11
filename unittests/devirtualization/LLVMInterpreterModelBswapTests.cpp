//===- LLVMInterpreterModelBswapTests.cpp - Scalar byte permutations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMInterpreterModelTest.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IRReader/IRReader.h"

namespace neverd::analysis::llvm_model_test {
namespace {
// Capture the original bytes before writing any destination. These direct
// byte copies are independent of the importer's extraction/concatenation tree.
Oracle byteOracle(unsigned Bytes) {
  auto From = [=](unsigned N) { return t(N < Bytes ? Bytes - N - 1 : N, 1); };
  Oracle Reference(
      {op(NdOp::COPY, t(0, 1), {r(0, 1)}), op(NdOp::COPY, t(1, 1), {r(1, 1)}),
       op(NdOp::COPY, t(2, 1), {r(2, 1)}), op(NdOp::COPY, t(3, 1), {r(3, 1)}),
       op(NdOp::COPY, t(4, 1), {r(4, 1)}), op(NdOp::COPY, t(5, 1), {r(5, 1)}),
       op(NdOp::COPY, t(6, 1), {r(6, 1)}), op(NdOp::COPY, t(7, 1), {r(7, 1)}),
       op(NdOp::COPY, r(0, 1), {From(0)}), op(NdOp::COPY, r(1, 1), {From(1)}),
       op(NdOp::COPY, r(2, 1), {From(2)}), op(NdOp::COPY, r(3, 1), {From(3)}),
       op(NdOp::COPY, r(4, 1), {From(4)}), op(NdOp::COPY, r(5, 1), {From(5)}),
       op(NdOp::COPY, r(6, 1), {From(6)}), op(NdOp::COPY, r(7, 1), {From(7)})});
  // The captured bytes live across the independently declared instructions.
  Reference.Function.FunctionTemporaries = {{0, 8}};
  return Reference;
}
std::string declaration(unsigned Bits) {
  const auto T = "i" + std::to_string(Bits);
  return "declare " + T + " @llvm.bswap." + T + "(" + T + ")\n";
}
class LLVMByteSwap : public LLVMModel {
protected:
  void parseSwap(unsigned Bits, llvm::StringRef ReturnAttrs = {}) {
    const auto T = "i" + std::to_string(Bits);
    parse("%x = load " + T + ", ptr %state, align 1\n%v = call " +
              ReturnAttrs.str() + " " + T + " @llvm.bswap." + T + "(" + T +
              " %x)\nstore " + T + " %v, ptr %state, align 1\nret i64 0",
          declaration(Bits));
  }
};
} // namespace

TEST_F(LLVMByteSwap, EveryWidthPreservesTheCompleteState) {
  for (unsigned Bits : {16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    parseSwap(Bits, "noundef");
    ASSERT_TRUE(Module);
    expect(byteOracle(Bits / 8));
    expect(Oracle(), Status::Different);
  }
}

TEST_F(LLVMByteSwap, ConstantsAndDoubleSwapsRetainAllBits) {
  for (unsigned Bits : {16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const auto T = "i" + std::to_string(Bits);
    const auto Bytes = static_cast<uint16_t>(Bits / 8);
    for (uint64_t Word :
         {uint64_t{0}, uint64_t{1}, UINT64_MAX, uint64_t{0x0123456789abcdef}}) {
      Word &= UINT64_MAX >> (64 - Bits);
      uint64_t Reversed = 0, Rest = Word;
      for (unsigned N = 0; N < Bytes; ++N) {
        Reversed = (Reversed << 8) | (Rest & 255);
        Rest >>= 8;
      }
      parse("%v = call " + T + " @llvm.bswap." + T + "(" + T + " " +
                std::to_string(Word) + ")\nstore " + T +
                " %v, ptr %state, align 1\nret i64 0",
            declaration(Bits));
      ASSERT_TRUE(Module);
      expect(Oracle({op(NdOp::COPY, r(0, Bytes), {n(Reversed, Bytes)})}));
    }
    parse("%x = load " + T + ", ptr %state, align 1\n%a = call " + T +
              " @llvm.bswap." + T + "(" + T + " %x)\n%v = call " + T +
              " @llvm.bswap." + T + "(" + T + " %a)\nstore " + T +
              " %v, ptr %state, align 1\nret i64 0",
          declaration(Bits));
    ASSERT_TRUE(Module);
    expect(Oracle());
  }
}

TEST_F(LLVMByteSwap, CrossBlockPhiUsesKeepTheResult) {
  for (unsigned Bits : {16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const auto T = "i" + std::to_string(Bits);
    parse("entry:\n%x = load " + T + ", ptr %state, align 1\n%a = call " + T +
              " @llvm.bswap." + T + "(" + T + " %x)\n%c = icmp eq " + T +
              " %x, 0\nbr i1 %c, label %left, label %right\n"
              "left: br label %join\nright: br label %join\n"
              "join:\n%v = phi " +
              T + " [%a, %left], [%a, %right]\nstore " + T +
              " %v, ptr %state, align 1\nret i64 0",
          declaration(Bits));
    ASSERT_TRUE(Module);
    expect(byteOracle(Bits / 8));
  }
}

TEST_F(LLVMByteSwap, UnusedAndOverwrittenResultsKeepPoisonObligations) {
  for (bool Overwrite : {false, true}) {
    parse(std::string(R"(
      %x = load i16, ptr %state, align 1
      %bad = add nuw i16 %x, 1
      %a = call i16 @llvm.bswap.i16(i16 %bad)
      %b = call i16 @llvm.bswap.i16(i16 %a)
    )") +
              (Overwrite ? "store i16 %b, ptr %state, align 1\n"
                           "store i16 0, ptr %state, align 1\n"
                         : "") +
              "ret i64 0",
          declaration(16));
    ASSERT_TRUE(Module);
    auto C = llvmInterpreterMachineStateContract();
    C.EntryConstants.push_back({r(0, 2), 1});
    auto Reference =
        Overwrite ? Oracle({op(NdOp::COPY, r(0, 2), {n(0, 2)})}) : Oracle();
    expect(Reference, Status::Proved, C);
    C.EntryConstants.back().Value = 65535;
    expect(Reference, Status::ContractViolation, C);
  }
  parse(R"(
    entry: br i1 false, label %bad, label %done
    bad:
      %x = add nuw i16 65535, 1
      %v = call i16 @llvm.bswap.i16(i16 %x)
      br label %done
    done: ret i64 0
  )",
        declaration(16));
  ASSERT_TRUE(Module);
  expect(Oracle());
}

TEST_F(LLVMByteSwap, UnsupportedWidthsAndCallContractsStillRefuse) {
  for (unsigned Bits : {1U, 8U, 24U, 48U, 128U}) {
    SCOPED_TRACE(Bits);
    parseSwap(Bits);
    ASSERT_TRUE(Module);
    reject();
  }
  parse(R"(
    %x = load <2 x i32>, ptr %state, align 1
    %v = call <2 x i32> @llvm.bswap.v2i32(<2 x i32> %x)
    store <2 x i32> %v, ptr %state, align 1
    ret i64 0
  )",
        "declare <2 x i32> @llvm.bswap.v2i32(<2 x i32>)");
  ASSERT_TRUE(Module);
  reject();
  for (const char *Call :
       {"call range(i32 0, 8) i32 @llvm.bswap.i32(i32 %x)",
        "call i32 @llvm.bswap.i32(i32 noundef %x)",
        "call i32 @llvm.bswap.i32(i32 %x) nounwind",
        "call i32 @llvm.bswap.i32(i32 %x) [\"deopt\"(i32 0)]",
        "call fastcc i32 @llvm.bswap.i32(i32 %x)",
        "call i32 @llvm.bswap.i32(i32 poison)",
        "call i32 @llvm.bswap.i32(i32 undef)"}) {
    SCOPED_TRACE(Call);
    parse(std::string("%x = load i32, ptr %state, align 1\n%v = ") + Call +
              "\nstore i32 %v, ptr %state, align 1\nret i64 0",
          declaration(32));
    ASSERT_TRUE(Module);
    reject();
  }
  parseSwap(32);
  ASSERT_TRUE(Module);
  Module->getFunction("llvm.bswap.i32")->addFnAttr(llvm::Attribute::Convergent);
  reject("unsupported intrinsic or call contract");
  parse(R"(
    %x = load i32, ptr %state, align 1
    %v = call i32 @llvm.bswap.i32(i32 %x), !annotation !0
    ret i64 0
  )",
        declaration(32) + "!0 = !{!\"extra\"}\n");
  ASSERT_TRUE(Module);
  reject();
  parse(R"(
    %x = load i64, ptr %state, align 1
    %v = call i32 @llvm.bswap.i32(i64 %x)
    ret i64 0
  )",
        "declare i32 @llvm.bswap.i32(i64)");
  ASSERT_TRUE(Module);
  reject();
  parse("%v = call i32 @external(i32 7)\nret i64 0",
        "declare i32 @external(i32)");
  ASSERT_TRUE(Module);
  reject("unsupported intrinsic or call contract");
}

TEST_F(LLVMByteSwap, EveryAllocationAndOperationConsumesItsBudget) {
  for (unsigned Bits : {16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    parseSwap(Bits);
    ASSERT_TRUE(Module);
    const unsigned Bytes = Bits / 8;
    LLVMInterpreterModelLimits Limits;
    // Independent accounting for this fixed four-instruction fixture:
    // preflight 20, pointer discovery 4, graph 9, outer emission 7;
    // the byte permutation adds (2N-2) allocations and (2N-1) operations.
    Limits.MaxWork = 4 * Bytes + 37;
    Limits.MaxOperations = 2 * Bytes + 2;
    auto Exact = model(Limits);
    ASSERT_TRUE(static_cast<bool>(Exact)) << llvm::toString(Exact.takeError());
    ASSERT_EQ(Exact->Function.Blocks.size(), 1U);
    EXPECT_EQ(Exact->Function.Blocks.front().Ops.size(), Limits.MaxOperations);
    for (bool Work : {false, true}) {
      auto Short = Limits;
      --(Work ? Short.MaxWork : Short.MaxOperations);
      auto Rejected = model(Short);
      ASSERT_FALSE(static_cast<bool>(Rejected));
      EXPECT_NE(llvm::toString(Rejected.takeError()).find("budget"),
                std::string::npos);
    }
  }
}

class LLVMByteSwapCompiled : public NeverDLiftTest {};
TEST_F(LLVMByteSwapCompiled, ClangIntrinsicsMatchIndependentFullStateOracles) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  for (unsigned Bits : {16U, 32U, 64U}) {
    const char *Type = Bits == 16   ? "unsigned short"
                       : Bits == 32 ? "unsigned int"
                                    : "unsigned long long";
    const auto Source = tmpFile("bswap" + std::to_string(Bits) + ".c");
    const auto IR = tmpFile("bswap" + std::to_string(Bits) + ".ll");
    std::ofstream(Source) << "typedef " << Type
                          << " word;\n"
                             "unsigned long long model(unsigned char *s) {\n"
                             "word *p = (word *)s; *p = __builtin_bswap"
                          << Bits << "(*p); return 0; }\n";
    for (const char *Optimization : {"-O1", "-O2"}) {
      SCOPED_TRACE(std::to_string(Bits) + Optimization);
      auto Built =
          exec(NEVERD_TEST_CLANG,
               {"-target", "x86_64-unknown-linux-gnu", "-std=c11", Optimization,
                "-fno-vectorize", "-fno-slp-vectorize", "-S", "-emit-llvm",
                Source.string(), "-o", IR.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      llvm::LLVMContext Context;
      llvm::SMDiagnostic Error;
      auto Input = llvm::parseIRFile(IR.string(), Error, Context);
      ASSERT_TRUE(Input);
      auto *F = Input->getFunction("model");
      ASSERT_NE(F, nullptr);
      unsigned Calls = 0;
      for (const auto &B : *F)
        for (const auto &I : B)
          if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I))
            if (auto *Callee = Call->getCalledFunction())
              Calls += Callee->getIntrinsicID() == llvm::Intrinsic::bswap;
      ASSERT_EQ(Calls, 1U);
      auto M = modelLLVMInterpreterMachineStateX64(*F);
      ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
      const auto Reference = byteOracle(Bits / 8);
      const auto R =
          checkLowIRRefinement(M->Function, M->Instructions, Reference.Function,
                               llvmInterpreterMachineStateContract(),
                               LowIRRefinementWitness::LiftedBits);
      EXPECT_EQ(R.Status, Status::Proved) << R.Diagnostic;
      EXPECT_TRUE(R.Certificate);
    }
  }
}
} // namespace neverd::analysis::llvm_model_test
