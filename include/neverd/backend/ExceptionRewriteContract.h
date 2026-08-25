//===- ExceptionRewriteContract.h - Native EH rewrite contract -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the format-neutral contract carried from exception-aware lifting
/// through optimization to the object-format unwind-table installers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_EXCEPTIONREWRITECONTRACT_H
#define NEVERD_BACKEND_EXCEPTIONREWRITECONTRACT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace neverd {
struct CompiledImage;
}

namespace neverd::exception_rewrite {

inline constexpr llvm::StringLiteral
    FunctionAttachment("neverd.exception.rewrite");
inline constexpr llvm::StringLiteral
    ModuleSchemaMetadata("neverd.exception.rewrite.schema");
inline constexpr uint32_t SchemaVersion = 1;

/// Independent module-level schema for a Microsoft C++ EH FuncInfo group.
/// All persistent state is encoded with explicitly sized integer operands;
/// function operands are identity-preserving LLVM value references.
inline constexpr llvm::StringLiteral
    CxxGroupTableMetadata("neverd.exception.rewrite.cxx-groups");
inline constexpr uint32_t CxxGroupSchemaVersion = 1;

enum class CxxGroupLoweringState : uint8_t {
  Incomplete = 0,
  Complete = 1,
};
static_assert(static_cast<uint8_t>(CxxGroupLoweringState::Incomplete) == 0 &&
              static_cast<uint8_t>(CxxGroupLoweringState::Complete) == 1);

enum class CxxGroupInstallState : uint8_t {
  Unattested = 0,
  AllOrNone = 1,
};
static_assert(static_cast<uint8_t>(CxxGroupInstallState::Unattested) == 0 &&
              static_cast<uint8_t>(CxxGroupInstallState::AllOrNone) == 1);

enum CxxGroupOperand : unsigned {
  CxxGroupVersion = 0,
  CxxGroupIdentity = 1,
  CxxGroupCanonicalSourceOwner = 2,
  CxxGroupLowering = 3,
  CxxGroupInstallation = 4,
  CxxGroupMemberCount = 5,
  CxxGroupMembers = 6,
  CxxGroupOperandCount = 7,
};
static_assert(CxxGroupVersion == 0 && CxxGroupIdentity == 1 &&
              CxxGroupCanonicalSourceOwner == 2 && CxxGroupLowering == 3 &&
              CxxGroupInstallation == 4 && CxxGroupMemberCount == 5 &&
              CxxGroupMembers == 6 && CxxGroupOperandCount == 7);

enum CxxGroupMemberOperand : unsigned {
  CxxGroupMemberSourceVA = 0,
  CxxGroupMemberIRFunction = 1,
  CxxGroupMemberOperandCount = 2,
};
static_assert(CxxGroupMemberSourceVA == 0 && CxxGroupMemberIRFunction == 1 &&
              CxxGroupMemberOperandCount == 2);

struct CxxGroupMemberBinding {
  uint64_t SourceMemberVA = 0;
  llvm::Function *IRFunction = nullptr;
};

struct CxxGroupRewriteContract {
  uint64_t GroupIdentity = 0;
  uint64_t CanonicalSourceOwnerVA = 0;
  CxxGroupLoweringState Lowering = CxxGroupLoweringState::Incomplete;
  CxxGroupInstallState Installation = CxxGroupInstallState::Unattested;
  std::vector<CxxGroupMemberBinding> Members;
};

/// Source-side group closure supplied by the object-format loader.  Groups and
/// member VAs must be strictly increasing so the validator can compare the
/// producer and source views without normalizing either side.
struct CxxSourceGroup {
  uint64_t GroupIdentity = 0;
  uint64_t CanonicalSourceOwnerVA = 0;
  std::vector<uint64_t> MemberVAs;
};

enum class CxxGroupContractErrorReason : uint8_t {
  InvalidMetadata = 0,
  UnsupportedSchema = 1,
  DuplicateGroup = 2,
  DuplicateMember = 3,
  NonCanonicalOrder = 4,
  MembershipMismatch = 5,
  InvalidFunction = 6,
  SourceIdentityMismatch = 7,
  IncompleteLowering = 8,
  UnattestedInstallation = 9,
  MissingGeneratedOwner = 10,
  AmbiguousGeneratedOwner = 11,
  GeneratedOwnerRoleMismatch = 12,
};
static_assert(
    static_cast<uint8_t>(CxxGroupContractErrorReason::InvalidMetadata) == 0 &&
    static_cast<uint8_t>(CxxGroupContractErrorReason::UnsupportedSchema) == 1 &&
    static_cast<uint8_t>(CxxGroupContractErrorReason::DuplicateGroup) == 2 &&
    static_cast<uint8_t>(CxxGroupContractErrorReason::DuplicateMember) == 3 &&
    static_cast<uint8_t>(CxxGroupContractErrorReason::NonCanonicalOrder) == 4 &&
    static_cast<uint8_t>(CxxGroupContractErrorReason::MembershipMismatch) ==
        5 &&
    static_cast<uint8_t>(CxxGroupContractErrorReason::InvalidFunction) == 6 &&
    static_cast<uint8_t>(CxxGroupContractErrorReason::SourceIdentityMismatch) ==
        7 &&
    static_cast<uint8_t>(CxxGroupContractErrorReason::IncompleteLowering) ==
        8 &&
    static_cast<uint8_t>(CxxGroupContractErrorReason::UnattestedInstallation) ==
        9 &&
    static_cast<uint8_t>(CxxGroupContractErrorReason::MissingGeneratedOwner) ==
        10 &&
    static_cast<uint8_t>(
        CxxGroupContractErrorReason::AmbiguousGeneratedOwner) == 11 &&
    static_cast<uint8_t>(
        CxxGroupContractErrorReason::GeneratedOwnerRoleMismatch) == 12);

/// Structured group validation failure.  Group and member identities remain
/// numeric so callers never need to parse a diagnostic or symbol spelling.
class CxxGroupRewriteContractError final
    : public llvm::ErrorInfo<CxxGroupRewriteContractError> {
public:
  static char ID;

  CxxGroupRewriteContractError(CxxGroupContractErrorReason Reason,
                               uint64_t GroupIdentity = 0,
                               uint64_t SourceMemberVA = 0,
                               std::string Detail = {});

  CxxGroupContractErrorReason reason() const { return Reason; }
  uint64_t groupIdentity() const { return GroupIdentity; }
  uint64_t sourceMemberVA() const { return SourceMemberVA; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  CxxGroupContractErrorReason Reason;
  uint64_t GroupIdentity;
  uint64_t SourceMemberVA;
  std::string Detail;
};

/// Replace the module's group table with the stable numeric representation of
/// \p Contracts.  Validation remains a separate, read-only operation.
llvm::Error
setCxxGroupRewriteContracts(llvm::Module &Module,
                            llvm::ArrayRef<CxxGroupRewriteContract> Contracts);

/// Validate group closure against the loader-authenticated source inventory.
llvm::Expected<std::vector<CxxGroupRewriteContract>>
validateCxxGroupRewriteContracts(const llvm::Module &Module,
                                 llvm::ArrayRef<CxxSourceGroup> SourceGroups);

struct ResolvedCxxGroupMemberOwner {
  uint64_t SourceMemberVA = 0;
  const llvm::Function *IRFunction = nullptr;
  std::string GeneratedOwnerSymbol;
  uint64_t GeneratedOwnerVA = 0;
};

struct ResolvedCxxGroupRewriteContract {
  uint64_t GroupIdentity = 0;
  uint64_t CanonicalSourceOwnerVA = 0;
  CxxGroupLoweringState Lowering = CxxGroupLoweringState::Incomplete;
  CxxGroupInstallState Installation = CxxGroupInstallState::Unattested;
  std::vector<ResolvedCxxGroupMemberOwner> Members;
};

/// Resolve every validated member through compiler-authenticated IR-function
/// provenance.  Generated symbols and addresses are reported exactly as
/// produced by the backend; source or target symbol spelling is never inferred.
/// \p Contracts must be the unmodified result of
/// validateCxxGroupRewriteContracts(); critical module and source-identity
/// invariants are nevertheless revalidated at this boundary.
llvm::Expected<std::vector<ResolvedCxxGroupRewriteContract>>
resolveCxxGroupRewriteOwners(const llvm::Module &Module,
                             llvm::ArrayRef<CxxGroupRewriteContract> Contracts,
                             const CompiledImage &Compiled);

/// One-step installer preflight which prevents callers from bypassing source
/// closure validation before resolving generated owners.
llvm::Expected<std::vector<ResolvedCxxGroupRewriteContract>>
validateAndResolveCxxGroupRewriteContracts(
    const llvm::Module &Module, llvm::ArrayRef<CxxSourceGroup> SourceGroups,
    const CompiledImage &Compiled);

enum class SourceState : uint8_t {
  Absent = 0,
  Complete = 1,
  Partial = 2,
  Malformed = 3,
};
static_assert(static_cast<uint8_t>(SourceState::Absent) == 0 &&
              static_cast<uint8_t>(SourceState::Complete) == 1 &&
              static_cast<uint8_t>(SourceState::Partial) == 2 &&
              static_cast<uint8_t>(SourceState::Malformed) == 3);

enum class LoweringState : uint8_t {
  NotRequired = 0,
  Complete = 1,
  Incomplete = 2,
  Missing = 3,
};
static_assert(static_cast<uint8_t>(LoweringState::NotRequired) == 0 &&
              static_cast<uint8_t>(LoweringState::Complete) == 1 &&
              static_cast<uint8_t>(LoweringState::Incomplete) == 2 &&
              static_cast<uint8_t>(LoweringState::Missing) == 3);

enum Operand : unsigned {
  Version = 0,
  Source = 1,
  Lowering = 2,
  RequiredProtectedCalls = 3,
  LoweredProtectedCalls = 4,
  SkippedLandingPads = 5,
  OperandCount = 6,
};
static_assert(Version == 0 && Source == 1 && Lowering == 2 &&
              RequiredProtectedCalls == 3 && LoweredProtectedCalls == 4 &&
              SkippedLandingPads == 5 && OperandCount == 6);

/// Write the stable numeric form consumed after optimization by format
/// installers.  No strings or diagnostics participate in the decision.
inline void markModule(llvm::Module &Module) {
  llvm::NamedMDNode *Schema =
      Module.getOrInsertNamedMetadata(ModuleSchemaMetadata);
  if (Schema->getNumOperands() != 0)
    return;
  llvm::LLVMContext &Context = Module.getContext();
  llvm::Metadata *Version = llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), SchemaVersion));
  Schema->addOperand(llvm::MDNode::get(Context, {Version}));
}

