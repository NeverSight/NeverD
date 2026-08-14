//===- NativeExceptionRewriteContractTests.cpp - EH rewrite contract -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t kFunctionVA = 0x100001000;
constexpr va_t kMayThrowVA = 0x100001100;
constexpr va_t kMissingLandingPadVA = 0x100001020;

std::unique_ptr<llvm::Module>
makeUnloweredExceptionModule(llvm::LLVMContext &Context, BinaryFormat Format,
                             ExceptionParseStatus ParseStatus) {
  MedFunc Func;
  Func.Entry = kFunctionVA;
  Func.Name = "unlowered_exception_contract";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = kFunctionVA;
  Protected.EndAddr = kFunctionVA + 0x10;

  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = kFunctionVA + 4;
  Call.addInput(MedVar::makeConst(kMayThrowVA, 8));
  Protected.Ops.push_back(std::move(Call));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = kFunctionVA + 8;
  Protected.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Protected));

  ExceptionFunction EH;
  EH.CodeRange = {kFunctionVA, kFunctionVA + 0x40};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ParseStatus;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;

  ItaniumCallSite Site;
  Site.GuardedRange = {kFunctionVA, kFunctionVA + 0x10};
  Site.LandingPadVA = kMissingLandingPadVA;
  EH.Itanium->CallSites.push_back(std::move(Site));
  Func.ExceptionMetadata = std::move(EH);

  return MedLLVMEmitter().emit({Func}, Context, "unlowered-exception-contract",
                               Arch::AArch64, {{kMayThrowVA, "may_throw"}},
                               nullptr, Format);
}

void expectOrdinaryCFG(const llvm::Module &Module) {
  const llvm::Function *Function =
      Module.getFunction("unlowered_exception_contract");
  ASSERT_NE(Function, nullptr);
  EXPECT_FALSE(Function->hasPersonalityFn());

  size_t Calls = 0;
  size_t Invokes = 0;
  size_t LandingPads = 0;
  for (const llvm::BasicBlock &Block : *Function)
    for (const llvm::Instruction &Instruction : Block) {
      Calls += llvm::isa<llvm::CallInst>(Instruction);
      Invokes += llvm::isa<llvm::InvokeInst>(Instruction);
      LandingPads += llvm::isa<llvm::LandingPadInst>(Instruction);
    }
  EXPECT_EQ(Calls, 1u);
  EXPECT_EQ(Invokes, 0u);
  EXPECT_EQ(LandingPads, 0u);
}

void expectRejected(llvm::Error Error) {
  const bool Rejected = static_cast<bool>(Error);
  if (Error)
    llvm::consumeError(std::move(Error));
  EXPECT_TRUE(Rejected);
}

std::unique_ptr<llvm::Module> makeVoidModule(llvm::LLVMContext &Context,
                                             llvm::StringRef Name = "f") {
  auto Module = std::make_unique<llvm::Module>("contract", Context);
  auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, Name, Module.get());
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();
  return Module;
}

void expectContractError(
    llvm::Expected<exception_rewrite::Requirements> Result,
    exception_rewrite::ContractErrorReason ExpectedReason) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool Seen = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const exception_rewrite::ExceptionRewriteContractError &Error) {
        Seen = true;
        EXPECT_EQ(Error.reason(), ExpectedReason);
      });
  EXPECT_TRUE(Seen);
}

std::string moduleIR(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  Module.print(Stream, nullptr);
  Stream.flush();
  return Text;
}

TEST(ExceptionRewriteContract, MarkedModuleRequiresEveryDefinedFunction) {
  llvm::LLVMContext Context;
  auto Module = makeVoidModule(Context);
  exception_rewrite::markModule(*Module);
  expectContractError(
      exception_rewrite::validateExceptionRewriteContracts(*Module),
      exception_rewrite::ContractErrorReason::InvalidMetadata);
}

