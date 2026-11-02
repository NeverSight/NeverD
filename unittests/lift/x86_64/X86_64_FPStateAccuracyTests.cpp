//===- X86_64_FPStateAccuracyTests.cpp - CPU accuracy regressions ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/X86/LLVMX86FPStateAsm.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdlib>
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
class X86FPStateAccuracy : public X86FPStateFixture {
protected:
  static std::vector<uint8_t> scalarKernel(bool IsDouble, uint8_t Opcode,
                                           bool KeepResult,
                                           bool Repeat = false) {
    const uint8_t Arg = memoryModRM();
    // MOVDQU XMM0,[arg]; MOVDQU XMM1,[arg+16]; OPSS/SD XMM0,XMM1.
    std::vector<uint8_t> Bytes = {
        0xf3, 0x0f,
        0x6f, Arg,
        0xf3, 0x0f,
        0x6f, static_cast<uint8_t>(0x48 | Arg),
        16,   static_cast<uint8_t>(IsDouble ? 0xf2 : 0xf3),
        0x0f, Opcode,
        0xc1};
    if (Repeat)
      Bytes.insert(Bytes.end(), {static_cast<uint8_t>(IsDouble ? 0xf2 : 0xf3),
                                 0x0f, Opcode, 0xc1});
    if (KeepResult)
      Bytes.insert(Bytes.end(),
                   {0xf3, 0x0f, 0x7f, static_cast<uint8_t>(0x40 | Arg), 48});
    Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
    return Bytes;
  }

