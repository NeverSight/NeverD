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
#include "neverd/backend/c/render/AArch64/AArch64PAuthLoadC.h"
#include "neverd/backend/c/render/AArch64/AArch64WritebackMemoryC.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/SourceStackFrame.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIRPrint.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/AArch64WritebackMemory.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/AArch64/AArch64SVEState.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedDispatchSlots.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64/AArch64Lifter.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/AArch64MemoryImage.h"
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
#include "llvm/Support/Error.h"
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
#include <limits>
#include <memory>
#include <optional>
#include <set>

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/sysctl.h>
#endif
#include <stdexcept>
#include <string>
#include <utility>
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

llvm::StringRef
writebackConstraintsWithoutMemoryClobber(const llvm::InlineAsm &Asm) {
  // LLVM rejects missing result constraints before a call can reach NeverD.
  // Keep that negative check, then use legal LLVM constraints which violate
  // the owned memory contract to exercise both NeverD admission boundaries.
  EXPECT_TRUE(llvm::errorToBool(
      llvm::InlineAsm::verify(Asm.getFunctionType(), llvm::StringRef())));
  auto Constraints = Asm.getConstraintString();
  EXPECT_TRUE(Constraints.consume_back(",~{memory}"));
  EXPECT_FALSE(llvm::errorToBool(
      llvm::InlineAsm::verify(Asm.getFunctionType(), Constraints)));
  return Constraints;
}

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

TEST(AArch64AddressMemory, PointerParametersRetainOriginalAddressRoles) {
  for (const auto [Word, IndexIsPointer] :
       {std::pair{0xf8606900U, true}, std::pair{0xf8206900U, true},
        std::pair{0xf8607900U, false}, std::pair{0xf8604900U, false}}) {
    SCOPED_TRACE(Word);
    auto Function = memoryFunction(Word);
    ASSERT_TRUE(aarch64AddressMemoryMedShapeIsValid(
        Function.Blocks.front().Ops.front()));
    const std::set<int> ExpectedParams =
        IndexIsPointer ? std::set<int>{0, 1} : std::set<int>{0};
    EXPECT_EQ(detectPtrParamIds(Function), ExpectedParams);
    auto Typed = Function;
    inferMedTypes(Typed, Arch::AArch64);
    ASSERT_EQ(Typed.TypedParams.size(), Function.Params.size());
    for (size_t I = 0; I != Typed.TypedParams.size(); ++I)
      EXPECT_EQ(Typed.TypedParams[I].Type->Kind == NdTypeKind::Ptr,
                ExpectedParams.count(I) != 0);
    auto &Load = Function.Blocks.front().Ops.front();
    Load.Inputs[1].Kind = Load.Inputs[2].Kind = MedVar::Reg;
    const std::set<uint64_t> ExpectedRegs =
        IndexIsPointer ? std::set<uint64_t>{a64reg::X0, a64reg::X1}
                       : std::set<uint64_t>{a64reg::X0};
    EXPECT_EQ(detectPtrParamRegs(Function), ExpectedRegs);
  }
}

TEST(AArch64AddressMemory,
     MalformedOriginalAccessCannotInferPointerParameters) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Function = memoryFunction(0xf8606900U);
    auto &Load = Function.Blocks.front().Ops.front();
    switch (Mutation) {
    case 0:
      Load.Inputs[1].TheArch = Arch::X64;
      break;
    case 1:
      Load.Inputs[2].Size = 4;
      break;
    case 2:
      Load.Inputs[3].Size = 8;
      break;
    case 3:
      --Load.NumInputs;
      break;
    }
    EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Load));
    EXPECT_TRUE(detectPtrParamIds(Function).empty());
    auto Typed = Function;
    inferMedTypes(Typed, Arch::AArch64);
    for (const auto &Param : Typed.TypedParams)
      EXPECT_NE(Param.Type->Kind, NdTypeKind::Ptr);
    Load.Inputs[1].Kind = Load.Inputs[2].Kind = MedVar::Reg;
    EXPECT_TRUE(detectPtrParamRegs(Function).empty());
  }
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
    llvm::StringRef ObjectPath = {}, bool Strict = false) {
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
    if (Strict)
      Args.append({kStrictWall, kStrictExtra, kStrictError,
                   kStrictUnusedParameter, kStrictAliasing});
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

// The native drivers pass machine words. Source type recovery may expose an
// address as a pointer; adapt only those call arguments after the unchanged
// emitted bodies. Each argument is evaluated once, and the original-word
// oracle, complete memory comparisons and compiler diagnostics stay intact.
void nativePointerArguments(llvm::raw_ostream &Out, llvm::StringRef Name,
                            llvm::ArrayRef<bool> Pointers) {
  if (llvm::none_of(Pointers, [](bool Pointer) { return Pointer; }))
    return;
  Out << "\n#define " << Name << '(';
  for (size_t I = 0; I < Pointers.size(); ++I)
    Out << (I ? ", " : "") << "a" << I;
  Out << ") " << Name << '(';
  for (size_t I = 0; I < Pointers.size(); ++I) {
    Out << (I ? ", " : "");
    if (Pointers[I])
      Out << "(void *)(uintptr_t)";
    Out << "(a" << I << ')';
  }
  Out << ")\n";
}

void nativePointerArguments(llvm::raw_ostream &Out,
                            llvm::ArrayRef<HighFunc> Functions) {
  for (const auto &Function : Functions) {
    llvm::SmallVector<bool, 4> Pointers;
    for (const auto &Parameter : Function.Params)
      Pointers.push_back(Parameter.Type &&
                         Parameter.Type->Kind == NdTypeKind::Ptr);
    nativePointerArguments(Out, Function.Name, Pointers);
  }
}

void nativePointerArguments(llvm::raw_ostream &Out,
                            const llvm::Module &Module) {
  for (const auto &Function : Module) {
    if (Function.isDeclaration())
      continue;
    llvm::SmallVector<bool, 4> Pointers;
    for (const auto &Parameter : Function.args())
      Pointers.push_back(Parameter.getType()->isPointerTy());
    nativePointerArguments(Out, Function.getName(), Pointers);
  }
}

std::string memorySource(const MedFunc &Function, llvm::Module &Module,
                         bool LLVMRoute, const BinaryImage *Image = nullptr,
                         bool UnsignedParameters = false) {
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  Options.Image = Image;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  if (LLVMRoute) {
    EXPECT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
    nativePointerArguments(Out, Module);
  } else {
    auto High = MedToHighConverter().convert(Function, Arch::AArch64);
    if (UnsignedParameters)
      for (auto &Parameter : High.Params)
        Parameter.Type = NdType::makeInt(kScalarBytes, false);
    EXPECT_TRUE(HighCEmitter().emit({High}, Out, Options));
    nativePointerArguments(Out, {High});
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
      const auto Memory = originalMemoryOperation(Ops);
      EXPECT_EQ(Memory.Inputs[kAddressMemoryWordOperand + 1].Offset,
                kImmediate);
      EXPECT_TRUE(Memory.Inputs[kAddressMemoryIndexOperand + 1].isConst());
      EXPECT_EQ(Memory.Inputs[kAddressMemoryIndexOperand + 1].Offset, 0);
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
    EXPECT_FALSE(endsCallArgumentSetup(Function.Blocks.front().Ops.front(),
                                       /*RegistersOnly=*/true));
    EXPECT_TRUE(verifyMedFunc(Function, kName));
  }
}

// Each original encoding keeps the ordinary per-test timeout. Separate cases
// also let CTest schedule the native compiler executions independently.
class AArch64ScalarMemorySource
    : public ::testing::TestWithParam<MemoryFamilyCase> {};

TEST_P(AArch64ScalarMemorySource, BothCProjectionsMatchNativeWords) {
  const auto &Case = GetParam();
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
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kObjectExtension,
                                                  ObjectPath));
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
  const auto DataType =
      llvm::formatv(kNativeObjectDataParameter,
                    llvm::formatv(kNativeUnsignedType, Case.Access * CHAR_BIT))
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
                        Case.Load ? kEmptyNativeArgument : kStoreNativeArgument,
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

INSTANTIATE_TEST_SUITE_P(
    ScalarFamilies, AArch64ScalarMemorySource, ::testing::ValuesIn(FamilyCases),
    [](const ::testing::TestParamInfo<MemoryFamilyCase> &Info) {
      return "Word" + std::to_string(Info.param.Word);
    });

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
      std::string Source, Adapters;
      llvm::raw_string_ostream Out(Source), AdapterOut(Adapters);
      if (LLVMRoute) {
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
        nativePointerArguments(AdapterOut, *Module);
      } else {
        std::vector<HighFunc> High;
        for (const auto &Function : Functions)
          High.push_back(MedToHighConverter().convert(Function, Arch::AArch64));
        ASSERT_TRUE(HighCEmitter().emit(High, Out, Options));
        nativePointerArguments(AdapterOut, High);
      }
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source + Adapters + Harness, true);
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
    std::string Source, Adapters;
    llvm::raw_string_ostream Out(Source), AdapterOut(Adapters);
    if (LLVMRoute) {
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      nativePointerArguments(AdapterOut, *Module);
    } else {
      std::vector<HighFunc> High;
      for (const auto &Function : Functions)
        High.push_back(MedToHighConverter().convert(Function, Arch::AArch64));
      ASSERT_TRUE(HighCEmitter().emit(High, Out, Options));
      nativePointerArguments(AdapterOut, High);
    }
#if defined(__APPLE__) && defined(__aarch64__)
    compileMemoryC(Source + Adapters + Harness, true);
#else
    compileMemoryC(Source + Adapters + Harness, false);
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
  for (bool Fast : {false, true}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    const std::pair<uint32_t, bool> Subtypes[] = {
        {llvm::MachO::CPU_SUBTYPE_ARM64_ALL, false},
        {macho_loader::arm64e::kCPUSubtype, false},
        {macho_loader::arm64e::kCPUSubtypeX1, true},
        {macho_loader::arm64e::kCPUSubtypeX1 | kImmediateSubtypeCapabilities,
         true},
        {macho_loader::arm64e::kCPUSubtype | kImmediateSubtypeCapabilities,
         false},
        {llvm::MachO::CPU_SUBTYPE_ARM64_ALL, false},
        {macho_loader::arm64e::kCPUSubtypeX1, true},
    };
    for (const auto &[Subtype, Preserve] : Subtypes) {
      Image.MachOCPUSubtype = Subtype;
      ASSERT_TRUE(Dec.selectMode(Image, kEntry));
      std::array<uint8_t, kWordBytes> Bytes;
      writeLE<uint32_t>(Bytes.data(), ImmediateMemoryCases[0].Word);
      DecodedInsn Instruction;
      ASSERT_EQ(
          Fast ? Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry,
                                      Instruction)
               : Dec.decodeOne(Bytes.data(), Bytes.size(), kEntry, Instruction),
          kWordBytes);
      std::vector<LowOp> Operations;
      Dec.liftToLow(Instruction, Operations);
      EXPECT_EQ(std::any_of(Operations.begin(), Operations.end(),
                            lowAArch64AddressMemoryShapeIsValid),
                Preserve);
    }
  }
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
    for (bool Fast : {false, true})
      for (bool Strict : {false, true}) {
        const auto Operations = liftMemory(Case.Word, Fast, Strict);
        const auto Memory = originalMemoryOperation(Operations);
        ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
        EXPECT_EQ(Memory.Inputs[kAddressMemoryWordOperand + 1].Offset,
                  Case.Word);
        EXPECT_EQ(Memory.Inputs[kAddressMemoryBaseOperand + 1].Offset,
                  a64reg::X0 + Instruction->base() * kScalarBytes);
        EXPECT_TRUE(Memory.Inputs[kAddressMemoryIndexOperand + 1].isConst());
        EXPECT_EQ(Memory.Inputs[kAddressMemoryIndexOperand + 1].Offset, 0);
        EXPECT_EQ(Memory.Inputs[kAddressMemoryIndexOperand + 1].Size,
                  kScalarBytes);
        EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Memory));
        EXPECT_FALSE(lowIntrinsicMemoryOperands(Memory).Complete);
      }
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
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Segment Data;
  Data.VA = kStorageBase;
  Data.Size = Data.FileSz = kImmediateMemoryBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));
  for (const auto &Case : ImmediateMemoryCases) {
    const auto Instruction = *aarch64AddressMemoryInstruction(Case.Word);
    const auto Range = Instruction.controlIndependentSpan(kStorageBase, 0);
    ASSERT_TRUE(Range);
    const uint64_t Start = kStorageBase + static_cast<uint64_t>(Case.Offset);
    EXPECT_LE(Range->Begin, Start);
    EXPECT_LE(Range->Begin, kStorageBase);
    EXPECT_GE(Range->Begin + Range->Bytes, Start + Case.Access);
    EXPECT_GE(Range->Begin + Range->Bytes, kStorageBase);
    const auto Base = kStorageBase + kImmediateBasePosition;
    EXPECT_TRUE(aarch64ImmediateMemoryImageSpan(
        Image, Instruction, Base, ConstantAddressProvenance::DataAddress,
        kStorageBase));
    for (auto Role :
         {ConstantAddressProvenance::Scalar, ConstantAddressProvenance::Unknown,
          ConstantAddressProvenance::AddressFragment,
          ConstantAddressProvenance::CodeAddress})
      EXPECT_FALSE(aarch64ImmediateMemoryImageSpan(Image, Instruction, Base,
                                                   Role, kStorageBase));
    EXPECT_FALSE(aarch64ImmediateMemoryImageSpan(
        Image, Instruction, Base, ConstantAddressProvenance::DataAddress,
        Base));
    auto Truncated = Image;
    const auto Span = Instruction.controlIndependentSpan(Base, 0);
    ASSERT_TRUE(Span);
    Truncated.Segments.front().Data.resize(Span->Begin - kStorageBase +
                                           Span->Bytes - 1);
    EXPECT_FALSE(aarch64ImmediateMemoryImageSpan(
        Truncated, Instruction, Base, ConstantAddressProvenance::DataAddress,
        kStorageBase));
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
  AArch64Lifter Lifter(Arch::AArch64);
  Decoder FastDecoder;
  ASSERT_TRUE(FastDecoder.init(Arch::AArch64));
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
        if (!Scratch) {
          Identity = Decoded->id;
          std::vector<LowOp> FullOps;
          Lifter.lift(Decoded, FullOps);
          const auto Full = originalMemoryOperation(FullOps);
          EXPECT_TRUE(lowAArch64AddressMemoryShapeIsValid(Full));
          EXPECT_EQ(Full.Inputs[kAddressMemoryWordOperand + 1].Offset, Word);
          DecodedInsn Fast;
          ASSERT_EQ(FastDecoder.decodeOneForLift(Bytes.data(), Bytes.size(),
                                                 kEntry, Fast),
                    kWordBytes);
          std::vector<LowOp> FastOps;
          FastDecoder.liftToLow(Fast, FastOps);
          ASSERT_EQ(FastOps.size(), FullOps.size());
          for (size_t Op = 0; Op != FullOps.size(); ++Op) {
            EXPECT_EQ(FullOps[Op].Opcode, FastOps[Op].Opcode);
            EXPECT_EQ(FullOps[Op].Output, FastOps[Op].Output);
            ASSERT_EQ(FullOps[Op].NumInputs, FastOps[Op].NumInputs);
            for (unsigned Input = 0; Input != FullOps[Op].NumInputs; ++Input)
              EXPECT_EQ(FullOps[Op].Inputs[Input], FastOps[Op].Inputs[Input]);
          }
        } else {
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
void executeDecodedImmediateImageBases() {
  for (uint32_t Subtype :
       {macho_loader::arm64e::kCPUSubtype, macho_loader::arm64e::kCPUSubtypeX1})
    for (bool Runtime : {false, true})
      for (bool Add : {false, true})
        for (bool Writable : {false, true}) {
          if (Runtime && (Add || Writable))
            continue;
          BinaryImage Image;
          Image.Arch = Arch::AArch64;
          Image.Bits = Bitness::Bits64;
          Image.Format = BinaryFormat::MachO;
          Image.MachOCPUSubtype = Subtype;
          Image.Base = Image.Entry = kEntry;
          std::vector<uint32_t> Words;
          if (!Runtime) {
            Words.push_back(kImmediateDecodedADRP);
            if (Add)
              Words.push_back(kImmediateDecodedADD);
          }
          const auto LoadWord =
              Runtime ? kImmediateDecodedRuntimeLoad : kImmediateDecodedLoad;
          Words.push_back(LoadWord);
          Words.push_back(kReturn);
          Segment Text;
          Text.VA = kEntry;
          Text.Size = Text.FileSz = Words.size() * kWordBytes;
          Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
          Text.Data.resize(Text.Size);
          for (size_t I = 0; I != Words.size(); ++I)
            writeLE<uint32_t>(Text.Data.data() + I * kWordBytes, Words[I]);
          Image.Segments.push_back(std::move(Text));
          Segment Data;
          Data.VA = kImmediateDecodedDataPage;
          Data.Size = Data.FileSz = kImmediateDecodedStorageBytes;
          Data.Flags = SegmentFlags::Readable;
          if (Writable)
            Data.Flags = Data.Flags | SegmentFlags::Writable;
          Data.Data.resize(Data.Size);
          writeLE<uint64_t>(Data.Data.data() + kImmediateDecodedLoadOffset +
                                (Add ? kImmediateDecodedAddOffset : 0),
                            kVectorLoadedLow);
          Image.Segments.push_back(std::move(Data));
          Decoder Dec;
          ASSERT_TRUE(Dec.init(Image));
          const auto Low = CFGBuilder().build(Image, Dec, kEntry, kName);
          LowToMedConverter Converter;
          Converter.setBinaryImage(&Image);
          const auto Function =
              Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
          ASSERT_TRUE(verifyMedFunc(Function, kName));
          ASSERT_EQ(Function.Params.size(), Runtime ? 1U : 0U);
          unsigned Reads = 0;
          for (const auto &Block : Function.Blocks)
            for (const auto &Op : Block.Ops)
              if (aarch64AddressMemoryMedShapeIsValid(Op)) {
                ++Reads;
                EXPECT_EQ(Op.Inputs[kAddressMemoryWordOperand + 1].ConstVal,
                          LoadWord);
              }
          ASSERT_EQ(Reads,
                    Subtype == macho_loader::arm64e::kCPUSubtypeX1 ? 1U : 0U);
          llvm::LLVMContext Context;
          std::unique_ptr<llvm::Module> Module;
          ASSERT_NO_THROW(Module =
                              MedLLVMEmitter().emit({Function}, Context, kName,
                                                    Arch::AArch64, {}, &Image))
              << Subtype << Runtime << Add << Writable;
          ASSERT_TRUE(Module);
          if (Runtime)
            EXPECT_TRUE(Module->global_empty());
          optimizeMemory(*Module);
          ASSERT_FALSE(llvm::verifyModule(*Module));
          const auto Harness =
              llvm::formatv(Runtime ? kImmediateDecodedRuntimeHarness
                                    : kImmediateDecodedHarness,
                            kName, kVectorLoadedLow)
                  .str();
          for (bool LLVMRoute : {false, true}) {
            const auto Source =
                memorySource(Function, *Module, LLVMRoute, &Image);
#if defined(__APPLE__) && defined(__aarch64__)
            compileMemoryC(Source + Harness, true);
#else
            compileMemoryC(Source, false);
#endif
          }
          // Analysis/source projections do not authorize rebuilding an arm64e
          // image without its authenticated ABI and pointer fixups.
          EXPECT_FALSE(Codegen()
                           .compile(*Module, Arch::AArch64, BinaryFormat::MachO)
                           .Success);
        }
}

struct ImmediateMemoryExecution {
  bool ExecuteNative = true;
  const char *ArtifactProperty = nullptr;
  bool AlignAccesses = false;
};

void executeImmediateMemoryFamilies(llvm::ArrayRef<ImmediateMemoryCase> Cases,
                                    bool Stack, bool Immediate,
                                    const BinaryImage *Image = nullptr,
                                    bool Relocate = false,
                                    ImmediateMemoryExecution Execution = {}) {
  llvm::SmallString<kVectorPathStorage> ArtifactDirectory;
  if (const auto *Root =
          std::getenv(Stack || !Immediate ? kStackArtifactEnvironment
                                          : kImmediateArtifactEnvironment);
      Root) {
    ASSERT_FALSE(llvm::sys::fs::create_directories(Root));
    llvm::SmallString<kVectorPathStorage> Prefix(Root);
    llvm::sys::path::append(
        Prefix,
        Relocate ? kImmediateRelocatedArtifactPrefix
        : Image  ? kImmediateImageArtifactPrefix
        : Stack
            ? (Immediate ? kStackArtifactPrefix : kStackRegisterArtifactPrefix)
            : (Immediate ? kImmediateArtifactPrefix : kRegisterArtifactPrefix));
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory(Prefix, ArtifactDirectory));
    ::testing::Test::RecordProperty(
        Execution.ArtifactProperty ? Execution.ArtifactProperty
        : Relocate                 ? kImmediateRelocatedArtifactProperty
        : Image                    ? kImmediateImageArtifactProperty
        : Stack ? (Immediate ? kStackArtifactProperty
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
  const auto BasePosition = [&](const ImmediateMemoryCase &Case) {
    return kImmediateBasePosition +
           (Execution.AlignAccesses
                ? (-uint64_t(Case.Offset) & (Case.Access - 1))
                : 0);
  };
  for (size_t I = 0; I != Cases.size(); ++I) {
    const auto &Case = Cases[I];
    auto Function = immediateMemoryFunction(Case, I);
    if (Relocate) {
      auto &Ops = Function.Blocks.front().Ops;
      const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                       aarch64AddressMemoryMedShapeIsValid);
      ASSERT_NE(Memory, Ops.end());
      const auto Address = MedVar::makeConst(
          Image->Segments.front().VA + BasePosition(Case), kScalarBytes,
          ConstantAddressProvenance::DataAddress, Image->Segments.front().VA);
      Memory->Inputs[kAddressMemoryBaseOperand + 1] = Address;
      if (I % 2) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 0;
        Copy.Output.TheArch = Arch::AArch64;
        Copy.Output.Size = kScalarBytes;
        for (const auto &Op : Ops)
          Copy.Output.Id = std::max(Copy.Output.Id, Op.Output.Id + 1);
        Copy.addInput(Address);
        Memory->Inputs[kAddressMemoryBaseOperand + 1] = Copy.Output;
        Ops.insert(Memory, Copy);
      }
    }
    const bool RuntimeMath = Image && !Relocate && I % 2;
    if (RuntimeMath) {
      auto &Ops = Function.Blocks.front().Ops;
      const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                       aarch64AddressMemoryMedShapeIsValid);
      ASSERT_NE(Memory, Ops.end());
      MedOp Sum;
      Sum.Opcode = NdOp::INT_ADD;
      Sum.Output.Kind = MedVar::Temp;
      Sum.Output.Id = 0;
      Sum.Output.TheArch = Arch::AArch64;
      Sum.Output.Size = kScalarBytes;
      for (const auto &Op : Ops)
        Sum.Output.Id = std::max(Sum.Output.Id, Op.Output.Id + 1);
      Sum.addInput(Function.Params[0]);
      Sum.addInput(MedVar::makeConst(kImmediateRuntimeDelta, kScalarBytes,
                                     ConstantAddressProvenance::Scalar));
      Memory->Inputs[kAddressMemoryBaseOperand + 1] = Sum.Output;
      Ops.insert(Memory, Sum);
    }
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
                      unsigned(Case.Signed), unsigned(Case.Vector),
                      Case.Offset + (RuntimeMath ? kImmediateRuntimeDelta : 0))
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
  if (Relocate) {
    const auto Name = kImmediateCasePrefix;
    size_t At = 0;
    while ((At = Wrappers.find(Name, At)) != std::string::npos) {
      Wrappers.replace(At, std::char_traits<char>::length(Name),
                       kImmediateInnerPrefix);
      At += std::char_traits<char>::length(kImmediateInnerPrefix);
    }
    for (size_t I = 0; I != Cases.size(); ++I)
      Wrappers += llvm::formatv(kImmediateRelocatedWrapper, I,
                                BasePosition(Cases[I]), kImmediateMemoryBytes)
                      .str();
    MedFunc Base;
    Base.Name = kImmediateImageBaseName;
    Base.Entry = kEntry + Cases.size() * kVectorFunctionStride;
    Base.ReturnType = NdType::makeInt(kScalarBytes, false);
    Base.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
    Base.Blocks.emplace_back();
    Base.Blocks.front().Id = 0;
    Base.Blocks.front().StartAddr = Base.Entry;
    MedOp Address;
    Address.Opcode = NdOp::COPY;
    Address.Output.Kind = MedVar::Reg;
    Address.Output.TheArch = Arch::AArch64;
    Address.Output.RegOff = a64reg::X0;
    Address.Output.Id = 0;
    Address.Output.SSAVer = 1;
    Address.Output.Size = kScalarBytes;
    Address.addInput(MedVar::makeConst(Image->Segments.front().VA, kScalarBytes,
                                       ConstantAddressProvenance::DataAddress,
                                       Image->Segments.front().VA));
    Base.Blocks.front().Ops.push_back(Address);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(Address.Output);
    Base.Blocks.front().Ops.push_back(Return);
    Functions.push_back(std::move(Base));
    Declarations += kImmediateImageBaseDeclaration;
  }
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(Functions, Context, kName, Arch::AArch64,
                                      {}, Image);
  ASSERT_TRUE(Module);
  if (Image)
    EXPECT_EQ(Module->global_empty(), !Relocate);
  optimizeMemory(*Module);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  const auto Harness =
      (Relocate ? std::string(kImmediateImageBaseDeclaration) : std::string{}) +
      llvm::formatv(kImmediateNativeHarness, Wrappers, Entries,
                    Execution.AlignAccesses
                        ? llvm::formatv(kImmediateAlignedBasePositionExpression,
                                        kImmediateBasePosition)
                              .str()
                        : llvm::utostr(kImmediateBasePosition))
          .str();
  SaveArtifact(kImmediateHarnessArtifact, Declarations + Harness);
  for (bool LLVMRoute : {false, true}) {
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    Options.Image = Image;
    std::string Source, Adapters;
    llvm::raw_string_ostream Out(Source), AdapterOut(Adapters);
    if (LLVMRoute) {
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      nativePointerArguments(AdapterOut, *Module);
    } else {
      std::vector<HighFunc> High;
      for (const auto &Function : Functions)
        High.push_back(MedToHighConverter().convert(Function, Arch::AArch64));
      ASSERT_TRUE(HighCEmitter().emit(High, Out, Options));
      nativePointerArguments(AdapterOut, High);
    }
    SaveArtifact(LLVMRoute ? kImmediateLLVMCArtifact : kImmediateHighCArtifact,
                 Source);
#if defined(__APPLE__) && defined(__aarch64__)
    compileMemoryC(Source + Adapters + Harness, Execution.ExecuteNative);
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
  if (Execution.ExecuteNative)
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

TEST(AArch64ImmediateMemory, ImageContextPreservesEveryRuntimeParameterAccess) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Segment Data;
  Data.Name = kStorageTableName;
  Data.VA = kImmediateRelocatedDataBase;
  Data.Size = Data.FileSz = kImmediateMemoryBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));
  executeImmediateMemoryFamilies(ImmediateMemoryCases, false, true, &Image,
                                 true);
  executeDecodedImmediateImageBases();
  // Zero is the required dummy index, and the original word is metadata.
  // Neither becomes an image pointer merely by matching a mapped address.
  const auto &Case = ImmediateMemoryCases[0];
  for (va_t Address : {va_t(0), va_t(Case.Word)}) {
    Image.Segments.front().VA = Address;
    const auto Function = immediateMemoryFunction(Case, 0);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Function}, Context, kName,
                                        Arch::AArch64, {}, &Image);
    ASSERT_TRUE(Module);
    EXPECT_TRUE(Module->global_empty());
    for (bool LLVMRoute : {false, true})
      compileMemoryC(memorySource(Function, *Module, LLVMRoute, &Image), false,
                     [&](llvm::MemoryBufferRef Buffer) {
                       inspectMemoryObject(Buffer, Case.Scratch);
                     });
  }
  Image.Segments.front().VA = kStorageBase;
  executeImmediateMemoryFamilies(ImmediateMemoryCases, false, true, &Image);
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
  {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    Image.MachOCPUSubtype = macho_loader::arm64e::kCPUSubtypeX1;
    Segment Text;
    const uint32_t Words[] = {kImmediateStackAliasMove,
                              kImmediateStackAliasRead, kReturn};
    Text.VA = kEntry;
    Text.Size = Text.FileSz = std::size(Words) * kWordBytes;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    for (size_t I = 0; I != std::size(Words); ++I)
      writeLE<uint32_t>(Text.Data.data() + I * kWordBytes, Words[I]);
    Image.Segments.push_back(std::move(Text));
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    const auto Low = CFGBuilder().build(Image, Dec, kEntry, kName);
    EXPECT_NO_THROW(
        LowToMedConverter().convert(Low, Arch::AArch64, BinaryFormat::MachO));
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    EXPECT_THROW(Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO),
                 LowToMedConversionError);
  }
  for (bool Before : {false, true}) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    Segment Data;
    Data.VA = kStorageBase;
    Data.Size = Data.FileSz = kImmediateMemoryBytes;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(Data.Size);
    Image.Segments.push_back(std::move(Data));
    auto Function = immediateMemoryFunction(ImmediateMemoryCases[0], 0);
    MedVar Temporary;
    Temporary.Kind = MedVar::Temp;
    Temporary.TheArch = Arch::AArch64;
    Temporary.Size = kScalarBytes;
    Temporary.Id = 0;
    for (const auto &Op : Function.Blocks.front().Ops)
      Temporary.Id = std::max(Temporary.Id, Op.Output.Id + 1);
    auto High = MedToHighConverter().convert(Function, Arch::AArch64);
    std::function<void(const ExprPtr &)> Replace;
    Replace = [&](const ExprPtr &Expression) {
      if (!Expression)
        return;
      if (isAArch64AddressMemoryIntrinsic(Expression->IntrinsicId))
        Expression->Operands[kAddressMemoryBaseOperand] =
            HighExpr::makeVar(Temporary, NdType::makeInt(kScalarBytes, false));
      Expression->forEachChildExpr(Replace);
    };
    walkStmts(High.Body, [&](const HighStmt &Statement) {
      forEachExpr(Statement, Replace);
    });
    HighStmt Definition;
    Definition.Kind = StmtKind::Assign;
    Definition.Dst =
        HighExpr::makeVar(Temporary, NdType::makeInt(kScalarBytes, false));
    Definition.Val = HighExpr::makeConst(
        kStorageBase + kImmediateBasePosition, kScalarBytes,
        ConstantAddressProvenance::DataAddress, kStorageBase);
    if (Before)
      High.Body.insert(High.Body.begin(), Definition);
    else
      High.Body.push_back(Definition);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Image = &Image;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (Before)
      EXPECT_NO_THROW(EXPECT_TRUE(HighCEmitter().emit({High}, Out, Options)));
    else {
      EXPECT_THROW(HighCEmitter().emit({High}, Out, Options),
                   std::invalid_argument);
      EXPECT_TRUE(Source.empty());
    }
  }
  for (bool Select : {false, true})
    for (bool Hidden : {false, true}) {
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::MachO;
      Segment Data;
      Data.VA = kStorageBase;
      Data.Size = Data.FileSz = kImmediateMemoryBytes;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(Data.Size);
      Image.Segments.push_back(std::move(Data));
      auto Function = immediateMemoryFunction(ImmediateMemoryCases[0], 0);
      Function.Params[1].Size = 1;
      auto &Ops = Function.Blocks.front().Ops;
      const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                       aarch64AddressMemoryMedShapeIsValid);
      ASSERT_NE(Memory, Ops.end());
      MedOp Base;
      Base.Opcode = Select ? NdOp::SELECT : NdOp::INT_ADD;
      Base.Output.Kind = MedVar::Temp;
      Base.Output.Id = 0;
      Base.Output.TheArch = Arch::AArch64;
      Base.Output.Size = kScalarBytes;
      for (const auto &Op : Ops)
        if (Op.Output.Kind == MedVar::Temp)
          Base.Output.Id = std::max(Base.Output.Id, Op.Output.Id + 1);
      if (Select)
        Base.addInput(Function.Params[1]);
      Base.addInput(MedVar::makeConst(
          kStorageBase + kImmediateBasePosition, kScalarBytes,
          ConstantAddressProvenance::DataAddress, kStorageBase));
      Base.addInput(Function.Params[0]);
      Memory->Inputs[kAddressMemoryBaseOperand + 1] = Base.Output;
      Ops.insert(Memory, Base);
      ASSERT_TRUE(verifyMedFunc(Function, kName));
      const std::vector<char> Mask{0};
      llvm::LLVMContext Context;
      EXPECT_THROW(MedLLVMEmitter().emit(
                       {Function}, Context, kName, Arch::AArch64, {}, &Image,
                       BinaryFormat::MachO, false, Hidden ? &Mask : nullptr),
                   std::runtime_error);
      auto High = MedToHighConverter().convert(Function, Arch::AArch64);
      if (Hidden) {
        HighStmt Branch;
        Branch.Kind = StmtKind::If;
        Branch.Cond = HighExpr::makeConst(0, 1);
        Branch.Body = std::move(High.Body);
        High.Body = {std::move(Branch)};
      }
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Image = &Image;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      EXPECT_THROW(HighCEmitter().emit({High}, Out, Options),
                   std::invalid_argument);
      EXPECT_TRUE(Source.empty());
    }
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
      for (auto Kind :
           {MedVar::Const, MedVar::Reg, MedVar::Temp, MedVar::Stack})
        for (bool Hidden : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(Kind));
          SCOPED_TRACE(Hidden);
          auto Rejected = Function;
          auto High = MedToHighConverter().convert(Function, Arch::AArch64);
          MedVar RejectedBase;
          for (auto &Op : Rejected.Blocks.front().Ops)
            if (aarch64AddressMemoryMedShapeIsValid(Op)) {
              auto &Base = Op.Inputs[kAddressMemoryBaseOperand + 1];
              Base.Kind = Kind;
              if (Kind != MedVar::Const) {
                Base.Id = 0;
                for (const auto &Definition : Rejected.Blocks.front().Ops)
                  Base.Id = std::max(Base.Id, Definition.Output.Id + 1);
              }
              Base.ConstVal = kStorageBase;
              RejectedBase = Base;
            }
          EXPECT_THROW(MedLLVMEmitter().emit({Rejected}, Context, kName,
                                             Arch::AArch64, {}, &Image,
                                             BinaryFormat::MachO, false,
                                             Hidden ? &Mask : nullptr),
                       std::runtime_error);
          std::function<void(const ExprPtr &)> RejectBase;
          RejectBase = [&](const ExprPtr &Expression) {
            if (!Expression)
              return;
            if (isAArch64AddressMemoryIntrinsic(Expression->IntrinsicId))
              Expression->Operands[kAddressMemoryBaseOperand] =
                  Kind == MedVar::Const
                      ? HighExpr::makeConst(kStorageBase, kScalarBytes)
                      : HighExpr::makeVar(RejectedBase,
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
          Options.Image = &Image;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          EXPECT_THROW(HighCEmitter().emit({High}, Out, Options),
                       std::invalid_argument);
          EXPECT_TRUE(Source.empty());
        }
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

namespace {
#define WRITEBACK_VALUE(Type, Name, Value) inline constexpr Type Name = Value;
#define WRITEBACK_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64WritebackMemoryFixtures.def"
struct WritebackCase {
  uint32_t Word;
  uint32_t Scratch;
  unsigned Access;
  unsigned Result;
  int64_t Offset;
  bool Pre;
};
constexpr WritebackCase WritebackCases[] = {
#define WRITEBACK_CASE(Word, Scratch, Access, Result, Offset, Pre)             \
  {Word, Scratch, Access, Result, Offset, Pre},
#include "AArch64WritebackMemoryFixtures.def"
};

MedFunc writebackFunction(const WritebackCase &Case, size_t Index) {
  MedFunc Function;
  Function.Name = kWritebackNamePrefix + llvm::utohexstr(Case.Word);
  Function.Entry = kEntry + Index * kVectorFunctionStride;
  Function.ReturnType =
      NdType::makeInt(Case.Result ? kWritebackPairBytes : kScalarBytes, false);
  Function.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  for (unsigned I = 0; I != (Case.Result ? 1U : 2U); ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Arch::AArch64;
    Parameter.Id = I;
    Parameter.RegOff = a64reg::X0 + I * kScalarBytes;
    Parameter.Size = kScalarBytes;
    Function.Params.push_back(Parameter);
    Function.TypedParams.push_back({{}, NdType::makeInt(kScalarBytes, false)});
  }
  MedOp Access;
  Access.Opcode = NdOp::INTRINSIC;
  Access.Output.Kind = MedVar::Reg;
  Access.Output.RegOff = a64reg::X0;
  Access.Output.TheArch = Arch::AArch64;
  Access.Output.Id = 0;
  Access.Output.SSAVer = 1;
  Access.Output.Size = Function.ReturnType->Size;
  Access.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Case.Result ? Intrinsic::A64_LoadWriteback
                                        : Intrinsic::A64_StoreWriteback),
      kIdBytes));
  Access.addInput(Function.Params[0]);
  Access.addInput(MedVar::makeConst(Case.Word, kWordBytes));
  if (!Case.Result)
    Access.addInput(Function.Params[1]);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Access.Output);
  Function.Blocks.emplace_back();
  Function.Blocks.front().Id = 0;
  Function.Blocks.front().StartAddr = Function.Entry;
  Function.Blocks.front().Ops = {Access, Return};
  return Function;
}

