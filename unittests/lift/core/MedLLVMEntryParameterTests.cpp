//===- MedLLVMEntryParameterTests.cpp - Defined entry register values
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// An unwritten PHI arm retains an established parameter's bits, without
/// borrowing a different path's SSA definition or recovering unknown state.

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

namespace {
using namespace neverd;

MedFunc entryJoin(Arch Architecture, uint16_t ParameterBytes,
                  uint16_t ReadBytes, bool Reordered) {
  const auto &TRI = getTargetRegInfo(Architecture);
  MedVar Parameter;
  Parameter.Kind = MedVar::Param;
  Parameter.Id = 0;
  Parameter.Size = ParameterBytes;
  Parameter.RegOff = TRI.IntParamRegs.front();
  Parameter.TheArch = Architecture;
  MedVar Condition = Parameter;
  Condition.Id = 1;
  Condition.RegOff = TRI.IntParamRegs[1];
  MedVar Incoming = Parameter;
  Incoming.Kind = MedVar::Reg;
  Incoming.Id = 11;
  Incoming.SSAVer = -1;
  Incoming.Size = ReadBytes;
  MedVar Changed = Incoming;
  Changed.SSAVer = 0;
  MedVar Joined = Changed;
  Joined.SSAVer = 1;
  MedFunc F;
  F.Entry = 0x1000;
  F.Name = "entry_join";
  F.Params = {Parameter, Condition};
  F.ReturnType = NdType::makeInt(ReadBytes, false);
  F.Blocks.resize(3);
  for (unsigned I = 0; I < F.Blocks.size(); ++I) {
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = F.Entry + I * 0x100;
  }
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {2};
  F.Blocks[2].Preds = {0, 1};
  auto Add = [&](unsigned Block, NdOp Code, MedVar Output,
                 std::initializer_list<MedVar> Inputs) {
    MedOp Op;
    Op.Opcode = Code;
    Op.Output = Output;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    F.Blocks[Block].Ops.push_back(Op);
  };
  Add(0, NdOp::COND_BR, {},
      {MedVar::makeConst(0x1200, TRI.PointerSize), Condition});
  // Version zero is a real write on the other path, not an entry definition.
  Add(1, NdOp::COPY, Changed, {MedVar::makeConst(0x73, ReadBytes)});
  Add(1, NdOp::BRANCH, {}, {MedVar::makeConst(0x1200, TRI.PointerSize)});
  PhiNode Phi;
  Phi.Output = Joined;
  Phi.Args = {{0, Incoming}, {1, Changed}};
  F.Blocks[2].Phis.push_back(Phi);
  Add(2, NdOp::RETURN, {}, {Joined});
  if (Reordered) {
    std::reverse(F.Blocks[2].Phis[0].Args.begin(),
                 F.Blocks[2].Phis[0].Args.end());
    std::swap(F.Blocks[1], F.Blocks[2]);
  }
  return F;
}

void projectLowReturn(MedFunc &F, uint16_t Bytes) {
  auto &Join = F.Blocks[2];
  auto Low = Join.Phis.front().Output;
  Low.Id = 13;
  Low.SSAVer = 2;
  Low.Size = Bytes;
  MedOp Slice;
  Slice.Opcode = NdOp::SUBBYTES;
  Slice.Output = Low;
  Slice.addInput(Join.Phis.front().Output);
  Slice.addInput(MedVar::makeConst(0, 4));
  Join.Ops.front().Inputs[0] = Low;
  Join.Ops.insert(Join.Ops.begin(), Slice);
  F.ReturnType = NdType::makeInt(Bytes, false);
}

std::unique_ptr<llvm::Module> emit(const MedFunc &F, const BinaryImage &Image,
                                   llvm::LLVMContext &Context) {
  MedLLVMEmitter Emitter;
  Emitter.setPreserveUnknownSourceValues(true);
  auto Module = Emitter.emit({F}, Context, "entry-values", Image.Arch, {},
                             &Image, Image.Format);
  if (Module) {
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    Pipeline::optimizeOrPromoteModule(*Module);
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  }
  return Module;
}

TEST(MedLLVMEntryParameters, UnwrittenJoinArmsKeepEstablishedArgumentBits) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64, Arch::ARM})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Reordered : {false, true})
        for (bool Narrow : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << unsigned(Architecture) << ':' << unsigned(Format)
                       << ':' << Reordered << ':' << Narrow);
          BinaryImage Image;
          Image.Arch = Architecture;
          Image.Format = Format;
          const unsigned Bytes = getTargetRegInfo(Architecture).PointerSize;
          Image.Bits = Bytes == 8 ? Bitness::Bits64 : Bitness::Bits32;
          const unsigned ReadBytes = Narrow ? Bytes / 2 : Bytes;
          auto F = entryJoin(Architecture, Bytes, ReadBytes, Reordered);
          llvm::LLVMContext Context;
          auto Module = emit(F, Image, Context);
          ASSERT_TRUE(Module);
          std::string Source;
          llvm::raw_string_ostream Output(Source);
          CEmitterOptions Options;
          Options.TheArch = Architecture;
          Options.PreserveLLVMFunctionTypes = true;
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, Options));
          ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
          Source += "\nint main(void) {\n"
                    "const uint64_t mask = UINT64_MAX >> " +
                    std::to_string(64 - ReadBytes * 8) + ";\n" +
                    R"c(
  uint64_t value = UINT64_MAX;
  for (unsigned i = 0; i < 1024; ++i) {
    if (entry_join(value, 1) != (value & mask)) return 1;
    if (entry_join(value, 0) != 0x73) return 2;
    value = value * UINT64_C(6364136223846793005) + 1;
  }
  return 0;
}
)c";
          for (llvm::StringRef Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
        }
}