inline void setContract(llvm::Function &Function, SourceState Source,
                        LoweringState Lowering, uint64_t RequiredCalls = 0,
                        uint64_t LoweredCalls = 0, uint64_t SkippedPads = 0) {
  if (llvm::Module *Module = Function.getParent())
    markModule(*Module);
  llvm::LLVMContext &Context = Function.getContext();
  auto UInt = [&](uint64_t Value, unsigned Width) -> llvm::Metadata * {
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::IntegerType::get(Context, Width), Value));
  };
  Function.setMetadata(
      FunctionAttachment,
      llvm::MDNode::get(
          Context,
          {UInt(SchemaVersion, 32), UInt(static_cast<uint8_t>(Source), 8),
           UInt(static_cast<uint8_t>(Lowering), 8), UInt(RequiredCalls, 64),
           UInt(LoweredCalls, 64), UInt(SkippedPads, 64)}));
}

enum class ContractErrorReason : uint8_t {
  InvalidMetadata = 0,
  UnsupportedSchema = 1,
  PartialSource = 2,
  MalformedSource = 3,
  IncompleteLowering = 4,
  CounterMismatch = 5,
  MissingCompiledFunction = 6,
  AmbiguousCompiledFunction = 7,
};
static_assert(
    static_cast<uint8_t>(ContractErrorReason::InvalidMetadata) == 0 &&
    static_cast<uint8_t>(ContractErrorReason::UnsupportedSchema) == 1 &&
    static_cast<uint8_t>(ContractErrorReason::PartialSource) == 2 &&
    static_cast<uint8_t>(ContractErrorReason::MalformedSource) == 3 &&
    static_cast<uint8_t>(ContractErrorReason::IncompleteLowering) == 4 &&
    static_cast<uint8_t>(ContractErrorReason::CounterMismatch) == 5 &&
    static_cast<uint8_t>(ContractErrorReason::MissingCompiledFunction) == 6 &&
    static_cast<uint8_t>(ContractErrorReason::AmbiguousCompiledFunction) == 7);

