//===- NativeExceptionRewriteContractTests.cpp - EH rewrite contract -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

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

llvm::Function &addVoidFunction(llvm::Module &Module, llvm::StringRef Name,
                                uint64_t SourceVA) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, Name, Module);
  rewrite_source::setOriginalVA(*Function, SourceVA);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();
  return *Function;
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

void expectCxxGroupContractError(
    llvm::Expected<std::vector<exception_rewrite::CxxGroupRewriteContract>>
        Result,
    exception_rewrite::CxxGroupContractErrorReason ExpectedReason) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool Seen = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const exception_rewrite::CxxGroupRewriteContractError &Error) {
        Seen = true;
        EXPECT_EQ(Error.reason(), ExpectedReason);
      });
  EXPECT_TRUE(Seen);
}

void expectResolvedCxxGroupContractError(
    llvm::Expected<
        std::vector<exception_rewrite::ResolvedCxxGroupRewriteContract>>
        Result,
    exception_rewrite::CxxGroupContractErrorReason ExpectedReason) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool Seen = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const exception_rewrite::CxxGroupRewriteContractError &Error) {
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

TEST(CxxGroupRewriteContract, AcceptsCompleteAtomicGroup) {
  llvm::LLVMContext Context;
  llvm::Module Module("cxx-group", Context);
  llvm::Function &Primary = addVoidFunction(Module, "primary", 0x1000);
  llvm::Function &Handler = addVoidFunction(Module, "handler", 0x1100);

  exception_rewrite::CxxGroupRewriteContract Contract;
  Contract.GroupIdentity = 0x5000;
  Contract.CanonicalSourceOwnerVA = 0x1000;
  Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
  Contract.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
  Contract.Members = {{0x1000, &Primary}, {0x1100, &Handler}};
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Contract)));

  const exception_rewrite::CxxSourceGroup Source{
      0x5000, 0x1000, {0x1000, 0x1100}};
  auto Validated = exception_rewrite::validateCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Source));
  ASSERT_TRUE(static_cast<bool>(Validated))
      << llvm::toString(Validated.takeError());
  ASSERT_EQ(Validated->size(), 1u);
  EXPECT_EQ(Validated->front().GroupIdentity, 0x5000u);
  EXPECT_EQ(Validated->front().CanonicalSourceOwnerVA, 0x1000u);
  ASSERT_EQ(Validated->front().Members.size(), 2u);
  EXPECT_EQ(Validated->front().Members[0].IRFunction, &Primary);
  EXPECT_EQ(Validated->front().Members[1].IRFunction, &Handler);
}

TEST(CxxGroupRewriteContract, RejectsDuplicateGroupsAndMembers) {
  llvm::LLVMContext Context;
  llvm::Module Module("duplicate-cxx-group", Context);
  llvm::Function &First = addVoidFunction(Module, "first", 0x1000);
  llvm::Function &Second = addVoidFunction(Module, "second", 0x2000);

  exception_rewrite::CxxGroupRewriteContract FirstGroup;
  FirstGroup.GroupIdentity = 0x5000;
  FirstGroup.CanonicalSourceOwnerVA = 0x1000;
  FirstGroup.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
  FirstGroup.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
  FirstGroup.Members = {{0x1000, &First}};
  exception_rewrite::CxxGroupRewriteContract DuplicateGroup = FirstGroup;
  DuplicateGroup.CanonicalSourceOwnerVA = 0x2000;
  DuplicateGroup.Members = {{0x2000, &Second}};
  const std::vector DuplicateGroups{FirstGroup, DuplicateGroup};
  ASSERT_FALSE(
      exception_rewrite::setCxxGroupRewriteContracts(Module, DuplicateGroups));
  const std::vector<exception_rewrite::CxxSourceGroup> DistinctSources = {
      {0x5000, 0x1000, {0x1000}}, {0x6000, 0x2000, {0x2000}}};
  expectCxxGroupContractError(
      exception_rewrite::validateCxxGroupRewriteContracts(Module,
                                                          DistinctSources),
      exception_rewrite::CxxGroupContractErrorReason::DuplicateGroup);

  FirstGroup.Members.push_back({0x1000, &First});
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(FirstGroup)));
  const exception_rewrite::CxxSourceGroup Source{
      0x5000, 0x1000, {0x1000, 0x2000}};
  expectCxxGroupContractError(
      exception_rewrite::validateCxxGroupRewriteContracts(
          Module, llvm::ArrayRef(Source)),
      exception_rewrite::CxxGroupContractErrorReason::DuplicateMember);
}