TEST(MedLLVMEntryParameters, UnboundUnspecifiedAndUncoveredBitsStayUnknown) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::ELF;
    Image.Bits = Bitness::Bits64;
    auto F = entryJoin(Image.Arch, Case == 2 ? 4 : 8, 8, false);
    auto &Incoming = F.Blocks[2].Phis[0].Args[0].second;
    if (Case == 0)
      Incoming.RegOff = getTargetRegInfo(Image.Arch).IntParamRegs[2];
    else if (Case == 1)
      Incoming = MedVar::makeUnspecified(8, Image.Arch);
    llvm::LLVMContext Context;
    auto Module = emit(F, Image, Context);
    ASSERT_TRUE(Module);
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, {}));
    EXPECT_NE(Source.find("no recovered machine state"), std::string::npos)
        << Source;
  }
}

TEST(MedLLVMEntryParameters, CallClobbersDoNotRegainEntryBindings) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  auto F = entryJoin(Image.Arch, 8, 8, false);
  auto &Call = F.Blocks[1].Ops.front();
  const auto Clobbered = Call.Output;
  Call = MedOp();
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(0x2000, 8));
  Call.CallSiteId = 1;
  Call.CalleeRegisterArgs = Call.CalleeStackArgs = Call.CalleeVectorArgs = 0;
  F.CallClobbers.push_back({Clobbered, 1});
  llvm::LLVMContext Context;
  auto Module = emit(F, Image, Context);
  ASSERT_TRUE(Module);
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, {}));
  EXPECT_NE(Source.find("no recovered machine state"), std::string::npos)
      << Source;
}

TEST(MedLLVMEntryParameters, HighCDoesNotInventUncoveredEntryBits) {
  for (auto Architecture : {Arch::AArch64, Arch::X64, Arch::ARM}) {
    SCOPED_TRACE(unsigned(Architecture));
    const auto Bytes = getTargetRegInfo(Architecture).PointerSize;
    auto F = entryJoin(Architecture, Bytes / 2, Bytes, false);
    auto High = MedToHighConverter().convert(F, Architecture);
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    ASSERT_TRUE(HighCEmitter().emit({High}, Output, {}));
    EXPECT_NE(Source.find("__builtin_trap()"), std::string::npos) << Source;
    for (llvm::StringRef Level : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          Source + "\nint main(void) { return entry_join(1, 0) != 0x73; }\n",
          {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
}

TEST(MedLLVMEntryParameters, KnownLowBytesSurviveWiderIncomingCarriers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64, Arch::ARM}) {
    const auto Bytes = getTargetRegInfo(Architecture).PointerSize;
    const uint16_t KnownBytes = Bytes / 2;
    auto F = entryJoin(Architecture, KnownBytes, Bytes, false);
    projectLowReturn(F, KnownBytes);
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(testing::Message() << unsigned(Architecture) << ':' << LLVM);
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      if (LLVM) {
        BinaryImage Image;
        Image.Arch = Architecture;
        Image.Format = BinaryFormat::ELF;
        Image.Bits = Bytes == 8 ? Bitness::Bits64 : Bitness::Bits32;
        llvm::LLVMContext Context;
        auto Module = emit(F, Image, Context);
        ASSERT_TRUE(Module);
        CEmitterOptions Options;
        Options.PreserveLLVMFunctionTypes = true;
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, Options));
      } else {
        auto High = MedToHighConverter().convert(F, Architecture);
        ASSERT_TRUE(HighCEmitter().emit({High}, Output, {}));
      }
      ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
      Source += "\nint main(void) {\n"
                "const uint64_t mask = UINT64_MAX >> " +
                std::to_string(64 - KnownBytes * 8) + ";\n" +
                R"c(
  uint64_t value = UINT64_MAX;
  for (unsigned i = 0; i < 1024; ++i) {
    if (entry_join(value, 1) != (value & mask)) return 1;
    if (entry_join(value, 0) != 0x73) return 2;
    value = value * UINT64_C(6364136223846793005) + 1;
  }
  return 0;
}
)c";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
  }
}

TEST(MedLLVMEntryParameters, FloatingLowLanesSurviveWiderCarriers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (uint16_t Bytes : {4, 8}) {
      auto F = entryJoin(Architecture, Bytes, 16, false);
      F.TypedParams = {{"value", NdType::makeFloat(Bytes)},
                       {"condition", NdType::makeInt(Bytes, false)}};
      const auto Register = getTargetRegInfo(Architecture)
                                .floatingParamRegs(BinaryFormat::ELF)
                                .front();
      F.Params[0].RegOff = Register;
      auto &Incoming = F.Blocks[2].Phis[0].Args[0].second;
      Incoming.RegOff = Register;
      Incoming.Id = 12;
      projectLowReturn(F, Bytes);
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << unsigned(Architecture) << ':' << Bytes << ':' << LLVM);
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        if (LLVM) {
          BinaryImage Image;
          Image.Arch = Architecture;
          Image.Format = BinaryFormat::ELF;
          Image.Bits = Bitness::Bits64;
          llvm::LLVMContext Context;
          auto Module = emit(F, Image, Context);
          ASSERT_TRUE(Module);
          CEmitterOptions Options;
          Options.PreserveLLVMFunctionTypes = true;
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, Options));
        } else {
          auto High = MedToHighConverter().convert(F, Architecture);
          ASSERT_TRUE(HighCEmitter().emit({High}, Output, {}));
        }
        ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
        Source += "\nint main(void) {\nconst uint" + std::to_string(Bytes * 8) +
                  "_t bits[] = {";
        Source += Bytes == 4 ? "0, 0x80000000U, 0x7f800000U, 0x7fc12345U, 1, "
                               "0xff800000U};\n"
                             : "0, UINT64_C(0x8000000000000000), "
                               "UINT64_C(0x7ff0000000000000), "
                               "UINT64_C(0x7ff8000012345678), 1, "
                               "UINT64_C(0xfff0000000000000)};\n";
        Source +=
            "for (unsigned i = 0; i < sizeof(bits)/sizeof(*bits); ++i) {\n";
        // Raw x64 LLVM uses the full physical vector argument. HighC and
        // scalar AArch64 LLVM expose the logical floating-point argument.
        if (LLVM && Architecture == Arch::X64)
          Source += "uint64_t __attribute__((vector_size(16))) value = "
                    "{UINT64_MAX, UINT64_MAX};\n";
        else
          Source += Bytes == 4 ? "float value;\n" : "double value;\n";
        Source += R"c(
    __builtin_memcpy(&value, &bits[i], sizeof(bits[i]));
    if (entry_join(value, 1) != bits[i]) return 1;
    if (entry_join(value, 0) != 0x73) return 2;
  }
  return 0;
}
)c";
        for (llvm::StringRef Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
    }
}

