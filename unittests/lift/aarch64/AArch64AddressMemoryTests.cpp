//===- AArch64AddressMemoryTests.cpp - Original memory contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Raw instruction admission, malformed IR, memory effects, real emitted
/// opcodes and native outcomes independently check checked-address ownership.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/SourceStackFrame.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/AArch64/AArch64SVEState.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedDispatchSlots.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

namespace {
#define MEMORY_VALUE(Type, Name, Value) inline constexpr Type Name = Value;
#define MEMORY_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64AddressMemoryFixtures.def"
#define STACK_MEMORY_VALUE(Type, Name, Value)                                  \
  inline constexpr Type Name = Value;
#define STACK_MEMORY_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64StackAddressMemoryFixtures.def"

struct MemoryCase {
  uint32_t Word;
  unsigned Destination;
  unsigned Base;
  unsigned Index;
  uint32_t Scratch;
  unsigned IndexBytes;
};
constexpr MemoryCase Cases[] = {
#define MEMORY_CASE(Word, Dst, Base, Index, Scratch, IndexBytes)               \
  {Word, Dst, Base, Index, Scratch, IndexBytes},
#include "AArch64AddressMemoryFixtures.def"
};

struct MemoryFamilyCase {
  uint32_t Word;
  uint32_t Scratch;
  bool Load;
  unsigned Access;
  unsigned Result;
  bool Signed;
  unsigned IndexBytes;
  unsigned Shift;
  uint64_t Expected;
};
constexpr MemoryFamilyCase FamilyCases[] = {
#define MEMORY_FAMILY_CASE(Word, Scratch, Load, Access, Result, Signed,        \
                           IndexBytes, Shift, Expected)                        \
  {Word, Scratch, Load, Access, Result, Signed, IndexBytes, Shift, Expected},
#include "AArch64AddressMemoryFixtures.def"
};

struct MemoryABICase {
  uint32_t Word;
  uint32_t Scratch;
  unsigned Access;
  bool SignedIndex;
};
constexpr MemoryABICase ABICases[] = {
#define MEMORY_ABI_CASE(Word, Scratch, Access, SignedIndex)                    \
  {Word, Scratch, Access, SignedIndex},
#include "AArch64AddressMemoryFixtures.def"
};

struct MemorySpecialCase {
  uint32_t Word;
  bool Load;
  unsigned Access;
  unsigned Destination;
  unsigned Base;
  unsigned Index;
};
constexpr MemorySpecialCase SpecialCases[] = {
#define MEMORY_SPECIAL_CASE(Word, Load, Access, Destination, Base, Index)      \
  {Word, Load, Access, Destination, Base, Index},
#include "AArch64AddressMemoryFixtures.def"
};

std::vector<LowOp> liftMemory(uint32_t Word, bool Fast, bool Strict = true) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::AArch64));
  Dec.setStrict(Strict);
  std::array<uint8_t, kWordBytes> Bytes;
  writeLE<uint32_t>(Bytes.data(), Word);
  DecodedInsn Insn;
  EXPECT_EQ(Fast
                ? Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn)
                : Dec.decodeOne(Bytes.data(), Bytes.size(), kEntry, Insn),
            kWordBytes);
  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

LowOp originalMemoryOperation(const std::vector<LowOp> &Ops) {
  const auto It =
      std::find_if(Ops.begin(), Ops.end(), lowAArch64AddressMemoryShapeIsValid);
  EXPECT_NE(It, Ops.end());
  return It == Ops.end() ? LowOp{} : *It;
}

MedFunc memoryFunction(uint32_t Word, bool Discard = false) {
  const auto Instruction = *aarch64AddressMemoryInstruction(Word);
  MedFunc Function;
  Function.Name = kName;
  Function.Entry = kEntry;
  Function.ReturnType = NdType::makeInt(kScalarBytes, false);
  Function.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  for (unsigned I = 0; I != (Instruction.isLoad() ? 2U : 3U); ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Arch::AArch64;
    Parameter.Id = I;
    Parameter.RegOff = a64reg::X0 + I * kScalarBytes;
    Parameter.Size = I == 0   ? kScalarBytes
                     : I == 1 ? Instruction.indexBytes()
                              : Instruction.accessBytes();
    Function.Params.push_back(Parameter);
  }
  MedOp Load;
  Load.Opcode = NdOp::INTRINSIC;
  Load.Output.Kind = Discard ? MedVar::Temp : MedVar::Reg;
  Load.Output.TheArch = Arch::AArch64;
  Load.Output.Id = 2;
  Load.Output.SSAVer = 1;
  Load.Output.Size = Instruction.accessBytes();
  Load.Output.RegOff = a64reg::X0;
  Load.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Instruction.operation().Id), kIdBytes));
  Load.addInput(Function.Params[0]);
  Load.addInput(aarch64AddressMemoryInstruction(Word)->index() == kZeroRegister
                    ? MedVar::makeConst(0, Instruction.indexBytes())
                    : Function.Params[1]);
  Load.addInput(MedVar::makeConst(Word, kWordBytes));
  if (!Instruction.isLoad()) {
    Load.addInput(Instruction.destination() == kZeroRegister
                      ? MedVar::makeConst(0, Instruction.accessBytes())
                      : Function.Params[2]);
    Load.Output = MedVar{};
    Load.Output.Size = 0;
    Load.Output.Id = -1;
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Function.Blocks.emplace_back();
  Function.Blocks.front().Id = 0;
  Function.Blocks.front().StartAddr = kEntry;
  Function.Blocks.front().Ops = {Load};
  if (Instruction.isLoad() && Instruction.accessBytes() != kScalarBytes &&
      !Discard) {
    Function.Blocks.front().Ops.front().Output.Kind = MedVar::Temp;
    MedOp Extend;
    Extend.Output = Load.Output;
    Extend.Output.Kind = MedVar::Reg;
    Extend.Output.Id = 3;
    Extend.Output.Size = kScalarBytes;
    Extend.addInput(Function.Blocks.front().Ops.front().Output);
    if (Instruction.operation().Signed &&
        Instruction.operation().ResultBytes < kScalarBytes) {
      auto Signed = Extend;
      Signed.Opcode = NdOp::INT_SEXT;
      Signed.Output.Kind = MedVar::Temp;
      Signed.Output.Id = 4;
      Signed.Output.Size = Instruction.operation().ResultBytes;
      Function.Blocks.front().Ops.push_back(Signed);
      Extend.Opcode = NdOp::INT_ZEXT;
      Extend.Inputs[0] = Signed.Output;
    } else {
      Extend.Opcode =
          Instruction.operation().Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
    }
    Function.Blocks.front().Ops.push_back(Extend);
  }
  if (Discard || !Instruction.isLoad()) {
    MedOp Zero;
    Zero.Opcode = NdOp::COPY;
    Zero.Output = Load.Output;
    Zero.Output.Kind = MedVar::Reg;
    Zero.Output.TheArch = Arch::AArch64;
    Zero.Output.Size = kScalarBytes;
    Zero.Output.RegOff = a64reg::X0;
    Zero.Output.Id = 3;
    Zero.Output.SSAVer = 2;
    Zero.addInput(MedVar::makeConst(0, kScalarBytes));
    Function.Blocks.front().Ops.push_back(Zero);
  }
  Function.Blocks.front().Ops.push_back(Return);
  return Function;
}

LowFunc lowFunction(const LowOp &Load) {
  LowFunc Function;
  Function.Entry = kEntry;
  Function.Name = kName;
  Function.Blocks.emplace_back();
  Function.Blocks.front().Id = 0;
  Function.Blocks.front().StartAddr = kEntry;
  Function.Blocks.front().EndAddr = kEntry + kWordBytes;
  Function.Blocks.front().Ops.push_back(Load);
  LowInstructionBoundary Boundary;
  Boundary.Address = kEntry;
  Boundary.Size = kWordBytes;
  Boundary.OpCount = 1;
  Function.Blocks.front().InstructionBoundaries.push_back(Boundary);
  return Function;
}

void optimizeMemory(llvm::Module &Module) {
  llvm::LoopAnalysisManager Loops;
  llvm::FunctionAnalysisManager Functions;
  llvm::CGSCCAnalysisManager CallGraph;
  llvm::ModuleAnalysisManager Modules;
  llvm::PassBuilder Passes;
  Passes.registerLoopAnalyses(Loops);
  Passes.registerFunctionAnalyses(Functions);
  Passes.registerCGSCCAnalyses(CallGraph);
  Passes.registerModuleAnalyses(Modules);
  Passes.crossRegisterProxies(Loops, Functions, CallGraph, Modules);
  auto Pipeline =
      Passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  Pipeline.run(Module, Modules);
}

void inspectMemoryObject(llvm::MemoryBufferRef Buffer, uint32_t Word) {
  auto Image = MachOLoader().loadBuffer(Buffer);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  unsigned Count = 0;
  for (const auto &Segment : Image->Segments)
    if (Segment.isExecutable())
      for (size_t Offset = 0; Offset + kWordBytes <= Segment.Data.size();
           Offset += kWordBytes)
        Count += readLE<uint32_t>(Segment.Data.data() + Offset) == Word;
  EXPECT_EQ(Count, 1);
}

void compileMemoryC(
    const std::string &Source, bool Execute,
    const std::function<void(llvm::MemoryBufferRef)> &Inspect = {},
    llvm::StringRef ObjectPath = {}) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Found = llvm::sys::findProgramByName(kClang);
  if (!Found)
    GTEST_SKIP() << kMissingCompiler;
  const auto Compiler = *Found;
#endif
  llvm::SmallString<128> SourcePath, OutputPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kSourceExtension,
                                                  SourcePath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kOutputExtension,
                                                  OutputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kErrorExtension,
                                                  ErrorPath));
  llvm::FileRemover RemoveSource(SourcePath), RemoveOutput(OutputPath),
      RemoveError(ErrorPath);
  std::error_code Error;
  {
    llvm::raw_fd_ostream Out(SourcePath, Error);
    ASSERT_FALSE(Error);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (llvm::StringRef Optimization : {kO0, kO2}) {
    llvm::SmallVector<llvm::StringRef> Args = {Compiler,
                                               kTargetFlag,
                                               kTarget,
                                               kFreestanding,
                                               kNoSystemHeaders,
                                               kInclude,
                                               NEVERD_ARM64E_TEST_HEADERS,
                                               kLanguage,
                                               Optimization,
                                               SourcePath,
                                               kOutputFlag,
                                               OutputPath};
    if (!Execute)
      Args.push_back(kCompileOnly);
    if (!ObjectPath.empty())
      Args.push_back(ObjectPath);
    std::string Diagnostic;
    const auto Status =
        llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt, Redirects,
                                  kTimeoutSeconds, 0, &Diagnostic);
    const auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Diagnostic
                         << (Errors ? (*Errors)->getBuffer().str()
                                    : std::string())
                         << Source;
    if (Inspect) {
      const auto Buffer = llvm::MemoryBuffer::getFile(OutputPath);
      ASSERT_TRUE(bool(Buffer));
      Inspect((*Buffer)->getMemBufferRef());
    }
    if (Execute) {
      const llvm::StringRef RunArgs[] = {OutputPath};
      EXPECT_EQ(llvm::sys::ExecuteAndWait(OutputPath, RunArgs, std::nullopt,
                                          Redirects, kTimeoutSeconds, 0,
                                          &Diagnostic),
                0)
          << Diagnostic << Source;
    }
  }
}

std::string memorySource(const MedFunc &Function, llvm::Module &Module,
                         bool LLVMRoute) {
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  if (LLVMRoute)
    EXPECT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
  else {
    const auto High = MedToHighConverter().convert(Function, Arch::AArch64);
    EXPECT_TRUE(HighCEmitter().emit({High}, Out, Options));
  }
  return Source;
}
} // namespace

TEST(AArch64AddressMemory, VectorIdentitiesAppendAfterPublishedInstructions) {
  EXPECT_EQ(static_cast<uint16_t>(Intrinsic::A64_Load64RegisterOffset),
            kPublishedWideAddressLoad);
  EXPECT_EQ(static_cast<uint16_t>(Intrinsic::A64_LoadRegisterOffset),
            kPublishedAddressLoad);
  EXPECT_EQ(static_cast<uint16_t>(Intrinsic::A64_StoreRegisterOffset),
            kPublishedAddressStore);
  EXPECT_EQ(static_cast<uint16_t>(Intrinsic::X86FourSourceMemory),
            kPublishedFourSourceMemory);
  EXPECT_LT(Intrinsic::X86FourSourceMemory,
            Intrinsic::A64_VectorLoadRegisterOffset);
  EXPECT_LT(Intrinsic::A64_VectorLoadRegisterOffset,
            Intrinsic::A64_VectorStoreRegisterOffset);
}

TEST(AArch64AddressMemory, RawWordsPreserveIndexScaleAndAliases) {
  for (const auto &Case : Cases)
    for (bool Fast : {false, true})
      for (bool Strict : {false, true}) {
        SCOPED_TRACE(Case.Word);
        const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
        ASSERT_TRUE(Instruction);
        EXPECT_EQ(Instruction->destination(), Case.Destination);
        EXPECT_EQ(Instruction->base(), Case.Base);
        EXPECT_EQ(Instruction->index(), Case.Index);
        EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
        EXPECT_EQ(Instruction->indexBytes(), Case.IndexBytes);
        const auto Ops = liftMemory(Case.Word, Fast, Strict);
        ASSERT_EQ(Ops.size(), 1);
        const auto &Load = Ops.front();
        EXPECT_EQ(Load.Opcode, NdOp::INTRINSIC);
        ASSERT_EQ(Load.NumInputs, 4);
        EXPECT_TRUE(lowAArch64AddressMemoryShapeIsValid(Load));
        EXPECT_EQ(Load.Output.Size, kScalarBytes);
        if (Case.Destination == kZeroRegister)
          EXPECT_TRUE(Load.Output.isTemp());
        else {
          EXPECT_TRUE(Load.Output.isReg());
          EXPECT_EQ(Load.Output.Offset, Case.Destination * kScalarBytes);
        }
        EXPECT_EQ(Load.Inputs[1].Offset, Case.Base == kZeroRegister
                                             ? a64reg::SP
                                             : Case.Base * kScalarBytes);
        if (Case.Index == kZeroRegister) {
          EXPECT_TRUE(Load.Inputs[2].isConst());
          EXPECT_EQ(Load.Inputs[2].Offset, 0);
        } else {
          EXPECT_TRUE(Load.Inputs[2].isReg());
          EXPECT_EQ(Load.Inputs[2].Offset, Case.Index * kScalarBytes);
        }
        EXPECT_EQ(Load.Inputs[2].Size, Case.IndexBytes);
        EXPECT_EQ(Load.Inputs[3].Offset, Case.Word);
        EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Load));
        EXPECT_FALSE(lowIntrinsicMemoryOperands(Load).Complete);
        EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::A64_Load64RegisterOffset));
      }
  for (uint64_t Word : {uint64_t(kInvalidIndex), kOversized,
                        uint64_t(kPrefetch), uint64_t(kReservedLoad)})
    EXPECT_FALSE(aarch64AddressMemoryInstruction(Word));
  const auto Immediate = aarch64AddressMemoryInstruction(kImmediate);
  ASSERT_TRUE(Immediate);
  EXPECT_FALSE(Immediate->hasRegisterIndex());
  for (bool Fast : {false, true})
    for (bool Strict : {false, true}) {
      const auto Ops = liftMemory(kImmediate, Fast, Strict);
      EXPECT_TRUE(std::none_of(Ops.begin(), Ops.end(),
                               lowAArch64AddressMemoryShapeIsValid));
      EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [](const auto &Op) {
        return Op.Opcode == NdOp::LOAD;
      }));
    }
}

TEST(AArch64AddressMemory,
     LLVMAndBothCProjectionsRetainOneLoadIncludingDeadResults) {
  for (const auto &Case : Cases) {
    if (Case.Base == kZeroRegister)
      continue;
    for (bool Discard : {false, true})
      for (bool Optimize : {false, true}) {
        SCOPED_TRACE(Case.Word);
        const auto Function = memoryFunction(Case.Word, Discard);
        EXPECT_FALSE(
            endsCallArgumentSetup(Function.Blocks.front().Ops.front()));
        ASSERT_TRUE(verifyMedFunc(Function, kName));
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
        ASSERT_TRUE(Module);
        if (Optimize)
          optimizeMemory(*Module);
        EXPECT_FALSE(llvm::verifyModule(*Module));
        unsigned Count = 0;
        for (const auto &Block : *Module->getFunction(kName))
          for (const auto &Insn : Block)
            if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Insn)) {
              const auto *Asm =
                  llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
              ASSERT_TRUE(Asm);
              EXPECT_EQ(Asm->getAsmString(), kAddressLoadAsm);
              EXPECT_EQ(Asm->getConstraintString(), kAddressLoadConstraints);
              EXPECT_TRUE(Asm->hasSideEffects());
              ++Count;
            }
        EXPECT_EQ(Count, 1);
        for (bool LLVMRoute : {false, true}) {
          if (!LLVMRoute && Optimize)
            continue;
          compileMemoryC(memorySource(Function, *Module, LLVMRoute), false,
                         [&](llvm::MemoryBufferRef Buffer) {
                           inspectMemoryObject(Buffer, Case.Scratch);
                         });
        }
        const auto Object =
            Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
        ASSERT_TRUE(Object.Success);
        inspectMemoryObject(llvm::MemoryBufferRef(
                                llvm::StringRef(reinterpret_cast<const char *>(
                                                    Object.ObjectData.data()),
                                                Object.ObjectData.size()),
                                kObject),
                            Case.Scratch);
      }
  }
}

