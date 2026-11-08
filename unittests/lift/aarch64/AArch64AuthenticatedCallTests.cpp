//===- AArch64AuthenticatedCallTests.cpp - Call authentication ABI ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the machine-to-call boundary and independently recompile both C
/// projections. These compiler checks do not establish PAC-enabled execution.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/AArch64/AArch64SourceResultCall.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/AArch64/AArch64AuthenticatedCall.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>

namespace {
using namespace neverd;

#define A64_CALL_TEXT(Name, Text) constexpr char Name[] = Text;
#define A64_CALL_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64AuthenticatedCallFixtures.def"

struct TypedShape {
  llvm::StringRef Name, Parameters;
  char ReturnKind;
  int Fixed;
};
constexpr TypedShape TypedShapes[] = {
#define A64_CALL_TYPED_SHAPE(Name, Parameters, ReturnKind, Fixed)              \
  {Name, Parameters, ReturnKind, Fixed},
#include "AArch64AuthenticatedCallFixtures.def"
};
struct TypedVariant {
  unsigned Key;
  bool Zero, Raw;
};
constexpr TypedVariant TypedVariants[] = {
#define A64_CALL_TYPED_VARIANT(Key, Zero, Raw) {Key, Zero, Raw},
#include "AArch64AuthenticatedCallFixtures.def"
};

TypeRef typedParameter(char Kind) {
  if (Kind == kVectorKind) {
    auto Type = NdType::makeInt(kSourceAArch64Vector128Bytes);
    Type->SourceName = kSourceAArch64Vector128CType;
    return Type;
  }
  if (Kind == kFloatKind)
    return NdType::makeFloat(kFloatBytes);
  if (Kind == kDoubleKind)
    return NdType::makeFloat(kPointerBytes);
  if (Kind == kPointerKind)
    return NdType::makePtr(NdType::makeVoid());
  switch (Kind) {
#define A64_CALL_NAMED_POINTER_TYPE(Kind, Name)                                \
  case Kind:                                                                   \
    return NdType::makePtr(NdType::makeNamedRecord(Name));
#define A64_CALL_RECORD_TYPE(Kind, Fields)                                     \
  case Kind: {                                                                 \
    std::vector<TypeRef> Members;                                              \
    for (char Field : llvm::StringRef(Fields))                                 \
      Members.push_back(typedParameter(Field));                                \
    return NdType::makeStruct(std::move(Members));                             \
  }
#define A64_CALL_INTEGER_TYPE(Kind, Bytes, Signed)                             \
  case Kind:                                                                   \
    return NdType::makeInt(Bytes, Signed);
#include "AArch64AuthenticatedCallFixtures.def"
  }
  llvm_unreachable(kUnknownTypedKind);
}

MedFunc function(unsigned Key, bool Zero, unsigned Arguments,
                 bool Raw = false) {
  MedFunc Function;
  Function.Name = kFunction;
  Function.Entry = kEntryAddress;
  Function.ReturnType = NdType::makeInt(kPointerBytes);
  for (unsigned I = 0; I < Arguments + 2; ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Arch::AArch64;
    Parameter.Id = I;
    Parameter.RegOff = I * kPointerBytes;
    Parameter.Size = kPointerBytes;
    Function.Params.push_back(Parameter);
  }
  MedOp Authentication;
  Authentication.Opcode = NdOp::INTRINSIC;
  Authentication.Output.Kind = MedVar::Temp;
  Authentication.Output.Id = kAuthenticationTemporaryId;
  Authentication.Output.Size = kPointerBytes;
  Authentication.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_AuthBranch), 4));
  Authentication.addInput(Function.Params[0]);
  Authentication.addInput(MedVar::makeConst(Key, 4));
  Authentication.addInput(Zero ? MedVar::makeConst(0, kPointerBytes)
                               : Function.Params[1]);
  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Output.Kind = MedVar::Reg;
  Call.Output.TheArch = Arch::AArch64;
  Call.Output.RegOff = a64reg::X0;
  Call.Output.Id = kCallResultId;
  Call.Output.SSAVer = 1;
  Call.Output.Size = kPointerBytes;
  Call.addInput(Raw ? Function.Params[0] : Authentication.Output);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Call.Output);
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.back();
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  if (!Raw)
    Block.Ops.push_back(Authentication);
  Block.Ops.push_back(Call);
  Block.Ops.push_back(Return);
  MedCallInfo Info;
  Info.BlockId = 0;
  Info.OpIdx = Raw ? 0 : 1;
  Info.IsIndirect = true;
  Info.Args.assign(Function.Params.begin() + 2, Function.Params.end());
  Function.CallInfos.push_back(std::move(Info));
  if (Zero)
    Function.Params.erase(Function.Params.begin() + 1);
  return Function;
}

void save(llvm::StringRef Case, llvm::StringRef Name, llvm::StringRef Bytes) {
  const char *Directory = std::getenv(kArtifactEnvironment);
  if (!Directory || !*Directory)
    return;
  llvm::SmallString<kPathCapacity> Path(Directory);
  llvm::sys::path::append(Path, Case);
  ASSERT_FALSE(llvm::sys::fs::create_directories(Path));
  llvm::sys::path::append(Path, Name);
  std::error_code Error;
  llvm::raw_fd_ostream Out(Path, Error);
  ASSERT_FALSE(Error);
  Out << Bytes;
  Out.close();
  ASSERT_FALSE(Out.has_error());
  ::testing::Test::RecordProperty(kArtifactProperty, Directory);
}

void optimize(llvm::Module &Module) {
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
  for (auto &Function : Module)
    Function.removeFnAttr(llvm::Attribute::OptimizeNone);
  auto Pipeline =
      Passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  Pipeline.run(Module, Modules);
}

void checkBundle(llvm::Module &Module, unsigned Key, bool Zero,
                 unsigned Arguments, bool Raw = false) {
  ASSERT_FALSE(llvm::verifyModule(Module));
  const auto *Function = Module.getFunction(kFunction);
  ASSERT_NE(Function, nullptr);
  unsigned Calls = 0;
  for (const auto &Block : *Function)
    for (const auto &Instruction : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
          Call && Call->isIndirectCall() && !Call->isInlineAsm()) {
        ++Calls;
        EXPECT_EQ(Call->arg_size(), Arguments);
        const auto Bundle = Call->getOperandBundle(kBundle);
        if (Raw) {
          EXPECT_FALSE(Bundle);
          continue;
        }
        ASSERT_TRUE(Bundle);
        ASSERT_EQ(Bundle->Inputs.size(), 2u);
        const auto *ActualKey =
            llvm::dyn_cast<llvm::ConstantInt>(Bundle->Inputs[0]);
        ASSERT_NE(ActualKey, nullptr);
        const llvm::Value *ModifierValue = Bundle->Inputs[1];
        // Clang O0 can retain auth-and-resign followed by its default IA/0
        // call. Check the complete composition, including both keys and
        // discriminators; accepting an arbitrary IA/0 bundle would miss the
        // original double-authentication bug.
        if (const auto *Cast =
                llvm::dyn_cast<llvm::IntToPtrInst>(Call->getCalledOperand()))
          if (const auto *Resign =
                  llvm::dyn_cast<llvm::CallBase>(Cast->getOperand(0));
              Resign &&
              Resign->getIntrinsicID() == llvm::Intrinsic::ptrauth_resign) {
            ASSERT_EQ(Resign->arg_size(), 5u);
            EXPECT_EQ(Resign->getArgOperand(0), Function->getArg(0));
            EXPECT_EQ(Resign->getArgOperand(3), Bundle->Inputs[0]);
            EXPECT_EQ(Resign->getArgOperand(4), Bundle->Inputs[1]);
            ActualKey =
                llvm::dyn_cast<llvm::ConstantInt>(Resign->getArgOperand(1));
            ASSERT_NE(ActualKey, nullptr);
            ModifierValue = Resign->getArgOperand(2);
          }
        EXPECT_EQ(ActualKey->getZExtValue(), Key);
        if (Zero) {
          const auto *Modifier =
              llvm::dyn_cast<llvm::ConstantInt>(ModifierValue);
          ASSERT_NE(Modifier, nullptr);
          EXPECT_TRUE(Modifier->isZero());
        } else {
          EXPECT_EQ(ModifierValue, Function->getArg(1));
        }
      }
  EXPECT_EQ(Calls, 1u);
  EXPECT_EQ(Module.getFunction(kStandaloneAuthentication), nullptr);
}