TEST(MedLLVMEntryParameters, PartialIncomingCopiesWaitForAnObservation) {
  auto F = entryJoin(Arch::AArch64, 4, 8, false);
  auto Observe = F.Params[1];
  Observe.Id = 2;
  Observe.RegOff = getTargetRegInfo(Arch::AArch64).IntParamRegs[2];
  F.Params.push_back(Observe);
  F.Blocks.resize(5);
  const auto Initial = F.Blocks[2].Phis.front().Output;
  auto Final = Initial;
  Final.SSAVer = 2;
  F.Blocks[2].Succs = {3, 4};
  F.Blocks[2].Ops.clear();
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(0x2000, 8));
  Call.CalleeRegisterArgs = Call.CalleeStackArgs = Call.CalleeVectorArgs = 0;
  F.Blocks[2].Ops.push_back(Call);
  MedOp Branch;
  Branch.Opcode = NdOp::COND_BR;
  Branch.addInput(MedVar::makeConst(0x1400, 8));
  Branch.addInput(Observe);
  F.Blocks[2].Ops.push_back(Branch);
  F.Blocks[3].Id = 3;
  F.Blocks[3].StartAddr = 0x1300;
  F.Blocks[3].Preds = {2};
  F.Blocks[3].Succs = {4};
  Branch = MedOp();
  Branch.Opcode = NdOp::BRANCH;
  Branch.addInput(MedVar::makeConst(0x1400, 8));
  F.Blocks[3].Ops.push_back(Branch);
  F.Blocks[4].Id = 4;
  F.Blocks[4].StartAddr = 0x1400;
  F.Blocks[4].Preds = {2, 3};
  PhiNode Result;
  Result.Output = Final;
  Result.Args = {{2, Initial}, {3, MedVar::makeConst(0x29, 8)}};
  F.Blocks[4].Phis.push_back(Result);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Final);
  F.Blocks[4].Ops.push_back(Return);
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    if (LLVM) {
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Format = BinaryFormat::ELF;
      Image.Bits = Bitness::Bits64;
      llvm::LLVMContext Context;
      auto Module = emit(F, Image, Context);
      ASSERT_TRUE(Module);
      CEmitterOptions Options;
      Options.PreserveLLVMFunctionTypes = true;
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, Options));
    } else {
      auto High = MedToHighConverter().convert(F, Arch::AArch64);
      ASSERT_TRUE(HighCEmitter().emit({High}, Output, {}));
    }
    ASSERT_NE(Source.find("__builtin_trap"), std::string::npos) << Source;
    Source += R"c(
static volatile unsigned ticks;
__typeof__(sub_2000()) sub_2000(void) { ++ticks; return 0; }
int main(void) {
  if (entry_join(19, 1, 0) != 0x29 || ticks != 1) return 1;
  if (entry_join(19, 0, 0) != 0x29 || ticks != 2) return 2;
  if (entry_join(19, 0, 1) != 0x73 || ticks != 3) return 3;
  return 0;
}
)c";
    for (llvm::StringRef Level : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
}

TEST(MedLLVMEntryParameters, PartialCarrierKeepsIndependentLowObservations) {
  llvm::LLVMContext Context;
  llvm::Module Module("partial-carrier-observations", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Half = llvm::Type::getInt32Ty(Context);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Half, llvm::Type::getInt1Ty(Context)},
                              false),
      llvm::GlobalValue::ExternalLinkage, "partial_slice", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
  auto *Whole = llvm::BasicBlock::Create(Context, "whole", F);
  auto *Low = llvm::BasicBlock::Create(Context, "low", F);
  llvm::IRBuilder<> B(Entry);
  auto *Unknown = llvm_source_unknown::create(B, Half);
  auto *Carrier = B.CreateOr(B.CreateZExt(F->getArg(0), Word),
                             B.CreateShl(B.CreateZExt(Unknown, Word), 32));
  B.CreateCondBr(F->getArg(1), Whole, Low);
  B.SetInsertPoint(Whole);
  B.CreateRet(Carrier);
  B.SetInsertPoint(Low);
  B.CreateRet(B.CreateZExt(B.CreateTrunc(Carrier, Half), Word));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Output, Options));
  ASSERT_NE(Source.find("__builtin_trap"), std::string::npos) << Source;
  Source += R"c(
int main(void) {
  uint32_t value = UINT32_MAX;
  for (unsigned i = 0; i < 1024; ++i) {
    if (partial_slice(value, 0) != value) return 1;
    value = value * UINT32_C(1664525) + UINT32_C(1013904223);
  }
  return 0;
}
)c";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(
        Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
}

