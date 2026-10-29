//===- X86_64_X87ConstantTests.cpp - Exact x87 constant checks ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare original constant-load encodings with all native output routes.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/backend/llvm/X86/LLVMX87ConstantAsm.h"
#include "neverd/ir/high/X86/X87ConstantShape.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Intrinsics.h"

#include <cstdlib>

namespace {
LowOp constantOp(unsigned Selector, uint16_t Control) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(2, 10);
  Op.addInput(NdVar::cst(unsigned(Intrinsic::X87LoadConstant), 2));
  Op.addInput(NdVar::cst(Selector, 1));
  Op.addInput(NdVar::cst(Control, 2));
  return Op;
}
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
template <unsigned Selector>
std::array<uint8_t, 10> originalConstant(uint16_t Control) {
  std::array<uint8_t, 10> Value{};
  uint16_t Saved;
  __asm__ volatile("fnstcw %0" : "=m"(Saved));
  __asm__ volatile("fldcw %1\n\t.byte 0xd9,%c2\n\tfstpt %0\n\tfldcw %3"
                   : "=m"(Value)
                   : "m"(Control), "i"(0xe8 + Selector), "m"(Saved)
                   : "st", "memory");
  return Value;
}
constexpr auto OriginalConstants =
    std::array{originalConstant<0>, originalConstant<1>, originalConstant<2>,
               originalConstant<3>, originalConstant<4>, originalConstant<5>,
               originalConstant<6>};
#endif
TEST(X87ConstantConcrete, AllDefinedControlFieldsMatchOriginalInstructions) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Selector = 0; Selector < 7; ++Selector)
    for (unsigned RC = 0; RC < 4; ++RC)
      for (unsigned PC : {0U, 2U, 3U})
        for (unsigned Masks = 0; Masks < 64; ++Masks) {
          const uint16_t Control = 0x40 | (RC << 10) | (PC << 8) | Masks;
          const auto Expected = OriginalConstants[Selector](Control);
          for (bool Strict : {false, true}) {
            SCOPED_TRACE(testing::Message()
                         << Selector << ':' << Control << ':' << Strict);
            NdOpEmulator E(Image);
            E.setStrictMode(Strict);
            E.setRegister(x86reg::FPU_CW, Control);
            // Masked sticky precision must remain a sticky input, not be
            // confused with a new inexact exception from loading the ROM.
            const uint16_t Status = Masks & 32 ? 32 : 0;
            E.setRegister(x86reg::FPU_SW, Status);
            ASSERT_TRUE(E.step(constantOp(Selector, Control)));
            ASSERT_TRUE(E.getRegisterBytes(2));
            EXPECT_EQ(*E.getRegisterBytes(2),
                      std::vector<uint8_t>(Expected.begin(), Expected.end()));
            EXPECT_EQ(E.getRegister(x86reg::FPU_CW), Control);
            EXPECT_EQ(E.getRegister(x86reg::FPU_SW), Status);
          }
        }
#else
  GTEST_SKIP() << "native x64 GCC/Clang oracle required";