TEST(CxxGroupRewriteContract, RejectsAMemberSharedByDifferentGroups) {
  llvm::LLVMContext Context;
  llvm::Module Module("cross-group-member", Context);
  llvm::Function &Member = addVoidFunction(Module, "member", 0x1000);

  auto MakeGroup = [&](uint64_t Identity) {
    exception_rewrite::CxxGroupRewriteContract Group;
    Group.GroupIdentity = Identity;
    Group.CanonicalSourceOwnerVA = 0x1000;
    Group.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
    Group.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
    Group.Members = {{0x1000, &Member}};
    return Group;
  };
  const std::vector Contracts{MakeGroup(0x5000), MakeGroup(0x6000)};
  ASSERT_FALSE(
      exception_rewrite::setCxxGroupRewriteContracts(Module, Contracts));
  const std::vector<exception_rewrite::CxxSourceGroup> Sources = {
      {0x5000, 0x1000, {0x1000}}, {0x6000, 0x1000, {0x1000}}};
  expectCxxGroupContractError(
      exception_rewrite::validateCxxGroupRewriteContracts(Module, Sources),
      exception_rewrite::CxxGroupContractErrorReason::DuplicateMember);
}

TEST(CxxGroupRewriteContract, RejectsNonCanonicalGroupAndMemberOrder) {
  llvm::LLVMContext Context;
  llvm::Module Module("unordered-cxx-group", Context);
  llvm::Function &First = addVoidFunction(Module, "first", 0x1000);
  llvm::Function &Second = addVoidFunction(Module, "second", 0x2000);

  auto MakeGroup = [](uint64_t Identity, uint64_t SourceVA,
                      llvm::Function &Function) {
    exception_rewrite::CxxGroupRewriteContract Group;
    Group.GroupIdentity = Identity;
    Group.CanonicalSourceOwnerVA = SourceVA;
    Group.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
    Group.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
    Group.Members = {{SourceVA, &Function}};
    return Group;
  };
  const std::vector Groups{MakeGroup(0x5000, 0x1000, First),
                           MakeGroup(0x4000, 0x2000, Second)};
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(Module, Groups));
  const std::vector<exception_rewrite::CxxSourceGroup> Sources = {
      {0x5000, 0x1000, {0x1000}}, {0x6000, 0x2000, {0x2000}}};
  expectCxxGroupContractError(
      exception_rewrite::validateCxxGroupRewriteContracts(Module, Sources),
      exception_rewrite::CxxGroupContractErrorReason::NonCanonicalOrder);

  auto MemberGroup = MakeGroup(0x5000, 0x1000, First);
  MemberGroup.Members = {{0x2000, &Second}, {0x1000, &First}};
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(MemberGroup)));
  const exception_rewrite::CxxSourceGroup Source{
      0x5000, 0x1000, {0x1000, 0x2000}};
  expectCxxGroupContractError(
      exception_rewrite::validateCxxGroupRewriteContracts(
          Module, llvm::ArrayRef(Source)),
      exception_rewrite::CxxGroupContractErrorReason::NonCanonicalOrder);
}

TEST(CxxGroupRewriteContract, RejectsMissingAndExtraSourceMembers) {
  llvm::LLVMContext Context;
  llvm::Module Module("cxx-group-closure", Context);
  llvm::Function &Primary = addVoidFunction(Module, "primary", 0x1000);
  llvm::Function &Handler = addVoidFunction(Module, "handler", 0x1100);

  exception_rewrite::CxxGroupRewriteContract Contract;
  Contract.GroupIdentity = 0x5000;
  Contract.CanonicalSourceOwnerVA = 0x1000;
  Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
  Contract.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
  Contract.Members = {{0x1000, &Primary}, {0x1100, &Handler}};
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Contract)));

  for (const exception_rewrite::CxxSourceGroup &Source :
       {exception_rewrite::CxxSourceGroup{0x5000, 0x1000, {0x1000}},
        exception_rewrite::CxxSourceGroup{
            0x5000, 0x1000, {0x1000, 0x1100, 0x1200}}}) {
    expectCxxGroupContractError(
        exception_rewrite::validateCxxGroupRewriteContracts(
            Module, llvm::ArrayRef(Source)),
        exception_rewrite::CxxGroupContractErrorReason::MembershipMismatch);
  }
}