TEST(MedLLVMEntryParameters, UnobservedPartialCopyCycleKeepsOnlyItsEffects) {
  for (bool Keep : {false, true}) {
    SCOPED_TRACE(Keep);
    llvm::LLVMContext Context;
    llvm::Module Module("unobserved-partial-copy-cycle", Context);
    Module.setDataLayout("e-p:64:64");
    auto *Half = llvm::Type::getInt32Ty(Context);
    auto *Word = llvm::Type::getInt128Ty(Context);
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(Half, {llvm::Type::getFloatTy(Context), Half},
                                false),
        llvm::GlobalValue::ExternalLinkage, "unused_partial", Module);
    auto *Effect = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "record_effect", Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
    auto *Loop = llvm::BasicBlock::Create(Context, "loop", F);
    auto *Exit = llvm::BasicBlock::Create(Context, "exit", F);
    llvm::IRBuilder<> B(Entry);
    auto *Bits = B.CreateBitCast(F->getArg(0), Half);
    auto *Unknown =
        llvm_source_unknown::create(B, llvm::IntegerType::get(Context, 96));
    auto *Carrier = B.CreateOr(B.CreateZExt(Bits, Word),
                               B.CreateShl(B.CreateZExt(Unknown, Word), 32));
    B.CreateBr(Loop);
    B.SetInsertPoint(Loop);
    auto *Copy = B.CreatePHI(Word, 2);
    auto *Index = B.CreatePHI(Half, 2);
    Copy->addIncoming(Carrier, Entry);
    Copy->addIncoming(Copy, Loop);
    Index->addIncoming(B.getInt32(0), Entry);
    B.CreateCall(Effect);
    auto *Next = B.CreateAdd(Index, B.getInt32(1));
    Index->addIncoming(Next, Loop);
    B.CreateCondBr(B.CreateICmpULT(Next, F->getArg(1)), Loop, Exit);
    B.SetInsertPoint(Exit);
    B.CreateRet(Bits);
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = Keep;
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Output, Options));
    Source += R"c(
static volatile unsigned ticks;
void record_effect(void) { ++ticks; }
int main(void) {
  const uint32_t values[] = {0, UINT32_C(0x80000000), UINT32_C(0x3f800000),
      UINT32_C(0x7f800000), UINT32_C(0x7fc12345), UINT32_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(*values); ++i) {
    float value;
    __builtin_memcpy(&value, &values[i], sizeof(value));
    for (unsigned count = 0; count != 8; ++count) {
      ticks = 0;
      if (unused_partial(value, count) != values[i]) return 1;
      if (ticks != (count ? count : 1)) return 2;
    }
  }
  return 0;
}
)c";
    for (llvm::StringRef Level : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
}

TEST(MedLLVMEntryParameters, CallClobbersTrapOnlyAtAnObservedUse) {
  llvm::LLVMContext Context;
  llvm::Module Module("clobber-transport", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Bit = llvm::Type::getInt1Ty(Context);
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Wide = llvm::Type::getInt128Ty(Context);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Bit, Bit, Word, Word}, false),
      llvm::GlobalValue::ExternalLinkage, "clobber_transport", Module);
  auto *Effect = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
      llvm::GlobalValue::ExternalLinkage, "record_effect", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
  auto *Known = llvm::BasicBlock::Create(Context, "known", F);
  auto *Unknown = llvm::BasicBlock::Create(Context, "unknown", F);
  // The transport chain exceeds the optional per-query dead-copy budget.
  // Exhausting that proof may keep copies, but cannot make a copy observe.
  std::vector<llvm::BasicBlock *> Blocks;
  for (unsigned I = 0; I < 96; ++I)
    Blocks.push_back(llvm::BasicBlock::Create(Context, "copy", F));
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", F);
  auto *Observe = llvm::BasicBlock::Create(Context, "observe", F);
  auto *Ignore = llvm::BasicBlock::Create(Context, "ignore", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(F->getArg(0), Known, Unknown);
  B.SetInsertPoint(Known);
  auto *KnownValue = B.CreateZExt(F->getArg(2), Wide);
  B.CreateBr(Blocks.front());
  B.SetInsertPoint(Unknown);
  auto *Clobber = B.CreateFreeze(llvm::UndefValue::get(Wide),
                                 "vector_call_clobber_unknown");
  B.CreateBr(Blocks.front());
  B.SetInsertPoint(Blocks.front());
  auto *Seed = B.CreatePHI(Wide, 3);
  Seed->addIncoming(KnownValue, Known);
  Seed->addIncoming(Clobber, Unknown);
  auto *Index = B.CreatePHI(Word, 3);
  Index->addIncoming(B.getInt64(0), Known);
  Index->addIncoming(B.getInt64(0), Unknown);
  B.CreateCall(Effect);
  auto *Next = B.CreateAdd(Index, B.getInt64(1));
  llvm::Value *Last = Seed;
  for (unsigned I = 0; I < Blocks.size(); ++I) {
    if (I) {
      B.SetInsertPoint(Blocks[I]);
      auto *Copy = B.CreatePHI(Wide, 1);
      Copy->addIncoming(Last, Blocks[I - 1]);
      Last = Copy;
    }
    if (I + 1 < Blocks.size())
      B.CreateBr(Blocks[I + 1]);
  }
  Seed->addIncoming(Last, Blocks.back());
  Index->addIncoming(Next, Blocks.back());
  B.CreateCondBr(B.CreateICmpULT(Next, F->getArg(3)), Blocks.front(), Exit);
  B.SetInsertPoint(Exit);
  B.CreateCondBr(F->getArg(1), Observe, Ignore);
  B.SetInsertPoint(Observe);
  B.CreateRet(B.CreateTrunc(Last, Word));
  B.SetInsertPoint(Ignore);
  B.CreateRet(B.getInt64(7));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Output, Options));
  Source += R"c(
#include <signal.h>
#include <stdlib.h>
static volatile unsigned ticks;
void record_effect(void) { ++ticks; }
static void expected_trap(int sig) { _Exit(0); }
int main(void) {
  for (unsigned count = 0; count < 8; ++count) {
    ticks = 0;
    if (clobber_transport(0, 0, 123, count) != 7) return 1;
    if (ticks != (count ? count : 1)) return 2;
    ticks = 0;
    if (clobber_transport(1, 1, UINT64_MAX - count, count) != UINT64_MAX - count)
      return 3;
    if (ticks != (count ? count : 1)) return 4;
  }
  signal(SIGILL, expected_trap);
#ifdef SIGTRAP
  signal(SIGTRAP, expected_trap);
#endif
  clobber_transport(0, 1, 123, 3);
  return 5; /* An actual read of the missing value must still fail. */
}
)c";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(Source, {Level});
}

