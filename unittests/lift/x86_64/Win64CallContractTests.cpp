//===- Win64CallContractTests.cpp - Microsoft x64 import arguments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// MSVC calls a C runtime import through its import address slot
// (`call [__imp_fputs]`); MinGW calls a thunk that jumps there
// (`jmp [__imp_fputs]`).  A wrapper that passes its own RCX and RDX on leaves
// them unwritten.  The import's prototype says it reads them, so they are the
// wrapper's parameters and the call's arguments.
//
//===----------------------------------------------------------------------===//

#include "../core/SourceCallExecution.h"
#include "../core/WindowsMixedScalarFixture.h"
#include "../core/WindowsQualifiedFixture.h"
#include "../core/WindowsVariadicFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/debug/UserPrototype.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

TEST(SummarizedCallArguments, PublishesAnEmptyVectorBankForScalarCallees) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    const auto &Registers = getTargetRegInfo(Arch::X64);
    const auto Integers = Registers.integerParamRegs(Format);
    const auto Vectors = Registers.floatingParamRegs(Format);
    for (unsigned Floating : {0u, 1u, 2u}) {
      SCOPED_TRACE(Floating);
      GPRReadWidths Reads{};
      Reads[Integers[0] / 8] = 8;
      if (Floating == 1)
        Reads[kX64VectorFamilyBase + 1] = 4;
      else
        Reads[Integers[1] / 8] = 1;
      if (Floating == 2)
        Reads[kX64VectorFamilyBase] = 4;
      const std::map<va_t, GPRReadWidths> Summaries{{0x2000, Reads}};
      LowFunc Low;
      Low.Entry = 0x1000;
      Low.Name = "forward_values";
      Low.Blocks.resize(1);
      Low.Blocks[0].Id = 0;
      Low.Blocks[0].StartAddr = Low.Entry;
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.Addr = Low.Entry;
      Call.Output = NdVar::reg(Registers.IntReturnReg, 8);
      Call.addInput(NdVar::cst(0x2000, 8));
      Low.Blocks[0].Ops.push_back(Call);
      LowOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.Addr = Low.Entry + 5;
      Return.addInput(Call.Output);
      Low.Blocks[0].Ops.push_back(Return);
      LowToMedConverter Converter;
      Converter.setCallEntryReadGPRs(&Summaries);
      const MedFunc Function = Converter.convert(Low, Arch::X64, Format);
      const MedOp *Bound = nullptr;
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL)
            Bound = &Op;
      ASSERT_NE(Bound, nullptr);
      if (!Floating) {
        EXPECT_EQ(Bound->CalleeRegisterArgs, 2);
        EXPECT_EQ(Bound->CalleeVectorArgs, 0);
        EXPECT_EQ(Bound->CalleeVectorSlots, 0);
        EXPECT_EQ(Bound->NumInputs, 3u);
      } else if (Floating == 1) {
        // The absence of a separate bank must not erase a positional float,
        // or a nonconsecutive vector register in the independent bank.
        ASSERT_EQ(Bound->NumInputs, 3u);
        EXPECT_EQ(Bound->Inputs[2].RegOff, Vectors[1]);
        EXPECT_EQ(Bound->vectorArgumentWidth(2), 4u);
      } else {
        // Simultaneous integer/vector reads of one positional slot are not
        // evidence of an empty bank, even without an ordinary C signature.
        EXPECT_NE(Bound->CalleeVectorArgs, 0);
      }
    }
  }
}

TEST(SummarizedCallArguments, ScalarCallsDoNotAcquireIncidentalVectorState) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool Indirect : {false, true})
      for (bool HasVectorSummary : {false, true})
        for (bool Phi : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << unsigned(Format) << ':' << Indirect << ':'
                       << HasVectorSummary << ':' << Phi);
          BinaryImage Image;
          Image.Arch = Arch::X64;
          Image.Format = Format;
          Image.Bits = Bitness::Bits64;
          MedFunc Function;
          Function.Entry = 0x1000;
          Function.Name = "caller";
          Function.Blocks.resize(Phi ? 2 : 1);
          for (size_t I = 0; I < Function.Blocks.size(); ++I)
            Function.Blocks[I].Id = I;
          auto &Block = Function.Blocks.back();
          MedVar Scratch;
          Scratch.Kind = MedVar::Reg;
          Scratch.Id = 1;
          Scratch.SSAVer = 1;
          Scratch.RegOff = x86reg::XMM0;
          Scratch.Size = 16;
          Scratch.TheArch = Arch::X64;
          if (Phi) {
            Function.Blocks.front().Succs = {Block.Id};
            Block.Preds = {0};
            PhiNode Merge;
            Merge.Output = Scratch;
            Merge.Args = {{0, MedVar::makeUnspecified(16, Arch::X64)}};
            Block.Phis.push_back(Merge);
          } else {
            MedOp Copy;
            Copy.Opcode = NdOp::COPY;
            Copy.Output = Scratch;
            Copy.addInput(MedVar::makeUnspecified(16, Arch::X64));
            Block.Ops.push_back(Copy);
          }
          MedOp Call;
          Call.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
          Call.Addr = Function.Entry + 8;
          Call.addInput(MedVar::makeConst(0x2000, 8));
          const std::vector<MedVar> Arguments = {MedVar::makeConst(19, 8),
                                                 MedVar::makeConst(29, 1)};
          for (const auto &Argument : Arguments)
            Call.addInput(Argument);
          Call.CalleeRegisterArgs = Arguments.size();
          Call.CalleeVectorArgs = HasVectorSummary ? 0 : -1;
          Call.CalleeStackArgs = 0;
          Block.Ops.push_back(Call);
          // Source preparation may no longer have the omitted callee's body
          // or its FP arity map. The published operands remain authoritative.
          for (unsigned Repeat = 0; Repeat < 2; ++Repeat) {
            recoverCallAbi(Function, Arch::X64, {{0x2000, "consumer"}}, &Image);
            ASSERT_EQ(Function.CallInfos.size(), 1u);
            const auto &Recovered = Function.CallInfos.front().Args;
            ASSERT_EQ(Recovered.size(), HasVectorSummary ? 2u : 3u);
            EXPECT_EQ(Recovered[0], Arguments[0]);
            EXPECT_EQ(Recovered[1], Arguments[1]);
            if (!HasVectorSummary)
              EXPECT_EQ(Recovered[2], Scratch);
          }
        }
}

TEST(SummarizedCallArguments, ScalarCallsExecuteWithoutUnobservedVectorTraps) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Format = Format;
    Image.Bits = Bitness::Bits64;
    MedFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "call_scalar";
    Function.ReturnType = NdType::makeInt(8, false);
    Function.Blocks.resize(1);
    auto &Block = Function.Blocks.front();
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    MedOp Scratch;
    Scratch.Opcode = NdOp::COPY;
    Scratch.Output.Kind = MedVar::Reg;
    Scratch.Output.Id = 1;
    Scratch.Output.SSAVer = 1;
    Scratch.Output.Size = 16;
    Scratch.Output.RegOff = x86reg::XMM0;
    Scratch.Output.TheArch = Arch::X64;
    Scratch.addInput(MedVar::makeUnspecified(16, Arch::X64));
    Block.Ops.push_back(Scratch);
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Function.Entry + 4;
    Call.Output.Kind = MedVar::Temp;
    Call.Output.Id = 2;
    Call.Output.SSAVer = 1;
    Call.Output.Size = 8;
    Call.Output.TheArch = Arch::X64;
    Call.addInput(MedVar::makeConst(0x2000, 8));
    Call.addInput(MedVar::makeConst(19, 8));
    Call.addInput(MedVar::makeConst(29, 1));
    Call.CalleeRegisterArgs = 2;
    Call.CalleeVectorArgs = 0;
    Call.CalleeStackArgs = 0;
    Block.Ops.push_back(Call);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Function.Entry + 9;
    Return.addInput(Call.Output);
    Block.Ops.push_back(Return);
    recoverCallAbi(Function, Arch::X64, {{0x2000, "consume_scalar"}}, &Image);
    ASSERT_EQ(Function.CallInfos.size(), 1u);
    ASSERT_EQ(Function.CallInfos.front().Args.size(), 2u);
    llvm::LLVMContext Context;
    MedLLVMEmitter Emitter;
    Emitter.setPreserveUnknownSourceValues(true);
    auto Module = Emitter.emit({Function}, Context, "scalar_only", Arch::X64,
                               {{0x2000, "consume_scalar"}}, &Image, Format);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    ASSERT_NE(Module->getFunction("consume_scalar"), nullptr);
    EXPECT_EQ(Module->getFunction("consume_scalar")->arg_size(), 2u);
    Pipeline::optimizeOrPromoteModule(*Module);
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      if (LLVM) {
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, {}));
      } else {
        const std::map<va_t, std::string> Names{{0x2000, "consume_scalar"}};
        MedToHighConverter Converter;
        Converter.setFuncNames(&Names);
        auto High = Converter.convert(Function, Arch::X64);
        ASSERT_TRUE(HighCEmitter().emit({High}, Output));
      }
      EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
      // An independently typed provider rejects an extra argument at compile
      // time and checks both exact-width values and the call's execution.
      const std::string Provider =
          "#include <stdint.h>\nstatic unsigned observed;\n" +
          std::string(LLVM ? "uint64_t consume_scalar(uint64_t a, uint8_t b)"
                           : "int64_t consume_scalar(int64_t a, int b)") +
          R"( {
  ++observed;
  return a == 19 && b == 29 ? UINT64_C(0x123456789ABCDE) : 0;
}
)";
      const std::string Main = R"(
int main(void) {
  return call_scalar() != UINT64_C(0x123456789ABCDE) || observed != 1;
}
)";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Provider + Source + Main,
            {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
  }
}

TEST(SummarizedCallArguments, RetainsReachingOperandsWithoutSetupWrites) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool Indirect : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(Format));
      SCOPED_TRACE(Indirect);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Format = Format;
      Image.Bits = Bitness::Bits64;
      MedFunc Function;
      Function.Entry = 0x1000;
      Function.Name = "caller";
      MedBlock Block;
      Block.Id = 0;
      MedOp Call;
      Call.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
      Call.Addr = Function.Entry;
      Call.addInput(MedVar::makeConst(0x2000, 8));
      const std::vector<MedVar> Arguments = {
          MedVar::makeConst(13, 1), MedVar::makeConst(257, 2),
          MedVar::makeConst(65539, 4), MedVar::makeConst(0x100000007, 8)};
      for (const MedVar &Argument : Arguments)
        Call.addInput(Argument);
      // LowToMed published these uses before SSA. Propagation may replace
      // every register copy by its value; no local setup scan may erase them.
      Call.CalleeRegisterArgs = Arguments.size();
      Call.CalleeVectorArgs = 0;
      Call.CalleeStackArgs = 0;
      Block.Ops.push_back(Call);
      Function.Blocks.push_back(std::move(Block));
      std::map<va_t, int> RegisterArity{{0x2000, Arguments.size()}};
      std::map<va_t, int> TotalArity = RegisterArity;
      for (unsigned Repeat = 0; Repeat < 2; ++Repeat) {
        recoverCallAbi(Function, Arch::X64, {{0x2000, "consumer"}}, &Image,
                       &RegisterArity, &TotalArity);
        ASSERT_EQ(Function.CallInfos.size(), 1u);
        EXPECT_EQ(Function.CallInfos.front().Args, Arguments);
      }
    }
}

TEST(SummarizedCallArguments, UnknownRequiredOperandsDoNotBecomeZeroArity) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  Image.Bits = Bitness::Bits64;
  MedFunc Function;
  Function.Entry = 0x1000;
  Function.Name = "caller";
  MedBlock Block;
  Block.Id = 0;
  MedOp EarlierCall;
  EarlierCall.Opcode = NdOp::CALL;
  EarlierCall.Addr = Function.Entry;
  EarlierCall.addInput(MedVar::makeConst(0x3000, 8));
  EarlierCall.CalleeRegisterArgs = 0;
  EarlierCall.CalleeStackArgs = 0;
  Block.Ops.push_back(EarlierCall);
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Function.Entry + 5;
  Call.addInput(MedVar::makeConst(0x2000, 8));
  const MedVar Unknown = MedVar::makeUnspecified(8, Arch::X64);
  Call.addInput(Unknown);
  Call.addInput(MedVar::makeConst(27, 8));
  Call.CalleeRegisterArgs = 2;
  Call.CalleeStackArgs = 0;
  Block.Ops.push_back(Call);
  Function.Blocks.push_back(std::move(Block));
  std::map<va_t, int> RegisterArity{{0x2000, 2}};
  std::map<va_t, int> TotalArity = RegisterArity;
  recoverCallAbi(Function, Arch::X64, {{0x2000, "consumer"}}, &Image,
                 &RegisterArity, &TotalArity);
  ASSERT_EQ(Function.CallInfos.size(), 2u);
  const auto &Arguments = Function.CallInfos.back().Args;
  ASSERT_EQ(Arguments.size(), 2u);
  EXPECT_EQ(Arguments.front(), Unknown);
  EXPECT_EQ(Arguments.back(), Call.Inputs[2]);
}

TEST(SummarizedCallArguments, UnreadRegisterTailsDoNotBecomeArguments) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (unsigned Count : {0u, 1u, 3u}) {
      SCOPED_TRACE(static_cast<unsigned>(Format));
      SCOPED_TRACE(Count);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Format = Format;
      Image.Bits = Bitness::Bits64;
      MedFunc Function;
      Function.Entry = 0x1000;
      Function.Name = "caller";
      MedBlock Block;
      Block.Id = 0;
      const auto Registers =
          getTargetRegInfo(Arch::X64).integerParamRegs(Format);
      for (unsigned I = 0; I < Registers.size(); ++I) {
        MedOp Setup;
        Setup.Opcode = NdOp::COPY;
        Setup.Output.Kind = MedVar::Reg;
        Setup.Output.TheArch = Arch::X64;
        Setup.Output.Id = I + 1;
        Setup.Output.SSAVer = 1;
        Setup.Output.Size = 8;
        Setup.Output.RegOff = Registers[I];
        Setup.addInput(MedVar::makeConst(100 + I, 8));
        Block.Ops.push_back(Setup);
      }
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.Addr = 0x1010;
      Call.addInput(MedVar::makeConst(0x2000, 8));
      for (unsigned I = 0; I < Count; ++I)
        Call.addInput(MedVar::makeConst(7 + I, 8));
      Call.CalleeRegisterArgs = Count;
      Call.CalleeStackArgs = 0;
      Block.Ops.push_back(Call);
      Function.Blocks.push_back(std::move(Block));
      std::map<va_t, int> Arity{{0x2000, Count}};
      recoverCallAbi(Function, Arch::X64, {{0x2000, "consumer"}}, &Image,
                     &Arity, &Arity);
      ASSERT_EQ(Function.CallInfos.size(), 1u);
      EXPECT_EQ(Function.CallInfos.front().Args,
                std::vector<MedVar>(Call.Inputs.begin() + 1,
                                    Call.Inputs.begin() + Call.NumInputs));
    }
}

