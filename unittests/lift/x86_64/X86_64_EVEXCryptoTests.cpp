#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

class MedLLVMEmitterTestPeer {
public:
  static llvm::Value *emitGfni(MedLLVMEmitter &Emitter,
                               llvm::LLVMContext &Context, llvm::Module &Module,
                               const MedOp &Op, Intrinsic Id, llvm::Value *Left,
                               llvm::Value *Right, llvm::IRBuilder<> &Builder) {
    Emitter.Ctx = &Context;
    Emitter.Mod = &Module;
    Emitter.Img = nullptr;
    Emitter.TargetArch = Arch::X64;
    Emitter.TargetFormat = BinaryFormat::ELF;
    Emitter.ParamArgs[Op.Inputs[1].display()] = Left;
    Emitter.ParamArgs[Op.Inputs[2].display()] = Right;
    return Emitter.emitGfniIntrinsic(Op, Id, Builder);
  }

  static llvm::Value *emitIntrinsic(MedLLVMEmitter &Emitter,
                                    llvm::LLVMContext &Context,
                                    llvm::Module &Module, const MedOp &Op,
                                    llvm::Value *Left, llvm::Value *Right,
                                    llvm::IRBuilder<> &Builder) {
    Emitter.Ctx = &Context;
    Emitter.Mod = &Module;
    Emitter.Img = nullptr;
    Emitter.TargetArch = Arch::X64;
    Emitter.TargetFormat = BinaryFormat::ELF;
    Emitter.ParamArgs[Op.Inputs[1].display()] = Left;
    Emitter.ParamArgs[Op.Inputs[2].display()] = Right;
    return Emitter.emitIntrinsic(Op, Builder);
  }
};

} // namespace neverd

using namespace neverd;
namespace {
std::vector<LowOp> lift(std::initializer_list<uint8_t> Raw) {
  std::vector<uint8_t> Bytes(Raw); Decoder D; EXPECT_TRUE(D.init(Arch::X64));
  DecodedInsn I{}; EXPECT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I), (int)Bytes.size());
  std::vector<LowOp> Ops; EXPECT_NO_THROW(D.liftToLow(I, Ops)); return Ops;
}
BinaryImage image(size_t Size) {
  BinaryImage I; I.Arch=Arch::X64; I.Bits=Bitness::Bits64; Segment S;
  S.VA=0x4000; S.Size=Size; S.Flags=SegmentFlags::Readable|SegmentFlags::Writable; S.Data.resize(Size);
  I.Segments.push_back(std::move(S)); return I;
}
void run(NdOpEmulator &E, const std::vector<LowOp> &Ops) { for (const auto &Op:Ops) ASSERT_TRUE(E.step(Op)); }

const LowOp *findIntrinsic(const std::vector<LowOp> &Ops, Intrinsic Id) {
  const auto It = std::find_if(Ops.begin(), Ops.end(), [Id](const LowOp &Op) {
    return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs != 0 &&
           Op.Inputs[0].isConst() &&
           Op.Inputs[0].Offset == static_cast<uint64_t>(Id);
  });
  return It == Ops.end() ? nullptr : &*It;
}

uint8_t gfniMultiply(uint8_t Left, uint8_t Right) {
  uint8_t Result = 0;
  for (unsigned Bit = 0; Bit < 8; ++Bit) {
    if ((Right & 1) != 0)
      Result ^= Left;
    const bool Reduce = (Left & 0x80) != 0;
    Left <<= 1;
    if (Reduce)
      Left ^= 0x1b;
    Right >>= 1;
  }
  return Result;
}

uint8_t gfniInverse(uint8_t Value) {
  uint8_t Result = 1;
  uint8_t Base = Value;
  unsigned Exponent = 254;
  while (Exponent != 0) {
    if ((Exponent & 1) != 0)
      Result = gfniMultiply(Result, Base);
    Base = gfniMultiply(Base, Base);
    Exponent >>= 1;
  }
  return Result;
}

uint8_t parity(uint8_t Value) {
  Value ^= Value >> 4;
  Value ^= Value >> 2;
  Value ^= Value >> 1;
  return Value & 1;
}

std::array<uint8_t, 64> expectedGfni(Intrinsic Id,
                                     const std::array<uint8_t, 64> &Left,
                                     const std::array<uint8_t, 64> &Right,
                                     uint8_t Immediate) {
  std::array<uint8_t, 64> Expected{};
  for (unsigned Byte = 0; Byte < Expected.size(); ++Byte) {
    if (Id == Intrinsic::Gf2p8MulB) {
      Expected[Byte] = gfniMultiply(Left[Byte], Right[Byte]);
      continue;
    }

    const uint8_t Value = Id == Intrinsic::Gf2p8AffineInvQb
                              ? gfniInverse(Left[Byte])
                              : Left[Byte];
    uint8_t Output = Immediate;
    for (unsigned Bit = 0; Bit < 8; ++Bit) {
      const unsigned Row = (Byte & ~7u) + (7u - Bit);
      Output ^= parity(Right[Row] & Value) << Bit;
    }
    Expected[Byte] = Output;
  }
  return Expected;
}

std::array<uint8_t, 64> expectedVdbpsadbw(const std::array<uint8_t, 64> &Left,
                                          const std::array<uint8_t, 64> &Right,
                                          uint16_t Width, uint8_t Immediate) {
  std::array<uint8_t, 64> Permuted{};
  std::array<uint8_t, 64> Expected{};
  for (unsigned Lane = 0; Lane < Width; Lane += 16)
    for (unsigned Dword = 0; Dword < 4; ++Dword) {
      const unsigned Source = Lane + ((Immediate >> (Dword * 2)) & 3) * 4;
      std::copy_n(Right.begin() + Source, 4,
                  Permuted.begin() + Lane + Dword * 4);
    }
  for (unsigned Base = 0; Base < Width; Base += 8)
    for (unsigned Word = 0; Word < 4; ++Word) {
      const unsigned LeftBase = Base + (Word >= 2 ? 4 : 0);
      const unsigned RightBase = Base + Word;
      uint16_t Sum = 0;
      for (unsigned Byte = 0; Byte < 4; ++Byte) {
        const uint8_t A = Left[LeftBase + Byte];
        const uint8_t B = Permuted[RightBase + Byte];
        Sum += A >= B ? static_cast<uint16_t>(A - B)
                      : static_cast<uint16_t>(B - A);
      }
      Expected[Base + Word * 2] = static_cast<uint8_t>(Sum);
      Expected[Base + Word * 2 + 1] = static_cast<uint8_t>(Sum >> 8);
    }
  return Expected;
}

llvm::ConstantInt *vectorConstant(llvm::LLVMContext &Context,
                                  const std::array<uint8_t, 64> &Bytes,
                                  uint16_t Width) {
  std::array<uint64_t, 8> Words{};
  for (unsigned Byte = 0; Byte < Width; ++Byte)
    Words[Byte / 8] |= static_cast<uint64_t>(Bytes[Byte]) << ((Byte % 8) * 8);
  return llvm::ConstantInt::get(
      Context, llvm::APInt(Width * 8,
                           llvm::ArrayRef<uint64_t>(Words.data(), Width / 8)));
}

MedVar parameter(int Id, uint16_t Size) {
  MedVar Value;
  Value.Kind = MedVar::Param;
  Value.TheArch = Arch::X64;
  Value.Id = Id;
  Value.Size = Size;
  Value.RegOff = kNoParamReg;
  return Value;
}