TEST(MedLLVMEntryParameters, MaskedUnknownCopiesWaitForTheirUnknownBits) {
  for (bool Clobber : {false, true}) {
    SCOPED_TRACE(Clobber);
    llvm::LLVMContext Context;
    llvm::Module Module("masked-unknown-transport", Context);
    Module.setDataLayout("e-p:64:64");
    auto *Word = llvm::Type::getInt64Ty(Context);
    auto *Wide = llvm::Type::getInt128Ty(Context);
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(Word, {Word}, false),
        llvm::GlobalValue::ExternalLinkage, "masked_unknown", Module);
    auto *Effect = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "record_effect", Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
    auto *Loop = llvm::BasicBlock::Create(Context, "loop", F);
    auto *Exit = llvm::BasicBlock::Create(Context, "exit", F);
    auto *Observe = llvm::BasicBlock::Create(Context, "observe", F);
    auto *Ignore = llvm::BasicBlock::Create(Context, "ignore", F);
    llvm::IRBuilder<> B(Entry);
    llvm::Value *Unknown = Clobber
                               ? B.CreateFreeze(llvm::UndefValue::get(Wide),
                                                "vector_call_clobber_unknown")
                               : llvm_source_unknown::create(B, Wide);
    auto *Masked = B.CreateAnd(
        Unknown,
        llvm::ConstantInt::get(Wide, llvm::APInt::getHighBitsSet(128, 64)));
    B.CreateBr(Loop);
    B.SetInsertPoint(Loop);
    auto *Copy = B.CreatePHI(Wide, 2);
    Copy->addIncoming(Masked, Entry);
    Copy->addIncoming(Copy, Loop);
    auto *Index = B.CreatePHI(Word, 2);
    Index->addIncoming(B.getInt64(0), Entry);
    B.CreateCall(Effect);
    auto *Next = B.CreateAdd(Index, B.getInt64(1));
    Index->addIncoming(Next, Loop);
    B.CreateCondBr(B.CreateICmpULT(Next, B.getInt64(4)), Loop, Exit);
    B.SetInsertPoint(Exit);
    B.CreateCondBr(B.CreateICmpNE(F->getArg(0), B.getInt64(0)), Observe,
                   Ignore);
    B.SetInsertPoint(Observe);
    B.CreateRet(B.CreateTrunc(B.CreateLShr(Copy, 64), Word));
    B.SetInsertPoint(Ignore);
    B.CreateRet(B.CreateTrunc(Copy, Word));
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
    Source += R"c(
#include <signal.h>
#include <stdlib.h>
static volatile unsigned ticks;
void record_effect(void) { ++ticks; }
static void expected_trap(int sig) { _Exit(ticks == 8 ? 0 : 3); }
int main(void) {
  if (masked_unknown(0) != 0 || ticks != 4) return 1;
  signal(SIGILL, expected_trap);
#ifdef SIGTRAP
  signal(SIGTRAP, expected_trap);
#endif
  masked_unknown(1);
  return 2;
}
)c";
    for (llvm::StringRef Level : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(Source, {Level});
  }
}