void executeWritebackFamilies(const BinaryImage *Image, bool Stack = false) {
  std::vector<MedFunc> Functions;
  std::string Declarations, Replays, Entries, Faults, FaultEntries;
  for (size_t I = 0; I != std::size(WritebackCases); ++I) {
    auto Case = WritebackCases[I];
    if (Stack) {
      Case.Word = (Case.Word & ~kStackBaseMask) | kStackBaseMask;
      Case.Scratch = (Case.Scratch & ~kStackBaseMask) | kStackBaseMask;
    }
    auto Function = writebackFunction(Case, I);
    ASSERT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
    Declarations += llvm::formatv(Case.Result ? kWritebackLoadDeclaration
                                              : kWritebackStoreDeclaration,
                                  Function.Name)
                        .str();
    Replays +=
        llvm::formatv(kWritebackNativeCase, I, Case.Word,
                      llvm::formatv(Case.Result ? kWritebackNativeLoad
                                                : kWritebackNativeStore,
                                    Function.Name)
                          .str(),
                      Stack ? kStackOracleX9Before : kEmptyNativeArgument,
                      Stack ? kStackOracleX9After : kEmptyNativeArgument,
                      Stack ? kStackOracleClobbers : kEmptyNativeArgument)
            .str();
    Entries += llvm::formatv(kWritebackHarnessCase, I).str();
    if (Stack) {
      Faults += llvm::formatv(kStackFaultWrapper, I, Function.Name,
                              Case.Result ? kStackFaultLoadArguments
                                          : kStackFaultStoreArguments)
                    .str();
      FaultEntries += llvm::formatv(kStackFaultEntry, I).str();
    }
    Functions.push_back(std::move(Function));
  }
  const auto Harness =
      std::string(kWritebackHarnessPrelude) + Declarations + Replays +
      (Stack ? std::string(kStackFaultBegin) + Faults +
                   llvm::formatv(kStackFaultTable, FaultEntries).str() +
                   kStackFaultEnd
             : std::string()) +
      llvm::formatv(kWritebackHarnessTail, Entries).str();
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(Functions, Context, kName, Arch::AArch64,
                                      {}, Image);
  ASSERT_TRUE(Module);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  if (Image)
    EXPECT_TRUE(Module->global_empty());
  optimizeMemory(*Module);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  Options.Image = Image;
  std::string ArtifactDirectory;
  if (!Image)
    if (const char *Directory =
            std::getenv(Stack ? kStackWritebackArtifactEnvironment
                              : kWritebackArtifactEnvironment)) {
      ArtifactDirectory = Directory;
      ::testing::Test::RecordProperty(Stack ? kStackWritebackArtifactProperty
                                            : kWritebackArtifactProperty,
                                      ArtifactDirectory);
      ASSERT_FALSE(llvm::sys::fs::create_directories(ArtifactDirectory));
    }
  const auto Save = [&](llvm::StringRef Name, llvm::StringRef Data) {
    if (ArtifactDirectory.empty())
      return;
    llvm::SmallString<kVectorPathStorage> Path(ArtifactDirectory);
    llvm::sys::path::append(Path, Name);
    std::error_code Error;
    llvm::raw_fd_ostream Output(Path, Error);
    ASSERT_FALSE(Error);
    Output << Data;
  };
  Save(kWritebackHarnessArtifact, Harness);
  for (bool LLVMRoute : {false, true}) {
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    if (LLVMRoute)
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, Options));
    else {
      std::vector<HighFunc> High;
      for (const auto &Function : Functions) {
        auto Body = MedToHighConverter().convert(Function, Arch::AArch64);
        // The explicit-IR bridge declares full-width unsigned inputs.
        for (auto &Parameter : Body.Params)
          Parameter.Type = NdType::makeInt(kScalarBytes, false);
        High.push_back(std::move(Body));
      }
      ASSERT_TRUE(HighCEmitter().emit(High, Output, Options));
    }
    Save(LLVMRoute ? kWritebackLLVMArtifact : kWritebackHighArtifact, Source);
#if defined(__APPLE__) && defined(__aarch64__)
    compileMemoryC(Source + Harness, true);
#else
    compileMemoryC(Source, false);
#endif
  }
  const auto Object =
      Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(Object.Success);
  if (Stack)
    Save(kImmediateMachOArtifact,
         llvm::StringRef(
             reinterpret_cast<const char *>(Object.ObjectData.data()),
             Object.ObjectData.size()));
  const llvm::MemoryBufferRef Buffer(
      llvm::StringRef(reinterpret_cast<const char *>(Object.ObjectData.data()),
                      Object.ObjectData.size()),
      kObject);
  for (const auto &Case : WritebackCases)
    inspectMemoryObject(Buffer, Stack ? (Case.Scratch & ~kStackBaseMask) |
                                            kStackBaseMask
                                      : Case.Scratch);
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
  compileMemoryC(Harness, true, {}, ObjectPath);
#endif
  if (!ArtifactDirectory.empty()) {
    const auto ELF =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::ELF);
    ASSERT_TRUE(ELF.Success);
    Save(kWritebackObjectArtifact,
         llvm::StringRef(reinterpret_cast<const char *>(ELF.ObjectData.data()),
                         ELF.ObjectData.size()));
  }
}
} // namespace

TEST(AArch64WritebackMemory,
     EncodingsAndEveryImmediateAgreeWithIndependentDecoder) {
  ASSERT_EQ(std::size(WritebackCases), kWritebackCaseCount);
  for (const auto &Case : WritebackCases) {
    const auto Instruction = aarch64WritebackMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
    EXPECT_EQ(Instruction->accessBytes(), Case.Access);
    EXPECT_EQ(Instruction->operation().ResultBytes, Case.Result);
    EXPECT_EQ(Instruction->base(), kWritebackFixtureBase);
    EXPECT_EQ(Instruction->destination(), kWritebackFixtureDestination);
    EXPECT_EQ(Instruction->preIndex(), Case.Pre);
    EXPECT_EQ(static_cast<int64_t>(Instruction->displacement()), Case.Offset);
    if (Case.Offset)
      continue;
    for (unsigned Immediate = kWritebackFirstOffset;
         Immediate != kWritebackLastImmediate; ++Immediate) {
      const auto Word = (Case.Word & ~kWritebackFixtureOffsetMask) |
                        (Immediate << kWritebackFixtureOffsetShift);
      Decoder Decoder;
      ASSERT_TRUE(Decoder.init(Arch::AArch64));
      std::array<uint8_t, kWordBytes> Bytes;
      writeLE<uint32_t>(Bytes.data(), Word);
      DecodedInsn Decoded;
      ASSERT_EQ(Decoder.decodeOne(Bytes.data(), Bytes.size(), kEntry, Decoded),
                kWordBytes);
      ASSERT_TRUE(Decoded.Raw && Decoded.Raw->detail);
      const auto &Detail = Decoded.Raw->detail->aarch64;
      ASSERT_TRUE(Decoded.Raw->detail->writeback);
      EXPECT_EQ(Detail.post_index, !Case.Pre);
      ASSERT_GE(Detail.op_count, 2);
      ASSERT_EQ(Detail.operands[1].type, AARCH64_OP_MEM);
      const auto Offset = !Case.Pre && Detail.op_count >= 3 &&
                                  Detail.operands[2].type == AARCH64_OP_IMM
                              ? Detail.operands[2].imm
                              : Detail.operands[1].mem.disp;
      const auto Parsed = aarch64WritebackMemoryInstruction(Word);
      ASSERT_TRUE(Parsed);
      EXPECT_EQ(static_cast<int64_t>(Parsed->displacement()), Offset);
      for (bool Fast : {false, true}) {
        const auto Ops = liftMemory(Word, Fast);
        const auto It = std::find_if(Ops.begin(), Ops.end(),
                                     lowAArch64WritebackMemoryShapeIsValid);
        ASSERT_NE(It, Ops.end());
        EXPECT_EQ(It->Inputs[kWritebackWordOperand + 1].Offset, Word);
        EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(*It));
        EXPECT_FALSE(lowIntrinsicMemoryOperands(*It).Complete);
      }
    }
  }
  EXPECT_FALSE(aarch64WritebackMemoryInstruction(kWritebackOverlapLoad));
  EXPECT_FALSE(aarch64WritebackMemoryInstruction(kWritebackOverlapStore));
  EXPECT_TRUE(aarch64WritebackMemoryInstruction(kWritebackZeroLoad));
  EXPECT_TRUE(aarch64WritebackMemoryInstruction(kWritebackZeroStore));
  EXPECT_LT(Intrinsic::A64_VectorStoreImmediate, Intrinsic::A64_LoadWriteback);
}

TEST(AArch64WritebackMemory, AutomaticLiftingKeepsWritebackAsOneOperation) {
  for (const auto &Case : WritebackCases)
    for (bool Fast : {false, true})
      for (bool Strict : {false, true}) {
        const auto Ops = liftMemory(Case.Word, Fast, Strict);
        const auto It = std::find_if(Ops.begin(), Ops.end(),
                                     lowAArch64WritebackMemoryShapeIsValid);
        ASSERT_NE(It, Ops.end());
        EXPECT_EQ(std::count_if(Ops.begin(), Ops.end(),
                                lowAArch64WritebackMemoryShapeIsValid),
                  1);
        EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
                 Op.Opcode == NdOp::INT_ADD;
        }));
        EXPECT_EQ(Ops.back().Output.Offset,
                  a64reg::X0 + kWritebackFixtureBase * kScalarBytes);
        EXPECT_EQ(Ops.back().Inputs[0], It->Output);
      }
}

TEST(AArch64WritebackMemory,
     BothCProjectionsAndLLVMExecuteEveryWritebackFamily) {
  executeWritebackFamilies(nullptr);
}

TEST(AArch64WritebackMemory,
     ImageContextKeepsReturnedBasesInRuntimeCoordinates) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Segment Data;
  Data.VA = kStorageBase;
  Data.Size = Data.FileSz = kWritebackImageStorageBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));
  executeWritebackFamilies(&Image);
}

TEST(AArch64WritebackMemory, MalformedContractsFailBeforeSourceFiltering) {
  for (const auto &Case :
       {WritebackCases[0], WritebackCases[kWritebackFirstLoadCase]}) {
    auto Function = writebackFunction(Case, 0);
    const auto Valid = Function.Blocks.front().Ops.front();
    ASSERT_TRUE(aarch64WritebackMemoryMedShapeIsValid(Valid));
    const std::vector<std::function<void(MedOp &)>> Mutations = {
        [](MedOp &Op) { Op.Output.Size = 0; },
        [](MedOp &Op) { Op.Inputs[0].ConstVal += kWrappedId; },
        [](MedOp &Op) { Op.Inputs[1].Size = kWordBytes; },
        [](MedOp &Op) { Op.Inputs[2] = Op.Inputs[1]; },
        [](MedOp &Op) { Op.Inputs[2].Size = kScalarBytes; },
        [](MedOp &Op) { Op.Inputs[2].ConstVal = kWritebackOverlapLoad; },
        [](MedOp &Op) { Op.Output.TheArch = Arch::X64; },
        [](MedOp &Op) { Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS; },
        [](MedOp &Op) { Op.MemoryOrdering = NdMemoryOrdering::Acquire; },
    };
    for (const auto &Mutate : Mutations) {
      auto Bad = Valid;
      Mutate(Bad);
      EXPECT_FALSE(aarch64WritebackMemoryMedShapeIsValid(Bad));
      Function.Blocks.front().Ops.front() = Bad;
      llvm::LLVMContext Context;
      EXPECT_THROW(
          MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64),
          std::runtime_error);
      const std::vector<char> Bodies{0};
      llvm::LLVMContext HiddenContext;
      EXPECT_THROW(MedLLVMEmitter().emit({Function}, HiddenContext, kName,
                                         Arch::AArch64, {}, nullptr,
                                         BinaryFormat::ELF, false, &Bodies),
                   std::runtime_error);
    }
    Function.Blocks.front().Ops.front() = Valid;
    for (Arch Other : {Arch::X64, Arch::ARM}) {
      llvm::LLVMContext Context;
      EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, kName, Other),
                   std::runtime_error);
    }
  }
  for (const auto &Case :
       {WritebackCases[0], WritebackCases[kWritebackFirstLoadCase]}) {
    auto Dead = writebackFunction(Case, 0);
    Dead.ReturnType = NdType::makeInt(kScalarBytes, false);
    MedOp Zero;
    Zero.Opcode = NdOp::COPY;
    Zero.Output = Dead.Blocks.front().Ops.front().Output;
    Zero.Output.Id = 1;
    Zero.Output.SSAVer = 2;
    Zero.Output.Size = kScalarBytes;
    Zero.addInput(MedVar::makeConst(0, kScalarBytes));
    Dead.Blocks.front().Ops.back().Inputs[0] = Zero.Output;
    Dead.Blocks.front().Ops.insert(Dead.Blocks.front().Ops.end() - 1, Zero);
    llvm::LLVMContext DeadContext;
    auto Module =
        MedLLVMEmitter().emit({Dead}, DeadContext, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    optimizeMemory(*Module);
    for (bool LLVMRoute : {false, true})
      compileMemoryC(memorySource(Dead, *Module, LLVMRoute, nullptr, true),
                     false, [&](llvm::MemoryBufferRef Buffer) {
                       inspectMemoryObject(Buffer, Case.Scratch);
                     });
  }
  for (const auto &Case :
       {WritebackCases[0], WritebackCases[kWritebackFirstLoadCase]}) {
    const auto Operations = liftMemory(Case.Word, false);
    const auto Memory = std::find_if(Operations.begin(), Operations.end(),
                                     lowAArch64WritebackMemoryShapeIsValid);
    ASSERT_NE(Memory, Operations.end());
    auto Invalid = *Memory;
    Invalid.Inputs[kWritebackWordOperand + 1].Size = kScalarBytes;
    auto Error = validateLowInstructionBoundaries(lowFunction(Invalid));
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  auto Function = writebackFunction(WritebackCases[kWritebackFirstLoadCase], 0);
  Function.Blocks.front().Ops.front().Inputs[2].ConstVal = kWritebackStackLoad;
  ASSERT_TRUE(aarch64WritebackMemoryMedShapeIsValid(
      Function.Blocks.front().Ops.front()));
  Function.Blocks.front().Ops.front().Inputs[1].Kind = MedVar::Temp;
  llvm::LLVMContext Context;
  EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64),
               std::runtime_error);
  const auto High = MedToHighConverter().convert(Function, Arch::AArch64);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  EXPECT_THROW(HighCEmitter().emit({High}, Output, Options),
               std::invalid_argument);
}

TEST(AArch64WritebackMemory,
     ImageBasesAndReturnedRuntimeBasesUseCompleteStorage) {
  for (bool Store : {false, true}) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    Segment Data;
    Data.VA = kWritebackImageData;
    Data.Size = Data.FileSz = kWritebackImageStorageBytes;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(Data.Size);
    writeLE<uint64_t>(Data.Data.data() + kWritebackImagePosition + kScalarBytes,
                      kWritebackImageLoaded);
    Image.Segments.push_back(Data);
    WritebackCase Case{Store ? kWritebackImageStore : kWritebackImageLoad,
                       0,
                       kScalarBytes,
                       Store ? 0U : kScalarBytes,
                       kScalarBytes,
                       !Store};
    auto Function = writebackFunction(Case, 0);
    auto &Ops = Function.Blocks.front().Ops;
    Ops.front().Inputs[1] = MedVar::makeConst(
        kWritebackImageData + kWritebackImagePosition, kScalarBytes,
        ConstantAddressProvenance::DataAddress, kWritebackImageData);
    if (Store)
      Ops.front().Inputs[3] =
          MedVar::makeConst(kWritebackImageLoaded, kScalarBytes);
    Function.Params.clear();
    Function.TypedParams.clear();
    MedVar Base = Ops.front().Output;
    if (!Store) {
      MedOp Extract;
      Extract.Opcode = NdOp::SUBBYTES;
      Extract.Output = Base;
      Extract.Output.Size = kScalarBytes;
      Extract.Output.Id = 1;
      Extract.addInput(Base);
      Extract.addInput(MedVar::makeConst(kScalarBytes, kWordBytes,
                                         ConstantAddressProvenance::Scalar));
      Ops.insert(Ops.begin() + 1, Extract);
      Base = Extract.Output;
    }
    MedOp Read;
    Read.Opcode = NdOp::INTRINSIC;
    Read.Output = Base;
    Read.Output.Id = 2;
    Read.addInput(MedVar::makeConst(
        static_cast<uint64_t>(Intrinsic::A64_LoadImmediate), kIdBytes));
    Read.addInput(Base);
    Read.addInput(MedVar::makeConst(0, kScalarBytes));
    Read.addInput(MedVar::makeConst(Store ? kWritebackImageReadback
                                          : kWritebackImageSecondLoad,
                                    kWordBytes));
    Ops.insert(Ops.end() - 1, Read);
    Ops.back().Inputs[0] = Read.Output;
    Function.ReturnType = NdType::makeInt(kScalarBytes, false);
    ASSERT_TRUE(verifyMedFunc(Function, kName));
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Function}, Context, kName,
                                        Arch::AArch64, {}, &Image);
    ASSERT_TRUE(Module);
    EXPECT_FALSE(Module->global_empty());
    optimizeMemory(*Module);
    for (bool LLVMRoute : {false, true}) {
      const auto Source =
          memorySource(Function, *Module, LLVMRoute, &Image, true);
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(
          Source +
              llvm::formatv(kWritebackReturnedBaseHarness, Function.Name).str(),
          true);
#else
      compileMemoryC(Source, false);
#endif
    }
    if (Store) {
      // Post-index may return one-past storage without accessing that byte.
      auto OnePast = Image;
      auto &Storage = OnePast.Segments.front();
      Storage.Size = Storage.FileSz = kWritebackImagePosition + kScalarBytes;
      Storage.Data.resize(Storage.Size);
      llvm::LLVMContext OnePastContext;
      auto OnePastModule = MedLLVMEmitter().emit(
          {Function}, OnePastContext, kName, Arch::AArch64, {}, &OnePast);
      ASSERT_TRUE(OnePastModule);
      for (bool LLVMRoute : {false, true}) {
        const auto Source =
            memorySource(Function, *OnePastModule, LLVMRoute, &OnePast, true);
#if defined(__APPLE__) && defined(__aarch64__)
        compileMemoryC(
            Source + llvm::formatv(kWritebackReturnedBaseHarness, Function.Name)
                         .str(),
            true);
#else
        compileMemoryC(Source, false);
#endif
      }
    }
    // Unknown provenance and a base without its full outgoing span fail closed.
    for (bool Truncate : {false, true}) {
      auto Invalid = Function;
      if (Truncate) {
        Image.Segments.front().Size = Image.Segments.front().FileSz =
            kWritebackImagePosition + kScalarBytes - 1;
        Image.Segments.front().Data.resize(Image.Segments.front().Size);
      } else
        Invalid.Blocks.front().Ops.front().Inputs[1].Provenance =
            ConstantAddressProvenance::Unknown;
      llvm::LLVMContext InvalidContext;
      EXPECT_THROW(MedLLVMEmitter().emit({Invalid}, InvalidContext, kName,
                                         Arch::AArch64, {}, &Image),
                   std::runtime_error);
    }
  }
}