TEST(AArch64AddressMemory, BothCProjectionsExecutePositiveAndNegativeOffsets) {
#if defined(__APPLE__) && defined(__aarch64__)
  for (bool Scaled : {false, true}) {
    const auto Function = memoryFunction(Scaled ? kScaled : kFirst);
    for (bool Optimize : {false, true}) {
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
      ASSERT_TRUE(Module);
      if (Optimize)
        optimizeMemory(*Module);
      for (bool LLVMRoute : {false, true})
        compileMemoryC(
            memorySource(Function, *Module, LLVMRoute) +
                llvm::formatv(kNativeHarness, kName, Scaled ? 1 : kScalarBytes)
                    .str(),
            true);
    }
  }
#else
  GTEST_SKIP() << kMissingNative;
#endif
}

TEST(AArch64AddressMemory,
     MalformedIRCannotReachLoweringOrHiddenLLVMFunctions) {
  enum class Mutation {
    IdWidth,
    WrappedId,
    OutputWidth,
    OutputKind,
    BaseWidth,
    IndexWidth,
    ZeroIndexValue,
    WordWidth,
    DynamicWord,
    WordRange,
    Opcode,
    InputKind,
    Ordering,
    AddressSpace,
    ExtraInput,
    ExtraOutput
  };
  for (auto Change :
       {Mutation::IdWidth, Mutation::WrappedId, Mutation::OutputWidth,
        Mutation::OutputKind, Mutation::BaseWidth, Mutation::IndexWidth,
        Mutation::ZeroIndexValue, Mutation::WordWidth, Mutation::DynamicWord,
        Mutation::WordRange, Mutation::Opcode, Mutation::InputKind,
        Mutation::Ordering, Mutation::AddressSpace, Mutation::ExtraInput,
        Mutation::ExtraOutput}) {
    SCOPED_TRACE(static_cast<unsigned>(Change));
    auto Function = memoryFunction(kFirst);
    auto &Med = Function.Blocks.front().Ops.front();
    auto Low = liftMemory(kFirst, false).front();
    switch (Change) {
    case Mutation::IdWidth:
      Low.Inputs[0].Size = Med.Inputs[0].Size = kWordBytes;
      break;
    case Mutation::WrappedId:
      Low.Inputs[0].Offset += kWrappedId;
      Med.Inputs[0].ConstVal += kWrappedId;
      break;
    case Mutation::OutputWidth:
      Low.Output.Size = Med.Output.Size = kWordBytes;
      break;
    case Mutation::OutputKind:
      Low.Output.Space = VnodeSpace::RAM;
      Med.Output.Kind = MedVar::Stack;
      break;
    case Mutation::BaseWidth:
      Low.Inputs[1].Size = Med.Inputs[1].Size = kWordBytes;
      break;
    case Mutation::IndexWidth:
      Low.Inputs[2].Size = Med.Inputs[2].Size = kWordBytes;
      break;
    case Mutation::ZeroIndexValue:
      // The raw word's XZR index requires an explicit zero value, even when
      // an adapter fabricates otherwise well-typed register operands.
      Low.Inputs[3].Offset = Med.Inputs[3].ConstVal = kZeroIndex;
      break;
    case Mutation::WordWidth:
      Low.Inputs[3].Size = Med.Inputs[3].Size = kScalarBytes;
      break;
    case Mutation::DynamicWord:
      Low.Inputs[3].Space = VnodeSpace::TEMP;
      Med.Inputs[3].Kind = MedVar::Temp;
      break;
    case Mutation::WordRange:
      Low.Inputs[3].Offset = Med.Inputs[3].ConstVal = kOversized;
      break;
    case Mutation::Opcode:
      Low.Inputs[3].Offset = Med.Inputs[3].ConstVal = kNotLoad;
      break;
    case Mutation::InputKind:
      Low.Inputs[2].Space = VnodeSpace::RAM;
      Med.Inputs[2].Kind = MedVar::Stack;
      break;
    case Mutation::Ordering:
      Low.MemoryOrdering = Med.MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case Mutation::AddressSpace:
      Low.MemoryAddressSpace = Med.MemoryAddressSpace =
          NdMemoryAddressSpace::X86FS;
      break;
    case Mutation::ExtraInput:
      Low.addInput(Low.Inputs[1]);
      Med.addInput(Med.Inputs[1]);
      break;
    case Mutation::ExtraOutput:
      Med.IntrinsicOutputs.push_back(Med.Output);
      break;
    }
    EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Med));
    EXPECT_TRUE(endsCallArgumentSetup(Med));
    EXPECT_FALSE(verifyMedFunc(Function, kName));
    if (Change != Mutation::ExtraOutput) {
      EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Low));
      const auto Input = lowFunction(Low);
      auto Error = validateLowInstructionBoundaries(Input.Blocks.front());
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
      EXPECT_THROW(LowToMedConverter().convert(Input, Arch::AArch64),
                   LowToMedConversionError);
    }
    for (bool Masked : {false, true}) {
      llvm::LLVMContext Context;
      const std::vector<char> Mask{0};
      EXPECT_THROW(MedLLVMEmitter().emit(
                       {Function}, Context, kName, Arch::AArch64, {}, nullptr,
                       BinaryFormat::MachO, false, Masked ? &Mask : nullptr),
                   std::runtime_error);
    }
  }
}

TEST(AArch64AddressMemory,
     SyntheticSPAndDifferentTargetsRemainExplicitlyUnavailable) {
  for (Arch Target : {Arch::AArch64, Arch::X64}) {
    auto Function =
        memoryFunction(Target == Arch::AArch64 ? kStackBase : kFirst);
    if (Target == Arch::AArch64) {
      auto &Base = Function.Blocks.front().Ops.front().Inputs[1];
      Base.Kind = MedVar::Reg;
      Base.RegOff = a64reg::SP;
    }
    llvm::LLVMContext Context;
    EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, kName, Target),
                 std::runtime_error);
    const auto High = MedToHighConverter().convert(Function, Arch::AArch64);
    CEmitterOptions Options;
    Options.TheArch = Target;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_THROW(HighCEmitter().emit({High}, Out, Options),
                 std::invalid_argument);
    EXPECT_TRUE(Source.empty());
  }
}

TEST(AArch64AddressMemory, LoadedPointerBitsSurviveBothTypedSourceRoutes) {
  auto Function = memoryFunction(kFirst);
  Function.ReturnType = NdType::makePtr();
  for (bool Optimize : {false, true}) {
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    if (Optimize)
      optimizeMemory(*Module);
    for (bool LLVMRoute : {false, true}) {
      const auto Source = memorySource(Function, *Module, LLVMRoute);
      compileMemoryC(Source, false);
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source + llvm::formatv(kPointerHarness, kName).str(),
                     true);
#endif
    }
  }
}

TEST(AArch64AddressMemory, PointerResultsUseTheCompleteABIReturnRegister) {
  for (Arch Target : {Arch::AArch64, Arch::ARM, Arch::X64, Arch::X86}) {
    const auto &Registers = getTargetRegInfo(Target);
    auto Function = memoryFunction(kFirst);
    Function.Params.resize(1);
    auto &Parameter = Function.Params.front();
    Parameter.TheArch = Target;
    Parameter.Size = Registers.PointerSize;
    Parameter.RegOff = Registers.IntReturnReg;
    Function.ReturnType = NdType::makePtr();
    Function.ReturnType->Size = Registers.PointerSize;
    auto &Copy = Function.Blocks.front().Ops.front();
    Copy.Opcode = NdOp::COPY;
    Copy.Output.TheArch = Target;
    Copy.Output.Size = Registers.PointerSize;
    Copy.Output.RegOff = Registers.IntReturnReg;
    Copy.NumInputs = 1;
    Copy.Inputs.front() = Parameter;
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Function}, Context, kName, Target);
    ASSERT_TRUE(Module);
    optimizeMemory(*Module);
    EXPECT_FALSE(llvm::verifyModule(*Module));
    const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(
        Module->getFunction(kName)->back().getTerminator());
    ASSERT_TRUE(Return);
    EXPECT_FALSE(llvm::isa<llvm::Constant>(Return->getReturnValue()));
  }
}

TEST(AArch64AddressMemory, SourceTemporariesCannotCaptureEitherInput) {
  const auto Expression =
      aarch64AddressMemoryC(*aarch64AddressMemoryInstruction(kFirst),
                            kBaseExpression, kIndexExpression);
  compileMemoryC(llvm::formatv(kCaptureSource, Expression).str(), false);
#if defined(__APPLE__) && defined(__aarch64__)
  const auto Calls = aarch64AddressMemoryC(
      *aarch64AddressMemoryInstruction(kFirst), kBaseCall, kIndexCall);
  compileMemoryC(llvm::formatv(kInputCallsSource, Calls).str(), true);
#endif
}

TEST(AArch64AddressMemory, DecodedFunctionReachesBothCProjectionsAndExecutes) {
  for (bool Scaled : {false, true}) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = kEntry;
    Text.Size = Text.FileSz = 2 * kWordBytes;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    writeLE<uint32_t>(Text.Data.data(), Scaled ? kABIScaled : kABIFirst);
    writeLE<uint32_t>(Text.Data.data() + kWordBytes, kReturn);
    Image.Segments.push_back(Text);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    Dec.setStrict(true);
    const auto Low = CFGBuilder().build(Image, Dec, kEntry, kName);
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto Function =
        Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
    ASSERT_TRUE(verifyMedFunc(Function, kName));
    ASSERT_EQ(Function.Params.size(), 2);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    for (bool LLVMRoute : {false, true}) {
      const auto Source = memorySource(Function, *Module, LLVMRoute);
      compileMemoryC(Source, false);
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source + llvm::formatv(kNativeHarness, kName,
                                            Scaled ? 1 : kScalarBytes)
                                  .str(),
                     true);
#endif
    }
  }
}

TEST(AArch64AddressMemory, StoresRemainOnTheirOriginalSidesOfTheFaultingLoad) {
  auto Function = memoryFunction(kFirst);
  MedOp Before;
  Before.Opcode = NdOp::STORE;
  Before.addInput(Function.Params[0]);
  Before.addInput(MedVar::makeConst(11, kScalarBytes));
  auto After = Before;
  After.Inputs[1].ConstVal = 23;
  auto &Ops = Function.Blocks.front().Ops;
  Ops.insert(Ops.begin(), Before);
  Ops.insert(Ops.begin() + 2, After);
  for (bool Optimize : {false, true}) {
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    if (Optimize)
      optimizeMemory(*Module);
    bool Loaded = false;
    unsigned BeforeCount = 0, AfterCount = 0;
    for (const auto &Block : *Module->getFunction(kName))
      for (const auto &Instruction : Block) {
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
          Loaded |= llvm::isa<llvm::InlineAsm>(Call->getCalledOperand());
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
          if (!llvm::isa<llvm::AllocaInst>(
                  llvm::getUnderlyingObject(Store->getPointerOperand())))
            ++(Loaded ? AfterCount : BeforeCount);
      }
    EXPECT_TRUE(Loaded);
    EXPECT_EQ(BeforeCount, 1);
    EXPECT_EQ(AfterCount, 1);
    for (bool LLVMRoute : {false, true}) {
      const auto Source = memorySource(Function, *Module, LLVMRoute);
      compileMemoryC(Source, false);
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source + llvm::formatv(kBarrierHarness, kName).str(),
                     true);
#endif
    }
  }
}

TEST(AArch64AddressMemory,
     TypedSourceAdmissionPrecedesHiddenOrDiscardedBodies) {
  enum class Mutation {
    ExtraOutput,
    Indirect,
    NonCall,
    MissingOperand,
    WrongWidth,
    DynamicWord,
    StackBase,
    Target
  };
  for (auto Change :
       {Mutation::ExtraOutput, Mutation::Indirect, Mutation::NonCall,
        Mutation::MissingOperand, Mutation::WrongWidth, Mutation::DynamicWord,
        Mutation::StackBase, Mutation::Target})
    for (bool Hidden : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(Change));
      HighFunc Function;
      Function.Name = kName;
      Function.Entry = kEntry;
      Function.ReturnType = NdType::makeVoid();
      auto Load = std::make_shared<HighExpr>();
      Load->Kind = ExprKind::Call;
      Load->IntrinsicId = Intrinsic::A64_Load64RegisterOffset;
      Load->Type = NdType::makeInt(kScalarBytes, false);
      Load->Operands = {HighExpr::makeConst(kEntry, kScalarBytes),
                        HighExpr::makeConst(0, kScalarBytes),
                        HighExpr::makeConst(kFirst, kWordBytes)};
      ASSERT_TRUE(aarch64AddressMemoryHighShapeIsValid(*Load));
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      switch (Change) {
      case Mutation::ExtraOutput:
        Load->IntrinsicOutputs.push_back(MedVar::makeConst(0, kScalarBytes));
        break;
      case Mutation::Indirect:
        Load->IsIndirectCall = true;
        break;
      case Mutation::NonCall:
        Load->Kind = ExprKind::Const;
        break;
      case Mutation::MissingOperand:
        Load->Operands.front().reset();
        break;
      case Mutation::WrongWidth:
        Load->Operands[1]->Type = NdType::makeInt(kWordBytes, false);
        break;
      case Mutation::DynamicWord:
        Load->Operands[2]->Kind = ExprKind::Var;
        break;
      case Mutation::StackBase:
        Load->Operands[2]->ConstVal = kStackBase;
        break;
      case Mutation::Target:
        Options.TheArch = Arch::X64;
        break;
      }
      HighStmt Invoke;
      Invoke.Kind = StmtKind::ExprStmt;
      Invoke.Val = Load;
      if (Hidden) {
        HighStmt Branch;
        Branch.Kind = StmtKind::If;
        Branch.Cond = HighExpr::makeConst(0, 1);
        Branch.Body.push_back(Invoke);
        Function.Body.push_back(Branch);
      } else {
        Function.Body.push_back(Invoke);
      }
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      EXPECT_THROW(HighCEmitter().emit({Function}, Out, Options),
                   std::invalid_argument);
      EXPECT_TRUE(Source.empty());
    }
}

TEST(AArch64AddressMemory,
     LLVMSourceValidatesTheExactTypedAssemblyBeforeFiltering) {
  const auto Function = memoryFunction(kFirst);
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
  ASSERT_TRUE(Module);
  llvm::CallInst *Load = nullptr;
  for (auto &Block : *Module->getFunction(kName))
    for (auto &Instruction : Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
        Load = Call;
  ASSERT_TRUE(Load);
  const std::vector<std::string> Args = {kBaseExpression, kIndexExpression,
                                         std::to_string(Cases[0].Scratch)};
  EXPECT_TRUE(renderAArch64AddressMemoryCall(Arch::AArch64, *Load, kResultName,
                                             true, Args));
  EXPECT_THROW(
      renderAArch64AddressMemoryCall(Arch::X64, *Load, kResultName, true, Args),
      std::invalid_argument);
  bool HasIntrinsics = false;
  EXPECT_THROW(renderIntrinsicCall(Intrinsic::A64_Load64RegisterOffset,
                                   Arch::X64, Args, kScalarBytes, HasIntrinsics,
                                   true,
                                   {kScalarBytes, kScalarBytes, kWordBytes}),
               std::invalid_argument);
  const auto *Original = llvm::cast<llvm::InlineAsm>(Load->getCalledOperand());
  auto *BadAsm =
      llvm::InlineAsm::get(Original->getFunctionType(), kAddressLoadAsm,
                           kAddressLoadConstraints, false);
  Load->setCalledOperand(BadAsm);
  EXPECT_THROW(validateAArch64AddressMemoryCall(Arch::AArch64, *Load),
               std::invalid_argument);
  Load->setCalledOperand(const_cast<llvm::InlineAsm *>(Original));
  Load->setMemoryEffects(llvm::MemoryEffects::none());
  EXPECT_THROW(validateAArch64AddressMemoryCall(Arch::AArch64, *Load),
               std::invalid_argument);
  Load->setMemoryEffects(llvm::MemoryEffects::unknown());
  Load->addFnAttr(llvm::Attribute::Speculatable);
  EXPECT_THROW(validateAArch64AddressMemoryCall(Arch::AArch64, *Load),
               std::invalid_argument);
  Load->removeFnAttr(llvm::Attribute::Speculatable);
  auto *Word = Load->getArgOperand(2);
  Load->setArgOperand(2, llvm::ConstantInt::get(Word->getType(), kFirst));
  EXPECT_THROW(renderAArch64AddressMemoryCall(Arch::AArch64, *Load, kResultName,
                                              false, Args),
               std::invalid_argument);
  // Even selecting a different, empty function must validate this bad body.
  auto *Other = llvm::Function::Create(Original->getFunctionType(),
                                       llvm::GlobalValue::ExternalLinkage,
                                       kResultName, *Module);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_THROW(
      LLVMCEmitter().emit(*Module, Out, Options, nullptr, nullptr, Other),
      std::invalid_argument);
  EXPECT_TRUE(Source.empty());
  Load->setArgOperand(2, Word);
}

TEST(AArch64AddressMemory,
     SoftwareEmulationCannotAssumeWrappingAddressArithmetic) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegister(a64reg::X0, kEntry);
  Emulator.setRegister(a64reg::X1, kEntry);
  Emulator.setRegister(a64reg::X2, 0);
  for (bool Strict : {false, true}) {
    Emulator.setStrictMode(Strict);
    EXPECT_FALSE(Emulator.step(liftMemory(kFirst, false).front()));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), kEntry);
  }
}

