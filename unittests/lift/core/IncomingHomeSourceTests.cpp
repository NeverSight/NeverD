//===- IncomingHomeSourceTests.cpp - Incoming stack source values --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute source projections of explicit incoming stack carriers.

#include "../../../lib/pipeline/PipelineIncomingStack.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t Caller = 0x140001000;
constexpr va_t Callee = Caller + 0x40;

BinaryImage homeImage(Arch Architecture, BinaryFormat Format) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Image.Format = Format;
  Image.Base = 0x140000000;
  Image.Entry = Caller;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Caller;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (Architecture == Arch::X64) {
    Text.Data = {0x48, 0x83, 0xec, 0x28,               // sub rsp,28h
                 0x48, 0xc7, 0x04, 0x24, 42,  0, 0, 0, // mov qword [rsp],42
                 0xe8, 0x2f, 0,    0,    0,            // call read_home
                 0x48, 0x83, 0xc4, 0x28, 0xc3};        // add rsp,28h; ret
    Text.Data.resize(0x40, 0xcc);
    // CALL pushes its return address: the caller's outgoing byte zero is the
    // callee's entry-SP+8, even though it is below Win64's usual fifth
    // argument.
    Text.Data.insert(Text.Data.end(), {0x48, 0x8b, 0x44, 0x24, 8, 0xc3});
  } else {
    Text.Data = {0xff, 0x83, 0x00, 0xd1,  // sub sp,sp,#32
                 0xfe, 0x0b, 0x00, 0xf9,  // str x30,[sp,#16]
                 0x40, 0x05, 0x80, 0xd2,  // mov x0,#42
                 0xe0, 0x03, 0x00, 0xf9,  // str x0,[sp]
                 0x0c, 0x00, 0x00, 0x94,  // bl read_home
                 0xfe, 0x0b, 0x40, 0xf9,  // ldr x30,[sp,#16]
                 0xff, 0x83, 0x00, 0x91,  // add sp,sp,#32
                 0xc0, 0x03, 0x5f, 0xd6}; // ret
    Text.Data.resize(0x40);
    Text.Data.insert(Text.Data.end(), {0xe0, 0x03, 0x40, 0xf9,   // ldr x0,[sp]
                                       0xc0, 0x03, 0x5f, 0xd6}); // ret
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  for (const auto &[Address, Name] :
       {std::pair{Caller, "caller_home"}, std::pair{Callee, "read_home"}}) {
    auto Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    Image.Symbols.push_back(std::move(Function));
  }
  return Image;
}

SourceFunctionTypeHint homeSignature(Arch Architecture, bool Floating) {
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  Hint.Architecture = Architecture;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = NdType::makeInt(8, false);
  Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                         getTargetRegInfo(Architecture).IntReturnReg, 0, 8};
  SourceParameterTypeHint Parameter;
  Parameter.Name = "incoming_home";
  Parameter.Type = Floating ? NdType::makeFloat(8) : NdType::makeInt(8, false);
  Parameter.Location = {SourceABICarrierKind::Stack, 0,
                        Architecture == Arch::X64 ? 8 : 0, 8};
  Hint.Parameters.push_back(std::move(Parameter));
  return Hint;
}

std::map<SourceCallOccurrenceKey, SourceCallTypeHint>
homeCallSites(const BinaryImage &Image,
              const SourceFunctionTypeHint &Signature) {
  Decoder Decoder;
  if (!Decoder.init(Image))
    return {};
  CFGBuilder Builder;
  const std::set<va_t> Entries{Caller, Callee};
  Builder.setKnownFuncEntries(&Entries);
  const auto Low = Builder.build(Image, Decoder, Caller, "caller_home");
  std::map<SourceCallOccurrenceKey, SourceCallTypeHint> Sites;
  for (const auto &Block : Low.Blocks)
    for (const auto &Operation : Block.Ops)
      if (const auto Site = sourceCallOccurrenceKey(Operation);
          Site && Site->StaticTarget == Callee) {
        SourceCallTypeHint Hint;
        Hint.CallKind = SourceCallTypeHint::Kind::Native;
        Hint.TargetAddress = Callee;
        Hint.TargetName = "read_home";
        Hint.Signature = Signature;
        Sites.emplace(*Site, std::move(Hint));
      }
  return Sites;
}