TEST(SummarizedCallArguments, CountOnlyContractsStillReadTheirSetup) {
  for (Arch Architecture : {Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    BinaryImage Image;
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::COFF;
    Image.Bits = Architecture == Arch::ARM ? Bitness::Bits32 : Bitness::Bits64;
    const auto &Registers = getTargetRegInfo(Architecture);
    MedFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "caller";
    MedBlock Block;
    Block.Id = 0;
    MedOp Setup;
    Setup.Opcode = NdOp::COPY;
    Setup.Output.Kind = MedVar::Reg;
    Setup.Output.TheArch = Architecture;
    Setup.Output.Id = 1;
    Setup.Output.SSAVer = 1;
    Setup.Output.Size = Registers.PointerSize;
    Setup.Output.RegOff = Registers.integerParamRegs(Image.Format).front();
    Setup.addInput(MedVar::makeConst(42, Registers.PointerSize));
    Block.Ops.push_back(Setup);
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Function.Entry + 4;
    Call.addInput(MedVar::makeConst(0x2000, Registers.PointerSize));
    Call.CalleeRegisterArgs = 1;
    Call.CalleeStackArgs = 0;
    Block.Ops.push_back(Call);
    Function.Blocks.push_back(std::move(Block));
    std::map<va_t, int> RegisterArity{{0x2000, 1}};
    std::map<va_t, int> TotalArity = RegisterArity;
    recoverCallAbi(Function, Architecture, {{0x2000, "consumer"}}, &Image,
                   &RegisterArity, &TotalArity);
    ASSERT_EQ(Function.CallInfos.size(), 1u);
    ASSERT_EQ(Function.CallInfos.front().Args.size(), 1u);
    EXPECT_EQ(Function.CallInfos.front().Args.front(), Setup.Inputs[0]);
  }
}

TEST(SummarizedCallArguments, PropagatedOperandsExecuteWithTheirExactWidths) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Format = Format;
    Image.Bits = Bitness::Bits64;
    MedFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "call_values";
    Function.ReturnType = NdType::makeInt(8, false);
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Function.Entry;
    Call.Output.Kind = MedVar::Temp;
    Call.Output.Id = 1;
    Call.Output.SSAVer = 1;
    Call.Output.Size = 8;
    Call.Output.TheArch = Arch::X64;
    Call.addInput(MedVar::makeConst(0x2000, 8));
    Call.addInput(MedVar::makeConst(0xD3, 1));
    Call.addInput(MedVar::makeConst(0xA157, 2));
    Call.addInput(MedVar::makeConst(0x87654321, 4));
    Call.addInput(MedVar::makeConst(0xFEDCBA9876543210, 8));
    Call.CalleeRegisterArgs = 4;
    Call.CalleeVectorArgs = 0;
    Call.CalleeStackArgs = 0;
    Block.Ops.push_back(Call);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Function.Entry + 5;
    Return.addInput(Call.Output);
    Block.Ops.push_back(Return);
    Function.Blocks.push_back(std::move(Block));
    std::map<va_t, int> RegisterArity{{0x2000, 4}};
    std::map<va_t, int> TotalArity = RegisterArity;
    recoverCallAbi(Function, Arch::X64, {{0x2000, "consume_values"}}, &Image,
                   &RegisterArity, &TotalArity);
    ASSERT_TRUE(Function.Params.empty());
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, "bound_operands", Arch::X64,
                              {{0x2000, "consume_values"}}, &Image, Format);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    const auto *Callee = Module->getFunction("consume_values");
    ASSERT_TRUE(Callee);
    ASSERT_EQ(Callee->arg_size(), 4u);
    const unsigned Widths[] = {8, 16, 32, 64};
    for (unsigned I = 0; I < 4; ++I)
      EXPECT_TRUE(
          Callee->getFunctionType()->getParamType(I)->isIntegerTy(Widths[I]));
    for (bool Optimize : {false, true}) {
      SCOPED_TRACE(Optimize);
      if (Optimize)
        Pipeline::optimizeOrPromoteModule(*Module);
      else
        Pipeline::promoteScaffoldingAllocas(*Module);
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, {}));
      const std::string Oracle = R"(
static unsigned observed;
uint64_t consume_values(uint8_t a, uint16_t b, uint32_t c, uint64_t d) {
  ++observed;
  return a == 0xD3 && b == 0xA157 && c == UINT32_C(0x87654321) &&
         d == UINT64_C(0xFEDCBA9876543210) ? UINT64_C(0x123456789ABCDE) : 0;
}
int main(void) {
  return call_values() != UINT64_C(0x123456789ABCDE) || observed != 1;
}
)";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source + Oracle,
            {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
  }
}

constexpr va_t Text = 0x140001000;
constexpr va_t Iat = 0x140003000;

void put(std::vector<uint8_t> &Code, va_t At, std::vector<uint8_t> Bytes) {
  std::copy(Bytes.begin(), Bytes.end(), Code.begin() + (At - Text));
}

std::vector<uint8_t> rel32(va_t Next, va_t To) {
  const uint32_t Delta = static_cast<uint32_t>(To - Next);
  return {static_cast<uint8_t>(Delta), static_cast<uint8_t>(Delta >> 8),
          static_cast<uint8_t>(Delta >> 16), static_cast<uint8_t>(Delta >> 24)};
}

/// The slot of import \p Index.
constexpr va_t slot(size_t Index) { return Iat + 8 * Index; }

/// `call [rip + Slot]`, or `jmp` when \p Jump, at \p At.
std::vector<uint8_t> throughSlot(va_t At, va_t Slot, bool Jump = false) {
  std::vector<uint8_t> Bytes = {0xFF, static_cast<uint8_t>(Jump ? 0x25 : 0x15)};
  for (uint8_t B : rel32(At + 6, Slot))
    Bytes.push_back(B);
  return Bytes;
}

/// `sub rsp, 28h`, the calls \p Calls, `add rsp, 28h` and `ret`: a function
/// that passes its own argument registers on to each callee.  The first call
/// starts 4 bytes in.
std::vector<uint8_t> framed(const std::vector<std::vector<uint8_t>> &Calls) {
  std::vector<uint8_t> Bytes = {0x48, 0x83, 0xEC, 0x28};
  for (const auto &Call : Calls)
    Bytes.insert(Bytes.end(), Call.begin(), Call.end());
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x28, 0xC3})
    Bytes.push_back(B);
  return Bytes;
}

/// A PE32+ image with \p Code at Text, the given function symbols, and the
/// msvcrt.dll imports \p Imports in consecutive slots at Iat.
BinaryImage
makeImage(std::vector<uint8_t> Code,
          const std::vector<std::pair<va_t, const char *>> &Functions,
          const std::vector<const char *> &Imports) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Text;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  Segment Idata;
  Idata.Name = ".idata";
  Idata.VA = Iat;
  Idata.Size = Idata.FileSz = 8 * Imports.size();
  Idata.Data.resize(Idata.Size);
  Idata.Flags = SegmentFlags::Readable;
  Img.Segments.push_back(std::move(Idata));
  for (size_t I = 0; I < Imports.size(); ++I) {
    Import Imp;
    Imp.Module = "msvcrt.dll";
    Imp.Name = Imports[I];
    Imp.IATAddr = slot(I);
    Img.Imports.push_back(std::move(Imp));
  }
  for (const auto &[Entry, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  return Img;
}

PipelineResult run(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                   std::set<va_t> Entries) {
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = std::move(Entries);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  return Result;
}

TEST(Win64CallContract, BoundIntegerOperandsAgreeAcrossSourceRoutes) {
  constexpr va_t Consumer = Text + 0x80;
  std::vector<uint8_t> Code(0xA0, 0xCC);
  std::vector<uint8_t> Invoke = {
      0xB9, 0x0D, 0x00, 0x00, 0x00,       // mov ecx,13
      0xBA, 0x01, 0x01, 0x00, 0x00,       // mov edx,257
      0x41, 0xB8, 0x03, 0x00, 0x01, 0x00, // mov r8d,65539
      0x49, 0xB9, 0x07, 0x00, 0x00, 0x00, // mov r9,100000007h
      0x01, 0x00, 0x00, 0x00, 0xE8};
  const auto Displacement = rel32(Text + 4 + Invoke.size() + 4, Consumer);
  Invoke.insert(Invoke.end(), Displacement.begin(), Displacement.end());
  put(Code, Text, framed({Invoke}));
  put(Code, Consumer,
      {0x48, 0x8D, 0x04, 0x11, // lea rax,[rcx+rdx]
       0x4C, 0x01, 0xC0,       // add rax,r8
       0x4C, 0x31, 0xC8,       // xor rax,r9
       0xC3});
  const BinaryImage Image = makeImage(
      Code, {{Text, "call_bound_values"}, {Consumer, "combine_values"}}, {});
  for (bool LLVM : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(NoOpt);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Text, Consumer};
      Options.LiftMode = LLVM;
      Options.SourceProjection = true;
      Options.NoOpt = NoOpt;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
      } else
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
      const std::string Oracle = R"(
int main(void) {
  const uint64_t expected = (UINT64_C(13) + 257 + 65539) ^ UINT64_C(0x100000007);
  return (uint64_t)call_bound_values() != expected;
}
)";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source + Oracle,
            {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
}

TEST(Win64CallContract, UnreadRegisterHolesBeforeStackArgumentsStayUnread) {
  constexpr va_t Consumer = Text + 0x80;
  std::vector<uint8_t> Code(0xA0, 0xCC);
  std::vector<uint8_t> Caller = {0x48, 0x83, 0xEC, 0x38};
  const auto Opaque = throughSlot(Text + Caller.size(), slot(0));
  Caller.insert(Caller.end(), Opaque.begin(), Opaque.end());
  const std::vector<uint8_t> Setup = {
      0xB9, 11,   0,    0,    0,                 // mov ecx,11
      0x41, 0xB8, 22,   0,    0,    0,           // mov r8d,22
      0x48, 0xC7, 0x44, 0x24, 0x20, 13, 0, 0, 0, // mov [rsp+20h],13
      0xE8};
  Caller.insert(Caller.end(), Setup.begin(), Setup.end());
  const auto Displacement = rel32(Text + Caller.size() + 4, Consumer);
  Caller.insert(Caller.end(), Displacement.begin(), Displacement.end());
  for (uint8_t Byte : {0x48, 0x83, 0xC4, 0x38, 0xC3})
    Caller.push_back(Byte);
  put(Code, Text, Caller);
  put(Code, Consumer,
      {0x48, 0x8B, 0x44, 0x24, 0x28, // mov rax,[rsp+28h]
       0x48, 0x01, 0xC8,             // add rax,rcx
       0x4C, 0x01, 0xC0,             // add rax,r8
       0xC3});
  auto Image = makeImage(
      Code, {{Text, "call_stack_holes"}, {Consumer, "read_stack_holes"}},
      {"rand"});
  ASSERT_TRUE(Image.recordImportStorageSlot(Iat, "rand", 0,
                                            ImportStorageEvidence::LoaderBind));
  for (bool LLVM : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(NoOpt);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Text, Consumer};
      Options.LiftMode = LLVM;
      Options.SourceProjection = true;
      Options.NoOpt = NoOpt;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
      } else
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
      EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
      const std::string Oracle =
          "\nint main(void) { return call_stack_holes() != 46; }\n";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source + Oracle,
            {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
}

TEST(Win64CallContract, DebugNamedOmittedRuntimeKeepsItsFixedArity) {
  constexpr va_t Runtime = Text + 0x80;
  class PublicNames : public NullDebugContext {
  public:
    bool hasInfo() const override { return true; }
    std::optional<std::string> functionName(va_t Address) const override {
      return Address == Runtime
                 ? std::optional<std::string>("__security_check_cookie")
                 : std::nullopt;
    }
  } Debug;
  std::vector<uint8_t> Code(0xA0, 0xCC);
  std::vector<uint8_t> Caller = {0x48, 0x83, 0xEC, 0x28};
  const auto Random = throughSlot(Text + Caller.size(), Iat);
  Caller.insert(Caller.end(), Random.begin(), Random.end());
  Caller.insert(Caller.end(), {0xB9, 47, 0, 0, 0, 0xE8});
  const auto Displacement = rel32(Text + Caller.size() + 4, Runtime);
  Caller.insert(Caller.end(), Displacement.begin(), Displacement.end());
  Caller.insert(Caller.end(),
                {0xB8, 19, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3});
  put(Code, Text, Caller);
  put(Code, Runtime, {0x48, 0x89, 0xC8, 0xC3}); // mov rax,rcx; ret
  const auto Image = makeImage(Code, {{Text, "checked_caller"}}, {"rand"});
  for (bool LLVM : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(NoOpt);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Text};
      Options.LiftMode = LLVM;
      Options.SourceProjection = true;
      Options.NoOpt = NoOpt;
      auto Result = Pipeline().run(Image, Context, Options, &Debug);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      if (LLVM) {
        const auto *Call = Result.MedFuncs.front().CallInfos.empty()
                               ? nullptr
                               : &Result.MedFuncs.front().CallInfos.back();
        ASSERT_TRUE(Call);
        EXPECT_EQ(Call->TargetName, "__security_check_cookie");
        EXPECT_EQ(Call->Args.size(), 1u);
      }
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions,
                                        &Debug, &Image));
      } else
        ASSERT_TRUE(
            HighCEmitter().emit(Result.HighFuncs, Output, COptions, &Debug));
      EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
      const std::string Oracle =
          std::string("\nstatic unsigned cookie_calls;\n") +
          (LLVM ? "uint64_t __security_check_cookie(uint64_t cookie)"
                : "int64_t __security_check_cookie(int64_t cookie)") +
          R"( {
  cookie_calls += cookie == 47;
  return 0;
}
int main(void) { return checked_caller() != 19 || cookie_calls != 1; }
)";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source + Oracle,
            {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
}

TEST(CallExitRegisters, ConditionalHomeSavesAreNotValueReads) {
  constexpr va_t Consumer = Text + 0x80;
  std::vector<uint8_t> Code(0xA0, 0xCC);
  std::vector<uint8_t> Caller = {
      0x85, 0xC9, 0x74, 0,          // test ecx,ecx; jz zero
      0x48, 0x89, 0x5C, 0x24, 0x08, // mov [rsp+8],rbx
      0x48, 0x83, 0xEC, 0x38,       // sub rsp,38h
      0x8B, 0xD9,                   // mov ebx,ecx
      0x48, 0x8D, 0x4C, 0x24, 0x20, // lea rcx,[rsp+20h]
      0xE8};
  const auto Displacement = rel32(Text + Caller.size() + 4, Consumer);
  Caller.insert(Caller.end(), Displacement.begin(), Displacement.end());
  const std::vector<uint8_t> Restore = {
      0x8B, 0xC3,                          // mov eax,ebx
      0x48, 0x8B, 0x5C, 0x24, 0x40,        // mov rbx,[rsp+40h]
      0x48, 0x83, 0xC4, 0x38, 0xEB, 0x02}; // add rsp,38h; jmp shared ret
  Caller.insert(Caller.end(), Restore.begin(), Restore.end());
  Caller[3] = static_cast<uint8_t>(Caller.size() - 4);
  Caller.insert(Caller.end(), {0x31, 0xC0, 0xC3}); // xor eax,eax; ret
  put(Code, Text, Caller);
  put(Code, Consumer,
      {0x48, 0xC7, 0x01, 0, 0, 0, 0, // mov [rcx],0
       0x31, 0xC0, 0xC3});
  const auto Image = makeImage(
      Code, {{Text, "conditional_save"}, {Consumer, "clear_local"}}, {});
  for (bool LLVM : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(NoOpt);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Text, Consumer};
      Options.LiftMode = LLVM;
      Options.SourceProjection = true;
      Options.NoOpt = NoOpt;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
      } else
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
      EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
      const std::string Oracle = R"(
int main(void) {
  return conditional_save(0) != 0 || conditional_save(19) != 19;
}
)";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source + Oracle,
            {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
}

TEST(CallExitRegisters, OverwrittenEntrySpillsAreNotValueReads) {
  constexpr va_t Consumer = Text + 0x80;
  for (bool Split : {false, true}) {
    std::vector<uint8_t> Code(0xA0, 0xCC);
    std::vector<uint8_t> Caller = {0x4C, 0x89, 0x4C, 0x24,
                                   0x20,                   // mov [rsp+20h],r9
                                   0x48, 0x83, 0xEC, 0x28, // sub rsp,28h
                                   0x48, 0x01, 0xD1,       // add rcx,rdx
                                   0x4C, 0x01, 0xC1,       // add rcx,r8
                                   0x48, 0x83, 0xC1, 19};  // add rcx,19
    if (Split) {
      Caller.insert(Caller.end(), {0x89, 0x4C, 0x24, 0x48, 0xC7, 0x44, 0x24,
                                   0x4C, 0, 0, 0, 0});
    } else {
      Caller.insert(Caller.end(), {0x48, 0x89, 0x4C, 0x24, 0x48});
    }
    Caller.insert(Caller.end(), {0x48, 0x8D, 0x4C, 0x24, 0x48, 0xE8});
    const auto Displacement = rel32(Text + Caller.size() + 4, Consumer);
    Caller.insert(Caller.end(), Displacement.begin(), Displacement.end());
    Caller.insert(Caller.end(), {0x48, 0x83, 0xC4, 0x28, 0xC3});
    put(Code, Text, Caller);
    put(Code, Consumer, {0x48, 0x8B, 0x01, 0xC3});
    const auto Image =
        makeImage(Code, {{Text, "reused_home"}, {Consumer, "read_home"}}, {});
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(Split);
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(NoOpt);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Text, Consumer};
        Options.LiftMode = LLVM;
        Options.SourceProjection = true;
        Options.NoOpt = NoOpt;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
        } else
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
        ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
        for (llvm::StringRef Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Source +
                  "\nint main(void) { return reused_home(1, 2, 3) != 25 || "
                  "reused_home(7, 11, 13) != 50; }\n",
              {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
  }
}