void checkMachineCall(llvm::Module &Module, unsigned Key, bool Raw = false,
                      bool HiddenResult = false) {
  ASSERT_FALSE(llvm::verifyModule(Module));
  const auto *Function = Module.getFunction(kFunction);
  ASSERT_NE(Function, nullptr);
  EXPECT_FALSE(Function->hasFnAttribute(llvm::Attribute::NoUnwind));
  unsigned Calls = 0;
  for (const auto &Block : *Function)
    for (const auto &Instruction : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
        EXPECT_NE(Call->getIntrinsicID(), llvm::Intrinsic::ptrauth_resign);
        EXPECT_NE(Call->getIntrinsicID(), llvm::Intrinsic::ptrauth_auth);
        EXPECT_NE(Call->getIntrinsicID(), llvm::Intrinsic::trap);
        if (const auto *Assembly =
                llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
            Assembly && llvm::StringRef(Assembly->getAsmString())
                            .starts_with(kResultCallMarker)) {
          ++Calls;
          EXPECT_TRUE(Assembly->canThrow());
          EXPECT_TRUE(Assembly->hasSideEffects());
          EXPECT_TRUE(Call->getType()->isVoidTy());
          EXPECT_EQ(Call->arg_size(), HiddenResult ? 2U : 3U);
          EXPECT_EQ(Assembly->getConstraintString(),
                    HiddenResult ? kResultCallConstraints
                                 : kValueCallConstraints);
          const char *Branch = Raw                         ? kResultCallRaw
                               : Key == kAArch64PAuthKeyIA ? kResultCallIA
                                                           : kResultCallIB;
          EXPECT_NE(Assembly->getAsmString().find(Branch), std::string::npos);
        }
      }
  EXPECT_EQ(Calls, 1U);
}

void compile(const std::string &Source,
             const std::function<void(llvm::Module &)> &Inspect,
             llvm::StringRef Case, bool LLVMRoute, bool DarwinObject = false,
             llvm::ArrayRef<llvm::StringRef> ExtraFlags = {},
             bool CompilerObject = false) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Found = llvm::sys::findProgramByName(kCompiler);
  if (!Found)
    GTEST_SKIP() << kCompilerUnavailable;
  const std::string Compiler = *Found;
#endif
  llvm::SmallString<kPathCapacity> SourcePath, LLVMPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      kTemporaryPrefix, kSourceExtension, SourcePath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTemporaryPrefix,
                                                  kLLVMExtension, LLVMPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTemporaryPrefix,
                                                  kErrorExtension, ErrorPath));
  llvm::FileRemover RemoveSource(SourcePath), RemoveLLVM(LLVMPath),
      RemoveError(ErrorPath);
  std::error_code Error;
  llvm::raw_fd_ostream Out(SourcePath, Error);
  ASSERT_FALSE(Error);
  Out << Source;
  Out.close();
  for (llvm::StringRef Optimization : {kNoOptimize, kOptimize}) {
    llvm::SmallVector<llvm::StringRef, 24> Arguments = {
        Compiler,
        kTargetOption,
        kTriple,
        kFreestandingOption,
        kResourceHeadersOption,
        kIncludeOption,
        NEVERD_ARM64E_TEST_HEADERS,
        kStandardOption,
        kUnwindOption,
        kAsyncUnwindOption,
        Optimization,
        kAssemblyOption,
        kLLVMOption,
        SourcePath,
        kOutputOption,
        LLVMPath};
    Arguments.append(ExtraFlags.begin(), ExtraFlags.end());
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, ErrorPath.str()};
    std::string Diagnostic;
    const int Status =
        llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt, Redirects,
                                  kCompilerTimeoutSeconds, 0, &Diagnostic);
    auto Messages = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Diagnostic
                         << (Messages ? (*Messages)->getBuffer().str()
                                      : std::string())
                         << Source;
    llvm::LLVMContext Context;
    llvm::SMDiagnostic ParseDiagnostic;
    auto Module = llvm::parseIRFile(LLVMPath, ParseDiagnostic, Context);
    ASSERT_TRUE(Module);
    Module->setModuleIdentifier(kFunction);
    Module->setSourceFileName(kFunction);
    std::string IR;
    llvm::raw_string_ostream IROut(IR);
    Module->print(IROut, nullptr);
    const char *Route = LLVMRoute ? kLLVMRoute : kHighRoute;
    save(Case,
         llvm::formatv(kCompiledIR, Route, Optimization.drop_front()).str(),
         IR);
    if (CompilerObject) {
      // A native C compiler owns its object and PAC IR dialect. The r6 LLVM
      // importer separately refuses Apple Clang's legacy llvm.ptrauth globals.
      for (const auto Flag : {kAssemblyOption, kLLVMOption})
        Arguments.erase(std::remove(Arguments.begin(), Arguments.end(), Flag),
                        Arguments.end());
      Arguments.push_back(kCompileOption);
      const int ObjectStatus = llvm::sys::ExecuteAndWait(
          Compiler, Arguments, std::nullopt, Redirects, kCompilerTimeoutSeconds,
          0, &Diagnostic);
      Messages = llvm::MemoryBuffer::getFile(ErrorPath);
      ASSERT_EQ(ObjectStatus, 0)
          << Diagnostic
          << (Messages ? (*Messages)->getBuffer().str() : std::string());
      auto Bytes = llvm::MemoryBuffer::getFile(LLVMPath);
      ASSERT_TRUE(Bytes);
      save(Case,
           llvm::formatv(kCompiledObject, Route, Optimization.drop_front())
               .str(),
           (*Bytes)->getBuffer());
    } else {
      const auto Object = Codegen().compile(
          *Module, Arch::AArch64,
          DarwinObject ? BinaryFormat::MachO : BinaryFormat::ELF,
          DarwinObject ? kTriple : llvm::StringRef());
      ASSERT_TRUE(Object.Success);
      save(Case,
           llvm::formatv(kCompiledObject, Route, Optimization.drop_front())
               .str(),
           llvm::StringRef(
               reinterpret_cast<const char *>(Object.ObjectData.data()),
               Object.ObjectData.size()));
    }
    optimize(*Module);
    Inspect(*Module);
  }
}