void runHomeSourceMatrix(bool Explicit, bool SelectedCaller = false) {
  for (bool Floating : {false, true})
    for (Arch Architecture : {Arch::X64, Arch::AArch64})
      for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF}) {
        if (Floating && !Explicit)
          continue; // Native bytes alone do not declare a floating parameter.
        SCOPED_TRACE(Explicit);
        SCOPED_TRACE(Floating);
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        const auto Image = homeImage(Architecture, Format);
        const auto Signature = homeSignature(Architecture, Floating);
        std::string Diagnostic;
        ASSERT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
        const std::set<va_t> Entries{Caller, Callee};
        const auto Sites = homeCallSites(Image, Signature);
        ASSERT_EQ(Sites.size(), 1u);
        for (bool LLVM : {false, true})
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(LLVM);
            SCOPED_TRACE(NoOpt);
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.OnlyFunctionEntries =
                SelectedCaller ? std::set<va_t>{Caller} : Entries;
            Options.SourceProjection = LLVM;
            Options.LiftMode = LLVM;
            Options.NoOpt = NoOpt;
            if (Explicit) {
              Options.SourceTypeHints.emplace(Callee, Signature);
              Options.SourceCallSiteHints.emplace(Caller, Sites);
            }
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            if (SelectedCaller) {
              ASSERT_EQ(Result.MedFuncs.size(), 1u);
              const auto &Calls = Result.MedFuncs.front().CallInfos;
              const auto Call = std::find_if(Calls.begin(), Calls.end(),
                                             [](const MedCallInfo &Info) {
                                               return Info.TargetAddr == Callee;
                                             });
              if (Call != Calls.end()) {
                if (Call->Args.size() != 1) {
                  Pipeline::dumpLowIR(Result.LowFuncs, llvm::errs());
                  Pipeline::dumpMedIR(Result.MedFuncs, llvm::errs());
                  const auto Hints =
                      readIncomingStackCallees(Image, Result, {Callee});
                  llvm::errs()
                      << "omitted hints=" << Hints.size() << " prototype="
                      << Result.CallUserPrototypes.count(Callee)
                      << " bound=" << bool(Call->SourceCallHint) << "\n";
                }
                ASSERT_EQ(Call->Args.size(), 1u);
              }

            } else {
              const auto Function =
                  std::find_if(Result.MedFuncs.begin(), Result.MedFuncs.end(),
                               [](const MedFunc &Function) {
                                 return Function.Entry == Callee;
                               });
              ASSERT_NE(Function, Result.MedFuncs.end());
              if (!Function->SourceParametersBound) {
                Pipeline::dumpLowIR(Result.LowFuncs, llvm::errs());
                Pipeline::dumpMedIR(Result.MedFuncs, llvm::errs());
              }
              ASSERT_TRUE(Function->SourceParametersBound);
              size_t Operations = 0;
              for (const auto &Block : Function->Blocks)
                Operations += Block.Ops.size();
              inferMedTypes(*Function, Architecture);
              ASSERT_TRUE(Function->SourceTypeHint);
              if (!Function->SourceParametersBound) {
                Pipeline::dumpLowIR(Result.LowFuncs, llvm::errs());
                Pipeline::dumpMedIR(Result.MedFuncs, llvm::errs());
              }
              ASSERT_TRUE(Function->SourceParametersBound);
              size_t ReboundOperations = 0;
              for (const auto &Block : Function->Blocks)
                ReboundOperations += Block.Ops.size();
              EXPECT_EQ(ReboundOperations, Operations);
            }
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
            ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos)
                << Source;
            const std::string Oracle =
                SelectedCaller
                    ? Floating
                          ? "\n#include <string.h>\n"
                            "uint64_t read_home(double value) { uint64_t bits; "
                            "memcpy(&bits, &value, sizeof(bits)); return bits; "
                            "}\n"
                            "int main(void) { return caller_home() != 42; }\n"
                          : "\nuint64_t read_home(uint64_t value) { return "
                            "value; }\n"
                            "int main(void) { return caller_home() != 42; }\n"
                : Floating
                    ? "\n#include <string.h>\n"
                      "int main(void) { double value = 71.0; uint64_t bits; "
                      "memcpy(&bits, &value, sizeof(bits)); "
                      "return caller_home() != 42 || read_home(value) != bits; "
                      "}\n"
                    : "\nint main(void) { return caller_home() != 42 || "
                      "read_home(71) != 71; }\n";
            for (llvm::StringRef Level : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  Source + Oracle,
                  {Level, "-ftrivial-auto-var-init=pattern",
                   "-fsanitize=undefined", "-fsanitize-trap=all"});
          }
      }
}