TEST(AArch64WritebackMemory,
     TracingCommitsOnlyCompleteControlIndependentOperations) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = kWritebackImageStorageBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  for (size_t I = 0; I != Data.Data.size(); ++I)
    Data.Data[I] = static_cast<uint8_t>(I);
  Image.Segments.push_back(Data);
  for (const auto &Case : WritebackCases)
    for (bool Strict : {false, true}) {
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      const auto Ops = liftMemory(Case.Word, false);
      const auto At = std::find_if(Ops.begin(), Ops.end(),
                                   lowAArch64WritebackMemoryShapeIsValid);
      ASSERT_NE(At, Ops.end());
      auto Memory = *At;
      Memory.Output = NdVar::reg(a64reg::V0, Memory.Output.Size);
      const auto BaseOffset = a64reg::X0 + kWritebackFixtureBase * kScalarBytes;
      const auto SourceOffset =
          a64reg::X0 + kWritebackFixtureDestination * kScalarBytes;
      const va_t Base =
          kEntry + kWritebackImagePosition + kWritebackLastImmediate;
      Emulator.setRegister(BaseOffset, Base);
      Emulator.setRegister(SourceOffset, kWritebackImageLoaded);
      ASSERT_TRUE(Emulator.step(Memory));
      const auto Result = Emulator.getRegisterBytes(a64reg::V0);
      ASSERT_TRUE(Result);
      EXPECT_EQ(
          readLE<uint64_t>(Result->data() + (Case.Result ? kScalarBytes : 0)),
          Base + static_cast<uint64_t>(Case.Offset));
      Emulator.setRegister(BaseOffset, kAddressMemoryPoisonBit | Base);
      EXPECT_FALSE(Emulator.step(Memory));
      EXPECT_EQ(Emulator.getRegisterBytes(a64reg::V0), Result);
    }
}

TEST(AArch64WritebackMemory,
     DecodedImagesRetainIncomingAndCompletedBaseOccurrences) {
  for (bool Add : {false, true})
    for (bool Writable : {false, true}) {
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::MachO;
      Image.MachOCPUSubtype = macho_loader::arm64e::kCPUSubtypeX1;
      Image.Base = Image.Entry = kEntry;
      std::vector<uint32_t> Words = {kWritebackImageADRP};
      if (Add)
        Words.push_back(kWritebackImageADD);
      Words.push_back(kWritebackImageLoad);
      Words.push_back(kWritebackImageSecondLoad);
      Words.push_back(kWritebackImageReturn);
      Segment Text;
      Text.VA = kEntry;
      Text.Size = Text.FileSz = Words.size() * kWordBytes;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data.resize(Text.Size);
      for (size_t I = 0; I != Words.size(); ++I)
        writeLE<uint32_t>(Text.Data.data() + I * kWordBytes, Words[I]);
      Image.Segments.push_back(std::move(Text));
      Segment Data;
      Data.VA = kWritebackImageData;
      Data.Size = Data.FileSz = kWritebackImageStorageBytes;
      Data.Flags = Writable ? SegmentFlags::Readable | SegmentFlags::Writable
                            : SegmentFlags::Readable;
      Data.Data.resize(Data.Size);
      writeLE<uint64_t>(Data.Data.data() + kScalarBytes +
                            (Add ? kWritebackImagePosition : 0),
                        kWritebackImageLoaded);
      Image.Segments.push_back(std::move(Data));
      Decoder Decoder;
      ASSERT_TRUE(Decoder.init(Image));
      const auto Low = CFGBuilder().build(Image, Decoder, kEntry, kName);
      LowToMedConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto Function =
          Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
      ASSERT_TRUE(Function.Params.empty());
      ASSERT_TRUE(verifyMedFunc(Function, kName));
      ASSERT_EQ(std::count_if(Function.Blocks.front().Ops.begin(),
                              Function.Blocks.front().Ops.end(),
                              aarch64WritebackMemoryMedShapeIsValid),
                1);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Function}, Context, kName,
                                          Arch::AArch64, {}, &Image);
      ASSERT_TRUE(Module);
      ASSERT_FALSE(llvm::verifyModule(*Module));
      optimizeMemory(*Module);
      for (bool LLVMRoute : {false, true}) {
        const auto Source =
            memorySource(Function, *Module, LLVMRoute, &Image, true);
#if defined(__APPLE__) && defined(__aarch64__)
        compileMemoryC(
            Source + llvm::formatv(kWritebackReturnedBaseHarness, Function.Name)
                         .str(),
            true);
#else
        compileMemoryC(Source, false);
#endif
      }
    }
  for (uint32_t Subtype : {uint32_t(llvm::MachO::CPU_SUBTYPE_ARM64_ALL),
                           macho_loader::arm64e::kCPUSubtype,
                           macho_loader::arm64e::kCPUSubtypeX1}) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    Image.MachOCPUSubtype = Subtype;
    Decoder Decoder;
    ASSERT_TRUE(Decoder.init(Image));
    std::array<uint8_t, kWordBytes> Bytes;
    writeLE<uint32_t>(Bytes.data(), kWritebackImageLoad);
    DecodedInsn Decoded;
    ASSERT_EQ(
        Decoder.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Decoded),
        kWordBytes);
    std::vector<LowOp> Ops;
    Decoder.liftToLow(Decoded, Ops);
    EXPECT_EQ(std::any_of(Ops.begin(), Ops.end(),
                          lowAArch64WritebackMemoryShapeIsValid),
              Subtype == macho_loader::arm64e::kCPUSubtypeX1);
  }
}

TEST(AArch64VectorWritebackMemory, IndependentWordsRetainSingleInstructions) {
  struct Case {
    uint32_t Word;
    uint32_t Scratch;
    unsigned Access;
    bool Load;
    bool Pre;
    int Offset;
  };
  constexpr Case Cases[] = {
#define VECTOR_WRITEBACK_CASE(Word, Scratch, Access, Load, Pre, Offset)        \
  {Word, Scratch, Access, Load, Pre, Offset},
#include "AArch64VectorWritebackMemoryFixtures.def"
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(llvm::utohexstr(Case.Word));
    const auto Instruction = aarch64WritebackMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
    EXPECT_EQ(Instruction->accessBytes(), Case.Access);
    EXPECT_EQ(Instruction->isLoad(), Case.Load);
    EXPECT_EQ(Instruction->preIndex(), Case.Pre);
    EXPECT_EQ(static_cast<int64_t>(Instruction->displacement()), Case.Offset);
    for (bool Fast : {false, true}) {
      const auto Ops = liftMemory(Case.Word, Fast);
      const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                       lowAArch64WritebackMemoryShapeIsValid);
      ASSERT_NE(Memory, Ops.end());
      EXPECT_EQ(Memory->Inputs[kWritebackWordOperand + 1].Offset, Case.Word);
      EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(*Memory));
      EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
               Op.Opcode == NdOp::INT_ADD;
      }));
    }
  }
}

namespace {
#define VECTOR_WRITEBACK_VALUE(Type, Name, Value)                              \
  inline constexpr Type Name = Value;
#define VECTOR_WRITEBACK_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64VectorWritebackMemoryFixtures.def"

struct VectorWritebackCase {
  uint32_t Word;
  uint32_t Scratch;
  unsigned Access;
  bool Load;
  bool Pre;
  int Offset;
};
constexpr VectorWritebackCase VectorWritebackCases[] = {
#define VECTOR_WRITEBACK_CASE(Word, Scratch, Access, Load, Pre, Offset)        \
  {Word, Scratch, Access, Load, Pre, Offset},
#include "AArch64VectorWritebackMemoryFixtures.def"
};

MedFunc vectorWritebackFunction(const VectorWritebackCase &Case, size_t Index) {
  MedFunc Function;
  Function.Name = kVectorWritebackNamePrefix + llvm::utohexstr(Case.Word);
  Function.Entry = kEntry + Index * kVectorFunctionStride;
  Function.ReturnType = NdType::makeInt(kScalarBytes, false);
  Function.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  for (unsigned I = 0; I != (Case.Load ? kVectorWritebackLoadParameters
                                       : kVectorWritebackStoreParameters);
       ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Arch::AArch64;
    Parameter.Id = I;
    Parameter.RegOff = a64reg::X0 + I * kScalarBytes;
    Parameter.Size = kScalarBytes;
    Function.Params.push_back(Parameter);
    Function.TypedParams.push_back({{}, NdType::makeInt(kScalarBytes, false)});
  }
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  int Next = Function.Params.size() + 1;
  const auto Temp = [&](unsigned Bytes) {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.TheArch = Arch::AArch64;
    Value.Id = Next++;
    Value.SSAVer = 1;
    Value.Size = Bytes;
    return Value;
  };
  const auto Emit = [&](NdOp Opcode, MedVar Output,
                        std::initializer_list<MedVar> Inputs) {
    MedOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = Function.Entry;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Block.Ops.push_back(Op);
  };
  const auto Value = Temp(kAddressMemoryVectorBytes);
  const auto Updated = Temp(kScalarBytes);
  MedVar Data;
  if (!Case.Load) {
    const auto Low = Temp(kAddressMemoryVectorBytes);
    const auto High = Temp(kAddressMemoryVectorBytes);
    const auto Shifted = Temp(kAddressMemoryVectorBytes);
    Data = Temp(kAddressMemoryVectorBytes);
    Emit(NdOp::INT_ZEXT, Low, {Function.Params[1]});
    Emit(NdOp::INT_ZEXT, High, {Function.Params[2]});
    Emit(NdOp::INT_LEFT, Shifted,
         {High, MedVar::makeConst(kAddressMemoryPointerBits, kWordBytes)});
    Emit(NdOp::INT_OR, Data, {Low, Shifted});
  }
  MedVar Primary = Updated;
  if (Case.Load) {
    Primary = MedVar{};
    Primary.Id = -1;
  }
  Emit(NdOp::INTRINSIC, Primary,
       {MedVar::makeConst(static_cast<uint64_t>(
                              Case.Load ? Intrinsic::A64_VectorLoadWriteback
                                        : Intrinsic::A64_VectorStoreWriteback),
                          kIdBytes),
        Function.Params.front(), MedVar::makeConst(Case.Word, kWordBytes)});
  if (Case.Load)
    Block.Ops.back().IntrinsicOutputs = {Value, Updated};
  else
    Block.Ops.back().addInput(Data);
  const auto Bits = Case.Load ? Value : Data;
  const auto Low = Temp(kScalarBytes);
  const auto High = Temp(kScalarBytes);
  const auto OutHigh = Temp(kScalarBytes);
  Emit(NdOp::SUBBYTES, Low, {Bits, MedVar::makeConst(0, kWordBytes)});
  Emit(NdOp::SUBBYTES, High,
       {Bits, MedVar::makeConst(kScalarBytes, kWordBytes)});
  Emit(NdOp::INT_ADD, OutHigh,
       {Function.Params.back(), MedVar::makeConst(kScalarBytes, kScalarBytes)});
  Emit(NdOp::STORE, {}, {Function.Params.back(), Low});
  Emit(NdOp::STORE, {}, {OutHigh, High});
  MedVar Return;
  Return.Kind = MedVar::Reg;
  Return.TheArch = Arch::AArch64;
  Return.RegOff = a64reg::X0;
  Return.Id = 0;
  Return.SSAVer = 1;
  Return.Size = kScalarBytes;
  Emit(NdOp::COPY, Return, {Updated});
  Emit(NdOp::RETURN, {}, {Return});
  return Function;
}

void executeVectorWritebackFamilies(const BinaryImage *Image,
                                    bool Stack = false) {
  std::vector<MedFunc> Functions;
  std::string Declarations, Replays, Entries, Faults, FaultEntries;
  for (size_t I = 0; I != std::size(VectorWritebackCases); ++I) {
    auto Case = VectorWritebackCases[I];
    if (Stack) {
      Case.Word = (Case.Word & ~kStackBaseMask) | kStackBaseMask;
      Case.Scratch = (Case.Scratch & ~kStackBaseMask) | kStackBaseMask;
    }
    auto Function = vectorWritebackFunction(Case, I);
    ASSERT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
    Declarations += llvm::formatv(Case.Load ? kVectorWritebackLoadDeclaration
                                            : kVectorWritebackStoreDeclaration,
                                  Function.Name)
                        .str();
    const auto Native =
        llvm::formatv(Case.Load ? kVectorWritebackNativeLoad
                                : kVectorWritebackNativeStore,
                      I, Case.Word,
                      Stack ? kStackOracleX21Before : kEmptyNativeArgument,
                      Stack ? kStackOracleX21After : kEmptyNativeArgument,
                      Stack ? kStackOracleClobbers : kEmptyNativeArgument)
            .str();
    const auto Call = llvm::formatv(Case.Load ? kVectorWritebackLoadCall
                                              : kVectorWritebackStoreCall,
                                    Function.Name)
                          .str();
    Replays +=
        Native + llvm::formatv(kVectorWritebackNativeCase, I, Case.Offset,
                               int(Case.Pre), Case.Access, int(Case.Load), Call)
                     .str();
    Entries += llvm::formatv(kVectorWritebackHarnessEntry, I).str();
    if (Stack) {
      Faults += llvm::formatv(kStackFaultWrapper, I, Function.Name,
                              Case.Load ? kStackFaultOutputArguments
                                        : kStackFaultDataArguments)
                    .str();
      FaultEntries += llvm::formatv(kStackFaultEntry, I).str();
    }
    Functions.push_back(std::move(Function));
  }
  const auto Harness =
      std::string(kVectorWritebackHarnessPrelude) + Declarations + Replays +
      (Stack ? std::string(kStackFaultBegin) + Faults +
                   llvm::formatv(kStackFaultTable, FaultEntries).str() +
                   kStackFaultEnd
             : std::string()) +
      llvm::formatv(kVectorWritebackHarnessTail, Entries).str();
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(Functions, Context, kName, Arch::AArch64,
                                      {}, Image);
  ASSERT_TRUE(Module);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  optimizeMemory(*Module);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  Options.Image = Image;
  std::string ArtifactDirectory;
  if (!Image)
    if (const char *Directory =
            std::getenv(Stack ? kStackVectorWritebackArtifactEnvironment
                              : kVectorWritebackArtifactEnvironment)) {
      ArtifactDirectory = Directory;
      ::testing::Test::RecordProperty(
          Stack ? kStackVectorWritebackArtifactProperty
                : kVectorWritebackArtifactProperty,
          ArtifactDirectory);
      ASSERT_FALSE(llvm::sys::fs::create_directories(ArtifactDirectory));
    }
  const auto Save = [&](llvm::StringRef Name, llvm::StringRef Data) {
    if (ArtifactDirectory.empty())
      return;
    llvm::SmallString<kVectorPathStorage> Path(ArtifactDirectory);
    llvm::sys::path::append(Path, Name);
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error);
    Out << Data;
  };
  Save(kWritebackHarnessArtifact, Harness);
  for (bool LLVMRoute : {false, true}) {
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    if (LLVMRoute)
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, Options));
    else {
      std::vector<HighFunc> High;
      for (const auto &Function : Functions) {
        auto Body = MedToHighConverter().convert(Function, Arch::AArch64);
        for (auto &Parameter : Body.Params)
          Parameter.Type = NdType::makeInt(kScalarBytes, false);
        High.push_back(std::move(Body));
      }
      ASSERT_TRUE(HighCEmitter().emit(High, Output, Options));
    }
    Save(LLVMRoute ? kWritebackLLVMArtifact : kWritebackHighArtifact, Source);
#if defined(__APPLE__) && defined(__aarch64__)
    compileMemoryC(Source + Harness, true);
#else
    compileMemoryC(Source, false);
#endif
  }
  const auto Object =
      Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(Object.Success);
  if (Stack)
    Save(kImmediateMachOArtifact,
         llvm::StringRef(
             reinterpret_cast<const char *>(Object.ObjectData.data()),
             Object.ObjectData.size()));
  const llvm::MemoryBufferRef Buffer(
      llvm::StringRef(reinterpret_cast<const char *>(Object.ObjectData.data()),
                      Object.ObjectData.size()),
      kObject);
  for (const auto &Case : VectorWritebackCases)
    inspectMemoryObject(Buffer, Stack ? (Case.Scratch & ~kStackBaseMask) |
                                            kStackBaseMask
                                      : Case.Scratch);
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
  compileMemoryC(Harness, true, {}, ObjectPath);
#endif
  if (!ArtifactDirectory.empty()) {
    const auto ELF =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::ELF);
    ASSERT_TRUE(ELF.Success);
    Save(kWritebackObjectArtifact,
         llvm::StringRef(reinterpret_cast<const char *>(ELF.ObjectData.data()),
                         ELF.ObjectData.size()));
  }
}
} // namespace

TEST(AArch64VectorWritebackMemory, BothCProjectionsAndLLVMExecuteEveryFamily) {
  executeVectorWritebackFamilies(nullptr);
}

TEST(AArch64VectorWritebackMemory,
     EveryImmediateKeepsTheIndependentOperandDetail) {
  for (const auto &Case : VectorWritebackCases) {
    if (Case.Offset)
      continue;
    for (unsigned Immediate = kWritebackFirstOffset;
         Immediate != kWritebackLastImmediate; ++Immediate) {
      const auto Word = (Case.Word & ~kWritebackFixtureOffsetMask) |
                        (Immediate << kWritebackFixtureOffsetShift);
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Arch::AArch64));
      std::array<uint8_t, kWordBytes> Bytes;
      writeLE<uint32_t>(Bytes.data(), Word);
      DecodedInsn Decoded;
      ASSERT_EQ(Decode.decodeOne(Bytes.data(), Bytes.size(), kEntry, Decoded),
                kWordBytes);
      ASSERT_TRUE(Decoded.Raw && Decoded.Raw->detail);
      const auto &Detail = Decoded.Raw->detail->aarch64;
      ASSERT_TRUE(Decoded.Raw->detail->writeback);
      EXPECT_EQ(Detail.post_index, !Case.Pre);
      ASSERT_GE(Detail.op_count, 2);
      ASSERT_EQ(Detail.operands[1].type, AARCH64_OP_MEM);
      const auto Offset = !Case.Pre && Detail.op_count >= 3 &&
                                  Detail.operands[2].type == AARCH64_OP_IMM
                              ? Detail.operands[2].imm
                              : Detail.operands[1].mem.disp;
      const auto Instruction = aarch64WritebackMemoryInstruction(Word);
      ASSERT_TRUE(Instruction);
      EXPECT_TRUE(Instruction->isVector());
      EXPECT_EQ(Instruction->base(), Instruction->destination());
      EXPECT_EQ(static_cast<int64_t>(Instruction->displacement()), Offset);
      for (bool Fast : {false, true}) {
        const auto Ops = liftMemory(Word, Fast);
        const auto At = std::find_if(Ops.begin(), Ops.end(),
                                     lowAArch64WritebackMemoryShapeIsValid);
        ASSERT_NE(At, Ops.end());
        auto Function = lowFunction(*At);
        auto &Block = Function.Blocks.front();
        Block.Ops = Ops;
        Block.InstructionBoundaries.front().OpCount = Ops.size();
        auto Error = validateLowInstructionBoundaries(Function);
        EXPECT_FALSE(bool(Error));
        llvm::consumeError(std::move(Error));
      }
    }
  }
}

TEST(AArch64VectorWritebackMemory, ImageContextKeepsTheCompleteRuntimeResults) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Segment Data;
  Data.VA = kStorageBase;
  Data.Size = Data.FileSz = kVectorWritebackStorageBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));
  executeVectorWritebackFamilies(&Image);
}

TEST(AArch64VectorWritebackMemory, MalformedTuplesFailBeforeBodyFiltering) {
  for (bool Load : {false, true}) {
    const auto At = std::find_if(std::begin(VectorWritebackCases),
                                 std::end(VectorWritebackCases),
                                 [&](const auto &C) { return C.Load == Load; });
    ASSERT_NE(At, std::end(VectorWritebackCases));
    const auto Function = vectorWritebackFunction(*At, 0);
    const auto &Ops = Function.Blocks.front().Ops;
    const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                     aarch64WritebackMemoryMedShapeIsValid);
    ASSERT_NE(Memory, Ops.end());
    const auto Index = Memory - Ops.begin();
    std::vector<std::function<void(MedOp &)>> Mutations = {
        [](MedOp &Op) { Op.Output.Size = kAddressMemoryVectorBytes; },
        [](MedOp &Op) {
          Op.Inputs[kWritebackBaseOperand + 1].Size = kWordBytes;
        },
        [](MedOp &Op) {
          Op.Inputs[kWritebackWordOperand + 1].Size = kScalarBytes;
        },
        [](MedOp &Op) { Op.Inputs[kWritebackWordOperand + 1] = Op.Inputs[1]; },
        [](MedOp &Op) { Op.Output.TheArch = Arch::X64; },
        [](MedOp &Op) { Op.MemoryOrdering = NdMemoryOrdering::Acquire; },
        [](MedOp &Op) { Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS; },
    };
    if (Load) {
      Mutations.push_back([](MedOp &Op) { Op.IntrinsicOutputs.pop_back(); });
      Mutations.push_back(
          [](MedOp &Op) { Op.IntrinsicOutputs.front().Size = kScalarBytes; });
      Mutations.push_back([](MedOp &Op) {
        Op.IntrinsicOutputs.back().Size = kAddressMemoryVectorBytes;
      });
      Mutations.push_back([](MedOp &Op) {
        Op.IntrinsicOutputs.back() = Op.IntrinsicOutputs.front();
      });
      Mutations.push_back(
          [](MedOp &Op) { Op.IntrinsicOutputs.front().Kind = MedVar::Const; });
      Mutations.push_back(
          [](MedOp &Op) { Op.IntrinsicOutputs.back().TheArch = Arch::X64; });
    } else {
      Mutations.push_back([](MedOp &Op) {
        Op.Inputs[kWritebackDataOperand + 1].Size = kScalarBytes;
      });
    }
    for (const auto &Mutate : Mutations) {
      auto Invalid = Function;
      auto &Bad = Invalid.Blocks.front().Ops[Index];
      Mutate(Bad);
      EXPECT_FALSE(aarch64WritebackMemoryMedShapeIsValid(Bad));
      for (bool Hidden : {false, true}) {
        llvm::LLVMContext Context;
        const std::vector<char> Mask{0};
        EXPECT_THROW(MedLLVMEmitter().emit(
                         {Invalid}, Context, kName, Arch::AArch64, {}, nullptr,
                         BinaryFormat::ELF, false, Hidden ? &Mask : nullptr),
                     std::runtime_error);
      }
    }
    const auto OriginalHigh =
        MedToHighConverter().convert(Function, Arch::AArch64);
    for (bool Hidden : {false, true}) {
      auto High = OriginalHigh;
      const auto Damage = [&](const ExprPtr &E) {
        if (E && isAArch64WritebackMemoryIntrinsic(E->IntrinsicId))
          E->Operands[kWritebackWordOperand] =
              HighExpr::makeConst(At->Word, kScalarBytes);
      };
      walkStmts(High.Body, [&](const HighStmt &S) { forEachExpr(S, Damage); });
      if (Hidden) {
        HighStmt If;
        If.Kind = StmtKind::If;
        If.Cond = HighExpr::makeConst(0, 1);
        If.Body = std::move(High.Body);
        High.Body = {std::move(If)};
      }
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      EXPECT_THROW(HighCEmitter().emit({High}, Out, Options),
                   std::invalid_argument);
      EXPECT_TRUE(Source.empty());
    }
    auto Stack = Function;
    Stack.Blocks.front().Ops[Index].Inputs[kWritebackWordOperand + 1].ConstVal =
        (At->Word & ~kVectorWritebackBaseMask) | kVectorWritebackSPBase;
    ASSERT_TRUE(
        aarch64WritebackMemoryMedShapeIsValid(Stack.Blocks.front().Ops[Index]));
    Stack.Blocks.front().Ops[Index].Inputs[kWritebackBaseOperand + 1].Kind =
        MedVar::Temp;
    llvm::LLVMContext StackContext;
    EXPECT_THROW(
        MedLLVMEmitter().emit({Stack}, StackContext, kName, Arch::AArch64),
        std::runtime_error);
    if (Load) {
      const auto Operations = liftMemory(At->Word, false);
      const auto Memory = std::find_if(Operations.begin(), Operations.end(),
                                       lowAArch64WritebackMemoryShapeIsValid);
      ASSERT_NE(Memory, Operations.end());
      auto Low = lowFunction(*Memory);
      auto &Block = Low.Blocks.front();
      for (unsigned Mutation = 0; Mutation != kVectorWritebackLowMutations;
           ++Mutation) {
        Block.Ops = Operations;
        if (Mutation == 0)
          Block.Ops.pop_back();
        else if (Mutation == 1) {
          auto &Output = Block.Ops[Memory - Operations.begin() + 1];
          Output.Inputs[0].Size = kScalarBytes;
        } else
          Block.Ops.back().Inputs[0].Offset =
              Block.Ops[Memory - Operations.begin() + 1].Inputs[0].Offset;
        Block.InstructionBoundaries.front().OpCount = Block.Ops.size();
        auto Error = validateLowInstructionBoundaries(Low);
        EXPECT_TRUE(bool(Error));
        llvm::consumeError(std::move(Error));
        EXPECT_THROW(LowToMedConverter().convert(Low, Arch::AArch64),
                     LowToMedConversionError);
        if (Mutation == kVectorWritebackLowMutations - 1) {
          const auto Generic = collectLowIntrinsicOutputBindings(
              Block.Ops[Memory - Operations.begin()],
              llvm::ArrayRef(Block.Ops).drop_front(Memory - Operations.begin() +
                                                   1),
              aarch64WritebackMemoryOutputCount(
                  Intrinsic::A64_VectorLoadWriteback));
          ASSERT_EQ(Generic.size(), 2u);
          EXPECT_EQ(Generic.front().Offset, Generic.back().Offset);
          EXPECT_NE(Generic.front().Size, Generic.back().Size);
        }
      }
    }
  }
}

TEST(AArch64VectorWritebackMemory,
     TracerCommitsCompleteResultsAndRefusesMissingBindings) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = kVectorWritebackStorageBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  for (size_t I = 0; I != Data.Data.size(); ++I)
    Data.Data[I] = static_cast<uint8_t>(I * kVectorWritebackSeedMultiplier +
                                        kVectorWritebackSeedBias);
  Image.Segments.push_back(Data);
  for (const auto &Case : VectorWritebackCases)
    for (bool Strict : {false, true}) {
      const auto Instruction = *aarch64WritebackMemoryInstruction(Case.Word);
      const auto Ops = liftMemory(Case.Word, false);
      const auto BaseRegister = a64reg::X0 + Instruction.base() * kScalarBytes;
      const auto VectorRegister = a64reg::V(Instruction.destination());
      const uint64_t Base = kEntry + kVectorWritebackBasePosition;
      const uint64_t Access =
          Base + static_cast<uint64_t>(Case.Pre ? Case.Offset : 0);
      std::vector<uint8_t> Initial(kAddressMemoryVectorBytes);
      for (unsigned I = 0; I != Initial.size(); ++I)
        Initial[I] = kVectorWritebackVectorSeed + I;
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setRegister(BaseRegister, Base);
      Emulator.setRegisterBytes(VectorRegister, Initial);
      auto Expected = Data.Data;
      std::vector<uint8_t> Value(kAddressMemoryVectorBytes, 0);
      if (Case.Load)
        std::copy_n(Data.Data.begin() + Access - Data.VA, Case.Access,
                    Value.begin());
      else {
        Value = Initial;
        std::copy_n(Initial.begin(), Case.Access,
                    Expected.begin() + Access - Data.VA);
      }
      EXPECT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegister(BaseRegister),
                Base + static_cast<uint64_t>(Case.Offset));
      EXPECT_EQ(Emulator.getRegisterBytes(VectorRegister),
                std::optional(Value));
      for (size_t Byte = 0; Byte != Expected.size();
           Byte += kAddressMemoryVectorBytes) {
        LowOp Read;
        Read.Opcode = NdOp::LOAD;
        Read.Output = NdVar::reg(a64reg::V0, kAddressMemoryVectorBytes);
        Read.addInput(NdVar::scalar(Data.VA + Byte, kScalarBytes));
        ASSERT_TRUE(Emulator.step(Read));
        const auto Actual = Emulator.getRegisterBytes(a64reg::V0);
        ASSERT_TRUE(Actual);
        EXPECT_TRUE(std::equal(Actual->begin(), Actual->end(),
                               Expected.begin() + Byte));
      }
      auto Short = Image;
      auto &Storage = Short.Segments.front();
      Storage.Size = Storage.FileSz = Access - Storage.VA + Case.Access - 1;
      Storage.Data.resize(Storage.Size);
      NdOpEmulator Refused(Short);
      Refused.setStrictMode(Strict);
      Refused.setRegister(BaseRegister, Base);
      Refused.setRegisterBytes(VectorRegister, Initial);
      EXPECT_EQ(Refused.run(Ops), 0u);
      EXPECT_EQ(Refused.getRegister(BaseRegister), Base);
      EXPECT_EQ(Refused.getRegisterBytes(VectorRegister),
                std::optional(Initial));
      if (Case.Load) {
        NdOpEmulator Naked(Image);
        Naked.setRegister(BaseRegister, Base);
        Naked.setRegisterBytes(VectorRegister, Initial);
        EXPECT_FALSE(Naked.step(Ops.front()));
        EXPECT_EQ(Naked.getRegisterBytes(VectorRegister),
                  std::optional(Initial));
        auto Missing = Ops;
        Missing.pop_back();
        EXPECT_EQ(Naked.run(Missing), 0u);
      }
    }
}