TEST(CallExitRegisters, EntrySpillProofKeepsObservedBytesAndEscapes) {
  for (unsigned Version : {0u, 3u})
    for (unsigned Mode = 0; Mode < 7; ++Mode) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Version);
      MedFunc Function;
      Function.Entry = Text;
      Function.FrameSize = 40;
      Function.CC = CallingConv::Win64;
      auto Register = [](int Id, uint64_t Offset) {
        MedVar V;
        V.Kind = MedVar::Reg;
        V.Id = Id;
        V.RegOff = Offset;
        V.Size = 8;
        V.TheArch = Arch::X64;
        return V;
      };
      const MedVar SP = Register(0, x86reg::RSP);
      MedVar Incoming = Register(1, x86reg::R9);
      Incoming.SSAVer = Version;
      MedVar Address = Register(2, x86reg::R10);
      Address.SSAVer = 1;
      MedVar Loaded = Register(3, x86reg::RAX);
      Loaded.SSAVer = 1;
      auto Operation = [](NdOp Opcode, MedVar Output,
                          std::initializer_list<MedVar> Inputs) {
        MedOp Op;
        Op.Opcode = Opcode;
        Op.Output = Output;
        for (const auto &Input : Inputs)
          Op.addInput(Input);
        return Op;
      };
      MedBlock Entry;
      Entry.Id = 0;
      Entry.StartAddr = Text;
      Entry.Ops = {
          Operation(NdOp::COPY, SP, {SP}),
          Operation(NdOp::COPY, Incoming, {Incoming}),
          Operation(NdOp::INT_ADD, Address, {SP, MedVar::makeConst(32, 8)}),
          Operation(NdOp::STORE, {}, {Address, Incoming})};
      const auto Write = Operation(
          NdOp::STORE, {}, {Address, MedVar::makeConst(19, Mode == 1 ? 4 : 8)});
      const auto Read = Operation(NdOp::LOAD, Loaded, {Address});
      const auto Return = Operation(NdOp::RETURN, {}, {Loaded});
      if (Mode == 2)
        Entry.Ops.push_back(Operation(
            NdOp::CALL, {}, {MedVar::makeConst(Text + 0x80, 8), Address}));
      if (Mode == 3)
        Entry.Ops.push_back(Read);
      if (Mode >= 5) {
        Entry.Succs = {1, 2};
        Entry.Ops.push_back(Operation(
            NdOp::COND_BR, {}, {MedVar::makeConst(Text + 0x20, 8), Incoming}));
        Function.Blocks.push_back(std::move(Entry));
        for (int Id : {1, 2}) {
          MedBlock Arm;
          Arm.Id = Id;
          Arm.StartAddr = Text + 0x20 * Id;
          Arm.Preds = {0};
          if (Mode == 5 || Id == 1)
            Arm.Ops.push_back(Write);
          Arm.Ops.push_back(Read);
          Arm.Ops.push_back(Return);
          Function.Blocks.push_back(std::move(Arm));
        }
      } else if (Mode == 4) {
        Entry.Ops.push_back(Operation(NdOp::RETURN, {}, {Address}));
        Function.Blocks.push_back(std::move(Entry));
      } else {
        Entry.Ops.push_back(Write);
        Entry.Ops.push_back(Read);
        Entry.Ops.push_back(Return);
        Function.Blocks.push_back(std::move(Entry));
      }
      markSourceStackAccesses(Function, Arch::X64, BinaryFormat::COFF);
      EXPECT_EQ(Function.Blocks.front().Ops[3].PrologueSave,
                Mode == 0 || Mode == 5);
    }
}

class OverwrittenCallClobberSpills : public testing::TestWithParam<unsigned> {};

INSTANTIATE_TEST_SUITE_P(SourceStackSpills, OverwrittenCallClobberSpills,
                         testing::Values(0u, 1u, 2u));

TEST_P(OverwrittenCallClobberSpills, AreNotValueReads) {
  const unsigned Variant = GetParam();
#if !defined(__x86_64__) && !defined(_M_X64)
  if (Variant)
    GTEST_SKIP() << "Native x64 flags-transfer execution requires an x64 host";
#endif
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    // An opaque call leaves R11 unavailable to an ordinary source function.
    // Its scratch spill is fully overwritten before the body reads the slot.
    const auto Call = throughSlot(Text + 4, slot(0));
    std::vector<uint8_t> Code = {0x48, 0x83, 0xEC, 0x28};
    Code.insert(Code.end(), Call.begin(), Call.end());
    const std::vector<uint8_t> Save = {0x4C, 0x89, 0x5C, 0x24, 0x20};
    Code.insert(Code.end(), Save.begin(), Save.end());
    if (Variant)
      Code.insert(Code.end(), {0x9C, 0x9D}); // pushfq; popfq
    if (Variant == 2)
      Code.insert(Code.end(), {0x4C, 0x8B, 0x5C, 0x24, 0x20}); // restore r11
    const std::vector<uint8_t> Body = {
        0x48, 0xC7, 0x44, 0x24, 0x20, 19, 0, 0, 0, // mov qword [...],19
        0x48, 0x8B, 0x44, 0x24, 0x20,              // mov rax,[rsp+20h]
        0x48, 0x83, 0xC4, 0x28, 0xC3};
    Code.insert(Code.end(), Body.begin(), Body.end());
    auto Image = makeImage(Code, {{Text, "reused_scratch"}}, {"rand"});
    Image.Format = Format;
    ASSERT_TRUE(Image.recordImportStorageSlot(
        Iat, "rand", 0, ImportStorageEvidence::LoaderBind));
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(testing::Message() << unsigned(Format) << ':' << Variant
                                        << ':' << LLVM << ':' << NoOpt);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Text};
        Options.LiftMode = LLVM;
        Options.SourceProjection = true;
        Options.NoOpt = NoOpt;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
        } else
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
        EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
        if (Variant) {
          EXPECT_NE(Source.find("__readeflags"), std::string::npos) << Source;
          EXPECT_NE(Source.find("__writeeflags"), std::string::npos) << Source;
        }
        if (Source.find("__builtin_trap") != std::string::npos)
          continue;
        // Execute the same recovered IR with the host intrinsic header.
        // COFF emission above keeps the Windows spelling; selecting ELF
        // here supplies x86intrin.h for the native Linux compiler.
        std::string Native;
        llvm::raw_string_ostream NativeOutput(Native);
        COptions.Format = BinaryFormat::ELF;
        if (LLVM)
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Result.LlvmModule, NativeOutput, COptions));
        else
          ASSERT_TRUE(
              HighCEmitter().emit(Result.HighFuncs, NativeOutput, COptions));
        const std::string Provider = R"(
#include <stdlib.h>
static unsigned calls;
static int spill_provider(void) { ++calls; return 13; }
#define rand spill_provider
)";
        const std::string Oracle = R"(
int main(void) { return reused_scratch() != 19 || calls != 1; }
)";
        for (llvm::StringRef Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Provider + Native + Oracle,
              {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
  }
}

TEST(CallExitRegisters, ConstantScratchValueReachesTheCallersStore) {
  constexpr va_t Producer = Text + 0x80;
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    const bool Windows = Format == BinaryFormat::COFF;
    const uint8_t Frame = Windows ? 32 : 0;
    std::vector<uint8_t> Code(0xA0, 0xCC);
    // Keep the output pointer in RBX while the leaf writes a non-return
    // volatile register. The caller observes its actual machine value.
    std::vector<uint8_t> Caller = {
        0x53, 0x48, 0x89, static_cast<uint8_t>(Windows ? 0xCB : 0xFB),
        0x48, 0x83, 0xEC, Frame,
        0xE8};
    const auto Displacement = rel32(Text + Caller.size() + 4, Producer);
    Caller.insert(Caller.end(), Displacement.begin(), Displacement.end());
    const std::vector<uint8_t> Suffix = {
        0x48,  0x89, static_cast<uint8_t>(Windows ? 0x0B : 0x3B),
        0x48,  0x83, 0xC4,
        Frame, 0x5B, 0xC3};
    Caller.insert(Caller.end(), Suffix.begin(), Suffix.end());
    put(Code, Text, Caller);
    put(Code, Producer,
        {static_cast<uint8_t>(Windows ? 0xB9 : 0xBF), 27, 0, 0, 0, 0xB8, 1, 0,
         0, 0, 0xC3});
    auto Image = makeImage(
        Code, {{Text, "observe_exit"}, {Producer, "produce_exit"}}, {});
    Image.Format = Format;
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(NoOpt);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Text, Producer};
        Options.LiftMode = LLVM;
        Options.SourceProjection = true;
        Options.NoOpt = NoOpt;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
        } else
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
        const std::string Oracle = R"(
int main(void) {
  uint64_t output = 999;
  return observe_exit(&output) != 1 || output != 27;
}
)";
        for (llvm::StringRef Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Source + Oracle,
              {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
  }
}

TEST(CallExitRegisters, UnknownSourceObservationsDoNotBecomeEqual) {
  for (bool Optimize : {false, true}) {
    SCOPED_TRACE(Optimize);
    llvm::LLVMContext Context;
    llvm::Module Module("unknown-observations", Context);
    Module.setDataLayout("e-p:64:64");
    auto *Word = llvm::Type::getInt64Ty(Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Word, false),
        llvm::GlobalValue::ExternalLinkage, "unknown_pair", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Left = llvm_source_unknown::create(Builder, Word);
    auto *Right = llvm_source_unknown::create(Builder, Word);
    Builder.CreateRet(Builder.CreateSub(Left, Right));
    // An unobserved clobber imposes no source execution failure.
    auto *Unused = llvm::Function::Create(Function->getFunctionType(),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "unobserved", Module);
    Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Unused));
    llvm_source_unknown::create(Builder, Word);
    Builder.CreateRet(llvm::ConstantInt::get(Word, 7));
    auto *Guarded = llvm::Function::Create(
        llvm::FunctionType::get(Word, {Word}, false),
        llvm::GlobalValue::ExternalLinkage, "guarded_unknown", Module);
    Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Guarded));
    auto *ConditionalValue = llvm_source_unknown::create(Builder, Word);
    auto *Condition = Builder.CreateICmpNE(Guarded->getArg(0),
                                           llvm::ConstantInt::get(Word, 0));
    Builder.CreateRet(Builder.CreateSelect(Condition, ConditionalValue,
                                           llvm::ConstantInt::get(Word, 17)));
    if (Optimize)
      Pipeline::optimizeModule(Module);
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    unsigned Observed = 0;
    for (const auto &Block : *Module.getFunction("unknown_pair"))
      for (const auto &Instruction : Block)
        Observed += llvm_source_unknown::isValue(&Instruction) &&
                    !Instruction.use_empty();
    ASSERT_EQ(Observed, 2u);
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Output));
    EXPECT_EQ(Source.find("__neverd_source_unknown_i"), std::string::npos)
        << Source;
    const std::string Guard = R"(
static unsigned unavailable_count;
static void observe_unavailable(void) { ++unavailable_count; }
#define __builtin_trap() observe_unavailable()
)";
    const std::string Oracle = R"(
int main(void) {
  if (unobserved() != 7 || unavailable_count != 0) return 1;
  if (guarded_unknown(0) != 17 || unavailable_count != 0) return 2;
  if (guarded_unknown(1) != 0 || unavailable_count != 1) return 3;
  unknown_pair();
  return unavailable_count != 3;
}
)";
    for (llvm::StringRef Level : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(Guard + Source + Oracle,
                                                {Level});
  }
}

TEST(CallExitRegisters, SourceUnknownMarkersDoNotClaimNamedFunctions) {
  llvm::LLVMContext Context;
  llvm::Module Module("unknown-name-collision", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Type = llvm::FunctionType::get(Word, false);
  auto *Named = llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                       "__neverd_source_unknown_i64", Module);
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, "entry", Named));
  Builder.CreateRet(llvm::ConstantInt::get(Word, 17));
  auto *Caller = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "legitimate_name", Module);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Caller));
  auto *Marker = llvm_source_unknown::create(Builder, Word);
  auto *Ordinary = Builder.CreateCall(Named);
  Builder.CreateRet(Ordinary);
  ASSERT_TRUE(llvm_source_unknown::isValue(Marker));
  ASSERT_FALSE(llvm_source_unknown::isValue(Ordinary));
  ASSERT_FALSE(llvm_source_unknown::isFunction(*Named));
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Output));
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
  source_call_execution_test::compileAndRun(
      Source + "\nint main(void) { return legitimate_name() != 17; }\n");
}

TEST(CallExitRegisters, UnknownPhiInputsFailOnlyWhenSelected) {
  for (bool Optimize : {false, true})
    for (bool LLVM : {false, true}) {
      if (!LLVM && Optimize)
        continue;
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(Optimize);
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      if (LLVM) {
        llvm::LLVMContext Context;
        llvm::Module Module("deferred-phi", Context);
        Module.setDataLayout("e-p:64:64");
        auto *Word = llvm::Type::getInt32Ty(Context);
        auto *Function = llvm::Function::Create(
            llvm::FunctionType::get(Word, {Word}, false),
            llvm::GlobalValue::ExternalLinkage, "deferred_scan", Module);
        auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
        auto *Loop = llvm::BasicBlock::Create(Context, "loop", Function);
        auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
        llvm::IRBuilder<> Builder(Entry);
        auto *Unknown = llvm_source_unknown::create(Builder, Word);
        Builder.CreateBr(Loop);
        Builder.SetInsertPoint(Loop);
        auto *Counter = Builder.CreatePHI(Word, 2, "counter");
        auto *Old = Builder.CreatePHI(Word, 2, "old");
        Counter->addIncoming(Function->getArg(0), Entry);
        Old->addIncoming(Unknown, Entry);
        auto *Zero = Builder.CreateICmpEQ(Counter, Builder.getInt32(0));
        auto *Value = Builder.CreateSelect(Zero, Old, Counter, "index");
        auto *Next = Builder.CreateSub(Counter, Builder.getInt32(1));
        Counter->addIncoming(Next, Loop);
        Old->addIncoming(Value, Loop);
        Builder.CreateCondBr(Zero, Exit, Loop);
        Builder.SetInsertPoint(Exit);
        Builder.CreateRet(Value);
        if (Optimize)
          Pipeline::optimizeModule(Module);
        ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(Module, Output));
      } else {
        HighFunc Function;
        Function.Name = "deferred_scan";
        Function.ReturnType = NdType::makeInt(4, false);
        Function.Params.push_back({"arg0", NdType::makeInt(4, false)});
        auto Variable = [](int Id) {
          MedVar Value;
          Value.Kind = MedVar::Temp;
          Value.Id = Id;
          Value.Size = 4;
          Value.TheArch = Arch::X64;
          return Value;
        };
        auto Assign = [](MedVar Dst, ExprPtr Value, bool Phi = false) {
          HighStmt Statement;
          Statement.Kind = StmtKind::Assign;
          Statement.Dst = HighExpr::makeVar(Dst);
          Statement.Val = std::move(Value);
          Statement.IsPhiCopy = Phi;
          return Statement;
        };
        MedVar Parameter = Variable(0);
        Parameter.Kind = MedVar::Param;
        const MedVar Counter = Variable(1), Old = Variable(2),
                     Value = Variable(3);
        auto Zero =
            HighExpr::makeBinop(NdOp::INT_EQUAL, HighExpr::makeVar(Counter),
                                HighExpr::makeConst(0, 4));
        Function.Body = {Assign(Counter, HighExpr::makeVar(Parameter), true),
                         Assign(Old, HighExpr::makeUndef(4), true)};
        HighStmt Loop;
        Loop.Kind = StmtKind::While;
        Loop.Cond = HighExpr::makeConst(1, 1);
        auto Select = std::make_shared<HighExpr>();
        Select->Kind = ExprKind::BinOp;
        Select->Op = NdOp::SELECT;
        Select->Type = NdType::makeInt(4, false);
        Select->Operands = {Zero, HighExpr::makeVar(Old),
                            HighExpr::makeVar(Counter)};
        Loop.Body.push_back(Assign(Value, Select));
        HighStmt Exit;
        Exit.Kind = StmtKind::IfElse;
        Exit.Cond = Zero;
        HighStmt Return;
        Return.Kind = StmtKind::Return;
        Return.RetVal = HighExpr::makeVar(Value);
        Exit.Body = {Return};
        Loop.Body.push_back(Exit);
        Loop.Body.push_back(Assign(
            Counter,
            HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeVar(Counter),
                                HighExpr::makeConst(1, 4)),
            true));
        Loop.Body.push_back(Assign(Old, HighExpr::makeVar(Value), true));
        Function.Body.push_back(Loop);
        ASSERT_TRUE(HighCEmitter().emit({Function}, Output));
      }
      const std::string Guard = R"(