  void compareFloatingReturn(bool IsDouble) {
    if (!nativeX64())
      GTEST_SKIP() << "native x86_64 host required";
    const std::vector<uint8_t> Bytes = {
        static_cast<uint8_t>(IsDouble ? 0xf2 : 0xf3), 0x0f, 0x58, 0xc0, 0xc3};
    const auto Native = file("return-native.s");
    const auto NativeObject = file("return-native.o");
    auto Assembly = assembly(Bytes);
    for (size_t Position = 0;
         (Position = Assembly.find("isa_probe", Position)) !=
         std::string::npos;)
      Assembly.replace(Position, 9, "native_probe");
    write(Native, Assembly);
    auto Built = command(Compiler, {"-c", Native, "-o", NativeObject});
    ASSERT_EQ(Built.Status, 0) << Built.Error;
    const std::string Scalar = IsDouble ? "double" : "float";
    const auto Common = floatingDriver(IsDouble, false);
    const auto Values = Common.find("static const");
    const auto Main = Common.find("int main(void)");
    ASSERT_NE(Values, std::string::npos);
    ASSERT_NE(Main, std::string::npos);
    std::string Driver = "\n#include <stdint.h>\n#include <string.h>\n"
                         "#include <immintrin.h>\nextern " +
                         Scalar + " native_probe(" + Scalar + ");\n" +
                         Common.substr(Values, Main - Values) +
                         "\nint main(void) {\nuint32_t saved = _mm_getcsr();\n";
    Driver += R"(
for (unsigned rounding = 0; rounding < 4; ++rounding)
  for (unsigned environment = 0; environment < 4; ++environment)
    for (unsigned sticky = 0; sticky < 2; ++sticky)
      for (unsigned a = 0; a < sizeof(values)/sizeof(values[0]); ++a) {
        uint32_t state = 0x1f80 | (rounding << 13) |
          ((environment & 1) ? 0x40 : 0) | ((environment & 2) ? 0x8000 : 0) |
          (sticky ? 0x25 : 0);
)";
    Driver += Scalar +
              " input; __builtin_memcpy(&input, &values[a], sizeof(input));\n"
              "_mm_setcsr(state);\n" +
              Scalar +
              " expected = native_probe(input);\n"
              "uint32_t expected_state = _mm_getcsr();\n"
              "_mm_setcsr(state);\n" +
              Scalar +
              " actual = isa_probe(input);\n"
              "uint32_t actual_state = _mm_getcsr();\n_mm_setcsr(saved);\n"
              "if (expected_state != actual_state || memcmp(&expected, "
              "&actual, sizeof(actual))) return 1;\n"
              "}\nreturn 0;\n}\n";
    for (bool NoOpt : {false, true})
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM ? "LLVMC return" : "HighC return");
        SCOPED_TRACE(NoOpt);
        llvm::LLVMContext Context;
        auto Image = image(Bytes);
        PipelineOptions Options;
        Options.LiftMode = LLVM;
        Options.SourceProjection = LLVM;
        Options.NoOpt = NoOpt;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Entry};
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        CEmitterOptions Emission;
        Emission.TheArch = Arch::X64;
        Emission.Format = hostFormat();
        Emission.PreserveLLVMFunctionTypes = true;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVM) {
          ASSERT_NE(Result.LlvmModule, nullptr);
          ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, Emission,
                                          nullptr, &Image));
        } else {
          ASSERT_EQ(Result.HighFuncs.size(), 1U);
          ASSERT_NE(Result.HighFuncs[0].ReturnType, nullptr);
          ASSERT_EQ(Result.HighFuncs[0].ReturnType->Kind, NdTypeKind::Float);
          ASSERT_EQ(Result.HighFuncs[0].ReturnType->Size, IsDouble ? 8 : 4);
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emission));
        }
        const auto C = file("return-driver.c");
        std::string TypedDriver = Driver;
        if (LLVM) {
          const auto *Function = Result.LlvmModule->getFunction("isa_probe");
          ASSERT_NE(Function, nullptr);
          ASSERT_EQ(Function->arg_size(), 1U);
          // PreserveLLVMFunctionTypes exposes the complete XMM carrier. Feed
          // and observe its low scalar bytes through that actual C signature.
          if (Function->getArg(0)->getType()->isVectorTy()) {
            ASSERT_TRUE(Function->getReturnType()->isVectorTy());
            const std::string Call = Scalar + " actual = isa_probe(input);";
            const auto Position = TypedDriver.find(Call);
            ASSERT_NE(Position, std::string::npos);
            TypedDriver.replace(
                Position, Call.size(),
                "uint64_t __attribute__((vector_size(16))) incoming = {0,0};\n"
                "__builtin_memcpy(&incoming, &input, sizeof(input));\n"
                "uint64_t __attribute__((vector_size(16))) returned = "
                "isa_probe(incoming);\n" +
                    Scalar +
                    " actual; __builtin_memcpy(&actual, &returned, "
                    "sizeof(actual));");
          }
        }
        write(C, Source + TypedDriver);
        for (const char *Optimization : {"-O0", "-O2"}) {
          const auto Executable = file("return.exe");
          Built = command(Compiler,
                          {Optimization, NativeObject, C, "-o", Executable});
          ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
          const auto Actual = command(Executable, {});
          EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error << Source;
        }
      }
  }
};

TEST_F(X86FPStateAccuracy, FloatingReturnBitsAndMxcsrSurviveSourceProjection) {
  compareFloatingReturn(false);
  compareFloatingReturn(true);
}