TEST(AArch64VectorWritebackMemory, LLVMCallAdmissionPrecedesFunctionSelection) {
  for (bool Load : {false, true}) {
    const auto At = std::find_if(std::begin(VectorWritebackCases),
                                 std::end(VectorWritebackCases),
                                 [&](const auto &C) { return C.Load == Load; });
    ASSERT_NE(At, std::end(VectorWritebackCases));
    for (unsigned Mutation = 0; Mutation != kVectorWritebackCallMutations;
         ++Mutation) {
      llvm::LLVMContext Context;
      const auto Function = vectorWritebackFunction(*At, 0);
      auto Module =
          MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
      ASSERT_TRUE(Module);
      llvm::CallInst *Owned = nullptr;
      for (auto &Block : *Module->getFunction(Function.Name))
        for (auto &Operation : Block)
          if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Operation))
            if (validateAArch64WritebackMemoryCall(Arch::AArch64, *Call))
              Owned = Call;
      ASSERT_TRUE(Owned);
      EXPECT_THROW(validateAArch64WritebackMemoryCall(Arch::X64, *Owned),
                   std::invalid_argument);
      const auto *Asm = llvm::cast<llvm::InlineAsm>(Owned->getCalledOperand());
      const auto Last = Owned->arg_size() - 1;
      const auto Word =
          llvm::cast<llvm::ConstantInt>(Owned->getArgOperand(Last))
              ->getZExtValue();
      if (Mutation == 0)
        Owned->setArgOperand(
            Last, llvm::ConstantInt::get(Owned->getArgOperand(Last)->getType(),
                                         Word ^ kVectorWritebackVectorBit));
      else if (Mutation == 1)
        Owned->setArgOperand(
            Last, llvm::ConstantInt::get(Owned->getArgOperand(Last)->getType(),
                                         Word ^ kVectorWritebackLoadBit));
      else if (Mutation == 2)
        Owned->setArgOperand(
            Last, llvm::ConstantInt::get(Owned->getArgOperand(Last)->getType(),
                                         (Word & ~kVectorWritebackBaseMask) |
                                             kVectorWritebackSPBase));
      else if (Mutation == 3)
        Owned->setCalledOperand(llvm::InlineAsm::get(
            Asm->getFunctionType(), Asm->getAsmString(),
            writebackConstraintsWithoutMemoryClobber(*Asm), true));
      else if (Mutation == 4)
        Owned->setCalledOperand(
            llvm::InlineAsm::get(Asm->getFunctionType(), Asm->getAsmString(),
                                 Asm->getConstraintString(), false));
      else if (Mutation == 5)
        Owned->setMemoryEffects(llvm::MemoryEffects::none());
      else if (Mutation == 6)
        Owned->setCallingConv(llvm::CallingConv::Fast);
      else if (Mutation == 7)
        Owned->addFnAttr(llvm::Attribute::Speculatable);
      else if (Mutation == 11)
        Owned->setDoesNotReturn();
      else
        Owned->setCalledOperand(llvm::InlineAsm::get(
            Asm->getFunctionType(), Asm->getAsmString(),
            Asm->getConstraintString(), true, Mutation == 8,
            Mutation == 9 ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT,
            Mutation == 10));
      EXPECT_THROW(validateAArch64WritebackMemoryCall(Arch::AArch64, *Owned),
                   std::invalid_argument);
      auto *Other = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
          llvm::GlobalValue::ExternalLinkage, kVectorWritebackHiddenFunction,
          *Module);
      auto *Entry = llvm::BasicBlock::Create(Context, {}, Other);
      llvm::IRBuilder<> Builder(Entry);
      Builder.CreateRetVoid();
      for (bool Selected : {false, true}) {
        CEmitterOptions Options;
        Options.TheArch = Arch::AArch64;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        EXPECT_THROW(LLVMCEmitter().emit(*Module, Out, Options, nullptr,
                                         nullptr, Selected ? Other : nullptr),
                     std::invalid_argument);
        EXPECT_TRUE(Source.empty());
      }
    }
  }
}

namespace {
MedFunc ownedVectorWritebackFunction(const VectorWritebackCase &Case,
                                     const BinaryImage &Image, va_t Base,
                                     bool Follow) {
  auto Function = vectorWritebackFunction(Case, 0);
  std::vector<MedVar> Replacements;
  Replacements.push_back(MedVar::makeConst(
      Base, kScalarBytes, ConstantAddressProvenance::DataAddress));
  if (!Case.Load) {
    Replacements.push_back(
        MedVar::makeConst(kVectorWritebackDataLow, kScalarBytes));
    Replacements.push_back(
        MedVar::makeConst(kVectorWritebackDataHigh, kScalarBytes));
  }
  Replacements.push_back(
      MedVar::makeConst(Image.Segments.front().VA, kScalarBytes,
                        ConstantAddressProvenance::DataAddress));
  auto &Ops = Function.Blocks.front().Ops;
  for (auto &Op : Ops)
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      if (Op.Inputs[I].Kind == MedVar::Param)
        Op.Inputs[I] = Replacements[Op.Inputs[I].Id];
  Function.Params.clear();
  Function.TypedParams.clear();
  Function.CC = CallingConv::ARM_AAPCS;
  const auto Updated = Ops[Ops.size() - 2].Inputs.front();
  const auto Return = Ops[Ops.size() - 2].Output;
  const auto FirstStore =
      std::find_if(Ops.begin(), Ops.end(),
                   [](const MedOp &Op) { return Op.Opcode == NdOp::STORE; });
  const auto LastStore =
      std::find_if(Ops.rbegin(), Ops.rend(),
                   [](const MedOp &Op) { return Op.Opcode == NdOp::STORE; });
  const auto Low = FirstStore->Inputs[1];
  const auto High = LastStore->Inputs[1];
  int Next = 0;
  for (const auto &Op : Ops) {
    Next = std::max(Next, Op.Output.Id + 1);
    for (const auto &Value : Op.IntrinsicOutputs)
      Next = std::max(Next, Value.Id + 1);
  }
  const auto Temp = [&] {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.TheArch = Arch::AArch64;
    Value.Id = Next++;
    Value.SSAVer = 1;
    Value.Size = kScalarBytes;
    return Value;
  };
  Ops.resize(Ops.size() - 2);
  const auto Emit = [&](NdOp Opcode, MedVar Output,
                        std::initializer_list<MedVar> Inputs) {
    MedOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = Function.Entry;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Ops.push_back(Op);
  };
  const auto Delta = Temp();
  Emit(NdOp::INT_SUB, Delta, {Updated, Replacements.front()});
  auto Result = Delta;
  if (Follow) {
    const auto Read = Temp();
    Emit(NdOp::INTRINSIC, Read,
         {MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_LoadImmediate),
                            kIdBytes),
          Updated, MedVar::makeConst(0, kScalarBytes),
          MedVar::makeConst(kVectorWritebackFollowLoad, kWordBytes)});
    const auto Pair = Temp();
    const auto WithRead = Temp();
    Result = Temp();
    Emit(NdOp::INT_XOR, Pair, {Low, High});
    Emit(NdOp::INT_XOR, WithRead, {Pair, Read});
    Emit(NdOp::INT_XOR, Result, {WithRead, Delta});
  }
  Emit(NdOp::COPY, Return, {Result});
  Emit(NdOp::RETURN, {}, {Return});
  return Function;
}
} // namespace

TEST(AArch64VectorWritebackMemory,
     OwnedImagesKeepBothValuesAndReturnedRuntimeBases) {
  for (bool Load : {false, true})
    for (bool Pre : {false, true}) {
      const auto At =
          std::find_if(std::begin(VectorWritebackCases),
                       std::end(VectorWritebackCases), [&](const auto &C) {
                         return C.Load == Load && C.Pre == Pre &&
                                C.Access == kAddressMemoryVectorBytes &&
                                C.Offset == kVectorWritebackOwnedOffset;
                       });
      ASSERT_NE(At, std::end(VectorWritebackCases));
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::MachO;
      Segment Data;
      Data.VA = kStorageBase;
      Data.Size = Data.FileSz = kVectorWritebackStorageBytes;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(Data.Size);
      for (size_t I = 0; I < Data.Data.size(); ++I)
        Data.Data[I] = static_cast<uint8_t>(I * kVectorWritebackSeedMultiplier +
                                            kVectorWritebackSeedBias);
      Image.Segments.push_back(Data);
      const auto Base = Data.VA + kVectorWritebackBasePosition;
      const auto Function =
          ownedVectorWritebackFunction(*At, Image, Base, true);
      auto Expected = Data.Data;
      const auto Access = kVectorWritebackBasePosition + (Pre ? At->Offset : 0);
      uint64_t Low, High;
      if (Load) {
        Low = readLE<uint64_t>(Expected.data() + Access);
        High = readLE<uint64_t>(Expected.data() + Access + kScalarBytes);
      } else {
        Low = kVectorWritebackDataLow;
        High = kVectorWritebackDataHigh;
        writeLE<uint64_t>(Expected.data() + Access, Low);
        writeLE<uint64_t>(Expected.data() + Access + kScalarBytes, High);
      }
      const auto Result =
          Low ^ High ^
          readLE<uint64_t>(Expected.data() + kVectorWritebackBasePosition +
                           At->Offset) ^
          static_cast<uint64_t>(At->Offset);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Function}, Context, kName,
                                          Arch::AArch64, {}, &Image);
      ASSERT_TRUE(Module);
      ASSERT_FALSE(llvm::verifyModule(*Module));
      for (bool LLVMRoute : {false, true}) {
        const auto Source = memorySource(Function, *Module, LLVMRoute, &Image);
#if defined(__APPLE__) && defined(__aarch64__)
        compileMemoryC(Source + llvm::formatv(kVectorWritebackOwnedHarness,
                                              Function.Name, Result)
                                    .str(),
                       true);
#else
        compileMemoryC(Source, false);
#endif
      }
      if (Load)
        for (bool EarlyCopy : {false, true}) {
          auto High = MedToHighConverter().convert(Function, Arch::AArch64);
          ExprPtr Loaded, Follow;
          auto At = std::find_if(
              High.Body.begin(), High.Body.end(),
              [&](const HighStmt &Statement) {
                bool Defines = false;
                forEachExpr(Statement, [&](const ExprPtr &E) {
                  if (E &&
                      E->IntrinsicId == Intrinsic::A64_VectorLoadWriteback) {
                    Loaded = E;
                    Defines = true;
                  }
                });
                return Defines;
              });
          ASSERT_NE(At, High.Body.end());
          walkStmts(High.Body, [&](const HighStmt &Statement) {
            forEachExpr(Statement, [&](const ExprPtr &E) {
              if (E && E->IntrinsicId == Intrinsic::A64_LoadImmediate)
                Follow = E;
            });
          });
          ASSERT_TRUE(Loaded && Follow);
          if (EarlyCopy) {
            auto Copy = Loaded->IntrinsicOutputs[kWritebackBaseIndex];
            ++Copy.SSAVer;
            HighStmt Assign;
            Assign.Kind = StmtKind::Assign;
            Assign.Dst = HighExpr::makeVar(Copy);
            Assign.Val = HighExpr::makeVar(
                Loaded->IntrinsicOutputs[kWritebackBaseIndex]);
            High.Body.insert(At, std::move(Assign));
            Follow->Operands[kAddressMemoryBaseOperand] =
                HighExpr::makeVar(Copy);
          } else {
            auto Definition = std::move(*At);
            *At = HighStmt{};
            At->Kind = StmtKind::If;
            At->Cond = HighExpr::makeConst(0, 1);
            At->Body.push_back(std::move(Definition));
          }
          CEmitterOptions Options;
          Options.TheArch = Arch::AArch64;
          Options.Format = BinaryFormat::MachO;
          Options.Image = &Image;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          EXPECT_THROW(HighCEmitter().emit({High}, Out, Options),
                       std::invalid_argument);
          EXPECT_TRUE(Source.empty());
        }
      auto Unknown = Function;
      auto &UnknownOps = Unknown.Blocks.front().Ops;
      const auto Memory = std::find_if(UnknownOps.begin(), UnknownOps.end(),
                                       aarch64WritebackMemoryMedShapeIsValid);
      ASSERT_NE(Memory, UnknownOps.end());
      Memory->Inputs[kWritebackBaseOperand + 1].Provenance =
          ConstantAddressProvenance::Unknown;
      llvm::LLVMContext UnknownContext;
      EXPECT_THROW(MedLLVMEmitter().emit({Unknown}, UnknownContext, kName,
                                         Arch::AArch64, {}, &Image),
                   std::runtime_error);
      if (!Load && !Pre) {
        const auto OnePastCase =
            std::find_if(std::begin(VectorWritebackCases),
                         std::end(VectorWritebackCases), [&](const auto &C) {
                           return !C.Load && !C.Pre &&
                                  C.Access == kAddressMemoryVectorBytes &&
                                  C.Offset == kVectorWritebackOnePastOffset;
                         });
        ASSERT_NE(OnePastCase, std::end(VectorWritebackCases));
        auto OnePast = Image;
        auto &Storage = OnePast.Segments.front();
        Storage.Size = Storage.FileSz =
            kVectorWritebackBasePosition + OnePastCase->Offset;
        Storage.Data.resize(Storage.Size);
        const auto Outgoing =
            ownedVectorWritebackFunction(*OnePastCase, OnePast, Base, false);
        llvm::LLVMContext OnePastContext;
        auto OnePastModule = MedLLVMEmitter().emit(
            {Outgoing}, OnePastContext, kName, Arch::AArch64, {}, &OnePast);
        ASSERT_TRUE(OnePastModule);
        for (bool LLVMRoute : {false, true}) {
          const auto Source =
              memorySource(Outgoing, *OnePastModule, LLVMRoute, &OnePast);
#if defined(__APPLE__) && defined(__aarch64__)
          compileMemoryC(Source + llvm::formatv(kVectorWritebackOwnedHarness,
                                                Outgoing.Name,
                                                OnePastCase->Offset)
                                      .str(),
                         true);
#else
          compileMemoryC(Source, false);
#endif
        }
      }
      auto Invalid = Image;
      auto &Short = Invalid.Segments.front();
      Short.Size = Short.FileSz = Access + At->Access - 1;
      Short.Data.resize(Short.Size);
      llvm::LLVMContext Refused;
      EXPECT_THROW(MedLLVMEmitter().emit({Function}, Refused, kName,
                                         Arch::AArch64, {}, &Invalid),
                   std::runtime_error);
    }
}

TEST(AArch64VectorWritebackMemory,
     DecodedImagesKeepVectorAndCompletedBaseOccurrences) {
  for (bool Store : {false, true})
    for (bool Add : {false, true}) {
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::MachO;
      Image.MachOCPUSubtype = macho_loader::arm64e::kCPUSubtypeX1;
      Image.Base = Image.Entry = kEntry;
      std::vector<uint32_t> Words{kVectorWritebackImageADRP};
      if (Add)
        Words.push_back(kVectorWritebackImageADD);
      if (Store)
        Words.push_back(kVectorWritebackImageInitialLoad);
      Words.push_back(Store ? kVectorWritebackImageStore
                            : kVectorWritebackImageLoad);
      Words.push_back(kVectorWritebackImageMove);
      Words.push_back(kVectorWritebackImageReadback);
      Words.push_back(kVectorWritebackImageXor);
      Words.push_back(kWritebackImageReturn);
      Segment Text;
      Text.VA = kEntry;
      Text.Size = Text.FileSz = Words.size() * kWordBytes;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data.resize(Text.Size);
      for (size_t I = 0; I != Words.size(); ++I)
        writeLE<uint32_t>(Text.Data.data() + I * kWordBytes, Words[I]);
      Image.Segments.push_back(std::move(Text));
      Segment Data;
      Data.VA = kWritebackImageData;
      Data.Size = Data.FileSz = kVectorWritebackStorageBytes;
      Data.Flags = Store ? SegmentFlags::Readable | SegmentFlags::Writable
                         : SegmentFlags::Readable;
      Data.Data.resize(Data.Size);
      for (size_t I = 0; I != Data.Data.size(); ++I)
        Data.Data[I] = static_cast<uint8_t>(I * kVectorWritebackSeedMultiplier +
                                            kVectorWritebackSeedBias);
      const auto Position = Add ? kWritebackImagePosition : 0;
      const auto Expected = readLE<uint64_t>(Data.Data.data() + Position) ^
                            readLE<uint64_t>(Data.Data.data() + Position +
                                             kAddressMemoryVectorBytes);
      Image.Segments.push_back(std::move(Data));
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      const auto Low = CFGBuilder().build(Image, Decode, kEntry, kName);
      LowToMedConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto Function =
          Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
      ASSERT_TRUE(Function.Params.empty());
      ASSERT_TRUE(verifyMedFunc(Function, kName));
      ASSERT_EQ(std::count_if(Function.Blocks.front().Ops.begin(),
                              Function.Blocks.front().Ops.end(),
                              aarch64WritebackMemoryMedShapeIsValid),
                1);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Function}, Context, kName,
                                          Arch::AArch64, {}, &Image);
      ASSERT_TRUE(Module);
      ASSERT_FALSE(llvm::verifyModule(*Module));
      optimizeMemory(*Module);
      for (bool LLVMRoute : {false, true}) {
        const auto Source =
            memorySource(Function, *Module, LLVMRoute, &Image, true);
#if defined(__APPLE__) && defined(__aarch64__)
        compileMemoryC(Source + llvm::formatv(kVectorWritebackOwnedHarness,
                                              Function.Name, Expected)
                                    .str(),
                       true);
#else
        compileMemoryC(Source, false);
#endif
      }
    }
  for (uint32_t Subtype :
       {uint32_t(llvm::MachO::CPU_SUBTYPE_ARM64_ALL),
        macho_loader::arm64e::kCPUSubtype, macho_loader::arm64e::kCPUSubtypeX1})
    for (uint32_t Word : {kVectorWritebackV31Load, kVectorWritebackV31Store}) {
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::MachO;
      Image.MachOCPUSubtype = Subtype;
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      const auto Instruction = aarch64WritebackMemoryInstruction(Word);
      ASSERT_TRUE(Instruction);
      EXPECT_TRUE(Instruction->isVector());
      EXPECT_EQ(Instruction->destination(), kAddressMemoryRegisterMask);
      std::array<uint8_t, kWordBytes> Bytes;
      writeLE<uint32_t>(Bytes.data(), Word);
      DecodedInsn Decoded;
      ASSERT_EQ(
          Decode.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Decoded),
          kWordBytes);
      std::vector<LowOp> Ops;
      Decode.liftToLow(Decoded, Ops);
      EXPECT_EQ(std::any_of(Ops.begin(), Ops.end(),
                            lowAArch64WritebackMemoryShapeIsValid),
                Subtype == macho_loader::arm64e::kCPUSubtypeX1);
    }
}

TEST(AArch64VectorWritebackMemory,
     DiscardedResultsRetainOriginalMemoryEffects) {
  for (bool Load : {false, true}) {
    const auto At = std::find_if(std::begin(VectorWritebackCases),
                                 std::end(VectorWritebackCases),
                                 [&](const auto &C) { return C.Load == Load; });
    ASSERT_NE(At, std::end(VectorWritebackCases));
    auto Function = vectorWritebackFunction(*At, 0);
    auto &Ops = Function.Blocks.front().Ops;
    const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                     aarch64WritebackMemoryMedShapeIsValid);
    ASSERT_NE(Memory, Ops.end());
    const auto Return = Ops[Ops.size() - 2].Output;
    Ops.resize(Memory - Ops.begin() + 1);
    MedOp Zero;
    Zero.Opcode = NdOp::COPY;
    Zero.Output = Return;
    Zero.addInput(MedVar::makeConst(0, kScalarBytes));
    Ops.push_back(Zero);
    MedOp End;
    End.Opcode = NdOp::RETURN;
    End.addInput(Return);
    Ops.push_back(End);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    optimizeMemory(*Module);
    for (bool LLVMRoute : {false, true})
      compileMemoryC(memorySource(Function, *Module, LLVMRoute, nullptr, true),
                     false, [&](llvm::MemoryBufferRef Buffer) {
                       inspectMemoryObject(Buffer, At->Scratch);
                     });
  }
}

namespace {
#define PAIR_MEMORY_VALUE(Type, Name, Value) inline constexpr Type Name = Value;
#define PAIR_MEMORY_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64PairMemoryFixtures.def"

struct PairMemoryCase {
  uint32_t Word;
  uint32_t Scratch;
  unsigned Access;
  bool Load;
  bool Vector;
  bool Signed;
  unsigned Mode;
  int Offset;
};
constexpr PairMemoryCase PairMemoryCases[] = {
#define PAIR_MEMORY_CASE(Word, Scratch, Access, Load, Vector, Signed, Mode,    \
                         Offset)                                               \
  {Word, Scratch, Access, Load, Vector, Signed, Mode, Offset},
#include "AArch64PairMemoryFixtures.def"
};

MedFunc pairMemoryFunction(const PairMemoryCase &Case, size_t Index) {
  MedFunc Function;
  Function.Name = kPairTestNamePrefix + llvm::utohexstr(Case.Word);
  Function.Entry = kEntry + Index * kVectorFunctionStride;
  Function.ReturnType = NdType::makeInt(kScalarBytes, false);
  Function.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  for (unsigned I = 0;
       I != (Case.Load ? kPairTestLoadParameters
                       : (Case.Vector ? kPairTestVectorStoreParameters
                                      : kPairTestScalarStoreParameters));
       ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Arch::AArch64;
    Parameter.Id = I;
    Parameter.RegOff = a64reg::X0 + I * kScalarBytes;
    Parameter.Size = kScalarBytes;
    Function.Params.push_back(Parameter);
    Function.TypedParams.push_back({{}, NdType::makeInt(kScalarBytes, false)});
  }
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  int Next = Function.Params.size() + 1;
  const auto Temp = [&](unsigned Bytes) {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.TheArch = Arch::AArch64;
    Value.Id = Next++;
    Value.SSAVer = 1;
    Value.Size = Bytes;
    return Value;
  };
  const auto Emit = [&](NdOp Opcode, MedVar Output,
                        std::initializer_list<MedVar> Inputs) {
    MedOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = Function.Entry;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Block.Ops.push_back(Op);
  };
  const unsigned Bytes = Case.Vector ? kAddressMemoryVectorBytes : kScalarBytes;
  std::array<MedVar, kPairValues> Values;
  for (unsigned I = 0; I != Values.size(); ++I) {
    if (Case.Load) {
      Values[I] = Temp(Bytes);
    } else if (!Case.Vector) {
      Values[I] = Function.Params[1 + I];
    } else {
      const auto Low = Temp(Bytes), High = Temp(Bytes), Shifted = Temp(Bytes);
      Values[I] = Temp(Bytes);
      Emit(NdOp::INT_ZEXT, Low, {Function.Params[1 + I * kPairValues]});
      Emit(NdOp::INT_ZEXT, High, {Function.Params[2 + I * kPairValues]});
      Emit(NdOp::INT_LEFT, Shifted,
           {High, MedVar::makeConst(kAddressMemoryPointerBits, kWordBytes)});
      Emit(NdOp::INT_OR, Values[I], {Low, Shifted});
    }
  }
  const auto Updated = Temp(kScalarBytes);
  MedVar Primary = Updated;
  if (Case.Load) {
    Primary = MedVar{};
    Primary.Id = -1;
  }
  const auto Instruction = *aarch64WritebackMemoryInstruction(Case.Word);
  Emit(NdOp::INTRINSIC, Primary,
       {MedVar::makeConst(static_cast<uint64_t>(Instruction.id()), kIdBytes),
        Function.Params.front(), MedVar::makeConst(Case.Word, kWordBytes)});
  if (Case.Load)
    Block.Ops.back().IntrinsicOutputs = {Values[0], Values[1], Updated};
  else {
    Block.Ops.back().addInput(Values[0]);
    Block.Ops.back().addInput(Values[1]);
  }
  for (unsigned I = 0; I != kPairTestOutputWords; ++I) {
    auto Bits = Values[I / kPairValues];
    auto Value = Bits;
    if (Case.Vector) {
      Value = Temp(kScalarBytes);
      Emit(NdOp::SUBBYTES, Value,
           {Bits,
            MedVar::makeConst((I % kPairValues) * kScalarBytes, kWordBytes)});
    } else if (I % kPairValues) {
      Value = MedVar::makeConst(0, kScalarBytes);
    }
    const auto Address = Temp(kScalarBytes);
    Emit(NdOp::INT_ADD, Address,
         {Function.Params.back(),
          MedVar::makeConst(I * kScalarBytes, kScalarBytes)});
    Emit(NdOp::STORE, {}, {Address, Value});
  }
  MedVar Return;
  Return.Kind = MedVar::Reg;
  Return.TheArch = Arch::AArch64;
  Return.RegOff = a64reg::X0;
  Return.Id = 0;
  Return.SSAVer = 1;
  Return.Size = kScalarBytes;
  Emit(NdOp::COPY, Return, {Updated});
  Emit(NdOp::RETURN, {}, {Return});
  return Function;
}

void executePairMemoryFamilies(const BinaryImage *Image, bool Stack = false) {
  std::vector<MedFunc> Functions;
  std::string Declarations, Replays, Entries, Faults, FaultEntries;
  for (size_t I = 0; I != std::size(PairMemoryCases); ++I) {
    auto Case = PairMemoryCases[I];
    if (Stack) {
      Case.Word = (Case.Word & ~kStackBaseMask) | kStackBaseMask;
      Case.Scratch = (Case.Scratch & ~kStackBaseMask) | kStackBaseMask;
    }
    auto Function = pairMemoryFunction(Case, I);
    ASSERT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
    Declarations +=
        llvm::formatv(Case.Load
                          ? kPairTestLoadDeclaration
                          : (Case.Vector ? kPairTestVectorStoreDeclaration
                                         : kPairTestScalarStoreDeclaration),
                      Function.Name)
            .str();
    const auto Call =
        llvm::formatv(Case.Load ? kPairTestLoadCall
                                : (Case.Vector ? kPairTestVectorStoreCall
                                               : kPairTestScalarStoreCall),
                      Function.Name)
            .str();
    const auto *Type = Case.Vector ? kPairTestVectorType : kPairTestScalarType;
    const auto *Bank = Case.Vector ? kPairTestVectorBank : kPairTestScalarBank;
    const auto *FirstBits =
        Case.Vector ? kPairTestFirstVectorBits : kPairTestFirstScalarBits;
    const auto *SecondBits =
        Case.Vector ? kPairTestSecondVectorBits : kPairTestSecondScalarBits;
    const auto Native = [&] {
      if (Case.Load)
        return llvm::formatv(
                   kPairTestNativeLoad, I, Case.Word, Type, Bank,
                   Case.Vector ? kPairTestVectorOutputConstraint
                               : kPairTestScalarOutputConstraint,
                   FirstBits, SecondBits,
                   Stack ? kStackOracleX20Before : kEmptyNativeArgument,
                   Stack ? kStackOracleX20After : kEmptyNativeArgument,
                   Stack ? kStackOracleClobbers : kEmptyNativeArgument)
            .str();
      return llvm::formatv(kPairTestNativeStore, I, Case.Word, Type, Bank,
                           Case.Vector ? kPairTestVectorInputConstraint
                                       : kPairTestScalarInputConstraint,
                           FirstBits, SecondBits,
                           Case.Vector ? kPairTestFirstVectorInput
                                       : kPairTestFirstScalarInput,
                           Case.Vector ? kPairTestSecondVectorInput
                                       : kPairTestSecondScalarInput,
                           Stack ? kStackOracleX20Before : kEmptyNativeArgument,
                           Stack ? kStackOracleX20After : kEmptyNativeArgument,
                           Stack ? kStackOracleClobbers : kEmptyNativeArgument)
          .str();
    }();
    const bool Writeback =
        Case.Mode == kPairTestPostMode || Case.Mode == kPairTestPreMode;
    Replays +=
        Native + llvm::formatv(kPairTestReplay, I, Case.Offset,
                               int(Case.Mode != kPairTestPostMode), Case.Access,
                               int(Case.Load), int(Case.Vector),
                               int(Case.Signed), int(Writeback), Call)
                     .str();
    Entries += llvm::formatv(kPairTestEntry, I).str();
    if (Stack) {
      Faults += llvm::formatv(kStackFaultWrapper, I, Function.Name,
                              Case.Load     ? kStackFaultOutputArguments
                              : Case.Vector ? kStackFaultPairVectorArguments
                                            : kStackFaultDataArguments)
                    .str();
      FaultEntries += llvm::formatv(kStackFaultEntry, I).str();
    }
    Functions.push_back(std::move(Function));
  }
  const auto Harness =
      std::string(kPairTestPrelude) + Declarations + Replays +
      (Stack ? std::string(kStackFaultBegin) + Faults +
                   llvm::formatv(kStackFaultTable, FaultEntries).str() +
                   kStackFaultEnd
             : std::string()) +
      llvm::formatv(kPairTestTail, Entries).str();
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(Functions, Context, kName, Arch::AArch64,
                                      {}, Image);
  ASSERT_TRUE(Module);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  optimizeMemory(*Module);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  Options.Image = Image;
  std::string Directory;
  if (!Image)
    if (const char *Path = std::getenv(Stack ? kStackPairArtifactEnvironment
                                             : kPairTestArtifactEnvironment)) {
      Directory = Path;
      ::testing::Test::RecordProperty(Stack ? kStackPairArtifactProperty
                                            : kPairTestArtifactProperty,
                                      Directory);
      ASSERT_FALSE(llvm::sys::fs::create_directories(Directory));
    }
  const auto Save = [&](llvm::StringRef Name, llvm::StringRef Bytes) {
    if (Directory.empty())
      return;
    llvm::SmallString<kVectorPathStorage> Path(Directory);
    llvm::sys::path::append(Path, Name);
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error);
    Out << Bytes;
  };
  Save(kWritebackHarnessArtifact, Harness);
  for (bool LLVMRoute : {false, true}) {
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    if (LLVMRoute)
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, Options));
    else {
      std::vector<HighFunc> High;
      for (const auto &Function : Functions) {
        auto Body = MedToHighConverter().convert(Function, Arch::AArch64);
        for (auto &Parameter : Body.Params)
          Parameter.Type = NdType::makeInt(kScalarBytes, false);
        High.push_back(std::move(Body));
      }
      ASSERT_TRUE(HighCEmitter().emit(High, Output, Options));
    }
    Save(LLVMRoute ? kWritebackLLVMArtifact : kWritebackHighArtifact, Source);