TEST(AArch64AuthenticatedCall, BothKeysAndZeroModifiersRemainOnTheCall) {
  for (unsigned Key : {0u, 1u})
    for (bool Zero : {false, true})
      for (unsigned Arguments : {1u, kManyArguments}) {
        const std::string Case =
            llvm::formatv(kCaseDirectory, Key, Zero, Arguments).str();
        const auto Function = function(Key, Zero, Arguments);
        ASSERT_EQ(findAArch64AuthenticatedCalls(Function).size(), 1u);
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit({Function}, Context, kFunction,
                                            Arch::AArch64);
        ASSERT_TRUE(Module);
        optimize(*Module);
        checkBundle(*Module, Key, Zero, Arguments);
        std::string IR;
        llvm::raw_string_ostream IROut(IR);
        Module->print(IROut, nullptr);
        save(Case, kLLVMIR, IR);
        for (bool LLVMRoute : {false, true}) {
          CEmitterOptions Options;
          Options.TheArch = Arch::AArch64;
          Options.Format = BinaryFormat::MachO;
          Options.PreserveLLVMFunctionTypes = true;
          std::string Source;
          llvm::raw_string_ostream Stream(Source);
          if (LLVMRoute)
            ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, Options));
          else {
            auto High = MedToHighConverter().convert(Function, Arch::AArch64);
            walkStmts(High.Body, [&](HighStmt &Statement) {
              forEachExpr(Statement, [&](const ExprPtr &Expression) {
                if (Expression && Expression->Kind == ExprKind::Call &&
                    Expression->IsIndirectCall)
                  Expression->CallTarget = kResolvedCallback;
              });
            });
            ASSERT_TRUE(HighCEmitter().emit({High}, Stream, Options));
          }
          EXPECT_NE(Source.find(kResignBuiltin), std::string::npos) << Source;
          save(Case, LLVMRoute ? kLLVMSource : kHighSource, Source);
          compile(
              Source,
              [&](llvm::Module &Recompiled) {
                checkBundle(Recompiled, Key, Zero, Arguments);
              },
              Case, LLVMRoute);
        }
        for (auto Format : {BinaryFormat::ELF, BinaryFormat::MachO}) {
          const auto Object = Codegen().compile(*Module, Arch::AArch64, Format);
          ASSERT_TRUE(Object.Success);
          save(Case, Format == BinaryFormat::ELF ? kELFObject : kMachOObject,
               llvm::StringRef(
                   reinterpret_cast<const char *>(Object.ObjectData.data()),
                   Object.ObjectData.size()));
        }
      }
}