TEST(IncomingHomeSource, ExplicitStackCarrierReachesBothCPaths) {
  runHomeSourceMatrix(true);
}

TEST(IncomingHomeSource, AutomaticStackCarrierReachesBothCPaths) {
  runHomeSourceMatrix(false);
}

TEST(IncomingHomeSource, OmittedStackLeafUsesTheOrdinaryCalleeReader) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF}) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(static_cast<unsigned>(Format));
      const auto Image = homeImage(Architecture, Format);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      ASSERT_TRUE(mayReadIncomingStackLeaf(Image, Callee, Dec));
      CFGBuilder Builder;
      const std::set<va_t> Entries{Caller, Callee};
      Builder.setKnownFuncEntries(&Entries);
      const auto Low = Builder.build(Image, Dec, Callee, "read_home");
      LowToMedConverter Converter;
      Converter.setBinaryImage(&Image);
      auto Med = Converter.convert(Low, Architecture, Format);
      inferMedTypes(Med, Architecture);
      ASSERT_TRUE(inferIncomingStackLeaf(Image, Low, Med));
      PipelineResult Result;
      const auto Hints = readIncomingStackCallees(Image, Result, {Callee});
      ASSERT_EQ(Hints.size(), 1u);
      ASSERT_TRUE(Hints.count(Callee));
    }
}

TEST(IncomingHomeSource, SelectedCallerRetainsIncomingStackArguments) {
  runHomeSourceMatrix(false, true);
}

TEST(IncomingHomeSource, SelectedCallerRetainsExplicitScalarDeclarations) {
  runHomeSourceMatrix(true, true);
}

TEST(IncomingHomeSource, CachedSelectionsKeepCurrentStackInputEvidence) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF}) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(static_cast<unsigned>(Format));
      auto Image = homeImage(Architecture, Format);
      PipelineAnalysisCache Cache;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.AnalysisCache = &Cache;
      std::string Baseline;
      for (unsigned Round = 0; Round != 4; ++Round) {
        SCOPED_TRACE(Round);
        Options.OnlyFunctionEntries = Round == 2
                                          ? std::set<va_t>{Caller, Callee}
                                          : std::set<va_t>{Caller};
        llvm::LLVMContext Context;
        const auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        if (Round == 2)
          continue;
        std::string Dump;
        llvm::raw_string_ostream Output(Dump);
        Pipeline::dumpMedIR(Result.MedFuncs, Output);
        if (!Round)
          Baseline = Dump;
        else
          EXPECT_EQ(Dump, Baseline);
      }
      Cache.reset(); // The cache contract requires reset before byte changes.
      auto &Bytes = Image.Segments.front().Data;
      const auto Start = Callee - Caller;
      if (Architecture == Arch::X64) {
        Bytes[Start] = 0x31; // xor eax,eax; ret
        Bytes[Start + 1] = 0xc0;
        Bytes[Start + 2] = 0xc3;
      } else {
        Bytes[Start] = 0; // mov x0,#0; ret
        Bytes[Start + 1] = 0;
        Bytes[Start + 2] = 0x80;
        Bytes[Start + 3] = 0xd2;
      }
      for (unsigned Round = 0; Round != 2; ++Round) {
        llvm::LLVMContext Context;
        const auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        for (const auto &Function : Result.MedFuncs)
          for (const auto &Call : Function.CallInfos)
            if (Call.TargetAddr == Callee)
              EXPECT_FALSE(Call.SourceCallHint);
      }
    }
}