#endif
}
TEST(X87ConstantConcrete, WaitingBoundaryAndUnknownControlRefuseAtomically) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (bool Strict : {false, true}) {
    NdOpEmulator E(Image);
    E.setStrictMode(Strict);
    E.setRegister(x86reg::FPU_CW, 0x37e);
    E.setRegister(x86reg::FPU_SW, 0x81);
    E.setRegisterBytes(2, std::vector<uint8_t>(10, 0xa5));
    EXPECT_FALSE(E.step(constantOp(3, 0x37e)));
    EXPECT_EQ(*E.getRegisterBytes(2), std::vector<uint8_t>(10, 0xa5));
    EXPECT_EQ(E.getRegister(x86reg::FPU_SW), 0x81);
    LowOp Read;
    Read.Opcode = NdOp::INTRINSIC;
    Read.Output = NdVar::tmp(3, 2);
    Read.addInput(NdVar::cst(unsigned(Intrinsic::X87ReadControl), 2));
    ASSERT_TRUE(E.step(Read)); // FNSTCW does not wait.
    EXPECT_EQ(E.getRegister(3), 0x37e);
    E.setCallPreservedRegisters({});
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(Entry, 8));
    ASSERT_TRUE(E.step(Call));
    EXPECT_FALSE(E.step(Read));
    EXPECT_FALSE(E.step(constantOp(3, 0x37f)));
  }
}
TEST(X87ConstantContract, ShapesRejectMalformedInputsAndInventedResults) {
  const auto Id = Intrinsic::X87LoadConstant;
  for (Arch A : {Arch::X86, Arch::X64}) {
    auto Op = constantOp(3, 0x37f);
    ASSERT_TRUE(
        x87ConstantControlShapeIsValid(Id, x87ConstantControlLowShape(Op, A)));
    for (unsigned Change = 0; Change < 8; ++Change) {
      auto Bad = Op;
      switch (Change) {
      case 0:
        Bad.Inputs[1].Offset = 7;
        break;
      case 1:
        Bad.Inputs[1].Size = 2;
        break;
      case 2:
        Bad.Inputs[2].Size = 4;
        break;
      case 3:
        Bad.Output.Size = 8;
        break;
      case 4:
        Bad.NumInputs = 4;
        break;
      case 5:
        Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
        break;
      case 6:
        Bad.Output.Space = VnodeSpace::CONST;
        break;
      case 7:
        Bad.Inputs[0].Size = 1;
        break;
      }
      EXPECT_FALSE(x87ConstantControlShapeIsValid(
          Id, x87ConstantControlLowShape(Bad, A)))
          << Change;
      BinaryImage Image;
      Image.Arch = A;
      NdOpEmulator E(Image);
      E.setRegisterBytes(2, std::vector<uint8_t>(10, 0x5a));
      EXPECT_FALSE(E.step(Bad));
      EXPECT_EQ(*E.getRegisterBytes(2), std::vector<uint8_t>(10, 0x5a));
    }
    MedOp M;
    M.Opcode = NdOp::INTRINSIC;
    M.Output = MedVar{.Kind = MedVar::Temp, .TheArch = A, .Id = 1, .Size = 10};
    M.addInput(MedVar::makeConst(unsigned(Id), 2));
    M.addInput(MedVar::makeConst(3, 1));
    M.addInput(MedVar::makeConst(0x37f, 2));
    ASSERT_TRUE(
        x87ConstantControlShapeIsValid(Id, x87ConstantControlMedShape(M, A)));
    M.IntrinsicOutputs.push_back(M.Output);
    EXPECT_FALSE(
        x87ConstantControlShapeIsValid(Id, x87ConstantControlMedShape(M, A)));
    auto H = HighExpr::makeCall(
        "neverd_x87_load_constant", 0,
        {HighExpr::makeConst(3, 1), HighExpr::makeConst(0x37f, 2)});
    H->IntrinsicId = Id;
    H->Type = NdType::makeInt(10, false);
    ASSERT_TRUE(
        x87ConstantControlShapeIsValid(Id, x87ConstantControlHighShape(*H, A)));
    H->IntrinsicOutputs.push_back(M.Output);
    EXPECT_FALSE(
        x87ConstantControlShapeIsValid(Id, x87ConstantControlHighShape(*H, A)));
  }
}
TEST(X87ConstantContract, ControlTransfersHaveOneExactWordAndNoHiddenResults) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  NdOpEmulator E(Image);
  LowOp Write;
  Write.Opcode = NdOp::INTRINSIC;
  Write.addInput(NdVar::cst(unsigned(Intrinsic::X87WriteControl), 2));
  Write.addInput(NdVar::cst(0xb7f, 2));
  ASSERT_TRUE(E.step(Write));
  EXPECT_EQ(E.getRegister(x86reg::FPU_CW), 0xb7f);
  LowOp Read;
  Read.Opcode = NdOp::INTRINSIC;
  Read.Output = NdVar::tmp(3, 2);
  Read.addInput(NdVar::cst(unsigned(Intrinsic::X87ReadControl), 2));
  ASSERT_TRUE(E.step(Read));
  EXPECT_EQ(E.getRegister(3), 0xb7f);
  for (unsigned Change = 0; Change < 4; ++Change) {
    auto Bad = Write;
    if (Change == 0)
      Bad.Output = NdVar::tmp(4, 2);
    if (Change == 1)
      Bad.Inputs[1].Size = 4;
    if (Change == 2)
      Bad.Inputs[1].Offset = 0x17f; // reserved PC
    if (Change == 3)
      Bad.Inputs[1].Offset = 0x3bf; // reserved bit
    EXPECT_FALSE(E.step(Bad));
    EXPECT_EQ(E.getRegister(x86reg::FPU_CW), 0xb7f);
  }
  E.setRegister(x86reg::FPU_SW, 0x81);
  EXPECT_FALSE(E.step(Write));
  EXPECT_EQ(E.getRegister(x86reg::FPU_CW), 0xb7f);
}
TEST(X87ConstantContract, OwnedAssemblyRetainsExactSelectorTypesAndClobbers) {
  llvm::LLVMContext C;
  llvm::Module M("x87", C);
  auto *Ptr = llvm::PointerType::get(C, 0);
  for (unsigned Selector = 0; Selector < 9; ++Selector) {
    std::vector<llvm::Type *> Types(Selector < 7 ? 2 : 1, Ptr);
    auto *Ty = llvm::FunctionType::get(llvm::Type::getVoidTy(C), Types, false);
    auto *F = llvm::Function::Create(Ty, llvm::GlobalValue::ExternalLinkage,
                                     "probe", M);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
    llvm::SmallVector<llvm::Value *, 2> Args;
    for (auto &Arg : F->args())
      Args.push_back(&Arg);
    const std::string Text = Selector < 7    ? x87ConstantAsm(Selector)
                             : Selector == 7 ? "fnstcw ($0)"
                                             : "fldcw ($0)";
    const auto Constraints =
        Selector < 7 ? X87ConstantConstraints : X87ControlConstraints;
    auto *Asm = llvm::InlineAsm::get(Ty, Text, Constraints, true);
    auto *Call = B.CreateCall(Asm, Args);
    Call->setMetadata(X87ConstantAsmMetadata, llvm::MDNode::get(C, {}));
    EXPECT_EQ(classifyX87ConstantAsm(*Call), Selector);
    // Keep LLVM's operand count valid so this reaches our clobber check even
    // when the linked LLVM was built with assertions enabled.
    auto *Bad =
        llvm::InlineAsm::get(Ty, Text, Selector < 7 ? "r,r" : "r", true);
    Call->setCalledOperand(Bad);
    EXPECT_DEATH_IF_SUPPORTED(classifyX87ConstantAsm(*Call),
                              "invalid owned x87");
    Call->setCalledOperand(Asm);
    B.CreateRetVoid();
  }
}
class X87ConstantFixture : public X86FPStateFixture {
protected:
  std::string comparisonDriver(bool, bool DeclareProbe, bool,
                               bool = false) override {
    std::string Text = R"(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern void native_probe(uintptr_t);
)";
    if (DeclareProbe)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += R"(
int main(void) {
  uint16_t saved;
  __asm__ volatile("fnstcw %0" : "=m"(saved));
  unsigned comparisons = 0;
  for (unsigned rc = 0; rc < 4; ++rc)
    for (unsigned pc = 0; pc < 4; ++pc) {
      if (pc == 1) continue; /* reserved precision mode */
      for (unsigned masks = 0; masks < 64; ++masks) {
        const uint16_t control = 0x0040 | (pc << 8) | (rc << 10) | masks;
        unsigned char expected[64], actual[64];
        memset(expected, 0xa5, sizeof(expected));
        memcpy(expected, &control, 2);
        memcpy(expected + 2, &saved, 2);
        memcpy(actual, expected, sizeof(actual));
        const uint16_t ambient = control ^ 0x0c00;
        __asm__ volatile("fninit\n\tfldcw %0" :: "m"(ambient) : "memory");
        native_probe((uintptr_t)expected);
        __asm__ volatile("fninit\n\tfldcw %0" :: "m"(ambient) : "memory");
        isa_probe((uintptr_t)actual);
        __asm__ volatile("fninit\n\tfldcw %0" :: "m"(saved) : "memory");
        if (memcmp(actual, expected, sizeof(actual))) {
          printf("control=%04x expected=", control);
          for (unsigned i = 16; i < 26; ++i) printf("%02x", expected[i]);
          printf(" actual=");
          for (unsigned i = 16; i < 26; ++i) printf("%02x", actual[i]);
          puts("");
          return 1;
        }
        ++comparisons;
      }
    }
  printf("%u exact binary80 and control-word comparisons\n", comparisons);
  return 0;
}
)";
    return Text;
  }
  void check(unsigned Opcode, bool InternalControl) {
#if defined(_WIN32)
    const uint8_t Base = 1; // RCX
#else
    const uint8_t Base = 7; // RDI
#endif
    std::vector<uint8_t> Bytes;
    if (InternalControl)
      Bytes = {0xd9, uint8_t(0x28 | Base)}; // fldcw word [arg]
    Bytes.insert(Bytes.end(),
                 {0xd9, uint8_t(Opcode), 0xdb, uint8_t(0x78 | Base),
                  16,                             // fstp tbyte [arg+16]
                  0xd9, uint8_t(0x78 | Base), 32, // fnstcw [arg+32]
                  0xd9, uint8_t(0x68 | Base), 2,  // fldcw [arg+2]
                  0xc3});
    compareFloating(Bytes, false);
  }
};
class X87Constants : public X87ConstantFixture,
                     public testing::WithParamInterface<unsigned> {};