static unsigned unavailable_count;
static void observe_unavailable(void) { ++unavailable_count; }
#define __builtin_trap() observe_unavailable()
)";
      const std::string Oracle = R"(
int main(void) {
  if (deferred_scan(4) != 1 || unavailable_count != 0) return 1;
  if (deferred_scan(1) != 1 || unavailable_count != 0) return 2;
  if (deferred_scan(0) != 0 || unavailable_count != 1) return 3;
  return 0;
}
)";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Guard + Source + Oracle,
            {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
}

TEST(CallExitRegisters, UnknownOnlyJoinCopiesAreNotReads) {
  HighFunc Function;
  Function.Name = "unknown_join";
  Function.ReturnType = NdType::makeInt(4, false);
  Function.Params.push_back({"arg0", NdType::makeInt(4, false)});
  auto Variable = [](int Id) {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.Id = Id;
    Value.Size = 4;
    Value.TheArch = Arch::X64;
    return Value;
  };
  MedVar Parameter = Variable(0);
  Parameter.Kind = MedVar::Param;
  const MedVar Unknown = Variable(1), Join = Variable(2), Other = Variable(3);
  auto Assign = [](MedVar Destination, ExprPtr Value) {
    HighStmt Statement;
    Statement.Kind = StmtKind::Assign;
    Statement.Dst = HighExpr::makeVar(Destination);
    Statement.Val = std::move(Value);
    Statement.IsPhiCopy = true;
    return Statement;
  };
  Function.Body.push_back(Assign(Unknown, HighExpr::makeUndef(4)));
  Function.Body.push_back(Assign(Other, HighExpr::makeUndef(4)));
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = HighExpr::makeVar(Parameter);
  HighStmt Effect;
  Effect.Kind = StmtKind::Call;
  Effect.CallExpr = HighExpr::makeCall("rand", 0x2000, {});
  Branch.Body = {Effect, Assign(Join, HighExpr::makeVar(Unknown))};
  Branch.ElseBody = {Assign(Join, HighExpr::makeVar(Other))};
  Function.Body.push_back(Branch);
  auto Selected = std::make_shared<HighExpr>();
  Selected->Kind = ExprKind::BinOp;
  Selected->Op = NdOp::SELECT;
  Selected->Type = Function.ReturnType;
  Selected->Operands = {HighExpr::makeBinop(NdOp::INT_EQUAL,
                                            HighExpr::makeVar(Parameter),
                                            HighExpr::makeConst(2, 4)),
                        HighExpr::makeVar(Join), HighExpr::makeConst(17, 4)};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = Selected;
  Function.Body.push_back(Return);
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(HighCEmitter().emit({Function}, Output));
  const std::string Guard = R"(
static unsigned unavailable_count;
static void observe_unavailable(void) { ++unavailable_count; }
#define __builtin_trap() observe_unavailable()
)";
  const std::string Oracle = R"(
int main(void) {
  if (unknown_join(0) != 17 || unavailable_count != 0) return 1;
  if (unknown_join(1) != 17 || unavailable_count != 0) return 2;
  if (unknown_join(2) != 0 || unavailable_count != 1) return 3;
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(
        Guard + Source + Oracle,
        {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
}

TEST(CallExitRegisters, AbsorbingBitMasksDoNotReadUnknownOperands) {
  for (unsigned Bytes : {1u, 2u, 4u, 8u})
    for (bool IsAnd : {false, true})
      for (bool Swap : {false, true})
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(Bytes);
          SCOPED_TRACE(IsAnd);
          SCOPED_TRACE(Swap);
          SCOPED_TRACE(LLVM);
          std::string Source;
          llvm::raw_string_ostream Output(Source);
          if (LLVM) {
            llvm::LLVMContext Context;
            llvm::Module Module("unknown-bit-mask", Context);
            Module.setDataLayout("e-p:64:64");
            auto *Word = llvm::IntegerType::get(Context, Bytes * 8);
            auto *Function = llvm::Function::Create(
                llvm::FunctionType::get(Word, {Word}, false),
                llvm::GlobalValue::ExternalLinkage, "masked_unknown", Module);
            llvm::IRBuilder<> Builder(
                llvm::BasicBlock::Create(Context, "entry", Function));
            llvm::Value *Left = llvm_source_unknown::create(Builder, Word);
            llvm::Value *Right = Function->getArg(0);
            if (Swap)
              std::swap(Left, Right);
            Builder.CreateRet(IsAnd ? Builder.CreateAnd(Left, Right)
                                    : Builder.CreateOr(Left, Right));
            ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
            ASSERT_TRUE(LLVMCEmitter().emit(Module, Output));
          } else {
            HighFunc Function;
            Function.Name = "masked_unknown";
            Function.ReturnType = NdType::makeInt(Bytes, false);
            Function.Params.push_back({"arg0", Function.ReturnType});
            MedVar Parameter;
            Parameter.Kind = MedVar::Param;
            Parameter.Id = 0;
            Parameter.Size = Bytes;
            Parameter.TheArch = Arch::X64;
            auto Left = HighExpr::makeUndef(Bytes);
            auto Right = HighExpr::makeVar(Parameter, Function.ReturnType);
            if (Swap)
              std::swap(Left, Right);
            HighStmt Return;
            Return.Kind = StmtKind::Return;
            Return.RetVal = HighExpr::makeBinop(
                IsAnd ? NdOp::INT_AND : NdOp::INT_OR, Left, Right);
            Return.RetVal->Type = Function.ReturnType;
            Function.Body.push_back(Return);
            ASSERT_TRUE(HighCEmitter().emit({Function}, Output));
          }
          const std::string Guard = R"(
static unsigned unavailable_count;
static void observe_unavailable(void) { ++unavailable_count; }
#define __builtin_trap() observe_unavailable()
)";
          const std::string Mask =
              IsAnd ? "0" : "UINT" + std::to_string(Bytes * 8) + "_MAX";
          const std::string Oracle = "\nint main(void) {\n"
                                     "  if (masked_unknown(" +
                                     Mask + ") != " + Mask +
                                     " || unavailable_count != 0) return 1;\n"
                                     "  masked_unknown(" +
                                     (IsAnd ? "1" : "0") +
                                     ");\n"
                                     "  return unavailable_count != 1;\n}\n";
          for (llvm::StringRef Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                Guard + Source + Oracle,
                {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
        }
}

TEST(CallExitRegisters, AbsorbingImmediateMasksDoNotReadEntryRegisters) {
  for (bool IsAnd : {false, true}) {
    const auto Image = makeImage(
        {0x49, 0x83, static_cast<uint8_t>(IsAnd ? 0xE2 : 0xCA),
         static_cast<uint8_t>(IsAnd ? 0 : 0xFF), // and/or r10, immediate
         0x4C, 0x89, 0xD0, 0xC3},                // mov rax,r10; ret
        {{Text, "masked_entry"}}, {});
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(IsAnd);
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(NoOpt);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Text};
        Options.LiftMode = LLVM;
        Options.SourceProjection = true;
        Options.NoOpt = NoOpt;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output));
        } else
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output));
        EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
        const std::string Oracle =
            "\nint main(void) { return (uint64_t)masked_entry() != " +
            std::string(IsAnd ? "0" : "UINT64_MAX") + "; }\n";
        for (llvm::StringRef Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Source + Oracle,
              {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
  }
}

TEST(CallExitRegisters, BitScanLoopsDeferClobberedDestinationReads) {
  for (bool Forward : {false, true})
    for (bool Initialized : {false, true})
      for (bool WrapResult : {false, true}) {
        SCOPED_TRACE(Forward);
        SCOPED_TRACE(Initialized);
        SCOPED_TRACE(WrapResult);
        // Keep the source in EBX across rand(), which leaves ECX unavailable.
        // Repeated BSF/BSR removes one set bit at a time. The zero-source exit
        // observes the preserved destination; a nonempty input defines it
        // first.
        std::vector<uint8_t> Code = {0x53, 0x48, 0x83, 0xEC, 0x20, 0x89, 0xCB};
        const auto Call = throughSlot(Text + Code.size(), slot(0));
        Code.insert(Code.end(), Call.begin(), Call.end());
        if (Initialized)
          Code.insert(Code.end(), {0xB9, 0x10, 0x32, 0x54, 0x76});
        Code.insert(Code.end(),
                    {0x0F, static_cast<uint8_t>(Forward ? 0xBC : 0xBD), 0xCB,
                     0x74, 0x0B, 0xB8, 1, 0, 0, 0, 0xD3, 0xE0, 0x31, 0xC3, 0xEB,
                     0xF0, 0x89, 0xC8});
        // The folded zero-input return must retain unknown observations even
        // inside a scalar expression, not only as a bare return value.
        if (WrapResult)
          Code.insert(Code.end(), {0x83, 0xF0, 0x01}); // xor eax,1
        Code.insert(Code.end(), {0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3});
        auto Image = makeImage(Code, {{Text, "scan_bits"}}, {"rand"});
        ASSERT_TRUE(Image.recordImportStorageSlot(
            Iat, "rand", 0, ImportStorageEvidence::LoaderBind));
        for (bool LLVM : {false, true})
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(LLVM);
            SCOPED_TRACE(NoOpt);
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.OnlyFunctionEntries = {Text};
            Options.LiftMode = LLVM;
            Options.SourceProjection = true;
            Options.NoOpt = NoOpt;
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            std::string Source;
            llvm::raw_string_ostream Output(Source);
            CEmitterOptions COptions;
            COptions.TheArch = Image.Arch;
            COptions.Format = Image.Format;
            COptions.Image = &Image;
            if (LLVM) {
              ASSERT_TRUE(Result.LlvmModule);
              ASSERT_FALSE(
                  llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
              ASSERT_TRUE(
                  LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
            } else
              ASSERT_TRUE(
                  HighCEmitter().emit(Result.HighFuncs, Output, COptions));
            const std::string Guard = R"(
static unsigned unavailable_count;
static void observe_unavailable(void) { ++unavailable_count; }
#define __builtin_trap() observe_unavailable()
)";
            const std::string Oracle = R"(
int main(void) {
  const uint32_t result_mask = )" + std::string(WrapResult ? "1" : "0") +
                                       R"(;
  for (unsigned bit = 0; bit < 32; ++bit)
    if (scan_bits((uint32_t)1 << bit) != (bit ^ result_mask) || unavailable_count) return 1;
  if (scan_bits(0x20000081u) != ()" + std::string(Forward ? "29" : "0") +
                                       R"( ^ result_mask)
      || unavailable_count) return 2;
  if (scan_bits(0) != ()" + std::string(Initialized ? "0x76543210u" : "0") +
                                       R"( ^ result_mask)
      || unavailable_count != )" + std::string(Initialized ? "0" : "1") +
                                       R"()
    return 3;
  return 0;
}
)";
            for (llvm::StringRef Level : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  Guard + Source + Oracle,
                  {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
          }
      }
}

TEST(CallExitRegisters, DeferredPhiCopiesPreserveParallelEdgesAndWidths) {
  llvm::LLVMContext Context;
  llvm::Module Module("deferred-phi-swaps", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Word = llvm::Type::getInt32Ty(Context);
  auto *Byte = llvm::Type::getInt8Ty(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Word, {Word, Word}, false),
      llvm::GlobalValue::ExternalLinkage, "swap_unknown", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", Function);
  auto *Step = llvm::BasicBlock::Create(Context, "step", Function);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  auto *Read = llvm::BasicBlock::Create(Context, "read", Function);
  auto *Skip = llvm::BasicBlock::Create(Context, "skip", Function);
  llvm::IRBuilder<> Builder(Entry);
  auto *Unknown = llvm_source_unknown::create(Builder, Byte);
  auto *UnusedWide = Builder.CreateZExt(Unknown, Word);
  Builder.CreateBr(Loop);
  Builder.SetInsertPoint(Loop);
  auto *Counter = Builder.CreatePHI(Word, 2, "counter");
  auto *Left = Builder.CreatePHI(Byte, 2, "left");
  auto *Right = Builder.CreatePHI(Byte, 2, "right");
  auto *Unused = Builder.CreatePHI(Word, 2, "unused");
  Counter->addIncoming(Function->getArg(0), Entry);
  Left->addIncoming(Unknown, Entry);
  Right->addIncoming(Builder.getInt8(0xF2), Entry);
  Unused->addIncoming(UnusedWide, Entry);
  Builder.CreateCondBr(Builder.CreateICmpEQ(Counter, Builder.getInt32(0)), Exit,
                       Step);
  Builder.SetInsertPoint(Step);
  Counter->addIncoming(Builder.CreateSub(Counter, Builder.getInt32(1)), Step);
  Left->addIncoming(Right, Step);
  Right->addIncoming(Left, Step);
  // A copy-only cycle has no observation or payload declaration.
  Unused->addIncoming(
      Builder.CreateZExt(Builder.CreateTrunc(Unused, Byte), Word), Step);
  Builder.CreateBr(Loop);
  Builder.SetInsertPoint(Exit);
  // The casts are transport too. Merely crossing them on the skipped read
  // must not observe the unavailable byte or lose sign extension.
  auto *Signed = Builder.CreateSExt(Right, Word);
  Builder.CreateCondBr(
      Builder.CreateICmpNE(Function->getArg(1), Builder.getInt32(0)), Read,
      Skip);
  Builder.SetInsertPoint(Read);
  Builder.CreateRet(Signed);
  Builder.SetInsertPoint(Skip);
  Builder.CreateRet(Builder.getInt32(17));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Output));
  const std::string Guard = R"(
static unsigned unavailable_count;
static void observe_unavailable(void) { ++unavailable_count; }
#define __builtin_trap() observe_unavailable()
)";
  const std::string Oracle = R"(