MedFunc malformedVoidGfni() {
  MedFunc Function;
  Function.Entry = 0x1000;
  Function.Name = "gfni_shape_guard";
  Function.CC = CallingConv::SysV_AMD64;
  Function.ReturnType = NdType::makeVoid();

  MedVar Left = parameter(1, 64);
  MedVar Right = parameter(2, 64);
  Function.Params = {Left, Right};

  MedOp Operation;
  Operation.Addr = Function.Entry;
  Operation.Opcode = NdOp::INTRINSIC;
  Operation.Output.Size = 0;
  Operation.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Gf2p8MulB), 2));
  Operation.addInput(Left);
  Operation.addInput(Right);

  MedOp Return;
  Return.Addr = Function.Entry + 1;
  Return.Opcode = NdOp::RETURN;

  MedBlock Block;
  Block.Id = 0;
  Block.Ops = {std::move(Operation), std::move(Return)};
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

void exitOnFatalError(void *, const char *Reason, bool) {
  std::fputs(Reason, stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::_Exit(1);
}

void foldModuleConstants(llvm::Module &Module) {
  llvm::LoopAnalysisManager LoopAnalyses;
  llvm::FunctionAnalysisManager FunctionAnalyses;
  llvm::CGSCCAnalysisManager CgsccAnalyses;
  llvm::ModuleAnalysisManager ModuleAnalyses;
  llvm::PassBuilder Passes;
  Passes.registerModuleAnalyses(ModuleAnalyses);
  Passes.registerCGSCCAnalyses(CgsccAnalyses);
  Passes.registerFunctionAnalyses(FunctionAnalyses);
  Passes.registerLoopAnalyses(LoopAnalyses);
  Passes.crossRegisterProxies(LoopAnalyses, FunctionAnalyses, CgsccAnalyses,
                              ModuleAnalyses);
  llvm::ModulePassManager Pipeline =
      Passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  Pipeline.run(Module, ModuleAnalyses);
}

HighFunc vectorHighCFunction(Intrinsic Id, uint16_t Width, unsigned Index) {
  HighFunc Function;
  Function.Entry = 0x2000 + Index * 0x10;
  Function.Name = "gfni_highc_" + std::to_string(Index);
  Function.ReturnType = NdType::makeInt(Width, false);
  Function.Params = {{"arg0", NdType::makeInt(Width, false)},
                     {"arg1", NdType::makeInt(Width, false)}};

  MedVar Left = parameter(0, Width);
  MedVar Right = parameter(1, Width);
  auto Call = HighExpr::makeCall(
      intrinsicName(Id), 0,
      {HighExpr::makeVar(Left, NdType::makeInt(Width, false)),
       HighExpr::makeVar(Right, NdType::makeInt(Width, false))});
  Call->IntrinsicId = Id;
  Call->Type = NdType::makeInt(Width, false);
  if (Id != Intrinsic::Gf2p8MulB)
    Call->Operands.push_back(HighExpr::makeConst(0x63, 1));

  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Call);
  Function.Body.push_back(std::move(Return));
  return Function;
}

testing::AssertionResult validGfniHighC(llvm::StringRef Source) {
  std::string CompilerPath = NEVERD_TEST_CLANG;
  if (CompilerPath.empty()) {
    auto Compiler = llvm::sys::findProgramByName("clang");
    if (!Compiler)
      return testing::AssertionSuccess();
    CompilerPath = *Compiler;
  }

  llvm::SmallString<128> SourcePath;
  llvm::SmallString<128> ObjectPath;
  llvm::SmallString<128> StderrPath;
  std::error_code Error =
      llvm::sys::fs::createTemporaryFile("neverd-gfni-highc", "c", SourcePath);
  if (Error)
    return testing::AssertionFailure() << Error.message();
  llvm::FileRemover RemoveSource(SourcePath);
  Error =
      llvm::sys::fs::createTemporaryFile("neverd-gfni-highc", "o", ObjectPath);
  if (Error)
    return testing::AssertionFailure() << Error.message();
  llvm::FileRemover RemoveObject(ObjectPath);
  Error = llvm::sys::fs::createTemporaryFile("neverd-gfni-highc", "err",
                                             StderrPath);
  if (Error)
    return testing::AssertionFailure() << Error.message();
  llvm::FileRemover RemoveStderr(StderrPath);
  {
    llvm::raw_fd_ostream Out(SourcePath, Error);
    if (Error)
      return testing::AssertionFailure() << Error.message();
    Out << Source;
  }

  llvm::SmallVector<llvm::StringRef, 12> Args{CompilerPath,
                                              "-target",
                                              "x86_64-none-elf",
                                              "-ffreestanding",
                                              "-std=gnu11",
                                              "-Werror",
                                              "-c",
                                              SourcePath,
                                              "-o",
                                              ObjectPath};
  std::optional<llvm::StringRef> Redirects[] = {std::nullopt, std::nullopt,
                                                StderrPath.str()};
  std::string ExecuteError;
  const int RC = llvm::sys::ExecuteAndWait(
      CompilerPath, Args, std::nullopt, Redirects,
      /*SecondsToWait=*/30, /*MemoryLimit=*/0, &ExecuteError);
  if (RC == 0)
    return testing::AssertionSuccess();
  auto ErrorBuffer = llvm::MemoryBuffer::getFile(StderrPath);
  return testing::AssertionFailure()
         << ExecuteError
         << (ErrorBuffer ? (*ErrorBuffer)->getBuffer().str() : std::string{});
}