void checkTypedShape(const TypedShape &Shape, bool Native) {
  for (const auto &Variant : TypedVariants) {
    const auto Case =
        llvm::formatv(Native ? kRecoveredTypedDirectory : kTypedDirectory,
                      Shape.Name, Variant.Key, Variant.Zero, Variant.Raw)
            .str();
    SCOPED_TRACE(Case);
    // All entry carriers remain full-width integers so the independently
    // compiled ELF probe and Darwin wrapper have the same outer ABI. Only
    // the inner call projects the original scalar types and PCS.
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->AnalystFixedCount = Shape.Fixed;
    const auto ReturnType = typedParameter(Shape.ReturnKind);
    Hint->Signature.ReturnType = ReturnType;
    for (char Kind : Shape.Parameters)
      Hint->Signature.Parameters.push_back(
          {kTypedArgument, typedParameter(Kind)});
    std::string Diagnostic;
    if (Native)
      Hint->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    ASSERT_TRUE(Native ? assignDarwinFixedSourceABI(Hint->Signature,
                                                    Arch::AArch64, Diagnostic)
                       : assignAnalystCallABI(*Hint, Arch::AArch64,
                                              BinaryFormat::MachO, Diagnostic))
        << Diagnostic;
    const auto Bindings = sourceABIParameters(Hint->Signature);
    unsigned Words = 0;
    for (const auto &Binding : Bindings)
      Words += (Binding.Type->Size + kPointerBytes - 1) / kPointerBytes;
    auto Function = function(Variant.Key, false, Words, Variant.Raw);
    const bool VectorReturn = isSourceAArch64Vector128Type(ReturnType);
    if (VectorReturn) {
      Function.ReturnType = NdType::makeStruct(
          {NdType::makeInt(kPointerBytes), NdType::makeInt(kPointerBytes)});
      Function.MultiReturn = {{a64reg::X0, kPointerBytes, false},
                              {a64reg::X1, kPointerBytes, false}};
    }
    auto &Block = Function.Blocks.front();
    MedOp Call = Block.Ops[Variant.Raw ? 0 : 1];
    Hint->CallKind = Native ? SourceCallTypeHint::Kind::Native
                            : SourceCallTypeHint::Kind::Analyst;
    if (Native) {
      Hint->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Hint->TargetName = kResolvedCallback;
    }
    Call.Addr = kEntryAddress;
    Call.OriginSeq = 0;
    Call.Output.Size = ReturnType->Size;
    if (!Native)
      Hint->AnalystSite = SourceCallOccurrenceKey{Call.Addr, Call.OriginSeq,
                                                  NdOp::INDIR_CALL, 0};
    Hint->AnalystFixedCount = Shape.Fixed;
    Hint->Signature.ReturnType = ReturnType;
    std::vector<MedOp> Setup;
    std::vector<MedVar> Arguments;
    unsigned Word = 2;
    for (unsigned I = 0; I < Bindings.size(); ++I) {
      const auto Type = Bindings[I].Type;
      auto Argument = Function.Params[Word++];
      if (Type->Size > kPointerBytes) {
        MedOp Join;
        Join.Opcode = NdOp::CONCAT;
        Join.Output.Kind = MedVar::Temp;
        Join.Output.Id = kTypedTemporaryId + I;
        Join.Output.Size = Type->Size;
        Join.addInput(Function.Params[Word++]);
        Join.addInput(Argument);
        Argument = Join.Output;
        Setup.push_back(std::move(Join));
      }
      if (Type->Size < Argument.Size) {
        MedOp Narrow;
        Narrow.Opcode = NdOp::COPY;
        Narrow.Output.Kind = MedVar::Temp;
        Narrow.Output.Id = kTypedTemporaryId + I;
        Narrow.Output.Size = Type->Size;
        Narrow.addInput(Argument);
        Argument = Narrow.Output;
        Setup.push_back(std::move(Narrow));
      }
      Call.addInput(Argument);
      Arguments.push_back(Argument);
    }
    Call.SourceCallHint = Hint;
    if (!Variant.Raw) {
      auto Authentication = Block.Ops.front();
      if (Variant.Zero)
        Authentication.Inputs[3] = MedVar::makeConst(0, kPointerBytes);
      Setup.push_back(std::move(Authentication));
    }
    auto &Info = Function.CallInfos.front();
    Info.OpIdx = Setup.size();
    Info.Args = std::move(Arguments);
    Info.SourceCallHint = Hint;
    if (ReturnType->Kind == NdTypeKind::Struct ||
        ReturnType->Size > kPointerBytes)
      Call.Output.Kind = MedVar::Temp;
    Setup.push_back(Call);
    auto Return = Block.Ops.back();
    Return.Inputs[0] = Call.Output;
    if (ReturnType->Size < kPromotedIntegerBytes) {
      // The outer fixture returns source bits in a full-width integer.
      // A byte/halfword register result alone does not define all X0 bits.
      MedOp Widen;
      Widen.Opcode = NdOp::INT_ZEXT;
      Widen.Output = Call.Output;
      ++Widen.Output.SSAVer;
      Widen.Output.Size = kPointerBytes;
      Widen.addInput(Call.Output);
      Return.Inputs[0] = Widen.Output;
      Setup.push_back(std::move(Widen));
    }
    if (VectorReturn) {
      // The outer ABI observes both words independently through X0/X1. A
      // temporary alone is not an AArch64 physical return-register definition.
      for (unsigned I = 0; I < Function.MultiReturn.size(); ++I) {
        MedOp Extract;
        Extract.Opcode = NdOp::SUBBYTES;
        Extract.Output = Call.Output;
        Extract.Output.Kind = MedVar::Reg;
        Extract.Output.Id = kTypedTemporaryId + Bindings.size() + I;
        Extract.Output.RegOff = Function.MultiReturn[I].RegOff;
        Extract.Output.Size = kPointerBytes;
        ++Extract.Output.SSAVer;
        Extract.addInput(Call.Output);
        Extract.addInput(
            MedVar::makeConst(I * kPointerBytes, kPromotedIntegerBytes));
        if (!I)
          Return.Inputs[0] = Extract.Output;
        Setup.push_back(std::move(Extract));
      }
    }
    if (!VectorReturn && (ReturnType->Kind == NdTypeKind::Struct ||
                          ReturnType->Size > kPointerBytes)) {
      MedVar Accumulator = MedVar::makeConst(0, kPointerBytes);
      int Id = kTypedTemporaryId + Bindings.size();
      unsigned Version = Call.Output.SSAVer;
      for (const auto &Member : sourceABIValueMembers(ReturnType)) {
        MedOp Extract;
        Extract.Opcode = NdOp::SUBBYTES;
        Extract.Output.Kind = MedVar::Temp;
        Extract.Output.Id = Id++;
        Extract.Output.Size = Member.Type->Size;
        Extract.addInput(Call.Output);
        Extract.addInput(
            MedVar::makeConst(Member.ByteOffset, kPromotedIntegerBytes));
        auto Value = Extract.Output;
        Setup.push_back(std::move(Extract));
        if (Value.Size < kPointerBytes) {
          MedOp Extend;
          Extend.Opcode = NdOp::INT_ZEXT;
          Extend.Output.Kind = MedVar::Temp;
          Extend.Output.Id = Id++;
          Extend.Output.Size = kPointerBytes;
          Extend.addInput(Value);
          Value = Extend.Output;
          Setup.push_back(std::move(Extend));
        }
        MedOp Mix;
        Mix.Opcode = NdOp::INT_XOR;
        Mix.Output = Return.Inputs[0];
        Mix.Output.Kind = MedVar::Reg;
        Mix.Output.Id = Id++;
        Mix.Output.RegOff = a64reg::X0;
        Mix.Output.Size = kPointerBytes;
        Mix.Output.SSAVer = ++Version;
        Mix.addInput(Accumulator);
        Mix.addInput(Value);
        Accumulator = Mix.Output;
        Setup.push_back(std::move(Mix));
      }
      Return.Inputs[0] = Accumulator;
    }
    Setup.push_back(std::move(Return));
    Block.Ops = std::move(Setup);
    MedLLVMEmitter Emitter;
    Emitter.setAnalystSourceProjection(!Native);
    llvm::LLVMContext Context;
    auto Module = Emitter.emit({Function}, Context, kFunction, Arch::AArch64,
                               {}, nullptr, BinaryFormat::MachO);
    ASSERT_TRUE(Module);
    optimize(*Module);
    const bool MachineCall =
        sourceCallUsesMachineABI(*Hint, BinaryFormat::MachO, !Variant.Raw);
    const auto Inspect = [&](llvm::Module &Candidate) {
      if (!MachineCall)
        checkBundle(Candidate, Variant.Key, Variant.Zero,
                    Shape.Parameters.size(), true);
      else
        checkMachineCall(Candidate, Variant.Key, Variant.Raw);
      for (const auto &Block : *Candidate.getFunction(kFunction))
        for (const auto &Instruction : Block) {
          if (const auto *Return =
                  llvm::dyn_cast<llvm::ReturnInst>(&Instruction))
            EXPECT_FALSE(llvm::isa<llvm::Constant>(Return->getReturnValue()));
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
              Call && Call->isIndirectCall() && !Call->isInlineAsm()) {
            const auto *Type = Call->getFunctionType();
            EXPECT_EQ(Type->isVarArg(), Shape.Fixed >= 0);
            EXPECT_EQ(Type->getNumParams(), Shape.Fixed >= 0
                                                ? unsigned(Shape.Fixed)
                                                : Shape.Parameters.size());
            const auto Width = [&](llvm::Type *Type) {
              return Candidate.getDataLayout()
                  .getTypeStoreSizeInBits(Type)
                  .getFixedValue();
            };
            EXPECT_EQ(Width(Call->getType()), ReturnType->Size * kBitsPerByte);
            EXPECT_EQ(Call->getType()->isFloatingPointTy(),
                      ReturnType->Kind == NdTypeKind::Float);
            if (ReturnType->Kind == NdTypeKind::Int &&
                ReturnType->Size < kPromotedIntegerBytes)
              EXPECT_TRUE(Call->hasRetAttr(ReturnType->IsSigned
                                               ? llvm::Attribute::SExt
                                               : llvm::Attribute::ZExt));
            for (unsigned I = 0; I < Shape.Parameters.size(); ++I) {
              auto *Argument = Call->getArgOperand(I)->getType();
              const auto Expected =
                  sourceCallParameterType(Hint->Signature.Parameters[I]);
              EXPECT_EQ(Width(Argument), Expected->Size * kBitsPerByte);
              if (Hint->Signature.Parameters[I].IndirectByValue) {
                EXPECT_TRUE(Argument->isPointerTy());
#define A64_CALL_INDIRECT_FORBIDDEN_ATTRIBUTE(Name)                            \
  EXPECT_FALSE(Call->paramHasAttr(I, llvm::Attribute::Name));
#include "AArch64AuthenticatedCallFixtures.def"
              }
              if (Expected->Kind == NdTypeKind::Int &&
                  Expected->Size < kPromotedIntegerBytes &&
                  Hint->Signature.Parameters[I].Location.Kind ==
                      SourceABICarrierKind::IntegerRegister)
                EXPECT_TRUE(Call->paramHasAttr(I, Expected->IsSigned
                                                      ? llvm::Attribute::SExt
                                                      : llvm::Attribute::ZExt));
              EXPECT_EQ(Argument->isFloatingPointTy(),
                        Expected->Kind == NdTypeKind::Float);
            }
          }
        }
    };
    Inspect(*Module);
    std::string IR;
    llvm::raw_string_ostream IROut(IR);
    Module->print(IROut, nullptr);
    save(Case, kLLVMIR, IR);
    for (bool LLVMRoute : {false, true}) {
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Format = BinaryFormat::MachO;
      Options.PreserveLLVMFunctionTypes = true;
      std::string Source;
      llvm::raw_string_ostream Stream(Source);
      if (LLVMRoute)
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, Options));
      else
        ASSERT_TRUE(HighCEmitter().emit(
            {MedToHighConverter().convert(Function, Arch::AArch64)}, Stream,
            Options));
      EXPECT_TRUE(llvm::StringRef(Source).contains(
          MachineCall ? llvm::StringRef(kResultCallMarker).rtrim()
                      : llvm::StringRef(kRawCallMacro)))
          << Source;
      save(Case, LLVMRoute ? kLLVMSource : kHighSource, Source);
      llvm::SmallVector<llvm::StringRef, 2> Flags;
      if (MachineCall)
        Flags = {kExceptionsOption, kNoStackProtectorOption};
      compile(Source, Inspect, Case, LLVMRoute, true, Flags);
    }
    const auto Object =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO, kTriple);
    ASSERT_TRUE(Object.Success);
    save(Case, kELFObject,
         llvm::StringRef(
             reinterpret_cast<const char *>(Object.ObjectData.data()),
             Object.ObjectData.size()));
  }
}