int main(void) {
  for (unsigned n = 0; n < 8; ++n) {
    if (swap_unknown(n, 0) != 17 || unavailable_count) return 1;
    int32_t result = swap_unknown(n, 1);
    if (n & 1) {
      if (result != 0 || unavailable_count != 1) return 2;
    } else if (result != -14 || unavailable_count) return 3;
    unavailable_count = 0;
  }
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(
        Guard + Source + Oracle,
        {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
}

TEST(CallExitRegisters, TypedMemberStoresRetainUnknownExpressions) {
  for (const TypeRef &Field :
       {NdType::makeInt(8, false), NdType::makePtr(NdType::makeVoid()),
        NdType::makeFloat(8)}) {
    auto Record = NdType::makeNamedRecord("unknown_destination", 8);
    Record->FieldDisplayNames = {"value"};
    Record->FieldDisplayOffsets = {0};
    Record->FieldDisplayTypes = {Field};
    const auto Pointer = NdType::makePtr(Record);
    HighFunc Function;
    Function.Name = "observe_unknown_member";
    Function.Entry = Text;
    Function.ReturnType = NdType::makeVoid();
    Function.Params.push_back({"arg0", Pointer});
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 0;
    Parameter.Size = 8;
    Parameter.TheArch = Arch::X64;
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = HighExpr::makeVar(Parameter, Pointer);
    Store.StoreVal = HighExpr::makeUndef(8);
    Store.StoreVal->Type = Field;
    Function.Body.push_back(Store);
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    ASSERT_TRUE(HighCEmitter().emit({Function}, Output));
    const std::string FieldSpelling = Field->Kind == NdTypeKind::Ptr ? "void*"
                                      : Field->Kind == NdTypeKind::Float
                                          ? "double"
                                          : "uint64_t";
    // Pointer-only display records are supplied by the caller's header.
    const std::string Guard =
        "#include <stdint.h>\nstruct unknown_destination { " + FieldSpelling +
        " value; };\n" + R"(
#include <stdlib.h>
static void unavailable(void) { exit(0); }
#define __builtin_trap() unavailable()
)";
    const std::string Oracle = R"(
int main(void) {
  struct unknown_destination destination = {0};
  observe_unknown_member(&destination);
  return 1;
}
)";
    for (llvm::StringRef Level : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          Guard + Source + Oracle,
          {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
}

TEST(CallExitRegisters, OpaqueExitDoesNotInventAStoredValue) {
  constexpr va_t Producer = Text + 0x80;
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    const bool Windows = Format == BinaryFormat::COFF;
    const uint8_t Frame = Windows ? 32 : 0;
    std::vector<uint8_t> Code(0x110, 0xCC);
    // CPUID supplies dynamic extra outputs outside the ordinary source ABI.
    // Until a call-state contract recovers them, an observed store must remain
    // explicitly unavailable through source optimization and both C routes.
    std::vector<uint8_t> Caller = {
        0x53, 0x48, 0x89, static_cast<uint8_t>(Windows ? 0xCB : 0xFB),
        0x48, 0x83, 0xEC, Frame,
        0xE8};
    const auto Displacement = rel32(Text + Caller.size() + 4, Producer);
    Caller.insert(Caller.end(), Displacement.begin(), Displacement.end());
    const std::vector<uint8_t> Suffix = {
        0x48,  0x89, static_cast<uint8_t>(Windows ? 0x0B : 0x3B),
        0x48,  0x83, 0xC4,
        Frame, 0x5B, 0xC3};
    Caller.insert(Caller.end(), Suffix.begin(), Suffix.end());
    put(Code, Text, Caller);
    std::vector<uint8_t> Callee = {0x53, 0x31, 0xC0, 0x31,
                                   0xC9, 0x0F, 0xA2, 0x5B};
    if (!Windows)
      Callee.insert(Callee.end(), {0x89, 0xCF});
    Callee.insert(Callee.end(), {0xB8, 1, 0, 0, 0, 0xC3});
    put(Code, Producer, Callee);
    for (unsigned I = 0; I < 6; ++I)
      put(Code, Text + 0xA0 + 16 * I,
          {0xB8, static_cast<uint8_t>(I + 2), 0, 0, 0, 0xC3});
    auto Image = makeImage(Code,
                           {{Text, "observe_exit"},
                            {Producer, "produce_exit"},
                            {Text + 0xA0, "independent_a"},
                            {Text + 0xB0, "independent_b"},
                            {Text + 0xC0, "independent_c"},
                            {Text + 0xD0, "independent_d"},
                            {Text + 0xE0, "independent_e"},
                            {Text + 0xF0, "independent_f"}},
                           {});
    Image.Format = Format;
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true})
        for (bool Shards : {false, true}) {
          if (Shards && !LLVM)
            continue;
          SCOPED_TRACE(Shards);
          SCOPED_TRACE(LLVM);
          SCOPED_TRACE(NoOpt);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = {Text, Producer};
          if (Shards)
            for (unsigned I = 0; I < 6; ++I)
              Options.OnlyFunctionEntries.insert(Text + 0xA0 + 16 * I);
          Options.LiftMode = LLVM;
          Options.SourceProjection = true;
          Options.NoOpt = NoOpt;
          auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          ASSERT_EQ(Result.MedFuncs.size(), Shards ? 8u : 2u);
          std::string Source;
          llvm::raw_string_ostream Output(Source);
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            ASSERT_TRUE(LLVMCEmitter().emit(
                *Result.LlvmModule, Output, COptions, nullptr, nullptr,
                Result.LlvmModule->getFunction("observe_exit")));
          } else {
            std::erase_if(Result.HighFuncs, [](const HighFunc &Function) {
              return Function.Entry != Text;
            });
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Output, COptions));
          }
          const auto Name = Source.find(" produce_exit(");
          ASSERT_NE(Name, std::string::npos) << Source;
          const auto Start = Source.rfind('\n', Name) + 1;
          const auto End = Source.find(';', Name);
          ASSERT_NE(End, std::string::npos) << Source;
          const std::string Provider = Source.substr(Start, End - Start) +
                                       " { ++observed_calls; return 1; }\n";
          const std::string Guard = R"(
#include <stdint.h>
#include <stdlib.h>
static uint64_t observed_output = 999;
static unsigned observed_calls;
static void unavailable(void) {
  exit(observed_output == 999 && observed_calls == 1 ? 0 : 2);
}
#define __builtin_trap() unavailable()
)";
          const std::string Oracle = R"(
int main(void) {
  observe_exit(&observed_output);
  return 1;
}
)";
          for (llvm::StringRef Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                Guard + Source + Provider + Oracle,
                {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
        }
  }
}

/// HighC for the functions at \p Entries of \p Img.
std::string liftEntries(const BinaryImage &Img, std::set<va_t> Entries) {
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, std::move(Entries));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  return Source;
}

/// The definition of function \p Name in \p Source: the line naming it that
/// opens a body, not a declaration before it.
std::string body(const std::string &Source, const std::string &Name) {
  const std::string Head = " " + Name + "(";
  for (size_t At = Source.find(Head); At != std::string::npos;
       At = Source.find(Head, At + 1)) {
    const size_t LineEnd = Source.find('\n', At);
    if (LineEnd == std::string::npos || Source[LineEnd - 1] != '{')
      continue;
    const size_t Close = Source.find("\n}", LineEnd);
    if (Close == std::string::npos)
      return {};
    const size_t Start = Source.rfind('\n', At) + 1;
    return Source.substr(Start, Close + 2 - Start);
  }
  return {};
}

unsigned family(uint64_t RegOff) { return static_cast<unsigned>(RegOff / 8); }

TEST(Win64CallContract, CpuidScratchExchangeAddsNoPhantomParameters) {
  constexpr va_t Query = Text + 0x40;
  std::vector<uint8_t> Code(0x80, 0xCC);
  std::vector<uint8_t> Call = {0xE8};
  for (uint8_t B : rel32(Text + 9, Query))
    Call.push_back(B);
  put(Code, Text, framed({Call}));
  put(Code, Query, {0x31, 0xC0,             // xor eax, eax
                    0x31, 0xC9,             // xor ecx, ecx
                    0x4C, 0x87, 0xC3,       // xchg rbx, r8
                    0x0F, 0xA2,             // cpuid
                    0x4C, 0x87, 0xC3,       // xchg rbx, r8
                    0x44, 0x31, 0xC0,       // xor eax, r8d
                    0x31, 0xC8, 0x31, 0xD0, // xor eax, ecx; xor eax, edx
                    0xC3});                 // ret
  const BinaryImage Img =
      makeImage(Code, {{Text, "query_wrapper"}, {Query, "cpu_query"}}, {});
  for (bool IncludeCallee : {false, true}) {
    SCOPED_TRACE(IncludeCallee);
    const std::set<va_t> Entries =
        IncludeCallee ? std::set<va_t>{Text, Query} : std::set<va_t>{Text};
    const std::string Source = liftEntries(Img, Entries);
    EXPECT_NE(Source.find("query_wrapper(void)"), std::string::npos) << Source;
    EXPECT_NE(Source.find("cpu_query()"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
    if (IncludeCallee)
      EXPECT_NE(Source.find("cpu_query(void)"), std::string::npos) << Source;

    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.LiftMode = true;
    Opts.SourceProjection = true;
    Opts.NoOpt = true;
    Opts.OnlyFunctionEntries = Entries;
    auto Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    if (IncludeCallee) {
      auto *Callee = Result.LlvmModule->getFunction("cpu_query");
      ASSERT_NE(Callee, nullptr);
      EXPECT_EQ(Callee->arg_size(), 0u);
    }
    auto *Caller = Result.LlvmModule->getFunction("query_wrapper");
    ASSERT_NE(Caller, nullptr);
    EXPECT_EQ(Caller->arg_size(), 0u);
    unsigned Calls = 0;
    for (auto &Block : *Caller)
      for (auto &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
            Call && (!Call->getCalledFunction() ||
                     !Call->getCalledFunction()->isIntrinsic())) {
          ++Calls;
          EXPECT_EQ(Call->arg_size(), 0u);
        }
    EXPECT_EQ(Calls, 1u);
  }
}

TEST(Win64CallContract, ACallThroughTheImportSlotPassesTheCallersArguments) {
  // wrap(s, f) calls fputs through its slot without writing RCX or RDX.
  constexpr va_t Wrap = Text;
  std::vector<uint8_t> Code(0x20, 0xCC);
  put(Code, Wrap, framed({throughSlot(Wrap + 4, slot(0))}));
  const BinaryImage Img = makeImage(Code, {{Wrap, "wrap"}}, {"fputs"});
  const std::string Source = liftEntries(Img, {Wrap});
  const std::string Body = body(Source, "wrap");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(
      Body.find("fputs((void *)(uintptr_t)arg0, (void *)(uintptr_t)arg1)"),
      std::string::npos)
      << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(Win64CallContract, LLVMImportCallKeepsUnwrittenLeadingArgument) {
  // run_pipe(command): RCX is forwarded, only the second argument is set.
  constexpr va_t Wrap = Text;
  std::vector<uint8_t> Code(0x20, 0xCC);
  put(Code, Wrap,
      framed({{0xBA, 0x45, 0x23, 0x01, 0x00}, throughSlot(Wrap + 9, slot(0))}));
  BinaryImage Img = makeImage(Code, {{Wrap, "wrap"}}, {"_popen"});
  ASSERT_TRUE(Img.recordImportStorageSlot(slot(0), "_popen", 0,
                                          ImportStorageEvidence::PointerTable));
  for (bool NoOpt : {true, false}) {
    SCOPED_TRACE(NoOpt);
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.LiftMode = true;
    Opts.SourceProjection = true;
    Opts.NoOpt = NoOpt;
    Opts.OnlyFunctionEntries = {Wrap};
    auto Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    auto *Caller = Result.LlvmModule->getFunction("wrap");
    auto *Callee = Result.LlvmModule->getFunction("_popen");
    ASSERT_NE(Caller, nullptr);
    ASSERT_NE(Callee, nullptr);
    ASSERT_EQ(Caller->arg_size(), 1u);
    ASSERT_EQ(Callee->arg_size(), 2u);
    unsigned Calls = 0;
    for (auto &Block : *Caller)
      for (auto &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
            Call && Call->getCalledFunction() == Callee) {
          ++Calls;
          EXPECT_EQ(Call->getFunctionType(), Callee->getFunctionType());
          ASSERT_EQ(Call->arg_size(), 2u);
          EXPECT_EQ(Call->getArgOperand(0), Caller->getArg(0));
          auto *Second =
              llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(1));
          ASSERT_NE(Second, nullptr);
          EXPECT_EQ(Second->getZExtValue(), 0x12345u);
        }
    EXPECT_EQ(Calls, 1u);
  }
}

TEST(Win64CallContract, SavedXmmRegistersDoNotShiftTheFifthParameter) {
  // Sum five integer arguments while saving/restoring nonvolatile XMM6/7.
  // The save slots are frame state, never extra parameters between R9 and
  // the incoming stack argument.
  const std::vector<uint8_t> Code{
      0x48, 0x83, 0xEC, 0x48,       // sub rsp,72
      0x0F, 0x29, 0x74, 0x24, 0x20, // movaps [rsp+32],xmm6
      0x0F, 0x29, 0x7C, 0x24, 0x30, // movaps [rsp+48],xmm7
      0x66, 0x0F, 0xEF, 0xF6,       // pxor xmm6,xmm6
      0x66, 0x0F, 0xEF, 0xFF,       // pxor xmm7,xmm7
      0x48, 0x8B, 0x44, 0x24, 0x70, // mov rax,[rsp+112]
      0x48, 0x01, 0xC8,             // add rax,rcx
      0x48, 0x01, 0xD0,             // add rax,rdx
      0x4C, 0x01, 0xC0,             // add rax,r8
      0x4C, 0x01, 0xC8,             // add rax,r9
      0x0F, 0x28, 0x74, 0x24, 0x20, // movaps xmm6,[rsp+32]
      0x0F, 0x28, 0x7C, 0x24, 0x30, // movaps xmm7,[rsp+48]
      0x48, 0x83, 0xC4, 0x48, 0xC3};
  const auto Image = makeImage(Code, {{Text, "sum_five"}}, {});
  for (bool NoOpt : {true, false}) {
    SCOPED_TRACE(NoOpt);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = true;
    Options.SourceProjection = true;
    Options.NoOpt = NoOpt;
    Options.OnlyFunctionEntries = {Text};
    const auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    auto *Function = Result.LlvmModule->getFunction("sum_five");
    ASSERT_NE(Function, nullptr);
    ASSERT_EQ(Function->arg_size(), 5u);
    for (const auto &Argument : Function->args())
      EXPECT_TRUE(Argument.getType()->isIntegerTy(64));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, {}));
    const std::string Driver = R"(
int main(void) {
  for (uint64_t i = 0; i != 1024; ++i) {
    uint64_t a = i * UINT64_C(0x9e3779b97f4a7c15);
    uint64_t b = ~a, c = i << 32, d = i * i, e = (i + 1) << 40;
    if (sum_five(a, b, c, d, e) != a + b + c + d + e) return 1;
  }
  return 0;
}
)";
    for (llvm::StringRef Optimization : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          Source + Driver,
          {Optimization, "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
}

TEST(Win64CallContract, AThunkJumpingThroughTheSlotForwardsTheArguments) {
  // A MinGW thunk `jmp [__imp_fputs]`, called by wrap(s, f) without writing
  // RCX or RDX: both read the parameters fputs reads.
  constexpr va_t Wrap = Text, Thunk = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> Call = {0xE8};
  for (uint8_t B : rel32(Wrap + 4 + 5, Thunk))
    Call.push_back(B);
  put(Code, Wrap, framed({Call}));
  put(Code, Thunk, throughSlot(Thunk, slot(0), /*Jump=*/true));
  const BinaryImage Img =
      makeImage(Code, {{Wrap, "wrap"}, {Thunk, "fputs_thunk"}}, {"fputs"});
  const std::string Source = liftEntries(Img, {Wrap, Thunk});
  const std::string ThunkBody = body(Source, "fputs_thunk");
  ASSERT_FALSE(ThunkBody.empty()) << Source;
  EXPECT_NE(
      ThunkBody.find("fputs((void *)(uintptr_t)arg0, (void *)(uintptr_t)arg1)"),
      std::string::npos)
      << ThunkBody;
  const std::string WrapBody = body(Source, "wrap");
  ASSERT_FALSE(WrapBody.empty()) << Source;
  EXPECT_NE(WrapBody.find("fputs_thunk(arg0, arg1)"), std::string::npos)
      << WrapBody;
  for (const std::string &Body : {ThunkBody, WrapBody})
    EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(Win64CallContract, AThunkForwardsTheStackArgumentsItsImportReads) {
  // A MinGW thunk `jmp [__imp___getmainargs]`, called by wrap(a, b, c, d, e)
  // with its own arguments: __getmainargs reads a fifth on the stack, which
  // wrap stores at [rsp+20h] and the thunk leaves where its caller put it.
  constexpr va_t Wrap = Text, Thunk = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> Body = {
      0x48, 0x83, 0xEC, 0x38,       // sub rsp, 38h
      0x48, 0x8B, 0x44, 0x24, 0x60, // mov rax, [rsp+60h]
      0x48, 0x89, 0x44, 0x24, 0x20, // mov [rsp+20h], rax
      0xE8};                        // call Thunk
  for (uint8_t B : rel32(Wrap + Body.size() + 4, Thunk))
    Body.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x38, 0xC3}) // add rsp, 38h; ret
    Body.push_back(B);
  put(Code, Wrap, Body);
  put(Code, Thunk, throughSlot(Thunk, slot(0), /*Jump=*/true));
  BinaryImage Img = makeImage(
      Code, {{Wrap, "wrap"}, {Thunk, "getmainargs_thunk"}}, {"__getmainargs"});
  // The PE loader records the thunk as the import's stub.
  ASSERT_TRUE(Img.recordImportStub(Thunk, 0));
  const std::string Source = liftEntries(Img, {Wrap, Thunk});
  const std::string ThunkBody = body(Source, "getmainargs_thunk");
  ASSERT_FALSE(ThunkBody.empty()) << Source;
  EXPECT_NE(ThunkBody.find("arg4)"), std::string::npos) << ThunkBody;
  const std::string WrapBody = body(Source, "wrap");
  ASSERT_FALSE(WrapBody.empty()) << Source;
  // A call to the stub names the import.
  EXPECT_NE(WrapBody.find("__getmainargs("), std::string::npos) << WrapBody;
  EXPECT_NE(WrapBody.find("arg4)"), std::string::npos) << WrapBody;
  for (const std::string &Body : {ThunkBody, WrapBody})
    EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(Win64CallContract, OwnedX87ContextKeepsCallSetupAndTailArguments) {
  // Win64 owns the x87 stack as one context and publishes it around every
  // call. That transport neither ends the stores that set a call's stack
  // arguments nor separates a tail jump from its return.
  constexpr va_t Caller = Text, Thunk = Text + 0x60;
  std::vector<uint8_t> Code(0x80, 0xCC);
  std::vector<uint8_t> CallerCode = {
      0x48, 0x83, 0xEC, 0x48,                         // sub rsp, 48h
      0xD9, 0xE8, 0xDD, 0xD8,                         // fld1; fstp st(0)
      0x48, 0xC7, 0x44, 0x24, 0x20, 0xF5, 0x01, 0, 0, // mov [rsp+20h], 501
      0x48, 0xC7, 0x44, 0x24, 0x28, 0x5A, 0x02, 0, 0, // mov [rsp+28h], 602
      0x48, 0xC7, 0x44, 0x24, 0x30, 0xBF, 0x02, 0, 0, // mov [rsp+30h], 703
      0x48, 0xC7, 0x44, 0x24, 0x38, 0x24, 0x03, 0, 0, // mov [rsp+38h], 804
      0x41, 0xB9, 4,    0,    0,    0,                // mov r9d, 4
      0x41, 0xB8, 3,    0,    0,    0,                // mov r8d, 3
      0xBA, 2,    0,    0,    0,                      // mov edx, 2
      0xB9, 1,    0,    0,    0};                     // mov ecx, 1
  const auto Call = throughSlot(Caller + CallerCode.size(), slot(0));
  CallerCode.insert(CallerCode.end(), Call.begin(), Call.end());
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x48, 0xC3}) // add rsp, 48h; ret
    CallerCode.push_back(B);
  put(Code, Caller, CallerCode);
  // The thunk changes x87 state, then jumps to the import with its own
  // caller's arguments, four of them in that caller's outgoing stack area.
  std::vector<uint8_t> ThunkCode = {0xD9, 0xE8, 0xDD, 0xD8}; // fld1; fstp st(0)
  const auto Jump =
      throughSlot(Thunk + ThunkCode.size(), slot(0), /*Jump=*/true);
  ThunkCode.insert(ThunkCode.end(), Jump.begin(), Jump.end());
  put(Code, Thunk, ThunkCode);
  const BinaryImage Img = makeImage(
      Code, {{Caller, "caller"}, {Thunk, "x87_thunk"}}, {"RtlVirtualUnwind"});
  const std::string Source = liftEntries(Img, {Caller, Thunk});
  EXPECT_NE(Source.find("neverd_x87_"), std::string::npos) << Source;
  const std::string CallerBody = body(Source, "caller");
  ASSERT_FALSE(CallerBody.empty()) << Source;
  EXPECT_NE(CallerBody.find("RtlVirtualUnwind("), std::string::npos)
      << CallerBody;
  // 501, 602, 703 and 804, as C writes numbers by default.
  for (const char *Value : {"0x1F5", "0x25A", "0x2BF", "0x324"})
    EXPECT_NE(CallerBody.find(Value), std::string::npos) << Value << '\n'
                                                         << CallerBody;
  const std::string ThunkBody = body(Source, "x87_thunk");
  ASSERT_FALSE(ThunkBody.empty()) << Source;
  EXPECT_NE(ThunkBody.find("arg7)"), std::string::npos) << ThunkBody;
  for (const std::string &Text : {CallerBody, ThunkBody})
    EXPECT_EQ(Text.find("unknown value"), std::string::npos) << Text;
}