TEST(AArch64AddressMemory, CompleteNarrowRangesNeedNoBranchOrMappingEvidence) {
  for (const auto &Case : FamilyCases) {
    SCOPED_TRACE(Case.Word);
    const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    const bool SignedWord =
        Case.IndexBytes == kWordBytes && (Case.Word & kFixtureSignedIndexBit);
    EXPECT_EQ(Instruction->controlIndependentAddressRange(kEntry,
                                                          kRangeNarrowMaximum),
              !SignedWord);
    EXPECT_TRUE(Instruction->controlIndependentAddressRange(
        kEntry, kRangePositiveMaximum));
    ASSERT_TRUE(Instruction->controlIndependentAddressRange(
        kEntry, kRangeExhaustiveMaximum));
    for (uint64_t Index = 0; Index <= kRangeExhaustiveMaximum; ++Index) {
      EXPECT_EQ(Instruction->controlIndependentAddress(kEntry, Index),
                kEntry + (Index << Case.Shift));
    }
    EXPECT_FALSE(Instruction->controlIndependentAddressRange(
        kAccessBoundary, kRangeNarrowMaximum));
    EXPECT_FALSE(
        Instruction->controlIndependentAddressRange(kEntry, kRangeFullMaximum));
  }
}

TEST(AArch64AddressMemory, AddressRangesRejectWrapStackAndMalformedEncodings) {
  const auto Scaled = *aarch64AddressMemoryInstruction(kScaled);
  EXPECT_TRUE(Scaled.controlIndependentAddressRange(kRangeTailBase, 1));
  EXPECT_FALSE(Scaled.controlIndependentAddressRange(kRangeTailBase, 2));
  EXPECT_TRUE(Scaled.controlIndependentAddressRange(kRangeWrapBase, 0));
  EXPECT_FALSE(Scaled.controlIndependentAddressRange(kRangeWrapBase, 1));
  const auto Zero = *aarch64AddressMemoryInstruction(kZeroIndex);
  EXPECT_TRUE(Zero.controlIndependentAddressRange(kEntry, 0));
  EXPECT_FALSE(Zero.controlIndependentAddressRange(kEntry, 1));
  EXPECT_FALSE(aarch64AddressMemoryInstruction(kStackBase)
                   ->controlIndependentAddressRange(kEntry, 0));
  for (uint32_t Invalid : {kInvalidIndex, kPrefetch, kReservedLoad})
    EXPECT_FALSE(
        AArch64AddressMemoryInstruction{Invalid}.controlIndependentAddressRange(
            kEntry, 0));
}

TEST(AArch64AddressMemory, ImageReadsRequireControlIndependentAddresses) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = 2 * kScalarBytes;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  writeLE<uint64_t>(Data.Data.data(), kLoadedValue);
  writeLE<uint64_t>(Data.Data.data() + kScalarBytes, ~kLoadedValue);
  Image.Segments.push_back(Data);
  const auto Load = liftMemory(kFirst, false).front();
  const auto Scaled = liftMemory(kScaled, false).front();
  const auto Instruction = aarch64AddressMemoryInstruction(kFirst);
  ASSERT_TRUE(Instruction);
  EXPECT_FALSE(Instruction->controlIndependentAddress(kAccessBoundary, 0));
  EXPECT_FALSE(Instruction->controlIndependentAddress(kTagHealingBase,
                                                      kTagHealingIndex));
  EXPECT_FALSE(Instruction->controlIndependentAddress(kPoisonHealingBase,
                                                      kPoisonHealingIndex));
  // Checking an already poisoned address without changing its region is
  // idempotent. Identical bits alone still grant no mapped-memory evidence.
  EXPECT_EQ(Instruction->controlIndependentAddress(kPoisonHealingBase, 0),
            kPoisonHealingBase);
  EXPECT_FALSE(aarch64AddressMemoryInstruction(kStackBase)
                   ->controlIndependentAddress(kEntry, 0));
  for (bool Strict : {false, true}) {
    NdOpEmulator UnknownIndex(Image);
    UnknownIndex.setStrictMode(Strict);
    UnknownIndex.setRegister(a64reg::X1, kEntry);
    EXPECT_FALSE(UnknownIndex.step(Load));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(Strict);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(a64reg::X1, kEntry);
    Emulator.setRegister(a64reg::X2, 0);
    ASSERT_TRUE(Emulator.step(Load));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), kLoadedValue);
    Emulator.setRegister(a64reg::X2, 1);
    ASSERT_TRUE(Emulator.step(Scaled));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), ~kLoadedValue);
    Emulator.setRegister(a64reg::X2, kScalarBytes);
    ASSERT_TRUE(Emulator.step(Load));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), ~kLoadedValue);
    const auto Reads = Emulator.getLoadRecords().size();
    // Both sums would read the mapped first word under unchecked addition.
    // Unknown CPTA must not turn either poisoned/tagged base into that read.
    for (const auto &[Base, Index] :
         {std::pair{kTagHealingBase, kTagHealingIndex},
          std::pair{kPoisonHealingBase, kPoisonHealingIndex},
          std::pair{kAccessBoundary, uint64_t(0)}}) {
      Emulator.setRegister(a64reg::X0, kEntry);
      Emulator.setRegister(a64reg::X1, Base);
      Emulator.setRegister(a64reg::X2, Index);
      EXPECT_FALSE(Emulator.step(Load));
      EXPECT_EQ(Emulator.getRegister(a64reg::X0), kEntry);
      EXPECT_EQ(Emulator.getLoadRecords().size(), Reads);
    }
  }
}

TEST(AArch64AddressMemory, DispatchUsesTheAuthenticatedLoadInputLifetime) {
  for (const uint32_t Word : {kAliasedIndex, kScaledZeroIndex}) {
    SCOPED_TRACE(Word);
    auto Function = memoryFunction(Word);
    auto &Block = Function.Blocks.front();
    auto &Load = Block.Ops.front();
    Load.Addr = kEntry;
    Load.OriginSeq = 0;
    MedOp &Branch = Block.Ops.back();
    Branch.Opcode = NdOp::INDIR_BR;
    Branch.Addr = kEntry + kWordBytes;
    Branch.addInput(Load.Output);
    JumpTable Table;
    Table.InsnAddr = Branch.Addr;
    Table.EntrySize = Table.EntryStride = kScalarBytes;
    Table.HasDispatchSlotMap = true;
    Table.AuthenticatedTableLoads = {
        {Load.Addr, Load.OriginSeq, Load.Output.Size}};
    Function.JumpTables.push_back(Table);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    MedDispatchContext Context;
    Context.Func = &Function;
    Context.Img = &Image;
    const auto Find = [&] {
      return findTableSwitchIndex(Context, Block, Branch.Inputs[0]);
    };
    const auto Index = Find();
    ASSERT_TRUE(Index);
    const auto &Expected = Load.Inputs[kAddressMemoryIndexOperand + 1];
    EXPECT_EQ(Index->Kind, Expected.Kind);
    EXPECT_EQ(Index->Id, Expected.Id);
    EXPECT_EQ(Index->SSAVer, Expected.SSAVer);
    EXPECT_EQ(Index->ConstVal, Expected.ConstVal);
    EXPECT_EQ(Index->Size, Expected.Size);

    // Neither a stale occurrence nor two matching tables can identify one
    // selector. A valid shape alone is not a resolver certificate.
    ++Function.JumpTables.front().AuthenticatedTableLoads.front().Seq;
    EXPECT_FALSE(Find());
    Function.JumpTables.front() = Table;
    Function.JumpTables.front().AuthenticatedTableLoads.push_back(
        Table.AuthenticatedTableLoads.front());
    EXPECT_FALSE(Find());
    Function.JumpTables.front() = Table;
    Function.JumpTables.front().MutatedUnsafe = true;
    EXPECT_FALSE(Find());
    Function.JumpTables.front() = Table;
    Function.JumpTables.push_back(Table);
    EXPECT_FALSE(Find());
    Function.JumpTables.clear();
    EXPECT_FALSE(Find());
    Function.JumpTables.push_back(Table);
    Image.Arch = Arch::X64;
    EXPECT_FALSE(Find());
    Image.Arch = Arch::AArch64;
    Block.Ops.insert(Block.Ops.begin(), Load);
    EXPECT_FALSE(
        findTableSwitchIndex(Context, Block, Block.Ops.back().Inputs[0]));
  }
}

TEST(AArch64AddressMemory, AllScalarRegisterOffsetFamiliesKeepTheirContracts) {
  EXPECT_EQ(std::size(FamilyCases), kScalarFamilyCombinations);
  for (const auto &Case : FamilyCases) {
    SCOPED_TRACE(Case.Word);
    const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    EXPECT_EQ(Instruction->isLoad(), Case.Load);
    EXPECT_EQ(Instruction->accessBytes(), Case.Access);
    EXPECT_EQ(Instruction->operation().ResultBytes, Case.Result);
    EXPECT_EQ(Instruction->operation().Signed, Case.Signed);
    EXPECT_EQ(Instruction->indexBytes(), Case.IndexBytes);
    EXPECT_EQ(Instruction->indexShift(), Case.Shift);
    EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
    EXPECT_EQ(bool(Instruction->controlIndependentAddress(kAccessBoundary, 0)),
              Case.Access <= kWordBytes);
    for (bool Fast : {false, true})
      for (bool Strict : {false, true}) {
        const auto Ops = liftMemory(Case.Word, Fast, Strict);
        ASSERT_FALSE(Ops.empty());
        const auto Op = originalMemoryOperation(Ops);
        ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Op));
        EXPECT_EQ(lowAArch64AddressLoadShapeIsValid(Op), Case.Load);
        EXPECT_EQ(Op.Inputs[2].Size, Case.IndexBytes);
        EXPECT_EQ(Op.Output.Size, Case.Load ? Case.Access : 0);
        EXPECT_EQ(Ops.back().Output.Size, Case.Load ? kScalarBytes : 0);
        EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Op));
        EXPECT_FALSE(lowIntrinsicMemoryOperands(Op).Complete);
        if (!Case.Load) {
          ASSERT_EQ(Op.NumInputs, 5);
          EXPECT_EQ(Op.Inputs[4].Size, Case.Access);
          if (Case.Access < kWordBytes) {
            EXPECT_TRUE(Op.Inputs[4].isTemp());
            const auto &Slice = Ops.front();
            EXPECT_EQ(Slice.Opcode, NdOp::SUBBYTES);
            EXPECT_EQ(Slice.Inputs[0].Offset, a64reg::X0);
            EXPECT_EQ(Slice.Inputs[0].Size, kWordBytes);
            EXPECT_EQ(Slice.Output, Op.Inputs[4]);
          } else
            EXPECT_EQ(Op.Inputs[4].Offset, a64reg::X0);
        }
      }
    const auto Function = memoryFunction(Case.Word);
    EXPECT_EQ(endsCallArgumentSetup(Function.Blocks.front().Ops.front()),
              !Case.Load);
    EXPECT_TRUE(verifyMedFunc(Function, kName));
  }
}

TEST(AArch64AddressMemory,
     ScalarFamiliesExecuteBothCProjectionsAgainstNativeWords) {
  for (const auto &Case : FamilyCases) {
    SCOPED_TRACE(Case.Word);
    const auto Function = memoryFunction(Case.Word);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    optimizeMemory(*Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    unsigned Calls = 0;
    for (const auto &Block : *Module->getFunction(kName))
      for (const auto &Insn : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Insn)) {
          ASSERT_TRUE(validateAArch64AddressMemoryCall(Arch::AArch64, *Call));
          EXPECT_EQ(Call->getType()->isVoidTy(), !Case.Load);
          ++Calls;
        }
    EXPECT_EQ(Calls, 1);
    const auto Object =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
    ASSERT_TRUE(Object.Success);
    inspectMemoryObject(
        llvm::MemoryBufferRef(llvm::StringRef(reinterpret_cast<const char *>(
                                                  Object.ObjectData.data()),
                                              Object.ObjectData.size()),
                              kObject),
        Case.Scratch);
#if defined(__APPLE__) && defined(__aarch64__)
    llvm::SmallString<128> ObjectPath;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
        kTempPrefix, kObjectExtension, ObjectPath));
    llvm::FileRemover RemoveObject(ObjectPath);
    std::error_code ObjectError;
    {
      llvm::raw_fd_ostream Out(ObjectPath, ObjectError);
      ASSERT_FALSE(ObjectError);
      Out.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
                Object.ObjectData.size());
    }
    const auto IndexType =
        llvm::formatv(kNativeUnsignedType, Case.IndexBytes * CHAR_BIT).str();
    const auto DataType = llvm::formatv(kNativeObjectDataParameter,
                                        llvm::formatv(kNativeUnsignedType,
                                                      Case.Access * CHAR_BIT))
                              .str();
    compileMemoryC(
        llvm::formatv(kNativeObjectDeclaration, kName, IndexType,
                      Case.Load ? kEmptyNativeArgument : DataType)
                .str() +
            llvm::formatv(kFamilyNativeHarness, kName, Case.Scratch,
                          Case.Shift ? 1 : Case.Access,
                          unsigned(Case.IndexBytes == kScalarBytes ||
                                   (Case.Word & kFixtureSignedIndexBit)),
                          unsigned(Case.IndexBytes == kWordBytes),
                          Case.Load ? kEmptyNativeArgument
                                    : kStoreNativeArgument,
                          unsigned(Case.Load))
                .str(),
        true, {}, ObjectPath);
#endif
    for (bool LLVMRoute : {false, true}) {
      std::string Source = memorySource(Function, *Module, LLVMRoute);
#if defined(__APPLE__) && defined(__aarch64__)
      Source +=
          llvm::formatv(kFamilyNativeHarness, kName, Case.Scratch,
                        Case.Shift ? 1 : Case.Access,
                        unsigned(Case.IndexBytes == kScalarBytes ||
                                 (Case.Word & kFixtureSignedIndexBit)),
                        unsigned(Case.IndexBytes == kWordBytes),
                        Case.Load ? kEmptyNativeArgument : kStoreNativeArgument,
                        unsigned(Case.Load))
              .str();
      compileMemoryC(Source, true);
#else
      compileMemoryC(Source, false, [&](llvm::MemoryBufferRef Buffer) {
        inspectMemoryObject(Buffer, Case.Scratch);
      });
#endif
    }
  }
}