TEST_F(X86FPStateAccuracy, HighCTypedStatePreservesBitsAndHelperNames) {
  if (!nativeX64())
    GTEST_SKIP() << "native x86_64 host required";
  const auto State = HighExpr::makeVar(
      MedVar{.Kind = MedVar::Param, .Id = 0, .Size = 4}, NdType::makeFloat(4));
  auto Sum = HighExpr::makeCall(
      "", 0,
      {HighExpr::makeConst(0x3f800000, 4), HighExpr::makeConst(0, 4), State});
  Sum->IntrinsicId = Intrinsic::X86FPAddState;
  Sum->Type = NdType::makeInt(8, false);
  HighFunc Function;
  Function.Name = "typed_sum";
  Function.ReturnType = Sum->Type;
  Function.Params = {{"state", State->Type}};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = Sum;
  Function.Body.push_back(Return);
  std::vector<HighFunc> Functions{Function};

  auto Write = HighExpr::makeCall("", 0, {State});
  Write->IntrinsicId = Intrinsic::X86WriteMXCSR;
  Write->Type = NdType::makeVoid();
  Function.Name = "typed_write";
  Function.ReturnType = NdType::makeVoid();
  Function.Body.clear();
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = Write;
  Function.Body.push_back(Statement);
  Functions.push_back(Function);

  auto Read = HighExpr::makeCall("", 0, {});
  Read->IntrinsicId = Intrinsic::X86ReadMXCSR;
  Read->Type = NdType::makeInt(4, false);
  Function.Name = "raw_read";
  Function.ReturnType = Read->Type;
  Function.Params.clear();
  Return.RetVal = Read;
  Function.Body = {Return};
  Functions.push_back(Function);
  for (const char *Name : {"neverd_x86_fp_add_state_f32",
                           "neverd_x86_read_mxcsr", "neverd_x86_write_mxcsr"}) {
    HighFunc Collision;
    Collision.Name = Name;
    Collision.ReturnType = NdType::makeVoid();
    Functions.push_back(Collision);
  }
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = hostFormat();
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  executeSource(Source, R"(
#include <immintrin.h>
int main(void) {
  uint32_t saved = _mm_getcsr(), state = 0x5f80;
  float carrier;
  __builtin_memcpy(&carrier, &state, 4);
  uint64_t result = typed_sum(carrier);
  uint32_t observed = raw_read();
  state = 0x3f80;
  __builtin_memcpy(&carrier, &state, 4);
  typed_write(carrier);
  uint32_t written = raw_read();
  _mm_setcsr(saved);
  return result != UINT64_C(0x5f803f800000) || observed != 0x5f80 || written != 0x3f80;
}
)");
}

TEST_F(X86FPStateAccuracy, LLVMCUnalignedStateAndHelperNamePreserveBytes) {
  if (!nativeX64())
    GTEST_SKIP() << "native x86_64 host required";
  llvm::LLVMContext Context;
  llvm::Module Module("fp-state-source", Context);
  llvm::IRBuilder<> Builder(Context);
  auto *I32 = Builder.getInt32Ty();
  auto *Float = Builder.getFloatTy();
  auto *Pointer = Builder.getPtrTy();
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I32, {Pointer}, false),
      llvm::Function::ExternalLinkage, "unaligned_sum", Module);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Asm = llvm::InlineAsm::get(
      llvm::FunctionType::get(Float, {Float, Float, Pointer}, false),
      x86FPStateBinaryAsm(Intrinsic::X86FPAddState, 4),
      X86FPStateBinaryConstraints, true);
  auto *Call = Builder.CreateCall(
      Asm, {Builder.CreateBitCast(Builder.getInt32(0x3f800000), Float),
            Builder.CreateBitCast(Builder.getInt32(0x33800000), Float),
            Function->getArg(0)});
  Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(Context, {}));
  Builder.CreateRet(Builder.CreateBitCast(Call, I32));
  auto *Collision = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getVoidTy(), false),
      llvm::Function::ExternalLinkage, "neverd_x86_add_value_f32", Module);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Collision));
  Builder.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = hostFormat();
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
  executeSource(Source, R"(
#include <immintrin.h>
int main(void) {
  uint32_t saved = _mm_getcsr(), state = 0x1f80;
  _Alignas(4) unsigned char memory[8] = {0x59,0,0,0,0,0x73,0x91,0x25};
  __builtin_memcpy(memory + 1, &state, 4);
  uint32_t result = unaligned_sum((void *)(memory + 1));
  __builtin_memcpy(&state, memory + 1, 4);
  _mm_setcsr(saved);
  return result != 0x3f800000 || state != 0x1fa0 ||
      memory[0] != 0x59 || memory[5] != 0x73 || memory[6] != 0x91 || memory[7] != 0x25;
}
)");
}