TEST(Win64CallContract, TheRuntimesOwnPrototypeFixesWhatItsSlotReads) {
  // One function calls each import through its slot.  calloc reads RCX and
  // RDX by the C library's arity, _initterm by the Windows C runtime's own
  // prototype, and _lock reads RCX alone as that runtime declares it.
  // __getmainargs reads all four registers and a fifth argument on the
  // stack.  _pipe(fds, size, mode) has no fixed reads: POSIX pipe(fds) is
  // another function.
  const std::vector<const char *> Imports = {"calloc", "_initterm", "_lock",
                                             "__getmainargs", "_pipe"};
  constexpr va_t Caller = Text;
  std::vector<uint8_t> Code(0x48, 0xCC);
  std::vector<std::vector<uint8_t>> Calls;
  for (size_t I = 0; I < Imports.size(); ++I)
    Calls.push_back(throughSlot(Caller + 4 + 6 * I, slot(I)));
  put(Code, Caller, framed(Calls));
  const BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, Imports);
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {Caller});
  const auto &Reads = Result.CallEntryReadGPRs;
  for (size_t I = 0; I < 2; ++I) {
    SCOPED_TRACE(Imports[I]);
    ASSERT_TRUE(Reads.count(slot(I)));
    EXPECT_EQ(Reads.at(slot(I))[family(x86reg::RCX)], 8u);
    EXPECT_EQ(Reads.at(slot(I))[family(x86reg::RDX)], 8u);
    EXPECT_EQ(Reads.at(slot(I))[family(x86reg::R8)], 0u);
  }
  ASSERT_TRUE(Reads.count(slot(2)));
  EXPECT_EQ(Reads.at(slot(2))[family(x86reg::RCX)], 8u);
  EXPECT_EQ(Reads.at(slot(2))[family(x86reg::RDX)], 0u);
  ASSERT_TRUE(Reads.count(slot(3)));
  EXPECT_EQ(Reads.at(slot(3))[family(x86reg::R9)], 8u);
  // Stack arguments count positions, the registers' included.
  ASSERT_TRUE(Result.CallEntryStackArgs.count(slot(3)));
  EXPECT_EQ(Result.CallEntryStackArgs.at(slot(3)), 5);
  EXPECT_EQ(Result.CallEntryStackArgs.at(slot(0)), 0);
  EXPECT_FALSE(Reads.count(slot(4)));
}

TEST(Win64CallContract, CallersPassWhatAnExportedRoutinesPrototypeDeclares) {
  // The image exports a system call stub that reads only RCX itself: the
  // kernel's dispatcher reads the rest of the five parameters its prototype
  // declares, the fifth from the stack.  It also exports a routine whose
  // body reads R9 past the three parameters its prototype declares, which
  // its summary keeps.
  constexpr va_t Caller = Text, Stub = Text + 0x30, Extra = Text + 0x40;
  std::vector<uint8_t> Code(0x48, 0xCC);
  // sub rsp, 38h; mov qword [rsp+20h], 0; mov r9d, 4; lea r8, [rsp+30h];
  // mov edx, 12; mov rcx, -2; call Stub; add rsp, 38h; ret
  std::vector<uint8_t> CallerCode = {
      0x48, 0x83, 0xEC, 0x38, 0x48, 0xC7, 0x44, 0x24, 0x20, 0x00,
      0x00, 0x00, 0x00, 0x41, 0xB9, 0x04, 0x00, 0x00, 0x00, 0x4C,
      0x8D, 0x44, 0x24, 0x30, 0xBA, 0x0C, 0x00, 0x00, 0x00, 0x48,
      0xC7, 0xC1, 0xFE, 0xFF, 0xFF, 0xFF, 0xE8};
  for (uint8_t B : rel32(Caller + CallerCode.size() + 4, Stub))
    CallerCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x38, 0xC3})
    CallerCode.push_back(B);
  put(Code, Caller, CallerCode);
  // mov r10, rcx; mov eax, 5; ret
  put(Code, Stub, {0x4C, 0x8B, 0xD1, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3});
  // mov rax, r9; ret
  put(Code, Extra, {0x4C, 0x89, 0xC8, 0xC3});
  BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, {});
  Img.Exports.push_back({"QueryThing", 1, Stub});
  Img.Exports.push_back({"AllocateThing", 2, Extra});
  class ExportedPrototypes : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
      if (Addr != Stub && Addr != Extra)
        return std::nullopt;
      FunctionSym Function;
      Function.Addr = Addr;
      Function.PrototypeLibrary = "windows-user-x64";
      const TypeRef Pointer = NdType::makePtr(NdType::makeVoid());
      if (Addr == Stub) {
        Function.Name = "QueryThing";
        Function.ReturnType = NdType::makeInt(4, true);
        Function.Params = {{"Handle", Pointer},
                           {"Class", NdType::makeInt(4, true)},
                           {"Buffer", Pointer},
                           {"Length", NdType::makeInt(4)},
                           {"Returned", Pointer}};
      } else {
        Function.Name = "AllocateThing";
        Function.ReturnType = Pointer;
        Function.Params = {{"Heap", Pointer},
                           {"Flags", NdType::makeInt(4)},
                           {"Size", NdType::makeInt(8)}};
      }
      return Function;
    }
    bool hasInfo() const override { return true; }
  } Dbg;
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Caller, Extra};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts, &Dbg);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.CallEntryStackArgs.count(Stub));
  EXPECT_EQ(Result.CallEntryStackArgs.at(Stub), 5);
  // Each parameter is read as wide as the prototype declares it, which a
  // caller setting only the low bytes of an argument register fills.
  const auto &Reads = Result.CallEntryReadGPRs;
  ASSERT_TRUE(Reads.count(Stub));
  EXPECT_EQ(Reads.at(Stub)[family(x86reg::RCX)], 8u);
  EXPECT_EQ(Reads.at(Stub)[family(x86reg::RDX)], 4u);
  EXPECT_EQ(Reads.at(Stub)[family(x86reg::R8)], 8u);
  EXPECT_EQ(Reads.at(Stub)[family(x86reg::R9)], 4u);
  ASSERT_TRUE(Reads.count(Extra));
  EXPECT_EQ(Reads.at(Extra)[family(x86reg::RCX)], 8u);
  EXPECT_EQ(Reads.at(Extra)[family(x86reg::RDX)], 4u);
  EXPECT_EQ(Reads.at(Extra)[family(x86reg::R9)], 8u);
}

TEST(Win64CallContract, TheStartupsKernel32CallsReadWhatTheyDeclare) {
  // MSVC's startup code calls these through their slots.  IsDebuggerPresent
  // takes no argument, GetModuleHandleW one, and RtlVirtualUnwind four in
  // registers and four on the stack.
  const std::vector<const char *> Imports = {
      "IsDebuggerPresent", "GetModuleHandleW", "RtlVirtualUnwind"};
  constexpr va_t Caller = Text;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<std::vector<uint8_t>> Calls;
  for (size_t I = 0; I < Imports.size(); ++I)
    Calls.push_back(throughSlot(Caller + 4 + 6 * I, slot(I)));
  put(Code, Caller, framed(Calls));
  const BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, Imports);
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {Caller});
  const auto &Reads = Result.CallEntryReadGPRs;
  if (Reads.count(slot(0)))
    for (unsigned Bytes : Reads.at(slot(0)))
      EXPECT_EQ(Bytes, 0u);
  ASSERT_TRUE(Reads.count(slot(1)));
  EXPECT_EQ(Reads.at(slot(1))[family(x86reg::RCX)], 8u);
  EXPECT_EQ(Reads.at(slot(1))[family(x86reg::RDX)], 0u);
  ASSERT_TRUE(Reads.count(slot(2)));
  EXPECT_EQ(Reads.at(slot(2))[family(x86reg::R9)], 8u);
  ASSERT_TRUE(Result.CallEntryStackArgs.count(slot(2)));
  EXPECT_EQ(Result.CallEntryStackArgs.at(slot(2)), 8);
  const std::string Source = body(liftEntries(Img, {Caller}), "caller");
  EXPECT_NE(Source.find("IsDebuggerPresent();"), std::string::npos) << Source;
}

TEST(Win64CallContract, ACallNamesTheFunctionNotItsObjectsSection) {
  // MinGW links each object's code with the section's own symbol where the
  // object's first function starts: `.text` comes first in the symbol table,
  // then _setargv.  A call from a function decompiled alone names _setargv,
  // as the function decompiled alone is named.
  constexpr va_t Caller = Text;
  constexpr va_t Callee = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> Call = {0xE8}; // call rel32
  for (uint8_t B : rel32(Caller + 9, Callee))
    Call.push_back(B);
  put(Code, Caller, framed({Call}));
  put(Code, Callee, {0x31, 0xC0, 0xC3}); // xor eax, eax; ret
  BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, {"calloc"});
  Symbol Section;
  Section.Name = ".text";
  Section.Addr = Callee;
  Img.Symbols.push_back(std::move(Section));
  Symbol Function;
  Function.Name = "_setargv";
  Function.Addr = Callee;
  Function.IsFunc = true;
  Img.Symbols.push_back(std::move(Function));
  ASSERT_EQ(Img.getFunctionNameAt(Callee), "_setargv");
  const std::string Source = body(liftEntries(Img, {Caller}), "caller");
  EXPECT_NE(Source.find("_setargv("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("text("), std::string::npos) << Source;
}

TEST(Win64CallContract, ADataImportReadsAsItsSlot) {
  // MinGW reads msvcrt's _commode through `.refptr.__imp__commode`, a
  // pointer to the import's slot in read-only data; MSVC reads the slot
  // itself.  Both read the address the loader binds to the slot.
  constexpr va_t Refptr = 0x140004000;
  constexpr va_t ViaRefptr = Text;
  constexpr va_t Direct = Text + 0x20;
  std::vector<uint8_t> Code(0x40, 0xCC);
  std::vector<uint8_t> Indirect = {0x48, 0x8B, 0x05}; // mov rax, [rip + Refptr]
  for (uint8_t B : rel32(ViaRefptr + 7, Refptr))
    Indirect.push_back(B);
  for (uint8_t B : {0x48, 0x8B, 0x00, 0xC3}) // mov rax, [rax]; ret
    Indirect.push_back(B);
  put(Code, ViaRefptr, Indirect);
  std::vector<uint8_t> Slot = {0x48, 0x8B, 0x05}; // mov rax, [rip + slot]
  for (uint8_t B : rel32(Direct + 7, slot(0)))
    Slot.push_back(B);
  Slot.push_back(0xC3); // ret
  put(Code, Direct, Slot);
  BinaryImage Img = makeImage(
      Code, {{ViaRefptr, "p_commode"}, {Direct, "read_commode"}}, {"_commode"});
  Segment Rdata;
  Rdata.Name = ".rdata";
  Rdata.VA = Refptr;
  Rdata.Size = Rdata.FileSz = 8;
  Rdata.Flags = SegmentFlags::Readable;
  const va_t SlotVA = slot(0);
  Rdata.Data.resize(8);
  std::memcpy(Rdata.Data.data(), &SlotVA, sizeof(SlotVA));
  Img.Segments.push_back(std::move(Rdata));
  const std::string Source = liftEntries(Img, {ViaRefptr, Direct});
  EXPECT_NE(Source.find("extern void *__imp__commode;"), std::string::npos)
      << Source;
  for (const char *Name : {"p_commode", "read_commode"}) {
    SCOPED_TRACE(Name);
    const std::string Body = body(Source, Name);
    EXPECT_NE(Body.find("__imp__commode;"), std::string::npos) << Source;
    EXPECT_EQ(Body.find("0x140003000"), std::string::npos) << Source;
  }
  EXPECT_EQ(Source.find("refptr"), std::string::npos) << Source;
}