TEST(AArch64AddressMemory,
     ScalarTracerReadsExactWidthsAndPublishesWholeXRegisters) {
  for (const auto &Case : FamilyCases) {
    SCOPED_TRACE(Case.Word);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Segment Data;
    Data.VA = kEntry;
    Data.Size = Data.FileSz = 3 * kScalarBytes;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(Data.Size);
    writeLE<uint64_t>(Data.Data.data() + kScalarBytes,
                      Case.Load ? kLoadedValue : 0);
    Image.Segments.push_back(Data);
    for (bool Strict : {false, true}) {
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(a64reg::X0, kLoadedValue);
      Emulator.setRegister(a64reg::X1, kEntry + kScalarBytes);
      Emulator.setRegister(
          a64reg::X2, Case.IndexBytes == kWordBytes ? kNarrowHighGarbage : 0);
      const auto Ops = liftMemory(Case.Word, false);
      const auto Op = originalMemoryOperation(Ops);
      for (const auto &Step : Ops)
        ASSERT_TRUE(Emulator.step(Step));
      if (Case.Load) {
        EXPECT_EQ(Emulator.getRegister(a64reg::X0), Case.Expected);
        ASSERT_EQ(Emulator.getLoadRecords().size(), 1);
        EXPECT_EQ(Emulator.getLoadRecords().front().Size, Case.Access);
      } else {
        EXPECT_EQ(Emulator.getRegister(a64reg::X0), kLoadedValue);
        auto Read = liftMemory(kFirst, false).front();
        Emulator.setRegister(a64reg::X2, 0);
        ASSERT_TRUE(Emulator.step(Read));
        EXPECT_EQ(Emulator.getRegister(a64reg::X0), Case.Expected);
        Image.Segments.front().Flags = SegmentFlags::Readable;
        NdOpEmulator Refused(Image);
        Refused.setStrictMode(Strict);
        Refused.setRegister(a64reg::X0, ~kLoadedValue);
        Refused.setRegister(a64reg::X1, kEntry + kScalarBytes);
        Refused.setRegister(a64reg::X2, 0);
        for (const auto &Step : Ops)
          if (Step.Opcode != NdOp::INTRINSIC)
            ASSERT_TRUE(Refused.step(Step));
        EXPECT_FALSE(Refused.step(Op));
        ASSERT_TRUE(Refused.step(Read));
        EXPECT_EQ(Refused.getRegister(a64reg::X0), 0);
        Image.Segments.front().Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      }
    }
  }
}

TEST(AArch64AddressMemory, NarrowIndicesNeedOnlyTheirLowWordBeforeExtension) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = 2 * kScalarBytes;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  writeLE<uint64_t>(Data.Data.data(), kLoadedValue);
  writeLE<uint64_t>(Data.Data.data() + kScalarBytes, ~kLoadedValue);
  Image.Segments.push_back(Data);
  for (bool Strict : {false, true}) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(Strict);
    Emulator.setRegister(a64reg::X1, kEntry);
    std::array<uint8_t, kWordBytes> Narrow{};
    writeLE<uint32_t>(Narrow.data(), kScalarBytes);
    Emulator.setRegisterBytes(a64reg::X2, Narrow);
    ASSERT_TRUE(Emulator.step(liftMemory(kNarrowUnsigned, false).front()));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), ~kLoadedValue);
    EXPECT_FALSE(Emulator.step(liftMemory(kFirst, false).front()));
    Emulator.setRegister(a64reg::X1, kEntry + kScalarBytes);
    Emulator.setRegister(a64reg::X2, kNarrowNegative);
    ASSERT_TRUE(Emulator.step(liftMemory(kNarrowSigned, false).front()));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), kLoadedValue);
  }
}

TEST(AArch64AddressMemory, NarrowStoresRequireExactlyTheirKnownDataSlice) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = 2 * kScalarBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(Data);
  std::array<uint8_t, kScalarBytes> Bytes;
  writeLE<uint64_t>(Bytes.data(), kLoadedValue);
  for (const auto &Case : FamilyCases) {
    if (Case.Load || Case.Access >= kWordBytes)
      continue;
    SCOPED_TRACE(Case.Word);
    const auto Ops = liftMemory(Case.Word, false);
    ASSERT_GT(Ops.size(), 1);
    for (bool Strict : {false, true}) {
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setRegister(a64reg::X1, kEntry);
      Emulator.setRegister(a64reg::X2, 0);
      EXPECT_FALSE(Emulator.step(Ops.front()));
      ASSERT_TRUE(Emulator.step(liftMemory(kFirst, false).front()));
      EXPECT_EQ(Emulator.getRegister(a64reg::X0), 0);
      Emulator.setRegisterBytes(
          a64reg::X0, llvm::ArrayRef<uint8_t>(Bytes).take_front(Case.Access));
      for (const auto &Step : Ops)
        ASSERT_TRUE(Emulator.step(Step));
      ASSERT_TRUE(Emulator.step(liftMemory(kFirst, false).front()));
      EXPECT_EQ(Emulator.getRegister(a64reg::X0), Case.Expected);
    }
  }
}

TEST(AArch64AddressMemory, UnobservedStorageCannotCaptureAnObservedRead) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.IsRelocatable = true;
  Segment Text;
  Text.Name = kStorageTextName;
  Text.VA = kEntry;
  Text.Size = Text.FileSz = kStorageFunctionCount * kScalarBytes;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  for (unsigned I = 0; I < kStorageFunctionCount; ++I) {
    writeLE<uint32_t>(Text.Data.data() + I * kScalarBytes,
                      kStorageMoveZeroWord);
    writeLE<uint32_t>(Text.Data.data() + I * kScalarBytes + kWordBytes,
                      kReturn);
  }
  Image.Segments.push_back(Text);
  Segment Data;
  Data.Name = kStorageTableName;
  Data.VA = kStorageBase;
  Data.Size = Data.FileSz = 2 * kScalarBytes;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  const va_t Target = kEntry + kScalarBytes;
  for (unsigned I = 0; I < 2; ++I) {
    writeLE<uint64_t>(Data.Data.data() + I * kScalarBytes, Target);
    Image.CodePtrRelocSlots.insert(kStorageBase + I * kScalarBytes);
  }
  Image.Segments.push_back(Data);
  Symbol Table;
  Table.Name = kStorageTableName;
  Table.Addr = kStorageBase;
  Table.Size = Data.Size;
  Image.Symbols.push_back(Table);
  for (bool SharedCall : {false, true})
    for (bool SeparateFunctions : {false, true}) {
      const auto Base = HighExpr::makeConst(
          kStorageBase, kScalarBytes, ConstantAddressProvenance::DataAddress);
      const auto Read = [&] {
        auto Call = HighExpr::makeCall(
            intrinsicName(Intrinsic::A64_Load64RegisterOffset), 0,
            {Base, HighExpr::makeConst(0, kScalarBytes),
             HighExpr::makeConst(kFirst, kWordBytes)});
        Call->IntrinsicId = Intrinsic::A64_Load64RegisterOffset;
        Call->Type = NdType::makeInt(kScalarBytes, false);
        return Call;
      };
      const auto Unobserved = Read();
      const auto Observed = SharedCall ? Unobserved : Read();
      HighFunc Reader;
      Reader.Name = kName;
      Reader.Entry = kEntry;
      Reader.ReturnType = NdType::makeInt(kScalarBytes, false);
      HighStmt Effect;
      Effect.Kind = StmtKind::Call;
      Effect.CallExpr = Unobserved;
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal =
          SeparateFunctions ? HighExpr::makeConst(0, kScalarBytes) : Observed;
      Reader.Body = {Effect, Return};
      HighFunc Callee;
      Callee.Name = kStorageTargetName;
      Callee.Entry = Target;
      Callee.ReturnType = Reader.ReturnType;
      Return.RetVal = HighExpr::makeConst(0, kScalarBytes);
      Callee.Body = {Return};
      std::vector<HighFunc> Functions{Reader, Callee};
      if (SeparateFunctions) {
        HighFunc Other = Reader;
        Other.Name = kStorageOtherName;
        Other.Entry = Target + kScalarBytes;
        Return.RetVal = Observed;
        Other.Body = {Return};
        Functions.push_back(Other);
      }
      CEmitterOptions Options;
      Options.TheArch = Image.Arch;
      Options.Format = Image.Format;
      Options.Image = &Image;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
      EXPECT_EQ(Source.find(kOriginalStoragePrefix) != std::string::npos,
                !SharedCall || SeparateFunctions)
          << Source;
      Source += llvm::formatv(kStorageIdentityHarness,
                              SeparateFunctions ? kStorageOtherName : kName,
                              kStorageTargetName,
                              SeparateFunctions ? kName : kStorageTargetName)
                    .str();
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source, true);
#else
      compileMemoryC(Source, false);
#endif
    }
}

TEST(AArch64AddressMemory, StoreShapesCannotInventOutputsOrUnusedHighBits) {
  for (const auto &Case : FamilyCases) {
    if (Case.Load)
      continue;
    SCOPED_TRACE(Case.Word);
    auto Function = memoryFunction(Case.Word);
    auto &Med = Function.Blocks.front().Ops.front();
    auto Low = originalMemoryOperation(liftMemory(Case.Word, false));
    ASSERT_TRUE(aarch64AddressMemoryMedShapeIsValid(Med));
    ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Low));
    Med.Output = Function.Params[0];
    Low.Output = Low.Inputs[1];
    EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Med));
    EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Low));
    Med.Output.Size = Low.Output.Size = 0;
    Med.Inputs[4].Size = Low.Inputs[4].Size = Case.Access + 1;
    EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Med));
    EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Low));
  }
}

TEST(AArch64AddressMemory,
     NarrowAliasedIndicesSurviveDecodedSSAAndBothSourceABIs) {
  for (const auto &Case : ABICases) {
    SCOPED_TRACE(Case.Word);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = kEntry;
    Text.Size = Text.FileSz = 3 * kWordBytes;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    writeLE<uint32_t>(Text.Data.data(), Case.Word);
    writeLE<uint32_t>(Text.Data.data() + kWordBytes, kMoveAliasToReturn);
    writeLE<uint32_t>(Text.Data.data() + 2 * kWordBytes, kReturn);
    Image.Segments.push_back(Text);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    Dec.setStrict(true);
    const auto Low = CFGBuilder().build(Image, Dec, kEntry, kName);
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto Function =
        Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
    ASSERT_TRUE(verifyMedFunc(Function, kName));
    ASSERT_EQ(Function.Params.size(), 2);
    EXPECT_EQ(Function.Params[1].Size, kWordBytes);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    optimizeMemory(*Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    for (bool LLVMRoute : {false, true}) {
      std::string Source = memorySource(Function, *Module, LLVMRoute);
#if defined(__APPLE__) && defined(__aarch64__)
      Source +=
          llvm::formatv(kFamilyNativeHarness, kName, Case.Scratch, 1,
                        unsigned(Case.SignedIndex), 1, kEmptyNativeArgument, 1)
              .str();
      compileMemoryC(Source, true);
#else
      compileMemoryC(Source, false);
#endif
    }
  }
}

TEST(AArch64AddressMemory,
     ScalarFamiliesPreserveZeroStackAndAliasedRegisterFields) {
  for (const auto &Case : SpecialCases) {
    SCOPED_TRACE(Case.Word);
    const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    EXPECT_EQ(Instruction->destination(), Case.Destination);
    EXPECT_EQ(Instruction->base(), Case.Base);
    EXPECT_EQ(Instruction->index(), Case.Index);
    for (bool Fast : {false, true})
      for (bool Strict : {false, true}) {
        auto Ops = liftMemory(Case.Word, Fast, Strict);
        ASSERT_FALSE(Ops.empty());
        const auto Memory = originalMemoryOperation(Ops);
        ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
        EXPECT_EQ(Memory.Inputs[1].Offset, Case.Base == kZeroRegister
                                               ? a64reg::SP
                                               : Case.Base * kScalarBytes);
        EXPECT_EQ(Memory.Inputs[2].Size, kWordBytes);
        EXPECT_EQ(Memory.Inputs[3].Offset, Case.Word);
        if (Case.Load) {
          EXPECT_EQ(Ops.back().Output.Size, kScalarBytes);
          if (Case.Destination == kZeroRegister)
            EXPECT_TRUE(Ops.back().Output.isTemp());
          else
            EXPECT_EQ(Ops.back().Output.Offset,
                      Case.Destination * kScalarBytes);
        } else {
          EXPECT_EQ(Memory.Output.Size, 0);
          EXPECT_EQ(Memory.Inputs[4].Size, Case.Access);
          if (Case.Destination == kZeroRegister) {
            EXPECT_TRUE(Memory.Inputs[4].isConst());
            EXPECT_EQ(Memory.Inputs[4].Offset, 0);
            auto Invalid = Memory;
            Invalid.Inputs[4].Offset = 1;
            EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Invalid));
          } else if (Case.Access < kWordBytes) {
            EXPECT_TRUE(Memory.Inputs[4].isTemp());
            EXPECT_EQ(Ops.front().Opcode, NdOp::SUBBYTES);
            EXPECT_EQ(Ops.front().Inputs[0].Offset,
                      Case.Destination * kScalarBytes);
            EXPECT_EQ(Ops.front().Inputs[0].Size, kWordBytes);
            EXPECT_EQ(Ops.front().Output, Memory.Inputs[4]);
          } else
            EXPECT_EQ(Memory.Inputs[4].Offset, Case.Destination * kScalarBytes);
        }
        if (Case.Index == kZeroRegister) {
          EXPECT_TRUE(Memory.Inputs[2].isConst());
          EXPECT_EQ(Memory.Inputs[2].Offset, 0);
          auto Invalid = Memory;
          Invalid.Inputs[2].Offset = 1;
          EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Invalid));
        } else
          EXPECT_EQ(Memory.Inputs[2].Offset, Case.Index * kScalarBytes);
      }
    if (Case.Base == kZeroRegister) {
      auto Function = memoryFunction(Case.Word);
      Function.Blocks.front().Ops.front().Inputs[1].Kind = MedVar::Reg;
      Function.Blocks.front().Ops.front().Inputs[1].RegOff = a64reg::SP;
      llvm::LLVMContext Context;
      EXPECT_THROW(
          MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64),
          std::runtime_error);
    }
  }
}

namespace {
#define VECTOR_MEMORY_VALUE(Type, Name, Value)                                 \
  inline constexpr Type Name = Value;
#define VECTOR_MEMORY_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64VectorAddressMemoryFixtures.def"

struct VectorAddressMemoryCase {
  uint32_t Word;
  uint32_t Scratch;
  bool Load;
  unsigned Access;
  unsigned IndexBytes;
  unsigned Shift;
  bool SignedIndex;
};
constexpr VectorAddressMemoryCase VectorMemoryCases[] = {
#define VECTOR_MEMORY_FAMILY(Word, Scratch, Load, Access, IndexBytes, Shift,   \
                             Signed)                                           \
  {Word, Scratch, Load, Access, IndexBytes, Shift, Signed},
#include "AArch64VectorAddressMemoryFixtures.def"
};

std::string vectorMemoryName(const VectorAddressMemoryCase &Case) {
  return kVectorNamePrefix + llvm::utohexstr(Case.Word);
}

va_t vectorMemoryEntry(const VectorAddressMemoryCase &Case) {
  const auto At = std::find_if(
      std::begin(VectorMemoryCases), std::end(VectorMemoryCases),
      [&](const auto &Candidate) { return Candidate.Word == Case.Word; });
  EXPECT_NE(At, std::end(VectorMemoryCases));
  return kEntry + (At - std::begin(VectorMemoryCases)) * kVectorFunctionStride;
}

MedFunc vectorMemoryFunction(const VectorAddressMemoryCase &Case,
                             std::optional<va_t> Entry = std::nullopt) {
  MedFunc Function;
  Function.Name = vectorMemoryName(Case);
  Function.Entry = Entry ? *Entry : vectorMemoryEntry(Case);
  Function.ReturnType = NdType::makeInt(kScalarBytes, false);
  Function.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  for (unsigned I = 0;
       I < (Case.Load ? kVectorLoadParameters : kVectorStoreParameters); ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Arch::AArch64;
    Parameter.Id = I;
    Parameter.RegOff = a64reg::X0 + I * kScalarBytes;
    Parameter.Size = I == 1 ? Case.IndexBytes : kScalarBytes;
    Function.Params.push_back(Parameter);
  }
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  unsigned Next = kVectorTempFirst;
  auto Temporary = [&](unsigned Bytes) {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.TheArch = Arch::AArch64;
    Value.Id = Next++;
    Value.Size = Bytes;
    return Value;
  };
  auto Emit = [&](NdOp Opcode, MedVar Output,
                  std::initializer_list<MedVar> Inputs) {
    MedOp Operation;
    Operation.Opcode = Opcode;
    Operation.Output = Output;
    for (const auto &Input : Inputs)
      Operation.addInput(Input);
    Block.Ops.push_back(Operation);
  };
  MedOp Memory;
  Memory.Opcode = NdOp::INTRINSIC;
  Memory.addInput(MedVar::makeConst(
      static_cast<uint64_t>(
          aarch64AddressMemoryInstruction(Case.Word)->operation().Id),
      kIdBytes));
  Memory.addInput(Function.Params[0]);
  Memory.addInput(aarch64AddressMemoryInstruction(Case.Word)->hasRegisterIndex()
                      ? Function.Params[1]
                      : MedVar::makeConst(0, kScalarBytes));
  Memory.addInput(MedVar::makeConst(Case.Word, kWordBytes));
  if (Case.Load)
    Memory.Output = Temporary(Case.Access);
  else {
    MedVar Data = Temporary(kVectorBytes);
    Emit(NdOp::CONCAT, Data, {Function.Params[3], Function.Params[2]});
    if (Case.Access < kVectorBytes) {
      const auto Slice = Temporary(Case.Access);
      Emit(NdOp::SUBBYTES, Slice, {Data, MedVar::makeConst(0, kWordBytes)});
      Data = Slice;
    }
    Memory.addInput(Data);
    Memory.Output.Size = 0;
    Memory.Output.Id = -1;
  }
  Block.Ops.push_back(Memory);
  MedVar Result = Temporary(kScalarBytes);
  Result.Kind = MedVar::Reg;
  Result.RegOff = a64reg::X0;
  if (Case.Load) {
    MedVar Wide = Memory.Output;
    if (Case.Access < kVectorBytes) {
      Wide = Temporary(kVectorBytes);
      Emit(NdOp::INT_ZEXT, Wide, {Memory.Output});
    }
    const auto Low = Temporary(kScalarBytes);
    const auto High = Temporary(kScalarBytes);
    Emit(NdOp::SUBBYTES, Low, {Wide, MedVar::makeConst(0, kWordBytes)});
    Emit(NdOp::SUBBYTES, High,
         {Wide, MedVar::makeConst(kScalarBytes, kWordBytes)});
    Emit(NdOp::INT_XOR, Result, {Low, High});
  } else
    Emit(NdOp::COPY, Result, {MedVar::makeConst(0, kScalarBytes)});
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Result);
  Block.Ops.push_back(Return);
  return Function;
}