TEST(IncomingHomeSource, PartialOutgoingWritesDoNotBecomeCompleteInputs) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
      for (bool Selected : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(Selected);
        auto Image = homeImage(Architecture, Format);
        auto &Text = Image.Segments.front();
        if (Architecture == Arch::X64) {
          const std::vector<uint8_t> Body(Text.Data.begin() + (Callee - Caller),
                                          Text.Data.end());
          Text.Data = {0x48, 0x83, 0xec, 0x28, 0xc7, 0x04,
                       0x24, 42,   0,    0,    0, // mov dword [rsp],42
                       0xe8, 0x30, 0,    0,    0,    0x48,
                       0x83, 0xc4, 0x28, 0xc3};
          Text.Data.resize(Callee - Caller, 0xcc);
          Text.Data.insert(Text.Data.end(), Body.begin(), Body.end());
        } else
          Text.Data[15] = 0xb9; // str w0,[sp], instead of str x0,[sp]
        Text.Size = Text.FileSz = Text.Data.size();
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries =
            Selected ? std::set<va_t>{Caller} : std::set<va_t>{Caller, Callee};
        const auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        for (const auto &Function : Result.MedFuncs) {
          EXPECT_FALSE(Function.SourceTypeHint);
          for (const auto &Call : Function.CallInfos)
            if (Call.TargetAddr == Callee) {
              EXPECT_FALSE(Call.SourceCallHint);
            }
        }
      }
}

TEST(IncomingHomeSource, AutomaticInputsRequireCompleteReadOnlyLeaves) {
  const std::vector<std::vector<uint8_t>> Rejected{
      {0x8b, 0x44, 0x24, 8, 0xc3},       // mov eax,[rsp+8]: partial word
      {0x48, 0x8d, 0x44, 0x24, 8, 0xc3}, // lea rax,[rsp+8]: frame escape
      {0x48, 0x8b, 0x00, 0xc3},          // mov rax,[rax]: external memory
      {0x48, 0x8b, 0x5c, 0x24, 8, 0x48, 0x89, 0xd8, 0xc3}, // changes rbx
      {0x48, 0xc7, 0x44, 0x24, 8, 42, 0, 0, 0, 0x48, 0x8b, 0x44, 0x24, 8,
       0xc3}}; // initialized by this function
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
    for (size_t Case = 0; Case < Rejected.size(); ++Case) {
      SCOPED_TRACE(Case);
      SCOPED_TRACE(static_cast<unsigned>(Format));
      auto Image = homeImage(Arch::X64, Format);
      auto &Text = Image.Segments.front();
      Text.Data.resize(Callee - Caller);
      Text.Data.insert(Text.Data.end(), Rejected[Case].begin(),
                       Rejected[Case].end());
      Text.Size = Text.FileSz = Text.Data.size();
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Callee};
      const auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      EXPECT_FALSE(Result.MedFuncs.front().SourceTypeHint);
      EXPECT_FALSE(Result.MedFuncs.front().SourceParametersBound);
    }
}

TEST(IncomingHomeSource, MachineModesIgnoreSourceStackDeclarations) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
      for (unsigned Mode = 0; Mode != 4; ++Mode) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(Mode);
        const auto Image = homeImage(Architecture, Format);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Caller, Callee};
        Options.LiftMode = Mode == 0;
        Options.PatchMode = Mode == 1 || Mode == 2;
        Options.SourceProjection = Mode == 2;
        Options.MedAnalysisOnly = Mode == 3;
        const auto Signature = homeSignature(Architecture, false);
        Options.SourceTypeHints.emplace(Callee, Signature);
        const auto Sites = homeCallSites(Image, Signature);
        ASSERT_EQ(Sites.size(), 1u);
        Options.SourceCallSiteHints.emplace(Caller, Sites);
        const auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_EQ(Result.MedFuncs.size(), 2u);
        for (const auto &Function : Result.MedFuncs) {
          EXPECT_FALSE(Function.SourceParametersBound);
          EXPECT_FALSE(Function.SourceTypeHint);
          for (const auto &Block : Function.Blocks)
            for (const auto &Op : Block.Ops)
              EXPECT_FALSE(Op.SourceCallHint);
        }
      }
}