TEST(CxxGroupRewriteContract, RejectsDeclarationAndCrossModuleFunction) {
  llvm::LLVMContext Context;
  llvm::Module Module("cxx-group-function", Context);
  auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Declaration = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "declaration", Module);
  rewrite_source::setOriginalVA(*Declaration, 0x1000);

  auto MakeContract = [](llvm::Function &Function) {
    exception_rewrite::CxxGroupRewriteContract Contract;
    Contract.GroupIdentity = 0x5000;
    Contract.CanonicalSourceOwnerVA = 0x1000;
    Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
    Contract.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
    Contract.Members = {{0x1000, &Function}};
    return Contract;
  };
  const exception_rewrite::CxxSourceGroup Source{0x5000, 0x1000, {0x1000}};

  auto Contract = MakeContract(*Declaration);
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Contract)));
  expectCxxGroupContractError(
      exception_rewrite::validateCxxGroupRewriteContracts(
          Module, llvm::ArrayRef(Source)),
      exception_rewrite::CxxGroupContractErrorReason::InvalidFunction);

  llvm::Module ForeignModule("foreign", Context);
  llvm::Function &Foreign =
      addVoidFunction(ForeignModule, "foreign_function", 0x1000);
  Contract = MakeContract(Foreign);
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Contract)));
  expectCxxGroupContractError(
      exception_rewrite::validateCxxGroupRewriteContracts(
          Module, llvm::ArrayRef(Source)),
      exception_rewrite::CxxGroupContractErrorReason::InvalidFunction);
}

TEST(CxxGroupRewriteContract, RejectsMismatchedSourceIdentity) {
  llvm::LLVMContext Context;
  llvm::Module Module("cxx-group-source-identity", Context);
  llvm::Function &Function = addVoidFunction(Module, "member", 0x1001);

  exception_rewrite::CxxGroupRewriteContract Contract;
  Contract.GroupIdentity = 0x5000;
  Contract.CanonicalSourceOwnerVA = 0x1000;
  Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
  Contract.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
  Contract.Members = {{0x1000, &Function}};
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Contract)));

  const exception_rewrite::CxxSourceGroup Source{0x5000, 0x1000, {0x1000}};
  expectCxxGroupContractError(
      exception_rewrite::validateCxxGroupRewriteContracts(
          Module, llvm::ArrayRef(Source)),
      exception_rewrite::CxxGroupContractErrorReason::SourceIdentityMismatch);
}

TEST(CxxGroupRewriteContract, RejectsPartialLoweringAndUnattestedInstall) {
  llvm::LLVMContext Context;
  llvm::Module Module("cxx-group-state", Context);
  llvm::Function &Function = addVoidFunction(Module, "member", 0x1000);
  const exception_rewrite::CxxSourceGroup Source{0x5000, 0x1000, {0x1000}};

  exception_rewrite::CxxGroupRewriteContract Contract;
  Contract.GroupIdentity = 0x5000;
  Contract.CanonicalSourceOwnerVA = 0x1000;
  Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Incomplete;
  Contract.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
  Contract.Members = {{0x1000, &Function}};
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Contract)));
  expectCxxGroupContractError(
      exception_rewrite::validateCxxGroupRewriteContracts(
          Module, llvm::ArrayRef(Source)),
      exception_rewrite::CxxGroupContractErrorReason::IncompleteLowering);

  Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
  Contract.Installation = exception_rewrite::CxxGroupInstallState::Unattested;
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Contract)));
  expectCxxGroupContractError(
      exception_rewrite::validateCxxGroupRewriteContracts(
          Module, llvm::ArrayRef(Source)),
      exception_rewrite::CxxGroupContractErrorReason::UnattestedInstallation);
}