MedFunc decodedVectorMemoryWords(const VectorAddressMemoryCase &Case,
                                 llvm::ArrayRef<uint32_t> Words) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Segment Text;
  const va_t Entry = vectorMemoryEntry(Case);
  Text.VA = Entry;
  Text.Size = Text.FileSz = Words.size() * kWordBytes;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  for (size_t I = 0; I < Words.size(); ++I)
    writeLE<uint32_t>(Text.Data.data() + I * kWordBytes, Words[I]);
  Image.Segments.push_back(Text);
  Decoder Decode;
  EXPECT_TRUE(Decode.init(Image));
  Decode.setStrict(true);
  const auto Low =
      CFGBuilder().build(Image, Decode, Entry, vectorMemoryName(Case));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Function = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  Function.ReturnType = NdType::makeInt(kScalarBytes, false);
  Function.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  return Function;
}

MedFunc decodedVectorMemoryFunction(const VectorAddressMemoryCase &Case,
                                    bool AcrossBranch) {
  std::vector<uint32_t> Words;
  const bool Double = Case.Access >= kScalarBytes;
  if (!Case.Load)
    Words = {kVectorSeedZero, kVectorInsertLow, kVectorInsertHigh};
  else if (AcrossBranch)
    Words.push_back(Double ? kVectorSeedDouble : kVectorSeedSingle);
  Words.push_back(Case.Word);
  if (Case.Load && AcrossBranch) {
    Words.push_back(kVectorBranchNext);
    Words.push_back(Double ? kVectorReadDouble : kVectorReadSingle);
  } else if (Case.Load) {
    Words.push_back(kVectorReadLow);
    Words.push_back(kVectorReadHigh);
    Words.push_back(kVectorDigest);
  } else
    Words.push_back(kVectorReturnZero);
  Words.push_back(kVectorReturnWord);
  return decodedVectorMemoryWords(Case, Words);
}

enum class VectorMemoryMode { Explicit, Decoded, AcrossBranch };

void executeVectorMemoryBatch(VectorMemoryMode Mode) {
  const bool AcrossBranch = Mode == VectorMemoryMode::AcrossBranch;
  for (bool Load : {false, true}) {
    if (AcrossBranch && !Load)
      continue;
    std::vector<MedFunc> Functions;
    std::string Wrappers, Entries;
    std::string Declarations = kVectorNativeIncludes;
    for (size_t I = 0; I < std::size(VectorMemoryCases); ++I) {
      const auto &Case = VectorMemoryCases[I];
      if (Case.Load != Load)
        continue;
      SCOPED_TRACE(Case.Word);
      auto Function = Mode == VectorMemoryMode::Explicit
                          ? vectorMemoryFunction(Case)
                          : decodedVectorMemoryFunction(Case, AcrossBranch);
      ASSERT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
      const bool SeedParameter =
          AcrossBranch && Function.Params.size() == kVectorAliasParameters;
      const auto Call =
          AcrossBranch
              ? llvm::formatv(kVectorNativeAliasCall, Function.Name,
                              SeedParameter ? kVectorNativeAliasArgument
                                            : kEmptyNativeArgument)
                    .str()
              : llvm::formatv(Load ? kVectorNativeLoadCall
                                   : kVectorNativeStoreCall,
                              Function.Name)
                    .str();
      Wrappers += llvm::formatv(kVectorNativeWrapper, I, Call).str();
      Entries +=
          llvm::formatv(
              kVectorNativeCase, I,
              AcrossBranch ? std::min(Case.Access, kScalarBytes) : Case.Access,
              unsigned{1} << Case.Shift,
              unsigned(Case.SignedIndex || Case.IndexBytes == kScalarBytes),
              unsigned(Case.IndexBytes == kWordBytes))
              .str();
      Declarations +=
          llvm::formatv(
              kVectorNativeDecl, Function.Name,
              SeedParameter ? kVectorNativeAliasParameter
                            : (Load ? kEmptyNativeArgument
                                    : kVectorNativeStoreParameters),
              llvm::formatv(kVectorIndexCType, Case.IndexBytes * CHAR_BIT)
                  .str())
              .str();
      Functions.push_back(std::move(Function));
    }
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit(Functions, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    optimizeMemory(*Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    const auto Harness =
        llvm::formatv(kVectorNativeHarness, Wrappers, Entries, unsigned(Load))
            .str();
    for (bool LLVMRoute : {false, true}) {
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Format = BinaryFormat::MachO;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVMRoute)
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      else {
        std::vector<HighFunc> High;
        for (const auto &Function : Functions)
          High.push_back(MedToHighConverter().convert(Function, Arch::AArch64));
        ASSERT_TRUE(HighCEmitter().emit(High, Out, Options));
      }
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source + Harness, true);
#else
      compileMemoryC(Source, false);
#endif
    }
    const auto Object =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
    ASSERT_TRUE(Object.Success);
    const llvm::MemoryBufferRef Buffer(
        llvm::StringRef(
            reinterpret_cast<const char *>(Object.ObjectData.data()),
            Object.ObjectData.size()),
        kObject);
    for (const auto &Case : VectorMemoryCases)
      if (Case.Load == Load) {
        SCOPED_TRACE(Case.Word);
        inspectMemoryObject(Buffer, Case.Scratch);
      }
#if defined(__APPLE__) && defined(__aarch64__)
    llvm::SmallString<kVectorPathStorage> ObjectPath;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
        kTempPrefix, kOutputExtension, ObjectPath));
    llvm::FileRemover Remove(ObjectPath);
    std::error_code Error;
    llvm::raw_fd_ostream Output(ObjectPath, Error);
    ASSERT_FALSE(Error);
    Output.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
                 Object.ObjectData.size());
    Output.close();
    compileMemoryC(Declarations + Harness, true, {}, ObjectPath);
#endif
  }
}
} // namespace

TEST(AArch64VectorAddressMemory,
     RawWordsKeepNaturalAccessesAndFullVDefinitions) {
  ASSERT_EQ(std::size(VectorMemoryCases), kVectorCaseCount);
  for (const auto &Case : VectorMemoryCases) {
    SCOPED_TRACE(Case.Word);
    const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    EXPECT_TRUE(Instruction->isVector());
    EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
    EXPECT_EQ(Instruction->accessBytes(), Case.Access);
    EXPECT_EQ(Instruction->indexBytes(), Case.IndexBytes);
    EXPECT_EQ(Instruction->indexShift(), Case.Shift);
    EXPECT_EQ(Instruction->isLoad(), Case.Load);
    for (bool Fast : {false, true})
      for (bool Strict : {false, true}) {
        const auto Ops = liftMemory(Case.Word, Fast, Strict);
        const auto Memory = originalMemoryOperation(Ops);
        EXPECT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
        EXPECT_EQ(Memory.Output.Size, Case.Load ? Case.Access : 0);
        if (Case.Load) {
          EXPECT_TRUE(Memory.Output.isReg());
          EXPECT_EQ(Memory.Output.Offset, a64reg::V0);
          EXPECT_TRUE(Ops.back().Output.isReg());
          EXPECT_EQ(Ops.back().Output.Offset, a64reg::V0);
          EXPECT_EQ(Ops.back().Output.Size, kVectorBytes);
          if (Case.Access < kVectorBytes)
            EXPECT_EQ(Ops.back().Opcode, NdOp::INT_ZEXT);
        } else {
          EXPECT_EQ(Memory.Inputs[kAddressMemoryDataOperand + 1].Size,
                    Case.Access);
          EXPECT_FALSE(lowAArch64AddressLoadShapeIsValid(Memory));
        }
      }
  }
}

TEST(AArch64VectorAddressMemory,
     RangeCertificatesIncludeTheCompleteVectorAccess) {
  for (const auto &Case : VectorMemoryCases) {
    SCOPED_TRACE(Case.Word);
    const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    const bool SignedWord =
        Case.IndexBytes == kVectorLowWordBytes && Case.SignedIndex;
    EXPECT_EQ(Instruction->controlIndependentAddressRange(kEntry,
                                                          kRangeNarrowMaximum),
              !SignedWord);
    EXPECT_TRUE(Instruction->controlIndependentAddressRange(
        kEntry, kRangePositiveMaximum));
    ASSERT_TRUE(Instruction->controlIndependentAddressRange(
        kEntry, kRangeExhaustiveMaximum));
    for (uint64_t Index = 0; Index <= kRangeExhaustiveMaximum; ++Index)
      EXPECT_EQ(Instruction->controlIndependentAddress(kEntry, Index),
                kEntry + (Index << Case.Shift));
    // The whole final access fits, but the next indexed element's extent
    // crosses the checked-pointer region. Q accesses need all sixteen bytes.
    const uint64_t Tail = kVectorRangeRegionEnd - Case.Access;
    EXPECT_TRUE(Instruction->controlIndependentAddressRange(Tail, 0));
    EXPECT_FALSE(Instruction->controlIndependentAddressRange(Tail, 1));
    EXPECT_FALSE(
        Instruction->controlIndependentAddressRange(kEntry, kRangeFullMaximum));
  }
}

TEST(AArch64VectorAddressMemory, Vector31IsDataRatherThanAZeroRegister) {
  for (const auto &Case : VectorMemoryCases) {
    const uint32_t Word = Case.Word | kZeroRegister;
    const auto Ops = liftMemory(Word, false);
    const auto Memory = originalMemoryOperation(Ops);
    ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
    const auto Instruction = aarch64AddressMemoryInstruction(Word);
    ASSERT_TRUE(Instruction);
    EXPECT_EQ(Instruction->destination(), kZeroRegister);
    if (Case.Load)
      EXPECT_EQ(Ops.back().Output.Offset, a64reg::V(kZeroRegister));
    else {
      const auto &Data = Memory.Inputs[kAddressMemoryDataOperand + 1];
      EXPECT_FALSE(Data.isConst());
      if (Case.Access == kVectorBytes)
        EXPECT_EQ(Data.Offset, a64reg::V(kZeroRegister));
      else {
        EXPECT_EQ(Ops.front().Opcode, NdOp::SUBBYTES);
        EXPECT_EQ(Ops.front().Inputs[0].Offset, a64reg::V(kZeroRegister));
      }
    }
  }
}

TEST(AArch64VectorAddressMemory,
     EncodedStackBasesCannotBecomeUncheckedGeneralRegisters) {
  const uint32_t BaseMask = kAddressMemoryRegisterMask
                            << kAddressMemoryBaseShift;
  for (const auto &Case : VectorMemoryCases) {
    SCOPED_TRACE(Case.Word);
    const uint32_t Word = (Case.Word & ~BaseMask) | BaseMask;
    const auto Instruction = aarch64AddressMemoryInstruction(Word);
    ASSERT_TRUE(Instruction);
    EXPECT_TRUE(Instruction->stackBase());
    EXPECT_FALSE(Instruction->controlIndependentAddress(kEntry, 0));
    const auto Memory = originalMemoryOperation(liftMemory(Word, false));
    ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
    EXPECT_EQ(Memory.Inputs[kAddressMemoryBaseOperand + 1].Offset, a64reg::SP);
    const auto Source =
        aarch64AddressMemoryC(*Instruction, kBaseCall, kIndexCall,
                              Case.Load ? kEmptyNativeArgument : kResultName);
    EXPECT_NE(Source.find(kStackSaveSP), std::string::npos);
    EXPECT_NE(Source.find(kStackSelectSP), std::string::npos);
    EXPECT_NE(Source.find(kStackRestoreSP), std::string::npos);
    auto Function = vectorMemoryFunction(Case);
    for (auto &Op : Function.Blocks.front().Ops)
      if (aarch64AddressMemoryMedShapeIsValid(Op)) {
        Op.Inputs[kAddressMemoryWordOperand + 1].ConstVal = Word;
        Op.Inputs[kAddressMemoryBaseOperand + 1].Kind = MedVar::Reg;
        Op.Inputs[kAddressMemoryBaseOperand + 1].RegOff = a64reg::SP;
      }
    ASSERT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
    llvm::LLVMContext Context;
    EXPECT_THROW(
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64),
        std::runtime_error);
  }
}

TEST(AArch64VectorAddressMemory,
     TracerZerosLoadedUpperBytesAndStoresOnlyItsSlice) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = kVectorMemorySize;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  writeLE<uint64_t>(Data.Data.data() + kVectorDataPosition, kVectorLoadedLow);
  writeLE<uint64_t>(Data.Data.data() + kVectorDataPosition + kVectorHalfBytes,
                    kVectorLoadedHigh);
  Image.Segments.push_back(Data);
  std::array<uint8_t, kVectorBytes> Seed;
  writeLE<uint64_t>(Seed.data(), kVectorStoreLow);
  writeLE<uint64_t>(Seed.data() + kVectorHalfBytes, kVectorStoreHigh);
  const auto FullRead =
      std::find_if(std::begin(VectorMemoryCases), std::end(VectorMemoryCases),
                   [](const auto &Case) {
                     return Case.Load && Case.Access == kVectorBytes &&
                            Case.IndexBytes == kScalarBytes && !Case.Shift;
                   });
  ASSERT_NE(FullRead, std::end(VectorMemoryCases));
  for (const auto &Case : VectorMemoryCases)
    for (bool Strict : {false, true}) {
      SCOPED_TRACE(Case.Word);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(a64reg::X0, kEntry + kVectorDataPosition);
      Emulator.setRegister(a64reg::X1, Case.IndexBytes == kWordBytes
                                           ? kVectorIndexHighGarbage
                                           : 0);
      Emulator.setRegisterBytes(a64reg::V0, Seed);
      for (const auto &Step : liftMemory(Case.Word, false))
        ASSERT_TRUE(Emulator.step(Step));
      const auto Register = Emulator.getRegisterBytes(a64reg::V0);
      ASSERT_TRUE(Register);
      ASSERT_EQ(Register->size(), kVectorBytes);
      for (unsigned I = 0; I < kVectorBytes; ++I)
        EXPECT_EQ(
            (*Register)[I],
            Case.Load
                ? (I < Case.Access ? Data.Data[kVectorDataPosition + I] : 0)
                : Seed[I]);
      if (Case.Load) {
        ASSERT_EQ(Emulator.getLoadRecords().size(), 1);
        EXPECT_EQ(Emulator.getLoadRecords().front().Size, Case.Access);
      } else {
        Emulator.setRegister(a64reg::X1, 0);
        for (unsigned Part = 0; Part < kVectorReadbackParts; ++Part) {
          Emulator.setRegister(a64reg::X0, kEntry + kVectorDataPosition +
                                               Part * kVectorBytes);
          for (const auto &Step : liftMemory(FullRead->Word, false))
            ASSERT_TRUE(Emulator.step(Step));
          const auto Bytes = Emulator.getRegisterBytes(a64reg::V0);
          ASSERT_TRUE(Bytes);
          ASSERT_EQ(Bytes->size(), kVectorBytes);
          for (unsigned I = 0; I < kVectorBytes; ++I)
            EXPECT_EQ(
                (*Bytes)[I],
                !Part && I < Case.Access
                    ? Seed[I]
                    : Data.Data[kVectorDataPosition + Part * kVectorBytes + I]);
        }
      }
    }
}

TEST(AArch64VectorAddressMemory,
     BothSourceRoutesAndObjectsExecuteAllEncodedFamilies) {
  executeVectorMemoryBatch(VectorMemoryMode::Explicit);
}