TEST(IncomingHomeSource, EntryInitializationRetainsCallOccurrenceIndices) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    MedFunc Function;
    Function.Entry = Callee;
    Function.Name = "read_home";
    Function.SourceTypeHint = homeSignature(Architecture, false);
    Function.Blocks.resize(1);
    auto &Block = Function.Blocks.front();
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    MedVar Stack;
    Stack.Kind = MedVar::Reg;
    Stack.TheArch = Architecture;
    Stack.Id = 20;
    Stack.RegOff = TRI.StackPointer;
    Stack.Size = TRI.PointerSize;
    MedOp Seed;
    Seed.Opcode = NdOp::COPY;
    Seed.Output = Stack;
    Seed.addInput(Stack);
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.CallSiteId = 1;
    Call.addInput(MedVar::makeConst(Caller, TRI.PointerSize));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(MedVar::makeConst(1, TRI.PointerSize));
    Block.Ops = {Seed, Call, Return};
    MedCallInfo Info;
    Info.BlockId = Block.Id;
    Info.OpIdx = 1;
    Info.TargetAddr = Caller;
    Function.CallInfos.push_back(Info);
    inferMedTypes(Function, Architecture);
    ASSERT_TRUE(Function.SourceParametersBound);
    ASSERT_EQ(Function.CallInfos.size(), 1u);
    ASSERT_GE(Function.CallInfos.front().OpIdx, 0);
    const auto Index = static_cast<size_t>(Function.CallInfos.front().OpIdx);
    ASSERT_LT(Index, Block.Ops.size());
    EXPECT_EQ(Block.Ops[Index].Opcode, NdOp::CALL);
    EXPECT_EQ(Block.Ops[Index].CallSiteId, 1u);
    EXPECT_EQ(Block.Ops[Index].Inputs[0].ConstVal, Caller);
    const auto Size = Block.Ops.size();
    inferMedTypes(Function, Architecture);
    ASSERT_TRUE(Function.SourceTypeHint);
    EXPECT_EQ(Function.CallInfos.front().OpIdx, static_cast<int>(Index));
    EXPECT_EQ(Block.Ops.size(), Size);
  }
}

TEST(IncomingHomeSource, NarrowDeclarationsDoNotDefineAdjacentStackBytes) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    MedFunc Function;
    Function.Entry = Callee;
    Function.Name = "wide_home_read";
    Function.SourceTypeHint = homeSignature(Architecture, false);
    Function.SourceTypeHint->Parameters.front().Type = NdType::makeInt(4);
    Function.SourceTypeHint->Parameters.front().Location.ValueBytes = 4;
    std::string Diagnostic;
    ASSERT_TRUE(validateSourceABI(*Function.SourceTypeHint, Diagnostic))
        << Diagnostic;
    Function.Blocks.resize(1);
    auto &Block = Function.Blocks.front();
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    MedVar Stack;
    Stack.Kind = MedVar::Reg;
    Stack.Id = 20;
    Stack.TheArch = Architecture;
    Stack.RegOff = TRI.StackPointer;
    Stack.Size = TRI.PointerSize;
    MedOp Seed;
    Seed.Opcode = NdOp::COPY;
    Seed.Output = Stack;
    Seed.addInput(Stack);
    MedOp Address;
    Address.Opcode = NdOp::INT_ADD;
    Address.Output.Kind = MedVar::Temp;
    Address.Output.Id = 21;
    Address.Output.TheArch = Architecture;
    Address.Output.Size = TRI.PointerSize;
    Address.addInput(Stack);
    Address.addInput(MedVar::makeConst(
        Function.SourceTypeHint->Parameters.front().Location.EntryStackOffset,
        TRI.PointerSize));
    MedOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output.Kind = MedVar::Reg;
    Load.Output.Id = 22;
    Load.Output.SSAVer = 1;
    Load.Output.TheArch = Architecture;
    Load.Output.RegOff = TRI.IntReturnReg;
    Load.Output.Size = 8;
    Load.addInput(Address.Output);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(Load.Output);
    Block.Ops = {Seed, Address, Load, Return};
    ASSERT_EQ(
        entryStackOffset(Function, Address.Output, Architecture,
                         BinaryFormat::Unknown),
        Function.SourceTypeHint->Parameters.front().Location.EntryStackOffset);
    inferMedTypes(Function, Architecture);
    EXPECT_FALSE(Function.SourceTypeHint);
    EXPECT_FALSE(Function.SourceParametersBound);
    EXPECT_EQ(Block.Ops.size(), 4u);

    Function.SourceTypeHint = homeSignature(Architecture, false);
    Block.StartAddr = Function.Entry + 1;
    EXPECT_FALSE(
        entryStackOffset(Function, Stack, Architecture, BinaryFormat::Unknown));
    inferMedTypes(Function, Architecture);
    EXPECT_FALSE(Function.SourceTypeHint);
    EXPECT_FALSE(Function.SourceParametersBound);
    EXPECT_EQ(Block.Ops.size(), 4u);
  }
}
} // namespace