TEST(X86FPStateContract, TypedNumericalStateResultIsRejected) {
  HighExpr Call;
  Call.Kind = ExprKind::Call;
  Call.IntrinsicId = Intrinsic::X86ReadMXCSR;
  Call.Type = NdType::makeFloat(4);
  bool HasCIntrinsics = false;
  EXPECT_DEATH(renderX86TypedIntrinsicCall(
                   Arch::X64, Call, [](const HighExpr &) { return "unused"; },
                   HasCIntrinsics, true),
               "requires raw bits");
}

TEST(X86FPStateContract, OnlyCompleteNumericalBitsEstablishAFloatReturn) {
  for (unsigned Mode = 0; Mode < 8; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Function;
    MedBlock Block;
    Block.Id = 0;
    const auto Temp = [](int Id, uint16_t Bytes) {
      return MedVar{.Kind = MedVar::Temp, .Id = Id, .Size = Bytes};
    };
    const auto Reg = [](int Id, uint64_t Offset, uint16_t Bytes) {
      MedVar Value{.Kind = MedVar::Reg,
                   .TheArch = Arch::X64,
                   .Id = Id,
                   .SSAVer = 1,
                   .Size = Bytes};
      Value.RegOff = Offset;
      return Value;
    };
    const auto Emit = [&](NdOp Opcode, MedVar Output,
                          std::initializer_list<MedVar> Inputs) {
      MedOp Op;
      Op.Opcode = Opcode;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Block.Ops.push_back(Op);
    };
    const auto Integer = Reg(90, x86reg::RAX, 8);
    Emit(NdOp::COPY, Integer, {MedVar::makeConst(7, 8)});
    Emit(NdOp::INTRINSIC, Temp(1, 12),
         {MedVar::makeConst(static_cast<unsigned>(Intrinsic::X86FPAddState), 2),
          MedVar::makeConst(UINT64_C(0x3ff0000000000000), 8),
          MedVar::makeConst(0, 8), MedVar::makeConst(0x1f80, 4)});
    Emit(NdOp::SUBBYTES, Temp(2, 8), {Temp(1, 12), MedVar::makeConst(0, 4)});
    MedVar Value = Temp(2, 8);
    if (Mode == 1 || Mode == 3) {
      Value = Temp(3, Mode == 1 ? 8 : 4);
      Emit(NdOp::COPY, Value, {Temp(2, 8)});
    } else if (Mode == 2 || Mode == 4) {
      Value = Temp(3, 4);
      Emit(NdOp::SUBBYTES, Value,
           {Mode == 2 ? Temp(2, 8) : Temp(1, 12),
            MedVar::makeConst(Mode == 2 ? 4 : 8, 4)});
    } else if (Mode == 5)
      Value = Temp(1, 12);
    else if (Mode == 6 || Mode == 7) {
      const auto Narrow = Temp(3, 4);
      Emit(Mode == 6 ? NdOp::COPY : NdOp::SUBBYTES, Narrow,
           Mode == 6 ? std::initializer_list<MedVar>{Temp(2, 8)}
                     : std::initializer_list<MedVar>{Temp(2, 8),
                                                     MedVar::makeConst(0, 4)});
      Value = Temp(4, 8);
      Emit(NdOp::COPY, Value, {Narrow});
    }
    Emit(NdOp::CONCAT, Reg(100, x86reg::XMM0, 16),
         {MedVar::makeConst(0, static_cast<uint16_t>(16 - Value.Size)), Value});
    Emit(NdOp::RETURN, {}, {Integer});
    Function.Blocks.push_back(Block);
    inferMedTypes(Function, Arch::X64);
    ASSERT_NE(Function.ReturnType, nullptr);
    EXPECT_EQ(Function.ReturnType->Kind,
              Mode < 2 ? NdTypeKind::Float : NdTypeKind::Int);
    EXPECT_EQ(Function.ReturnType->Size, 8);
  }
}