TEST(AArch64VectorAddressMemory,
     StoresRequireOnlyTheirKnownSliceAndPreserveRefusedMemory) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = kVectorReadbackParts * kVectorBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(Data);
  std::array<uint8_t, kVectorBytes> Bytes;
  writeLE<uint64_t>(Bytes.data(), kVectorStoreLow);
  writeLE<uint64_t>(Bytes.data() + kVectorHalfBytes, kVectorStoreHigh);
  const auto FullRead =
      std::find_if(std::begin(VectorMemoryCases), std::end(VectorMemoryCases),
                   [](const auto &Case) {
                     return Case.Load && Case.Access == kVectorBytes &&
                            Case.IndexBytes == kScalarBytes && !Case.Shift;
                   });
  ASSERT_NE(FullRead, std::end(VectorMemoryCases));
  for (const auto &Case : VectorMemoryCases) {
    if (Case.Load)
      continue;
    SCOPED_TRACE(Case.Word);
    const auto Operations = liftMemory(Case.Word, false);
    for (bool Strict : {false, true}) {
      auto Prepare = [&](NdOpEmulator &Emulator) {
        Emulator.setStrictMode(Strict);
        Emulator.setRegister(a64reg::X0, kEntry);
        Emulator.setRegister(a64reg::X1, 0);
      };
      auto Read = [&](NdOpEmulator &Emulator, bool Written) {
        for (const auto &Step : liftMemory(FullRead->Word, false))
          ASSERT_TRUE(Emulator.step(Step));
        const auto Observed = Emulator.getRegisterBytes(a64reg::V0);
        ASSERT_TRUE(Observed);
        ASSERT_EQ(Observed->size(), kVectorBytes);
        for (unsigned I = 0; I < kVectorBytes; ++I)
          EXPECT_EQ((*Observed)[I], Written && I < Case.Access ? Bytes[I] : 0);
      };
      NdOpEmulator Unknown(Image);
      Prepare(Unknown);
      if (Case.Access > 1)
        Unknown.setRegisterBytes(
            a64reg::V0,
            llvm::ArrayRef<uint8_t>(Bytes).take_front(Case.Access - 1));
      EXPECT_FALSE(Unknown.step(Operations.front()));
      Read(Unknown, false);

      NdOpEmulator Known(Image);
      Prepare(Known);
      Known.setRegisterBytes(
          a64reg::V0, llvm::ArrayRef<uint8_t>(Bytes).take_front(Case.Access));
      for (const auto &Step : Operations)
        ASSERT_TRUE(Known.step(Step));
      Read(Known, true);

      Image.Segments.front().Flags = SegmentFlags::Readable;
      NdOpEmulator ReadOnly(Image);
      Prepare(ReadOnly);
      ReadOnly.setRegisterBytes(a64reg::V0, Bytes);
      for (const auto &Step : Operations)
        if (Step.Opcode != NdOp::INTRINSIC)
          ASSERT_TRUE(ReadOnly.step(Step));
      EXPECT_FALSE(ReadOnly.step(originalMemoryOperation(Operations)));
      Read(ReadOnly, false);
      Image.Segments.front().Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    }
  }
}

TEST(AArch64VectorAddressMemory,
     DisjointScalableStateSurvivesWhilePhysicalAliasesAreRefused) {
  for (const auto &Case : VectorMemoryCases)
    for (unsigned Register : {0U, kVectorSeparateRegister}) {
      SCOPED_TRACE(Case.Word);
      SCOPED_TRACE(Register);
      std::vector<uint32_t> Words{kVectorSVEPtrue, kVectorSVEIndex};
      if (!Case.Load)
        Words.push_back(kVectorAliasSeed | Register);
      Words.push_back(Case.Word | Register);
      Words.push_back(kVectorSVELast);
      Words.push_back(kVectorReturnWord);
      const auto Function = decodedVectorMemoryWords(Case, Words);
      ASSERT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
      llvm::LLVMContext Context;
      if (!Register) {
        EXPECT_NE(validateAArch64SVEState(Function, Arch::AArch64), nullptr);
        EXPECT_THROW(MedToHighConverter().convert(Function, Arch::AArch64),
                     std::runtime_error);
        EXPECT_THROW(
            MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64),
            std::runtime_error);
        continue;
      }
      ASSERT_EQ(validateAArch64SVEState(Function, Arch::AArch64), nullptr);
      const auto Module =
          MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
      ASSERT_TRUE(Module);
      EXPECT_FALSE(llvm::verifyModule(*Module));
      const auto High = MedToHighConverter().convert(Function, Arch::AArch64);
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Format = BinaryFormat::MachO;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      EXPECT_TRUE(HighCEmitter().emit({High}, Out, Options));
      EXPECT_FALSE(Source.empty());
      // LLVMC has no native scalable-vector carrier. The valid memory
      // operation cannot turn that existing refusal into a scalar fallback.
      EXPECT_THROW(LLVMCEmitter().emit(*Module, Out, Options),
                   std::runtime_error);
      const auto Object = Codegen().compile(
          *Module, Arch::AArch64, BinaryFormat::ELF, kVectorSVETriple);
      ASSERT_TRUE(Object.Success);
      auto Parsed = llvm::object::ObjectFile::createObjectFile(
          llvm::MemoryBufferRef(llvm::StringRef(reinterpret_cast<const char *>(
                                                    Object.ObjectData.data()),
                                                Object.ObjectData.size()),
                                kObject));
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      unsigned Count = 0;
      for (const auto &Section : (*Parsed)->sections()) {
        if (!Section.isText())
          continue;
        auto Bytes = Section.getContents();
        ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
        for (size_t I = 0; I + kWordBytes <= Bytes->size(); I += kWordBytes)
          Count += readLE<uint32_t>(reinterpret_cast<const uint8_t *>(
                       Bytes->data() + I)) == Case.Scratch;
      }
      EXPECT_EQ(Count, 1);
    }
}

TEST(AArch64VectorAddressMemory,
     DecodedRegisterTransportExecutesBothSourceRoutesAndObjects) {
  executeVectorMemoryBatch(VectorMemoryMode::Decoded);
}

TEST(AArch64VectorAddressMemory,
     LoadedScalarViewsReplaceOlderLifetimesAcrossBranches) {
  executeVectorMemoryBatch(VectorMemoryMode::AcrossBranch);
}

TEST(AArch64VectorAddressMemory,
     ScalarFloatingParametersAndReturnsKeepTheirNativeABI) {
  std::vector<MedFunc> Functions;
  std::string Wrappers, Entries;
  std::string Declarations = kVectorNativeIncludes;
  for (size_t I = 0; I < std::size(VectorMemoryCases); ++I) {
    const auto &Case = VectorMemoryCases[I];
    if (Case.Access != kVectorLowWordBytes && Case.Access != kVectorHalfBytes)
      continue;
    SCOPED_TRACE(Case.Word);
    auto Function =
        decodedVectorMemoryWords(Case, {Case.Word, kVectorReturnWord});
    Function.ReturnValueEvidence = MedReturnValueEvidence::Unknown;
    inferMedTypes(Function, Arch::AArch64);
    ASSERT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
    if (Case.Load) {
      ASSERT_TRUE(Function.ReturnType);
      EXPECT_EQ(Function.ReturnType->Kind, NdTypeKind::Float);
      EXPECT_EQ(Function.ReturnType->Size, Case.Access);
      EXPECT_EQ(Function.Params.size(), kVectorLoadParameters);
    } else {
      ASSERT_EQ(Function.Params.size(), kVectorAliasParameters);
      EXPECT_EQ(Function.FPParamScalarBytes.at(a64reg::V0), Case.Access);
    }
    const auto *Type = Case.Access == kVectorLowWordBytes ? kVectorFPTypeSingle
                                                          : kVectorFPTypeDouble;
    const auto Integer =
        llvm::formatv(kVectorFPIntegerType, Case.Access * CHAR_BIT).str();
    const auto IntegerReturn =
        llvm::formatv(kNativeUnsignedType, kScalarBytes * CHAR_BIT).str();
    const auto Body =
        Case.Load
            ? llvm::formatv(kVectorFPLoadBody, Integer, Function.Name).str()
            : llvm::formatv(kVectorFPStoreBody, Function.Name, Type, Integer)
                  .str();
    Wrappers += llvm::formatv(kVectorFPWrapper, I, Body).str();
    Entries +=
        llvm::formatv(
            kVectorFPCase, I, Case.Access, unsigned{1} << Case.Shift,
            unsigned(Case.SignedIndex || Case.IndexBytes == kVectorHalfBytes),
            unsigned(Case.IndexBytes == kVectorLowWordBytes),
            unsigned(Case.Load))
            .str();
    Declarations +=
        llvm::formatv(
            kVectorFPDecl, Case.Load ? Type : IntegerReturn, Function.Name,
            llvm::formatv(kVectorIndexCType, Case.IndexBytes * CHAR_BIT).str(),
            Case.Load ? kEmptyNativeArgument
                      : llvm::formatv(kVectorFPParameter, Type).str())
            .str();
    Functions.push_back(std::move(Function));
  }
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(Functions, Context, kName, Arch::AArch64);
  ASSERT_TRUE(Module);
  for (const auto &Function : Functions) {
    const auto *Emitted = Module->getFunction(Function.Name);
    ASSERT_NE(Emitted, nullptr);
    if (Function.ReturnType->Kind == NdTypeKind::Float)
      EXPECT_TRUE(Emitted->getReturnType()->isFloatingPointTy());
    else
      EXPECT_TRUE(Emitted->getArg(kVectorLoadParameters)
                      ->getType()
                      ->isFloatingPointTy());
  }
  optimizeMemory(*Module);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  const auto Harness = llvm::formatv(kVectorFPHarness, Wrappers, Entries).str();
  for (bool LLVMRoute : {false, true}) {
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (LLVMRoute)
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
    else {
      std::vector<HighFunc> High;
      for (const auto &Function : Functions)
        High.push_back(MedToHighConverter().convert(Function, Arch::AArch64));
      ASSERT_TRUE(HighCEmitter().emit(High, Out, Options));
    }
#if defined(__APPLE__) && defined(__aarch64__)
    compileMemoryC(Source + Harness, true);
#else
    compileMemoryC(Source + Harness, false);
#endif
  }
  const auto Object =
      Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(Object.Success);
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<kVectorPathStorage> ObjectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kOutputExtension,
                                                  ObjectPath));
  llvm::FileRemover Remove(ObjectPath);
  std::error_code Error;
  llvm::raw_fd_ostream Output(ObjectPath, Error);
  ASSERT_FALSE(Error);
  Output.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
               Object.ObjectData.size());
  Output.close();
  compileMemoryC(Declarations + Harness, true, {}, ObjectPath);
#endif
}

TEST(AArch64VectorAddressMemory,
     MalformedVectorShapesCannotReachFilteredEmission) {
  for (const auto &Case : VectorMemoryCases) {
    auto Function = vectorMemoryFunction(Case);
    auto &Operations = Function.Blocks.front().Ops;
    const auto At = std::find_if(Operations.begin(), Operations.end(),
                                 aarch64AddressMemoryMedShapeIsValid);
    ASSERT_NE(At, Operations.end());
    SCOPED_TRACE(Case.Word);
    auto Invalid = *At;
    if (Case.Load)
      ++Invalid.Output.Size;
    else
      ++Invalid.Inputs[kAddressMemoryDataOperand + 1].Size;
    EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Invalid));
    *At = Invalid;
    EXPECT_FALSE(verifyMedFunc(Function, Function.Name.c_str()));
    llvm::LLVMContext Context;
    const std::vector<char> Mask{0};
    EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, kName,
                                       Arch::AArch64, {}, nullptr,
                                       BinaryFormat::MachO, false, &Mask),
                 std::runtime_error);
  }
}

TEST(AArch64VectorAddressMemory, TypedVectorAsmRequiresItsOwnRegisterClass) {
  for (bool Load : {false, true}) {
    const auto Case =
        std::find_if(std::begin(VectorMemoryCases), std::end(VectorMemoryCases),
                     [&](const auto &Case) { return Case.Load == Load; });
    ASSERT_NE(Case, std::end(VectorMemoryCases));
    const auto Function = vectorMemoryFunction(*Case);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    bool Found = false;
    for (auto &Block : *Module->getFunction(Function.Name))
      for (auto &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction)) {
          auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
          if (!Asm)
            continue;
          EXPECT_TRUE(validateAArch64AddressMemoryCall(Arch::AArch64, *Call));
          auto *Wrong = llvm::InlineAsm::get(
              Asm->getFunctionType(), Asm->getAsmString(),
              Load ? kAddressLoadConstraints : kAddressStoreConstraints, true);
          Call->setCalledOperand(Wrong);
          EXPECT_THROW(validateAArch64AddressMemoryCall(Arch::AArch64, *Call),
                       std::invalid_argument);
          Found = true;
        }
    EXPECT_TRUE(Found);
  }
}

namespace {
#define IMMEDIATE_MEMORY_VALUE(Type, Name, Value)                              \
  inline constexpr Type Name = Value;
#define IMMEDIATE_MEMORY_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64ImmediateMemoryFixtures.def"

struct ImmediateMemoryCase {
  uint32_t Word;
  uint32_t Scratch;
  bool Vector;
  bool Unsigned;
  unsigned Access;
  unsigned Result;
  bool Signed;
  int64_t Offset;
  uint64_t RawIndex = 0;
};
constexpr ImmediateMemoryCase ImmediateMemoryCases[] = {
#define IMMEDIATE_MEMORY_CASE(Word, Scratch, Vector, Unsigned, Access, Result, \
                              Signed, Offset)                                  \
  {Word, Scratch, Vector, Unsigned, Access, Result, Signed, Offset},
#include "AArch64ImmediateMemoryFixtures.def"
};

MedFunc immediateMemoryFunction(const ImmediateMemoryCase &Case, size_t Index) {
  const va_t Entry = kEntry + Index * kVectorFunctionStride;
  auto Function =
      Case.Vector
          ? vectorMemoryFunction(
                {Case.Word, Case.Scratch, Case.Result != 0, Case.Access,
                 aarch64AddressMemoryInstruction(Case.Word)->indexBytes(), 0,
                 false},
                Entry)
          : memoryFunction(Case.Word);
  Function.Name = kImmediateNamePrefix + llvm::utohexstr(Case.Word);
  Function.Entry = Entry;
  Function.Blocks.front().StartAddr = Entry;
  if (Case.Vector && Case.Result) {
    auto &Operations = Function.Blocks.front().Ops;
    const auto At =
        std::find_if(Operations.begin(), Operations.end(),
                     [](const auto &Op) { return Op.Opcode == NdOp::INT_XOR; });
    EXPECT_NE(At, Operations.end());
    if (At != Operations.end()) {
      MedOp Compare;
      Compare.Opcode = NdOp::INT_NOTEQUAL;
      Compare.Output.Kind = MedVar::Temp;
      Compare.Output.TheArch = Arch::AArch64;
      Compare.Output.Id = 0;
      for (const auto &Operation : Operations)
        if (Operation.Output.Kind == MedVar::Temp)
          Compare.Output.Id =
              std::max(Compare.Output.Id, Operation.Output.Id + 1);
      Compare.Output.Size = 1;
      Compare.addInput(Function.Params[1]);
      Compare.addInput(MedVar::makeConst(0, Function.Params[1].Size));
      const auto Low = At->Inputs[0], High = At->Inputs[1];
      At->Opcode = NdOp::SELECT;
      At->NumInputs = 0;
      At->addInput(Compare.Output);
      At->addInput(High);
      At->addInput(Low);
      Operations.insert(At, Compare);
    }
  }
  if (aarch64AddressMemoryInstruction(Case.Word)->hasRegisterIndex())
    for (auto &Op : Function.Blocks.front().Ops)
      if (aarch64AddressMemoryMedShapeIsValid(Op))
        Op.Inputs[kAddressMemoryIndexOperand + 1] = MedVar::makeConst(
            Case.RawIndex,
            aarch64AddressMemoryInstruction(Case.Word)->indexBytes());
  return Function;
}

std::vector<ImmediateMemoryCase> stackMemoryCases(bool Immediate) {
  std::vector<ImmediateMemoryCase> Cases;
  if (Immediate)
    Cases.assign(std::begin(ImmediateMemoryCases),
                 std::end(ImmediateMemoryCases));
  else {
    auto Add = [&](uint32_t Word, uint32_t Scratch, bool Vector,
                   unsigned Access, unsigned Result, bool Signed,
                   unsigned IndexBytes, unsigned Shift, bool SignedIndex) {
      const uint64_t Index =
          SignedIndex ? (IndexBytes == kWordBytes ? kStackNegativeNarrowIndex
                                                  : kStackNegativeWideIndex)
                      : 1;
      const int64_t Displacement = int64_t(1) << Shift;
      Cases.push_back({Word, Scratch, Vector, false, Access, Result, Signed,
                       SignedIndex ? -Displacement : Displacement, Index});
    };
    for (const auto &Case : FamilyCases)
      Add(Case.Word, Case.Scratch, false, Case.Access, Case.Result, Case.Signed,
          Case.IndexBytes, Case.Shift, Case.Word & kStackSignedIndexBit);
    for (const auto &Case : VectorMemoryCases)
      Add(Case.Word, Case.Scratch, true, Case.Access,
          Case.Load ? kVectorBytes : 0, false, Case.IndexBytes, Case.Shift,
          Case.SignedIndex);
  }
  for (auto &Case : Cases) {
    Case.Word = (Case.Word & ~kStackBaseMask) | kStackBaseMask;
    Case.Scratch = (Case.Scratch & ~kStackBaseMask) | kStackBaseMask;
  }
  return Cases;
}
} // namespace

