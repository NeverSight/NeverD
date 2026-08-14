//===- ExceptionRewriteContract.cpp - Native EH contract validation ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/ExceptionRewriteContract.h"

#include "neverd/backend/codegen/BinaryRewriter.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
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

} // namespace

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
    Result.Functions.push_back({Function.getName().str(), true});
  }
  Result.RequiresRegisteredUnwind = !Result.Functions.empty();
  return Result;
}

llvm::Expected<std::vector<uint64_t>>
resolveRequiredFunctionAddresses(const Requirements &Requirements,
                                 const CompiledImage &Compiled) {
  std::vector<uint64_t> Addresses;
  Addresses.reserve(Requirements.Functions.size());
  for (const Requirements::Function &Function : Requirements.Functions) {
    std::set<std::string> Candidates{Function.Name, "_" + Function.Name};
    if (!Function.Name.empty() && Function.Name.front() == '_')
      Candidates.insert(Function.Name.substr(1));

    std::optional<uint64_t> Address;
    for (const std::string &Candidate : Candidates) {
      auto It = Compiled.SymbolAddrs.find(Candidate);
      if (It == Compiled.SymbolAddrs.end() || It->second == 0)
        continue;
      if (Address && *Address != It->second)
        return llvm::make_error<ExceptionRewriteContractError>(
            ContractErrorReason::AmbiguousCompiledFunction, Function.Name);
      Address = It->second;
    }
    if (!Address)
      return llvm::make_error<ExceptionRewriteContractError>(
          ContractErrorReason::MissingCompiledFunction, Function.Name);
    Addresses.push_back(*Address);
  }
  std::sort(Addresses.begin(), Addresses.end());
  Addresses.erase(std::unique(Addresses.begin(), Addresses.end()),
                  Addresses.end());
  return Addresses;
}

} // namespace neverd::exception_rewrite