#if defined(__APPLE__) && defined(__aarch64__)
    compileMemoryC(Source + Harness, true);
#else
    compileMemoryC(Source, false);
#endif
  }
  const auto Object =
      Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(Object.Success);
  if (Stack)
    Save(kImmediateMachOArtifact,
         llvm::StringRef(
             reinterpret_cast<const char *>(Object.ObjectData.data()),
             Object.ObjectData.size()));
  const llvm::MemoryBufferRef Buffer(
      llvm::StringRef(reinterpret_cast<const char *>(Object.ObjectData.data()),
                      Object.ObjectData.size()),
      kObject);
  for (const auto &Case : PairMemoryCases)
    inspectMemoryObject(Buffer, Stack ? (Case.Scratch & ~kStackBaseMask) |
                                            kStackBaseMask
                                      : Case.Scratch);
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
  compileMemoryC(Harness, true, {}, ObjectPath);
#endif
  if (!Directory.empty()) {
    const auto ELF =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::ELF);
    ASSERT_TRUE(ELF.Success);
    Save(kWritebackObjectArtifact,
         llvm::StringRef(reinterpret_cast<const char *>(ELF.ObjectData.data()),
                         ELF.ObjectData.size()));
  }
}
} // namespace

TEST(AArch64PairMemory, BothCProjectionsAndLLVMExecuteEveryFamily) {
  executePairMemoryFamilies(nullptr);
}

TEST(AArch64PairMemory, ImageContextKeepsCompleteRuntimeResults) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  executePairMemoryFamilies(&Image);
}

TEST(AArch64PairMemory, IndependentWordsAndAllImmediatesKeepOneInstruction) {
  unsigned Exhaustive = 0;
  for (const auto &Case : PairMemoryCases) {
    const auto Instruction = aarch64WritebackMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    EXPECT_TRUE(Instruction->isPair());
    EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
    EXPECT_EQ(Instruction->accessBytes(), Case.Access);
    EXPECT_EQ(Instruction->isLoad(), Case.Load);
    EXPECT_EQ(Instruction->isVector(), Case.Vector);
    EXPECT_EQ(Instruction->operation().Signed, Case.Signed);
    EXPECT_EQ(static_cast<int64_t>(Instruction->displacement()), Case.Offset);
    if (Case.Offset)
      continue;
    for (unsigned Immediate = 0; Immediate != kPairTestImmediates;
         ++Immediate) {
      const auto Word =
          (Case.Word & ~(kPairImmediateMask << kPairImmediateShift)) |
          (Immediate << kPairImmediateShift);
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Arch::AArch64));
      std::array<uint8_t, kWordBytes> Bytes;
      writeLE<uint32_t>(Bytes.data(), Word);
      DecodedInsn Decoded;
      ASSERT_EQ(Decode.decodeOne(Bytes.data(), Bytes.size(), kEntry, Decoded),
                kWordBytes);
      ASSERT_TRUE(Decoded.Raw && Decoded.Raw->detail);
      const auto &Detail = Decoded.Raw->detail->aarch64;
      ASSERT_GE(Detail.op_count, 3);
      ASSERT_EQ(Detail.operands[2].type, AARCH64_OP_MEM);
      const auto Offset = Detail.post_index && Detail.op_count >= 4 &&
                                  Detail.operands[3].type == AARCH64_OP_IMM
                              ? Detail.operands[3].imm
                              : Detail.operands[2].mem.disp;
      const auto Owned = aarch64WritebackMemoryInstruction(Word);
      ASSERT_TRUE(Owned);
      EXPECT_EQ(static_cast<int64_t>(Owned->displacement()), Offset);
      EXPECT_EQ(Owned->hasWriteback(), bool(Decoded.Raw->detail->writeback));
      EXPECT_EQ(Case.Mode == kPairTestPostMode, Detail.post_index);
      for (bool Fast : {false, true}) {
        const auto Ops = liftMemory(Word, Fast);
        const auto At = std::find_if(Ops.begin(), Ops.end(),
                                     lowAArch64WritebackMemoryShapeIsValid);
        ASSERT_NE(At, Ops.end());
        EXPECT_EQ(At->Inputs[kWritebackWordOperand + 1].Offset, Word);
        EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(*At));
        EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
                 Op.Opcode == NdOp::INT_ADD;
        }));
        auto Function = lowFunction(*At);
        auto &Block = Function.Blocks.front();
        Block.Ops = Ops;
        Block.InstructionBoundaries.front().OpCount = Ops.size();
        auto Error = validateLowInstructionBoundaries(Function);
        EXPECT_FALSE(bool(Error));
        llvm::consumeError(std::move(Error));
        const auto Med = LowToMedConverter().convert(Function, Arch::AArch64);
        EXPECT_TRUE(std::any_of(Med.Blocks.front().Ops.begin(),
                                Med.Blocks.front().Ops.end(),
                                aarch64WritebackMemoryMedShapeIsValid));
      }
      ++Exhaustive;
    }
  }
  EXPECT_EQ(Exhaustive, (std::size(PairMemoryCases) / kPairTestSamples) *
                            kPairTestImmediates);
}

TEST(AArch64PairMemory, MalformedResultsAndSecondOperandsFailBeforeFiltering) {
  for (bool Vector : {false, true})
    for (bool Load : {false, true}) {
      const auto At =
          std::find_if(std::begin(PairMemoryCases), std::end(PairMemoryCases),
                       [&](const auto &Case) {
                         return Case.Vector == Vector && Case.Load == Load;
                       });
      ASSERT_NE(At, std::end(PairMemoryCases));
      const auto Function = pairMemoryFunction(*At, 0);
      const auto &Ops = Function.Blocks.front().Ops;
      const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                       aarch64WritebackMemoryMedShapeIsValid);
      ASSERT_NE(Memory, Ops.end());
      const auto Index = Memory - Ops.begin();
      std::vector<std::function<void(MedOp &)>> Mutations = {
          [](MedOp &Op) { Op.NumInputs--; },
          [](MedOp &Op) { Op.Output.Size = kScalarBytes + 1; },
          [](MedOp &Op) { Op.Inputs[kWritebackBaseOperand + 1].Size--; },
          [](MedOp &Op) {
            Op.Inputs[kWritebackWordOperand + 1] = Op.Inputs[1];
          },
          [](MedOp &Op) { Op.MemoryOrdering = NdMemoryOrdering::Acquire; },
          [](MedOp &Op) {
            Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
          },
      };
      if (Load) {
        Mutations.push_back([](MedOp &Op) { Op.IntrinsicOutputs.pop_back(); });
        Mutations.push_back([](MedOp &Op) { Op.IntrinsicOutputs[1].Size--; });
        Mutations.push_back(
            [](MedOp &Op) { Op.IntrinsicOutputs[1] = Op.IntrinsicOutputs[0]; });
        Mutations.push_back([](MedOp &Op) {
          Op.IntrinsicOutputs[1] = Op.IntrinsicOutputs[kPairBaseIndex];
        });
        Mutations.push_back([](MedOp &Op) {
          Op.IntrinsicOutputs[0] = Op.IntrinsicOutputs[kPairBaseIndex];
        });
        Mutations.push_back(
            [](MedOp &Op) { Op.IntrinsicOutputs[1].Kind = MedVar::Const; });
        Mutations.push_back(
            [](MedOp &Op) { Op.IntrinsicOutputs[1].TheArch = Arch::X64; });
      } else {
        Mutations.push_back(
            [](MedOp &Op) { Op.Inputs[kPairSecondDataOperand + 1].Size--; });
      }
      for (const auto &Mutate : Mutations) {
        auto Invalid = Function;
        auto &Bad = Invalid.Blocks.front().Ops[Index];
        Mutate(Bad);
        EXPECT_FALSE(aarch64WritebackMemoryMedShapeIsValid(Bad));
        for (bool Hidden : {false, true}) {
          llvm::LLVMContext Context;
          const std::vector<char> Mask{0};
          EXPECT_THROW(MedLLVMEmitter().emit({Invalid}, Context, kName,
                                             Arch::AArch64, {}, nullptr,
                                             BinaryFormat::ELF, false,
                                             Hidden ? &Mask : nullptr),
                       std::runtime_error);
        }
      }
      for (bool Hidden : {false, true}) {
        auto Invalid = MedToHighConverter().convert(Function, Arch::AArch64);
        walkStmts(Invalid.Body, [&](const HighStmt &Statement) {
          forEachExpr(Statement, [&](const ExprPtr &Expression) {
            if (Expression &&
                Expression->IntrinsicId ==
                    static_cast<Intrinsic>(Memory->Inputs[0].ConstVal)) {
              if (Load)
                Expression->IntrinsicOutputs[1] =
                    Expression->IntrinsicOutputs[0];
              else
                Expression->Operands[kPairSecondDataOperand]->Type =
                    NdType::makeInt(kScalarBytes - 1, false);
            }
          });
        });
        if (Hidden) {
          HighStmt If;
          If.Kind = StmtKind::If;
          If.Cond = HighExpr::makeConst(0, 1);
          If.Body = std::move(Invalid.Body);
          Invalid.Body = {std::move(If)};
        }
        CEmitterOptions Options;
        Options.TheArch = Arch::AArch64;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        EXPECT_THROW(HighCEmitter().emit({Invalid}, Out, Options),
                     std::invalid_argument);
        EXPECT_TRUE(Source.empty());
      }
      auto Stack = Function;
      Stack.Blocks.front()
          .Ops[Index]
          .Inputs[kWritebackWordOperand + 1]
          .ConstVal |= kAddressMemoryRegisterMask << kAddressMemoryBaseShift;
      ASSERT_TRUE(aarch64WritebackMemoryMedShapeIsValid(
          Stack.Blocks.front().Ops[Index]));
      Stack.Blocks.front().Ops[Index].Inputs[kWritebackBaseOperand + 1].Kind =
          MedVar::Temp;
      llvm::LLVMContext StackContext;
      EXPECT_THROW(
          MedLLVMEmitter().emit({Stack}, StackContext, kName, Arch::AArch64),
          std::runtime_error);
      if (!Load)
        continue;
      const auto Operations = liftMemory(At->Word, false);
      const auto LowMemory =
          std::find_if(Operations.begin(), Operations.end(),
                       lowAArch64WritebackMemoryShapeIsValid);
      ASSERT_NE(LowMemory, Operations.end());
      for (unsigned Mutation = 0; Mutation != kPairTestLowMutations;
           ++Mutation) {
        auto Low = lowFunction(*LowMemory);
        auto &Block = Low.Blocks.front();
        Block.Ops = Operations;
        auto &First = Block.Ops[LowMemory - Operations.begin() + 1];
        if (Mutation == 0)
          Block.Ops.pop_back();
        else if (Mutation == 1)
          First.Inputs[0].Size--;
        else {
          const auto Bindings = lowAArch64WritebackOutputs(
              *LowMemory, llvm::ArrayRef(Operations)
                              .drop_front(LowMemory - Operations.begin() + 1));
          ASSERT_TRUE(Bindings);
          const unsigned Target = Mutation == 2 ? 1 : kPairBaseIndex;
          const unsigned Source = Mutation == 4 ? 1 : 0;
          for (auto &Op : Block.Ops)
            if (Op.NumInputs && Op.Inputs[0] == (*Bindings)[Target])
              Op.Inputs[0].Offset = (*Bindings)[Source].Offset;
        }
        Block.InstructionBoundaries.front().OpCount = Block.Ops.size();
        auto Error = validateLowInstructionBoundaries(Low);
        EXPECT_TRUE(bool(Error));
        llvm::consumeError(std::move(Error));
        EXPECT_THROW(LowToMedConverter().convert(Low, Arch::AArch64),
                     LowToMedConversionError);
      }
    }
}

TEST(AArch64PairMemory, TracerKeepsBothValuesBaseAndCompleteMemory) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = kPairTestStorageBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  for (size_t I = 0; I != Data.Data.size(); ++I)
    Data.Data[I] =
        static_cast<uint8_t>(I * kPairTestSeedMultiplier + kPairTestSeedBias);
  Image.Segments.push_back(Data);
  const std::array<std::array<uint64_t, kPairValues>, kPairValues> Seeds{
      {{kPairTestFirstLow, kPairTestFirstHigh},
       {kPairTestSecondLow, kPairTestSecondHigh}}};
  for (const auto &Case : PairMemoryCases)
    for (bool Strict : {false, true}) {
      const auto Instruction = *aarch64WritebackMemoryInstruction(Case.Word);
      const auto Ops = liftMemory(Case.Word, false);
      const uint64_t Base = kEntry + kPairTestBasePosition;
      const uint64_t Address =
          Base + (Case.Mode != kPairTestPostMode ? Case.Offset : 0);
      const unsigned ValueBytes =
          Case.Vector ? kAddressMemoryVectorBytes : kScalarBytes;
      const auto BaseReg = a64reg::X0 + Instruction.base() * kScalarBytes;
      const std::array<uint64_t, kPairValues> Registers{
          Case.Vector ? a64reg::V(Instruction.destination())
                      : a64reg::X0 + Instruction.destination() * kScalarBytes,
          Case.Vector
              ? a64reg::V(Instruction.secondDestination())
              : a64reg::X0 + Instruction.secondDestination() * kScalarBytes};
      std::array<std::vector<uint8_t>, kPairValues> Initial;
      for (unsigned I = 0; I != kPairValues; ++I) {
        Initial[I].resize(ValueBytes);
        for (unsigned J = 0; J != ValueBytes / kScalarBytes; ++J)
          writeLE<uint64_t>(Initial[I].data() + J * kScalarBytes, Seeds[I][J]);
      }
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setRegister(BaseReg, Base);
      for (unsigned I = 0; I != kPairValues; ++I)
        Emulator.setRegisterBytes(Registers[I], Initial[I]);
      auto Expected = Data.Data;
      auto Values = Initial;
      for (unsigned I = 0; I != kPairValues; ++I) {
        const auto At = Address - Data.VA + I * Case.Access;
        if (Case.Load) {
          Values[I].assign(ValueBytes,
                           Case.Signed && (Data.Data[At + Case.Access - 1] &
                                           kWritebackByteSignBit)
                               ? kWritebackByteMask
                               : 0);
          std::copy_n(Data.Data.begin() + At, Case.Access, Values[I].begin());
        } else
          std::copy_n(Initial[I].begin(), Case.Access, Expected.begin() + At);
      }
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegister(BaseReg),
                Base + (Instruction.hasWriteback() ? Case.Offset : 0));
      for (unsigned I = 0; I != kPairValues; ++I) {
        auto Actual = Emulator.getRegisterBytes(Registers[I]);
        ASSERT_TRUE(Actual);
        EXPECT_TRUE(
            std::equal(Values[I].begin(), Values[I].end(), Actual->begin()));
      }
      for (size_t Byte = 0; Byte != Expected.size();
           Byte += kAddressMemoryVectorBytes) {
        LowOp Read;
        Read.Opcode = NdOp::LOAD;
        Read.Output = NdVar::reg(a64reg::V0, kAddressMemoryVectorBytes);
        Read.addInput(NdVar::scalar(Data.VA + Byte, kScalarBytes));
        ASSERT_TRUE(Emulator.step(Read));
        const auto Actual = Emulator.getRegisterBytes(a64reg::V0);
        ASSERT_TRUE(Actual);
        EXPECT_TRUE(std::equal(Actual->begin(), Actual->end(),
                               Expected.begin() + Byte));
      }
      auto Short = Image;
      auto &Storage = Short.Segments.front();
      Storage.Size = Storage.FileSz =
          Address - Storage.VA + Instruction.totalAccessBytes() - 1;
      Storage.Data.resize(Storage.Size);
      NdOpEmulator Refused(Short);
      Refused.setStrictMode(Strict);
      Refused.setRegister(BaseReg, Base);
      for (unsigned I = 0; I != kPairValues; ++I)
        Refused.setRegisterBytes(Registers[I], Initial[I]);
      EXPECT_EQ(Refused.run(Ops), 0u);
      EXPECT_EQ(Refused.getRegister(BaseReg), Base);
      for (unsigned I = 0; I != kPairValues; ++I) {
        auto Actual = Refused.getRegisterBytes(Registers[I]);
        ASSERT_TRUE(Actual);
        EXPECT_TRUE(
            std::equal(Initial[I].begin(), Initial[I].end(), Actual->begin()));
      }
      if (Case.Load) {
        NdOpEmulator Naked(Image);
        Naked.setRegister(BaseReg, Base);
        EXPECT_FALSE(Naked.step(Ops.front()));
        auto Missing = Ops;
        Missing.pop_back();
        EXPECT_EQ(Naked.run(Missing), 0u);
      }
    }
}

TEST(AArch64PairMemory, RegisterRolesRejectAmbiguityAndKeepLegalAliases) {
  for (const auto &Case : PairMemoryCases) {
    const auto Instruction = *aarch64WritebackMemoryInstruction(Case.Word);
    const auto WithBase = [&](unsigned Base) {
      return (Case.Word &
              ~(kAddressMemoryRegisterMask << kAddressMemoryBaseShift)) |
             (Base << kAddressMemoryBaseShift);
    };
    const auto WithSecond = [&](unsigned Second) {
      return (Case.Word & ~(kAddressMemoryRegisterMask << kPairSecondShift)) |
             (Second << kPairSecondShift);
    };
    if (!Case.Offset)
      for (uint32_t Subtype : {uint32_t(llvm::MachO::CPU_SUBTYPE_ARM64_ALL),
                               macho_loader::arm64e::kCPUSubtype,
                               macho_loader::arm64e::kCPUSubtypeX1})
        for (bool Fast : {false, true}) {
          BinaryImage Image;
          Image.Arch = Arch::AArch64;
          Image.Bits = Bitness::Bits64;
          Image.Format = BinaryFormat::MachO;
          Image.MachOCPUSubtype = Subtype;
          Decoder Decode;
          ASSERT_TRUE(Decode.init(Image));
          std::array<uint8_t, kWordBytes> Bytes;
          writeLE<uint32_t>(Bytes.data(), Case.Word);
          DecodedInsn Decoded;
          ASSERT_EQ(Fast ? Decode.decodeOneForLift(Bytes.data(), Bytes.size(),
                                                   kEntry, Decoded)
                         : Decode.decodeOne(Bytes.data(), Bytes.size(), kEntry,
                                            Decoded),
                    kWordBytes);
          std::vector<LowOp> Ops;
          Decode.liftToLow(Decoded, Ops);
          EXPECT_EQ(std::any_of(Ops.begin(), Ops.end(),
                                lowAArch64WritebackMemoryShapeIsValid),
                    Subtype == macho_loader::arm64e::kCPUSubtypeX1);
        }
    if (Case.Load) {
      for (bool Fast : {false, true}) {
        Decoder Decode;
        ASSERT_TRUE(Decode.init(Arch::AArch64));
        std::array<uint8_t, kWordBytes> Bytes;
        writeLE<uint32_t>(Bytes.data(), Case.Word);
        DecodedInsn Decoded;
        ASSERT_EQ(Fast ? Decode.decodeOneForLift(Bytes.data(), Bytes.size(),
                                                 kEntry, Decoded)
                       : Decode.decodeOne(Bytes.data(), Bytes.size(), kEntry,
                                          Decoded),
                  kWordBytes);
        ASSERT_TRUE(Decoded.Raw && Decoded.Raw->detail);
        // Exercise the public lifter boundary independently of a decoder's
        // illegal/soft-fail marker. It must not fall through to legacy pairs.
        auto Raw = *Decoded.Raw;
        writeLE<uint32_t>(Raw.bytes, WithSecond(Instruction.destination()));
        Raw.illegal = false;
        for (bool Strict : {false, true}) {
          AArch64Lifter Lifter(Arch::AArch64);
          Lifter.setStrict(Strict);
          std::vector<LowOp> Ops;
          EXPECT_THROW(Lifter.lift(&Raw, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
      }
    }
    EXPECT_EQ(bool(aarch64WritebackMemoryInstruction(
                  WithSecond(Instruction.destination()))),
              !Case.Load);
    for (unsigned Base :
         {Instruction.destination(), Instruction.secondDestination()})
      EXPECT_EQ(bool(aarch64WritebackMemoryInstruction(WithBase(Base))),
                Case.Vector || !Instruction.hasWriteback());
    EXPECT_TRUE(aarch64WritebackMemoryInstruction(
        WithBase(kAddressMemoryRegisterMask)));
    EXPECT_TRUE(aarch64WritebackMemoryInstruction(
        WithSecond(kAddressMemoryRegisterMask)));
    if (Case.Vector || Instruction.hasWriteback() || !Case.Load)
      continue;
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Segment Data;
    Data.VA = kEntry;
    Data.Size = Data.FileSz = kPairTestStorageBytes;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.resize(Data.Size, kPairTestSeedBias);
    Image.Segments.push_back(Data);
    for (unsigned Base :
         {Instruction.destination(), Instruction.secondDestination()}) {
      const auto Word = WithBase(Base);
      const auto Ops = liftMemory(Word, false);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      const auto Reg = a64reg::X0 + Base * kScalarBytes;
      Emulator.setRegister(Reg, kEntry + kPairTestBasePosition);
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      uint64_t Expected = 0;
      for (unsigned Byte = 0; Byte != Case.Access; ++Byte)
        Expected |= uint64_t(kPairTestSeedBias) << (Byte * CHAR_BIT);
      EXPECT_EQ(Emulator.getRegister(Reg), Expected);
    }
  }
}

TEST(AArch64PairMemory, LLVMAdmissionChecksBothValuesBeforeFunctionSelection) {
  for (bool Vector : {false, true})
    for (bool Load : {false, true}) {
      const auto At =
          std::find_if(std::begin(PairMemoryCases), std::end(PairMemoryCases),
                       [&](const auto &Case) {
                         return Case.Vector == Vector && Case.Load == Load;
                       });
      ASSERT_NE(At, std::end(PairMemoryCases));
      for (unsigned Mutation = 0; Mutation != kVectorWritebackCallMutations + 1;
           ++Mutation) {
        llvm::LLVMContext Context;
        const auto Function = pairMemoryFunction(*At, 0);
        auto Module =
            MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
        ASSERT_TRUE(Module);
        llvm::CallInst *Owned = nullptr;
        for (auto &Block : *Module->getFunction(Function.Name))
          for (auto &Operation : Block)
            if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Operation))
              if (validateAArch64WritebackMemoryCall(Arch::AArch64, *Call))
                Owned = Call;
        ASSERT_TRUE(Owned);
        EXPECT_THROW(validateAArch64WritebackMemoryCall(Arch::X64, *Owned),
                     std::invalid_argument);
        const auto *Asm =
            llvm::cast<llvm::InlineAsm>(Owned->getCalledOperand());
        const auto Last = Owned->arg_size() - 1;
        const auto Word =
            llvm::cast<llvm::ConstantInt>(Owned->getArgOperand(Last))
                ->getZExtValue();
        if (Mutation == 0 || Mutation == 1 || Mutation == 2 ||
            Mutation == kVectorWritebackCallMutations) {
          uint64_t Changed = Word;
          if (Mutation == 0)
            Changed ^= kVectorWritebackVectorBit;
          else if (Mutation == 1)
            Changed ^= kVectorWritebackLoadBit;
          else if (Mutation == 2)
            Changed |= kAddressMemoryRegisterMask << kAddressMemoryBaseShift;
          else
            Changed ^= kAddressMemoryRegisterMask << kPairSecondShift;
          Owned->setArgOperand(
              Last, llvm::ConstantInt::get(
                        Owned->getArgOperand(Last)->getType(), Changed));
        } else if (Mutation == 3)
          Owned->setCalledOperand(llvm::InlineAsm::get(
              Asm->getFunctionType(), Asm->getAsmString(),
              writebackConstraintsWithoutMemoryClobber(*Asm), true));
        else if (Mutation == 4)
          Owned->setCalledOperand(
              llvm::InlineAsm::get(Asm->getFunctionType(), Asm->getAsmString(),
                                   Asm->getConstraintString(), false));
        else if (Mutation == 5)
          Owned->setMemoryEffects(llvm::MemoryEffects::none());
        else if (Mutation == 6)
          Owned->setCallingConv(llvm::CallingConv::Fast);
        else if (Mutation == 7)
          Owned->addFnAttr(llvm::Attribute::Speculatable);
        else if (Mutation == 11)
          Owned->setDoesNotReturn();
        else
          Owned->setCalledOperand(llvm::InlineAsm::get(
              Asm->getFunctionType(), Asm->getAsmString(),
              Asm->getConstraintString(), true, Mutation == 8,
              Mutation == 9 ? llvm::InlineAsm::AD_Intel
                            : llvm::InlineAsm::AD_ATT,
              Mutation == 10));
        EXPECT_THROW(validateAArch64WritebackMemoryCall(Arch::AArch64, *Owned),
                     std::invalid_argument);
        auto *Other = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
            llvm::GlobalValue::ExternalLinkage, kVectorWritebackHiddenFunction,
            *Module);
        auto *Entry = llvm::BasicBlock::Create(Context, {}, Other);
        llvm::IRBuilder<> Builder(Entry);
        Builder.CreateRetVoid();
        for (bool Selected : {false, true}) {
          CEmitterOptions Options;
          Options.TheArch = Arch::AArch64;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          EXPECT_THROW(LLVMCEmitter().emit(*Module, Out, Options, nullptr,
                                           nullptr, Selected ? Other : nullptr),
                       std::invalid_argument);
          EXPECT_TRUE(Source.empty());
        }
      }
    }
}
namespace {
MedFunc ownedPairMemoryFunction(const PairMemoryCase &Case,
                                const BinaryImage &Image, va_t Base,
                                bool Follow) {
  auto Function = pairMemoryFunction(Case, 0);
  std::vector<MedVar> Replacements{MedVar::makeConst(
      Base, kScalarBytes, ConstantAddressProvenance::DataAddress)};
  if (!Case.Load) {
    Replacements.push_back(MedVar::makeConst(kPairTestFirstLow, kScalarBytes));
    if (Case.Vector)
      Replacements.push_back(
          MedVar::makeConst(kPairTestFirstHigh, kScalarBytes));
    Replacements.push_back(MedVar::makeConst(kPairTestSecondLow, kScalarBytes));
    if (Case.Vector)
      Replacements.push_back(
          MedVar::makeConst(kPairTestSecondHigh, kScalarBytes));
  }
  Replacements.push_back(
      MedVar::makeConst(Image.Segments.front().VA, kScalarBytes,
                        ConstantAddressProvenance::DataAddress));
  auto &Ops = Function.Blocks.front().Ops;
  for (auto &Op : Ops)
    for (unsigned I = 0; I != Op.NumInputs; ++I)
      if (Op.Inputs[I].Kind == MedVar::Param)
        Op.Inputs[I] = Replacements[Op.Inputs[I].Id];
  Function.Params.clear();
  Function.TypedParams.clear();
  Function.CC = CallingConv::ARM_AAPCS;
  const auto Updated = Ops[Ops.size() - 2].Inputs.front();
  const auto Return = Ops[Ops.size() - 2].Output;
  std::vector<MedVar> Words;
  int Next = 0;
  for (const auto &Op : Ops) {
    if (Op.Opcode == NdOp::STORE)
      Words.push_back(Op.Inputs[1]);
    Next = std::max(Next, Op.Output.Id + 1);
    for (const auto &Value : Op.IntrinsicOutputs)
      Next = std::max(Next, Value.Id + 1);
  }
  const auto Temp = [&] {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.TheArch = Arch::AArch64;
    Value.Id = Next++;
    Value.SSAVer = 1;
    Value.Size = kScalarBytes;
    return Value;
  };
  Ops.resize(Ops.size() - 2);
  const auto Emit = [&](NdOp Opcode, MedVar Output,
                        std::initializer_list<MedVar> Inputs) {
    MedOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = Function.Entry;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Ops.push_back(Op);
  };
  auto Result = Temp();
  Emit(NdOp::INT_SUB, Result, {Updated, Replacements.front()});
  if (Follow) {
    for (const auto &Word : Words) {
      const auto Next = Temp();
      Emit(NdOp::INT_XOR, Next, {Result, Word});
      Result = Next;
    }
    const auto Read = Temp();
    Emit(NdOp::INTRINSIC, Read,
         {MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_LoadImmediate),
                            kIdBytes),
          Updated, MedVar::makeConst(0, kScalarBytes),
          MedVar::makeConst(kVectorWritebackFollowLoad, kWordBytes)});
    const auto Next = Temp();
    Emit(NdOp::INT_XOR, Next, {Result, Read});
    Result = Next;
  }
  Emit(NdOp::COPY, Return, {Result});
  Emit(NdOp::RETURN, {}, {Return});
  return Function;
}
} // namespace