TEST(X86FPStateContract, AuxiliaryDefinitionsCannotInventStateResults) {
  for (Intrinsic Id : {Intrinsic::X86ReadMXCSR, Intrinsic::X86WriteMXCSR,
                       Intrinsic::X86FPAddState, Intrinsic::X86FPSubState,
                       Intrinsic::X86FPMulState, Intrinsic::X86FPDivState}) {
    SCOPED_TRACE(static_cast<unsigned>(Id));
    MedOp Operation;
    Operation.Opcode = NdOp::INTRINSIC;
    Operation.addInput(MedVar::makeConst(static_cast<unsigned>(Id), 2));
    if (Id != Intrinsic::X86WriteMXCSR)
      Operation.Output = MedVar{
          .Kind = MedVar::Temp,
          .Id = 1,
          .Size = static_cast<uint16_t>(Id == Intrinsic::X86ReadMXCSR ? 4 : 8)};
    if (isX86ScalarFPStateIntrinsic(Id)) {
      Operation.addInput(MedVar::makeConst(0x3f800000, 4));
      Operation.addInput(MedVar::makeConst(0x33800000, 4));
    }
    if (Id != Intrinsic::X86ReadMXCSR)
      Operation.addInput(MedVar::makeConst(0x1f80, 4));
    MedFunc Function;
    MedBlock Block;
    Block.Id = 0;
    Block.Ops.push_back(Operation);
    Function.Blocks.push_back(Block);
    ASSERT_TRUE(verifyMedFunc(Function, "valid scalar state"));
    Function.Blocks[0].Ops[0].IntrinsicOutputs.push_back(
        MedVar{.Kind = MedVar::Temp, .Id = 2, .Size = 4});
    EXPECT_FALSE(x86FPStateShapeIsValid(
        Id, x86FPStateMedShape(Function.Blocks[0].Ops[0])));
    EXPECT_FALSE(verifyMedFunc(Function, "unproduced scalar state result"));
  }
  HighExpr Call;
  Call.Kind = ExprKind::Call;
  Call.IntrinsicId = Intrinsic::X86ReadMXCSR;
  Call.Type = NdType::makeInt(4, false);
  Call.IntrinsicOutputs.push_back(
      MedVar{.Kind = MedVar::Temp, .Id = 2, .Size = 4});
  bool HasCIntrinsics = false;
  EXPECT_DEATH(renderX86TypedIntrinsicCall(
                   Arch::X64, Call, [](const HighExpr &) { return "unused"; },
                   HasCIntrinsics, true),
               "invalid x86 FP state C contract");
  Call.IntrinsicOutputs.clear();
  Call.MemoryOrdering = NdMemoryOrdering::Acquire;
  EXPECT_DEATH(renderX86TypedIntrinsicCall(
                   Arch::X64, Call, [](const HighExpr &) { return "unused"; },
                   HasCIntrinsics, true),
               "invalid x86 FP state C contract");
  Call.MemoryOrdering = NdMemoryOrdering::None;
  Call.IntrinsicId = Intrinsic::X86FPAddState;
  Call.Type = NdType::makeInt(12, false);
  auto NonScalar = HighExpr::makeConst(0, 8);
  NonScalar->Type = NdType::makeArray(NdType::makeInt(1, false), 8, 8);
  Call.Operands = {NonScalar, HighExpr::makeConst(0, 8),
                   HighExpr::makeConst(0x1f80, 4)};
  EXPECT_DEATH(renderX86TypedIntrinsicCall(
                   Arch::X64, Call, [](const HighExpr &) { return "unused"; },
                   HasCIntrinsics, true),
               "invalid x86 FP state C contract");
}

TEST_F(X86FPStateAccuracy, ScalarResultsAndMxcsrMatchNativeInstruction) {
  for (bool IsDouble : {false, true})
    for (uint8_t Opcode : {0x58, 0x5c, 0x59, 0x5e}) {
      SCOPED_TRACE(IsDouble ? "f64" : "f32");
      SCOPED_TRACE(static_cast<unsigned>(Opcode));
      compareFloating(scalarKernel(IsDouble, Opcode, true), IsDouble);
    }
}