TEST(ExceptionRewriteContract, RejectsAnOperandWithTheWrongIntegerWidth) {
  llvm::LLVMContext Context;
  auto Module = makeVoidModule(Context);
  llvm::Function *Function = Module->getFunction("f");
  ASSERT_NE(Function, nullptr);
  exception_rewrite::markModule(*Module);
  auto UInt = [&](uint64_t Value, unsigned Width) -> llvm::Metadata * {
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::IntegerType::get(Context, Width), Value));
  };
  Function->setMetadata(
      exception_rewrite::FunctionAttachment,
      llvm::MDNode::get(
          Context,
          {UInt(exception_rewrite::SchemaVersion, 32),
           UInt(static_cast<uint8_t>(exception_rewrite::SourceState::Absent),
                32),
           UInt(static_cast<uint8_t>(
                    exception_rewrite::LoweringState::NotRequired),
                8),
           UInt(0, 64), UInt(0, 64), UInt(0, 64)}));
  expectContractError(
      exception_rewrite::validateExceptionRewriteContracts(*Module),
      exception_rewrite::ContractErrorReason::InvalidMetadata);
}

TEST(ExceptionRewriteContract,
     OptimizationPreflightRejectsBlockingStatesWithoutMutatingModule) {
  struct Case {
    exception_rewrite::SourceState Source;
    exception_rewrite::LoweringState Lowering;
    exception_rewrite::ContractErrorReason Reason;
  };
  const Case Cases[] = {
      {exception_rewrite::SourceState::Partial,
       exception_rewrite::LoweringState::Missing,
       exception_rewrite::ContractErrorReason::PartialSource},
      {exception_rewrite::SourceState::Malformed,
       exception_rewrite::LoweringState::Missing,
       exception_rewrite::ContractErrorReason::MalformedSource},
      {exception_rewrite::SourceState::Complete,
       exception_rewrite::LoweringState::Missing,
       exception_rewrite::ContractErrorReason::IncompleteLowering},
      {exception_rewrite::SourceState::Complete,
       exception_rewrite::LoweringState::Incomplete,
       exception_rewrite::ContractErrorReason::IncompleteLowering},
  };

  for (const Case &Current : Cases) {
    llvm::LLVMContext Context;
    auto Module = makeVoidModule(Context);
    llvm::Function *Function = Module->getFunction("f");
    ASSERT_NE(Function, nullptr);
    exception_rewrite::setContract(*Function, Current.Source, Current.Lowering,
                                   1, 0, 1);
    const std::string Before = moduleIR(*Module);

    // Optimization must use this validator as a preflight: once inlining or
    // DCE removes the function, its blocking per-function state is gone too.
    expectContractError(
        exception_rewrite::validateExceptionRewriteContracts(*Module),
        Current.Reason);
    EXPECT_EQ(moduleIR(*Module), Before);
  }
}

TEST(ExceptionRewriteContract, UnmarkedExternalUWTableRequiresRegistration) {
  llvm::LLVMContext Context;
  auto Module = makeVoidModule(Context);
  llvm::Function *Function = Module->getFunction("f");
  ASSERT_NE(Function, nullptr);
  Function->setUWTableKind(llvm::UWTableKind::Default);

  auto Requirements =
      exception_rewrite::validateExceptionRewriteContracts(*Module);
  ASSERT_TRUE(static_cast<bool>(Requirements))
      << llvm::toString(Requirements.takeError());
  EXPECT_TRUE(Requirements->RequiresRegisteredUnwind);
  ASSERT_EQ(Requirements->Functions.size(), 1u);
  EXPECT_EQ(Requirements->Functions[0].Name, "f");
  EXPECT_FALSE(Requirements->Functions[0].HasSourceContract);
}

TEST(ExceptionRewriteContract, PureSourceCFIRequiresRegistration) {
  llvm::LLVMContext Context;
  auto Module = makeVoidModule(Context);
  llvm::Function *Function = Module->getFunction("f");
  ASSERT_NE(Function, nullptr);
  Function->setUWTableKind(llvm::UWTableKind::Default);
  exception_rewrite::setContract(*Function,
                                 exception_rewrite::SourceState::Complete,
                                 exception_rewrite::LoweringState::NotRequired);

  auto Requirements =
      exception_rewrite::validateExceptionRewriteContracts(*Module);
  ASSERT_TRUE(static_cast<bool>(Requirements))
      << llvm::toString(Requirements.takeError());
  EXPECT_TRUE(Requirements->RequiresRegisteredUnwind);
  ASSERT_EQ(Requirements->Functions.size(), 1u);
  EXPECT_TRUE(Requirements->Functions[0].HasSourceContract);
}