TEST(Win64CallContract, AFloatArgumentTakesItsSlotsVectorRegister) {
  // scale(x, n) = x * n reads x in xmm0, the first slot's vector register,
  // and n in edx, the second slot's integer one. twice(x) = x + x;
  // quadruple(x) doubles twice(x), passing x on in xmm0.
  constexpr va_t Scale = Text, Twice = Text + 0x10, Quadruple = Text + 0x20;
  std::vector<uint8_t> Code(0x40, 0xCC);
  put(Code, Scale,
      {0xF3, 0x0F, 0x2A, 0xCA, // cvtsi2ss xmm1, edx
       0xF3, 0x0F, 0x59, 0xC1, // mulss xmm0, xmm1
       0xC3});                 // ret
  put(Code, Twice,
      {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
       0xC3});                 // ret
  std::vector<uint8_t> QuadrupleCode = {0x48, 0x83, 0xEC, 0x28, // sub rsp, 28h
                                        0xE8};                  // call twice
  for (uint8_t B : rel32(Quadruple + 9, Twice))
    QuadrupleCode.push_back(B);
  for (uint8_t B : {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
                    0x48, 0x83, 0xC4, 0x28, // add rsp, 28h
                    0xC3})                  // ret
    QuadrupleCode.push_back(B);
  put(Code, Quadruple, QuadrupleCode);
  const BinaryImage Img = makeImage(
      Code, {{Scale, "scale"}, {Twice, "twice"}, {Quadruple, "quadruple"}}, {});
  const std::string Source = liftEntries(Img, {Scale, Twice, Quadruple});
  EXPECT_NE(Source.find("float scale(float arg0, int32_t arg1)"),
            std::string::npos)
      << Source;
  const std::string Body = body(Source, "quadruple");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("double quadruple(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("twice(arg0)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
}

TEST(Win64CallContract, AFloatArgumentReachesItsImportsSlot) {
  // sin_twice(x) = sin(x + x) through the import's slot: sin's double
  // parameter takes the first slot's vector register.
  constexpr va_t SinTwice = Text;
  std::vector<uint8_t> Code(0x20, 0xCC);
  std::vector<uint8_t> SinTwiceCode = {0x48, 0x83, 0xEC, 0x28,  // sub rsp, 28h
                                       0xF2, 0x0F, 0x58, 0xC0}; // addsd
  for (uint8_t B : throughSlot(SinTwice + 8, slot(0)))
    SinTwiceCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x28, // add rsp, 28h
                    0xC3})                  // ret
    SinTwiceCode.push_back(B);
  put(Code, SinTwice, SinTwiceCode);
  const BinaryImage Img = makeImage(Code, {{SinTwice, "sin_twice"}}, {"sin"});
  const std::string Body = body(liftEntries(Img, {SinTwice}), "sin_twice");
  ASSERT_FALSE(Body.empty());
  EXPECT_NE(Body.find("double sin_twice(double arg0)"), std::string::npos)
      << Body;
  // ADD's numerical bits and MXCSR complete together before the import.
  EXPECT_NE(Body.find("neverd_x86_fp_add_state_f64("), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("return sin("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
}

void checkPointerQualifiers(bool Local) {
  auto Image = makeImage(throughSlot(Text, Iat, true),
                         {{Text, "forward_qualified"}}, {"fixture_qualified"});
  ASSERT_TRUE(Image.recordImportStorageSlot(Iat, "fixture_qualified", 0,
                                            ImportStorageEvidence::LoaderBind));
  for (bool Nested : {false, true})
    for (unsigned Record : {0u, 1u, 2u})
      for (bool VoidResult : {false, true})
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(Nested);
          SCOPED_TRACE(Record);
          SCOPED_TRACE(VoidResult);
          SCOPED_TRACE(LLVM);
          windows_qualified_test::Header Debug;
          Debug.Local = Local;
          Debug.Slot = Iat;
          Debug.Nested = Nested;
          Debug.Record = Record;
          Debug.Union = Record == 2;
          Debug.VoidResult = VoidResult;
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.LiftMode = LLVM;
          Options.SourceProjection = LLVM;
          Options.OnlyFunctionEntries = {Text};
          auto Result = Pipeline().run(Image, Context, Options, &Debug);
          ASSERT_TRUE(Result.Success) << Result.Error;
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          COptions.RecordDefinitions = [&](const std::vector<TypeRef> &Types) {
            CRecordDefinitions Definitions;
            for (const TypeRef &Requested : Types) {
              const bool Pointer = Requested->Kind == NdTypeKind::Ptr;
              const auto &Type = Pointer ? Requested->Pointee : Requested;
              if (!Type || Type->SourceName != "fixture_record")
                continue;
              Definitions.Tags["fixture_record"] = Debug.Union;
              if (!Pointer) {
                Definitions.Records["fixture_record"] = Debug.Union;
                Definitions.Text = Debug.element() + " { uint16_t value; };";
              }
            }
            return Definitions;
          };
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                            &Debug, &Image));
          } else {
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
          }
          // Independent C definitions require the same qualifiers, including
          // those behind a pointer and on an opaque record's returned pointer.
          const std::string Element = Debug.element();
          const std::string Value =
              Record ? (Nested ? "(*input)->value" : "input->value")
                     : (Nested ? "**input" : "*input");
          const std::string Definition =
              std::string("\n") + (Debug.Union ? "union" : "struct") +
              " fixture_record { uint16_t value; };\n" +
              (VoidResult ? "void " : "const " + Element + " *") +
              "fixture_qualified(const " + Element +
              (Nested ? " *const *input" : " *input") +
              ", volatile uint32_t *output) {\n"
              "  if (*output != 41) __builtin_trap();\n"
              "  if (" +
              Value +
              " != 0x1234) __builtin_trap();\n"
              "  *output = 42;" +
              (VoidResult ? ""
                          : std::string(" return ") +
                                (Nested ? "*input" : "input") + ";") +
              "\n}\nint main(void) {\n  " + Element +
              " value = {0x1234};\n  const " + Element +
              " *input = &value;\n  uint32_t output = 41;\n"
              "  " +
              (VoidResult ? "" : "uintptr_t result = ") +
              "forward_qualified((uintptr_t)" + (Nested ? "&input" : "input") +
              ", (uintptr_t)&output);\n"
              "  return " +
              (VoidResult ? "" : "result != (uintptr_t)&value || ") +
              "output != 42;\n}\n";
          source_call_execution_test::compileAndRun(Source + Definition);
          if (::testing::Test::HasFailure())
            return;
        }
}

TEST(Win64CallContract,
     HeaderPointerQualifiersAgreeWithIndependentDefinitions) {
  checkPointerQualifiers(false);
}

TEST(Win64CallContract, LocalPointerQualifiersAgreeWithIndependentDefinitions) {
  checkPointerQualifiers(true);
}

TEST(Win64CallContract, MixedHeaderScalarsKeepRegistersAndStackArguments) {
  windows_mixed_scalar_test::Header Debug(Iat);
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x38, // sub rsp, 38h
      0xb9, 7,    0,    0,    0,    0xba, 9, 0, 0,    0,
      0x48, 0xb8, 0,    0,    0,    0,    0, 0, 4,    0x40, // 2.5
      0x66, 0x48, 0x0f, 0x6e, 0xd0,                         // movq xmm2, rax
      0x48, 0xb8, 0,    0,    0,    0,    0, 0, 0x1a, 0xc0, // -6.5
      0x66, 0x48, 0x0f, 0x6e, 0xd8,                         // movq xmm3, rax
      0xc7, 0x44, 0x24, 0x20, 11,   0,    0, 0, 0x48, 0xc7,
      0x44, 0x24, 0x28, 0x34, 0x12, 0,    0};
  const auto Call = throughSlot(Text + Code.size(), Iat);
  Code.insert(Code.end(), Call.begin(), Call.end());
  Code.insert(Code.end(), {0x48, 0x83, 0xc4, 0x38, 0xc3});
  auto Image = makeImage(Code, {{Text, "mixed_wrapper"}}, {"fixture_mixed"});
  ASSERT_TRUE(Image.recordImportStorageSlot(Iat, "fixture_mixed", 0,
                                            ImportStorageEvidence::LoaderBind));
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Text};
    Options.LiftMode = LLVM;
    Options.SourceProjection = LLVM;
    auto Result = Pipeline().run(Image, Context, Options, &Debug);
    ASSERT_TRUE(Result.Success) << Result.Error;
    for (const auto &Function : Result.MedFuncs)
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops) {
          if (!Op.ScalarCallSignature)
            continue;
          ASSERT_TRUE(boundFloatingCallArguments(Op, Image.Arch, Image.Format));
          MedOp Broken = Op;
          auto Wrong =
              std::make_shared<FloatingCallArguments>(*Op.ScalarCallSignature);
          Wrong->Scalars.back().StackOffset += 8;
          Broken.ScalarCallSignature = Wrong;
          EXPECT_FALSE(
              boundFloatingCallArguments(Broken, Image.Arch, Image.Format));
          Broken = Op;
          Broken.Output.RegOff = x86reg::RAX;
          EXPECT_FALSE(
              boundFloatingCallArguments(Broken, Image.Arch, Image.Format));
          Broken = Op;
          Broken.CalleeVariadicFrom = 1;
          EXPECT_FALSE(
              boundFloatingCallArguments(Broken, Image.Arch, Image.Format));
          Broken = Op;
          Broken.Inputs[3].Size = 4;
          EXPECT_FALSE(
              boundFloatingCallArguments(Broken, Image.Arch, Image.Format));
        }
    CEmitterOptions COptions;
    COptions.TheArch = Image.Arch;
    COptions.Format = Image.Format;
    COptions.Image = &Image;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(
        LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions, &Debug,
                                   &Image)
             : HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
    source_call_execution_test::compileAndRun(
        Source + windows_mixed_scalar_test::Provider + R"(
int main(void) {
  __auto_type output = mixed_wrapper();
  double result;
  __builtin_memcpy(&result, &output, sizeof(result));
  return result != 42.25;
}
)");
  }
}

TEST(Win64CallContract, AVoidSourceReturnDoesNotEraseAnObservedCallResult) {
  windows_qualified_test::Header Debug;
  Debug.VoidResult = true;
  llvm::LLVMContext Context;
  llvm::Module Module("observed-void-result", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> Builder(Context);
  auto *Type = llvm::FunctionType::get(
      Builder.getInt64Ty(), {Builder.getInt64Ty(), Builder.getInt64Ty()},
      false);
  auto *Callee = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "fixture_qualified", Module);
  auto *Wrapper = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getInt64Ty(), false),
      llvm::GlobalValue::ExternalLinkage, "observe_result", Module);
  llvm_value_provenance::markReturnsNoValue(*Wrapper);
  auto *Observed = new llvm::GlobalVariable(Module, Builder.getInt64Ty(), false,
                                            llvm::GlobalValue::ExternalLinkage,
                                            Builder.getInt64(0), "observed");
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Wrapper));
  auto *Call =
      Builder.CreateCall(Callee, {Builder.getInt64(17), Builder.getInt64(25)});
  Builder.CreateStore(Call, Observed)->setVolatile(true);
  Builder.CreateRet(Call);
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options, &Debug));
  // This machine code observes the call result despite the proposed void
  // prototype. Keep the machine declaration and the store; only the wrapper's
  // own return is unobserved. An independent provider makes that visible.
  source_call_execution_test::compileAndRun(Source + R"(
uint64_t fixture_qualified(uint64_t input, uint64_t output) { return input + output; }
int main(void) { observe_result(); return observed != 42; }
)");
}

TEST(Win64CallContract, HeaderVoidReturnFactsAreSourceOnly) {
  auto Image = makeImage(throughSlot(Text, Iat, true),
                         {{Text, "forward_qualified"}}, {"fixture_qualified"});
  ASSERT_TRUE(Image.recordImportStorageSlot(Iat, "fixture_qualified", 0,
                                            ImportStorageEvidence::LoaderBind));
  windows_qualified_test::Header Debug;
  Debug.Slot = Iat;
  Debug.VoidResult = true;
  for (bool Source : {false, true}) {
    SCOPED_TRACE(Source);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = true;
    Options.SourceProjection = Source;
    Options.OnlyFunctionEntries = {Text};
    auto Result = Pipeline().run(Image, Context, Options, &Debug);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    EXPECT_EQ(Result.MedFuncs.front().ReturnsNoValue, Source);
    ASSERT_TRUE(Result.LlvmModule);
    auto *Function = Result.LlvmModule->getFunction("forward_qualified");
    ASSERT_NE(Function, nullptr);
    EXPECT_TRUE(Function->getReturnType()->isIntegerTy());
  }
}

TEST(Win64CallContract, HeaderVariadicPrefixKeepsEveryProvenTailArgument) {
  windows_variadic_test::Header Debug;
  Debug.Slot = Iat;
  for (unsigned Tail : {0u, 1u, 3u}) {
    // RCX passes through untouched; only the count and variadic values are set.
    std::vector<uint8_t> Setup = {0xBA, static_cast<uint8_t>(Tail), 0, 0, 0};
    if (Tail >= 1)
      Setup.insert(Setup.end(), {0x41, 0xB8, 11, 0, 0, 0});
    if (Tail >= 3) {
      Setup.insert(Setup.end(), {0x41, 0xB9, 22, 0, 0, 0});
      Setup.insert(Setup.end(), {0xC7, 0x44, 0x24, 0x20, 33, 0, 0, 0});
    }
    for (bool Writable : {false, true}) {
      for (bool RegisterTarget : {false, true}) {
        const va_t At = Text + 4 + Setup.size();
        auto Call = throughSlot(At, Iat);
        if (RegisterTarget) {
          Call = {0x48, 0x8B, 0x05};
          const auto Displacement = rel32(At + 7, Iat);
          Call.insert(Call.end(), Displacement.begin(), Displacement.end());
          Call.insert(Call.end(), {0xFF, 0xD0});
        }
        BinaryImage Image =
            makeImage(framed({Setup, Call}), {{Text, "forward_variadic"}},
                      {"fixture_variadic"});
        ASSERT_TRUE(Image.recordImportStorageSlot(
            Iat, "fixture_variadic", 0, ImportStorageEvidence::LoaderBind));
        if (Writable)
          Image.Segments.back().Flags =
              SegmentFlags::Readable | SegmentFlags::Writable;
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(Tail);
          SCOPED_TRACE(RegisterTarget);
          SCOPED_TRACE(LLVM);
          SCOPED_TRACE(Writable);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.LiftMode = LLVM;
          Options.SourceProjection = LLVM;
          Options.OnlyFunctionEntries.insert(Text);
          auto Result = Pipeline().run(Image, Context, Options, &Debug);
          ASSERT_TRUE(Result.Success) << Result.Error;
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            const auto *Callee =
                Result.LlvmModule->getFunction("fixture_variadic");
            ASSERT_NE(Callee, nullptr);
            EXPECT_TRUE(Callee->isVarArg());
            ASSERT_EQ(Callee->arg_size(), 2u);
            EXPECT_TRUE(Callee->getReturnType()->isIntegerTy(32));
            EXPECT_TRUE(
                Callee->getFunctionType()->getParamType(0)->isIntegerTy(32));
            EXPECT_TRUE(
                Callee->getFunctionType()->getParamType(1)->isIntegerTy(32));
            if (Writable && RegisterTarget) {
              const auto *Forward =
                  Result.LlvmModule->getFunction("forward_variadic");
              ASSERT_NE(Forward, nullptr);
              bool Indirect = false;
              for (const auto &Block : *Forward)
                for (const auto &Instruction : Block)
                  if (const auto *Call =
                          llvm::dyn_cast<llvm::CallBase>(&Instruction);
                      Call && Call->isIndirectCall()) {
                    Indirect = true;
                    EXPECT_EQ(Call->getFunctionType(),
                              Callee->getFunctionType());
                    EXPECT_EQ(Call->arg_size(), 2u + Tail);
                  }
              EXPECT_TRUE(Indirect);
            }
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                            &Debug, &Image));
          } else {
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
          }
          source_call_execution_test::compileAndRun(
              Source + windows_variadic_test::oracle(Tail, "forward_variadic"));
        }
      }
    }
  }
}

TEST(Win64CallContract, HeaderVariadicFixedPrefixCanOverflowToTheStack) {
  windows_variadic_test::Header Debug;
  Debug.Slot = Iat;
  Debug.Fixed = 5;
  auto Image = makeImage(throughSlot(Text, Iat, true), {{Text, "forward_many"}},
                         {"fixture_variadic"});
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = LLVM;
    Options.SourceProjection = LLVM;
    Options.OnlyFunctionEntries = {Text};
    auto Result = Pipeline().run(Image, Context, Options, &Debug);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    EXPECT_EQ(Result.MedFuncs.front().Params.size(), 5u);
    CEmitterOptions COptions;
    COptions.TheArch = Image.Arch;
    COptions.Format = Image.Format;
    COptions.Image = &Image;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (LLVM) {
      ASSERT_TRUE(Result.LlvmModule);
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions, &Debug,
                                      &Image));
    } else {
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
    }
    source_call_execution_test::compileAndRun(Source + R"(
int32_t fixture_variadic(int32_t first, int32_t count, int32_t fixed2,
                        int32_t fixed3, int32_t fixed4, ...) {
  if (first != -7 || count != 8 || fixed2 != 9 || fixed3 != 10 || fixed4 != 11)
    __builtin_trap();
  return -19;
}
int main(void) { return (int32_t)forward_many(-7, 8, 9, 10, 11) != -19; }
)");
  }
}