TEST(MedLLVMEntryParameters, DeadUnknownTransportHasStableSource) {
  llvm::LLVMContext Context;
  llvm::Module Module("dead-transport-order", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *F = llvm::Function::Create(llvm::FunctionType::get(Word, {Word}, false),
                                   llvm::GlobalValue::ExternalLinkage,
                                   "dead_transport_order", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", F);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", F);
  llvm::IRBuilder<> B(Entry);
  auto *Unknown = llvm_source_unknown::create(B, Word);
  B.CreateBr(Loop);
  B.SetInsertPoint(Loop);
  std::vector<llvm::PHINode *> Phis;
  for (unsigned I = 0; I < 96; ++I) {
    auto *Phi = B.CreatePHI(Word, 2, "transport");
    Phi->addIncoming(I ? static_cast<llvm::Value *>(B.getInt64(I)) : Unknown,
                     Entry);
    Phis.push_back(Phi);
  }
  // A long dead tail feeds a short dead cycle. Proof work may retain some
  // copies, but allocation order must not decide which source is emitted.
  for (unsigned I = 0; I < Phis.size(); ++I)
    Phis[I]->addIncoming(Phis[I ? I - 1 : 7], Loop);
  auto *Index = B.CreatePHI(Word, 2, "index");
  Index->addIncoming(B.getInt64(0), Entry);
  auto *Next = B.CreateAdd(Index, B.getInt64(1));
  Index->addIncoming(Next, Loop);
  B.CreateCondBr(B.CreateICmpULT(Next, F->getArg(0)), Loop, Exit);
  B.SetInsertPoint(Exit);
  B.CreateRet(Next);
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Expected;
  for (unsigned I = 0; I < 24; ++I) {
    SCOPED_TRACE(I);
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
    if (I == 0)
      Expected = Source;
    else
      EXPECT_EQ(Source, Expected);
  }
  Expected += "\nint main(void) { return dead_transport_order(8) != 8; }\n";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(Expected, {Level});
}

TEST(MedLLVMEntryParameters, UnreachableUnknownStoresStayOutsideSource) {
  llvm::LLVMContext Context;
  llvm::Module Module("unreachable-unknown-storage", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *F = llvm::Function::Create(llvm::FunctionType::get(Word, {Word}, false),
                                   llvm::GlobalValue::ExternalLinkage,
                                   "reachable_value", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
  auto *Read = llvm::BasicBlock::Create(Context, "read", F);
  auto *Known = llvm::BasicBlock::Create(Context, "known", F);
  auto *Dead = llvm::BasicBlock::Create(Context, "dead_ring", F);
  auto *Back = llvm::BasicBlock::Create(Context, "dead_backedge", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(B.CreateICmpNE(F->getArg(0), B.getInt64(0)), Read, Known);
  B.SetInsertPoint(Read);
  B.CreateRet(llvm_source_unknown::create(B, Word));
  B.SetInsertPoint(Known);
  B.CreateRet(B.getInt64(7));
  for (auto *Block : {Dead, Back}) {
    B.SetInsertPoint(Block);
    B.CreateStore(llvm_source_unknown::create(B, Word),
                  llvm::ConstantPointerNull::get(B.getPtrTy()), true);
    B.CreateBr(Block == Dead ? Back : Dead);
  }
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
  // Projection may remove only unreachable blocks in its private clone.
  EXPECT_EQ(F->size(), 5u);
  EXPECT_EQ(Source.find("dead_ring"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("dead_backedge"), std::string::npos) << Source;
  EXPECT_NE(Source.find("no recovered machine state"), std::string::npos)
      << Source;
  Source += R"c(
#include <signal.h>
#include <stdlib.h>
static void expected_trap(int sig) { _Exit(0); }
int main(void) {
  if (reachable_value(0) != 7) return 1;
  signal(SIGILL, expected_trap);
#ifdef SIGTRAP
  signal(SIGTRAP, expected_trap);
#endif
  reachable_value(1);
  return 2;
}
)c";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(Source, {Level});
}

TEST(MedLLVMEntryParameters, PartialPhiKeepsLowBitsAndWholeAvailability) {
  llvm::LLVMContext Context;
  llvm::Module Module("partial-phi-observations", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Half = llvm::Type::getInt32Ty(Context);
  auto *Bit = llvm::Type::getInt1Ty(Context);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Half, Bit, Bit}, false),
      llvm::GlobalValue::ExternalLinkage, "partial_join", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
  auto *Partial = llvm::BasicBlock::Create(Context, "partial", F);
  auto *Known = llvm::BasicBlock::Create(Context, "known", F);
  auto *Join = llvm::BasicBlock::Create(Context, "join", F);
  auto *Whole = llvm::BasicBlock::Create(Context, "whole", F);
  auto *Low = llvm::BasicBlock::Create(Context, "low", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(F->getArg(1), Partial, Known);
  B.SetInsertPoint(Partial);
  auto *Unknown = llvm_source_unknown::create(B, Half);
  auto *Carrier = B.CreateOr(B.CreateZExt(F->getArg(0), Word),
                             B.CreateShl(B.CreateZExt(Unknown, Word), 32));
  B.CreateBr(Join);
  B.SetInsertPoint(Known);
  B.CreateBr(Join);
  B.SetInsertPoint(Join);
  auto *Value = B.CreatePHI(Word, 2);
  Value->addIncoming(Carrier, Partial);
  Value->addIncoming(B.getInt64(UINT64_C(0xfedcba9876543210)), Known);
  B.CreateCondBr(F->getArg(2), Whole, Low);
  B.SetInsertPoint(Whole);
  B.CreateRet(Value);
  B.SetInsertPoint(Low);
  B.CreateRet(B.CreateZExt(B.CreateTrunc(Value, Half), Word));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Output, Options));
  ASSERT_NE(Source.find("__builtin_trap"), std::string::npos) << Source;
  Source += R"c(
#include <signal.h>
#include <stdlib.h>
static void expected_unknown(int code) { (void)code; _Exit(0); }
int main(void) {
  uint32_t value = UINT32_MAX;
  for (unsigned i = 0; i < 1024; ++i) {
    if (partial_join(value, 1, 0) != value) return 1;
    if (partial_join(value, 0, 0) != UINT32_C(0x76543210)) return 2;
    if (partial_join(value, 0, 1) != UINT64_C(0xfedcba9876543210)) return 3;
    value = value * UINT32_C(1664525) + UINT32_C(1013904223);
  }
  if (signal(SIGILL, expected_unknown) == SIG_ERR) return 8;
#ifdef SIGTRAP
  if (signal(SIGTRAP, expected_unknown) == SIG_ERR) return 8;
#endif
  (void)partial_join(value, 1, 1);
  return 9;
}
)c";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(
        Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
}

TEST(MedLLVMEntryParameters, LiteralXorSelectPreservesItsKnownLowWord) {
  llvm::LLVMContext Context;
  llvm::Module Module("selected-words", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Half = llvm::Type::getInt32Ty(Context);
  auto *Bit = llvm::Type::getInt1Ty(Context);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Half, Bit, Bit}, false),
      llvm::GlobalValue::ExternalLinkage, "selected_bits", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
  auto *Whole = llvm::BasicBlock::Create(Context, "whole", F);
  auto *Low = llvm::BasicBlock::Create(Context, "low", F);
  llvm::IRBuilder<> B(Entry);
  auto *Missing = llvm_source_unknown::create(B, Half);
  auto *Partial = B.CreateOr(B.CreateZExt(F->getArg(0), Word),
                             B.CreateShl(B.CreateZExt(Missing, Word), 32));
  auto *Selected = B.CreateSelect(F->getArg(1), B.CreateXor(Partial, 3),
                                  B.getInt64(UINT64_C(0xa5710045efde0310)));
  B.CreateCondBr(F->getArg(2), Whole, Low);
  B.SetInsertPoint(Whole);
  B.CreateRet(Selected);
  B.SetInsertPoint(Low);
  B.CreateRet(B.CreateZExt(B.CreateTrunc(Selected, Half), Word));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source(
      "#include <stdlib.h>\nstatic int expect_missing;\n"
      "#define __builtin_trap() _Exit(expect_missing ? 0 : 3)\n");
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  Source += R"(
int main(void) {
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t x = i * UINT32_C(0xe5710001);
    if (selected_bits(x, 1, 0) != (x ^ 3) ||
        selected_bits(x, 0, 0) != UINT32_C(0xefde0310) ||
        selected_bits(x, 0, 1) != UINT64_C(0xa5710045efde0310)) return 1;
  }
  expect_missing = 1;
  (void)selected_bits(19, 1, 1);
  return 2;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(Source, {Level});
}

