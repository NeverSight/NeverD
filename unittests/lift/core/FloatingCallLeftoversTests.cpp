//===- FloatingCallLeftoversTests.cpp - Floating call setup evidence ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Call-clobber joins cannot introduce speculative floating arguments.

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>

namespace {
using namespace neverd;

const std::map<va_t, std::string> Names{{0x2000, "prepare_callback"},
                                        {0x3000, "consume_scalar"}};

MedFunc joinedCall(Arch Architecture, BinaryFormat Format, unsigned Variant,
                   bool Indirect) {
  const auto &TRI = getTargetRegInfo(Architecture);
  auto Value = [&](MedVar::VarKind Kind, int Id, uint16_t Size,
                   uint64_t Register = 0, int Version = 0) {
    MedVar V;
    V.Kind = Kind;
    V.Id = Id;
    V.Size = Size;
    V.RegOff = Register;
    V.SSAVer = Version;
    V.TheArch = Architecture;
    return V;
  };
  auto Op = [](NdOp Code, MedVar Output, std::initializer_list<MedVar> Inputs) {
    MedOp O;
    O.Opcode = Code;
    O.Output = Output;
    for (const auto &Input : Inputs)
      O.addInput(Input);
    return O;
  };
  MedFunc F;
  F.Entry = 0x1000;
  F.Name = "call_scalar";
  F.ReturnType = NdType::makeInt(8, false);
  F.Params = {Value(MedVar::Param, 0, 1)};
  if (Indirect)
    F.Params.push_back(Value(MedVar::Param, 1, 8));
  F.Blocks.resize(4);
  for (unsigned I = 0; I < F.Blocks.size(); ++I) {
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = F.Entry + I * 0x100;
  }
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[2].Preds = {0};
  F.Blocks[1].Succs = F.Blocks[2].Succs = {3};
  F.Blocks[3].Preds = {1, 2};
  const uint64_t Register = TRI.floatingParamRegs(Format).front();
  const auto First = Value(MedVar::Reg, 10, 16, Register, 1);
  const auto Second = Value(MedVar::Reg, 10, 16, Register, 2);
  const auto Joined = Value(MedVar::Reg, 10, 16, Register, 3);
  for (unsigned I = 0; I < 2; ++I) {
    auto Call = Op(NdOp::CALL, {}, {MedVar::makeConst(0x2000, 8)});
    Call.CallSiteId = I + 1;
    Call.CalleeRegisterArgs = 0;
    Call.CalleeStackArgs = 0;
    Call.CalleeVectorArgs = 0;
    F.Blocks[I].Ops.push_back(Call);
    F.CallClobbers.push_back({I ? Second : First, Call.CallSiteId});
  }
  F.Blocks[0].Ops.push_back(
      Op(NdOp::COND_BR, {}, {MedVar::makeConst(0x1200, 8), F.Params[0]}));
  F.Blocks[1].Ops.push_back(
      Op(NdOp::BRANCH, {}, {MedVar::makeConst(0x1300, 8)}));
  F.Blocks[2].Ops.push_back(
      Op(NdOp::BRANCH, {}, {MedVar::makeConst(0x1300, 8)}));
  PhiNode Phi;
  Phi.Output = Joined;
  Phi.Args = {{1, Second}, {2, First}};
  if (Variant == 1)
    Phi.Args[1].second = MedVar::makeConst(7, 16);
  if (Variant == 2) {
    F.CallClobbers[0].PreservedPrefixSize = 8;
    F.CallClobbers[0].PreservedInput = MedVar::makeConst(7, 16);
  }
  if (Variant == 3) {
    MedStructReturnCandidate Result;
    Result.CallSiteId = 1;
    Result.Fields = {First};
    F.StructReturnCandidates.push_back(Result);
  }
  if (Variant == 4)
    Phi.ExceptionalEntry = MedVar::makeConst(7, 16);
  F.Blocks[3].Phis.push_back(Phi);
  const auto IntegerRegisters = TRI.integerParamRegs(Format);
  for (unsigned I = 0; I != 2; ++I)
    F.Blocks[3].Ops.push_back(
        Op(NdOp::COPY, Value(MedVar::Reg, 30 + I, 8, IntegerRegisters[I], 1),
           {MedVar::makeConst(I ? 29 : 19, 8)}));
  auto Call =
      Op(Indirect ? NdOp::INDIR_CALL : NdOp::CALL, Value(MedVar::Temp, 20, 8),
         {Indirect ? F.Params[1] : MedVar::makeConst(0x3000, 8),
          MedVar::makeConst(19, 8), MedVar::makeConst(29, 8)});
  Call.CalleeRegisterArgs = 2;
  Call.CalleeStackArgs = 0;
  Call.CalleeVectorArgs = -1;
  F.Blocks[3].Ops.push_back(Call);
  F.Blocks[3].Ops.push_back(Op(NdOp::RETURN, {}, {Call.Output}));
  for (auto &Block : F.Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I)
      Block.Ops[I].Addr = Block.StartAddr + I * 4;
  return F;
}

TEST(FloatingCallLeftovers, OnlyEntirelyClobberedJoinsEndSpeculativeArguments) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Indirect : {false, true})
        for (unsigned Variant = 0; Variant != 5; ++Variant) {
          SCOPED_TRACE(testing::Message()
                       << unsigned(Architecture) << ':' << unsigned(Format)
                       << ':' << Indirect << ':' << Variant);
          BinaryImage Image;
          Image.Arch = Architecture;
          Image.Format = Format;
          Image.Bits = Bitness::Bits64;
          auto F = joinedCall(Architecture, Format, Variant, Indirect);
          for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
            recoverCallAbi(F, Architecture, Names, &Image);
            ASSERT_EQ(F.CallInfos.size(), 3u);
            const auto &Arguments = F.CallInfos.back().Args;
            ASSERT_EQ(Arguments.size(), Variant ? 3u : 2u);
            EXPECT_EQ(Arguments[0], MedVar::makeConst(19, 8));
            EXPECT_EQ(Arguments[1], MedVar::makeConst(29, 8));
          }
        }
}