class AArch64TypedAuthenticatedCall
    : public ::testing::TestWithParam<TypedShape> {};

TEST_P(AArch64TypedAuthenticatedCall,
       ExplicitTypesKeepAuthenticationOnTheCall) {
  checkTypedShape(GetParam(), false);
}

class AArch64RecoveredAuthenticatedCall
    : public ::testing::TestWithParam<TypedShape> {};

TEST_P(AArch64RecoveredAuthenticatedCall,
       NativeTypesKeepAuthenticationOnTheCall) {
  checkTypedShape(GetParam(), true);
}

std::vector<TypedShape> recoveredShapes() {
  std::vector<TypedShape> Shapes;
  for (const auto &Shape : TypedShapes) {
    if (Shape.Fixed >= 0)
      continue;
    SourceCallTypeHint Hint;
    Hint.Signature.ReturnType = typedParameter(Shape.ReturnKind);
    for (char Kind : Shape.Parameters)
      Hint.Signature.Parameters.push_back(
          {kTypedArgument, typedParameter(Kind)});
    std::string Diagnostic;
    if (assignAnalystCallABI(Hint, Arch::AArch64, BinaryFormat::MachO,
                             Diagnostic) &&
        Hint.Signature.ReturnLocation.Kind !=
            SourceABICarrierKind::IndirectResultPointer)
      Shapes.push_back(Shape);
  }
#define A64_CALL_NATIVE_SHAPE(Name, Parameters, ReturnKind)                    \
  Shapes.push_back({Name, Parameters, ReturnKind, -1});
#include "AArch64AuthenticatedCallFixtures.def"
  return Shapes;
}

INSTANTIATE_TEST_SUITE_P(ByShape, AArch64RecoveredAuthenticatedCall,
                         ::testing::ValuesIn(recoveredShapes()),
                         [](const ::testing::TestParamInfo<TypedShape> &Info) {
                           std::string Name = Info.param.Name.str();
                           std::replace(Name.begin(), Name.end(), '-', '_');
                           return Name;
                         });

INSTANTIATE_TEST_SUITE_P(ByShape, AArch64TypedAuthenticatedCall,
                         ::testing::ValuesIn(TypedShapes),
                         [](const ::testing::TestParamInfo<TypedShape> &Info) {
                           std::string Name = Info.param.Name.str();
                           std::replace(Name.begin(), Name.end(), '-', '_');
                           return Name;
                         });

TEST(AArch64AuthenticatedCall, NativeTypesKeepTheOriginalIndirectTarget) {
  for (const auto &Variant : TypedVariants) {
    const auto Case = llvm::formatv(kNativeTypedDirectory, Variant.Key,
                                    Variant.Zero, Variant.Raw)
                          .str();
    SCOPED_TRACE(Case);
    auto Function = function(Variant.Key, false, 1, Variant.Raw);
    auto &Call = Function.Blocks.front().Ops[Variant.Raw ? 0 : 1];
    Call.Addr = kEntryAddress;
    Call.OriginSeq = 0;
    Call.addInput(Function.Params[2]);
    if (!Variant.Raw && Variant.Zero)
      Function.Blocks.front().Ops.front().Inputs[3] =
          MedVar::makeConst(0, kPointerBytes);
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->Signature.ReturnType = NdType::makeFloat(kPointerBytes);
    Hint->Signature.Parameters = {
        {kTypedArgument, NdType::makeFloat(kPointerBytes)}};
    std::string Diagnostic;
    ASSERT_TRUE(assignAnalystCallABI(*Hint, Arch::AArch64, BinaryFormat::MachO,
                                     Diagnostic))
        << Diagnostic;
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint->TargetName = kResolvedCallback;
    Call.SourceCallHint = Hint;
    Function.CallInfos.front().SourceCallHint = Hint;
    auto High = MedToHighConverter().convert(Function, Arch::AArch64);
    walkStmts(High.Body, [&](HighStmt &Statement) {
      forEachExpr(Statement, [&](const ExprPtr &Expression) {
        if (Expression && Expression->SourceCallHint)
          Expression->CallTarget = kResolvedCallback;
      });
    });
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(HighCEmitter().emit({High}, Out, Options));
    EXPECT_TRUE(llvm::StringRef(Source).contains(
        Variant.Raw ? llvm::StringRef(kRawCallMacro)
                    : llvm::StringRef(kResultCallMarker).rtrim()))
        << Source;
    EXPECT_EQ(Source.find(kResignBuiltin), std::string::npos) << Source;
    save(Case, kHighSource, Source);
    compile(
        Source,
        [&](llvm::Module &Recompiled) {
          if (Variant.Raw)
            checkBundle(Recompiled, Variant.Key, Variant.Zero, 1, true);
          else
            checkMachineCall(Recompiled, Variant.Key);
        },
        Case, false, true,
        Variant.Raw ? llvm::ArrayRef<llvm::StringRef>()
                    : llvm::ArrayRef<llvm::StringRef>{kExceptionsOption,
                                                      kNoStackProtectorOption});
    if (!Variant.Raw && !Variant.Zero) {
      auto TypedCaller = High;
      TypedCaller.Params[2].Type = NdType::makeFloat(kPointerBytes);
      std::string TypedSource;
      llvm::raw_string_ostream TypedOut(TypedSource);
      ASSERT_TRUE(HighCEmitter().emit({TypedCaller}, TypedOut, Options));
      const auto TypedCase =
          llvm::formatv(kNativeTypedCallerDirectory, Variant.Key, Variant.Zero)
              .str();
      save(TypedCase, kHighSource, TypedSource);
      compile(TypedSource,
              [&](llvm::Module &Recompiled) {
                checkMachineCall(Recompiled, Variant.Key);
                for (const auto &Block : *Recompiled.getFunction(kFunction))
                  for (const auto &Instruction : Block) {
                    EXPECT_FALSE(llvm::isa<llvm::FPToSIInst>(Instruction));
                    EXPECT_FALSE(llvm::isa<llvm::FPToUIInst>(Instruction));
                    EXPECT_FALSE(llvm::isa<llvm::SIToFPInst>(Instruction));
                    EXPECT_FALSE(llvm::isa<llvm::UIToFPInst>(Instruction));
                  }
              },
              TypedCase, false, true,
              {kExceptionsOption, kNoStackProtectorOption});
    }
    if (!Variant.Raw) {
      const auto Emit = [&] {
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({Function}, Context, kFunction, Arch::AArch64,
                                  {}, nullptr, BinaryFormat::MachO);
        if (Module)
          checkMachineCall(*Module, Variant.Key);
        return bool(Module);
      };
      EXPECT_TRUE(Emit());
      // A recovered display name is optional in LLVM and never supplies the
      // callable address. HighIR above retains its own resolved display name.
      Hint->TargetName.clear();
      EXPECT_TRUE(Emit());
      Hint->TargetName = kResolvedCallback;
      Hint->Signature.HasExplicitABI = false;
      EXPECT_FALSE(Emit());
      Hint->Signature.HasExplicitABI = true;
      auto &Argument = Function.CallInfos.front().Args.front();
      const auto Width = Argument.Size;
      Argument.Size = kPromotedIntegerBytes;
      EXPECT_FALSE(Emit());
      Argument.Size = Width;
      const auto Parameter = Hint->Signature.Parameters.front();
      auto &Vector = Hint->Signature.Parameters.front();
      Vector.Type = NdType::makeInt(2 * kPointerBytes, false);
      Vector.Type->SourceName = kSourceAArch64Vector128CType;
      Vector.Location.ValueBytes = Vector.Type->Size;
      ASSERT_TRUE(validateSourceABI(Hint->Signature, Diagnostic)) << Diagnostic;
      EXPECT_FALSE(Emit());
      std::string WideRefused;
      llvm::raw_string_ostream WideOut(WideRefused);
      EXPECT_THROW(HighCEmitter().emit({High}, WideOut, Options),
                   std::invalid_argument);
      Vector = Parameter;
      Hint->Signature.Convention =
          SourceFunctionTypeHint::ConventionKind::Swift;
      EXPECT_FALSE(Emit());
      std::string Refused;
      llvm::raw_string_ostream RefusedOut(Refused);
      EXPECT_THROW(HighCEmitter().emit({High}, RefusedOut, Options),
                   std::invalid_argument);
    }
  }
}