TEST(X86EVEXCrypto, AesIsLaneLocalAndSupportsHighRegisters) {
  const std::vector<std::pair<std::vector<uint8_t>, unsigned>> Cases{
    {{0x62,0xf2,0x6d,0x48,0xdc,0xcb},1}, {{0x62,0xa2,0x6d,0x40,0xdc,0xcb},17}};
  for (const auto &[Raw,Dst]:Cases) {
    Decoder D; ASSERT_TRUE(D.init(Arch::X64)); DecodedInsn I{};
    ASSERT_EQ(D.decodeOneForLift(Raw.data(),Raw.size(),0x1000,I),(int)Raw.size());
    std::vector<LowOp> Ops; ASSERT_NO_THROW(D.liftToLow(I,Ops));
    BinaryImage Img=image(1); NdOpEmulator E(Img); E.setStrictMode(true);
    E.setRegisterBytes(x86reg::vectorReg(Dst+1),std::vector<uint8_t>(64));
    E.setRegisterBytes(x86reg::vectorReg(Dst+2),std::vector<uint8_t>(64)); run(E,Ops);
    auto R=E.getRegisterBytes(x86reg::vectorReg(Dst)); ASSERT_TRUE(R);
    EXPECT_TRUE(std::all_of(R->begin(),R->end(),[](uint8_t B){return B==0x63;}));
  }
}
TEST(X86EVEXCrypto, AllAesRoundsAndWidths) {
  for(uint8_t Opcode:{uint8_t(0xdc),uint8_t(0xdd),uint8_t(0xde),uint8_t(0xdf)})
    for(uint8_t Length:{uint8_t(0x08),uint8_t(0x28),uint8_t(0x48)}) {
      BinaryImage I=image(1); NdOpEmulator E(I); E.setStrictMode(true);
      E.setRegisterBytes(x86reg::vectorReg(2),std::vector<uint8_t>(64));
      E.setRegisterBytes(x86reg::vectorReg(3),std::vector<uint8_t>(64));
      run(E,lift({0x62,0xf2,0x6d,Length,Opcode,0xcb}));
      auto R=E.getRegisterBytes(x86reg::vectorReg(1)); ASSERT_TRUE(R);
      const size_t Width=Length==0x08?16:(Length==0x28?32:64);
      const uint8_t Expected=Opcode<0xde?0x63:0x52;
      EXPECT_TRUE(std::all_of(R->begin(),R->begin()+Width,[Expected](uint8_t B){return B==Expected;}));
    }
}
TEST(X86EVEXCrypto, PclmulIsLaneLocal) {
  BinaryImage I=image(1); NdOpEmulator E(I); E.setStrictMode(true); std::vector<uint8_t>A(64),B(64);
  for(unsigned L=0;L<4;++L){A[L*16+8]=1;B[L*16+8]=3+L;}
  E.setRegisterBytes(x86reg::vectorReg(2),A);E.setRegisterBytes(x86reg::vectorReg(3),B);
  run(E,lift({0x62,0xf3,0x6d,0x48,0x44,0xcb,0x11})); auto R=E.getRegisterBytes(x86reg::vectorReg(1));ASSERT_TRUE(R);
  for(unsigned L=0;L<4;++L) EXPECT_EQ((*R)[L*16],3+L);
}
TEST(X86EVEXCrypto, PclmulSupportsAllWidths) {
  for(uint8_t Length:{uint8_t(0x08),uint8_t(0x28),uint8_t(0x48)}) {
    BinaryImage I=image(1); NdOpEmulator E(I); E.setStrictMode(true);
    std::vector<uint8_t>A(64),B(64);for(unsigned L=0;L<4;++L){A[L*16]=2;B[L*16]=5;}
    E.setRegisterBytes(x86reg::vectorReg(2),A);E.setRegisterBytes(x86reg::vectorReg(3),B);
    run(E,lift({0x62,0xf3,0x6d,Length,0x44,0xcb,0x00}));
    auto R=E.getRegisterBytes(x86reg::vectorReg(1));ASSERT_TRUE(R);
    const unsigned Lanes=Length==0x08?1:(Length==0x28?2:4);
    for(unsigned L=0;L<Lanes;++L)EXPECT_EQ((*R)[L*16],10);
  }
}
TEST(X86EVEXCrypto, MemorySourceExecutesAfterFullLoad) {
  auto Ops=lift({0x62,0xf2,0x6d,0x48,0xdf,0x08}); BinaryImage I=image(64);
  NdOpEmulator E(I);E.setStrictMode(true);E.setRegister(x86reg::RAX,0x4000);
  E.setRegisterBytes(x86reg::vectorReg(2),std::vector<uint8_t>(64));run(E,Ops);
  auto R=E.getRegisterBytes(x86reg::vectorReg(1));ASSERT_TRUE(R);
  EXPECT_TRUE(std::all_of(R->begin(),R->end(),[](uint8_t B){return B==0x52;}));
}
TEST(X86EVEXCrypto, MemoryFaultDoesNotCommitDestination) {
  auto Ops=lift({0x62,0xf2,0x6d,0x48,0xdf,0x08}); BinaryImage I=image(16); NdOpEmulator E(I);E.setStrictMode(true);
  E.setRegister(x86reg::RAX,0x5000);E.setRegisterBytes(x86reg::vectorReg(2),std::vector<uint8_t>(64));
  std::vector<uint8_t> Old(64,0xa5);E.setRegisterBytes(x86reg::vectorReg(1),Old);bool Failed=false;
  for(const auto &Op:Ops)if(!E.step(Op)){Failed=true;break;} EXPECT_TRUE(Failed);EXPECT_EQ(E.getRegisterBytes(x86reg::vectorReg(1)),Old);
}
TEST(X86EVEXCrypto, ReservedMaskEncodingFailsClosed) {
  const std::vector<uint8_t> Raw{0x62,0xf2,0x6d,0x49,0xdc,0xcb};
  Decoder D;ASSERT_TRUE(D.init(Arch::X64));DecodedInsn I{};
  if(D.decodeOneForLift(Raw.data(),Raw.size(),0x1000,I)!=(int)Raw.size())return;
  std::vector<LowOp> Ops;EXPECT_THROW(D.liftToLow(I,Ops),UnliftedInstruction);
}