TEST(CxxGroupRewriteContract,
     ResolvesEveryMemberToAuthenticatedGeneratedOwner) {
  llvm::LLVMContext Context;
  llvm::Module Module("cxx-group-generated-owners", Context);
  llvm::Function &Primary = addVoidFunction(Module, "primary", 0x1000);
  llvm::Function &Handler = addVoidFunction(Module, "handler", 0x1100);

  exception_rewrite::CxxGroupRewriteContract Contract;
  Contract.GroupIdentity = 0x5000;
  Contract.CanonicalSourceOwnerVA = 0x1000;
  Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
  Contract.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
  Contract.Members = {{0x1000, &Primary}, {0x1100, &Handler}};
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Contract)));
  const exception_rewrite::CxxSourceGroup Source{
      0x5000, 0x1000, {0x1000, 0x1100}};

  CompiledImage Compiled;
  Compiled.SourceFunctionOriginalVAs = {{"handler", 0x1100},
                                        {"primary", 0x1000}};
  using OwnerKind = llvm::mc_rewrite::RewriteSourceFunctionOwnerKind;
  Compiled.SourceFunctionOwners = {{"handler", "handler$owner", 0x8100, true,
                                    OwnerKind::WinCxxCatchFunclet, "primary"},
                                   {"primary",
                                    "primary$owner",
                                    0x8000,
                                    false,
                                    OwnerKind::FunctionEntry,
                                    {}}};
  Compiled.FunctionOwnerAddrs = {{"handler$owner", 0x8100},
                                 {"primary$owner", 0x8000}};
  CompiledSection Code;
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  Code.VA = 0x8000;
  Code.Size = 0x200;
  Code.IsAllocated = true;
  Compiled.Sections.push_back(std::move(Code));

  auto Resolved = exception_rewrite::validateAndResolveCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Source), Compiled);
  ASSERT_TRUE(static_cast<bool>(Resolved))
      << llvm::toString(Resolved.takeError());
  ASSERT_EQ(Resolved->size(), 1u);
  EXPECT_EQ(Resolved->front().GroupIdentity, 0x5000u);
  EXPECT_EQ(Resolved->front().CanonicalSourceOwnerVA, 0x1000u);
  ASSERT_EQ(Resolved->front().Members.size(), 2u);
  EXPECT_EQ(Resolved->front().Members[0].SourceMemberVA, 0x1000u);
  EXPECT_EQ(Resolved->front().Members[0].IRFunction, &Primary);
  EXPECT_EQ(Resolved->front().Members[0].GeneratedOwnerSymbol, "primary$owner");
  EXPECT_EQ(Resolved->front().Members[0].GeneratedOwnerVA, 0x8000u);
  EXPECT_EQ(Resolved->front().Members[1].SourceMemberVA, 0x1100u);
  EXPECT_EQ(Resolved->front().Members[1].IRFunction, &Handler);
  EXPECT_EQ(Resolved->front().Members[1].GeneratedOwnerSymbol, "handler$owner");
  EXPECT_EQ(Resolved->front().Members[1].GeneratedOwnerVA, 0x8100u);
}