TEST(MedLLVMEntryParameters, PartialPhiLoopsKeepCopiesAndEffects) {
  for (bool Keep : {false, true}) {
    SCOPED_TRACE(Keep);
    llvm::LLVMContext Context;
    llvm::Module Module("partial-phi-loop", Context);
    Module.setDataLayout("e-p:64:64");
    auto *Word = llvm::Type::getInt64Ty(Context);
    auto *Half = llvm::Type::getInt32Ty(Context);
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(
            Word, {Half, Half, llvm::Type::getInt1Ty(Context)}, false),
        llvm::GlobalValue::ExternalLinkage, "partial_loop", Module);
    auto *Tick = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {}, false),
        llvm::GlobalValue::ExternalLinkage, "tick", Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
    auto *Header = llvm::BasicBlock::Create(Context, "header", F);
    auto *Body = llvm::BasicBlock::Create(Context, "body", F);
    auto *Exit = llvm::BasicBlock::Create(Context, "exit", F);
    auto *Whole = llvm::BasicBlock::Create(Context, "whole", F);
    auto *Low = llvm::BasicBlock::Create(Context, "low", F);
    llvm::IRBuilder<> B(Entry);
    auto *Unknown = llvm_source_unknown::create(B, Half);
    auto *Carrier = B.CreateOr(B.CreateZExt(F->getArg(0), Word),
                               B.CreateShl(B.CreateZExt(Unknown, Word), 32));
    B.CreateBr(Header);
    B.SetInsertPoint(Header);
    auto *Value = B.CreatePHI(Word, 2);
    auto *Count = B.CreatePHI(Half, 2);
    Value->addIncoming(Carrier, Entry);
    Count->addIncoming(F->getArg(1), Entry);
    B.CreateCondBr(B.CreateICmpNE(Count, B.getInt32(0)), Body, Exit);
    B.SetInsertPoint(Body);
    B.CreateCall(Tick);
    auto *Remaining = B.CreateSub(Count, B.getInt32(1));
    Value->addIncoming(Keep ? static_cast<llvm::Value *>(Value)
                            : B.getInt64(UINT64_C(0xfedcba9876543210)),
                       Body);
    Count->addIncoming(Remaining, Body);
    B.CreateBr(Header);
    B.SetInsertPoint(Exit);
    B.CreateCondBr(F->getArg(2), Whole, Low);
    B.SetInsertPoint(Whole);
    B.CreateRet(Value);
    B.SetInsertPoint(Low);
    B.CreateRet(B.CreateZExt(B.CreateTrunc(Value, Half), Word));
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Output, Options));
    ASSERT_NE(Source.find("__builtin_trap"), std::string::npos) << Source;
    Source += Keep ? "\n#define KEEP 1\n" : "\n#define KEEP 0\n";
    Source += R"c(
#include <signal.h>
#include <stdlib.h>
static volatile unsigned ticks;
void tick(void) { ++ticks; }
static void expected_unknown(int code) { (void)code; _Exit(0); }
int main(void) {
  uint32_t value = UINT32_MAX;
  for (unsigned count = 0; count < 32; ++count) {
    ticks = 0;
    const uint64_t expected = KEEP || count == 0 ? value : UINT32_C(0x76543210);
    if (partial_loop(value, count, 0) != expected || ticks != count) return 1;
    if (!KEEP && count) {
      ticks = 0;
      if (partial_loop(value, count, 1) != UINT64_C(0xfedcba9876543210) || ticks != count) return 2;
    }
    value = value * UINT32_C(1664525) + UINT32_C(1013904223);
  }
  if (signal(SIGILL, expected_unknown) == SIG_ERR) return 8;
#ifdef SIGTRAP
  if (signal(SIGTRAP, expected_unknown) == SIG_ERR) return 8;
#endif
  (void)partial_loop(value, 0, 1);
  return 9;
}
)c";
    for (llvm::StringRef Level : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
}

TEST(MedLLVMEntryParameters, HighCPartialJoinKeepsIndependentLowObservations) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (bool Reversed : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << unsigned(Architecture) << ':' << Reversed);
      auto F = entryJoin(Architecture, 4, 8, false);
      F.Blocks[1].Ops.front().Inputs[0] =
          MedVar::makeConst(UINT64_C(0xfedcba9876543210), 8);
      auto Condition = F.Params.back();
      Condition.Id = 2;
      Condition.RegOff = getTargetRegInfo(Architecture).IntParamRegs[2];
      F.Params.push_back(Condition);
      F.Blocks.resize(5);
      F.Blocks[2].Succs = {3, 4};
      F.Blocks[2].Phis.front().Output.RegOff =
          getTargetRegInfo(Architecture).IntReturnReg;
      auto Value = F.Blocks[2].Phis.front().Output;
      F.Blocks[2].Ops.clear();
      MedOp Branch;
      Branch.Opcode = NdOp::COND_BR;
      Branch.addInput(MedVar::makeConst(0x1400, 8));
      Branch.addInput(Condition);
      F.Blocks[2].Ops.push_back(Branch);
      for (unsigned I : {3u, 4u}) {
        F.Blocks[I].Id = I;
        F.Blocks[I].StartAddr = 0x1000 + I * 0x100;
        F.Blocks[I].Preds = {2};
      }
      MedVar Low = Value;
      Low.Id = 26;
      Low.SSAVer = 2;
      Low.Size = 4;
      auto Wide = Low;
      Wide.Id = 27;
      Wide.SSAVer = 3;
      Wide.Size = 8;
      MedOp Slice;
      Slice.Opcode = NdOp::SUBBYTES;
      Slice.Output = Low;
      Slice.addInput(Value);
      Slice.addInput(MedVar::makeConst(0, 4));
      F.Blocks[3].Ops.push_back(Slice);
      MedOp Extend;
      Extend.Opcode = NdOp::INT_ZEXT;
      Extend.Output = Wide;
      Extend.addInput(Low);
      F.Blocks[3].Ops.push_back(Extend);
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.addInput(Wide);
      F.Blocks[3].Ops.push_back(Return);
      Return.Inputs[0] = Value;
      F.Blocks[4].Ops.push_back(Return);
      if (Reversed) {
        std::reverse(F.Blocks[2].Phis[0].Args.begin(),
                     F.Blocks[2].Phis[0].Args.end());
        std::reverse(F.Blocks.begin() + 1, F.Blocks.end());
        // MedIR edges index the block vector. Preserve that contract while
        // changing layout; branch operands remain machine addresses.
        const auto Reindex = [&](int Id) {
          return Id == 0 ? 0 : static_cast<int>(F.Blocks.size()) - Id;
        };
        for (auto &Block : F.Blocks) {
          Block.Id = Reindex(Block.Id);
          for (auto &Pred : Block.Preds)
            Pred = Reindex(Pred);
          for (auto &Succ : Block.Succs)
            Succ = Reindex(Succ);
          for (auto &Phi : Block.Phis)
            for (auto &[Pred, Input] : Phi.Args)
              Pred = Reindex(Pred);
        }
      }
      auto High = MedToHighConverter().convert(F, Architecture);
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({High}, Output, Options));
      ASSERT_NE(Source.find("__builtin_trap"), std::string::npos) << Source;
      Source += R"c(