TEST(Win64CallContract, HeaderOnlyFloatingImportRetainsItsTypedABI) {
  class Header : public NullDebugContext {
  public:
    unsigned Bytes = 8;
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "fixture_fp")
        return std::nullopt;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.ReturnType = NdType::makeFloat(Bytes);
      Function.Params = {{"x", NdType::makeFloat(Bytes)},
                         {"y", NdType::makeFloat(Bytes)}};
      Function.PrototypeFromHeader = true;
      Function.PrototypeLibrary = "test-windows-header";
      return Function;
    }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      auto Result =
          Address == Iat ? resolvePrototype("fixture_fp") : std::nullopt;
      if (Result)
        Result->Addr = Address;
      return Result;
    }
  } Debug;
  for (bool RegisterTarget : {false, true}) {
    for (bool Writable : {false, true}) {
      std::vector<uint8_t> Call = throughSlot(Text + 4, Iat);
      if (RegisterTarget) {
        Call = {0x48, 0x8B, 0x05}; // mov rax, [rip + Iat]; call rax
        const auto Displacement = rel32(Text + 4 + 7, Iat);
        Call.insert(Call.end(), Displacement.begin(), Displacement.end());
        Call.insert(Call.end(), {0xFF, 0xD0});
      }
      BinaryImage Image =
          makeImage(framed({Call}), {{Text, "forward_fp"}}, {"fixture_fp"});
      ASSERT_TRUE(Image.recordImportStorageSlot(
          Iat, "fixture_fp", 0, ImportStorageEvidence::LoaderBind));
      if (Writable)
        Image.Segments.back().Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      for (unsigned Bytes : {4u, 8u}) {
        Debug.Bytes = Bytes;
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(Bytes);
          SCOPED_TRACE(LLVM);
          SCOPED_TRACE(RegisterTarget);
          SCOPED_TRACE(Writable);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.LiftMode = LLVM;
          Options.SourceProjection = LLVM;
          Options.OnlyFunctionEntries.insert(Text);
          auto Result = Pipeline().run(Image, Context, Options, &Debug);
          ASSERT_TRUE(Result.Success) << Result.Error;
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            const auto *Callee = Result.LlvmModule->getFunction("fixture_fp");
            std::string IR;
            llvm::raw_string_ostream IROut(IR);
            Result.LlvmModule->print(IROut, nullptr);
            ASSERT_NE(Callee, nullptr) << IR;
            ASSERT_EQ(Callee->arg_size(), 2u);
            const auto *Type = Bytes == 4 ? llvm::Type::getFloatTy(Context)
                                          : llvm::Type::getDoubleTy(Context);
            EXPECT_EQ(Callee->getReturnType(), Type);
            EXPECT_EQ(Callee->getFunctionType()->getParamType(0), Type);
            EXPECT_EQ(Callee->getFunctionType()->getParamType(1), Type);
            EXPECT_EQ(Callee->getCallingConv(), llvm::CallingConv::Win64);
            if (Writable && RegisterTarget) {
              const auto *Forward =
                  Result.LlvmModule->getFunction("forward_fp");
              ASSERT_NE(Forward, nullptr);
              bool Indirect = false;
              for (const auto &Block : *Forward)
                for (const auto &Instruction : Block)
                  if (const auto *Call =
                          llvm::dyn_cast<llvm::CallBase>(&Instruction);
                      Call && Call->isIndirectCall()) {
                    Indirect = true;
                    EXPECT_EQ(Call->getFunctionType(),
                              Callee->getFunctionType());
                  }
              EXPECT_TRUE(Indirect) << IR;
            }
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                            &Debug, &Image));
          } else {
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
          }
          const std::string Type = Bytes == 4 ? "float" : "double";
          std::string Oracle =
              "\n" + Type + " fixture_fp(" + Type + " x, " + Type +
              " y) {\n"
              "  if (x != 1.25 || y != -2.5) __builtin_trap();\n"
              "  return 7.75;\n}\n";
          if (LLVM) {
            // The machine LLVM entry retains complete vector register carriers.
            // Its external import must still consume/produce scalar FP values.
            Oracle +=
                "typedef uint64_t carrier __attribute__((vector_size(16)));\n"
                "int main(void) {\n"
                "  carrier x = {0, 0}, y = {0, 0};\n  " +
                Type +
                " a = 1.25, b = -2.5, result;\n"
                "  __builtin_memcpy(&x, &a, sizeof(a));\n"
                "  __builtin_memcpy(&y, &b, sizeof(b));\n"
                "  carrier out = forward_fp(x, y);\n"
                "  __builtin_memcpy(&result, &out, sizeof(result));\n"
                "  return result != 7.75;\n}\n";
          } else {
            Oracle +=
                "int main(void) { return forward_fp(1.25, -2.5) != 7.75; }\n";
          }
          source_call_execution_test::compileAndRun(Source + Oracle);
        }
      }
    }
  }
}

class FixedStackHeader : public NullDebugContext {
public:
  uint16_t ReturnBytes = 8;
  unsigned Registers = 4;
  bool Local = false;
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    if (Name != "fixture_stack")
      return std::nullopt;
    FunctionSym Function;
    Function.Name = Name.str();
    Function.PrototypeFromHeader = !Local;
    Function.PrototypeLibrary = Local ? UserPrototypeLibrary : "test-header";
    Function.CallConv = DebugCallConv::Cdecl;
    Function.ReturnType =
        ReturnBytes ? NdType::makeInt(ReturnBytes, true) : NdType::makeVoid();
    Function.Params = {{"a", NdType::makeInt(1, false)},
                       {"b", NdType::makeInt(2, false)},
                       {"c", NdType::makeInt(4, false)},
                       {"d", NdType::makeInt(8, false)},
                       {"value", NdType::makePtr(NdType::makeInt(8, true))}};
    if (Registers == 6) {
      Function.Params.insert(
          Function.Params.end() - 1,
          {{"e", NdType::makeInt(8, false)}, {"f", NdType::makeInt(8, false)}});
    }
    Function.PortablePrototypeDeclaration =
        std::string(ReturnBytes == 0   ? "void"
                    : ReturnBytes == 4 ? "int32_t"
                                       : "int64_t") +
        " fixture_stack(uint8_t a, uint16_t b, uint32_t c, uint64_t d, " +
        (Registers == 6 ? "uint64_t e, uint64_t f, " : "") + "int64_t *value)";
    return Function;
  }
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    auto Function =
        Address == Iat ? resolvePrototype("fixture_stack") : std::nullopt;
    if (Function)
      Function->Addr = Address;
    return Function;
  }
};

TEST(Win64CallContract, HeaderStackOperandSurvivesRecoveredBlockBoundaries) {
  FixedStackHeader Debug;
  for (uint16_t ReturnBytes : {0, 4, 8})
    for (unsigned Split : {0, 1, 2}) {
      SCOPED_TRACE(ReturnBytes);
      SCOPED_TRACE(Split);
      Debug.ReturnBytes = ReturnBytes;
      std::vector<uint8_t> Code = {
          0x56,                                     // push rsi
          0x48, 0x83, 0xEC, 0x40,                   // sub rsp,40h
          0x48, 0x8D, 0x44, 0x24, 0x30,             // lea rax,[rsp+30h]
          0x48, 0xC7, 0x00, 0x2A, 0x00, 0x00, 0x00, // mov [rax],42
          0x48, 0x89, 0x44, 0x24, 0x20};            // mov [rsp+20h],rax
      if (Split == 1)
        Code.insert(Code.end(), {0xEB, 0x00}); // jmp next block
      if (Split == 2)
        Code.insert(Code.end(), {0x45, 0x31, 0xC9, // initialize saved scratch
                                 0x41, 0x51,       // push r9
                                 0x4C, 0x8D, 0x0D, 4, 0, 0, 0, // lea r9,[rip+4]
                                 0x41, 0xFF, 0xE1, 0xE9, // jmp r9; junk byte
                                 0x41, 0x59});           // pop r9
      Code.insert(Code.end(), {0xB9, 7, 0, 0, 0,         // mov ecx,7
                               0x31, 0xD2,               // xor edx,edx
                               0x45, 0x31, 0xC0,         // xor r8d,r8d
                               0x45, 0x31, 0xC9});       // xor r9d,r9d
      const auto Call = throughSlot(Text + Code.size(), Iat);
      Code.insert(Code.end(), Call.begin(), Call.end());
      if (!ReturnBytes)
        Code.insert(Code.end(), {0x48, 0x8B, 0x44, 0x24, 0x30});
      if (ReturnBytes == 4)
        Code.insert(Code.end(), {0x48, 0x63, 0xC0}); // movsxd rax,eax
      Code.insert(Code.end(), {0x48, 0x83, 0xC4, 0x40, 0x5E, 0xC3});
      auto Image =
          makeImage(Code, {{Text, "stack_wrapper"}}, {"fixture_stack"});
      Image.Imports.front().Module = "fixture.dll";
      ASSERT_TRUE(Image.recordImportStorageSlot(
          Iat, "fixture_stack", 0, ImportStorageEvidence::LoaderBind));
      for (bool LLVM : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(LLVM);
          SCOPED_TRACE(NoOpt);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = {Text};
          Options.LiftMode = LLVM;
          Options.SourceProjection = true;
          Options.NoOpt = NoOpt;
          auto Result = Pipeline().run(Image, Context, Options, &Debug);
          ASSERT_TRUE(Result.Success) << Result.Error;
          std::string Source;
          llvm::raw_string_ostream Output(Source);
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Output,
                                                 COptions, &Debug, &Image)
                           : HighCEmitter().emit(Result.HighFuncs, Output,
                                                 COptions, &Debug));
          const std::string Provider =
              Debug.resolvePrototype("fixture_stack")
                  ->PortablePrototypeDeclaration +
              R"( {
  if (a != 7 || b || c || d || !value || *value != 42) __builtin_trap();
 )" + std::string(ReturnBytes ? "return -27;" : "*value = -27;") +
              "\n}\nint main(void) { return stack_wrapper() != -27; }\n";
          for (llvm::StringRef Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                Source + Provider,
                {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
        }
    }
}

TEST(Win64CallContract, UnwrittenFixedStackArgumentRemainsUnavailable) {
  for (bool Partial : {false, true}) {
    SCOPED_TRACE(Partial);
    FixedStackHeader Debug;
    std::vector<uint8_t> Code{0x48, 0x83, 0xEC, 0x28,    // sub rsp,40
                              0xB9, 7,    0,    0,    0, // mov ecx,7
                              0x31, 0xD2,                // xor edx,edx
                              0x45, 0x31, 0xC0,          // xor r8d,r8d
                              0x45, 0x31, 0xC9};         // xor r9d,r9d
    if (Partial)
      Code.insert(Code.end(), {0xC7, 0x44, 0x24, 0x20, 0, 0, 0, 0});
    const auto Call = throughSlot(Text + Code.size(), Iat);
    Code.insert(Code.end(), Call.begin(), Call.end());
    Code.insert(Code.end(), {0x48, 0x83, 0xC4, 0x28, 0xC3});
    auto Image = makeImage(Code, {{Text, "stack_wrapper"}}, {"fixture_stack"});
    ASSERT_TRUE(Image.recordImportStorageSlot(
        Iat, "fixture_stack", 0, ImportStorageEvidence::LoaderBind));
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(NoOpt);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Text};
        Options.LiftMode = LLVM;
        Options.SourceProjection = true;
        Options.NoOpt = NoOpt;
        auto Result = Pipeline().run(Image, Context, Options, &Debug);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Output,
                                               COptions, &Debug, &Image)
                         : HighCEmitter().emit(Result.HighFuncs, Output,
                                               COptions, &Debug));
        EXPECT_NE(Source.find("__builtin_trap"), std::string::npos) << Source;
        // Observe the unavailable-value path without invoking host trap signal
        // handling. Returning normally (including an invented zero) fails.
        const std::string Provider =
            Debug.resolvePrototype("fixture_stack")
                ->PortablePrototypeDeclaration +
            " { return 0; }\nint main(void) { stack_wrapper(); return 1; }\n";
        for (llvm::StringRef Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              "#include <stdlib.h>\n#define __builtin_trap() _Exit(0)\n" +
                  Source + Provider,
              {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
  }
}
TEST(FixedIntegerStackCalls, SysVAndTailOperandsUseTheirDeclaredLocations) {
  for (auto Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool Tail : {false, true})
      for (bool Local : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(Tail);
        SCOPED_TRACE(Local);
        const bool Windows = Format == BinaryFormat::COFF;
        FixedStackHeader Debug;
        Debug.Local = Local;
        Debug.Registers = Windows ? 4 : 6;
        const uint8_t Frame = Windows ? 56 : 24;
        const uint8_t Slot = Windows ? 32 : 0;
        std::vector<uint8_t> Code;
        if (Tail) {
          // The first incoming pointer becomes the callee's stack argument.
          // A tail jump's SP already includes its caller's return address.
          Code = {0x48, 0x89, uint8_t(Windows ? 0x4C : 0x7C), 0x24,
                  uint8_t(Slot + 8)};
        } else {
          Code = {0x48,  0x83, 0xEC,
                  Frame, 0x48, 0x8D,
                  0x44,  0x24, uint8_t(Slot + 16),
                  0x48,  0xC7, 0x00,
                  42,    0,    0,
                  0,     0x48, 0x89,
                  0x44,  0x24, Slot,
                  0xEB,  0x00}; // A real predecessor owns the complete store.
        }
        if (Windows)
          Code.insert(Code.end(), {0xB9, 7, 0, 0, 0, 0x31, 0xD2});
        else
          Code.insert(Code.end(),
                      {0xBF, 7, 0, 0, 0, 0x31, 0xF6, 0x31, 0xD2, 0x31, 0xC9});
        Code.insert(Code.end(), {0x45, 0x31, 0xC0, 0x45, 0x31, 0xC9});
        auto Call = throughSlot(Text + Code.size(), Iat);
        if (Tail)
          Call[1] = 0x25; // jmp [rip+slot], not call [rip+slot]
        Code.insert(Code.end(), Call.begin(), Call.end());
        if (!Tail)
          Code.insert(Code.end(), {0x48, 0x83, 0xC4, Frame, 0xC3});
        auto Image =
            makeImage(Code, {{Text, "stack_wrapper"}}, {"fixture_stack"});
        Image.Format = Format;
        Image.Imports.front().Module = "fixture";
        ASSERT_TRUE(Image.recordImportStorageSlot(
            Iat, "fixture_stack", 0, ImportStorageEvidence::LoaderBind));
        for (bool LLVM : {false, true})
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(LLVM);
            SCOPED_TRACE(NoOpt);
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.OnlyFunctionEntries = {Text};
            Options.LiftMode = LLVM;
            Options.SourceProjection = true;
            Options.NoOpt = NoOpt;
            auto Result = Pipeline().run(Image, Context, Options, &Debug);
            ASSERT_TRUE(Result.Success) << Result.Error;
            std::string Source;
            llvm::raw_string_ostream Output(Source);
            CEmitterOptions COptions;
            COptions.TheArch = Image.Arch;
            COptions.Format = Image.Format;
            COptions.Image = &Image;
            ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Output,
                                                   COptions, &Debug, &Image)
                             : HighCEmitter().emit(Result.HighFuncs, Output,
                                                   COptions, &Debug));
            std::string Actuals;
            if (Tail) {
              // This test checks the callee's physical stack location. The
              // source wrapper may retain unused incoming stack-home inputs.
              const auto Begin = Source.find("stack_wrapper(");
              ASSERT_NE(Begin, std::string::npos);
              const auto End = Source.find(')', Begin);
              ASSERT_NE(End, std::string::npos);
              const auto Parameters =
                  Source.substr(Begin + 14, End - Begin - 14);
              ASSERT_FALSE(Parameters.empty());
              Actuals = "(uintptr_t)&value";
              for (char C : Parameters)
                if (C == ',')
                  Actuals += ", 0xBAD";
            }
            const std::string Provider =
                Debug.resolvePrototype("fixture_stack")
                    ->PortablePrototypeDeclaration +
                " { if (a != 7 || b || c || d || " +
                (Windows ? std::string() : "e || f || ") +
                "!value || *value != 42) __builtin_trap(); return -27; }\n" +
                "int main(void) { int64_t value = 42; return stack_wrapper(" +
                Actuals + ") != -27; }\n";
            for (llvm::StringRef Level : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  Source + Provider,
                  {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
          }
      }
}
} // namespace