TEST(AArch64AuthenticatedCall, OrdinaryRawCallsAcquireNoDefaultAuthentication) {
  const auto Function = function(0, true, 1, true);
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Function}, Context, kFunction, Arch::AArch64);
  ASSERT_TRUE(Module);
  optimize(*Module);
  std::string IR;
  llvm::raw_string_ostream IROut(IR);
  Module->print(IROut, nullptr);
  const auto Case = llvm::formatv(kRawCaseDirectory, 1).str();
  save(Case, kLLVMIR, IR);
  const auto Object =
      Codegen().compile(*Module, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(Object.Success);
  save(Case, kELFObject,
       llvm::StringRef(reinterpret_cast<const char *>(Object.ObjectData.data()),
                       Object.ObjectData.size()));
  for (bool LLVMRoute : {false, true}) {
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    if (LLVMRoute)
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, Options));
    else
      ASSERT_TRUE(HighCEmitter().emit(
          {MedToHighConverter().convert(Function, Arch::AArch64)}, Stream,
          Options));
    EXPECT_NE(Source.find(kRawCallMacro), std::string::npos) << Source;
    save(llvm::formatv(kRawCaseDirectory, 1).str(),
         LLVMRoute ? kLLVMSource : kHighSource, Source);
    compile(
        Source,
        [&](llvm::Module &Recompiled) {
          checkBundle(Recompiled, 0, true, 1, true);
        },
        Case, LLVMRoute);
  }
}

TEST(AArch64AuthenticatedCall,
     ObservableValuesAndInterveningEffectsStayExplicit) {
  const auto Original = function(1, false, 1);
  ASSERT_EQ(findAArch64AuthenticatedCalls(Original).size(), 1u);
  auto ExtraUse = Original;
  ExtraUse.CallInfos.front().Args.push_back(
      ExtraUse.Blocks.front().Ops.front().Output);
  EXPECT_TRUE(findAArch64AuthenticatedCalls(ExtraUse).empty());
  auto Memory = Original;
  MedOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Memory.Params[0]);
  Store.addInput(Memory.Params[1]);
  Memory.Blocks.front().Ops.insert(Memory.Blocks.front().Ops.begin() + 1,
                                   Store);
  EXPECT_TRUE(findAArch64AuthenticatedCalls(Memory).empty());
  auto StackRead = Original;
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = StackRead.Blocks.front().Ops.front().Output;
  ++Copy.Output.Id;
  MedVar Stack;
  Stack.Kind = MedVar::Stack;
  Stack.Size = kPointerBytes;
  Copy.addInput(Stack);
  StackRead.Blocks.front().Ops.insert(StackRead.Blocks.front().Ops.begin() + 1,
                                      Copy);
  EXPECT_TRUE(findAArch64AuthenticatedCalls(StackRead).empty());
  auto Mutable = Original;
  Mutable.SkippedSSA = true;
  EXPECT_TRUE(findAArch64AuthenticatedCalls(Mutable).empty());
  auto Dead = Original;
  Dead.Blocks.front().Ops[1].Dead = true;
  EXPECT_TRUE(findAArch64AuthenticatedCalls(Dead).empty());
}

TEST(AArch64AuthenticatedCall, MalformedBundlesRejectBeforeOutput) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    llvm::LLVMContext Context;
    llvm::Module Module(kFunction, Context);
    llvm::IRBuilder<> Builder(Context);
    auto *Type = llvm::FunctionType::get(
        Builder.getInt64Ty(), {Builder.getPtrTy(), Builder.getInt64Ty()},
        false);
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, kFunction, Module);
    Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, {}, Function));
    llvm::Value *Key = Case == 1   ? Builder.CreateTrunc(Function->getArg(1),
                                                         Builder.getInt32Ty())
                       : Case == 2 ? Builder.getInt64(0)
                                   : Builder.getInt32(Case == 3 ? 2 : 0);
    llvm::Value *Modifier = Function->getArg(1);
    if (Case == 4)
      Modifier = Builder.getInt32(0);
    llvm::SmallVector<llvm::Value *, 2> Inputs{Key, Modifier};
    llvm::OperandBundleDef Bundle(kBundle, Inputs);
    auto *Call =
        Builder.CreateCall(llvm::FunctionType::get(Builder.getInt64Ty(), false),
                           Function->getArg(0), {}, {Bundle});
    if (Case == 5)
      Call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    Builder.CreateRet(Call);
    CEmitterOptions Options;
    Options.TheArch = Case == 0 ? Arch::X64 : Arch::AArch64;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_THROW(LLVMCEmitter().emit(Module, Out, Options),
                 std::invalid_argument);
    EXPECT_TRUE(Source.empty());
  }
}