TEST(AArch64PairMemory, OwnedImagesValidateBothAccessesAndReturnedBases) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Segment Data;
  Data.VA = kStorageBase;
  Data.Size = Data.FileSz = kPairTestStorageBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  for (size_t I = 0; I != Data.Data.size(); ++I)
    Data.Data[I] =
        static_cast<uint8_t>(I * kPairTestSeedMultiplier + kPairTestSeedBias);
  Image.Segments.push_back(Data);
  const auto Base = Data.VA + kPairTestBasePosition;
  for (const auto &Case : PairMemoryCases) {
    const auto Instruction = *aarch64WritebackMemoryInstruction(Case.Word);
    const auto Span = aarch64ImmediateMemoryImageSpan(
        Image, Instruction, Base, ConstantAddressProvenance::DataAddress,
        InvalidVA);
    ASSERT_TRUE(Span);
    const auto Access =
        Base + (Case.Mode != kPairTestPostMode ? Case.Offset : 0);
    const auto Updated = Base + (Instruction.hasWriteback() ? Case.Offset : 0);
    EXPECT_LE(Span->Begin, std::min({Base, Access, Updated}));
    EXPECT_GE(
        Span->Begin + Span->Bytes,
        std::max({Base, Access + Instruction.totalAccessBytes(), Updated}));
    EXPECT_FALSE(aarch64ImmediateMemoryImageSpan(
        Image, Instruction, Base, ConstantAddressProvenance::Unknown,
        InvalidVA));
    auto Short = Image;
    auto &Storage = Short.Segments.front();
    Storage.Size = Storage.FileSz =
        Access - Data.VA + Instruction.totalAccessBytes() - 1;
    Storage.Data.resize(Storage.Size);
    EXPECT_FALSE(aarch64ImmediateMemoryImageSpan(
        Short, Instruction, Base, ConstantAddressProvenance::DataAddress,
        InvalidVA));
    if (Case.Offset >= 0)
      EXPECT_FALSE(Instruction.controlIndependentSpan(
          std::numeric_limits<uint64_t>::max(), 0));
    if (Case.Offset > 0) {
      const auto Boundary = kAddressMemorySignBit - uint64_t(Case.Offset);
      EXPECT_FALSE(Instruction.controlIndependentSpan(Boundary, 0));
    }
    if (Case.Offset != int(Case.Access) ||
        Case.Access != (Case.Vector ? kAddressMemoryVectorBytes : kScalarBytes))
      continue;
    const auto Function = ownedPairMemoryFunction(Case, Image, Base, true);
    auto Expected = Data.Data;
    uint64_t Result = Updated - Base;
    for (unsigned I = 0; I != kPairValues; ++I) {
      std::array<uint8_t, kAddressMemoryVectorBytes> Value{};
      const auto At = Access - Data.VA + I * Case.Access;
      if (Case.Load)
        std::copy_n(Expected.begin() + At, Case.Access, Value.begin());
      else {
        writeLE<uint64_t>(Value.data(),
                          I ? kPairTestSecondLow : kPairTestFirstLow);
        if (Case.Vector)
          writeLE<uint64_t>(Value.data() + kScalarBytes,
                            I ? kPairTestSecondHigh : kPairTestFirstHigh);
        std::copy_n(Value.begin(), Case.Access, Expected.begin() + At);
      }
      Result ^= readLE<uint64_t>(Value.data()) ^
                readLE<uint64_t>(Value.data() + kScalarBytes);
    }
    Result ^= readLE<uint64_t>(Expected.data() + Updated - Data.VA);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Function}, Context, kName,
                                        Arch::AArch64, {}, &Image);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    for (bool LLVMRoute : {false, true}) {
      const auto Source = memorySource(Function, *Module, LLVMRoute, &Image);
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source + llvm::formatv(kVectorWritebackOwnedHarness,
                                            Function.Name, Result)
                                  .str(),
                     true);
#else
      compileMemoryC(Source, false);
#endif
    }
    llvm::LLVMContext ShortContext;
    EXPECT_THROW(MedLLVMEmitter().emit({Function}, ShortContext, kName,
                                       Arch::AArch64, {}, &Short),
                 std::runtime_error);
    auto Unknown = Function;
    auto &Ops = Unknown.Blocks.front().Ops;
    const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                     aarch64WritebackMemoryMedShapeIsValid);
    ASSERT_NE(Memory, Ops.end());
    Memory->Inputs[kWritebackBaseOperand + 1].Provenance =
        ConstantAddressProvenance::Unknown;
    llvm::LLVMContext UnknownContext;
    EXPECT_THROW(MedLLVMEmitter().emit({Unknown}, UnknownContext, kName,
                                       Arch::AArch64, {}, &Image),
                 std::runtime_error);
  }
}

TEST(AArch64PairMemory, DiscardedResultsRetainPairEffects) {
  for (bool Vector : {false, true})
    for (bool Load : {false, true}) {
      const auto At =
          std::find_if(std::begin(PairMemoryCases), std::end(PairMemoryCases),
                       [&](const auto &Case) {
                         return Case.Vector == Vector && Case.Load == Load;
                       });
      ASSERT_NE(At, std::end(PairMemoryCases));
      auto Function = pairMemoryFunction(*At, 0);
      auto &Ops = Function.Blocks.front().Ops;
      const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                       aarch64WritebackMemoryMedShapeIsValid);
      ASSERT_NE(Memory, Ops.end());
      const auto Return = Ops[Ops.size() - 2].Output;
      Ops.resize(Memory - Ops.begin() + 1);
      MedOp Zero;
      Zero.Opcode = NdOp::COPY;
      Zero.Output = Return;
      Zero.addInput(MedVar::makeConst(0, kScalarBytes));
      Ops.push_back(Zero);
      MedOp End;
      End.Opcode = NdOp::RETURN;
      End.addInput(Return);
      Ops.push_back(End);
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
      ASSERT_TRUE(Module);
      optimizeMemory(*Module);
      for (bool LLVMRoute : {false, true})
        compileMemoryC(
            memorySource(Function, *Module, LLVMRoute, nullptr, true), false,
            [&](llvm::MemoryBufferRef Buffer) {
              inspectMemoryObject(Buffer, At->Scratch);
            });
    }
}

TEST(AArch64PairMemory, NominalFrameFootprintsCoverBothTransfers) {
  for (const auto &Case : PairMemoryCases) {
    const auto Instruction = *aarch64WritebackMemoryInstruction(Case.Word);
    const auto Operations = liftMemory(Case.Word, false);
    ASSERT_FALSE(Operations.empty());
    auto Function = lowFunction(Operations.front());
    auto &Block = Function.Blocks.front();
    Block.Ops.clear();
    Block.InstructionBoundaries.clear();
    LowOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Addr = kEntry;
    Copy.Output = NdVar::reg(a64reg::X0 + Instruction.base() * kScalarBytes,
                             kScalarBytes);
    Copy.addInput(NdVar::reg(a64reg::SP, kScalarBytes));
    Block.Ops.push_back(Copy);
    for (auto Op : Operations) {
      Op.Addr = kEntry + kWordBytes;
      Op.Seq = 1;
      Block.Ops.push_back(Op);
    }
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = kEntry + 2 * kWordBytes;
    Block.Ops.push_back(Return);
    Block.EndAddr = Return.Addr + kWordBytes;
    const auto Frame = analyzeSourceStackFrame(Function, Arch::AArch64);
    ASSERT_TRUE(Frame.Error.empty()) << Frame.Error;
    ASSERT_EQ(Frame.Accesses.size(), 1u);
    EXPECT_EQ(Frame.Accesses.front().Bytes, Instruction.totalAccessBytes());
    EXPECT_EQ(Frame.Accesses.front().Offset,
              Case.Mode != kPairTestPostMode ? Case.Offset : 0);
    EXPECT_EQ(Frame.Accesses.front().Write, !Case.Load);
    EXPECT_EQ(Frame.Accesses.front().OriginalWord, Case.Word);
  }
}

namespace {
#define ORDERED_MEMORY_VALUE(Type, Name, Value)                                \
  inline constexpr Type Name = Value;
#define ORDERED_MEMORY_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64OrderedMemoryFixtures.def"

constexpr ImmediateMemoryCase OrderedMemoryCases[] = {
#define ORDERED_MEMORY_CASE(Word, Scratch, Vector, Access, Result, Signed,     \
                            Offset)                                            \
  {Word, Scratch, Vector, false, Access, Result, Signed, Offset},
#include "AArch64OrderedMemoryFixtures.def"
};
constexpr uint32_t UnallocatedOrderedWords[] = {
#define ORDERED_MEMORY_REJECT(Word) Word,
#include "AArch64OrderedMemoryFixtures.def"
};

std::vector<ImmediateMemoryCase> orderedRegisterCases(bool Vector) {
  std::vector<ImmediateMemoryCase> Result;
  for (const auto &Case : OrderedMemoryCases)
    if (Case.Vector == Vector)
      Result.push_back(Case);
  return Result;
}

bool nativeOrderedMemorySupported(bool Vector) {
#if defined(__APPLE__) && defined(__aarch64__)
  int Supported = 0;
  size_t Size = sizeof(Supported);
  return sysctlbyname(Vector ? kOrderedRCPC3Feature : kOrderedRCPC2Feature,
                      &Supported, &Size, nullptr, 0) == 0 &&
         Size == sizeof(Supported) && Supported;
#else
  return false;
#endif
}
} // namespace

TEST(AArch64OrderedMemory, IndependentWordsAndEveryImmediateKeepOneOperation) {
  ASSERT_EQ(std::size(OrderedMemoryCases), kOrderedCaseCount);
  unsigned Families = 0, Encodings = 0;
  for (const auto &Case : OrderedMemoryCases) {
    const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    EXPECT_TRUE(isAArch64OrderedUnscaledMemoryEncoding(Case.Word));
    EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
    EXPECT_EQ(Instruction->accessBytes(), Case.Access);
    EXPECT_EQ(Instruction->isVector(), Case.Vector);
    EXPECT_EQ(Instruction->operation().ResultBytes, Case.Result);
    EXPECT_EQ(Instruction->operation().Signed, Case.Signed);
    EXPECT_EQ(Instruction->addressOffset(0), uint64_t(Case.Offset));
    if (Case.Offset)
      continue;
    ++Families;
    for (unsigned I = 0; I != kImmediateSignedSlots; ++I) {
      const uint32_t Word = Case.Word | (I << kImmediateSignedShift);
      const int64_t Offset = I < kImmediateSignedSlots / 2
                                 ? int64_t(I)
                                 : int64_t(I) - kImmediateSignedSlots;
      const auto Owned = aarch64AddressMemoryInstruction(Word);
      ASSERT_TRUE(Owned);
      EXPECT_EQ(Owned->addressOffset(0), uint64_t(Offset));
      for (bool Fast : {false, true}) {
        const auto Ops = liftMemory(Word, Fast);
        const auto Memory = originalMemoryOperation(Ops);
        ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
        EXPECT_EQ(Memory.Inputs[kAddressMemoryWordOperand + 1].Offset, Word);
        EXPECT_EQ(std::count_if(Ops.begin(), Ops.end(),
                                lowAArch64AddressMemoryShapeIsValid),
                  1);
        EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Memory));
        EXPECT_FALSE(lowIntrinsicMemoryOperands(Memory).Complete);
        EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::LOAD ||
                 Op.Opcode == NdOp::STORE;
        }));
        auto Low = lowFunction(Memory);
        auto Error = validateLowInstructionBoundaries(Low);
        EXPECT_FALSE(bool(Error));
        llvm::consumeError(std::move(Error));
        const auto Med = LowToMedConverter().convert(Low, Arch::AArch64);
        EXPECT_TRUE(std::any_of(Med.Blocks.front().Ops.begin(),
                                Med.Blocks.front().Ops.end(),
                                aarch64AddressMemoryMedShapeIsValid));
      }
      ++Encodings;
    }
  }
  EXPECT_EQ(Families, kOrderedFamilyCount);
  EXPECT_EQ(Encodings, kOrderedEncodingCount);
}

TEST(AArch64OrderedMemory, BothCProjectionsAndLLVMRetainOrderedInstructions) {
  // Compile every family independently of this host's optional CPU features.
  // The exported exact source/object inputs are executed by the Arm model.
  executeImmediateMemoryFamilies(OrderedMemoryCases, false, true, nullptr,
                                 false,
                                 {.ExecuteNative = false,
                                  .ArtifactProperty = kOrderedArtifactProperty,
                                  .AlignAccesses = true});
}

TEST(AArch64OrderedMemoryNative, GPRFamiliesExecuteBothSourcesAndLLVM) {
  if (!nativeOrderedMemorySupported(false))
    GTEST_SKIP() << kOrderedUnsupported;
  executeImmediateMemoryFamilies(orderedRegisterCases(false), false, true,
                                 nullptr, false, {.AlignAccesses = true});
}

TEST(AArch64OrderedMemoryNative, SIMDFamiliesExecuteBothSourcesAndLLVM) {
  if (!nativeOrderedMemorySupported(true))
    GTEST_SKIP() << kOrderedUnsupported;
  executeImmediateMemoryFamilies(orderedRegisterCases(true), false, true,
                                 nullptr, false, {.AlignAccesses = true});
}

TEST(AArch64OrderedMemory, ImageContextPreservesRuntimeBases) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  executeImmediateMemoryFamilies(
      OrderedMemoryCases, false, true, &Image, false,
      {.ExecuteNative = false,
       .ArtifactProperty = kOrderedImageArtifactProperty,
       .AlignAccesses = true});
}

TEST(AArch64OrderedMemory, ImageRelocationRetainsCompleteOrderedAccesses) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Segment Data;
  Data.VA = kImmediateRelocatedDataBase;
  Data.Size = Data.FileSz = kImmediateMemoryBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(Data);
  for (const auto &Case : OrderedMemoryCases) {
    const auto Instruction = *aarch64AddressMemoryInstruction(Case.Word);
    const va_t Base = Data.VA + kImmediateBasePosition;
    const auto Span = Instruction.controlIndependentSpan(Base, 0);
    ASSERT_TRUE(Span);
    EXPECT_LE(Span->Begin, Base + uint64_t(Case.Offset));
    EXPECT_GE(Span->Begin + Span->Bytes,
              Base + uint64_t(Case.Offset) + Case.Access);
    EXPECT_TRUE(aarch64ImmediateMemoryImageSpan(
        Image, Instruction, Base, ConstantAddressProvenance::DataAddress,
        Data.VA));
    auto Short = Image;
    Short.Segments.front().Size = Short.Segments.front().FileSz =
        Span->Begin + Span->Bytes - Data.VA - 1;
    Short.Segments.front().Data.resize(Short.Segments.front().Size);
    EXPECT_FALSE(aarch64ImmediateMemoryImageSpan(
        Short, Instruction, Base, ConstantAddressProvenance::DataAddress,
        Data.VA));
  }
  executeImmediateMemoryFamilies(
      OrderedMemoryCases, false, true, &Image, true,
      {.ExecuteNative = false,
       .ArtifactProperty = kOrderedRelocatedArtifactProperty,
       .AlignAccesses = true});
}

TEST(AArch64OrderedMemory, TracerUsesExactOriginalMemoryShapes) {
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
  for (const auto &Case : OrderedMemoryCases)
    for (bool Strict : {false, true}) {
      const va_t Base = kEntry + kImmediateBasePosition + kVectorBytes +
                        (-uint64_t(Case.Offset) & (Case.Access - 1));
      const va_t Address = Base + uint64_t(Case.Offset);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(a64reg::X1, Base);
      Emulator.setRegisterBytes(a64reg::V0, Seed);
      auto Memory = originalMemoryOperation(liftMemory(Case.Word, false));
      if (Case.Result)
        Memory.Output = NdVar::reg(a64reg::V0, Case.Access);
      else
        Memory.Inputs[kAddressMemoryDataOperand + 1] =
            NdVar::reg(a64reg::V0, Case.Access);
      ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
      ASSERT_TRUE(Emulator.step(Memory));
      if (Case.Result) {
        const auto Value = Emulator.getRegisterBytes(a64reg::V0);
        ASSERT_TRUE(Value);
        for (unsigned I = 0; I != Case.Access; ++I)
          EXPECT_EQ((*Value)[I], Data.Data[Address - kEntry + I]);
        ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
        EXPECT_EQ(Emulator.getLoadRecords().front().Addr, Address);
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
      if (Case.Access > 1) {
        const auto ReadGuard = [&](va_t Start) {
          LowOp Read;
          Read.Opcode = NdOp::LOAD;
          Read.Output =
              NdVar::reg(a64reg::V(kOrderedGuardVectorRegister), kVectorBytes);
          Read.addInput(NdVar::dataAddress(Start, kScalarBytes));
          if (!Emulator.step(Read))
            return std::optional<std::vector<uint8_t>>{};
          return Emulator.getRegisterBytes(
              a64reg::V(kOrderedGuardVectorRegister));
        };
        const auto Before = ReadGuard(Address);
        const auto Tail = ReadGuard(Address + kVectorBytes);
        ASSERT_TRUE(Before);
        ASSERT_TRUE(Tail);
        const auto Value = Emulator.getRegisterBytes(a64reg::V0);
        const auto LoadCount = Emulator.getLoadRecords().size();
        Emulator.setRegister(a64reg::X1, Base + 1);
        EXPECT_TRUE(aarch64AddressMemoryInstruction(Case.Word)
                        ->controlIndependentAddress(Base + 1, 0));
        EXPECT_FALSE(Emulator.step(Memory));
        EXPECT_EQ(Emulator.getRegisterBytes(a64reg::V0), Value);
        EXPECT_EQ(Emulator.getLoadRecords().size(), LoadCount);
        EXPECT_EQ(ReadGuard(Address), Before);
        EXPECT_EQ(ReadGuard(Address + kVectorBytes), Tail);
        Emulator.setRegister(a64reg::X1, Base);
      }
      Memory.Inputs[kAddressMemoryIndexOperand + 1] =
          NdVar::scalar(1, kScalarBytes);
      EXPECT_FALSE(Emulator.step(Memory));
    }
}

TEST(AArch64OrderedMemory, MalformedOperationsFailBeforeSourceFiltering) {
  for (const auto &Case : OrderedMemoryCases) {
    if (Case.Offset)
      continue;
    const auto Function = immediateMemoryFunction(Case, 0);
    const auto &Ops = Function.Blocks.front().Ops;
    const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                     aarch64AddressMemoryMedShapeIsValid);
    ASSERT_NE(Memory, Ops.end());
    const auto Index = Memory - Ops.begin();
    const std::function<void(MedOp &)> Mutations[] = {
        [](MedOp &Op) { Op.NumInputs--; },
        [](MedOp &Op) { Op.Output.Size++; },
        [](MedOp &Op) { Op.Inputs[kAddressMemoryBaseOperand + 1].Size--; },
        [](MedOp &Op) {
          Op.Inputs[kAddressMemoryIndexOperand + 1] =
              MedVar::makeConst(1, kScalarBytes);
        },
        [](MedOp &Op) {
          Op.Inputs[kAddressMemoryWordOperand + 1] = Op.Inputs[1];
        },
        [](MedOp &Op) { Op.MemoryOrdering = NdMemoryOrdering::Acquire; },
        [](MedOp &Op) { Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS; }};
    for (const auto &Mutate : Mutations) {
      auto Invalid = Function;
      auto &Bad = Invalid.Blocks.front().Ops[Index];
      Mutate(Bad);
      EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Bad));
      for (bool Hidden : {false, true}) {
        llvm::LLVMContext Context;
        const std::vector<char> Mask{0};
        EXPECT_THROW(MedLLVMEmitter().emit(
                         {Invalid}, Context, kName, Arch::AArch64, {}, nullptr,
                         BinaryFormat::ELF, false, Hidden ? &Mask : nullptr),
                     std::runtime_error);
      }
    }
    for (bool Hidden : {false, true}) {
      auto High = MedToHighConverter().convert(Function, Arch::AArch64);
      walkStmts(High.Body, [&](const HighStmt &Statement) {
        forEachExpr(Statement, [&](const ExprPtr &Expression) {
          if (Expression &&
              isAArch64AddressMemoryIntrinsic(Expression->IntrinsicId))
            Expression->Operands[kAddressMemoryIndexOperand] =
                HighExpr::makeConst(1, kScalarBytes);
        });
      });
      if (Hidden) {
        HighStmt If;
        If.Kind = StmtKind::If;
        If.Cond = HighExpr::makeConst(0, 1);
        If.Body = std::move(High.Body);
        High.Body = {std::move(If)};
      }
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      EXPECT_THROW(HighCEmitter().emit({High}, Out, Options),
                   std::invalid_argument);
      EXPECT_TRUE(Source.empty());
    }
  }
}

TEST(AArch64OrderedMemory, UnallocatedFormsCannotReachLegacyHandlers) {
  for (const uint32_t Word : UnallocatedOrderedWords) {
    EXPECT_TRUE(isAArch64OrderedUnscaledMemoryEncoding(Word));
    EXPECT_FALSE(aarch64AddressMemoryInstruction(Word));
    for (bool Fast : {false, true}) {
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Arch::AArch64));
      std::array<uint8_t, kWordBytes> Bytes;
      writeLE<uint32_t>(Bytes.data(), OrderedMemoryCases[0].Word);
      DecodedInsn Decoded;
      ASSERT_EQ(
          Fast ? Decode.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry,
                                         Decoded)
               : Decode.decodeOne(Bytes.data(), Bytes.size(), kEntry, Decoded),
          kWordBytes);
      ASSERT_TRUE(Decoded.Raw && Decoded.Raw->detail);
      auto Raw = *Decoded.Raw;
      writeLE<uint32_t>(Raw.bytes, Word);
      Raw.illegal = false;
      for (bool Strict : {false, true}) {
        AArch64Lifter Lifter(Arch::AArch64);
        Lifter.setStrict(Strict);
        std::vector<LowOp> Ops;
        EXPECT_THROW(Lifter.lift(&Raw, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
    }
  }
}

TEST(AArch64OrderedMemory, RegisterRolesKeepSPAndZeroDistinct) {
  for (const auto &Case : OrderedMemoryCases) {
    if (Case.Offset)
      continue;
    const auto Instruction = *aarch64AddressMemoryInstruction(Case.Word);
    const auto StackWord =
        Case.Word | (kAddressMemoryRegisterMask << kAddressMemoryBaseShift);
    const auto Stack = *aarch64AddressMemoryInstruction(StackWord);
    EXPECT_TRUE(Stack.stackBase());
    EXPECT_NE(Stack.sourceBaseFailure(true, true), nullptr);
    EXPECT_NE(Stack.sourceBaseFailure(false, false), nullptr);
    EXPECT_EQ(Stack.sourceBaseFailure(false, true), nullptr);
    EXPECT_FALSE(Stack.controlIndependentAddress(kStorageBase, 0));
    for (uint32_t Subtype : {uint32_t(llvm::MachO::CPU_SUBTYPE_ARM64_ALL),
                             macho_loader::arm64e::kCPUSubtype,
                             macho_loader::arm64e::kCPUSubtypeX1})
      for (bool Fast : {false, true}) {
        BinaryImage Image;
        Image.Arch = Arch::AArch64;
        Image.Bits = Bitness::Bits64;
        Image.Format = BinaryFormat::MachO;
        Image.MachOCPUSubtype = Subtype;
        Decoder Decode;
        ASSERT_TRUE(Decode.init(Image));
        std::array<uint8_t, kWordBytes> Bytes;
        writeLE<uint32_t>(Bytes.data(), Case.Word);
        DecodedInsn Decoded;
        ASSERT_EQ(Fast ? Decode.decodeOneForLift(Bytes.data(), Bytes.size(),
                                                 kEntry, Decoded)
                       : Decode.decodeOne(Bytes.data(), Bytes.size(), kEntry,
                                          Decoded),
                  kWordBytes);
        std::vector<LowOp> Ops;
        Decode.liftToLow(Decoded, Ops);
        EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(),
                                lowAArch64AddressMemoryShapeIsValid));
      }
    for (bool Fast : {false, true}) {
      const auto Word = Case.Word | kAddressMemoryRegisterMask;
      const auto Ops = liftMemory(Word, Fast);
      const auto Memory = originalMemoryOperation(Ops);
      ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
      if (!Instruction.isLoad() && !Case.Vector) {
        EXPECT_TRUE(Memory.Inputs[kAddressMemoryDataOperand + 1].isConst());
        EXPECT_EQ(Memory.Inputs[kAddressMemoryDataOperand + 1].Offset, 0);
        auto Invalid = Memory;
        Invalid.Inputs[kAddressMemoryDataOperand + 1].Offset = 1;
        EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Invalid));
      } else if (Case.Vector) {
        const auto &Value = Instruction.isLoad()
                                ? Memory.Output
                                : Memory.Inputs[kAddressMemoryDataOperand + 1];
        EXPECT_FALSE(Value.isConst());
      }
    }
  }
}

TEST(AArch64OrderedMemory, DiscardedReadsRetainOrderedEffects) {
  for (const auto &Case : OrderedMemoryCases) {
    if (Case.Offset || !Case.Result)
      continue;
    auto Function = immediateMemoryFunction(Case, 0);
    auto &Ops = Function.Blocks.front().Ops;
    const auto Memory = std::find_if(Ops.begin(), Ops.end(),
                                     aarch64AddressMemoryMedShapeIsValid);
    ASSERT_NE(Memory, Ops.end());
    auto Result = Ops.back().Inputs[0];
    Ops.resize(Memory - Ops.begin() + 1);
    MedOp Zero;
    Zero.Opcode = NdOp::COPY;
    Zero.Output = Result;
    Zero.addInput(MedVar::makeConst(0, kScalarBytes));
    Ops.push_back(Zero);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(Result);
    Ops.push_back(Return);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    optimizeMemory(*Module);
    for (bool LLVMRoute : {false, true})
      compileMemoryC(memorySource(Function, *Module, LLVMRoute, nullptr, true),
                     false, [&](llvm::MemoryBufferRef Buffer) {
                       inspectMemoryObject(Buffer, Case.Scratch);
                     });
  }
}