TEST(AArch64ImmediateMemory,
     ImmediateFieldsAndUnprivilegedVariantsStayEncoded) {
  ASSERT_EQ(std::size(ImmediateMemoryCases), kImmediateCaseCount);
#define IMMEDIATE_MEMORY_ID(Id, Expected)                                      \
  EXPECT_EQ(static_cast<uint16_t>(Intrinsic::Id), Expected);
#include "AArch64ImmediateMemoryFixtures.def"
  for (size_t I = 0; I != std::size(ImmediateMemoryCases); ++I) {
    const auto &Case = ImmediateMemoryCases[I];
    SCOPED_TRACE(Case.Word);
    const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    EXPECT_FALSE(Instruction->hasRegisterIndex());
    EXPECT_EQ(Instruction->isVector(), Case.Vector);
    EXPECT_EQ(Instruction->accessBytes(), Case.Access);
    EXPECT_EQ(Instruction->operation().ResultBytes, Case.Result);
    EXPECT_EQ(Instruction->operation().Signed, Case.Signed);
    EXPECT_EQ(Instruction->addressOffset(0),
              static_cast<uint64_t>(Case.Offset));
    EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
    EXPECT_EQ(Instruction->index(), kZeroRegister);
    EXPECT_EQ(Instruction->indexShift(), 0);
    EXPECT_EQ(Instruction->controlIndependentAddress(kStorageBase, 0),
              kStorageBase + static_cast<uint64_t>(Case.Offset));
    EXPECT_FALSE(Instruction->controlIndependentAddress(kStorageBase, 1));
    EXPECT_FALSE(Instruction->controlIndependentAddressRange(kStorageBase, 1));
    auto Function = immediateMemoryFunction(Case, I);
    auto &Operations = Function.Blocks.front().Ops;
    const auto At = std::find_if(Operations.begin(), Operations.end(),
                                 aarch64AddressMemoryMedShapeIsValid);
    ASSERT_NE(At, Operations.end());
    EXPECT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
    auto Invalid = *At;
    Invalid.Inputs[kAddressMemoryIndexOperand + 1] =
        MedVar::makeConst(1, kScalarBytes);
    EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Invalid));
    Invalid.Inputs[kAddressMemoryIndexOperand + 1] = Function.Params[0];
    EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Invalid));
  }
#define IMMEDIATE_MEMORY_REJECT(Word)                                          \
  EXPECT_FALSE(aarch64AddressMemoryInstruction(Word)) << Word;
#include "AArch64ImmediateMemoryFixtures.def"
}

TEST(AArch64ImmediateMemory,
     ByteSpansIncludeNegativeOffsetsAndTheMaximumQAccess) {
  const auto Maximum = aarch64AddressMemoryInstruction(kImmediateMaximumQLoad);
  ASSERT_TRUE(Maximum);
  const auto Span = Maximum->controlIndependentSpan(kStorageBase, 0);
  ASSERT_TRUE(Span);
  EXPECT_EQ(Span->Begin, kStorageBase);
  EXPECT_EQ(Span->Bytes, kImmediateSpanBytes);
  const auto Negative =
      aarch64AddressMemoryInstruction(kImmediateNegativeQLoad);
  ASSERT_TRUE(Negative);
  const auto Prefix = Negative->controlIndependentSpan(kStorageBase, 0);
  ASSERT_TRUE(Prefix);
  EXPECT_EQ(Prefix->Begin, kStorageBase - kImmediateBasePosition);
  EXPECT_EQ(Prefix->Bytes, kImmediateBasePosition);
  EXPECT_FALSE(Maximum->controlIndependentSpan(kStorageBase, 1));
  EXPECT_FALSE(Negative->controlIndependentSpan(0, 0));
  EXPECT_FALSE(Maximum->controlIndependentSpan(kVectorRangeRegionEnd - 1, 0));
  for (const auto &Case : ImmediateMemoryCases) {
    const auto Instruction = *aarch64AddressMemoryInstruction(Case.Word);
    const auto Range = Instruction.controlIndependentSpan(kStorageBase, 0);
    ASSERT_TRUE(Range);
    const uint64_t Start = kStorageBase + static_cast<uint64_t>(Case.Offset);
    EXPECT_LE(Range->Begin, Start);
    EXPECT_LE(Range->Begin, kStorageBase);
    EXPECT_GE(Range->Begin + Range->Bytes, Start + Case.Access);
    EXPECT_GE(Range->Begin + Range->Bytes, kStorageBase);
  }
}

TEST(AArch64ImmediateMemory,
     EveryImmediateValueAgreesWithIndependentOperandDetail) {
  csh Handle;
  ASSERT_EQ(cs_open(CS_ARCH_AARCH64, CS_MODE_LITTLE_ENDIAN, &Handle),
            CS_ERR_OK);
  llvm::scope_exit Close([&] { cs_close(&Handle); });
  ASSERT_EQ(cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON), CS_ERR_OK);
  cs_insn *Decoded = cs_malloc(Handle);
  ASSERT_NE(Decoded, nullptr);
  llvm::scope_exit Free([&] { cs_free(Decoded, 1); });
  unsigned Count = 0;
  for (const auto &Case : ImmediateMemoryCases) {
    if (Case.Offset)
      continue;
    const unsigned Slots =
        Case.Unsigned ? kImmediateUnsignedSlots : kImmediateSignedSlots;
    for (unsigned I = 0; I != Slots; ++I) {
      const int64_t Offset = Case.Unsigned
                                 ? int64_t(I) * Case.Access
                                 : int64_t(I) - kImmediateSignedSlots / 2;
      const uint32_t Field =
          Case.Unsigned ? I << kImmediateUnsignedShift
                        : (uint32_t(Offset) & kImmediateSignedFieldMask)
                              << kImmediateSignedShift;
      const uint32_t Word = Case.Word | Field;
      SCOPED_TRACE(Word);
      const auto Instruction = aarch64AddressMemoryInstruction(Word);
      ASSERT_TRUE(Instruction);
      EXPECT_EQ(Instruction->addressOffset(0), static_cast<uint64_t>(Offset));
      unsigned Identity = 0;
      for (bool Scratch : {false, true}) {
        const uint32_t Encoding = Scratch ? Instruction->scratchWord() : Word;
        std::array<uint8_t, kWordBytes> Bytes;
        writeLE<uint32_t>(Bytes.data(), Encoding);
        const uint8_t *Input = Bytes.data();
        size_t Size = Bytes.size();
        uint64_t Address = kEntry;
        ASSERT_TRUE(cs_disasm_iter(Handle, &Input, &Size, &Address, Decoded));
        ASSERT_EQ(Decoded->detail->aarch64.op_count, 2);
        const auto &Memory = Decoded->detail->aarch64.operands[1];
        ASSERT_EQ(Memory.type, AARCH64_OP_MEM);
        EXPECT_EQ(Memory.mem.disp, Offset);
        EXPECT_EQ(static_cast<uint64_t>(Memory.mem.disp),
                  Instruction->addressOffset(0));
        if (!Scratch)
          Identity = Decoded->id;
        else {
          EXPECT_EQ(Decoded->id, Identity);
          EXPECT_EQ(Memory.mem.base, AARCH64_REG_X16);
        }
      }
      ++Count;
    }
  }
  EXPECT_EQ(Count, kImmediateEncodingCount);
}

namespace {
void executeImmediateMemoryFamilies(llvm::ArrayRef<ImmediateMemoryCase> Cases,
                                    bool Stack, bool Immediate) {
  llvm::SmallString<kVectorPathStorage> ArtifactDirectory;
  if (const auto *Root =
          std::getenv(Stack || !Immediate ? kStackArtifactEnvironment
                                          : kImmediateArtifactEnvironment);
      Root) {
    ASSERT_FALSE(llvm::sys::fs::create_directories(Root));
    llvm::SmallString<kVectorPathStorage> Prefix(Root);
    llvm::sys::path::append(
        Prefix,
        Stack
            ? (Immediate ? kStackArtifactPrefix : kStackRegisterArtifactPrefix)
            : (Immediate ? kImmediateArtifactPrefix : kRegisterArtifactPrefix));
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory(Prefix, ArtifactDirectory));
    ::testing::Test::RecordProperty(
        Stack ? (Immediate ? kStackArtifactProperty
                           : kStackRegisterArtifactProperty)
              : (Immediate ? kImmediateArtifactProperty
                           : kRegisterArtifactProperty),
        ArtifactDirectory.str().str());
  }
  auto SaveArtifact = [&](llvm::StringRef Name, llvm::StringRef Bytes) {
    if (ArtifactDirectory.empty())
      return;
    llvm::SmallString<kVectorPathStorage> Path(ArtifactDirectory);
    llvm::sys::path::append(Path, Name);
    std::error_code Error;
    llvm::raw_fd_ostream Output(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Output << Bytes;
    Output.close();
    EXPECT_FALSE(Output.has_error());
  };
  std::vector<MedFunc> Functions;
  std::string Wrappers, Entries;
  std::string Declarations = kVectorNativeIncludes;
  for (size_t I = 0; I != Cases.size(); ++I) {
    const auto &Case = Cases[I];
    auto Function = immediateMemoryFunction(Case, I);
    ASSERT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
    const auto Call =
        llvm::formatv(Case.Result   ? kImmediateNativeLoadCall
                      : Case.Vector ? kImmediateNativeStoreVectorCall
                                    : kImmediateNativeStoreScalarCall,
                      Function.Name)
            .str();
    Wrappers +=
        Case.Vector && Case.Result
            ? llvm::formatv(kImmediateNativeVectorLoadWrapper, I, Function.Name)
                  .str()
            : llvm::formatv(kImmediateNativeWrapper, I, Call).str();
    Entries +=
        llvm::formatv(kImmediateNativeCase, I, Case.Access, Case.Result,
                      unsigned(Case.Signed), unsigned(Case.Vector), Case.Offset)
            .str();
    const auto Parameters =
        Case.Result   ? std::string{}
        : Case.Vector ? std::string(kImmediateNativeStoreVectorParameters)
                      : llvm::formatv(kImmediateNativeStoreScalarParameter,
                                      Case.Access * CHAR_BIT)
                            .str();
    Declarations +=
        llvm::formatv(kImmediateNativeDecl, Function.Name, Parameters).str();
    Functions.push_back(std::move(Function));
  }
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(Functions, Context, kName, Arch::AArch64);
  ASSERT_TRUE(Module);
  optimizeMemory(*Module);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  const auto Harness =
      llvm::formatv(kImmediateNativeHarness, Wrappers, Entries).str();
  SaveArtifact(kImmediateHarnessArtifact, Declarations + Harness);
  for (bool LLVMRoute : {false, true}) {
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (LLVMRoute)
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
    else {
      std::vector<HighFunc> High;
      for (const auto &Function : Functions)
        High.push_back(MedToHighConverter().convert(Function, Arch::AArch64));
      ASSERT_TRUE(HighCEmitter().emit(High, Out, Options));
    }
    SaveArtifact(LLVMRoute ? kImmediateLLVMCArtifact : kImmediateHighCArtifact,
                 Source);
#if defined(__APPLE__) && defined(__aarch64__)
    compileMemoryC(Source + Harness, true);
#else
    compileMemoryC(Source, false);
#endif
  }
  const auto Object =
      Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(Object.Success);
  const llvm::MemoryBufferRef Buffer(
      llvm::StringRef(reinterpret_cast<const char *>(Object.ObjectData.data()),
                      Object.ObjectData.size()),
      kObject);
  SaveArtifact(
      kImmediateMachOArtifact,
      llvm::StringRef(reinterpret_cast<const char *>(Object.ObjectData.data()),
                      Object.ObjectData.size()));
  for (const auto &Case : Cases)
    inspectMemoryObject(Buffer, Case.Scratch);
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<kVectorPathStorage> ObjectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kOutputExtension,
                                                  ObjectPath));
  llvm::FileRemover Remove(ObjectPath);
  std::error_code Error;
  llvm::raw_fd_ostream Output(ObjectPath, Error);
  ASSERT_FALSE(Error);
  Output.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
               Object.ObjectData.size());
  Output.close();
  compileMemoryC(Declarations + Harness, true, {}, ObjectPath);
#endif
  if (!ArtifactDirectory.empty()) {
    const auto ELF =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::ELF);
    ASSERT_TRUE(ELF.Success);
    SaveArtifact(
        kImmediateELFArtifact,
        llvm::StringRef(reinterpret_cast<const char *>(ELF.ObjectData.data()),
                        ELF.ObjectData.size()));
  }
}
} // namespace

TEST(AArch64ImmediateMemory,
     BothCProjectionsAndLLVMExecuteEveryImmediateFamily) {
  executeImmediateMemoryFamilies(ImmediateMemoryCases, false, true);
}

TEST(AArch64StackAddressMemory,
     BothCProjectionsAndLLVMExecuteEveryImmediateFamily) {
  executeImmediateMemoryFamilies(stackMemoryCases(true), true, true);
}

TEST(AArch64StackAddressMemory,
     BothCProjectionsAndLLVMExecuteEveryRegisterFamily) {
  auto Cases = stackMemoryCases(false);
  ASSERT_EQ(Cases.size(), kStackRegisterCaseCount);
  executeImmediateMemoryFamilies(Cases, true, false);
  for (auto &Case : Cases) {
    Case.Word = (Case.Word & ~kStackBaseMask) | kStackGeneralBaseBits;
    Case.Scratch = (Case.Scratch & ~kStackBaseMask) | kStackGeneralBaseBits;
  }
  executeImmediateMemoryFamilies(Cases, false, false);
}

TEST(AArch64ImmediateMemory, TracingUsesTheEncodedOffsetAndExactDataSlice) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = kImmediateMemoryBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  for (size_t I = 0; I != Data.Data.size(); ++I)
    Data.Data[I] = static_cast<uint8_t>(I);
  Image.Segments.push_back(Data);
  std::array<uint8_t, kVectorBytes> Seed;
  writeLE<uint64_t>(Seed.data(), kVectorStoreLow);
  writeLE<uint64_t>(Seed.data() + kScalarBytes, kVectorStoreHigh);
  for (const auto &Case : ImmediateMemoryCases)
    for (bool Strict : {false, true}) {
      SCOPED_TRACE(Case.Word);
      const auto Instruction = *aarch64AddressMemoryInstruction(Case.Word);
      const va_t Base = kEntry + kImmediateBasePosition + kVectorBytes;
      const va_t Address = Base + static_cast<uint64_t>(Case.Offset);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(a64reg::X1, Base);
      Emulator.setRegisterBytes(a64reg::V0, Seed);
      LowOp Memory;
      Memory.Opcode = NdOp::INTRINSIC;
      Memory.addInput(NdVar::cst(
          static_cast<uint64_t>(Instruction.operation().Id), kIdBytes));
      Memory.addInput(NdVar::reg(a64reg::X1, kScalarBytes));
      Memory.addInput(NdVar::scalar(0, kScalarBytes));
      Memory.addInput(NdVar::cst(Case.Word, kWordBytes));
      if (Case.Result)
        Memory.Output = NdVar::reg(a64reg::V0, Case.Access);
      else
        Memory.addInput(NdVar::reg(a64reg::V0, Case.Access));
      ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
      ASSERT_TRUE(Emulator.step(Memory));
      if (Case.Result) {
        const auto Value = Emulator.getRegisterBytes(a64reg::V0);
        ASSERT_TRUE(Value);
        for (unsigned I = 0; I != Case.Access; ++I)
          EXPECT_EQ((*Value)[I], Data.Data[Address - kEntry + I]);
        ASSERT_EQ(Emulator.getLoadRecords().size(), 1);
        EXPECT_EQ(Emulator.getLoadRecords().front().Addr, Address);
        EXPECT_EQ(Emulator.getLoadRecords().front().Size, Case.Access);
      } else {
        for (va_t Start :
             {Address - kVectorBytes, Address, Address + Case.Access}) {
          LowOp Read;
          Read.Opcode = NdOp::LOAD;
          Read.Output = NdVar::reg(a64reg::V0, kVectorBytes);
          Read.addInput(NdVar::dataAddress(Start, kScalarBytes));
          ASSERT_TRUE(Emulator.step(Read));
          const auto Value = Emulator.getRegisterBytes(a64reg::V0);
          ASSERT_TRUE(Value);
          for (unsigned I = 0; I != kVectorBytes; ++I) {
            const va_t Byte = Start + I;
            EXPECT_EQ((*Value)[I],
                      Byte >= Address && Byte - Address < Case.Access
                          ? Seed[Byte - Address]
                          : Data.Data[Byte - kEntry]);
          }
        }
      }
      Memory.Inputs[kAddressMemoryIndexOperand + 1] =
          NdVar::scalar(1, kScalarBytes);
      EXPECT_FALSE(Emulator.step(Memory));
    }
}