/// A structured validation failure.  Callers can inspect the reason without
/// parsing the diagnostic rendered by log().
class ExceptionRewriteContractError final
    : public llvm::ErrorInfo<ExceptionRewriteContractError> {
public:
  static char ID;

  ExceptionRewriteContractError(ContractErrorReason Reason,
                                std::string FunctionName,
                                std::string Detail = {});

  ContractErrorReason reason() const { return Reason; }
  llvm::StringRef functionName() const { return FunctionName; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  ContractErrorReason Reason;
  std::string FunctionName;
  std::string Detail;
};

struct Requirements {
  bool RequiresRegisteredUnwind = false;
  struct Function {
    std::string Name;
    bool HasSourceContract = false;
  };
  std::vector<Function> Functions;
};

/// Validate every numeric contract in \p Module and report whether its native
/// unwind records must be registered by the target-format installer.
llvm::Expected<Requirements>
validateExceptionRewriteContracts(const llvm::Module &Module);

struct ResolvedFunctionOwner {
  std::string SourceFunction;
  std::string OwnerSymbol;
  uint64_t OwnerVA = 0;
};

/// Resolve required IR definitions through compiler-authenticated source-owner
/// provenance.  Source identities and target-selected MC symbols are compared
/// exactly; no object-format spelling is inferred.
llvm::Expected<std::vector<ResolvedFunctionOwner>>
resolveRequiredFunctionOwners(const Requirements &Requirements,
                              const CompiledImage &Compiled);

/// Return the unique addresses from resolveRequiredFunctionOwners().
llvm::Expected<std::vector<uint64_t>>
resolveRequiredFunctionAddresses(const Requirements &Requirements,
                                 const CompiledImage &Compiled);

} // namespace neverd::exception_rewrite

#endif // NEVERD_BACKEND_EXCEPTIONREWRITECONTRACT_H
