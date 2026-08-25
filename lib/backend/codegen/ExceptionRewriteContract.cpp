//===- ExceptionRewriteContract.cpp - Native EH contract validation ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/ExceptionRewriteContract.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/ADT/Twine.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace neverd::exception_rewrite {

char ExceptionRewriteContractError::ID;
char CxxGroupRewriteContractError::ID;

CxxGroupRewriteContractError::CxxGroupRewriteContractError(
    CxxGroupContractErrorReason Reason, uint64_t GroupIdentity,
    uint64_t SourceMemberVA, std::string Detail)
    : Reason(Reason), GroupIdentity(GroupIdentity),
      SourceMemberVA(SourceMemberVA), Detail(std::move(Detail)) {}

void CxxGroupRewriteContractError::log(llvm::raw_ostream &OS) const {
  OS << "C++ exception group rewrite contract";
  if (GroupIdentity != 0)
    OS << " 0x" << llvm::utohexstr(GroupIdentity);
  if (SourceMemberVA != 0)
    OS << " member 0x" << llvm::utohexstr(SourceMemberVA);
  OS << ": ";
  switch (Reason) {
  case CxxGroupContractErrorReason::InvalidMetadata:
    OS << "invalid numeric metadata";
    break;
  case CxxGroupContractErrorReason::UnsupportedSchema:
    OS << "unsupported metadata schema";
    break;
  case CxxGroupContractErrorReason::DuplicateGroup:
    OS << "duplicate group identity";
    break;
  case CxxGroupContractErrorReason::DuplicateMember:
    OS << "duplicate source member";
    break;
  case CxxGroupContractErrorReason::NonCanonicalOrder:
    OS << "identities are not in canonical order";
    break;
  case CxxGroupContractErrorReason::MembershipMismatch:
    OS << "source group membership does not match";
    break;
  case CxxGroupContractErrorReason::InvalidFunction:
    OS << "member does not name a definition in this module";
    break;
  case CxxGroupContractErrorReason::SourceIdentityMismatch:
    OS << "member source identity does not match";
    break;
  case CxxGroupContractErrorReason::IncompleteLowering:
    OS << "group lowering is incomplete";
    break;
  case CxxGroupContractErrorReason::UnattestedInstallation:
    OS << "all-or-none installation is not attested";
    break;
  case CxxGroupContractErrorReason::MissingGeneratedOwner:
    OS << "member has no generated owner";
    break;
  case CxxGroupContractErrorReason::AmbiguousGeneratedOwner:
    OS << "member generated owner is ambiguous";
    break;
  case CxxGroupContractErrorReason::GeneratedOwnerRoleMismatch:
    OS << "member generated owner has the wrong C++ EH role";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ")";
}

std::error_code CxxGroupRewriteContractError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

ExceptionRewriteContractError::ExceptionRewriteContractError(
    ContractErrorReason Reason, std::string FunctionName, std::string Detail)
    : Reason(Reason), FunctionName(std::move(FunctionName)),
      Detail(std::move(Detail)) {}

void ExceptionRewriteContractError::log(llvm::raw_ostream &OS) const {
  OS << "exception rewrite contract";
  if (!FunctionName.empty())
    OS << " for '" << FunctionName << "'";
  OS << ": ";
  switch (Reason) {
  case ContractErrorReason::InvalidMetadata:
    OS << "invalid numeric metadata";
    break;
  case ContractErrorReason::UnsupportedSchema:
    OS << "unsupported metadata schema";
    break;
  case ContractErrorReason::PartialSource:
    OS << "source exception metadata is partial";
    break;
  case ContractErrorReason::MalformedSource:
    OS << "source exception metadata is malformed";
    break;
  case ContractErrorReason::IncompleteLowering:
    OS << "native exception lowering is incomplete";
    break;
  case ContractErrorReason::CounterMismatch:
    OS << "native exception lowering counters disagree";
    break;
  case ContractErrorReason::MissingCompiledFunction:
    OS << "required function has no compiled address";
    break;
  case ContractErrorReason::AmbiguousCompiledFunction:
    OS << "required function aliases have different addresses";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ")";
}

std::error_code ExceptionRewriteContractError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

namespace {

llvm::Error cxxGroupError(CxxGroupContractErrorReason Reason,
                          uint64_t GroupIdentity = 0,
                          uint64_t SourceMemberVA = 0,
                          llvm::StringRef Detail = {}) {
  return llvm::make_error<CxxGroupRewriteContractError>(
      Reason, GroupIdentity, SourceMemberVA, Detail.str());
}

llvm::Error contractError(const llvm::Function &Function,
                          ContractErrorReason Reason,
                          llvm::StringRef Detail = {}) {
  return llvm::make_error<ExceptionRewriteContractError>(
      Reason, Function.getName().str(), Detail.str());
}

std::optional<uint64_t> uintOperand(const llvm::MDNode &Node, unsigned Index,
                                    unsigned Width) {
  if (Index >= Node.getNumOperands())
    return std::nullopt;
  const auto *Constant = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
      Node.getOperand(Index).get());
  const auto *Integer =
      Constant ? llvm::dyn_cast<llvm::ConstantInt>(Constant->getValue())
               : nullptr;
  if (!Integer || Integer->getBitWidth() != Width)
    return std::nullopt;
  return Integer->getZExtValue();
}

bool hasNativeIRExceptionContract(const llvm::Function &Function) {
  if (Function.hasUWTable() || Function.hasPersonalityFn())
    return true;
  for (const llvm::BasicBlock &Block : Function)
    for (const llvm::Instruction &Instruction : Block)
      if (llvm::isa<llvm::InvokeInst, llvm::LandingPadInst, llvm::ResumeInst>(
              Instruction))
        return true;
  return false;
}

std::optional<uint64_t>
countNativeProtectedCalls(const llvm::Function &Function) {
  if (!Function.getMetadata(windows_eh_md::NativeAttachment)) {
    uint64_t Count = 0;
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Instruction);
        if (!Invoke)
          continue;
        const llvm::Function *Callee = Invoke->getCalledFunction();
        if (!Callee || !Callee->isIntrinsic())
          ++Count;
      }
    return Count;
  }

  uint64_t Count = 0;
  std::set<const llvm::InvokeInst *> CountedInvokes;
  for (const llvm::BasicBlock &Block : Function)
    for (const llvm::Instruction &Instruction : Block) {
      const auto *Anchor = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      if (!Anchor || Anchor->countOperandBundlesOfType(
                         windows_eh_md::ProvenanceBundle) == 0)
        continue;
      auto Bundle = Anchor->getOperandBundle(windows_eh_md::ProvenanceBundle);
      if (!Bundle ||
          Bundle->Inputs.size() != windows_eh_md::ProvenanceOperandCount)
        return std::nullopt;
      const auto *Role = llvm::dyn_cast<llvm::ConstantInt>(
          Bundle->Inputs[windows_eh_md::ProvenanceRole].get());
      if (!Role || Role->getBitWidth() != 8)
        return std::nullopt;
      if (Role->getZExtValue() !=
          static_cast<unsigned>(
              windows_eh_md::NativeProvenanceRole::ProtectedInvoke))
        continue;

      const auto *Version = llvm::dyn_cast<llvm::ConstantInt>(
          Bundle->Inputs[windows_eh_md::ProvenanceVersion].get());
      const llvm::Function *AnchorCallee = Anchor->getCalledFunction();
      const llvm::Instruction *Next = Anchor->getNextNode();
      while (Next && Next->isDebugOrPseudoInst())
        Next = Next->getNextNode();
      const auto *Invoke = llvm::dyn_cast_or_null<llvm::InvokeInst>(Next);
      const llvm::Function *InvokeCallee =
          Invoke ? Invoke->getCalledFunction() : nullptr;
      if (Anchor->countOperandBundlesOfType(windows_eh_md::ProvenanceBundle) !=
              1 ||
          !Version || Version->getBitWidth() != 32 ||
          Version->getZExtValue() != windows_eh_md::ProvenanceSchemaVersion ||
          !AnchorCallee ||
          AnchorCallee->getIntrinsicID() != llvm::Intrinsic::sideeffect ||
          !Invoke || (InvokeCallee && InvokeCallee->isIntrinsic()) ||
          !CountedInvokes.insert(Invoke).second)
        return std::nullopt;
      ++Count;
    }
  return Count;
}

} // namespace