TEST_P(X87Constants, IncomingControlWord) { check(GetParam(), false); }
TEST_P(X87Constants, LoadedControlWord) { check(GetParam(), true); }
INSTANTIATE_TEST_SUITE_P(AllSeven, X87Constants,
                         testing::Values(0xe8U, 0xe9U, 0xeaU, 0xebU, 0xecU,
                                         0xedU, 0xeeU),
                         ([](const auto &Info) {
                           static const char *Names[] = {
                               "One",      "Log2Ten", "Log2E", "Pi",
                               "Log10Two", "LnTwo",   "Zero"};
                           return Names[Info.param - 0xe8];
                         }));
class X87ControlProjectionFixture : public X86FPStateFixture {
protected:
  void checkNativeCalls(llvm::StringRef SourceCompiler) {
#if defined(__x86_64__) || defined(_M_X64)
    llvm::LLVMContext Context;
    llvm::Module Module("control-access", Context);
    Module.setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
    auto *Ptr = llvm::PointerType::get(Context, 0);
    auto *Type =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {Ptr}, false);
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "control_probe", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    const auto Store = Module.getOrInsertFunction("llvm.x86.fnstcw", Type);
    const auto Load = Module.getOrInsertFunction("llvm.x86.fldcw", Type);
    const auto Pointer = [&](unsigned Offset) {
      return Builder.CreateGEP(Builder.getInt8Ty(), Function->getArg(0),
                               Builder.getInt64(Offset));
    };
    Builder.CreateCall(Store, {Pointer(1)});
    Builder.CreateCall(Load, {Pointer(3)});
    Builder.CreateCall(Store, {Pointer(5)});
    Builder.CreateCall(Load, {Pointer(1)});
    Builder.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = hostFormat();
    Options.PreserveLLVMFunctionTypes = true;
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
    EXPECT_NE(Source.find("__builtin_ia32_fnstcw"), std::string::npos);
    EXPECT_NE(Source.find("__builtin_ia32_fldcw"), std::string::npos);
    const auto Unit = file("control-projection.c");
    write(Unit, Source + R"(
#include <string.h>
int main(void) {
  unsigned short saved;
  __asm__ volatile("fnstcw %0" : "=m"(saved));
  for (unsigned pc = 0; pc < 4; ++pc) {
    if (pc == 1) continue;
    for (unsigned rc = 0; rc < 4; ++rc) {
      unsigned char bytes[16], expected[16];
      memset(bytes, 0xa5, sizeof(bytes));
      const unsigned short input = 0x7f | pc << 8 | rc << 10;
      memcpy(bytes + 3, &input, 2);
      memcpy(expected, bytes, sizeof(bytes));
      memcpy(expected + 1, &saved, 2);
      memcpy(expected + 5, &input, 2);
      control_probe(bytes);
      unsigned short after;
      __asm__ volatile("fnstcw %0" : "=m"(after));
      if (memcmp(bytes, expected, sizeof(bytes)) || after != saved) return 1;
    }
  }
  return 0;
}
)");
    retainFile("control-projection.c", Unit);
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Object = file(std::string("control") + Optimization + ".o");
      const auto Binary = file(std::string("control") + Optimization + ".exe");
      auto Result = command(SourceCompiler,
                            {Optimization, "-fno-lto", "-fno-builtin-mimalloc",
                             "-c", Unit, "-o", Object});
      ASSERT_EQ(Result.Status, 0) << Result.Error << Source;
      Result = command(Compiler, {Object, "-o", Binary});
      ASSERT_EQ(Result.Status, 0) << Result.Error;
      Result = command(Binary, {});
      EXPECT_EQ(Result.Status, 0) << Result.Out << Result.Error;
    }