TEST_F(X86FPStateAccuracy, DeadNumericalResultsStillUpdateMxcsr) {
  for (bool IsDouble : {false, true})
    for (uint8_t Opcode : {0x58, 0x5c, 0x59, 0x5e}) {
      SCOPED_TRACE(IsDouble ? "f64" : "f32");
      SCOPED_TRACE(static_cast<unsigned>(Opcode));
      compareFloating(scalarKernel(IsDouble, Opcode, false), IsDouble);
    }
}

TEST_F(X86FPStateAccuracy,
       ConsecutiveOperationsPreserveStickyFlagsAndRounding) {
  for (bool IsDouble : {false, true})
    compareFloating(scalarKernel(IsDouble, 0x58, true, true), IsDouble);
}

TEST_F(X86FPStateAccuracy,
       NativeUnmaskedExceptionPreventsResultAndStateCommit) {
  for (bool IsDouble : {false, true})
    compareFloating(scalarKernel(IsDouble, 0x5e, true), IsDouble, true);
}

TEST_F(X86FPStateAccuracy, BranchesAndLoopsPreserveNumericalAndStateJoins) {
  for (bool IsDouble : {false, true}) {
    const uint8_t Arg = memoryModRM();
    const uint8_t Prefix = IsDouble ? 0xf2 : 0xf3;
    const auto Base = scalarKernel(IsDouble, 0x58, true);
    std::vector<uint8_t> Branch(Base.begin(), Base.begin() + 9);
    // Select add/sub from a varying input bit, then join both result and state.
    Branch.insert(Branch.end(), {0xf6, Arg, 1, 0x74, 6, Prefix, 0x0f, 0x58,
                                 0xc1, 0xeb, 4, Prefix, 0x0f, 0x5c, 0xc1});
    Branch.insert(Branch.end(), Base.begin() + 13, Base.end());
    compareFloating(Branch, IsDouble);
    std::vector<uint8_t> Loop(Base.begin(), Base.begin() + 9);
    // Three iterations require the value/state backedge, not an entry-only
    // seed.
    Loop.insert(Loop.end(), {0xb8, 3, 0, 0, 0, Prefix, 0x0f, 0x58, 0xc1, 0xff,
                             0xc8, 0x75, 0xf8});
    Loop.insert(Loop.end(), Base.begin() + 13, Base.end());
    compareFloating(Loop, IsDouble);
  }
}

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
template <typename Scalar>
std::pair<uint64_t, uint32_t> nativeScalarFP(Intrinsic Id, uint64_t A,
                                             uint64_t B, uint32_t State) {
  Scalar Left, Right;
  std::memcpy(&Left, &A, sizeof(Left));
  std::memcpy(&Right, &B, sizeof(Right));
  const uint32_t Saved = _mm_getcsr();
  switch (Id) {
#define RUN_FP(ID, SS, SD)                                                     \
  case Intrinsic::ID:                                                          \
    if constexpr (sizeof(Scalar) == 4)                                         \
      __asm__ volatile("ldmxcsr %1\n\t" SS " %2,%0\n\tstmxcsr %1"              \
                       : "+x"(Left), "+m"(State)                               \
                       : "x"(Right)                                            \
                       : "memory");                                            \
    else                                                                       \
      __asm__ volatile("ldmxcsr %1\n\t" SD " %2,%0\n\tstmxcsr %1"              \
                       : "+x"(Left), "+m"(State)                               \
                       : "x"(Right)                                            \
                       : "memory");                                            \
    break;
    RUN_FP(X86FPAddState, "addss", "addsd")
    RUN_FP(X86FPSubState, "subss", "subsd")
    RUN_FP(X86FPMulState, "mulss", "mulsd")
    RUN_FP(X86FPDivState, "divss", "divsd")
#undef RUN_FP
  default:
    std::abort();
  }
  uint64_t Result = 0;
  std::memcpy(&Result, &Left, sizeof(Left));
  _mm_setcsr(Saved);
  return {Result, State};
}
#endif