TEST(AArch64AuthenticatedCall, OriginalLinkRegisterOperandsSurviveTheNewLink) {
  struct Case {
    uint32_t Word;
    unsigned Key;
    bool Zero, TargetLR, ModifierLR;
  };
  const Case Cases[] = {
#define A64_CALL_WORD(Word, Key, Zero, TargetLR, ModifierLR)                   \
  {Word, Key, Zero, TargetLR, ModifierLR},
#include "AArch64AuthenticatedCallFixtures.def"
  };
  for (const auto &Case : Cases) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::ELF;
    Image.Bits = Bitness::Bits64;
    const std::array Words{kSaveFrame,
                           Case.TargetLR ? kTargetX30 : kTargetX16,
                           Case.ModifierLR ? kModifierX30 : kModifierX17,
                           kArgumentX0,
                           Case.Word,
                           kAddFive,
                           kRestoreFrame,
                           kReturn};
    Segment Text;
    Text.VA = kEntryAddress;
    Text.Size = Text.FileSz = Words.size() * sizeof(uint32_t);
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    for (size_t I = 0; I < Words.size(); ++I)
      writeLE<uint32_t>(Text.Data.data() + I * sizeof(uint32_t), Words[I]);
    Image.Segments.push_back(std::move(Text));
    Decoder Decoder;
    ASSERT_TRUE(Decoder.init(Arch::AArch64));
    Decoder.setStrict(true);
    auto Low = CFGBuilder().build(Image, Decoder, kEntryAddress, kFunction);
    auto Function =
        LowToMedConverter().convert(Low, Arch::AArch64, BinaryFormat::ELF);
    const auto Calls = findAArch64AuthenticatedCalls(Function);
    ASSERT_EQ(Calls.size(), 1u) << Case.Word;
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kFunction, Arch::AArch64);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    EXPECT_EQ(Module->getFunction(kStandaloneAuthentication), nullptr);
    optimize(*Module);
    unsigned CallsWithBundle = 0;
    for (const auto &Block : *Module->getFunction(kFunction))
      for (const auto &Instruction : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          if (const auto Bundle = Call->getOperandBundle(kBundle)) {
            ++CallsWithBundle;
            EXPECT_EQ(llvm::cast<llvm::ConstantInt>(Bundle->Inputs[0])
                          ->getZExtValue(),
                      Case.Key);
            EXPECT_FALSE(
                llvm::isa<llvm::ConstantInt>(Call->getCalledOperand()));
          }
    EXPECT_EQ(CallsWithBundle, 1u);
  }
}
} // namespace

void checkResultStorage(bool Native) {
  for (const auto &Variant : TypedVariants) {
    auto Function = function(Variant.Key, false, 2, Variant.Raw);
    auto &Block = Function.Blocks.front();
    auto &Call = Block.Ops[Variant.Raw ? 0 : 1];
    Call.Addr = kEntryAddress;
    Call.OriginSeq = 0;
    // AArch64 RETURN can name the return address. Model the actual x0 write
    // so both value and register-based consumers see the dispatcher's result.
    MedOp Result;
    Result.Opcode = NdOp::COPY;
    Result.Output = Call.Output;
    Result.addInput(MedVar::makeConst(0, kPointerBytes));
    Call.Output = {};
    for (unsigned I = 2; I < Function.Params.size(); ++I)
      Call.addInput(Function.Params[I]);
    if (!Variant.Raw && Variant.Zero)
      Block.Ops.front().Inputs[3] = MedVar::makeConst(0, kPointerBytes);
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = Native ? SourceCallTypeHint::Kind::Native
                            : SourceCallTypeHint::Kind::Analyst;
    if (Native) {
      Hint->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Hint->TargetName = kResolvedCallback;
    }
    Hint->Signature.ReturnType = NdType::makeStruct(
        {NdType::makeInt(kPointerBytes), NdType::makeFloat(kPointerBytes),
         NdType::makePtr(NdType::makeVoid())});
    Hint->Signature.Parameters = {{kTypedArgument, Hint->Signature.ReturnType}};
    if (!Native)
      Hint->AnalystSite = SourceCallOccurrenceKey{Call.Addr, Call.OriginSeq,
                                                  NdOp::INDIR_CALL, 0};
    std::string Diagnostic;
    ASSERT_TRUE(Native ? assignDarwinFixedSourceABI(Hint->Signature,
                                                    Arch::AArch64, Diagnostic)
                       : assignAnalystCallABI(*Hint, Arch::AArch64,
                                              BinaryFormat::MachO, Diagnostic))
        << Diagnostic;
    Call.SourceCallHint = Hint;
    Block.Ops.insert(Block.Ops.end() - 1, std::move(Result));
    recoverCallAbi(Function, Arch::AArch64, {});
    ASSERT_EQ(Function.CallInfos.size(), 1U);
    ASSERT_EQ(Function.CallInfos.front().SourceCallHint, Hint);
    ASSERT_EQ(Function.CallInfos.front().Args.size(),
              sourceABIParameters(Hint->Signature).size() + 1);
    MedLLVMEmitter Emitter;
    Emitter.setAnalystSourceProjection(!Native);
    llvm::LLVMContext Context;
    auto Module = Emitter.emit({Function}, Context, kFunction, Arch::AArch64,
                               {}, nullptr, BinaryFormat::MachO);
    ASSERT_TRUE(Module);
    const auto Inspect = [&](llvm::Module &Candidate) {
      EXPECT_FALSE(llvm::verifyModule(Candidate));
      unsigned Calls = 0;
      const auto *Fn = Candidate.getFunction(kFunction);
      ASSERT_TRUE(Fn);
      EXPECT_FALSE(Fn->hasFnAttribute(llvm::Attribute::NoUnwind));
      for (unsigned I = 0; I < Fn->arg_size(); ++I) {
#define A64_CALL_INDIRECT_FORBIDDEN_ATTRIBUTE(Name)                            \
  EXPECT_FALSE(Fn->hasParamAttribute(I, llvm::Attribute::Name));
#include "AArch64AuthenticatedCallFixtures.def"
        EXPECT_FALSE(Fn->hasParamAttribute(I, llvm::Attribute::StructRet));
      }
      for (const auto &B : *Fn)
        for (const auto &I : B)
          if (const auto *C = llvm::dyn_cast<llvm::CallBase>(&I)) {
            EXPECT_NE(C->getIntrinsicID(), llvm::Intrinsic::trap);
            if (const auto *Assembly =
                    llvm::dyn_cast<llvm::InlineAsm>(C->getCalledOperand());
                Assembly && llvm::StringRef(Assembly->getAsmString())
                                .starts_with(kResultCallMarker)) {
              ++Calls;
              EXPECT_TRUE(Assembly->canThrow());
              EXPECT_FALSE(C->hasFnAttr(llvm::Attribute::NoUnwind));
              EXPECT_TRUE(C->getType()->isVoidTy());
              const char *Branch = Variant.Raw ? kResultCallRaw
                                   : Variant.Key == kAArch64PAuthKeyIA
                                       ? kResultCallIA
                                       : kResultCallIB;
              EXPECT_NE(Assembly->getAsmString().find(Branch),
                        std::string::npos);
            }
          }
      EXPECT_EQ(Calls, 1U);
    };
    Inspect(*Module);
    optimize(*Module);
    Inspect(*Module);
    const auto Case = llvm::formatv(Native ? kRecoveredResultStorageDirectory
                                           : kResultStorageDirectory,
                                    Variant.Key, Variant.Zero, Variant.Raw)
                          .str();
    std::string IR;
    llvm::raw_string_ostream IRStream(IR);
    Module->print(IRStream, nullptr);
    save(Case, kLLVMIR, IR);
    const auto Object =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO, kTriple);
    ASSERT_TRUE(Object.Success);
    save(Case, kMachOObject,
         llvm::StringRef(
             reinterpret_cast<const char *>(Object.ObjectData.data()),
             Object.ObjectData.size()));
    for (const bool LLVMRoute : {false, true}) {
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Format = BinaryFormat::MachO;
      Options.PreserveLLVMFunctionTypes = true;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVMRoute)
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      else {
        const auto High = MedToHighConverter().convert(Function, Arch::AArch64);
        ASSERT_TRUE(HighCEmitter().emit({High}, Out, Options));
      }
      save(Case, LLVMRoute ? kLLVMSource : kHighSource, Source);
      // The FVP guest consumes the unchanged text without a Darwin runtime.
      // Public SDK executions independently retain the compiler's defaults.
      compile(Source, Inspect, Case, LLVMRoute, /*DarwinObject=*/true,
              {kExceptionsOption, kNoStackProtectorOption});
    }
  }
}