namespace {
#define STRUCTURE_MEMORY_VALUE(Type, Name, Value)                              \
  inline constexpr Type Name = Value;
#define STRUCTURE_MEMORY_TEXT(Name, Value) inline constexpr char Name[] = Value;
#include "AArch64StructureMemoryFixtures.def"
struct StructureMemoryCase {
  uint32_t Word, Scratch;
  unsigned Members, Repetitions, ElementBytes, Elements, Lane;
  bool Replicate, Single, Load;
  unsigned Mode, Width, Vectors;
};
constexpr StructureMemoryCase StructureMemoryCases[] = {
#define STRUCTURE_MEMORY_CASE(...) {__VA_ARGS__},
#include "AArch64StructureMemoryFixtures.def"
};

MedFunc structureMemoryFunction(const StructureMemoryCase &Case, size_t Index) {
  const auto I = *aarch64StructureMemoryInstruction(Case.Word);
  MedFunc F;
  F.Name = llvm::formatv(kStructureTestName, Index).str();
  F.Entry = kEntry + Index * kWordBytes;
  F.ReturnType = NdType::makeInt(kScalarBytes, false);
  F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  const auto Param = [&]() {
    MedVar V;
    V.Kind = MedVar::Param;
    V.Id = F.Params.size();
    V.RegOff = a64reg::X0 + F.Params.size() * kScalarBytes;
    V.Size = kScalarBytes;
    V.TheArch = Arch::AArch64;
    F.Params.push_back(V);
    return V;
  };
  const auto Base = Param();
  const auto Delta =
      I.hasRegisterIndex() ? Param() : MedVar::makeConst(0, kScalarBytes);
  const auto Input = !I.isLoad() || I.laneLoad() ? Param() : MedVar{};
  const auto Output = I.isLoad() ? Param() : MedVar{};
  F.Blocks.emplace_back();
  auto &B = F.Blocks.back();
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.EndAddr = F.Entry + kWordBytes;
  int Next = 0;
  const auto Temp = [&](unsigned Bytes) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = Next++;
    V.Size = Bytes;
    V.TheArch = Arch::AArch64;
    return V;
  };
  const auto Emit = [&](NdOp Code, MedVar Dst,
                        std::initializer_list<MedVar> Inputs) {
    MedOp O;
    O.Opcode = Code;
    O.Output = Dst;
    O.Addr = F.Entry;
    for (const auto &V : Inputs)
      O.addInput(V);
    B.Ops.push_back(O);
  };
  std::vector<MedVar> Values;
  for (unsigned N = 0; N != I.vectors(); ++N) {
    Values.push_back(Temp(kVectorBytes));
    if (!I.isLoad() || I.laneLoad()) {
      const auto Address = Temp(kScalarBytes);
      Emit(NdOp::INT_ADD, Address,
           {Input, MedVar::makeConst(N * kVectorBytes, kScalarBytes)});
      Emit(NdOp::LOAD, Values.back(), {Address});
    }
  }
  const auto Updated = Temp(kScalarBytes);
  Emit(NdOp::INTRINSIC, I.isLoad() ? Temp(0) : Updated,
       {MedVar::makeConst(unsigned(I.id()), kIdBytes), Base,
        MedVar::makeConst(Case.Word, kWordBytes), Delta});
  if (!I.isLoad() || I.laneLoad())
    for (const auto &Value : Values)
      B.Ops.back().addInput(Value);
  if (I.isLoad()) {
    B.Ops.back().IntrinsicOutputs = Values;
    B.Ops.back().IntrinsicOutputs.push_back(Updated);
    for (unsigned N = 0; N != Values.size(); ++N) {
      const auto Address = Temp(kScalarBytes);
      Emit(NdOp::INT_ADD, Address,
           {Output, MedVar::makeConst(N * kVectorBytes, kScalarBytes)});
      Emit(NdOp::STORE, {}, {Address, Values[N]});
    }
  }
  MedVar Return;
  Return.Kind = MedVar::Reg;
  Return.TheArch = Arch::AArch64;
  Return.RegOff = a64reg::X0;
  Return.Id = Next++;
  Return.SSAVer = 1;
  Return.Size = kScalarBytes;
  Emit(NdOp::COPY, Return, {Updated});
  Emit(NdOp::RETURN, {}, {Return});
  return F;
}

void executeStructureMemoryFamilies(const BinaryImage *Image, size_t Begin,
                                    bool Stack = false) {
  const auto End = std::min(Begin + kStructureTestBatchCases,
                            std::size(StructureMemoryCases));
  SCOPED_TRACE(Begin);
  std::vector<MedFunc> Functions;
  std::string Declarations, Replays, Entries, Faults, FaultEntries;
  for (size_t N = Begin; N != End; ++N) {
    auto Case = StructureMemoryCases[N];
    if (Stack) {
      Case.Word = (Case.Word & ~kStackBaseMask) | kStackBaseMask;
      Case.Scratch = (Case.Scratch & ~kStackBaseMask) | kStackBaseMask;
    }
    const auto I = *aarch64StructureMemoryInstruction(Case.Word);
    auto F = structureMemoryFunction(Case, N);
    ASSERT_TRUE(verifyMedFunc(F, F.Name.c_str()));
    std::string Types;
    for (size_t P = 1; P < F.Params.size(); ++P)
      Types += kStructureTestParameterType;
    Declarations +=
        llvm::formatv(kStructureTestDeclaration, F.Name, Types).str();
    std::string Arguments =
        Stack ? kStackStructureBaseArgument : kStructureTestBaseArgument;
    if (I.hasRegisterIndex())
      Arguments += kStructureTestIndexArgument;
    if (!I.isLoad() || I.laneLoad())
      Arguments += kStructureTestDataArgument;
    if (I.isLoad())
      Arguments += kStructureTestOutputArgument;
    const auto Call = llvm::formatv(kStructureTestCall, N, Arguments).str();
    const auto Copy = I.isLoad() ? llvm::formatv(kStructureTestCopyOutputs,
                                                 I.vectors() * kVectorBytes)
                                       .str()
                                 : std::string();
    Replays +=
        llvm::formatv(kStructureTestNativeFunction, N, Case.Word, Copy,
                      Stack ? kStackOracleX9Before : kEmptyNativeArgument,
                      Stack ? kStackOracleX9After : kEmptyNativeArgument,
                      Stack ? kStackOracleClobbers : kEmptyNativeArgument)
            .str();
    Replays += llvm::formatv(kStructureTestReplay, N,
                             llvm::formatv(Stack ? kStackStructureNativeCall
                                                 : kStructureTestNativeCall,
                                           N)
                                 .str(),
                             Call)
                   .str();
    Entries += llvm::formatv(kStructureTestEntry, N).str();
    if (Stack) {
      std::string FaultArguments = kStackFaultStructureBase;
      if (I.hasRegisterIndex())
        FaultArguments += kStackFaultStructureIndex;
      if (!I.isLoad() || I.laneLoad())
        FaultArguments += kStackFaultStructureInput;
      if (I.isLoad())
        FaultArguments += kStackFaultStructureOutput;
      Faults +=
          llvm::formatv(kStackFaultWrapper, N, F.Name, FaultArguments).str();
      FaultEntries += llvm::formatv(kStackFaultEntry, N).str();
    }
    Functions.push_back(std::move(F));
  }
  const auto Harness =
      std::string(kStructureTestPrefix) + Declarations + Replays +
      (Stack ? std::string(kStackFaultBegin) + Faults +
                   llvm::formatv(kStackFaultTable, FaultEntries).str() +
                   kStackFaultEnd
             : std::string()) +
      llvm::formatv(kStructureTestMain, Entries).str();
  llvm::LLVMContext C;
  auto Module =
      MedLLVMEmitter().emit(Functions, C, kName, Arch::AArch64, {}, Image);
  ASSERT_TRUE(Module);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  optimizeMemory(*Module);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  Options.Image = Image;
  std::string Directory;
  if (const char *Path = std::getenv(kStructureTestArtifactEnvironment)) {
    Directory = Path;
    if (Stack)
      Directory += kStackStructureDirectory;
    if (Image)
      Directory += kStructureTestImageDirectory;
    Directory += llvm::formatv(kStructureTestBatchDirectory, Begin).str();
    ASSERT_FALSE(llvm::sys::fs::create_directories(Directory));
    if (Begin == 0)
      ::testing::Test::RecordProperty(
          Stack   ? kStackStructureArtifactProperty
          : Image ? kStructureTestImageArtifactProperty
                  : kStructureTestArtifactProperty,
          Image ? std::string(Path) + kStructureTestImageDirectory
                : std::string(Path) + (Stack ? kStackStructureDirectory
                                             : kEmptyNativeArgument));
  }
  const auto Save = [&](llvm::StringRef Name, llvm::StringRef Bytes) {
    if (Directory.empty())
      return;
    llvm::SmallString<kVectorPathStorage> Path(Directory);
    llvm::sys::path::append(Path, Name);
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error);
    Out << Bytes;
  };
  Save(kWritebackHarnessArtifact, Harness);
  for (bool LLVMRoute : {false, true}) {
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (LLVMRoute)
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
    else {
      std::vector<HighFunc> Bodies;
      for (const auto &F : Functions) {
        auto Body = MedToHighConverter().convert(F, Arch::AArch64);
        for (auto &P : Body.Params)
          P.Type = NdType::makeInt(kScalarBytes, false);
        Bodies.push_back(std::move(Body));
      }
      ASSERT_TRUE(HighCEmitter().emit(Bodies, Out, Options));
    }
    Save(LLVMRoute ? kWritebackLLVMArtifact : kWritebackHighArtifact, Source);
#if defined(__APPLE__) && defined(__aarch64__)
    compileMemoryC(Source + Harness, true);
#else
    compileMemoryC(Source, false);
#endif
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
  }
  const auto Object =
      Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(Object.Success);
  if (Stack)
    Save(kImmediateMachOArtifact,
         llvm::StringRef(
             reinterpret_cast<const char *>(Object.ObjectData.data()),
             Object.ObjectData.size()));
  const llvm::MemoryBufferRef Buffer(
      llvm::StringRef(reinterpret_cast<const char *>(Object.ObjectData.data()),
                      Object.ObjectData.size()),
      kObject);
  for (size_t N = Begin; N != End; ++N)
    inspectMemoryObject(
        Buffer, Stack ? (StructureMemoryCases[N].Scratch & ~kStackBaseMask) |
                            kStackBaseMask
                      : StructureMemoryCases[N].Scratch);
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
  compileMemoryC(Harness, true, {}, ObjectPath);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
#endif
  if (!Directory.empty()) {
    const auto ELF =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::ELF);
    ASSERT_TRUE(ELF.Success);
    Save(kWritebackObjectArtifact,
         llvm::StringRef(reinterpret_cast<const char *>(ELF.ObjectData.data()),
                         ELF.ObjectData.size()));
  }
}
} // namespace

TEST(AArch64StructureMemory,
     IndependentWordsCoverEveryArrangementLaneAndIndex) {
  ASSERT_EQ(std::size(StructureMemoryCases), kStructureTestCases);
  for (const auto &Case : StructureMemoryCases) {
    SCOPED_TRACE(Case.Word);
    const auto I = aarch64StructureMemoryInstruction(Case.Word);
    ASSERT_TRUE(I);
    EXPECT_EQ(I->scratchWord(), Case.Scratch);
    EXPECT_EQ(I->Members, Case.Members);
    EXPECT_EQ(I->Repetitions, Case.Repetitions);
    EXPECT_EQ(I->ElementBytes, Case.ElementBytes);
    EXPECT_EQ(I->Elements, Case.Elements);
    EXPECT_EQ(I->Lane, Case.Lane);
    EXPECT_EQ(I->Replicate, Case.Replicate);
    EXPECT_EQ(I->Single, Case.Single);
    EXPECT_EQ(I->isLoad(), Case.Load);
    EXPECT_EQ(I->hasWriteback(), Case.Mode != 0);
    EXPECT_EQ(I->hasRegisterIndex(), Case.Mode == 2);
    EXPECT_EQ(I->vectors(), Case.Vectors);
    EXPECT_EQ(intrinsicOutputCount(I->id()), Case.Load ? Case.Vectors + 1 : 0);
    for (bool Fast : {false, true}) {
      const auto Ops = liftMemory(Case.Word, Fast);
      const auto Op = std::find_if(Ops.begin(), Ops.end(),
                                   lowAArch64WritebackMemoryShapeIsValid);
      ASSERT_NE(Op, Ops.end());
      EXPECT_EQ(Op->Inputs[0].Offset, unsigned(I->id()));
      EXPECT_EQ(Op->Inputs[kWritebackWordOperand + 1].Offset, Case.Word);
      EXPECT_EQ(Op->NumInputs, aarch64WritebackMemoryOperandCount(I->id()) + 1);
      auto Low = lowFunction(*Op);
      Low.Blocks.front().Ops = Ops;
      Low.Blocks.front().InstructionBoundaries.front().OpCount = Ops.size();
      auto Error = validateLowInstructionBoundaries(Low);
      EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      const auto F = LowToMedConverter().convert(Low, Arch::AArch64);
      EXPECT_TRUE(verifyMedFunc(F, kName));
      if (Case.Load)
        EXPECT_TRUE(lowAArch64WritebackOutputs(
            *Op, llvm::ArrayRef(Ops).drop_front(Op - Ops.begin() + 1)));
    }
  }
}

class AArch64StructureMemoryProjection
    : public ::testing::TestWithParam<size_t> {};

TEST_P(AArch64StructureMemoryProjection,
       BothCProjectionsAndLLVMExecuteCompleteResults) {
  executeStructureMemoryFamilies(nullptr, GetParam());
}

TEST_P(AArch64StructureMemoryProjection,
       ImageContextRetainsRuntimePostIndexAndVectorResults) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  executeStructureMemoryFamilies(&Image, GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    AllBatches, AArch64StructureMemoryProjection,
    ::testing::Range(size_t(0), std::size(StructureMemoryCases),
                     size_t(kStructureTestBatchCases)),
    [](const ::testing::TestParamInfo<size_t> &Info) {
      return llvm::formatv(kStructureTestBatchName, Info.index).str();
    });

TEST(AArch64StructureMemory, TracerKeepsCompleteValuesMemoryAndCompletedBase) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = kStructureTestStorageBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  for (size_t N = 0; N != Data.Data.size(); ++N)
    Data.Data[N] = N * kStructureTestSeedMultiplier + kStructureTestSeedBias;
  Image.Segments.push_back(Data);
  for (const auto &Case : StructureMemoryCases)
    for (bool Strict : {false, true}) {
      const auto Ops = liftMemory(Case.Word, false);
      const auto I = *aarch64StructureMemoryInstruction(Case.Word);
      const auto Base = kEntry + kStructureTestBasePosition;
      NdOpEmulator E(Image);
      E.setStrictMode(Strict);
      E.setRegister(a64reg::X0 + kStructureTestBaseRegister * kScalarBytes,
                    Base);
      E.setRegister(a64reg::X0 + kStructureTestIndexRegister * kScalarBytes,
                    kStructureTestRegisterDelta);
      std::vector<std::vector<uint8_t>> Values(
          Case.Vectors, std::vector<uint8_t>(kVectorBytes));
      for (unsigned R = 0; R != Case.Vectors; ++R) {
        for (unsigned B = 0; B != kVectorBytes; ++B)
          Values[R][B] = (R * kVectorBytes + B) * kStructureTestSeedMultiplier +
                         kStructureTestSeedBias;
        E.setRegisterBytes(a64reg::V((kStructureTestFirstVector + R) &
                                     kAddressMemoryRegisterMask),
                           Values[R]);
      }
      if (Case.Load && !I.laneLoad())
        for (auto &Value : Values)
          std::fill(Value.begin(), Value.end(), 0);
      auto Expected = Data.Data;
      unsigned Memory = kStructureTestBasePosition;
      for (unsigned Repeat = 0; Repeat != Case.Repetitions; ++Repeat)
        for (unsigned Element = 0; Element != Case.Elements; ++Element)
          for (unsigned Member = 0; Member != Case.Members; ++Member) {
            auto &Value = Values[Repeat * Case.Members + Member];
            const unsigned Offset = (Case.Lane + Element) * Case.ElementBytes;
            for (unsigned B = 0; B != Case.ElementBytes; ++B) {
              if (Case.Load)
                Value[Offset + B] = Expected[Memory++];
              else
                Expected[Memory++] = Value[Offset + B];
            }
          }
      if (Case.Replicate)
        for (auto &Value : Values)
          for (unsigned B = Case.ElementBytes; B < Case.Width; ++B)
            Value[B] = Value[B % Case.ElementBytes];
      ASSERT_EQ(E.run(Ops), Ops.size());
      EXPECT_EQ(
          E.getRegister(a64reg::X0 + kStructureTestBaseRegister * kScalarBytes),
          Base + (Case.Mode == 2   ? kStructureTestRegisterDelta
                  : Case.Mode == 1 ? Memory - kStructureTestBasePosition
                                   : 0));
      for (unsigned R = 0; R != Case.Vectors; ++R) {
        const auto Actual = E.getRegisterBytes(a64reg::V(
            (kStructureTestFirstVector + R) & kAddressMemoryRegisterMask));
        ASSERT_TRUE(Actual);
        EXPECT_TRUE(
            std::equal(Values[R].begin(), Values[R].end(), Actual->begin()));
      }
      for (unsigned B = 0; B != Expected.size(); B += kVectorBytes) {
        LowOp Read;
        Read.Opcode = NdOp::LOAD;
        Read.Output = NdVar::reg(a64reg::V0, kVectorBytes);
        Read.addInput(NdVar::scalar(Data.VA + B, kScalarBytes));
        ASSERT_TRUE(E.step(Read));
        const auto Actual = E.getRegisterBytes(a64reg::V0);
        ASSERT_TRUE(Actual);
        EXPECT_TRUE(
            std::equal(Actual->begin(), Actual->end(), Expected.begin() + B));
      }
    }
}

TEST(AArch64StructureMemory,
     MalformedLastVectorsAndResultsFailBeforeFiltering) {
  std::set<Intrinsic> Seen;
  for (size_t N = 0; N != std::size(StructureMemoryCases); ++N) {
    const auto &Case = StructureMemoryCases[N];
    const auto I = *aarch64StructureMemoryInstruction(Case.Word);
    if (!Seen.insert(I.id()).second)
      continue;
    auto Original = structureMemoryFunction(Case, N);
    const auto &Ops = Original.Blocks.front().Ops;
    const auto At = std::find_if(Ops.begin(), Ops.end(),
                                 aarch64WritebackMemoryMedShapeIsValid) -
                    Ops.begin();
    ASSERT_LT(At, Ops.size());
    std::vector<std::function<void(MedOp &)>> Mutations{
        [](MedOp &O) { --O.NumInputs; },
        [](MedOp &O) { O.Inputs.pop_back(); },
        [](MedOp &O) { O.Output.Size = kWordBytes; },
        [](MedOp &O) { --O.Inputs[kStructureIndexOperand + 1].Size; },
        [](MedOp &O) {
          O.Inputs[kWritebackWordOperand + 1] =
              MedVar::makeConst(0, kWordBytes);
        },
        [](MedOp &O) { O.MemoryOrdering = NdMemoryOrdering::Acquire; },
        [](MedOp &O) { O.MemoryAddressSpace = NdMemoryAddressSpace::X86FS; },
    };
    if (!I.isLoad() || I.laneLoad())
      Mutations.push_back([](MedOp &O) { --O.Inputs[O.NumInputs - 1].Size; });
    if (I.isLoad()) {
      Mutations.push_back([](MedOp &O) { O.IntrinsicOutputs.pop_back(); });
      Mutations.push_back([](MedOp &O) { --O.IntrinsicOutputs.back().Size; });
      Mutations.push_back([](MedOp &O) {
        O.IntrinsicOutputs.back() = O.IntrinsicOutputs.front();
      });
    }
    for (const auto &Mutate : Mutations) {
      auto Bad = Original;
      Mutate(Bad.Blocks.front().Ops[At]);
      EXPECT_FALSE(verifyMedFunc(Bad, kName));
      llvm::LLVMContext C;
      EXPECT_THROW(MedLLVMEmitter().emit({Bad}, C, kName, Arch::AArch64),
                   std::runtime_error);
    }
  }
}

TEST(AArch64StructureMemory, AllEightInputsAreKeptAndOverflowFailsClearly) {
  LowOp O;
  for (size_t N = 0; N != std::size(O.Inputs); ++N)
    O.addInput(NdVar::scalar(N, kScalarBytes));
  EXPECT_EQ(O.NumInputs, std::size(O.Inputs));
  for (size_t N = 0; N != std::size(O.Inputs); ++N)
    EXPECT_EQ(O.Inputs[N].Offset, N);
  EXPECT_THROW(O.addInput(NdVar::scalar(0, kScalarBytes)), std::length_error);
}

TEST(AArch64StructureMemory,
     SourceFilteringCannotHideMalformedCompleteVectors) {
  std::set<Intrinsic> Seen;
  for (size_t N = 0; N != std::size(StructureMemoryCases); ++N) {
    const auto I =
        *aarch64StructureMemoryInstruction(StructureMemoryCases[N].Word);
    if (!Seen.insert(I.id()).second)
      continue;
    const auto F = structureMemoryFunction(StructureMemoryCases[N], N);
    for (bool Hidden : {false, true}) {
      auto High = MedToHighConverter().convert(F, Arch::AArch64);
      walkStmts(High.Body, [&](const HighStmt &S) {
        forEachExpr(S, [&](const ExprPtr &E) {
          if (!E || E->IntrinsicId != I.id())
            return;
          if (I.isLoad())
            E->IntrinsicOutputs.back() = E->IntrinsicOutputs.front();
          else
            E->Operands.back()->Type = NdType::makeInt(kWordBytes, false);
        });
      });
      if (Hidden) {
        HighStmt If;
        If.Kind = StmtKind::If;
        If.Cond = HighExpr::makeConst(0, 1);
        If.Body = std::move(High.Body);
        High.Body = {std::move(If)};
      }
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      std::string Text;
      llvm::raw_string_ostream Out(Text);
      EXPECT_THROW(HighCEmitter().emit({High}, Out, Options),
                   std::invalid_argument);
      EXPECT_TRUE(Text.empty());
    }
  }
}

TEST(AArch64StructureMemory, RawSPAndWrappingVectorRolesRemainExplicit) {
  std::set<std::pair<Intrinsic, bool>> Seen;
  for (size_t N = 0; N != std::size(StructureMemoryCases); ++N) {
    const auto &Case = StructureMemoryCases[N];
    const auto I = *aarch64StructureMemoryInstruction(Case.Word);
    if (!Seen.emplace(I.id(), I.hasRegisterIndex()).second)
      continue;
    for (unsigned First :
         {0U, kStructureTestFirstVector, kAddressMemoryRegisterMask})
      for (bool Fast : {false, true}) {
        const auto Word = (Case.Word & ~kAddressMemoryRegisterMask) | First;
        const auto Ops = liftMemory(Word, Fast);
        const auto At = std::find_if(Ops.begin(), Ops.end(),
                                     lowAArch64WritebackMemoryShapeIsValid);
        ASSERT_NE(At, Ops.end());
        EXPECT_EQ(At->Inputs[kWritebackWordOperand + 1].Offset, Word);
        if (!I.isLoad() || I.laneLoad())
          for (unsigned R = 0; R != I.vectors(); ++R) {
            const auto &Input = At->Inputs[kStructureDataOperand + R + 1];
            EXPECT_TRUE(Input.isReg());
            EXPECT_EQ(Input.Offset,
                      a64reg::V((First + R) & kAddressMemoryRegisterMask));
          }
      }
    const auto SPWord =
        (Case.Word & ~(kAddressMemoryRegisterMask << kAddressMemoryBaseShift)) |
        (kAddressMemoryRegisterMask << kAddressMemoryBaseShift);
    const auto Ops = liftMemory(SPWord, false);
    const auto At = std::find_if(Ops.begin(), Ops.end(),
                                 lowAArch64WritebackMemoryShapeIsValid);
    ASSERT_NE(At, Ops.end());
    EXPECT_EQ(At->Inputs[kWritebackBaseOperand + 1],
              NdVar::reg(a64reg::SP, kScalarBytes));
    auto Low = lowFunction(*At);
    Low.Blocks.front().Ops = Ops;
    Low.Blocks.front().InstructionBoundaries.front().OpCount = Ops.size();
    const auto Frame = analyzeSourceStackFrame(Low, Arch::AArch64);
    ASSERT_TRUE(Frame.Error.empty());
    ASSERT_EQ(Frame.Accesses.size(), 1U);
    EXPECT_EQ(Frame.Accesses[0].Bytes, I.totalAccessBytes());
    EXPECT_EQ(Frame.Accesses[0].Offset, 0);
    EXPECT_EQ(Frame.Accesses[0].OriginalWord, SPWord);
    const auto Analyzed = LowToMedConverter().convert(Low, Arch::AArch64);
    EXPECT_TRUE(std::any_of(Analyzed.Blocks.front().Ops.begin(),
                            Analyzed.Blocks.front().Ops.end(),
                            aarch64WritebackMemoryMedShapeIsValid));
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    Image.MachOCPUSubtype = macho_loader::arm64e::kCPUSubtypeX1;
    LowToMedConverter SourceConverter;
    SourceConverter.setBinaryImage(&Image);
    EXPECT_THROW(SourceConverter.convert(Low, Arch::AArch64),
                 LowToMedConversionError);
    auto F = structureMemoryFunction(Case, N);
    for (auto &Op : F.Blocks.front().Ops)
      if (aarch64WritebackMemoryMedShapeIsValid(Op))
        Op.Inputs[kWritebackWordOperand + 1].ConstVal = SPWord;
    llvm::LLVMContext C;
    EXPECT_THROW(
        MedLLVMEmitter().emit({F}, C, kName, Arch::AArch64, {}, &Image),
        std::runtime_error);
  }
}

TEST(AArch64StructureMemory, LLVMAdmissionValidatesEveryVectorBeforeSelection) {
  std::set<std::pair<Intrinsic, bool>> Seen;
  for (size_t N = 0; N != std::size(StructureMemoryCases); ++N) {
    const auto &Case = StructureMemoryCases[N];
    const auto I = *aarch64StructureMemoryInstruction(Case.Word);
    if (!Seen.emplace(I.id(), I.hasRegisterIndex()).second)
      continue;
    for (unsigned Mutation = 0; Mutation != kStructureTestCallMutations;
         ++Mutation) {
      llvm::LLVMContext C;
      const auto F = structureMemoryFunction(Case, N);
      auto M = MedLLVMEmitter().emit({F}, C, kName, Arch::AArch64);
      ASSERT_TRUE(M);
      llvm::CallInst *Owned = nullptr;
      for (auto &B : *M->getFunction(F.Name))
        for (auto &Op : B)
          if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Op))
            if (validateAArch64WritebackMemoryCall(Arch::AArch64, *Call))
              Owned = Call;
      ASSERT_TRUE(Owned);
      EXPECT_THROW(validateAArch64WritebackMemoryCall(Arch::X64, *Owned),
                   std::invalid_argument);
      const auto *Asm = llvm::cast<llvm::InlineAsm>(Owned->getCalledOperand());
      if (Mutation == 0) {
        const auto Last = Owned->arg_size() - 1;
        const auto Word =
            llvm::cast<llvm::ConstantInt>(Owned->getArgOperand(Last))
                ->getZExtValue();
        Owned->setArgOperand(
            Last, llvm::ConstantInt::get(llvm::Type::getInt32Ty(C),
                                         Word ^ kAddressMemoryRegisterMask));
      } else if (Mutation == 1) {
        Owned->setCalledOperand(
            llvm::InlineAsm::get(Asm->getFunctionType(), Asm->getAsmString(),
                                 Asm->getConstraintString(), false));
      } else if (Mutation == 2) {
        Owned->setCallingConv(llvm::CallingConv::Fast);
      } else if (Mutation == 3) {
        Owned->addFnAttr(llvm::Attribute::Speculatable);
      } else if (Mutation == 4) {
        Owned->setMemoryEffects(llvm::MemoryEffects::none());
      } else {
        Owned->setCalledOperand(llvm::InlineAsm::get(
            Asm->getFunctionType(), Asm->getAsmString(),
            writebackConstraintsWithoutMemoryClobber(*Asm), true));
      }
      auto *Other = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getVoidTy(C), false),
          llvm::GlobalValue::ExternalLinkage, kVectorWritebackHiddenFunction,
          *M);
      llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, {}, Other));
      B.CreateRetVoid();
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      std::string Text;
      llvm::raw_string_ostream Out(Text);
      EXPECT_THROW(
          LLVMCEmitter().emit(*M, Out, Options, nullptr, nullptr, Other),
          std::invalid_argument);
      EXPECT_TRUE(Text.empty());
    }
  }
}

TEST(AArch64StructureMemory, OwnedSpansIncludeAllElementsAndReturnedBases) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = kStructureTestStorageBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size, kStructureTestSeedBias);
  Image.Segments.push_back(Data);
  const auto Base = Data.VA + kStructureTestBasePosition;
  for (const auto &Case : StructureMemoryCases) {
    const auto I = *aarch64WritebackMemoryInstruction(Case.Word);
    const auto Span = aarch64ImmediateMemoryImageSpan(
        Image, I, Base, ConstantAddressProvenance::DataAddress, InvalidVA);
    if (I.hasRegisterIndex()) {
      EXPECT_FALSE(Span);
      continue;
    }
    ASSERT_TRUE(Span);
    EXPECT_LE(Span->Begin, Base);
    EXPECT_GE(Span->Begin + Span->Bytes, Base + I.totalAccessBytes());
    EXPECT_GE(Span->Begin + Span->Bytes, *I.completedBase(Base));
    auto Short = Image;
    auto &Storage = Short.Segments.front();
    Storage.Size = Storage.FileSz =
        kStructureTestBasePosition + I.totalAccessBytes() - 1;
    Storage.Data.resize(Storage.Size);
    EXPECT_FALSE(aarch64ImmediateMemoryImageSpan(
        Short, I, Base, ConstantAddressProvenance::DataAddress, InvalidVA));
    EXPECT_FALSE(aarch64ImmediateMemoryImageSpan(
        Image, I, Base, ConstantAddressProvenance::Unknown, InvalidVA));
    EXPECT_FALSE(
        I.controlIndependentSpan(std::numeric_limits<uint64_t>::max(), 0));
  }
}