#include <signal.h>
#include <stdlib.h>
static void expected_unknown(int code) { (void)code; _Exit(0); }
int main(void) {
  uint32_t value = UINT32_MAX;
  for (unsigned i = 0; i < 1024; ++i) {
    if ((uint64_t)entry_join(value, 1, 0) != value) return 1;
    if ((uint64_t)entry_join(value, 0, 0) != UINT32_C(0x76543210)) return 2;
    if ((uint64_t)entry_join(value, 0, 1) != UINT64_C(0xfedcba9876543210)) return 3;
    value = value * UINT32_C(1664525) + UINT32_C(1013904223);
  }
  if (signal(SIGILL, expected_unknown) == SIG_ERR) return 8;
#ifdef SIGTRAP
  if (signal(SIGTRAP, expected_unknown) == SIG_ERR) return 8;
#endif
  (void)entry_join(value, 1, 1);
  return 9;
}
)c";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
}

TEST(MedLLVMEntryParameters, PointerAndFloatingEntryViewsKeepTheirBits) {
  for (bool Floating : {false, true}) {
    SCOPED_TRACE(Floating);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::ELF;
    Image.Bits = Bitness::Bits64;
    auto F = entryJoin(Image.Arch, 8, 8, false);
    F.TypedParams = {{"value", Floating ? NdType::makeFloat(8)
                                        : NdType::makePtr(NdType::makeVoid())},
                     {"condition", NdType::makeInt(8, false)}};
    if (Floating) {
      const auto Register =
          getTargetRegInfo(Image.Arch).floatingParamRegs(Image.Format).front();
      F.Params[0].RegOff = Register;
      // Merge the incoming floating argument's bits into the integer return
      // register; the other arm and the RETURN retain their integer identity.
      auto &Incoming = F.Blocks[2].Phis[0].Args[0].second;
      Incoming.RegOff = Register;
      Incoming.Id = 12;
    }
    llvm::LLVMContext Context;
    auto Module = emit(F, Image, Context);
    ASSERT_TRUE(Module);
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, Options));
    ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
    Source += Floating ? R"c(
int main(void) {
  const uint64_t bits[] = {0, UINT64_C(0x8000000000000000),
      UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000012345678), 1,
      UINT64_C(0xfff0000000000000)};
  for (unsigned i = 0; i < sizeof(bits) / sizeof(*bits); ++i) {
    double value;
    __builtin_memcpy(&value, &bits[i], sizeof(value));
    if (entry_join(value, 1) != bits[i]) return 1;
    if (entry_join(value, 0) != 0x73) return 2;
  }
  return 0;
}
)c"
                       : R"c(
int main(void) {
  unsigned char storage[31];
  for (unsigned i = 0; i < sizeof(storage); ++i) {
    if (entry_join(storage + i, 1) != (uintptr_t)(storage + i)) return 1;
    if (entry_join(storage + i, 0) != 0x73) return 2;
  }
  return entry_join((void*)0, 1) != 0;
}
)c";
    for (llvm::StringRef Level : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
}

TEST(MedLLVMEntryParameters, MachineJoinKeepsBothCPaths) {
  // One path returns the incoming x0; the other defines version zero of x0.
  // Use actual instructions as well as the policy-level Med fixtures above.
  constexpr std::array<uint32_t, 3> Words{
      0x35000041, // cbnz w1, return
      0xd2800e60, // mov x0, #0x73
      0xd65f03c0  // return: ret
  };
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = Format;
    Image.Entry = 0x1000;
    Segment Code;
    Code.Name = ".text";
    Code.VA = Image.Entry;
    Code.Size = Code.FileSz = Words.size() * sizeof(uint32_t);
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data.resize(Code.Size);
    for (size_t I = 0; I < Words.size(); ++I)
      llvm::support::endian::write32le(Code.Data.data() + I * 4, Words[I]);
    Image.Segments.push_back(std::move(Code));
    Image.Symbols.push_back({"entry_join", Image.Entry, 12, true});
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(testing::Message() << unsigned(Format) << ':' << LLVM);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Image.Entry};
      Options.LiftMode = LLVM;
      Options.SourceProjection = LLVM;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions,
                                        nullptr, &Image));
      } else {
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
      }
      Source += R"c(
#if defined(__aarch64__)
__attribute__((naked, noinline))
static uint64_t original_entry(uint64_t value, uint64_t condition) {
  __asm__(".long 0x35000041\n.long 0xd2800e60\n.long 0xd65f03c0\n");
}
#endif
int main(void) {
  const uint64_t conditions[] = {0, 1, UINT64_C(0x100000000), UINT64_MAX};
  uint64_t value = UINT64_MAX;
  for (unsigned i = 0; i < 1024; ++i) {
    for (unsigned j = 0; j < sizeof(conditions) / sizeof(*conditions); ++j) {
      uint64_t expected = (uint32_t)conditions[j] ? value : 0x73;
      if ((uint64_t)entry_join(value, conditions[j]) != expected) return 1;
#if defined(__aarch64__)
      if (original_entry(value, conditions[j]) != expected) return 2;
#endif
    }
    value = value * UINT64_C(6364136223846793005) + 1;
  }
  return 0;
}
)c";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
  }
}
} // namespace