TEST(ExceptionRewriteContract, ResolvesAliasesOnlyWhenTheirAddressesAgree) {
  exception_rewrite::Requirements Requirements;
  Requirements.RequiresRegisteredUnwind = true;
  Requirements.Functions.push_back({"_f", true});
  Requirements.Functions.push_back({"g", true});

  CompiledImage Compiled;
  Compiled.SymbolAddrs = {{"_f", 0x2000},
                          {"__f", 0x2000},
                          {"f", 0x2000},
                          {"g", 0x1000},
                          {"_g", 0x1000}};
  auto Addresses = exception_rewrite::resolveRequiredFunctionAddresses(
      Requirements, Compiled);
  ASSERT_TRUE(static_cast<bool>(Addresses))
      << llvm::toString(Addresses.takeError());
  EXPECT_EQ(*Addresses, (std::vector<uint64_t>{0x1000, 0x2000}));

  Compiled.SymbolAddrs["__f"] = 0x3000;
  auto Ambiguous = exception_rewrite::resolveRequiredFunctionAddresses(
      Requirements, Compiled);
  ASSERT_FALSE(static_cast<bool>(Ambiguous));
  bool Seen = false;
  llvm::handleAllErrors(
      Ambiguous.takeError(),
      [&](const exception_rewrite::ExceptionRewriteContractError &Error) {
        Seen = true;
        EXPECT_EQ(
            Error.reason(),
            exception_rewrite::ContractErrorReason::AmbiguousCompiledFunction);
      });
  EXPECT_TRUE(Seen);
}

TEST(ExceptionRewriteContract, RequiredUnresolvedOutputIsByteIdenticalOnError) {
  llvm::LLVMContext Context;
  auto Module = makeVoidModule(Context);
  llvm::Function *Function = Module->getFunction("f");
  ASSERT_NE(Function, nullptr);
  exception_rewrite::setContract(*Function,
                                 exception_rewrite::SourceState::Complete,
                                 exception_rewrite::LoweringState::NotRequired);

  CompiledImage Compiled;
  Compiled.Success = true;
  Compiled.Unresolved.push_back("personality");
  std::vector<uint8_t> ELF = {0xde, 0xad, 0xbe, 0xef};
  const std::vector<uint8_t> ELFBefore = ELF;
  expectRejected(installELFEHFrame(ELF, std::nullopt, Compiled, *Module));
  EXPECT_EQ(ELF, ELFBefore);

  std::vector<uint8_t> MachO = {0xca, 0xfe, 0xba, 0xbe};
  const std::vector<uint8_t> MachOBefore = MachO;
  expectRejected(installMachOEHFrame(MachO, std::nullopt, Compiled, *Module));
  EXPECT_EQ(MachO, MachOBefore);
}

TEST(ELFExceptionRewriteContract,
     RejectsCompleteSourceWhenNativeLoweringIsMissing) {
  llvm::LLVMContext Context;
  auto Module = makeUnloweredExceptionModule(Context, BinaryFormat::ELF,
                                             ExceptionParseStatus::Complete);
  ASSERT_NE(Module, nullptr);
  expectOrdinaryCFG(*Module);

  std::vector<uint8_t> Binary;
  CompiledImage Compiled;
  Compiled.Success = true;
  expectRejected(installELFEHFrame(Binary, std::nullopt, Compiled, *Module));
}

TEST(MachOExceptionRewriteContract,
     RejectsPartialSourceWhenNativeLoweringIsMissing) {
  llvm::LLVMContext Context;
  auto Module = makeUnloweredExceptionModule(Context, BinaryFormat::MachO,
                                             ExceptionParseStatus::Partial);
  ASSERT_NE(Module, nullptr);
  expectOrdinaryCFG(*Module);

  std::vector<uint8_t> Binary;
  CompiledImage Compiled;
  Compiled.Success = true;
  expectRejected(installMachOEHFrame(Binary, std::nullopt, Compiled, *Module));
}

} // namespace