TEST(CxxGroupRewriteContract,
     RejectsGeneratedOwnersWithWrongParentOrCatchRole) {
  using Owner = llvm::mc_rewrite::RewriteSourceFunctionOwner;
  using OwnerKind = llvm::mc_rewrite::RewriteSourceFunctionOwnerKind;

  llvm::LLVMContext Context;
  llvm::Module Module("cxx-group-generated-owner-roles", Context);
  llvm::Function &Primary = addVoidFunction(Module, "primary", 0x1000);
  llvm::Function &Handler = addVoidFunction(Module, "handler", 0x1100);

  exception_rewrite::CxxGroupRewriteContract Contract;
  Contract.GroupIdentity = 0x5000;
  Contract.CanonicalSourceOwnerVA = 0x1000;
  Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
  Contract.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
  Contract.Members = {{0x1000, &Primary}, {0x1100, &Handler}};
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      Module, llvm::ArrayRef(Contract)));
  const exception_rewrite::CxxSourceGroup Source{
      0x5000, 0x1000, {0x1000, 0x1100}};

  auto MakeCompiled = [&] {
    CompiledImage Compiled;
    Compiled.SourceFunctionOriginalVAs = {{"handler", 0x1100},
                                          {"primary", 0x1000}};
    Compiled.FunctionOwnerAddrs = {{"handler$owner", 0x8100},
                                   {"other$owner", 0x8200},
                                   {"primary$owner", 0x8000}};
    CompiledSection Code;
    Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
    Code.VA = 0x8000;
    Code.Size = 0x300;
    Code.IsAllocated = true;
    Compiled.Sections.push_back(std::move(Code));
    return Compiled;
  };
  auto ExpectRoleRejected = [&](std::vector<Owner> Owners) {
    CompiledImage Compiled = MakeCompiled();
    Compiled.SourceFunctionOwners = std::move(Owners);
    expectResolvedCxxGroupContractError(
        exception_rewrite::validateAndResolveCxxGroupRewriteContracts(
            Module, llvm::ArrayRef(Source), Compiled),
        exception_rewrite::CxxGroupContractErrorReason::
            GeneratedOwnerRoleMismatch);
  };

  ExpectRoleRejected({
      {"handler", "handler$owner", 0x8100, false, OwnerKind::FunctionEntry, {}},
      {"primary", "primary$owner", 0x8000, false, OwnerKind::FunctionEntry, {}},
  });
  ExpectRoleRejected({
      {"handler", "handler$owner", 0x8100, true, OwnerKind::WinCxxCatchFunclet,
       "other"},
      {"other", "other$owner", 0x8200, false, OwnerKind::FunctionEntry, {}},
      {"primary", "primary$owner", 0x8000, false, OwnerKind::FunctionEntry, {}},
  });
  ExpectRoleRejected({
      {"handler", "handler$owner", 0x8100, false, OwnerKind::FunctionEntry, {}},
      {"other", "other$owner", 0x8200, false, OwnerKind::FunctionEntry, {}},
      {"primary", "primary$owner", 0x8000, true, OwnerKind::WinCxxCatchFunclet,
       "other"},
  });
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

TEST(ExceptionRewriteContract, AcceptsCompleteAArch64NativeSEHContract) {
  llvm::LLVMContext Context;
  auto Module = makeVoidModule(Context, "aarch64_seh_native");
  Module->setTargetTriple(llvm::Triple("aarch64-pc-windows-msvc"));
  llvm::Function *Function = Module->getFunction("aarch64_seh_native");
  ASSERT_NE(Function, nullptr);

  auto *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module.get());
  Function->setPersonalityFn(Personality);
  llvm::Metadata *NativeVersion = llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::get(llvm::Type::getInt1Ty(Context), 1));
  Function->setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(
          Context,
          {NativeVersion, llvm::MDString::get(Context, "seh-aarch64-native")}));
  exception_rewrite::setContract(
      *Function, exception_rewrite::SourceState::Complete,
      exception_rewrite::LoweringState::Complete,
      /*RequiredProtectedCalls=*/0, /*LoweredProtectedCalls=*/0,
      /*SkippedLandingPads=*/0);

  const std::string Before = moduleIR(*Module);
  auto Requirements =
      exception_rewrite::validateExceptionRewriteContracts(*Module);
  ASSERT_TRUE(static_cast<bool>(Requirements))
      << llvm::toString(Requirements.takeError());
  EXPECT_TRUE(Requirements->RequiresRegisteredUnwind);
  ASSERT_EQ(Requirements->Functions.size(), 1u);
  EXPECT_EQ(Requirements->Functions.front().Name, "aarch64_seh_native");
  EXPECT_TRUE(Requirements->Functions.front().HasSourceContract);
  EXPECT_EQ(moduleIR(*Module), Before);
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

TEST(ExceptionRewriteContract, RejectsSymbolSpellingWithoutCompilerIdentity) {
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
  auto Missing = exception_rewrite::resolveRequiredFunctionAddresses(
      Requirements, Compiled);
  ASSERT_FALSE(static_cast<bool>(Missing));
  bool Seen = false;
  llvm::handleAllErrors(
      Missing.takeError(),
      [&](const exception_rewrite::ExceptionRewriteContractError &Error) {
        Seen = true;
        EXPECT_EQ(
            Error.reason(),
            exception_rewrite::ContractErrorReason::MissingCompiledFunction);
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