llvm::Error
setCxxGroupRewriteContracts(llvm::Module &Module,
                            llvm::ArrayRef<CxxGroupRewriteContract> Contracts) {
  llvm::LLVMContext &Context = Module.getContext();
  auto UInt = [&](uint64_t Value, unsigned Width) -> llvm::Metadata * {
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::IntegerType::get(Context, Width), Value));
  };

  std::vector<llvm::MDNode *> Groups;
  Groups.reserve(Contracts.size());
  for (const CxxGroupRewriteContract &Contract : Contracts) {
    std::vector<llvm::Metadata *> Members;
    Members.reserve(Contract.Members.size());
    for (const CxxGroupMemberBinding &Member : Contract.Members) {
      if (!Member.IRFunction || &Member.IRFunction->getContext() != &Context)
        return cxxGroupError(CxxGroupContractErrorReason::InvalidFunction,
                             Contract.GroupIdentity, Member.SourceMemberVA);
      Members.push_back(llvm::MDNode::get(
          Context, {UInt(Member.SourceMemberVA, 64),
                    llvm::ValueAsMetadata::get(Member.IRFunction)}));
    }
    llvm::MDNode *MemberList = llvm::MDNode::get(Context, Members);
    Groups.push_back(llvm::MDNode::get(
        Context,
        {UInt(CxxGroupSchemaVersion, 32), UInt(Contract.GroupIdentity, 64),
         UInt(Contract.CanonicalSourceOwnerVA, 64),
         UInt(static_cast<uint8_t>(Contract.Lowering), 8),
         UInt(static_cast<uint8_t>(Contract.Installation), 8),
         UInt(Contract.Members.size(), 64), MemberList}));
  }

  if (llvm::NamedMDNode *Existing =
          Module.getNamedMetadata(CxxGroupTableMetadata))
    Module.eraseNamedMetadata(Existing);
  if (Groups.empty())
    return llvm::Error::success();
  llvm::NamedMDNode *Table =
      Module.getOrInsertNamedMetadata(CxxGroupTableMetadata);
  for (llvm::MDNode *Group : Groups)
    Table->addOperand(Group);
  return llvm::Error::success();
}