TEST(AArch64ImmediateMemory,
     StackAndImageRequirementsCannotBeHiddenByFiltering) {
  const uint32_t StackMask = kAddressMemoryRegisterMask
                             << kAddressMemoryBaseShift;
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  for (bool Vector : {false, true})
    for (bool Load : {false, true}) {
      const auto Case = std::find_if(
          std::begin(ImmediateMemoryCases), std::end(ImmediateMemoryCases),
          [&](const auto &Case) {
            return Case.Vector == Vector && bool(Case.Result) == Load;
          });
      ASSERT_NE(Case, std::end(ImmediateMemoryCases));
      auto Function = immediateMemoryFunction(*Case, 0);
      const std::vector<char> Mask{0};
      llvm::LLVMContext Context;
      EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, kName,
                                         Arch::AArch64, {}, &Image,
                                         BinaryFormat::MachO, false, &Mask),
                   std::runtime_error);
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Image = &Image;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      EXPECT_THROW(HighCEmitter().emit(
                       {MedToHighConverter().convert(Function, Arch::AArch64)},
                       Out, Options),
                   std::invalid_argument);
      const uint32_t Word = (Case->Word & ~StackMask) | StackMask;
      const auto Stack = aarch64AddressMemoryInstruction(Word);
      ASSERT_TRUE(Stack);
      EXPECT_TRUE(Stack->stackBase());
      EXPECT_FALSE(Stack->controlIndependentSpan(kStorageBase, 0));
      EXPECT_NE(aarch64AddressMemoryC(*Stack, kBaseCall, kIndexCall,
                                      Load ? kEmptyNativeArgument : kResultName)
                    .find(kStackSelectSP),
                std::string::npos);
      for (auto &Op : Function.Blocks.front().Ops)
        if (aarch64AddressMemoryMedShapeIsValid(Op)) {
          Op.Inputs[kAddressMemoryWordOperand + 1].ConstVal = Word;
          Op.Inputs[kAddressMemoryBaseOperand + 1].Kind = MedVar::Reg;
          Op.Inputs[kAddressMemoryBaseOperand + 1].RegOff = a64reg::SP;
        }
      EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, kName,
                                         Arch::AArch64, {}, nullptr,
                                         BinaryFormat::MachO, false, &Mask),
                   std::runtime_error);
    }
}

TEST(AArch64ImmediateMemory, AssemblyCannotConsumeAPhantomRegisterIndex) {
  const auto Function = immediateMemoryFunction(ImmediateMemoryCases[0], 0);
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
  ASSERT_TRUE(Module);
  bool Found = false;
  for (auto &Block : *Module->getFunction(Function.Name))
    for (auto &Instruction : Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction)) {
        if (!llvm::isa<llvm::InlineAsm>(Call->getCalledOperand()))
          continue;
        EXPECT_TRUE(validateAArch64AddressMemoryCall(Arch::AArch64, *Call));
        Call->setArgOperand(
            kAddressMemoryIndexOperand,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), 1));
        EXPECT_THROW(validateAArch64AddressMemoryCall(Arch::AArch64, *Call),
                     std::invalid_argument);
        Found = true;
      }
  EXPECT_TRUE(Found);
}

namespace {
LowFunc stackMemoryLowFunction(const ImmediateMemoryCase &Case) {
  const auto Instruction = *aarch64AddressMemoryInstruction(Case.Word);
  const auto SP = NdVar::reg(a64reg::SP, kScalarBytes);
  LowOp Subtract;
  Subtract.Opcode = NdOp::INT_SUB;
  Subtract.Addr = kEntry;
  Subtract.Output = SP;
  Subtract.addInput(SP);
  Subtract.addInput(NdVar::scalar(kStackFrameDistance, kScalarBytes));
  LowOp Memory;
  Memory.Opcode = NdOp::INTRINSIC;
  Memory.Addr = kEntry + kWordBytes;
  Memory.Seq = 1;
  Memory.addInput(
      NdVar::cst(static_cast<uint64_t>(Instruction.operation().Id), kIdBytes));
  Memory.addInput(SP);
  Memory.addInput(NdVar::scalar(Case.RawIndex, Instruction.indexBytes()));
  Memory.addInput(NdVar::cst(Case.Word, kWordBytes));
  if (Case.Result)
    Memory.Output =
        NdVar::reg(Case.Vector ? a64reg::V(0) : a64reg::X0, Case.Access);
  else
    Memory.addInput(NdVar::scalar(0, Case.Access));
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = kEntry + 2 * kWordBytes;
  auto Function = lowFunction(Memory);
  auto &Block = Function.Blocks.front();
  Block.Ops = {Subtract, Memory, Return};
  Block.EndAddr = Return.Addr + kWordBytes;
  Block.InstructionBoundaries.clear();
  return Function;
}
} // namespace

TEST(AArch64StackAddressMemory, NominalFootprintsKeepTheExactEntrySPBasis) {
  for (bool Immediate : {false, true})
    for (const auto &Case : stackMemoryCases(Immediate)) {
      SCOPED_TRACE(Case.Word);
      const auto Function = stackMemoryLowFunction(Case);
      const auto &Memory = Function.Blocks.front().Ops[1];
      ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
      EXPECT_FALSE(lowIntrinsicMemoryOperands(Memory).Complete);
      const auto Frame = analyzeSourceStackFrame(Function, Arch::AArch64);
      ASSERT_TRUE(Frame.Error.empty()) << Frame.Error;
      ASSERT_EQ(Frame.Accesses.size(), 1);
      const auto &Access = Frame.Accesses.front();
      EXPECT_EQ(Access.Instruction, Memory.Addr);
      EXPECT_EQ(Access.Sequence, Memory.Seq);
      EXPECT_EQ(Access.Offset, Case.Offset - kStackFrameDistance);
      EXPECT_EQ(Access.Bytes, Case.Access);
      EXPECT_EQ(Access.Write, Case.Result == 0);
      EXPECT_EQ(Access.OriginalWord, Case.Word);
      const auto Converted =
          LowToMedConverter().convert(Function, Arch::AArch64);
      EXPECT_EQ(Converted.FrameSize,
                std::max(kStackFrameDistance, -Access.Offset));
      EXPECT_EQ(Converted.FrameHeadroom,
                std::max(int64_t(0), Access.Offset + Access.Bytes));
    }
}

TEST(AArch64StackAddressMemory,
     UnknownOrExpiredInputsDoNotInventFrameAccesses) {
  const auto Cases = stackMemoryCases(false);
  auto Function = stackMemoryLowFunction(Cases.front());
  const auto Original = Function;
  Function.Blocks.front().Ops[1].Inputs[kAddressMemoryIndexOperand + 1] =
      NdVar::reg(a64reg::X0, kScalarBytes);
  EXPECT_TRUE(
      analyzeSourceStackFrame(Function, Arch::AArch64).Accesses.empty());
  Function = Original;
  Function.Blocks.front().Ops[0].Output.Size = kWordBytes;
  EXPECT_TRUE(
      analyzeSourceStackFrame(Function, Arch::AArch64).Accesses.empty());
  Function = Original;
  const auto Temporary = NdVar::tmp(TmpBase, kScalarBytes);
  auto &Capture = Function.Blocks.front().Ops[0];
  Capture.Opcode = NdOp::COPY;
  Capture.NumInputs = 1;
  Capture.Output = Temporary;
  Function.Blocks.front().Ops[1].Inputs[kAddressMemoryBaseOperand + 1] =
      Temporary;
  EXPECT_TRUE(
      analyzeSourceStackFrame(Function, Arch::AArch64).Accesses.empty());
  Function.FunctionTemporaries.push_back({Temporary.Offset, kWordBytes});
  EXPECT_TRUE(
      analyzeSourceStackFrame(Function, Arch::AArch64).Accesses.empty());
  Function.FunctionTemporaries.front().Bytes = kScalarBytes;
  EXPECT_EQ(analyzeSourceStackFrame(Function, Arch::AArch64).Accesses.size(),
            1);
  Function = Original;
  Function.Blocks.front().Ops[1].Inputs[kAddressMemoryWordOperand + 1].Size = 1;
  EXPECT_EQ(analyzeSourceStackFrame(Function, Arch::AArch64).Error,
            kAddressMemoryMalformed);
  EXPECT_EQ(analyzeSourceStackFrame(Original, Arch::X64).Error,
            kAddressMemoryMalformed);
}

TEST(AArch64StackAddressMemory, JoinsAndIndependentRootsKeepTheirOwnSPBasis) {
  auto Function = stackMemoryLowFunction(stackMemoryCases(false).front());
  const auto Memory = Function.Blocks.front().Ops[1];
  auto Subtract = Function.Blocks.front().Ops.front();
  Function.Blocks.front().Ops.resize(1);
  Function.Blocks.front().Succs = {1, 2};
  for (int Id : {1, 2, 3}) {
    LowBlock Block;
    Block.Id = Id;
    Block.StartAddr = kEntry + Id * kVectorFunctionStride;
    Block.EndAddr = Block.StartAddr + kWordBytes;
    if (Id != 3)
      Block.Succs = {3};
    Function.Blocks.push_back(Block);
  }
  Subtract.Addr = Function.Blocks[1].StartAddr;
  Subtract.Inputs[1] = NdVar::scalar(kScalarBytes, kScalarBytes);
  Function.Blocks[1].Ops = {Subtract};
  auto &Join = Function.Blocks[3];
  Join.Ops = {Memory};
  Join.Ops.front().Addr = Join.StartAddr;
  EXPECT_TRUE(
      analyzeSourceStackFrame(Function, Arch::AArch64).Accesses.empty());
  Subtract.Addr = Function.Blocks[2].StartAddr;
  Function.Blocks[2].Ops = {Subtract};
  ASSERT_EQ(analyzeSourceStackFrame(Function, Arch::AArch64).Accesses.size(),
            1);
  Join.Succs = {3};
  ASSERT_EQ(analyzeSourceStackFrame(Function, Arch::AArch64).Accesses.size(),
            1);
  Function.ModuleAnalysisRoots.insert(Join.StartAddr);
  EXPECT_TRUE(
      analyzeSourceStackFrame(Function, Arch::AArch64).Accesses.empty());
}

TEST(AArch64StackAddressMemory,
     SourceBoundariesRequireAnExternalRawSPParameter) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  for (bool Immediate : {false, true})
    for (bool Vector : {false, true})
      for (bool Load : {false, true}) {
        const auto Cases = stackMemoryCases(Immediate);
        const auto Case =
            std::find_if(Cases.begin(), Cases.end(), [&](const auto &C) {
              return C.Vector == Vector && bool(C.Result) == Load;
            });
        ASSERT_NE(Case, Cases.end());
        for (auto Kind :
             {MedVar::Param, MedVar::Reg, MedVar::Temp, MedVar::Const})
          for (bool WithImage : {false, true}) {
            if (Kind == MedVar::Param && !WithImage)
              continue;
            SCOPED_TRACE(Case->Word);
            SCOPED_TRACE(static_cast<unsigned>(Kind));
            SCOPED_TRACE(WithImage);
            auto Function = immediateMemoryFunction(*Case, 0);
            const auto OriginalHigh =
                MedToHighConverter().convert(Function, Arch::AArch64);
            MedVar RejectedBase;
            for (auto &Op : Function.Blocks.front().Ops)
              if (aarch64AddressMemoryMedShapeIsValid(Op)) {
                auto &Base = Op.Inputs[kAddressMemoryBaseOperand + 1];
                Base.Kind = Kind;
                Base.RegOff = a64reg::SP;
                Base.ConstVal = kEntry;
                RejectedBase = Base;
              }
            for (bool Hidden : {false, true}) {
              llvm::LLVMContext Context;
              const std::vector<char> Mask{0};
              EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, kName,
                                                 Arch::AArch64, {},
                                                 WithImage ? &Image : nullptr,
                                                 BinaryFormat::MachO, false,
                                                 Hidden ? &Mask : nullptr),
                           std::runtime_error);
              // Convert valid IR first. An undefined Med temporary can already
              // fail the vector-state owner; independently test the HighIR
              // source boundary without weakening that earlier refusal.
              auto High = OriginalHigh;
              std::function<void(const ExprPtr &)> RejectBase;
              RejectBase = [&](const ExprPtr &Expression) {
                if (!Expression)
                  return;
                if (isAArch64AddressMemoryIntrinsic(Expression->IntrinsicId))
                  Expression->Operands[kAddressMemoryBaseOperand] =
                      Kind == MedVar::Const
                          ? HighExpr::makeConst(kEntry, kScalarBytes)
                          : HighExpr::makeVar(
                                RejectedBase,
                                NdType::makeInt(kScalarBytes, false));
                Expression->forEachChildExpr(RejectBase);
              };
              walkStmts(High.Body, [&](const HighStmt &Statement) {
                forEachExpr(Statement, RejectBase);
              });
              if (Hidden) {
                HighStmt Branch;
                Branch.Kind = StmtKind::If;
                Branch.Cond = HighExpr::makeConst(0, 1);
                Branch.Body = std::move(High.Body);
                High.Body = {std::move(Branch)};
              }
              CEmitterOptions Options;
              Options.TheArch = Arch::AArch64;
              Options.Image = WithImage ? &Image : nullptr;
              std::string Source;
              llvm::raw_string_ostream Out(Source);
              EXPECT_THROW(HighCEmitter().emit({High}, Out, Options),
                           std::invalid_argument);
              EXPECT_TRUE(Source.empty());
            }
          }
      }
}

TEST(AArch64StackAddressMemory, StackAssemblyRequiresTheLiteralSPAndClobber) {
  for (bool Immediate : {false, true})
    for (bool Vector : {false, true})
      for (bool Load : {false, true}) {
        const auto Cases = stackMemoryCases(Immediate);
        const auto At =
            std::find_if(Cases.begin(), Cases.end(), [&](const auto &C) {
              return C.Vector == Vector && bool(C.Result) == Load;
            });
        ASSERT_NE(At, Cases.end());
        auto Function = immediateMemoryFunction(*At, 0);
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
        ASSERT_TRUE(Module);
        bool Found = false;
        for (auto &Block : *Module->getFunction(Function.Name))
          for (auto &Operation : Block)
            if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Operation)) {
              auto *Asm =
                  llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
              if (!Asm)
                continue;
              ASSERT_TRUE(
                  validateAArch64AddressMemoryCall(Arch::AArch64, *Call));
              const auto *Word = llvm::cast<llvm::ConstantInt>(
                  Call->getArgOperand(kAddressMemoryWordOperand));
              EXPECT_EQ(Word->getZExtValue(), At->Scratch);
              EXPECT_NE(Asm->getAsmString().find(kStackSaveSP),
                        std::string::npos);
              EXPECT_NE(Asm->getAsmString().find(kStackSelectSP),
                        std::string::npos);
              EXPECT_NE(Asm->getAsmString().find(kStackRestoreSP),
                        std::string::npos);
              Call->setArgOperand(
                  kAddressMemoryWordOperand,
                  llvm::ConstantInt::get(Word->getType(),
                                         (At->Scratch & ~kStackBaseMask) |
                                             kStackGeneralBaseBits));
              EXPECT_THROW(
                  validateAArch64AddressMemoryCall(Arch::AArch64, *Call),
                  std::invalid_argument);
              Call->setArgOperand(kAddressMemoryWordOperand,
                                  const_cast<llvm::ConstantInt *>(Word));
              auto Constraints = Asm->getConstraintString().str();
              const auto Clobber = Constraints.find(kStackClobberSP);
              ASSERT_NE(Clobber, std::string::npos);
              Constraints.erase(
                  Clobber, std::char_traits<char>::length(kStackClobberSP) + 1);
              Call->setCalledOperand(
                  llvm::InlineAsm::get(Asm->getFunctionType(),
                                       Asm->getAsmString(), Constraints, true));
              EXPECT_THROW(
                  validateAArch64AddressMemoryCall(Arch::AArch64, *Call),
                  std::invalid_argument);
              Found = true;
            }
        EXPECT_TRUE(Found);
      }
}