#else
    GTEST_SKIP() << "native x64 control-word execution required";
#endif
  }
};

TEST_F(X87ControlProjectionFixture, NativeMemoryCallsExecuteThroughClang) {
  checkNativeCalls(Compiler);
}

TEST_F(X87ControlProjectionFixture, NativeMemoryCallsExecuteThroughNeverC) {
  const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
  if (!NeverC || !*NeverC)
    GTEST_SKIP()
        << "NEVERD_TEST_NEVERC is required for the native builtin branch";
  checkNativeCalls(NeverC);
}

TEST_F(X87ControlProjectionFixture, SourceLLVMUsesRegisteredControlIntrinsics) {
  X87ContextOverride = false;
#if defined(_WIN32)
  const uint8_t Base = 1;
#else
  const uint8_t Base = 7;
#endif
  const std::vector<uint8_t> Bytes = {0xd9,
                                      uint8_t(0x38 | Base),
                                      0xd9,
                                      uint8_t(0x68 | Base),
                                      2,
                                      0xd9,
                                      uint8_t(0x78 | Base),
                                      4,
                                      0xd9,
                                      uint8_t(0x28 | Base),
                                      0xc3};
  for (bool NoOpt : {false, true}) {
    auto Image = image(Bytes);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.LiftMode = true;
    Options.SourceProjection = true;
    Options.NoOpt = NoOpt;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry};
    const auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    unsigned Stores = 0, Loads = 0;
    for (const auto &Function : *Result.LlvmModule)
      for (const auto &Block : Function)
        for (const auto &Instruction : Block)
          if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction)) {
            if (const auto *Callee = Call->getCalledFunction()) {
              Stores += Callee->getName() == "llvm.x86.fnstcw";
              Loads += Callee->getName() == "llvm.x86.fldcw";
            } else if (auto Selector = classifyX87ConstantAsm(*Call)) {
              Stores += *Selector == 7;
              Loads += *Selector == 8;
              if (*Selector >= 7)
                EXPECT_EQ(
                    llvm::Intrinsic::lookupIntrinsicID(
                        *Selector == 7 ? "llvm.x86.fnstcw" : "llvm.x86.fldcw"),
                    llvm::Intrinsic::not_intrinsic);
            }
          }
    EXPECT_GT(Stores, 0U);
    EXPECT_GT(Loads, 0U);
  }
}
} // namespace