llvm::Expected<std::vector<CxxGroupRewriteContract>>
validateCxxGroupRewriteContracts(const llvm::Module &Module,
                                 llvm::ArrayRef<CxxSourceGroup> SourceGroups) {
  uint64_t PreviousGroup = 0;
  bool HavePreviousGroup = false;
  std::set<uint64_t> SeenSourceMembers;
  for (const CxxSourceGroup &Source : SourceGroups) {
    if (Source.GroupIdentity == 0 || Source.CanonicalSourceOwnerVA == 0 ||
        Source.MemberVAs.empty())
      return cxxGroupError(CxxGroupContractErrorReason::InvalidMetadata,
                           Source.GroupIdentity);
    if (HavePreviousGroup && Source.GroupIdentity == PreviousGroup)
      return cxxGroupError(CxxGroupContractErrorReason::DuplicateGroup,
                           Source.GroupIdentity);
    if (HavePreviousGroup && Source.GroupIdentity < PreviousGroup)
      return cxxGroupError(CxxGroupContractErrorReason::NonCanonicalOrder,
                           Source.GroupIdentity);
    PreviousGroup = Source.GroupIdentity;
    HavePreviousGroup = true;

    uint64_t PreviousMember = 0;
    bool HavePreviousMember = false;
    bool HasCanonicalOwner = false;
    for (uint64_t MemberVA : Source.MemberVAs) {
      if (MemberVA == 0)
        return cxxGroupError(CxxGroupContractErrorReason::InvalidMetadata,
                             Source.GroupIdentity, MemberVA);
      if (HavePreviousMember && MemberVA == PreviousMember)
        return cxxGroupError(CxxGroupContractErrorReason::DuplicateMember,
                             Source.GroupIdentity, MemberVA);
      if (HavePreviousMember && MemberVA < PreviousMember)
        return cxxGroupError(CxxGroupContractErrorReason::NonCanonicalOrder,
                             Source.GroupIdentity, MemberVA);
      if (!SeenSourceMembers.insert(MemberVA).second)
        return cxxGroupError(CxxGroupContractErrorReason::DuplicateMember,
                             Source.GroupIdentity, MemberVA,
                             "source member belongs to more than one group");
      PreviousMember = MemberVA;
      HavePreviousMember = true;
      HasCanonicalOwner |= MemberVA == Source.CanonicalSourceOwnerVA;
    }
    if (!HasCanonicalOwner)
      return cxxGroupError(CxxGroupContractErrorReason::MembershipMismatch,
                           Source.GroupIdentity, Source.CanonicalSourceOwnerVA,
                           "canonical owner is not a source member");
  }

  const llvm::NamedMDNode *Table =
      Module.getNamedMetadata(CxxGroupTableMetadata);
  const size_t ContractCount = Table ? Table->getNumOperands() : 0;
  if (ContractCount != SourceGroups.size())
    return cxxGroupError(CxxGroupContractErrorReason::MembershipMismatch, 0, 0,
                         "group count differs");

  std::vector<CxxGroupRewriteContract> Result;
  Result.reserve(ContractCount);
  PreviousGroup = 0;
  HavePreviousGroup = false;
  std::set<uint64_t> SeenContractMembers;
  for (size_t GroupIndex = 0; GroupIndex < ContractCount; ++GroupIndex) {
    const llvm::MDNode *Group = Table->getOperand(GroupIndex);
    if (!Group || Group->getNumOperands() != CxxGroupOperandCount)
      return cxxGroupError(CxxGroupContractErrorReason::InvalidMetadata);
    const std::optional<uint64_t> VersionValue =
        uintOperand(*Group, CxxGroupVersion, 32);
    const std::optional<uint64_t> IdentityValue =
        uintOperand(*Group, CxxGroupIdentity, 64);
    const std::optional<uint64_t> CanonicalOwner =
        uintOperand(*Group, CxxGroupCanonicalSourceOwner, 64);
    const std::optional<uint64_t> LoweringValue =
        uintOperand(*Group, CxxGroupLowering, 8);
    const std::optional<uint64_t> InstallationValue =
        uintOperand(*Group, CxxGroupInstallation, 8);
    const std::optional<uint64_t> MemberCount =
        uintOperand(*Group, CxxGroupMemberCount, 64);
    const auto *Members = llvm::dyn_cast_or_null<llvm::MDNode>(
        Group->getOperand(CxxGroupMembers).get());
    if (!VersionValue || !IdentityValue || !CanonicalOwner || !LoweringValue ||
        !InstallationValue || !MemberCount || !Members)
      return cxxGroupError(CxxGroupContractErrorReason::InvalidMetadata,
                           IdentityValue.value_or(0));
    if (*VersionValue != CxxGroupSchemaVersion)
      return cxxGroupError(CxxGroupContractErrorReason::UnsupportedSchema,
                           *IdentityValue);
    if (*IdentityValue == 0 || *CanonicalOwner == 0 || *MemberCount == 0 ||
        *MemberCount != Members->getNumOperands() ||
        *LoweringValue >
            static_cast<uint8_t>(CxxGroupLoweringState::Complete) ||
        *InstallationValue >
            static_cast<uint8_t>(CxxGroupInstallState::AllOrNone))
      return cxxGroupError(CxxGroupContractErrorReason::InvalidMetadata,
                           *IdentityValue);
    if (HavePreviousGroup && *IdentityValue == PreviousGroup)
      return cxxGroupError(CxxGroupContractErrorReason::DuplicateGroup,
                           *IdentityValue);
    if (HavePreviousGroup && *IdentityValue < PreviousGroup)
      return cxxGroupError(CxxGroupContractErrorReason::NonCanonicalOrder,
                           *IdentityValue);
    PreviousGroup = *IdentityValue;
    HavePreviousGroup = true;

    CxxGroupRewriteContract Contract;
    Contract.GroupIdentity = *IdentityValue;
    Contract.CanonicalSourceOwnerVA = *CanonicalOwner;
    Contract.Lowering = static_cast<CxxGroupLoweringState>(*LoweringValue);
    Contract.Installation =
        static_cast<CxxGroupInstallState>(*InstallationValue);
    Contract.Members.reserve(*MemberCount);

    uint64_t PreviousMember = 0;
    bool HavePreviousMember = false;
    bool HasCanonicalOwner = false;
    for (const llvm::MDOperand &MemberOperand : Members->operands()) {
      const auto *Member =
          llvm::dyn_cast_or_null<llvm::MDNode>(MemberOperand.get());
      if (!Member || Member->getNumOperands() != CxxGroupMemberOperandCount)
        return cxxGroupError(CxxGroupContractErrorReason::InvalidMetadata,
                             *IdentityValue);
      const std::optional<uint64_t> MemberVA =
          uintOperand(*Member, CxxGroupMemberSourceVA, 64);
      const auto *FunctionValue = llvm::dyn_cast_or_null<llvm::ValueAsMetadata>(
          Member->getOperand(CxxGroupMemberIRFunction).get());
      auto *Function =
          FunctionValue
              ? llvm::dyn_cast<llvm::Function>(FunctionValue->getValue())
              : nullptr;
      if (!MemberVA || *MemberVA == 0)
        return cxxGroupError(CxxGroupContractErrorReason::InvalidMetadata,
                             *IdentityValue, MemberVA.value_or(0));
      if (HavePreviousMember && *MemberVA == PreviousMember)
        return cxxGroupError(CxxGroupContractErrorReason::DuplicateMember,
                             *IdentityValue, *MemberVA);
      if (HavePreviousMember && *MemberVA < PreviousMember)
        return cxxGroupError(CxxGroupContractErrorReason::NonCanonicalOrder,
                             *IdentityValue, *MemberVA);
      if (!SeenContractMembers.insert(*MemberVA).second)
        return cxxGroupError(
            CxxGroupContractErrorReason::DuplicateMember, *IdentityValue,
            *MemberVA, "contract member belongs to more than one group");
      PreviousMember = *MemberVA;
      HavePreviousMember = true;
      HasCanonicalOwner |= *MemberVA == *CanonicalOwner;
      if (!Function || Function->getParent() != &Module ||
          Function->isDeclaration())
        return cxxGroupError(CxxGroupContractErrorReason::InvalidFunction,
                             *IdentityValue, *MemberVA);
      auto SourceIdentity = rewrite_source::getOriginalVA(*Function);
      if (!SourceIdentity) {
        llvm::consumeError(SourceIdentity.takeError());
        return cxxGroupError(
            CxxGroupContractErrorReason::SourceIdentityMismatch, *IdentityValue,
            *MemberVA, "invalid source identity attachment");
      }
      if (!*SourceIdentity || **SourceIdentity != *MemberVA)
        return cxxGroupError(
            CxxGroupContractErrorReason::SourceIdentityMismatch, *IdentityValue,
            *MemberVA);
      Contract.Members.push_back({*MemberVA, Function});
    }
    if (!HasCanonicalOwner)
      return cxxGroupError(CxxGroupContractErrorReason::MembershipMismatch,
                           *IdentityValue, *CanonicalOwner,
                           "canonical owner is not a contract member");

    const CxxSourceGroup &Source = SourceGroups[GroupIndex];
    if (Source.GroupIdentity != Contract.GroupIdentity ||
        Source.CanonicalSourceOwnerVA != Contract.CanonicalSourceOwnerVA ||
        !std::equal(Source.MemberVAs.begin(), Source.MemberVAs.end(),
                    Contract.Members.begin(), Contract.Members.end(),
                    [](uint64_t SourceVA, const CxxGroupMemberBinding &Member) {
                      return SourceVA == Member.SourceMemberVA;
                    }))
      return cxxGroupError(CxxGroupContractErrorReason::MembershipMismatch,
                           Contract.GroupIdentity);
    if (Contract.Lowering != CxxGroupLoweringState::Complete)
      return cxxGroupError(CxxGroupContractErrorReason::IncompleteLowering,
                           Contract.GroupIdentity);
    if (Contract.Installation != CxxGroupInstallState::AllOrNone)
      return cxxGroupError(CxxGroupContractErrorReason::UnattestedInstallation,
                           Contract.GroupIdentity);
    Result.push_back(std::move(Contract));
  }
  return Result;
}