TEST(X86EVEXCrypto, GfniEvexMaskedRegistersAndZmmBackendAreExact) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EQ(x86HighCIntrinsicFatalReason(Intrinsic::Vdbpsadbw), nullptr);
  EXPECT_NE(x86HighCIntrinsicFatalReason(Intrinsic::X86FourFMA), nullptr);

  std::vector<HighFunc> HighFunctions;
  unsigned HighFunctionIndex = 0;
  for (Intrinsic Id : {Intrinsic::Gf2p8MulB, Intrinsic::Gf2p8AffineQb,
                       Intrinsic::Gf2p8AffineInvQb})
    for (uint16_t Width : {uint16_t{16}, uint16_t{32}, uint16_t{64}})
      HighFunctions.push_back(
          vectorHighCFunction(Id, Width, HighFunctionIndex++));
  for (uint16_t Width : {uint16_t{16}, uint16_t{32}, uint16_t{64}})
    HighFunctions.push_back(
        vectorHighCFunction(Intrinsic::Vdbpsadbw, Width, HighFunctionIndex++));
  std::string HighC;
  llvm::raw_string_ostream HighCOS(HighC);
  CEmitterOptions HighCOptions;
  HighCOptions.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(HighFunctions, HighCOS, HighCOptions));
  HighCOS.flush();
  EXPECT_NE(HighC.find("_mm_gf2p8mul_epi8"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("_mm256_gf2p8affine_epi64_epi8"), std::string::npos)
      << HighC;
  EXPECT_NE(HighC.find("_mm512_gf2p8affineinv_epi64_epi8"), std::string::npos)
      << HighC;
  EXPECT_NE(HighC.find("_mm512_dbsad_epu8"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("typedef unsigned _BitInt(512) uint512_t;"),
            std::string::npos)
      << HighC;
  EXPECT_NE(HighC.find("__builtin_bit_cast"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("target(\"gfni\")"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("target(\"avx,gfni\")"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("target(\"avx512f,gfni\")"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("target(\"avx512bw,avx512vl\")"), std::string::npos)
      << HighC;
  EXPECT_NE(HighC.find("target(\"avx512bw\")"), std::string::npos) << HighC;
  EXPECT_EQ(HighC.find("<unknown"), std::string::npos) << HighC;
  EXPECT_TRUE(validGfniHighC(HighC));

  HighFunc EmptyTarget =
      vectorHighCFunction(Intrinsic::Gf2p8MulB, 16, HighFunctionIndex++);
  ASSERT_FALSE(EmptyTarget.Body.empty());
  ASSERT_NE(EmptyTarget.Body.front().RetVal, nullptr);
  EmptyTarget.Body.front().RetVal->CallTarget.clear();
  std::string EmptyTargetHighC;
  llvm::raw_string_ostream EmptyTargetHighCOS(EmptyTargetHighC);
  ASSERT_TRUE(
      HighCEmitter().emit({EmptyTarget}, EmptyTargetHighCOS, HighCOptions));
  EmptyTargetHighCOS.flush();
  EXPECT_NE(EmptyTargetHighC.find("#include <immintrin.h>"), std::string::npos)
      << EmptyTargetHighC;
  EXPECT_TRUE(validGfniHighC(EmptyTargetHighC));

  EXPECT_EXIT(
      {
        llvm::install_fatal_error_handler(exitOnFatalError);
        llvm::LLVMContext Context;
        MedFunc Probe = malformedVoidGfni();
        (void)MedLLVMEmitter().emit({Probe}, Context, "gfni-shape-guard",
                                    Arch::X64);
      },
      ::testing::ExitedWithCode(1), "invalid GFNI intrinsic shape");
  EXPECT_EXIT(
      {
        llvm::install_fatal_error_handler(exitOnFatalError);
        llvm::LLVMContext Context;
        MedFunc Probe = malformedVoidGfni();
        Probe.Blocks.front().Ops.front().Output = MedVar::makeConst(0, 64);
        (void)MedLLVMEmitter().emit({Probe}, Context, "gfni-output-guard",
                                    Arch::X64);
      },
      ::testing::ExitedWithCode(1), "invalid GFNI intrinsic shape");
  EXPECT_EXIT(
      {
        llvm::install_fatal_error_handler(exitOnFatalError);
        llvm::LLVMContext Context;
        MedFunc Probe = malformedVoidGfni();
        Probe.Blocks.front().Ops.front().Output.Kind = MedVar::Temp;
        Probe.Blocks.front().Ops.front().Output.TheArch = Arch::X64;
        Probe.Blocks.front().Ops.front().Output.Id = 3;
        Probe.Blocks.front().Ops.front().Output.Size = 64;
        (void)MedLLVMEmitter().emit({Probe}, Context, "gfni-target-guard",
                                    Arch::AArch64);
      },
      ::testing::ExitedWithCode(1), "GFNI intrinsic requires an x86 target");
  EXPECT_EXIT(
      ([] {
        llvm::install_fatal_error_handler(exitOnFatalError);
        llvm::LLVMContext Context;
        llvm::Module Module("vdbpsadbw-shape", Context);
        auto *Function = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getInt128Ty(Context), false),
            llvm::GlobalValue::ExternalLinkage, "vdbpsadbw_shape", Module);
        llvm::IRBuilder<> Builder(
            llvm::BasicBlock::Create(Context, "entry", Function));
        MedOp Operation;
        Operation.Opcode = NdOp::INTRINSIC;
        Operation.Output.Kind = MedVar::Temp;
        Operation.Output.TheArch = Arch::X64;
        Operation.Output.Id = 4;
        Operation.Output.Size = 16;
        Operation.addInput(
            MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Vdbpsadbw), 2));
        Operation.addInput(parameter(1, 16));
        Operation.addInput(parameter(2, 16));
        Operation.addInput(MedVar::makeConst(0x100, 1));
        std::array<uint8_t, 64> Bytes{};
        MedLLVMEmitter Emitter;
        (void)MedLLVMEmitterTestPeer::emitIntrinsic(
            Emitter, Context, Module, Operation,
            vectorConstant(Context, Bytes, 16),
            vectorConstant(Context, Bytes, 16), Builder);
        std::_Exit(0);
      }()),
      ::testing::ExitedWithCode(1), "invalid VDBPSADBW intrinsic shape");
  EXPECT_EXIT(
      ([] {
        llvm::install_fatal_error_handler(exitOnFatalError);
        for (uint16_t Width : {uint16_t{16}, uint16_t{32}, uint16_t{64}})
          for (uint8_t Immediate :
               {uint8_t{0x00}, uint8_t{0x1b}, uint8_t{0xe4}, uint8_t{0xff}}) {
            llvm::LLVMContext Context;
            llvm::Module Module("vdbpsadbw-vector", Context);
            auto *FunctionType = llvm::FunctionType::get(
                llvm::IntegerType::get(Context, Width * 8), false);
            const std::string FunctionName = "vdbpsadbw_" +
                                             std::to_string(Width) + "_" +
                                             std::to_string(Immediate);
            auto *Function = llvm::Function::Create(
                FunctionType, llvm::GlobalValue::ExternalLinkage, FunctionName,
                Module);
            auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
            llvm::IRBuilder<> Builder(Entry);

            MedOp Operation;
            Operation.Opcode = NdOp::INTRINSIC;
            Operation.Output.Kind = MedVar::Temp;
            Operation.Output.TheArch = Arch::X64;
            Operation.Output.Id = 4;
            Operation.Output.SSAVer = 1;
            Operation.Output.Size = Width;
            Operation.addInput(MedVar::makeConst(
                static_cast<uint64_t>(Intrinsic::Vdbpsadbw), 2));
            Operation.addInput(parameter(1, Width));
            Operation.addInput(parameter(2, Width));
            Operation.addInput(MedVar::makeConst(Immediate, 1));

            std::array<uint8_t, 64> Left{};
            std::array<uint8_t, 64> Right{};
            for (unsigned Byte = 0; Byte < Width; ++Byte) {
              Left[Byte] = static_cast<uint8_t>(Byte * 43 + 7);
              Right[Byte] = static_cast<uint8_t>(Byte * 31 + 0xd3);
            }
            const std::array<uint8_t, 64> Expected =
                expectedVdbpsadbw(Left, Right, Width, Immediate);

            MedLLVMEmitter Emitter;
            llvm::Value *Result = MedLLVMEmitterTestPeer::emitIntrinsic(
                Emitter, Context, Module, Operation,
                vectorConstant(Context, Left, Width),
                vectorConstant(Context, Right, Width), Builder);
            if (!Result || !Result->getType()->isIntegerTy(Width * 8))
              std::_Exit(2);
            Builder.CreateRet(Result);

            std::string Verification;
            llvm::raw_string_ostream VerificationStream(Verification);
            if (llvm::verifyModule(Module, &VerificationStream))
              std::_Exit(3);
            foldModuleConstants(Module);
            llvm::Function *FoldedFunction = Module.getFunction(FunctionName);
            if (!FoldedFunction)
              std::_Exit(4);
            const llvm::ReturnInst *Return = nullptr;
            for (const llvm::BasicBlock &Block : *FoldedFunction)
              if (const auto *Candidate =
                      llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator()))
                Return = Candidate;
            if (!Return)
              std::_Exit(5);
            const auto *Constant =
                llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
            if (!Constant)
              std::_Exit(6);
            const llvm::APInt &Value = Constant->getValue();
            for (unsigned Byte = 0; Byte < Width; ++Byte)
              if (Value.extractBitsAsZExtValue(8, Byte * 8) != Expected[Byte])
                std::_Exit(7);
          }
        std::_Exit(0);
      }()),
      ::testing::ExitedWithCode(0), ".*");

  struct MaskCase {
    std::vector<uint8_t> Encoding;
    Intrinsic Id;
    bool HasImmediate;
    uint8_t Immediate;
    uint64_t Mask;
    bool Zeroing;
  };
  const MaskCase MaskCases[] = {
      {{0x62, 0xf2, 0x6d, 0x4a, 0xcf, 0xcb},
       Intrinsic::Gf2p8MulB,
       false,
       0,
       UINT64_C(0xaaaaaaaaaaaaaaaa),
       false},
      {{0x62, 0xf2, 0x6d, 0xca, 0xcf, 0xcb},
       Intrinsic::Gf2p8MulB,
       false,
       0,
       UINT64_C(0x5555555555555555),
       true},
      {{0x62, 0xf3, 0xed, 0x4a, 0xce, 0xcb, 0x63},
       Intrinsic::Gf2p8AffineQb,
       true,
       0x63,
       UINT64_C(0xaaaaaaaaaaaaaaaa),
       false},
      {{0x62, 0xf3, 0xed, 0xca, 0xce, 0xcb, 0x63},
       Intrinsic::Gf2p8AffineQb,
       true,
       0x63,
       UINT64_C(0x5555555555555555),
       true},
      {{0x62, 0xf3, 0xed, 0x4a, 0xcf, 0xcb, 0xa5},
       Intrinsic::Gf2p8AffineInvQb,
       true,
       0xa5,
       UINT64_C(0xaaaaaaaaaaaaaaaa),
       false},
      {{0x62, 0xf3, 0xed, 0xca, 0xcf, 0xcb, 0xa5},
       Intrinsic::Gf2p8AffineInvQb,
       true,
       0xa5,
       UINT64_C(0x5555555555555555),
       true},
  };

  for (const MaskCase &Case : MaskCases) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Case.Encoding.data(), Case.Encoding.size(),
                                   0x1000, Insn),
              static_cast<int>(Case.Encoding.size()));
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops));

    const LowOp *Gfni = findIntrinsic(Ops, Case.Id);
    ASSERT_NE(Gfni, nullptr);
    ASSERT_TRUE(Gfni->Output.isTemp());
    ASSERT_EQ(Gfni->Output.Size, 64);
    ASSERT_EQ(Gfni->NumInputs, Case.HasImmediate ? 4 : 3);
    EXPECT_EQ(Gfni->Inputs[1], NdVar::reg(x86reg::vectorReg(2), 64));
    EXPECT_EQ(Gfni->Inputs[2], NdVar::reg(x86reg::vectorReg(3), 64));
    if (Case.HasImmediate) {
      ASSERT_TRUE(Gfni->Inputs[3].isConst());
      EXPECT_EQ(Gfni->Inputs[3].Size, 1);
      EXPECT_EQ(Gfni->Inputs[3].Offset, Case.Immediate);
    }

    std::vector<uint8_t> Old(64);
    for (size_t Index = 0; Index < Old.size(); ++Index)
      Old[Index] = static_cast<uint8_t>(0xa0 + (Index & 0x0f));
    const std::vector<uint8_t> Raw(64, 0x1b);

    for (LowOp &Op : Ops) {
      if (&Op != Gfni)
        continue;
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] = NdVar::reg(x86reg::vectorReg(4), 64);
    }

    BinaryImage Img = image(1);
    NdOpEmulator Emulator(Img);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::vectorReg(1), Old);
    Emulator.setRegisterBytes(x86reg::vectorReg(4), Raw);
    Emulator.setRegister(x86reg::K2, Case.Mask);
    run(Emulator, Ops);

    const auto Result = Emulator.getRegisterBytes(x86reg::vectorReg(1));
    ASSERT_TRUE(Result.has_value());
    ASSERT_EQ(Result->size(), 64u);
    for (size_t Lane = 0; Lane < Result->size(); ++Lane) {
      const bool Active = ((Case.Mask >> Lane) & 1) != 0;
      const uint8_t Expected = Active ? 0x1b : (Case.Zeroing ? 0 : Old[Lane]);
      EXPECT_EQ((*Result)[Lane], Expected) << "byte lane " << Lane;
    }
    EXPECT_EQ(Emulator.getRegister(x86reg::K2), Case.Mask);
  }

  struct RegisterCase {
    std::vector<uint8_t> Encoding;
    Arch TheArch;
    uint16_t Width;
    unsigned Destination;
    unsigned Left;
    unsigned Right;
    bool HasMask;
    bool LegacyVex;
  };
  const RegisterCase RegisterCases[] = {
      {{0x62, 0xf2, 0x6d, 0x0a, 0xcf, 0xcb},
       Arch::X64,
       16,
       1,
       2,
       3,
       true,
       false},
      {{0x62, 0xf2, 0x6d, 0x28, 0xcf, 0xcb},
       Arch::X64,
       32,
       1,
       2,
       3,
       false,
       false},
      {{0x62, 0xa2, 0x6d, 0x42, 0xcf, 0xcb},
       Arch::X64,
       64,
       17,
       18,
       19,
       true,
       false},
      {{0xc4, 0xe2, 0x6d, 0xcf, 0xcb}, Arch::X64, 32, 1, 2, 3, false, true},
      {{0x62, 0xf2, 0x6d, 0x4a, 0xcf, 0xcb},
       Arch::X86,
       64,
       1,
       2,
       3,
       true,
       false},
  };
  for (const RegisterCase &Case : RegisterCases) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Case.TheArch));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Case.Encoding.data(), Case.Encoding.size(),
                                   0x1000, Insn),
              static_cast<int>(Case.Encoding.size()));
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops));

    const LowOp *Gfni = findIntrinsic(Ops, Intrinsic::Gf2p8MulB);
    ASSERT_NE(Gfni, nullptr);
    ASSERT_EQ(Gfni->Output.Size, Case.Width);
    if (Case.LegacyVex) {
      EXPECT_EQ(Gfni->Output,
                NdVar::reg(x86reg::vectorReg(Case.Destination), Case.Width));
    } else {
      EXPECT_TRUE(Gfni->Output.isTemp());
    }
    ASSERT_EQ(Gfni->NumInputs, 3);
    EXPECT_EQ(Gfni->Inputs[1],
              NdVar::reg(x86reg::vectorReg(Case.Left), Case.Width));
    EXPECT_EQ(Gfni->Inputs[2],
              NdVar::reg(x86reg::vectorReg(Case.Right), Case.Width));

    const NdVar Mask = NdVar::reg(x86reg::K2, Case.Width / 8);
    const bool ReadsExpectedMask =
        std::any_of(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
          const NdVar *End = std::begin(Op.Inputs) + Op.NumInputs;
          return std::find(std::begin(Op.Inputs), End, Mask) != End;
        });
    EXPECT_EQ(ReadsExpectedMask, Case.HasMask);

    if (Case.TheArch != Arch::X64)
      continue;

    for (LowOp &Op : Ops) {
      if (&Op != Gfni)
        continue;
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] =
          NdVar::reg(x86reg::vectorReg(4), static_cast<uint16_t>(Case.Width));
    }

    std::vector<uint8_t> Old(64);
    std::vector<uint8_t> Raw(64);
    for (unsigned Byte = 0; Byte < 64; ++Byte) {
      Old[Byte] = static_cast<uint8_t>(0xa0 + (Byte & 0x0f));
      Raw[Byte] = static_cast<uint8_t>(0x31 + Byte);
    }
    constexpr uint64_t kMask = UINT64_C(0xaaaaaaaaaaaaaaaa);
    BinaryImage Img = image(1);
    NdOpEmulator Emulator(Img);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::vectorReg(Case.Destination), Old);
    Emulator.setRegisterBytes(x86reg::vectorReg(4), Raw);
    if (Case.HasMask)
      Emulator.setRegister(x86reg::K2, kMask);
    run(Emulator, Ops);

    const auto Result =
        Emulator.getRegisterBytes(x86reg::vectorReg(Case.Destination));
    ASSERT_TRUE(Result.has_value());
    ASSERT_EQ(Result->size(), 64u);
    for (unsigned Byte = 0; Byte < 64; ++Byte) {
      uint8_t Expected = 0;
      if (Byte < Case.Width) {
        const bool Active = !Case.HasMask || ((kMask >> Byte) & 1) != 0;
        Expected = Active ? Raw[Byte] : Old[Byte];
      }
      EXPECT_EQ((*Result)[Byte], Expected) << "byte " << Byte;
    }
    if (Case.HasMask)
      EXPECT_EQ(Emulator.getRegister(x86reg::K2), kMask);
  }

  auto bypassGfniWithLoadedSource = [](std::vector<LowOp> &Ops, Intrinsic Id) {
    for (LowOp &Op : Ops) {
      if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs < 3 ||
          !Op.Inputs[0].isConst() ||
          Op.Inputs[0].Offset != static_cast<uint64_t>(Id))
        continue;
      const NdVar LoadedSource = Op.Inputs[2];
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] = LoadedSource;
      return true;
    }
    return false;
  };
  auto bytesImage = [](std::vector<uint8_t> Bytes) {
    BinaryImage Img = image(Bytes.size());
    Img.Segments.front().Data = std::move(Bytes);
    return Img;
  };

  // EVEX GF2P8MULB is E4: inactive byte lanes suppress their corresponding
  // memory accesses before the destination writemask is applied.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d, 0x4a,
                                           0xcf, 0x48, 0x01};
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(Encoding.data(), Encoding.size(), 0x1000, Insn),
        static_cast<int>(Encoding.size()));
    ASSERT_NE(Insn.Raw, nullptr);
    ASSERT_NE(Insn.Raw->detail, nullptr);
    const cs_x86 &X86 = Insn.Raw->detail->x86;
    ASSERT_EQ(X86.op_count, 4u);
    EXPECT_EQ(X86.operands[3].type, X86_OP_MEM);
    EXPECT_EQ(X86.operands[3].size, 64u);
    EXPECT_EQ(X86.operands[3].mem.disp, 64);
    EXPECT_EQ(X86.operands[3].avx_bcast, X86_AVX_BCAST_INVALID);

    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops));
    ASSERT_TRUE(bypassGfniWithLoadedSource(Ops, Intrinsic::Gf2p8MulB));

    std::vector<uint8_t> OldDestination(64, 0xa5);
    BinaryImage OneByte = bytesImage({0x7b});
    NdOpEmulator OneActive(OneByte);
    OneActive.setStrictMode(true);
    OneActive.setLoadCollect(true);
    OneActive.setRegister(x86reg::RAX, 0x3fc0);
    OneActive.setRegister(x86reg::K2, 1);
    OneActive.setRegisterBytes(x86reg::vectorReg(1), OldDestination);
    OneActive.setRegisterBytes(x86reg::vectorReg(2),
                               std::vector<uint8_t>(64, 0x33));
    ASSERT_EQ(OneActive.run(Ops), Ops.size());
    std::vector<uint8_t> Expected = OldDestination;
    Expected[0] = 0x7b;
    EXPECT_EQ(OneActive.getRegisterBytes(x86reg::vectorReg(1)), Expected);
    ASSERT_EQ(OneActive.getLoadRecords().size(), 1u);
    EXPECT_EQ(OneActive.getLoadRecords()[0].Addr, UINT64_C(0x4000));
    EXPECT_EQ(OneActive.getLoadRecords()[0].Size, 1u);
    EXPECT_EQ(OneActive.getRegister(x86reg::K2), 1u);

    NdOpEmulator SecondActive(OneByte);
    SecondActive.setStrictMode(true);
    SecondActive.setRegister(x86reg::RAX, 0x3fc0);
    SecondActive.setRegister(x86reg::K2, 2);
    SecondActive.setRegisterBytes(x86reg::vectorReg(1), OldDestination);
    SecondActive.setRegisterBytes(x86reg::vectorReg(2),
                                  std::vector<uint8_t>(64, 0x33));
    EXPECT_LT(SecondActive.run(Ops), Ops.size());
    EXPECT_EQ(SecondActive.getRegisterBytes(x86reg::vectorReg(1)),
              OldDestination);
    EXPECT_EQ(SecondActive.getRegister(x86reg::K2), 2u);

    BinaryImage SuppressedImage = image(1);
    NdOpEmulator Suppressed(SuppressedImage);
    Suppressed.setStrictMode(true);
    Suppressed.setLoadCollect(true);
    Suppressed.setRegister(x86reg::RAX, 0x9000);
    Suppressed.setRegister(x86reg::K2, 0);
    Suppressed.setRegisterBytes(x86reg::vectorReg(1), OldDestination);
    Suppressed.setRegisterBytes(x86reg::vectorReg(2),
                                std::vector<uint8_t>(64, 0x33));
    ASSERT_EQ(Suppressed.run(Ops), Ops.size());
    EXPECT_EQ(Suppressed.getRegisterBytes(x86reg::vectorReg(1)),
              OldDestination);
    EXPECT_TRUE(Suppressed.getLoadRecords().empty());
    EXPECT_EQ(Suppressed.getRegister(x86reg::K2), 0u);

    const std::vector<uint8_t> UnmaskedEncoding = {0x62, 0xf2, 0x6d,
                                                   0x48, 0xcf, 0x08};
    Decoder UnmaskedDec;
    ASSERT_TRUE(UnmaskedDec.init(Arch::X64));
    DecodedInsn UnmaskedInsn{};
    ASSERT_EQ(UnmaskedDec.decodeOneForLift(UnmaskedEncoding.data(),
                                           UnmaskedEncoding.size(), 0x1000,
                                           UnmaskedInsn),
              static_cast<int>(UnmaskedEncoding.size()));
    std::vector<LowOp> UnmaskedOps;
    ASSERT_NO_THROW(UnmaskedDec.liftToLow(UnmaskedInsn, UnmaskedOps));
    ASSERT_TRUE(bypassGfniWithLoadedSource(UnmaskedOps, Intrinsic::Gf2p8MulB));
    std::vector<uint8_t> AllBytes(64);
    for (unsigned Byte = 0; Byte < AllBytes.size(); ++Byte)
      AllBytes[Byte] = static_cast<uint8_t>(Byte * 3 + 1);
    BinaryImage AllBytesImage = bytesImage(AllBytes);
    NdOpEmulator Unmasked(AllBytesImage);
    Unmasked.setStrictMode(true);
    Unmasked.setLoadCollect(true);
    Unmasked.setRegister(x86reg::RAX, 0x4000);
    Unmasked.setRegisterBytes(x86reg::vectorReg(1), OldDestination);
    Unmasked.setRegisterBytes(x86reg::vectorReg(2),
                              std::vector<uint8_t>(64, 0x33));
    ASSERT_EQ(Unmasked.run(UnmaskedOps), UnmaskedOps.size());
    EXPECT_EQ(Unmasked.getRegisterBytes(x86reg::vectorReg(1)), AllBytes);
    ASSERT_EQ(Unmasked.getLoadRecords().size(), 64u);
    EXPECT_EQ(Unmasked.getLoadRecords().front().Addr, UINT64_C(0x4000));
    EXPECT_EQ(Unmasked.getLoadRecords().back().Addr, UINT64_C(0x403f));
  }

  // GF2P8AFFINEQB and GF2P8AFFINEINVQB are E4NF: both ordinary full tuples
  // and m64 broadcasts are read independently of the destination writemask.
  struct AffineMemoryCase {
    std::vector<uint8_t> FullEncoding;
    std::vector<uint8_t> BroadcastEncoding;
    Intrinsic Id;
  };
  const AffineMemoryCase AffineMemoryCases[] = {
      {{0x62, 0xf3, 0xed, 0x4a, 0xce, 0x48, 0x01, 0x63},
       {0x62, 0xf3, 0xed, 0x5a, 0xce, 0x48, 0x01, 0x63},
       Intrinsic::Gf2p8AffineQb},
      {{0x62, 0xf3, 0xed, 0x4a, 0xcf, 0x48, 0x01, 0xa5},
       {0x62, 0xf3, 0xed, 0x5a, 0xcf, 0x48, 0x01, 0xa5},
       Intrinsic::Gf2p8AffineInvQb},
  };
  for (const AffineMemoryCase &Case : AffineMemoryCases) {
    const std::vector<uint8_t> OldMemoryDestination(64, 0xa5);
    std::vector<uint8_t> FullBytes(64);
    for (unsigned Byte = 0; Byte < FullBytes.size(); ++Byte)
      FullBytes[Byte] = static_cast<uint8_t>(Byte + 1);

    Decoder FullDec;
    ASSERT_TRUE(FullDec.init(Arch::X64));
    DecodedInsn FullInsn{};
    ASSERT_EQ(FullDec.decodeOneForLift(Case.FullEncoding.data(),
                                       Case.FullEncoding.size(), 0x1000,
                                       FullInsn),
              static_cast<int>(Case.FullEncoding.size()));
    ASSERT_NE(FullInsn.Raw, nullptr);
    ASSERT_NE(FullInsn.Raw->detail, nullptr);
    const cs_x86 &FullX86 = FullInsn.Raw->detail->x86;
    ASSERT_EQ(FullX86.op_count, 5u);
    EXPECT_EQ(FullX86.operands[3].type, X86_OP_MEM);
    EXPECT_EQ(FullX86.operands[3].size, 64u);
    EXPECT_EQ(FullX86.operands[3].mem.disp, 64);
    EXPECT_EQ(FullX86.operands[3].avx_bcast, X86_AVX_BCAST_INVALID);
    std::vector<LowOp> FullOps;
    ASSERT_NO_THROW(FullDec.liftToLow(FullInsn, FullOps));
    ASSERT_TRUE(bypassGfniWithLoadedSource(FullOps, Case.Id));

    BinaryImage FullImage = bytesImage(FullBytes);
    NdOpEmulator Full(FullImage);
    Full.setStrictMode(true);
    Full.setLoadCollect(true);
    Full.setRegister(x86reg::RAX, 0x3fc0);
    Full.setRegister(x86reg::K2, UINT64_MAX);
    Full.setRegisterBytes(x86reg::vectorReg(1), OldMemoryDestination);
    Full.setRegisterBytes(x86reg::vectorReg(2), std::vector<uint8_t>(64, 0x33));
    ASSERT_EQ(Full.run(FullOps), FullOps.size());
    EXPECT_EQ(Full.getRegisterBytes(x86reg::vectorReg(1)), FullBytes);
    ASSERT_EQ(Full.getLoadRecords().size(), 1u);
    EXPECT_EQ(Full.getLoadRecords()[0].Addr, UINT64_C(0x4000));
    EXPECT_EQ(Full.getLoadRecords()[0].Size, 64u);

    BinaryImage FullZeroMaskImage = image(1);
    NdOpEmulator FullZeroMask(FullZeroMaskImage);
    FullZeroMask.setStrictMode(true);
    FullZeroMask.setRegister(x86reg::RAX, 0x9000);
    FullZeroMask.setRegister(x86reg::K2, 0);
    FullZeroMask.setRegisterBytes(x86reg::vectorReg(1), OldMemoryDestination);
    FullZeroMask.setRegisterBytes(x86reg::vectorReg(2),
                                  std::vector<uint8_t>(64, 0x33));
    EXPECT_LT(FullZeroMask.run(FullOps), FullOps.size());
    EXPECT_EQ(FullZeroMask.getRegisterBytes(x86reg::vectorReg(1)),
              OldMemoryDestination);
    EXPECT_EQ(FullZeroMask.getRegister(x86reg::K2), 0u);

    Decoder BroadcastDec;
    ASSERT_TRUE(BroadcastDec.init(Arch::X64));
    DecodedInsn BroadcastInsn{};
    ASSERT_EQ(BroadcastDec.decodeOneForLift(Case.BroadcastEncoding.data(),
                                            Case.BroadcastEncoding.size(),
                                            0x1000, BroadcastInsn),
              static_cast<int>(Case.BroadcastEncoding.size()));
    ASSERT_NE(BroadcastInsn.Raw, nullptr);
    ASSERT_NE(BroadcastInsn.Raw->detail, nullptr);
    const cs_x86 &BroadcastX86 = BroadcastInsn.Raw->detail->x86;
    ASSERT_EQ(BroadcastX86.op_count, 5u);
    EXPECT_EQ(BroadcastX86.operands[3].type, X86_OP_MEM);
    EXPECT_EQ(BroadcastX86.operands[3].size, 8u);
    EXPECT_EQ(BroadcastX86.operands[3].mem.disp, 8);
    EXPECT_EQ(BroadcastX86.operands[3].avx_bcast, X86_AVX_BCAST_8);
    std::vector<LowOp> BroadcastOps;
    ASSERT_NO_THROW(BroadcastDec.liftToLow(BroadcastInsn, BroadcastOps));
    ASSERT_TRUE(bypassGfniWithLoadedSource(BroadcastOps, Case.Id));

    const std::vector<uint8_t> Matrix = {0x11, 0x22, 0x33, 0x44,
                                         0x55, 0x66, 0x77, 0x88};
    BinaryImage BroadcastImage = bytesImage(Matrix);
    NdOpEmulator Broadcast(BroadcastImage);
    Broadcast.setStrictMode(true);
    Broadcast.setLoadCollect(true);
    Broadcast.setRegister(x86reg::RAX, 0x3ff8);
    Broadcast.setRegister(x86reg::K2, UINT64_MAX);
    Broadcast.setRegisterBytes(x86reg::vectorReg(1), OldMemoryDestination);
    Broadcast.setRegisterBytes(x86reg::vectorReg(2),
                               std::vector<uint8_t>(64, 0x33));
    ASSERT_EQ(Broadcast.run(BroadcastOps), BroadcastOps.size());
    const auto BroadcastResult =
        Broadcast.getRegisterBytes(x86reg::vectorReg(1));
    ASSERT_TRUE(BroadcastResult.has_value());
    for (unsigned Byte = 0; Byte < 64; ++Byte)
      EXPECT_EQ((*BroadcastResult)[Byte], Matrix[Byte % Matrix.size()]);
    ASSERT_EQ(Broadcast.getLoadRecords().size(), 1u);
    EXPECT_EQ(Broadcast.getLoadRecords()[0].Addr, UINT64_C(0x4000));
    EXPECT_EQ(Broadcast.getLoadRecords()[0].Size, 8u);
    EXPECT_EQ(Broadcast.getRegister(x86reg::K2), UINT64_MAX);

    BinaryImage ShortBroadcastImage =
        bytesImage(std::vector<uint8_t>(Matrix.begin(), Matrix.end() - 1));
    NdOpEmulator ShortBroadcast(ShortBroadcastImage);
    ShortBroadcast.setStrictMode(true);
    ShortBroadcast.setRegister(x86reg::RAX, 0x3ff8);
    ShortBroadcast.setRegister(x86reg::K2, UINT64_MAX);
    ShortBroadcast.setRegisterBytes(x86reg::vectorReg(1), OldMemoryDestination);
    ShortBroadcast.setRegisterBytes(x86reg::vectorReg(2),
                                    std::vector<uint8_t>(64, 0x33));
    EXPECT_LT(ShortBroadcast.run(BroadcastOps), BroadcastOps.size());
    EXPECT_EQ(ShortBroadcast.getRegisterBytes(x86reg::vectorReg(1)),
              OldMemoryDestination);
    EXPECT_EQ(ShortBroadcast.getRegister(x86reg::K2), UINT64_MAX);

    BinaryImage BroadcastZeroMaskImage = image(1);
    NdOpEmulator BroadcastZeroMask(BroadcastZeroMaskImage);
    BroadcastZeroMask.setStrictMode(true);
    BroadcastZeroMask.setRegister(x86reg::RAX, 0x9000);
    BroadcastZeroMask.setRegister(x86reg::K2, 0);
    BroadcastZeroMask.setRegisterBytes(x86reg::vectorReg(1),
                                       OldMemoryDestination);
    BroadcastZeroMask.setRegisterBytes(x86reg::vectorReg(2),
                                       std::vector<uint8_t>(64, 0x33));
    EXPECT_LT(BroadcastZeroMask.run(BroadcastOps), BroadcastOps.size());
    EXPECT_EQ(BroadcastZeroMask.getRegisterBytes(x86reg::vectorReg(1)),
              OldMemoryDestination);
    EXPECT_EQ(BroadcastZeroMask.getRegister(x86reg::K2), 0u);
  }

  const struct NarrowBroadcastCase {
    std::vector<uint8_t> Encoding;
    uint16_t Width;
    x86_avx_bcast Broadcast;
    uint64_t Mask;
  } NarrowBroadcastCases[] = {
      {{0x62, 0xf3, 0xed, 0x1a, 0xce, 0x08, 0x63},
       16,
       X86_AVX_BCAST_2,
       UINT64_C(0xffff)},
      {{0x62, 0xf3, 0xed, 0x3a, 0xce, 0x08, 0x63},
       32,
       X86_AVX_BCAST_4,
       UINT64_C(0xffffffff)},
  };
  for (const NarrowBroadcastCase &Case : NarrowBroadcastCases) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Case.Encoding.data(), Case.Encoding.size(),
                                   0x1000, Insn),
              static_cast<int>(Case.Encoding.size()));
    ASSERT_NE(Insn.Raw, nullptr);
    ASSERT_NE(Insn.Raw->detail, nullptr);
    const cs_x86 &X86 = Insn.Raw->detail->x86;
    ASSERT_EQ(X86.op_count, 5u);
    EXPECT_EQ(X86.operands[1].size, Case.Width / 8);
    EXPECT_EQ(X86.operands[3].size, 8u);
    EXPECT_EQ(X86.operands[3].avx_bcast, Case.Broadcast);
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops));
    ASSERT_TRUE(bypassGfniWithLoadedSource(Ops, Intrinsic::Gf2p8AffineQb));

    const std::vector<uint8_t> Matrix = {0x81, 0x72, 0x63, 0x54,
                                         0x45, 0x36, 0x27, 0x18};
    BinaryImage Img = bytesImage(Matrix);
    NdOpEmulator Emulator(Img);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, 0x4000);
    Emulator.setRegister(x86reg::K2, Case.Mask);
    Emulator.setRegisterBytes(x86reg::vectorReg(1),
                              std::vector<uint8_t>(64, 0xa5));
    Emulator.setRegisterBytes(x86reg::vectorReg(2),
                              std::vector<uint8_t>(64, 0x33));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(x86reg::vectorReg(1));
    ASSERT_TRUE(Result.has_value());
    ASSERT_EQ(Result->size(), 64u);
    for (unsigned Byte = 0; Byte < 64; ++Byte) {
      const uint8_t Expected =
          Byte < Case.Width ? Matrix[Byte % Matrix.size()] : 0;
      EXPECT_EQ((*Result)[Byte], Expected) << "byte " << Byte;
    }
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, UINT64_C(0x4000));
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 8u);
    EXPECT_EQ(Emulator.getRegister(x86reg::K2), Case.Mask);
  }

  const std::vector<std::vector<uint8_t>> RejectedRegisterOnlyForms = {
      {0x62, 0xf2, 0x6d, 0x6a, 0xcf, 0xcb},
      {0x66, 0x62, 0xf2, 0x6d, 0x4a, 0xcf, 0xcb},
  };
  for (const std::vector<uint8_t> &Encoding : RejectedRegisterOnlyForms) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(Encoding.data(), Encoding.size(), 0x1000, Insn),
        static_cast<int>(Encoding.size()));
    std::vector<LowOp> Ops;
    EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }

  const struct BackendCase {
    Intrinsic Id;
    const char *Name;
    bool HasImmediate;
    uint8_t Immediate;
  } BackendCases[] = {
      {Intrinsic::Gf2p8MulB, "gfni_mul_zmm", false, 0},
      {Intrinsic::Gf2p8AffineQb, "gfni_affine_zmm", true, 0x63},
      {Intrinsic::Gf2p8AffineInvQb, "gfni_affine_inv_zmm", true, 0xa5},
  };
  for (const BackendCase &Case : BackendCases) {
    for (uint16_t Width : {uint16_t{16}, uint16_t{32}, uint16_t{64}}) {
      llvm::LLVMContext Context;
      llvm::Module Module("gfni-vector", Context);
      auto *FunctionType = llvm::FunctionType::get(
          llvm::IntegerType::get(Context, Width * 8), false);
      const std::string FunctionName =
          std::string(Case.Name) + "_" + std::to_string(Width);
      auto *Function = llvm::Function::Create(
          FunctionType, llvm::GlobalValue::ExternalLinkage, FunctionName,
          Module);
      auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
      llvm::IRBuilder<> Builder(Entry);

      MedOp Operation;
      Operation.Opcode = NdOp::INTRINSIC;
      Operation.Output.Kind = MedVar::Temp;
      Operation.Output.TheArch = Arch::X64;
      Operation.Output.Id = 2;
      Operation.Output.SSAVer = 1;
      Operation.Output.Size = Width;
      Operation.addInput(MedVar::makeConst(static_cast<uint64_t>(Case.Id), 2));
      Operation.addInput(parameter(1, Width));
      Operation.addInput(parameter(2, Width));
      if (Case.HasImmediate)
        Operation.addInput(MedVar::makeConst(Case.Immediate, 1));

      std::array<uint8_t, 64> Left{};
      std::array<uint8_t, 64> Right{};
      for (unsigned Byte = 0; Byte < Width; ++Byte) {
        Left[Byte] = static_cast<uint8_t>(Byte * 37 + 1);
        Right[Byte] = static_cast<uint8_t>(Byte * 29 + 0x53);
      }
      const std::array<uint8_t, 64> Expected =
          expectedGfni(Case.Id, Left, Right, Case.Immediate);

      MedLLVMEmitter Emitter;
      llvm::Value *Result = MedLLVMEmitterTestPeer::emitGfni(
          Emitter, Context, Module, Operation, Case.Id,
          vectorConstant(Context, Left, Width),
          vectorConstant(Context, Right, Width), Builder);
      ASSERT_NE(Result, nullptr);
      ASSERT_TRUE(Result->getType()->isIntegerTy(Width * 8));
      Builder.CreateRet(Result);

      std::string Verification;
      llvm::raw_string_ostream VerificationStream(Verification);
      EXPECT_FALSE(llvm::verifyModule(Module, &VerificationStream))
          << VerificationStream.str();

      foldModuleConstants(Module);
      llvm::Function *FoldedFunction = Module.getFunction(FunctionName);
      ASSERT_NE(FoldedFunction, nullptr);
      const llvm::ReturnInst *Return = nullptr;
      for (const llvm::BasicBlock &Block : *FoldedFunction)
        if (const auto *Candidate =
                llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator()))
          Return = Candidate;
      ASSERT_NE(Return, nullptr);
      const auto *Constant =
          llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
      ASSERT_NE(Constant, nullptr);
      const llvm::APInt &Value = Constant->getValue();
      for (unsigned Byte = 0; Byte < Width; ++Byte)
        EXPECT_EQ(Value.extractBitsAsZExtValue(8, Byte * 8), Expected[Byte])
            << FunctionName << " byte " << Byte;
    }
  }
}
} // namespace