TEST(X86FPStateContract, ConcreteEvaluatorMatchesNativeScalarStateMatrix) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  const std::vector<uint64_t> Singles = {
      0,          0x80000000, 0x3f800000, 0xbf800000, 0x3f800001, 0x3dcccccd,
      1,          0x007fffff, 0x00800000, 0x7f7fffff, 0x7f800000, 0xff800000,
      0x7fc00011, 0x7fc00077, 0x7f800031, 0x7f800071, 0x33800000, 0x40000000};
  const std::vector<uint64_t> Doubles = {0,
                                         UINT64_C(0x8000000000000000),
                                         UINT64_C(0x3ff0000000000000),
                                         UINT64_C(0xbff0000000000000),
                                         UINT64_C(0x3ff0000000000001),
                                         UINT64_C(0x3fb999999999999a),
                                         1,
                                         UINT64_C(0x000fffffffffffff),
                                         UINT64_C(0x0010000000000000),
                                         UINT64_C(0x7fefffffffffffff),
                                         UINT64_C(0x7ff0000000000000),
                                         UINT64_C(0xfff0000000000000),
                                         UINT64_C(0x7ff8000000000011),
                                         UINT64_C(0x7ff8000000000077),
                                         UINT64_C(0x7ff0000000000031),
                                         UINT64_C(0x7ff0000000000071),
                                         UINT64_C(0x3ca0000000000000),
                                         UINT64_C(0x4000000000000000)};
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (bool IsDouble : {false, true}) {
    const unsigned Bytes = IsDouble ? 8 : 4;
    const auto &Values = IsDouble ? Doubles : Singles;
    for (Intrinsic Id : {Intrinsic::X86FPAddState, Intrinsic::X86FPSubState,
                         Intrinsic::X86FPMulState, Intrinsic::X86FPDivState})
      for (unsigned Rounding = 0; Rounding < 4; ++Rounding)
        for (unsigned Environment = 0; Environment < 4; ++Environment)
          for (unsigned Sticky = 0; Sticky < 2; ++Sticky)
            for (uint64_t A : Values)
              for (uint64_t B : Values) {
                const uint32_t State =
                    0x1f80 | (Rounding << 13) | ((Environment & 1) ? 0x40 : 0) |
                    ((Environment & 2) ? 0x8000 : 0) | (Sticky ? 0x25 : 0);
                const auto Expected =
                    IsDouble ? nativeScalarFP<double>(Id, A, B, State)
                             : nativeScalarFP<float>(Id, A, B, State);
                NdOpEmulator Emulator(Image);
                Emulator.setStrictMode(true);
                LowOp Operation;
                Operation.Opcode = NdOp::INTRINSIC;
                Operation.Output = NdVar::tmp(0, Bytes + 4);
                Operation.addInput(NdVar::cst(static_cast<unsigned>(Id), 2));
                Operation.addInput(NdVar::cst(A, Bytes));
                Operation.addInput(NdVar::cst(B, Bytes));
                Operation.addInput(NdVar::cst(State, 4));
                ASSERT_TRUE(Emulator.step(Operation))
                    << "op=" << unsigned(Id) << " bytes=" << Bytes
                    << " state=" << State << " A=" << A << " B=" << B;
                const auto Actual = Emulator.getRegisterBytes(0);
                ASSERT_TRUE(Actual);
                uint64_t Number = 0;
                uint32_t Outgoing = 0;
                std::memcpy(&Number, Actual->data(), Bytes);
                std::memcpy(&Outgoing, Actual->data() + Bytes, 4);
                ASSERT_EQ(Number, Expected.first)
                    << "op=" << unsigned(Id) << " bytes=" << Bytes
                    << " state=" << State << " A=" << A << " B=" << B;
                ASSERT_EQ(Outgoing, Expected.second)
                    << "op=" << unsigned(Id) << " bytes=" << Bytes
                    << " state=" << State << " A=" << A << " B=" << B;
              }
  }