llvm::Expected<std::vector<ResolvedCxxGroupRewriteContract>>
resolveCxxGroupRewriteOwners(const llvm::Module &Module,
                             llvm::ArrayRef<CxxGroupRewriteContract> Contracts,
                             const CompiledImage &Compiled) {
  if (!Compiled.FunctionRangesValid)
    return cxxGroupError(CxxGroupContractErrorReason::MissingGeneratedOwner, 0,
                         0, "compiled function ranges are invalid");
  if (!llvm::mc_rewrite::validateRewriteSourceFunctionOwners(
          Compiled.SourceFunctionOwners))
    return cxxGroupError(CxxGroupContractErrorReason::AmbiguousGeneratedOwner,
                         0, 0,
                         "compiled source-owner provenance is not bijective");

  std::vector<ResolvedCxxGroupRewriteContract> Result;
  Result.reserve(Contracts.size());
  uint64_t PreviousGroup = 0;
  bool HavePreviousGroup = false;
  std::set<uint64_t> SeenMembers;
  for (const CxxGroupRewriteContract &Contract : Contracts) {
    if (Contract.GroupIdentity == 0 || Contract.CanonicalSourceOwnerVA == 0 ||
        Contract.Members.empty())
      return cxxGroupError(CxxGroupContractErrorReason::InvalidMetadata,
                           Contract.GroupIdentity);
    if (HavePreviousGroup && Contract.GroupIdentity == PreviousGroup)
      return cxxGroupError(CxxGroupContractErrorReason::DuplicateGroup,
                           Contract.GroupIdentity);
    if (HavePreviousGroup && Contract.GroupIdentity < PreviousGroup)
      return cxxGroupError(CxxGroupContractErrorReason::NonCanonicalOrder,
                           Contract.GroupIdentity);
    PreviousGroup = Contract.GroupIdentity;
    HavePreviousGroup = true;
    if (Contract.Lowering != CxxGroupLoweringState::Complete)
      return cxxGroupError(CxxGroupContractErrorReason::IncompleteLowering,
                           Contract.GroupIdentity);
    if (Contract.Installation != CxxGroupInstallState::AllOrNone)
      return cxxGroupError(CxxGroupContractErrorReason::UnattestedInstallation,
                           Contract.GroupIdentity);

    const auto CanonicalMember = llvm::find_if(
        Contract.Members, [&](const CxxGroupMemberBinding &Member) {
          return Member.SourceMemberVA == Contract.CanonicalSourceOwnerVA;
        });
    if (CanonicalMember == Contract.Members.end())
      return cxxGroupError(CxxGroupContractErrorReason::MembershipMismatch,
                           Contract.GroupIdentity,
                           Contract.CanonicalSourceOwnerVA,
                           "canonical owner is not a contract member");
    if (!CanonicalMember->IRFunction ||
        CanonicalMember->IRFunction->getParent() != &Module ||
        CanonicalMember->IRFunction->isDeclaration())
      return cxxGroupError(
          CxxGroupContractErrorReason::InvalidFunction, Contract.GroupIdentity,
          Contract.CanonicalSourceOwnerVA,
          "canonical owner is not a definition in this module");
    const std::string CanonicalFunctionName =
        CanonicalMember->IRFunction->getName().str();

    ResolvedCxxGroupRewriteContract Resolved;
    Resolved.GroupIdentity = Contract.GroupIdentity;
    Resolved.CanonicalSourceOwnerVA = Contract.CanonicalSourceOwnerVA;
    Resolved.Lowering = Contract.Lowering;
    Resolved.Installation = Contract.Installation;
    Resolved.Members.reserve(Contract.Members.size());

    uint64_t PreviousMember = 0;
    bool HavePreviousMember = false;
    bool HasCanonicalOwner = false;
    for (const CxxGroupMemberBinding &Member : Contract.Members) {
      if (!Member.IRFunction || Member.IRFunction->getParent() != &Module ||
          Member.IRFunction->isDeclaration() || Member.SourceMemberVA == 0)
        return cxxGroupError(CxxGroupContractErrorReason::InvalidFunction,
                             Contract.GroupIdentity, Member.SourceMemberVA);
      if (HavePreviousMember && Member.SourceMemberVA == PreviousMember)
        return cxxGroupError(CxxGroupContractErrorReason::DuplicateMember,
                             Contract.GroupIdentity, Member.SourceMemberVA);
      if (HavePreviousMember && Member.SourceMemberVA < PreviousMember)
        return cxxGroupError(CxxGroupContractErrorReason::NonCanonicalOrder,
                             Contract.GroupIdentity, Member.SourceMemberVA);
      if (!SeenMembers.insert(Member.SourceMemberVA).second)
        return cxxGroupError(CxxGroupContractErrorReason::DuplicateMember,
                             Contract.GroupIdentity, Member.SourceMemberVA,
                             "resolved member belongs to more than one group");
      PreviousMember = Member.SourceMemberVA;
      HavePreviousMember = true;
      HasCanonicalOwner |=
          Member.SourceMemberVA == Contract.CanonicalSourceOwnerVA;

      auto SourceIdentity = rewrite_source::getOriginalVA(*Member.IRFunction);
      if (!SourceIdentity) {
        llvm::consumeError(SourceIdentity.takeError());
        return cxxGroupError(
            CxxGroupContractErrorReason::SourceIdentityMismatch,
            Contract.GroupIdentity, Member.SourceMemberVA,
            "invalid source identity attachment");
      }
      if (!*SourceIdentity || **SourceIdentity != Member.SourceMemberVA)
        return cxxGroupError(
            CxxGroupContractErrorReason::SourceIdentityMismatch,
            Contract.GroupIdentity, Member.SourceMemberVA);

      const std::string FunctionName = Member.IRFunction->getName().str();
      const auto Original =
          Compiled.SourceFunctionOriginalVAs.find(FunctionName);
      if (Original == Compiled.SourceFunctionOriginalVAs.end() ||
          Original->second != Member.SourceMemberVA)
        return cxxGroupError(
            CxxGroupContractErrorReason::SourceIdentityMismatch,
            Contract.GroupIdentity, Member.SourceMemberVA,
            "compiled source identity does not match the member");

      const llvm::mc_rewrite::RewriteSourceFunctionOwner *Owner = nullptr;
      for (const llvm::mc_rewrite::RewriteSourceFunctionOwner &Candidate :
           Compiled.SourceFunctionOwners) {
        if (Candidate.SourceFunction != FunctionName)
          continue;
        if (Owner)
          return cxxGroupError(
              CxxGroupContractErrorReason::AmbiguousGeneratedOwner,
              Contract.GroupIdentity, Member.SourceMemberVA);
        Owner = &Candidate;
      }
      if (!Owner)
        return cxxGroupError(CxxGroupContractErrorReason::MissingGeneratedOwner,
                             Contract.GroupIdentity, Member.SourceMemberVA);

      using OwnerKind = llvm::mc_rewrite::RewriteSourceFunctionOwnerKind;
      const bool IsCanonical =
          Member.SourceMemberVA == Contract.CanonicalSourceOwnerVA;
      const bool HasExactRole =
          IsCanonical
              ? Owner->Kind == OwnerKind::FunctionEntry &&
                    Owner->ParentSourceFunction.empty()
              : Owner->Kind == OwnerKind::WinCxxCatchFunclet &&
                    Owner->ParentSourceFunction == CanonicalFunctionName;
      if (!HasExactRole)
        return cxxGroupError(
            CxxGroupContractErrorReason::GeneratedOwnerRoleMismatch,
            Contract.GroupIdentity, Member.SourceMemberVA,
            IsCanonical
                ? "canonical parent must be a FunctionEntry receipt"
                : "catch funclet must name the canonical parent receipt");

      const auto OwnerAddress =
          Compiled.FunctionOwnerAddrs.find(Owner->OwnerSymbol);
      if (OwnerAddress == Compiled.FunctionOwnerAddrs.end() ||
          OwnerAddress->second != Owner->OwnerVA)
        return cxxGroupError(
            CxxGroupContractErrorReason::MissingGeneratedOwner,
            Contract.GroupIdentity, Member.SourceMemberVA,
            "generated owner map disagrees with compiler provenance");

      const CompiledSection *OwningCode = nullptr;
      for (const CompiledSection &Section : Compiled.Sections) {
        if (!Section.IsAllocated ||
            Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code ||
            Owner->OwnerVA < Section.VA ||
            Owner->OwnerVA - Section.VA >= Section.Size)
          continue;
        if (OwningCode)
          return cxxGroupError(
              CxxGroupContractErrorReason::AmbiguousGeneratedOwner,
              Contract.GroupIdentity, Member.SourceMemberVA,
              "generated owner belongs to overlapping code sections");
        OwningCode = &Section;
      }
      if (!OwningCode)
        return cxxGroupError(CxxGroupContractErrorReason::MissingGeneratedOwner,
                             Contract.GroupIdentity, Member.SourceMemberVA,
                             "generated owner is outside allocated code");

      Resolved.Members.push_back({Member.SourceMemberVA, Member.IRFunction,
                                  Owner->OwnerSymbol, Owner->OwnerVA});
    }
    if (!HasCanonicalOwner)
      return cxxGroupError(CxxGroupContractErrorReason::MembershipMismatch,
                           Contract.GroupIdentity,
                           Contract.CanonicalSourceOwnerVA,
                           "canonical owner is not a contract member");
    Result.push_back(std::move(Resolved));
  }
  return Result;
}