TEST(FloatingCallLeftovers, KnownFloatingConsumersRetainUnavailableInputs) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  auto F = joinedCall(Image.Arch, Image.Format, 0, false);
  const std::map<va_t, int> Counts{{0x3000, 1}};
  recoverCallAbi(F, Image.Arch, Names, &Image, nullptr, nullptr, &Counts);
  ASSERT_EQ(F.CallInfos.size(), 3u);
  ASSERT_EQ(F.CallInfos.back().Args.size(), 3u);
  EXPECT_EQ(F.CallInfos.back().Args.back(), F.Blocks.back().Phis[0].Output);
}

TEST(FloatingCallLeftovers, FloatingReturnJoinsRemainArgumentsInAnyBlockOrder) {
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Format = BinaryFormat::ELF;
    Image.Bits = Bitness::Bits64;
    auto F = joinedCall(Image.Arch, Image.Format, 0, true);
    // Both producers are in the branch arms. The join may be visited before
    // either producer's floating result has been rebound from its clobber.
    F.Blocks[2].Ops.insert(F.Blocks[2].Ops.begin(), F.Blocks[0].Ops.front());
    F.Blocks[0].Ops.erase(F.Blocks[0].Ops.begin());
    for (auto &Block : F.Blocks)
      for (auto &Call : Block.Ops)
        if (Call.CallSiteId) {
          Call.Output.Kind = MedVar::Reg;
          Call.Output.Id = 50;
          Call.Output.SSAVer = Call.CallSiteId;
          Call.Output.RegOff = getTargetRegInfo(Image.Arch).IntReturnReg;
          Call.Output.Size = 8;
          Call.Output.TheArch = Image.Arch;
        }
    if (Reverse)
      std::swap(F.Blocks[1], F.Blocks[3]);
    const std::map<va_t, uint16_t> Results{{0x2000, 8}};
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      recoverCallAbi(F, Image.Arch, Names, &Image, nullptr, nullptr, nullptr,
                     &Results);
      ASSERT_EQ(F.CallInfos.size(), 3u);
      for (const auto &Call : F.CallInfos)
        if (Call.IsIndirect)
          EXPECT_EQ(Call.Args.size(), 3u);
    }
  }
}

TEST(FloatingCallLeftovers,
     BothSourceRoutesExecuteEveryBranchWithoutPhantomFP) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Format = Format;
    Image.Bits = Bitness::Bits64;
    auto F = joinedCall(Image.Arch, Format, 0, false);
    recoverCallAbi(F, Image.Arch, Names, &Image);
    ASSERT_EQ(F.CallInfos.back().Args.size(), 2u);
    llvm::LLVMContext Context;
    MedLLVMEmitter Emitter;
    Emitter.setPreserveUnknownSourceValues(true);
    auto Module = Emitter.emit({F}, Context, "call_leftovers", Image.Arch,
                               {Names.begin(), Names.end()}, &Image, Format);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    Pipeline::optimizeOrPromoteModule(*Module);
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(testing::Message() << unsigned(Format) << ':' << LLVM);
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      if (LLVM) {
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, {}));
      } else {
        MedToHighConverter Converter;
        Converter.setFuncNames(&Names);
        auto High = Converter.convert(F, Image.Arch);
        ASSERT_TRUE(HighCEmitter().emit({High}, Output));
      }
      EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
      const std::string Provider =
          R"(
#include <stdint.h>
static unsigned preparations, observations;
)" + std::string(LLVM ? "uint64_t" : "int") +
          " prepare_callback(void) { ++preparations; return 0; }\n" +
          std::string(LLVM ? "uint64_t consume_scalar(uint64_t a, uint64_t b)"
                           : "int64_t consume_scalar(int64_t a, int64_t b)") +
          R"( {
  ++observations;
  return a == 19 && b == 29 ? UINT64_C(0x123456789ABCDE) : 0;
}
)";
      const std::string Main = R"(
int main(void) {
  if (call_scalar(0) != UINT64_C(0x123456789ABCDE)) return 1;
  if (call_scalar(1) != UINT64_C(0x123456789ABCDE)) return 2;
  return observations != 2 || preparations != 3;
}
)";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Provider + Source + Main,
            {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
  }
}
} // namespace
