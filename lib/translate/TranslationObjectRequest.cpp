//===- TranslationObjectRequest.cpp - x86-64 to AArch64 object slice ----===//

#include "neverd/translate/TranslationObjectRequest.h"

#include "TranslationCacheIdentity.h"

#include "neverd/translate/RuntimeABI.h"
#include "neverd/translate/RuntimeGuestState.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace neverd::translate {

char TranslationObjectRequestError::ID;

TranslationObjectRequestError::TranslationObjectRequestError(
    TranslationObjectRequestErrorCode Code, std::string Detail,
    std::optional<TranslationTargetMachineErrorCode> TargetCode,
    std::optional<X86TranslationBlockBuilderErrorCode> BuilderCode,
    std::optional<TranslationBlockLoweringErrorCode> LoweringCode,
    std::optional<TranslationObjectCompilerErrorCode> CompilerCode,
    std::optional<uint64_t> CompilerBudgetObserved,
    std::optional<uint64_t> CompilerBudgetLimit,
    std::optional<uint64_t> GuestInstructionCount,
    std::optional<uint64_t> BuilderGuestPC,
    std::optional<RuntimeMemoryFaultKindV1> BuilderMemoryFault,
    std::optional<GuestMemoryFault> BuilderMemoryFaultDetails,
    std::optional<uint64_t> LoweringGuestPC)
    : Code(Code), Detail(std::move(Detail)), TargetCode(TargetCode),
      BuilderCode(BuilderCode), BuilderGuestPC(BuilderGuestPC),
      BuilderMemoryFault(BuilderMemoryFault),
      BuilderMemoryFaultDetails(std::move(BuilderMemoryFaultDetails)),
      LoweringCode(LoweringCode), LoweringGuestPC(LoweringGuestPC),
      CompilerCode(CompilerCode),
      CompilerBudgetObserved(CompilerBudgetObserved),
      CompilerBudgetLimit(CompilerBudgetLimit),
      GuestInstructionCount(GuestInstructionCount) {
  if (this->BuilderMemoryFaultDetails)
    this->BuilderMemoryFault = this->BuilderMemoryFaultDetails->Kind;
}