llvm::Expected<std::vector<ResolvedCxxGroupRewriteContract>>
validateAndResolveCxxGroupRewriteContracts(
    const llvm::Module &Module, llvm::ArrayRef<CxxSourceGroup> SourceGroups,
    const CompiledImage &Compiled) {
  auto Contracts = validateCxxGroupRewriteContracts(Module, SourceGroups);
  if (!Contracts)
    return Contracts.takeError();
  return resolveCxxGroupRewriteOwners(Module, *Contracts, Compiled);
}

llvm::Expected<Requirements>
validateExceptionRewriteContracts(const llvm::Module &Module) {
  Requirements Result;
  const llvm::NamedMDNode *Schema =
      Module.getNamedMetadata(ModuleSchemaMetadata);
  const bool IsMarkedModule = Schema != nullptr;
  if (Schema) {
    if (Schema->getNumOperands() != 1 ||
        Schema->getOperand(0)->getNumOperands() != 1)
      return llvm::make_error<ExceptionRewriteContractError>(
          ContractErrorReason::InvalidMetadata, std::string{},
          "invalid module schema marker");
    const std::optional<uint64_t> ModuleVersion =
        uintOperand(*Schema->getOperand(0), 0, 32);
    if (!ModuleVersion)
      return llvm::make_error<ExceptionRewriteContractError>(
          ContractErrorReason::InvalidMetadata, std::string{},
          "invalid module schema width");
    if (*ModuleVersion != SchemaVersion)
      return llvm::make_error<ExceptionRewriteContractError>(
          ContractErrorReason::UnsupportedSchema, std::string{},
          "module schema marker");
  }

  for (const llvm::Function &Function : Module) {
    if (Function.isDeclaration())
      continue;

    const llvm::MDNode *Contract = Function.getMetadata(FunctionAttachment);
    if (!IsMarkedModule) {
      if (Contract)
        return contractError(Function, ContractErrorReason::InvalidMetadata,
                             "contract has no module schema marker");
      if (hasNativeIRExceptionContract(Function))
        Result.Functions.push_back({Function.getName().str(), false});
      continue;
    }
    if (!Contract)
      return contractError(Function, ContractErrorReason::InvalidMetadata,
                           "missing per-function contract");
    if (Contract->getNumOperands() != OperandCount)
      return contractError(Function, ContractErrorReason::InvalidMetadata,
                           "wrong operand count");

    const std::optional<uint64_t> VersionValue =
        uintOperand(*Contract, Version, 32);
    const std::optional<uint64_t> SourceValue =
        uintOperand(*Contract, Source, 8);
    const std::optional<uint64_t> LoweringValue =
        uintOperand(*Contract, Lowering, 8);
    const std::optional<uint64_t> Required =
        uintOperand(*Contract, RequiredProtectedCalls, 64);
    const std::optional<uint64_t> Lowered =
        uintOperand(*Contract, LoweredProtectedCalls, 64);
    const std::optional<uint64_t> Skipped =
        uintOperand(*Contract, SkippedLandingPads, 64);
    if (!VersionValue || !SourceValue || !LoweringValue || !Required ||
        !Lowered || !Skipped)
      return contractError(Function, ContractErrorReason::InvalidMetadata,
                           "non-integer operand");
    if (*VersionValue != SchemaVersion)
      return contractError(Function, ContractErrorReason::UnsupportedSchema);
    if (*SourceValue > static_cast<uint8_t>(SourceState::Malformed) ||
        *LoweringValue > static_cast<uint8_t>(LoweringState::Missing))
      return contractError(Function, ContractErrorReason::InvalidMetadata,
                           "state is out of range");

    const auto Source = static_cast<SourceState>(*SourceValue);
    const auto Lowering = static_cast<LoweringState>(*LoweringValue);
    if (Source == SourceState::Partial)
      return contractError(Function, ContractErrorReason::PartialSource);
    if (Source == SourceState::Malformed)
      return contractError(Function, ContractErrorReason::MalformedSource);

    if (Source == SourceState::Absent) {
      if (Lowering != LoweringState::NotRequired || *Required != 0 ||
          *Lowered != 0 || *Skipped != 0)
        return contractError(Function, ContractErrorReason::InvalidMetadata,
                             "absent source has a lowering result");
      if (hasNativeIRExceptionContract(Function))
        Result.Functions.push_back({Function.getName().str(), false});
      continue;
    }

    if (Lowering == LoweringState::Missing ||
        Lowering == LoweringState::Incomplete)
      return contractError(Function, ContractErrorReason::IncompleteLowering);
    if (Lowering == LoweringState::NotRequired) {
      if (*Required != 0 || *Lowered != 0 || *Skipped != 0)
        return contractError(Function, ContractErrorReason::CounterMismatch,
                             "unneeded lowering has nonzero counters");
      Result.Functions.push_back({Function.getName().str(), true});
      continue;
    }
    if (*Required != *Lowered || *Skipped != 0)
      return contractError(Function, ContractErrorReason::CounterMismatch);
    const std::optional<uint64_t> ActualProtectedCalls =
        countNativeProtectedCalls(Function);
    if (!ActualProtectedCalls)
      return contractError(Function, ContractErrorReason::InvalidMetadata,
                           "malformed protected-invoke provenance");
    if (*ActualProtectedCalls != *Lowered)
      return contractError(
          Function, ContractErrorReason::CounterMismatch,
          (llvm::Twine("metadata records ") + llvm::Twine(*Lowered) +
           " lowered protected calls but IR authenticates " +
           llvm::Twine(*ActualProtectedCalls) + " protected invokes")
              .str());
    Result.Functions.push_back({Function.getName().str(), true});
  }
  Result.RequiresRegisteredUnwind = !Result.Functions.empty();
  return Result;
}