TEST(AArch64AuthenticatedCall, IndirectResultsKeepOriginalStorageAndPAC) {
  checkResultStorage(false);
}

TEST(AArch64AuthenticatedCall, NativeIndirectResultsKeepOriginalStorageAndPAC) {
  checkResultStorage(true);
}

TEST(AArch64AuthenticatedCall, LoweringRetainsNativeVectorAndOriginalX8) {
  const auto Word = NdType::makeInt(kPointerBytes);
  const auto Pointer = NdType::makePtr(Word);
  const auto Vector = typedParameter(kVectorKind);
  const auto Record = NdType::makeStruct({Word, Word, Word});
  SourceFunctionTypeHint Entry, Callee;
  Entry.Origin = Callee.Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Entry.ReturnType = Word;
  Entry.Parameters = {{kTypedArgument, Pointer},
                      {kTypedArgument, Pointer},
                      {kTypedArgument, Vector}};
  Callee.ReturnType = Record;
  Callee.Parameters = {{kTypedArgument, Record}, {kTypedArgument, Vector}};
  std::string Diagnostic;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Entry, Arch::AArch64, Diagnostic))
      << Diagnostic;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Callee, Arch::AArch64, Diagnostic))
      << Diagnostic;
  LowFunc Low;
  Low.Entry = kEntryAddress;
  Low.Name = kFunction;
  Low.Blocks.emplace_back();
  auto &Block = Low.Blocks.back();
  Block.Id = 0;
  Block.StartAddr = Low.Entry;
  auto Add = [&](NdOp Opcode, NdVar Output,
                 std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = Low.Entry + Block.Ops.size() * kInstructionBytes;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Block.Ops.push_back(Op);
  };
  Add(NdOp::COPY, NdVar::reg(a64reg::X8, kPointerBytes),
      {NdVar::reg(a64reg::X1, kPointerBytes)});
  Add(NdOp::CALL, {}, {NdVar::cst(kCalleeAddress, kPointerBytes)});
  Add(NdOp::COPY, NdVar::reg(a64reg::X0, kPointerBytes),
      {NdVar::cst(0, kPointerBytes)});
  Add(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, kPointerBytes)});
  Block.EndAddr = Low.Entry + Block.Ops.size() * kInstructionBytes;
  const std::map<va_t, SourceFunctionTypeHint> Hints{{Low.Entry, Entry},
                                                     {kCalleeAddress, Callee}};
  LowToMedConverter Converter;
  Converter.setSourceCallHintsEnabled(true);
  Converter.setSourceCalleeTypeHints(&Hints);
  auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  const std::map<va_t, std::string> Names{{kCalleeAddress, kResolvedCallback}};
  recoverCallAbi(Med, Arch::AArch64, Names);
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  const auto &Info = Med.CallInfos.front();
  ASSERT_TRUE(Info.SourceCallHint);
  EXPECT_TRUE(sourceCallRetainsNativeCarriers(*Info.SourceCallHint));
  EXPECT_TRUE(sourceCallRetainsResultStorage(*Info.SourceCallHint));
  ASSERT_EQ(Info.Args.size(), sourceABIParameters(Callee).size() + 1);
  EXPECT_EQ(Info.Args.front().Size, kPointerBytes);
  EXPECT_EQ(Info.Args[1].Size, kSourceAArch64Vector128Bytes);
  EXPECT_EQ(Info.Args.back().Size, kPointerBytes);
  EXPECT_EQ(Info.Args.front().Kind, MedVar::Reg);
  EXPECT_EQ(Info.Args.front().RegOff, a64reg::X0);
  EXPECT_EQ(Info.Args.back().Kind, MedVar::Reg);
  EXPECT_EQ(Info.Args.back().RegOff, a64reg::X1);
  for (const auto &B : Med.Blocks)
    for (const auto &Op : B.Ops) {
      EXPECT_NE(Op.Opcode, NdOp::STORE);
      if (Op.Opcode == NdOp::CALL)
        EXPECT_EQ(Op.Output.Size, 0U);
    }
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Med}, Context, kFunction, Arch::AArch64,
                                      {{kCalleeAddress, kResolvedCallback}},
                                      nullptr, BinaryFormat::MachO);
  ASSERT_TRUE(Module);
  const auto Inspect = [&](llvm::Module &Candidate) {
    checkMachineCall(Candidate, kAArch64PAuthKeyIA, true, true);
  };
  Inspect(*Module);
  EXPECT_TRUE(Codegen()
                  .compile(*Module, Arch::AArch64, BinaryFormat::MachO, kTriple)
                  .Success);
  for (bool LLVMRoute : {false, true}) {
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    Options.PreserveLLVMFunctionTypes = true;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (LLVMRoute)
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
    else {
      MedToHighConverter HighConverter;
      HighConverter.setFuncNames(&Names);
      ASSERT_TRUE(HighCEmitter().emit(
          {HighConverter.convert(Med, Arch::AArch64)}, Out, Options));
    }
    compile(Source, Inspect, kLoweredNativeDirectory, LLVMRoute, true,
            {kExceptionsOption, kNoStackProtectorOption},
            /*CompilerObject=*/true);
  }
}

TEST(AArch64AuthenticatedCall,
     LegacyApplePACGlobalsRefuseBeforeCodeGeneration) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseIR(llvm::MemoryBufferRef(kLegacyPAuthIR, kFunction),
                              Diagnostic, Context);
  ASSERT_TRUE(Module);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  const auto Object =
      Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO, kTriple);
  EXPECT_FALSE(Object.Success);
  EXPECT_TRUE(Object.ObjectData.empty());
  const auto Rewrite = Codegen().compileForRewrite(
      *Module, Arch::AArch64, {}, BinaryFormat::MachO, kTriple);
  EXPECT_FALSE(Rewrite.ImageValid);
}