void TranslationObjectRequestError::log(llvm::raw_ostream &OS) const {
  OS << "translation object request: ";
  switch (Code) {
  case TranslationObjectRequestErrorCode::InvalidRequest:
    OS << "invalid request";
    break;
  case TranslationObjectRequestErrorCode::GuestStateRejected:
    OS << "guest state rejected";
    break;
  case TranslationObjectRequestErrorCode::RuntimeCreationFailed:
    OS << "guest memory runtime creation failed";
    break;
  case TranslationObjectRequestErrorCode::TargetMachineCreationFailed:
    OS << "target-machine creation failed";
    break;
  case TranslationObjectRequestErrorCode::BlockBuilderCreationFailed:
    OS << "block builder creation failed";
    break;
  case TranslationObjectRequestErrorCode::BlockConstructionFailed:
    OS << "block construction failed";
    break;
  case TranslationObjectRequestErrorCode::InstructionBudgetExceeded:
    OS << "instruction budget exceeded";
    break;
  case TranslationObjectRequestErrorCode::BlockLoweringFailed:
    OS << "block lowering failed";
    break;
  case TranslationObjectRequestErrorCode::ObjectCompilationFailed:
    OS << "object compilation failed";
    break;
  case TranslationObjectRequestErrorCode::GeneratedCodeBudgetExceeded:
    OS << "generated-code budget exceeded";
    break;
  case TranslationObjectRequestErrorCode::ArtifactVerificationFailed:
    OS << "artifact verification failed";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code TranslationObjectRequestError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}

namespace {

llvm::Error failure(TranslationObjectRequestErrorCode Code,
                    llvm::StringRef Detail = {}) {
  return llvm::make_error<TranslationObjectRequestError>(Code, Detail.str());
}

llvm::Error targetFailure(llvm::Error Cause) {
  std::optional<TranslationTargetMachineErrorCode> TargetCode;
  std::string Detail;
  llvm::Error Unhandled = llvm::handleErrors(
      std::move(Cause), [&](const TranslationTargetMachineError &Error) {
        TargetCode = Error.code();
        llvm::raw_string_ostream Stream(Detail);
        Error.log(Stream);
      });
  if (Unhandled)
    Detail = llvm::toString(std::move(Unhandled));
  return llvm::make_error<TranslationObjectRequestError>(
      TranslationObjectRequestErrorCode::TargetMachineCreationFailed,
      std::move(Detail), TargetCode);
}

llvm::Error builderFailure(llvm::Error Cause, bool DuringCreation) {
  std::optional<X86TranslationBlockBuilderErrorCode> BuilderCode;
  std::optional<uint64_t> BuilderGuestPC;
  std::optional<RuntimeMemoryFaultKindV1> BuilderMemoryFault;
  std::optional<GuestMemoryFault> BuilderMemoryFaultDetails;
  std::string Detail;
  llvm::Error Unhandled = llvm::handleErrors(
      std::move(Cause), [&](const X86TranslationBlockBuilderError &Error) {
        BuilderCode = Error.code();
        if (!DuringCreation) {
          BuilderGuestPC = Error.guestPC();
          if (Error.code() !=
              X86TranslationBlockBuilderErrorCode::InstructionBudgetExceeded) {
            BuilderMemoryFault = Error.fault();
            BuilderMemoryFaultDetails = Error.faultDetails();
          }
        }
        llvm::raw_string_ostream Stream(Detail);
        Error.log(Stream);
      });
  if (Unhandled)
    Detail = llvm::toString(std::move(Unhandled));
  TranslationObjectRequestErrorCode Code =
      DuringCreation
          ? TranslationObjectRequestErrorCode::BlockBuilderCreationFailed
          : TranslationObjectRequestErrorCode::BlockConstructionFailed;
  if (BuilderCode ==
      X86TranslationBlockBuilderErrorCode::InstructionBudgetExceeded)
    Code = TranslationObjectRequestErrorCode::InstructionBudgetExceeded;
  return llvm::make_error<TranslationObjectRequestError>(
      Code, std::move(Detail), std::nullopt, BuilderCode, std::nullopt,
      std::nullopt, std::nullopt, std::nullopt, std::nullopt, BuilderGuestPC,
      BuilderMemoryFault, std::move(BuilderMemoryFaultDetails));
}

llvm::Error loweringFailure(llvm::Error Cause) {
  std::optional<TranslationBlockLoweringErrorCode> LoweringCode;
  std::optional<uint64_t> LoweringGuestPC;
  std::string Detail;
  llvm::Error Unhandled = llvm::handleErrors(
      std::move(Cause), [&](const TranslationBlockLoweringError &Error) {
        LoweringCode = Error.code();
        LoweringGuestPC = Error.guestPC();
        llvm::raw_string_ostream Stream(Detail);
        Error.log(Stream);
      });
  if (Unhandled)
    Detail = llvm::toString(std::move(Unhandled));
  return llvm::make_error<TranslationObjectRequestError>(
      TranslationObjectRequestErrorCode::BlockLoweringFailed, std::move(Detail),
      std::nullopt, std::nullopt, LoweringCode, std::nullopt, std::nullopt,
      std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      LoweringGuestPC);
}

llvm::Error compilerFailure(llvm::Error Cause, uint64_t GuestInstructionCount) {
  std::optional<TranslationObjectCompilerErrorCode> CompilerCode;
  std::optional<uint64_t> BudgetObserved;
  std::optional<uint64_t> BudgetLimit;
  std::string Detail;
  llvm::Error Unhandled = llvm::handleErrors(
      std::move(Cause), [&](const TranslationObjectCompilerError &Error) {
        CompilerCode = Error.code();
        BudgetObserved = Error.budgetObserved();
        BudgetLimit = Error.budgetLimit();
        llvm::raw_string_ostream Stream(Detail);
        Error.log(Stream);
      });
  if (Unhandled)
    Detail = llvm::toString(std::move(Unhandled));

  TranslationObjectRequestErrorCode Code =
      TranslationObjectRequestErrorCode::ObjectCompilationFailed;
  if (CompilerCode ==
      TranslationObjectCompilerErrorCode::GeneratedCodeBudgetExceeded)
    Code = TranslationObjectRequestErrorCode::GeneratedCodeBudgetExceeded;
  else if (CompilerCode ==
           TranslationObjectCompilerErrorCode::ArtifactVerificationFailed)
    Code = TranslationObjectRequestErrorCode::ArtifactVerificationFailed;
  return llvm::make_error<TranslationObjectRequestError>(
      Code, std::move(Detail), std::nullopt, std::nullopt, std::nullopt,
      CompilerCode, BudgetObserved, BudgetLimit, GuestInstructionCount);
}

llvm::Error validateRequest(const TranslationObjectRequestV1 &Request) {
  const TranslationOptions &Options = Request.options();
  if (Options.Guest != GuestArchitecture::X86_64)
    return failure(TranslationObjectRequestErrorCode::InvalidRequest,
                   "v1 only accepts an x86-64 guest");
  switch (Options.Mode) {
  case TranslationMode::AOT:
    if (Options.Target.Kind != HostTargetKind::Explicit ||
        !Options.Target.Architecture ||
        *Options.Target.Architecture != GuestArchitecture::AArch64)
      return failure(TranslationObjectRequestErrorCode::InvalidRequest,
                     "AOT v1 requires an explicit AArch64 host target");
    break;
  case TranslationMode::JIT:
    if (Options.Target.Kind != HostTargetKind::Native)
      return failure(TranslationObjectRequestErrorCode::InvalidRequest,
                     "JIT v1 requires the native process target");
    break;
  default:
    return failure(TranslationObjectRequestErrorCode::InvalidRequest,
                   "v1 received an unknown translation mode");
  }
  if (Options.UnsupportedInstructions != UnsupportedInstructionPolicy::Fail)
    return failure(TranslationObjectRequestErrorCode::InvalidRequest,
                   "v1 requires fail-closed unsupported instructions");
  if (Options.Optimization !=
      TranslationOptimizationPolicy::ProvenSemanticAndLLVM)
    return failure(
        TranslationObjectRequestErrorCode::InvalidRequest,
        "v1 requires the composed semantic and LLVM optimization pipeline");
  if (Request.guestState().Architecture != GuestArchitecture::X86_64)
    return failure(TranslationObjectRequestErrorCode::InvalidRequest,
                   "guest state is not x86-64");
  if (llvm::Error Error = validateTranslationOptions(Options))
    return failure(TranslationObjectRequestErrorCode::InvalidRequest,
                   llvm::toString(std::move(Error)));
  return llvm::Error::success();
}

std::string
createWrapperCacheIdentity(const TranslationBlockDescriptorV1 &Descriptor,
                           const GuestState &State,
                           const TranslationOptions &Options,
                           const TranslationSemanticPolicyV1 &Semantic,
                           const TranslationTargetMachineV1 &Target,
                           const TranslationObjectArtifactV1 &Artifact) {
  detail::StableHashWriter Hash;
  Hash.addString("neverd.translation-object-wrapper.v1");
  Hash.addU32(kTranslationObjectWrapperCacheIdentityVersion);
  Hash.addU32(kTranslationObjectRequestSchemaV1);
  detail::hashTranslationBlockDescriptor(Hash, Descriptor);
  Hash.addByte(static_cast<uint8_t>(State.Architecture));
  Hash.addByte(static_cast<uint8_t>(State.ByteOrder));
  Hash.addByte(static_cast<uint8_t>(State.ExecutionMode));
  Hash.addU16(State.AddressWidth);
  llvm::SmallVector<llvm::StringRef, 8> Features;
  Features.reserve(State.Features.size());
  for (const std::string &Feature : State.Features)
    Features.push_back(Feature);
  llvm::sort(Features);
  Hash.addU64(Features.size());
  for (llvm::StringRef Feature : Features)
    Hash.addString(Feature);
  detail::hashTranslationOptions(Hash, Options, Target.hostTarget());
  detail::hashSemanticPolicy(Hash, Semantic);
  Hash.addString(Target.dataLayout().getStringRepresentation());
  Hash.addU32(kX86TranslationBlockLoweringSchemaV1);
  Hash.addU32(kRuntimeGuestStateX86_64MagicV1);
  Hash.addU16(kRuntimeGuestStateX86_64VersionV1);
  Hash.addU16(kRuntimeGuestStateX86_64SizeV1);
  detail::hashMemorySlots(Hash, runtimeGuestStateX86_64MemorySlotsV1());
  Hash.addU32(kRuntimeABIMagicV1);
  Hash.addU16(kRuntimeABIVersionV1);
  Hash.addU16(kRuntimeControlBlockSizeV1);
  Hash.addU32(kBlockExitMagicV1);
  Hash.addU16(kBlockExitVersionV1);
  Hash.addU16(kBlockExitSizeV1);
  Hash.addU32(TranslationObjectArtifactV1::CacheIdentityVersion);
  Hash.addU32(TranslationObjectArtifactV1::PipelineSchemaVersion);
  Hash.addString("reloc-static");
  Hash.addString("code-model-small");
  Hash.addString("exception-model-dwarf-cfi-no-uwtable");
  Hash.addString(Artifact.runtimeRegistryIdentity());
  Hash.addU64(Artifact.blockSymbols().size());
  for (const TranslationObjectSymbolV1 &Symbol : Artifact.blockSymbols()) {
    Hash.addString(Symbol.IRName);
    Hash.addString(Symbol.ObjectName);
  }
  Hash.addU64(Artifact.runtimeSymbols().size());
  for (const TranslationObjectSymbolV1 &Symbol : Artifact.runtimeSymbols()) {
    Hash.addString(Symbol.IRName);
    Hash.addString(Symbol.ObjectName);
  }
  Hash.addString(Artifact.requestCacheKey());
  return Hash.finish("neverd.translation-object-wrapper.v1.sha256:");
}

} // namespace

llvm::Expected<TranslationObjectResultV1>
compileTranslationObjectRequestV1(const TranslationObjectRequestV1 &Request) {
  if (llvm::Error Error = validateRequest(Request))
    return std::move(Error);
  if (llvm::Error Error = validateGuestState(Request.guestState()))
    return failure(TranslationObjectRequestErrorCode::GuestStateRejected,
                   llvm::toString(std::move(Error)));

  llvm::Expected<TranslationTargetMachineV1> TargetOrErr =
      createTranslationTargetMachineV1(Request.options());
  if (!TargetOrErr)
    return targetFailure(TargetOrErr.takeError());
  TranslationTargetMachineV1 Target = std::move(*TargetOrErr);
  if (Target.hostTarget().architecture() != GuestArchitecture::AArch64)
    return failure(TranslationObjectRequestErrorCode::InvalidRequest,
                   "v1 requires an AArch64 code-generation target");
  const llvm::Triple HostTriple(Target.hostTarget().triple());
  if (!HostTriple.isOSBinFormatELF() && !HostTriple.isOSBinFormatMachO())
    return failure(TranslationObjectRequestErrorCode::InvalidRequest,
                   "v1 emits only AArch64 ELF or Mach-O objects");

  GuestMemoryRuntimeConfig RuntimeConfig;
  RuntimeConfig.CodeInvalidation = Request.options().CodeInvalidation;
  RuntimeConfig.InstructionBudget = Request.options().InstructionBudget;
  RuntimeConfig.BlockBudget = Request.options().BlockBudget;
  llvm::Expected<std::unique_ptr<GuestMemoryRuntime>> RuntimeOrErr =
      GuestMemoryRuntime::create(Request.guestState(), RuntimeConfig);
  if (!RuntimeOrErr)
    return failure(TranslationObjectRequestErrorCode::RuntimeCreationFailed,
                   llvm::toString(RuntimeOrErr.takeError()));

  llvm::Expected<std::unique_ptr<X86TranslationBlockBuilder>> BuilderOrErr =
      X86TranslationBlockBuilder::create();
  if (!BuilderOrErr)
    return builderFailure(BuilderOrErr.takeError(), /*DuringCreation=*/true);
  llvm::Expected<TranslationBlockDescriptorV1> DescriptorOrErr =
      (*BuilderOrErr)
          ->build(**RuntimeOrErr, Request.entryPC(),
                  Request.options().InstructionBudget);
  if (!DescriptorOrErr)
    return builderFailure(DescriptorOrErr.takeError(),
                          /*DuringCreation=*/false);
  TranslationBlockDescriptorV1 Descriptor = std::move(*DescriptorOrErr);

  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Descriptor, Target.hostTarget(),
                                 Target.dataLayout(), Context);
  if (!LoweredOrErr)
    return loweringFailure(LoweredOrErr.takeError());

  const llvm::StringRef BlockSymbol = LoweredOrErr->blockSymbol();
  const llvm::StringRef RequiredBlocks[] = {BlockSymbol};
  TranslationObjectPolicyV1 Policy;
  Policy.StateSize = kRuntimeGuestStateX86_64SizeV1;
  Policy.StateSlots = runtimeGuestStateX86_64MemorySlotsV1();
  Policy.RequiredBlockSymbols = RequiredBlocks;
  Policy.Semantic = Request.semanticPolicy();
  llvm::Expected<TranslationObjectArtifactV1> ArtifactOrErr =
      compileTranslationObjectWithTargetV1(LoweredOrErr->module(),
                                           Request.options(), Policy, Target);
  if (!ArtifactOrErr)
    return compilerFailure(ArtifactOrErr.takeError(),
                           Descriptor.Header.GuestInstructionCount);

  TranslationObjectArtifactV1 Artifact = std::move(*ArtifactOrErr);
  const std::string CacheIdentity = createWrapperCacheIdentity(
      Descriptor, Request.guestState(), Request.options(),
      Request.semanticPolicy(), Target, Artifact);
  return TranslationObjectResultV1(std::move(Descriptor), std::move(Artifact),
                                   CacheIdentity);
}

} // namespace neverd::translate