llvm::Expected<std::vector<uint64_t>>
resolveRequiredFunctionAddresses(const Requirements &Requirements,
                                 const CompiledImage &Compiled) {
  auto Owners = resolveRequiredFunctionOwners(Requirements, Compiled);
  if (!Owners)
    return Owners.takeError();
  std::vector<uint64_t> Addresses;
  Addresses.reserve(Owners->size());
  for (const ResolvedFunctionOwner &Owner : *Owners)
    Addresses.push_back(Owner.OwnerVA);
  std::sort(Addresses.begin(), Addresses.end());
  Addresses.erase(std::unique(Addresses.begin(), Addresses.end()),
                  Addresses.end());
  return Addresses;
}

llvm::Expected<std::vector<ResolvedFunctionOwner>>
resolveRequiredFunctionOwners(const Requirements &Requirements,
                              const CompiledImage &Compiled) {
  if (!Compiled.FunctionRangesValid ||
      !llvm::mc_rewrite::validateRewriteSourceFunctionOwners(
          Compiled.SourceFunctionOwners))
    return llvm::make_error<ExceptionRewriteContractError>(
        ContractErrorReason::MissingCompiledFunction, std::string{},
        "compiled source-owner provenance is invalid");

  std::vector<ResolvedFunctionOwner> Owners;
  Owners.reserve(Requirements.Functions.size());
  for (const Requirements::Function &Function : Requirements.Functions) {
    const llvm::mc_rewrite::RewriteSourceFunctionOwner *Match = nullptr;
    for (const llvm::mc_rewrite::RewriteSourceFunctionOwner &Candidate :
         Compiled.SourceFunctionOwners) {
      if (Candidate.SourceFunction != Function.Name)
        continue;
      if (Match)
        return llvm::make_error<ExceptionRewriteContractError>(
            ContractErrorReason::AmbiguousCompiledFunction, Function.Name);
      Match = &Candidate;
    }
    if (!Match)
      return llvm::make_error<ExceptionRewriteContractError>(
          ContractErrorReason::MissingCompiledFunction, Function.Name);

    const auto OwnerAddress =
        Compiled.FunctionOwnerAddrs.find(Match->OwnerSymbol);
    if (OwnerAddress == Compiled.FunctionOwnerAddrs.end() ||
        OwnerAddress->second != Match->OwnerVA)
      return llvm::make_error<ExceptionRewriteContractError>(
          ContractErrorReason::MissingCompiledFunction, Function.Name,
          "owner map does not match compiler provenance");

    const CompiledSection *OwningCode = nullptr;
    for (const CompiledSection &Section : Compiled.Sections) {
      if (!Section.IsAllocated ||
          Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code ||
          Match->OwnerVA < Section.VA ||
          Match->OwnerVA - Section.VA >= Section.Size)
        continue;
      if (OwningCode)
        return llvm::make_error<ExceptionRewriteContractError>(
            ContractErrorReason::AmbiguousCompiledFunction, Function.Name,
            "owner belongs to overlapping code sections");
      OwningCode = &Section;
    }
    if (!OwningCode)
      return llvm::make_error<ExceptionRewriteContractError>(
          ContractErrorReason::MissingCompiledFunction, Function.Name,
          "owner is not inside allocated generated code");

    Owners.push_back(
        {Function.Name, Match->OwnerSymbol, Match->OwnerVA});
  }
  return Owners;
}

} // namespace neverd::exception_rewrite