#else
  GTEST_SKIP() << "native scalar SSE oracle requires x64 GCC/Clang";
#endif
}

TEST(X86FPStateContract, UnknownCalleeCannotSupplyInventedMxcsr) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Segment StateMemory;
  StateMemory.VA = 0x2000;
  StateMemory.Size = StateMemory.FileSz = 4;
  StateMemory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  StateMemory.Data.resize(4);
  Image.Segments.push_back(StateMemory);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setCallPreservedRegisters({});
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(0x1000, 8));
  ASSERT_TRUE(Emulator.step(Call));
  LowOp Read;
  Read.Opcode = NdOp::INTRINSIC;
  Read.Output = NdVar::tmp(0, 4);
  Read.addInput(NdVar::cst(static_cast<unsigned>(Intrinsic::X86ReadMXCSR), 2));
  EXPECT_FALSE(Emulator.step(Read));
  EXPECT_FALSE(Emulator.getRegister(0));
  LowOp Store;
  Store.Opcode = NdOp::INTRINSIC;
  Store.addInput(NdVar::cst(static_cast<unsigned>(Intrinsic::Stmxcsr), 2));
  Store.addInput(NdVar::cst(0x2000, 8));
  EXPECT_FALSE(Emulator.step(Store));
  Emulator.setMXCSR(0x7fa1);
  ASSERT_TRUE(Emulator.step(Read));
  EXPECT_EQ(Emulator.getRegister(0), 0x7fa1U);
  ASSERT_TRUE(Emulator.step(Store));
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::tmp(8, 4);
  Load.addInput(NdVar::cst(0x2000, 8));
  ASSERT_TRUE(Emulator.step(Load));
  EXPECT_EQ(Emulator.getRegister(8), 0x7fa1U);
}

TEST(X86FPStateContract, InvalidShapeStopsWithoutPublishingAResult) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  LowOp Operation;
  Operation.Opcode = NdOp::INTRINSIC;
  Operation.Output = NdVar::tmp(0, 8);
  Operation.addInput(
      NdVar::cst(static_cast<unsigned>(Intrinsic::X86FPAddState), 2));
  Operation.addInput(NdVar::cst(0x3f800000, 4));
  Operation.addInput(NdVar::cst(0x33800000, 4));
  Operation.addInput(NdVar::cst(0x1f80, 2));
  EXPECT_FALSE(Emulator.step(Operation));
  EXPECT_FALSE(Emulator.getRegister(0));
  Operation.Inputs[3] = NdVar::cst(0x1f80, 4);
  ASSERT_TRUE(Emulator.step(Operation));
  EXPECT_EQ(Emulator.getRegister(0), UINT64_C(0x1fa03f800000));
  Operation.Inputs[3] = NdVar::cst(0x10000, 4);
  EXPECT_FALSE(Emulator.step(Operation));
}

TEST(X86FPStateContract,
     UnmaskedExceptionStopsBeforeCommittingNumericalOutput) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  LowOp Operation;
  Operation.Opcode = NdOp::INTRINSIC;
  Operation.Output = NdVar::tmp(0, 12);
  Operation.addInput(
      NdVar::cst(static_cast<unsigned>(Intrinsic::X86FPDivState), 2));
  Operation.addInput(NdVar::cst(UINT64_C(0x3ff0000000000000), 8));
  Operation.addInput(NdVar::cst(0, 8));
  Operation.addInput(NdVar::cst(0x1d80, 4));
  EXPECT_FALSE(Emulator.step(Operation));
  EXPECT_FALSE(Emulator.getRegister(0));
  EXPECT_EQ(Emulator.getMXCSR() & 0x3fU, 4U);
}

} // namespace
