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
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/AArch64/AArch64AuthenticatedCall.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"

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
  if (Kind == kFloatKind)
    return NdType::makeFloat(kFloatBytes);
  if (Kind == kDoubleKind)
    return NdType::makeFloat(kPointerBytes);
  if (Kind == kPointerKind)
    return NdType::makePtr(NdType::makeVoid());
  switch (Kind) {
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

void compile(const std::string &Source,
             const std::function<void(llvm::Module &)> &Inspect,
             llvm::StringRef Case, bool LLVMRoute, bool DarwinObject = false) {
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
    const llvm::StringRef Arguments[] = {Compiler,
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
    const auto Object = Codegen().compile(
        *Module, Arch::AArch64,
        DarwinObject ? BinaryFormat::MachO : BinaryFormat::ELF,
        DarwinObject ? kTriple : llvm::StringRef());
    ASSERT_TRUE(Object.Success);
    save(Case,
         llvm::formatv(kCompiledObject, Route, Optimization.drop_front()).str(),
         llvm::StringRef(
             reinterpret_cast<const char *>(Object.ObjectData.data()),
             Object.ObjectData.size()));
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

TEST(AArch64AuthenticatedCall, ExplicitTypesKeepAuthenticationOnTheCall) {
  for (const auto &Shape : TypedShapes)
    for (const auto &Variant : TypedVariants) {
      const auto Case = llvm::formatv(kTypedDirectory, Shape.Name, Variant.Key,
                                      Variant.Zero, Variant.Raw)
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
      ASSERT_TRUE(assignAnalystCallABI(*Hint, Arch::AArch64,
                                       BinaryFormat::MachO, Diagnostic))
          << Diagnostic;
      const auto Bindings = sourceABIParameters(Hint->Signature);
      auto Function =
          function(Variant.Key, false, Bindings.size(), Variant.Raw);
      auto &Block = Function.Blocks.front();
      MedOp Call = Block.Ops[Variant.Raw ? 0 : 1];
      Hint->CallKind = SourceCallTypeHint::Kind::Analyst;
      Call.Addr = kEntryAddress;
      Call.OriginSeq = 0;
      Call.Output.Size = ReturnType->Size;
      Hint->AnalystSite = SourceCallOccurrenceKey{Call.Addr, Call.OriginSeq,
                                                  NdOp::INDIR_CALL, 0};
      Hint->AnalystFixedCount = Shape.Fixed;
      Hint->Signature.ReturnType = ReturnType;
      std::vector<MedOp> Setup;
      std::vector<MedVar> Arguments;
      for (unsigned I = 0; I < Bindings.size(); ++I) {
        const auto Type = Bindings[I].Type;
        auto Argument = Function.Params[I + 2];
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
      if (ReturnType->Kind == NdTypeKind::Struct ||
          ReturnType->Size > kPointerBytes) {
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
      Emitter.setAnalystSourceProjection(true);
      llvm::LLVMContext Context;
      auto Module = Emitter.emit({Function}, Context, kFunction, Arch::AArch64,
                                 {}, nullptr, BinaryFormat::MachO);
      ASSERT_TRUE(Module);
      optimize(*Module);
      const auto Inspect = [&](llvm::Module &Candidate) {
        checkBundle(Candidate, Variant.Key, Variant.Zero,
                    Shape.Parameters.size(), Variant.Raw);
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
              EXPECT_EQ(Width(Call->getType()),
                        ReturnType->Size * kBitsPerByte);
              EXPECT_EQ(Call->getType()->isFloatingPointTy(),
                        ReturnType->Kind == NdTypeKind::Float);
              if (ReturnType->Kind == NdTypeKind::Int &&
                  ReturnType->Size < kPromotedIntegerBytes)
                EXPECT_TRUE(Call->hasRetAttr(ReturnType->IsSigned
                                                 ? llvm::Attribute::SExt
                                                 : llvm::Attribute::ZExt));
              for (unsigned I = 0; I < Shape.Parameters.size(); ++I) {
                auto *Argument = Call->getArgOperand(I)->getType();
                const auto Expected = typedParameter(Shape.Parameters[I]);
                EXPECT_EQ(Width(Argument), Expected->Size * kBitsPerByte);
                if (Expected->Kind == NdTypeKind::Int &&
                    Expected->Size < kPromotedIntegerBytes &&
                    Hint->Signature.Parameters[I].Location.Kind ==
                        SourceABICarrierKind::IntegerRegister)
                  EXPECT_TRUE(Call->paramHasAttr(
                      I, Expected->IsSigned ? llvm::Attribute::SExt
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
        EXPECT_NE(Source.find(Variant.Raw ? kRawCallMacro : kResignBuiltin),
                  std::string::npos)
            << Source;
        save(Case, LLVMRoute ? kLLVMSource : kHighSource, Source);
        compile(Source, Inspect, Case, LLVMRoute, true);
      }
      const auto Object = Codegen().compile(*Module, Arch::AArch64,
                                            BinaryFormat::MachO, kTriple);
      ASSERT_TRUE(Object.Success);
      save(Case, kELFObject,
           llvm::StringRef(
               reinterpret_cast<const char *>(Object.ObjectData.data()),
               Object.ObjectData.size()));
    }
}

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
    EXPECT_NE(Source.find(Variant.Raw ? kRawCallMacro : kResignBuiltin),
              std::string::npos)
        << Source;
    save(Case, kHighSource, Source);
    compile(
        Source,
        [&](llvm::Module &Recompiled) {
          checkBundle(Recompiled, Variant.Key, Variant.Zero, 1, Variant.Raw);
        },
        Case, false, true);
    if (!Variant.Raw) {
      Hint->Signature.Convention =
          SourceFunctionTypeHint::ConventionKind::Swift;
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