TEST(AArch64StructureMemory,
     ReservedWordsCannotReachLegacyOrPermissiveHandlers) {
  constexpr uint32_t Words[] = {
#define STRUCTURE_RESERVED_WORD(Word) Word,
#include "AArch64StructureMemoryReserved.def"
  };
  Decoder D;
  ASSERT_TRUE(D.init(Arch::AArch64));
  std::array<uint8_t, kWordBytes> Bytes;
  writeLE<uint32_t>(Bytes.data(), StructureMemoryCases[0].Word);
  DecodedInsn Valid;
  ASSERT_EQ(D.decodeOne(Bytes.data(), Bytes.size(), kEntry, Valid), kWordBytes);
  ASSERT_TRUE(Valid.Raw && Valid.Raw->detail);
  for (const auto Word : Words) {
    SCOPED_TRACE(Word);
    EXPECT_TRUE(isAArch64StructureMemoryEncoding(Word));
    EXPECT_FALSE(aarch64StructureMemoryInstruction(Word));
    EXPECT_FALSE(aarch64WritebackMemoryInstruction(Word));
    auto Raw = *Valid.Raw;
    writeLE<uint32_t>(Raw.bytes, Word);
    Raw.illegal = false;
    for (bool Strict : {false, true}) {
      AArch64Lifter L(Arch::AArch64);
      L.setStrict(Strict);
      std::vector<LowOp> Ops;
      EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
  }
}

TEST(AArch64StructureMemory, DiscardedCompleteResultsRetainOriginalEffects) {
  std::set<Intrinsic> Seen;
  for (size_t N = 0; N != std::size(StructureMemoryCases); ++N) {
    const auto &Case = StructureMemoryCases[N];
    const auto I = *aarch64StructureMemoryInstruction(Case.Word);
    if (!Seen.insert(I.id()).second)
      continue;
    auto F = structureMemoryFunction(Case, N);
    auto &Ops = F.Blocks.front().Ops;
    const auto At = std::find_if(Ops.begin(), Ops.end(),
                                 aarch64WritebackMemoryMedShapeIsValid);
    ASSERT_NE(At, Ops.end());
    const auto Return = Ops[Ops.size() - 2].Output;
    Ops.resize(At - Ops.begin() + 1);
    MedOp Zero;
    Zero.Opcode = NdOp::COPY;
    Zero.Output = Return;
    Zero.addInput(MedVar::makeConst(0, kScalarBytes));
    Ops.push_back(Zero);
    MedOp End;
    End.Opcode = NdOp::RETURN;
    End.addInput(Return);
    Ops.push_back(End);
    llvm::LLVMContext C;
    auto M = MedLLVMEmitter().emit({F}, C, kName, Arch::AArch64);
    ASSERT_TRUE(M);
    optimizeMemory(*M);
    for (bool LLVMRoute : {false, true})
      compileMemoryC(memorySource(F, *M, LLVMRoute, nullptr, true), false,
                     [&](llvm::MemoryBufferRef Buffer) {
                       inspectMemoryObject(Buffer, Case.Scratch);
                     });
  }
}

TEST(AArch64StackWritebackMemory, BothCProjectionsAndLLVMExecuteEveryFamily) {
  executeWritebackFamilies(nullptr, true);
}

TEST(AArch64StackVectorWritebackMemory,
     BothCProjectionsAndLLVMExecuteEveryFamily) {
  executeVectorWritebackFamilies(nullptr, true);
}

TEST(AArch64StackPairMemory, BothCProjectionsAndLLVMExecuteEveryFamily) {
  executePairMemoryFamilies(nullptr, true);
}

TEST_P(AArch64StructureMemoryProjection,
       RawSPKeepsCompleteResultsAndHostStack) {
  executeStructureMemoryFamilies(nullptr, GetParam(), true);
}

TEST(AArch64StackWritebackMemory, SourceAdmissionAndInverseAssemblyKeepRawSP) {
  std::vector<MedFunc> Functions;
  std::set<Intrinsic> Seen;
  const auto Add = [&](MedFunc F) {
    for (auto &B : F.Blocks)
      for (auto &O : B.Ops)
        if (aarch64WritebackMemoryMedShapeIsValid(O)) {
          const auto Id = static_cast<Intrinsic>(O.Inputs[0].ConstVal);
          if (!Seen.insert(Id).second)
            return;
          auto &Word = O.Inputs[kWritebackWordOperand + 1].ConstVal;
          Word = (Word & ~kStackBaseMask) | kStackBaseMask;
        }
    Functions.push_back(std::move(F));
  };
  for (const auto &C : WritebackCases)
    Add(writebackFunction(C, Functions.size()));
  for (const auto &C : VectorWritebackCases)
    Add(vectorWritebackFunction(C, Functions.size()));
  for (const auto &C : PairMemoryCases)
    Add(pairMemoryFunction(C, Functions.size()));
  for (const auto &C : StructureMemoryCases)
    Add(structureMemoryFunction(C, Functions.size()));
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  for (const auto &F : Functions) {
    SCOPED_TRACE(F.Name);
    llvm::LLVMContext C;
    auto Module = MedLLVMEmitter().emit({F}, C, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      llvm::LLVMContext MC;
      auto M = MedLLVMEmitter().emit({F}, MC, kName, Arch::AArch64);
      llvm::CallInst *Owned = nullptr;
      for (auto &Fn : *M)
        for (auto &B : Fn)
          for (auto &O : B)
            if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&O))
              if (validateAArch64WritebackMemoryCall(Arch::AArch64, *Call))
                Owned = Call;
      ASSERT_NE(Owned, nullptr);
      const auto *Asm = llvm::cast<llvm::InlineAsm>(Owned->getCalledOperand());
      EXPECT_NE(Asm->getAsmString().find(kStackCaptureSP), std::string::npos);
      EXPECT_LT(Asm->getAsmString().find(kStackCaptureSP),
                Asm->getAsmString().find(kStackRestoreSP));
      auto Text = Asm->getAsmString().str();
      auto Constraints = Asm->getConstraintString().str();
      if (Mutation == 0) {
        const auto At = Constraints.find(kStackClobberSP);
        ASSERT_NE(At, std::string::npos);
        Constraints.erase(At, sizeof(kStackClobberSP));
      } else if (Mutation == 1) {
        const auto At = Text.find(kStackRestoreSP);
        ASSERT_NE(At, std::string::npos);
        Text.erase(At, sizeof(kStackRestoreSP) - 1);
      } else {
        auto *Word = llvm::cast<llvm::ConstantInt>(
            Owned->getArgOperand(Owned->arg_size() - 1));
        Owned->setArgOperand(
            Owned->arg_size() - 1,
            llvm::ConstantInt::get(Word->getType(),
                                   (Word->getZExtValue() & ~kStackBaseMask) |
                                       kStackGeneralBaseBits));
      }
      Owned->setCalledOperand(llvm::InlineAsm::get(Asm->getFunctionType(), Text,
                                                   Constraints, true));
      ASSERT_FALSE(llvm::verifyModule(*M));
      EXPECT_THROW(validateAArch64WritebackMemoryCall(Arch::AArch64, *Owned),
                   std::invalid_argument);
    }
    for (auto Kind : {MedVar::Param, MedVar::Reg, MedVar::Temp, MedVar::Const})
      for (bool WithImage : {false, true}) {
        if (Kind == MedVar::Param && !WithImage)
          continue;
        auto Bad = F;
        auto High = MedToHighConverter().convert(F, Arch::AArch64);
        MedVar Base;
        for (auto &B : Bad.Blocks)
          for (auto &O : B.Ops)
            if (aarch64WritebackMemoryMedShapeIsValid(O)) {
              Base = O.Inputs[kWritebackBaseOperand + 1];
              Base.Kind = Kind;
              Base.RegOff = a64reg::SP;
              Base.ConstVal = kEntry;
              O.Inputs[kWritebackBaseOperand + 1] = Base;
            }
        const std::vector<char> Mask{0};
        llvm::LLVMContext BC;
        EXPECT_THROW(MedLLVMEmitter().emit({Bad}, BC, kName, Arch::AArch64, {},
                                           WithImage ? &Image : nullptr,
                                           BinaryFormat::MachO, false, &Mask),
                     std::runtime_error);
        std::function<void(const ExprPtr &)> Reject;
        Reject = [&](const ExprPtr &E) {
          if (!E)
            return;
          if (isAArch64WritebackMemoryIntrinsic(E->IntrinsicId))
            E->Operands[kWritebackBaseOperand] =
                Kind == MedVar::Const
                    ? HighExpr::makeConst(kEntry, kScalarBytes)
                    : HighExpr::makeVar(Base,
                                        NdType::makeInt(kScalarBytes, false));
          E->forEachChildExpr(Reject);
        };
        walkStmts(High.Body,
                  [&](const HighStmt &S) { forEachExpr(S, Reject); });
        HighStmt Branch;
        Branch.Kind = StmtKind::If;
        Branch.Cond = HighExpr::makeConst(0, 1);
        Branch.Body = std::move(High.Body);
        High.Body = {std::move(Branch)};
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

namespace {
#define RAW_SP_FLOW_VALUE(Type, Name, Value) inline constexpr Type Name = Value;
#define RAW_SP_FLOW_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64RawSPFlowFixtures.def"

struct RawSPFlowCase {
  uint32_t Word;
  uint32_t Native;
};

std::vector<RawSPFlowCase> rawSPFlowCases(bool Stack = true) {
  std::vector<RawSPFlowCase> Result;
  std::set<Intrinsic> Seen;
  const auto Add = [&](const auto &Case) {
    const auto I = *aarch64WritebackMemoryInstruction(Case.Word);
    if ((I.hasWriteback() && !I.hasRegisterIndex() &&
         I.displacement() % kFlowAlignment) ||
        !Seen.insert(I.id()).second)
      return;
    const auto Base = Stack ? kStackBaseMask : kStackGeneralBaseBits;
    Result.push_back({(Case.Word & ~kStackBaseMask) | Base,
                      (Case.Scratch & ~kStackBaseMask) | Base});
  };
  for (const auto &C : WritebackCases)
    Add(C);
  for (const auto &C : VectorWritebackCases)
    Add(C);
  for (const auto &C : PairMemoryCases)
    Add(C);
  for (const auto &C : StructureMemoryCases)
    Add(C);
  return Result;
}

MedFunc rawSPFlowFunction(uint32_t Word, size_t Index) {
  const auto Instruction = *aarch64WritebackMemoryInstruction(Word);
  const auto Id = Instruction.id();
  MedFunc Function;
  Function.Name = kFlowName + llvm::utostr(Index);
  Function.Entry = kEntry + Index * kVectorFunctionStride;
  Function.ReturnType = NdType::makeInt(kWritebackPairBytes, false);
  Function.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  MedVar Parameter;
  Parameter.Kind = MedVar::Param;
  Parameter.TheArch = Arch::AArch64;
  Parameter.Id = 0;
  Parameter.Size = kScalarBytes;
  Parameter.RegOff = a64reg::X0;
  Function.Params.push_back(Parameter);
  Function.TypedParams.push_back({{}, NdType::makeInt(kScalarBytes, false)});
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  int Next = 1;
  const auto Temporary = [&](unsigned Bytes) {
    MedVar Result;
    Result.Kind = MedVar::Temp;
    Result.TheArch = Arch::AArch64;
    Result.Id = Next++;
    Result.SSAVer = 1;
    Result.Size = Bytes;
    return Result;
  };
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = Temporary(kScalarBytes);
  Copy.addInput(Parameter);
  Block.Ops.push_back(Copy);
  MedOp Access;
  Access.Opcode = NdOp::INTRINSIC;
  Access.addInput(MedVar::makeConst(static_cast<uint64_t>(Id), kIdBytes));
  Access.addInput(Copy.Output);
  Access.addInput(MedVar::makeConst(Word, kWordBytes));
  if (Instruction.isStructure())
    Access.addInput(MedVar::makeConst(
        Instruction.hasRegisterIndex() ? kFlowAlignment : 0, kScalarBytes));
  while (Access.NumInputs != aarch64WritebackMemoryOperandCount(Id) + 1)
    Access.addInput(MedVar::makeConst(
        0, Instruction.isVector() ? kAddressMemoryVectorBytes : kScalarBytes));
  MedVar Completed;
  if (isAArch64WritebackAuxiliaryLoad(Id)) {
    Access.Output.Id = -1;
    Access.Output.Size = 0;
    const auto BaseIndex = aarch64WritebackBaseIndex(Id);
    for (unsigned N = 0; N <= BaseIndex; ++N)
      Access.IntrinsicOutputs.push_back(
          Temporary(N == BaseIndex || Id == Intrinsic::A64_PairLoad
                        ? kScalarBytes
                        : kAddressMemoryVectorBytes));
    Completed = Access.IntrinsicOutputs.back();
  } else {
    Access.Output =
        Temporary(Instruction.isLoad() ? kWritebackPairBytes : kScalarBytes);
    Completed = Access.Output;
  }
  Block.Ops.push_back(Access);
  if (Id == Intrinsic::A64_LoadWriteback) {
    MedOp Extract;
    Extract.Opcode = NdOp::SUBBYTES;
    Extract.Output = Temporary(kScalarBytes);
    Extract.addInput(Access.Output);
    Extract.addInput(MedVar::makeConst(kScalarBytes, kWordBytes));
    Block.Ops.push_back(Extract);
    Completed = Extract.Output;
  }
  Copy.Output = Temporary(kScalarBytes);
  Copy.Inputs[0] = Completed;
  Block.Ops.push_back(Copy);
  MedOp Load;
  Load.Opcode = NdOp::INTRINSIC;
  Load.Output = Temporary(kScalarBytes);
  Load.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Intrinsic::A64_LoadImmediate), kIdBytes));
  Load.addInput(Copy.Output);
  Load.addInput(MedVar::makeConst(0, kScalarBytes));
  Load.addInput(MedVar::makeConst(Instruction.stackBase() ? kFlowSecondWord
                                                          : kFlowSecondGPRWord,
                                  kWordBytes));
  Block.Ops.push_back(Load);
  MedOp Pack;
  Pack.Opcode = NdOp::CONCAT;
  Pack.Output = Temporary(kWritebackPairBytes);
  Pack.Output.Kind = MedVar::Reg;
  Pack.Output.RegOff = a64reg::X0;
  Pack.addInput(Copy.Output);
  Pack.addInput(Load.Output);
  Block.Ops.push_back(Pack);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Pack.Output);
  Block.Ops.push_back(Return);
  return Function;
}

void executeRawSPMemoryProjections(const std::vector<MedFunc> &Functions,
                                   const std::string &Harness,
                                   const char *Environment,
                                   const char *Property,
                                   bool PhiReads = false) {
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(Functions, Context, kName, Arch::AArch64);
  ASSERT_TRUE(Module);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  optimizeMemory(*Module);
  std::string ArtifactDirectory;
  if (const char *Directory = std::getenv(Environment)) {
    ArtifactDirectory = Directory;
    ::testing::Test::RecordProperty(Property, ArtifactDirectory);
    ASSERT_FALSE(llvm::sys::fs::create_directories(ArtifactDirectory));
  }
  const auto Save = [&](llvm::StringRef Name, llvm::StringRef Data) {
    if (ArtifactDirectory.empty())
      return;
    llvm::SmallString<kVectorPathStorage> Path(ArtifactDirectory);
    llvm::sys::path::append(Path, Name);
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error);
    Out << Data;
  };
  Save(kWritebackHarnessArtifact, Harness);
  std::string IR;
  llvm::raw_string_ostream IRStream(IR);
  Module->print(IRStream, nullptr);
  Save(kFlowModuleArtifact, IR);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  for (bool LLVMRoute : {false, true}) {
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (LLVMRoute)
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
    else {
      std::vector<HighFunc> High;
      std::string HighIR;
      for (const auto &F : Functions) {
        auto Body = MedToHighConverter().convert(F, Arch::AArch64);
        for (auto &P : Body.Params)
          P.Type = NdType::makeInt(kScalarBytes, false);
        if (PhiReads) {
          std::function<void(const ExprPtr &)> Mark;
          Mark = [&](const ExprPtr &E) {
            if (!E)
              return;
            if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Temp)
              E->Kind = ExprKind::Phi;
            E->forEachChildExpr(Mark);
          };
          walkStmts(Body.Body,
                    [&](const HighStmt &S) { forEachRhsExpr(S, Mark); });
        }
        HighIR += llvm::formatv(kFlowDumpFunction, F.Name).str();
        for (const auto &Statement : Body.Body)
          printHighStatementRows(Statement, 0,
                                 [&](llvm::StringRef Row, const HighStmt *) {
                                   HighIR += Row.str();
                                 });
        High.push_back(std::move(Body));
      }
      Save(kFlowHighIRArtifact, HighIR);
      ASSERT_TRUE(HighCEmitter().emit(High, Out, Options));
    }
    Save(LLVMRoute ? kWritebackLLVMArtifact : kWritebackHighArtifact, Source);
#if defined(__APPLE__) && defined(__aarch64__)
    compileMemoryC(Source + Harness, true, {}, {}, true);
#else
    compileMemoryC(Source, false, {}, {}, true);
#endif
  }
  for (auto Format : {BinaryFormat::MachO, BinaryFormat::ELF}) {
    const auto Object = Codegen().compile(*Module, Arch::AArch64, Format);
    ASSERT_TRUE(Object.Success);
    Save(Format == BinaryFormat::ELF ? kWritebackObjectArtifact
                                     : kImmediateMachOArtifact,
         llvm::StringRef(
             reinterpret_cast<const char *>(Object.ObjectData.data()),
             Object.ObjectData.size()));
#if defined(__APPLE__) && defined(__aarch64__)
    if (Format != BinaryFormat::MachO)
      continue;
    llvm::SmallString<kVectorPathStorage> Path;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix,
                                                    kOutputExtension, Path));
    llvm::FileRemover Remove(Path);
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error);
    Out.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
              Object.ObjectData.size());
    Out.close();
    compileMemoryC(Harness, true, {}, Path, true);
#endif
  }
}

TEST(AArch64RawSPLifting, AutomaticCheckedMemoryRetainsEveryOriginalSPWord) {
  std::vector<std::pair<uint32_t, bool>> Words;
  const auto Add = [&](const auto &Cases, bool Writeback) {
    for (const auto &C : Cases)
      Words.push_back({(C.Word & ~kStackBaseMask) | kStackBaseMask, Writeback});
  };
  Add(ImmediateMemoryCases, false);
  Add(OrderedMemoryCases, false);
  Add(WritebackCases, true);
  Add(VectorWritebackCases, true);
  Add(PairMemoryCases, true);
  Add(StructureMemoryCases, true);
  ASSERT_EQ(Words.size(), kFlowAutomaticWords);
  for (bool ArchitectureOnly : {false, true})
    for (bool Fast : {false, true})
      for (bool Strict : {false, true}) {
        BinaryImage Image;
        Image.Arch = Arch::AArch64;
        Image.Bits = Bitness::Bits64;
        Image.Format = BinaryFormat::MachO;
        Image.MachOCPUSubtype = macho_loader::arm64e::kCPUSubtypeX1;
        Decoder Decoder;
        ASSERT_TRUE(ArchitectureOnly ? Decoder.init(Arch::AArch64)
                                     : Decoder.init(Image));
        Decoder.setStrict(Strict);
        for (const auto &[Word, Writeback] : Words) {
          SCOPED_TRACE(Word);
          std::array<uint8_t, kWordBytes> Bytes;
          writeLE<uint32_t>(Bytes.data(), Word);
          DecodedInsn Insn;
          ASSERT_EQ(Fast ? Decoder.decodeOneForLift(Bytes.data(), Bytes.size(),
                                                    kEntry, Insn)
                         : Decoder.decodeOne(Bytes.data(), Bytes.size(), kEntry,
                                             Insn),
                    kWordBytes);
          std::vector<LowOp> Ops;
          Decoder.liftToLow(Insn, Ops);
          const auto Shape = Writeback ? lowAArch64WritebackMemoryShapeIsValid
                                       : lowAArch64AddressMemoryShapeIsValid;
          ASSERT_EQ(std::count_if(Ops.begin(), Ops.end(), Shape), 1);
          const auto &Memory = *std::find_if(Ops.begin(), Ops.end(), Shape);
          const auto Base =
              Writeback ? kWritebackBaseOperand : kAddressMemoryBaseOperand;
          const auto Encoding =
              Writeback ? kWritebackWordOperand : kAddressMemoryWordOperand;
          EXPECT_EQ(Memory.Inputs[Base + 1],
                    NdVar::reg(a64reg::SP, kScalarBytes));
          EXPECT_EQ(Memory.Inputs[Encoding + 1].Offset, Word);
          EXPECT_EQ(Memory.Inputs[Encoding + 1].Provenance,
                    ConstantAddressProvenance::Scalar);
          EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &O) {
            return O.Opcode == NdOp::LOAD || O.Opcode == NdOp::STORE ||
                   O.Opcode == NdOp::INT_ADD;
          }));
        }
      }
}

} // namespace

TEST(AArch64RawSPFlow, CompletedBasesExecuteThroughEveryMemoryFamily) {
  const auto Cases = rawSPFlowCases();
  ASSERT_EQ(Cases.size(), kFlowFamilyCount);
  std::vector<MedFunc> Functions;
  std::string Harness = kFlowPrelude, Entries;
  for (size_t N = 0; N != Cases.size(); ++N) {
    auto Function = rawSPFlowFunction(Cases[N].Word, N);
    ASSERT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
    Harness += llvm::formatv(kFlowDeclaration, Function.Name).str();
    Harness +=
        llvm::formatv(
            kFlowNative, N, Cases[N].Native,
            aarch64WritebackMemoryInstruction(Cases[N].Word)->hasRegisterIndex()
                ? kFlowAlignment
                : 0,
            kFlowSPBefore, kFlowSecondWord, kFlowSPAfter)
            .str();
    Entries += llvm::formatv(kFlowEntry, Function.Name, N).str();
    Functions.push_back(std::move(Function));
  }
  Harness += llvm::formatv(kFlowTail, Entries).str();
  executeRawSPMemoryProjections(Functions, Harness, kFlowArtifactEnvironment,
                                kFlowArtifactProperty);
}

TEST(AArch64RawSPFlow, GPRControlsRetainEveryAuxiliaryResultCopy) {
  const auto Cases = rawSPFlowCases(false);
  ASSERT_EQ(Cases.size(), kFlowFamilyCount);
  std::vector<MedFunc> Functions;
  std::string Harness = kFlowPrelude, Entries;
  for (size_t N = 0; N != Cases.size(); ++N) {
    auto Function = rawSPFlowFunction(Cases[N].Word, N);
    ASSERT_TRUE(verifyMedFunc(Function, Function.Name.c_str()));
    Harness += llvm::formatv(kFlowDeclaration, Function.Name).str();
    Harness +=
        llvm::formatv(
            kFlowNative, N, Cases[N].Native,
            aarch64WritebackMemoryInstruction(Cases[N].Word)->hasRegisterIndex()
                ? kFlowAlignment
                : 0,
            kFlowGPRBefore, kFlowSecondGPRWord, kFlowGPRAfter)
            .str();
    Entries += llvm::formatv(kFlowEntry, Function.Name, N).str();
    Functions.push_back(std::move(Function));
  }
  Harness += llvm::formatv(kFlowTail, Entries).str();
  executeRawSPMemoryProjections(Functions, Harness, kEmptyNativeArgument,
                                kFlowArtifactProperty, true);
}

TEST(AArch64RawSPLifting, OrderedSPAccessesNeverBecomeOrdinaryFrameMemory) {
  for (auto Subtype :
       {uint32_t(llvm::MachO::CPU_SUBTYPE_ARM64_ALL),
        macho_loader::arm64e::kCPUSubtype, macho_loader::arm64e::kCPUSubtypeX1})
    for (bool Fast : {false, true}) {
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::MachO;
      Image.MachOCPUSubtype = Subtype;
      Decoder Decoder;
      ASSERT_TRUE(Decoder.init(Image));
      for (const auto &C : OrderedMemoryCases) {
        const auto Word = (C.Word & ~kStackBaseMask) | kStackBaseMask;
        std::array<uint8_t, kWordBytes> Bytes;
        writeLE<uint32_t>(Bytes.data(), Word);
        DecodedInsn Insn;
        ASSERT_EQ(
            Fast ? Decoder.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry,
                                            Insn)
                 : Decoder.decodeOne(Bytes.data(), Bytes.size(), kEntry, Insn),
            kWordBytes);
        std::vector<LowOp> Ops;
        Decoder.liftToLow(Insn, Ops);
        const auto Memory = originalMemoryOperation(Ops);
        ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
        EXPECT_EQ(Memory.Inputs[kAddressMemoryBaseOperand + 1].Offset,
                  a64reg::SP);
        EXPECT_EQ(Memory.Inputs[kAddressMemoryWordOperand + 1].Offset, Word);
      }
    }
}

namespace {
#define STACK_PAUTH_VALUE(Type, Name, Value) inline constexpr Type Name = Value;
#define STACK_PAUTH_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64StackPAuthLoadFixtures.def"
struct StackPAuthCase {
  uint32_t Word;
  Intrinsic Id;
  unsigned Base;
  int64_t Offset;
  bool Writeback;
  uint32_t Scratch;
};
constexpr StackPAuthCase StackPAuthCases[] = {
#define NEVERD_PAUTH_LOAD_CASE(Word, Id, Dst, Base, Offset, WB, Scratch,       \
                               Compile)                                        \
  {Word, Intrinsic::Id, Base, Offset, WB, Scratch},
#include "AArch64PAuthLoadFixtures.def"
};
} // namespace

TEST(AArch64StackPAuthLoad, BothCProjectionsAndLLVMExecuteCombinedSPAccesses) {
  std::vector<MedFunc> Functions;
  std::string Harness = kStackPAuthPrelude, Entries;
  for (const auto &C : StackPAuthCases) {
    if (C.Base != kZeroRegister)
      continue;
    const auto N = Functions.size();
    MedFunc F;
    F.Name = kStackPAuthName + llvm::utostr(N);
    F.Entry = kEntry + N * kVectorFunctionStride;
    F.ReturnType = NdType::makeInt(kWritebackPairBytes, false);
    F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
    MedVar Base;
    Base.Kind = MedVar::Param;
    Base.TheArch = Arch::AArch64;
    Base.Id = 0;
    Base.Size = kScalarBytes;
    Base.RegOff = a64reg::X0;
    F.Params = {Base};
    F.TypedParams.push_back({{}, NdType::makeInt(kScalarBytes, false)});
    MedOp Load;
    Load.Opcode = NdOp::INTRINSIC;
    Load.Output = Base;
    Load.Output.Kind = MedVar::Reg;
    Load.Output.RegOff = a64reg::X0;
    Load.Output.Id = 1;
    Load.Output.SSAVer = 1;
    Load.Output.Size = kWritebackPairBytes;
    Load.addInput(MedVar::makeConst(static_cast<uint64_t>(C.Id), kIdBytes));
    Load.addInput(Base);
    Load.addInput(MedVar::makeConst(C.Word, kWordBytes));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(Load.Output);
    F.Blocks.emplace_back();
    F.Blocks.front().Id = 0;
    F.Blocks.front().StartAddr = F.Entry;
    F.Blocks.front().Ops = {Load, Return};
    ASSERT_TRUE(verifyMedFunc(F, F.Name.c_str()));
    Harness += llvm::formatv(kStackPAuthNative, F.Name, N, C.Scratch).str();
    Entries += llvm::formatv(kStackPAuthEntry, F.Name, N,
                             unsigned(C.Id == Intrinsic::A64_Ldrab),
                             unsigned(C.Writeback), C.Offset)
                   .str();
    Functions.push_back(std::move(F));
  }
  ASSERT_EQ(Functions.size(), kStackPAuthCaseCount);
  Harness += llvm::formatv(kStackPAuthTail, Entries).str();
  executeRawSPMemoryProjections(Functions, Harness,
                                kStackPAuthArtifactEnvironment,
                                kStackPAuthArtifactProperty);
}
