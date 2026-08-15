//===- TranslationObjectCompiler.cpp - Verified host object emission -----===//

#include "neverd/translate/TranslationObjectCompiler.h"

#include "TranslationCacheIdentity.h"

#include "neverd/translate/RuntimeABI.h"
#include "neverd/translate/RuntimeHelpers.h"
#include "neverd/translate/RuntimeSymbolRegistry.h"
#include "neverd/translate/TranslationArtifactVerifier.h"
#include "neverd/translate/TranslationTargetMachine.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/StructuralHash.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::translate {

char TranslationObjectCompilerError::ID = 0;

namespace detail {

class TranslationObjectCompilerAccess final {
public:
  static llvm::TargetMachine &
  targetMachine(TranslationTargetMachineV1 &Target) {
    return Target.targetMachine();
  }
};

} // namespace detail

TranslationObjectCompilerError::TranslationObjectCompilerError(
    TranslationObjectCompilerErrorCode Code, std::string Detail,
    std::optional<uint64_t> BudgetObserved, std::optional<uint64_t> BudgetLimit)
    : Code(Code), Detail(std::move(Detail)), BudgetObserved(BudgetObserved),
      BudgetLimit(BudgetLimit) {}

namespace {

llvm::StringLiteral errorCodeName(TranslationObjectCompilerErrorCode Code) {
  switch (Code) {
  case TranslationObjectCompilerErrorCode::InvalidRequest:
    return "invalid-request";
  case TranslationObjectCompilerErrorCode::InvalidArtifactPolicy:
    return "invalid-artifact-policy";
  case TranslationObjectCompilerErrorCode::InvalidSemanticPolicy:
    return "invalid-semantic-policy";
  case TranslationObjectCompilerErrorCode::HostTargetResolutionFailed:
    return "host-target-resolution-failed";
  case TranslationObjectCompilerErrorCode::UnsupportedHostArchitecture:
    return "unsupported-host-architecture";
  case TranslationObjectCompilerErrorCode::TargetLookupFailed:
    return "target-lookup-failed";
  case TranslationObjectCompilerErrorCode::TargetCPUOrFeatureRejected:
    return "target-cpu-or-feature-rejected";
  case TranslationObjectCompilerErrorCode::TargetMachineCreationFailed:
    return "target-machine-creation-failed";
  case TranslationObjectCompilerErrorCode::InputIRVerificationFailed:
    return "input-ir-verification-failed";
  case TranslationObjectCompilerErrorCode::FinalIRVerificationFailed:
    return "final-ir-verification-failed";
  case TranslationObjectCompilerErrorCode::ObjectEmissionUnsupported:
    return "object-emission-unsupported";
  case TranslationObjectCompilerErrorCode::ObjectEmissionFailed:
    return "object-emission-failed";
  case TranslationObjectCompilerErrorCode::GeneratedCodeBudgetExceeded:
    return "generated-code-budget-exceeded";
  case TranslationObjectCompilerErrorCode::ArtifactVerificationFailed:
    return "artifact-verification-failed";
  case TranslationObjectCompilerErrorCode::RuntimeRegistryUnavailable:
    return "runtime-registry-unavailable";
  }
  return "unknown";
}

llvm::Error failure(TranslationObjectCompilerErrorCode Code,
                    llvm::StringRef Detail = {}) {
  return llvm::make_error<TranslationObjectCompilerError>(Code, Detail.str());
}

llvm::Error budgetFailure(TranslationObjectCompilerErrorCode Code,
                          uint64_t Observed, uint64_t Limit,
                          llvm::StringRef Detail) {
  return llvm::make_error<TranslationObjectCompilerError>(Code, Detail.str(),
                                                          Observed, Limit);
}

llvm::Error failure(TranslationObjectCompilerErrorCode Code,
                    llvm::Error Cause) {
  return failure(Code, llvm::toString(std::move(Cause)));
}

TranslationObjectCompilerErrorCode
compilerTargetErrorCode(TranslationTargetMachineErrorCode Code) {
  switch (Code) {
  case TranslationTargetMachineErrorCode::HostTargetResolutionFailed:
    return TranslationObjectCompilerErrorCode::HostTargetResolutionFailed;
  case TranslationTargetMachineErrorCode::UnsupportedHostArchitecture:
    return TranslationObjectCompilerErrorCode::UnsupportedHostArchitecture;
  case TranslationTargetMachineErrorCode::TargetLookupFailed:
    return TranslationObjectCompilerErrorCode::TargetLookupFailed;
  case TranslationTargetMachineErrorCode::TargetCPUOrFeatureRejected:
    return TranslationObjectCompilerErrorCode::TargetCPUOrFeatureRejected;
  case TranslationTargetMachineErrorCode::TargetMachineCreationFailed:
    return TranslationObjectCompilerErrorCode::TargetMachineCreationFailed;
  }
  return TranslationObjectCompilerErrorCode::TargetMachineCreationFailed;
}

llvm::Expected<TranslationTargetMachineV1>
createCompilerTarget(const TranslationOptions &Options) {
  llvm::Expected<TranslationTargetMachineV1> Target =
      createTranslationTargetMachineV1(Options);
  if (Target)
    return std::move(*Target);

  TranslationObjectCompilerErrorCode Code =
      TranslationObjectCompilerErrorCode::TargetMachineCreationFailed;
  std::string Detail;
  llvm::Error Unhandled = llvm::handleErrors(
      Target.takeError(), [&](const TranslationTargetMachineError &Error) {
        Code = compilerTargetErrorCode(Error.code());
        Detail = Error.detail().str();
      });
  if (Unhandled)
    Detail = llvm::toString(std::move(Unhandled));
  return failure(Code, Detail);
}

uint64_t asStableSize(size_t Value) {
  if constexpr (sizeof(size_t) > sizeof(uint64_t))
    if (Value > std::numeric_limits<uint64_t>::max())
      return std::numeric_limits<uint64_t>::max();
  return static_cast<uint64_t>(Value);
}

void addSaturating(uint64_t &Total, uint64_t Delta) {
  constexpr uint64_t Max = std::numeric_limits<uint64_t>::max();
  Total = Delta > Max - Total ? Max : Total + Delta;
}

void addProofStats(solver::ProofStats &Total, const solver::ProofStats &Delta) {
  addSaturating(Total.Queries, Delta.Queries);
  addSaturating(Total.Conflicts, Delta.Conflicts);
  addSaturating(Total.Propagations, Delta.Propagations);
  addSaturating(Total.WatchVisits, Delta.WatchVisits);
}

unsigned proofSeverity(solver::ProofStatus Proof) {
  switch (Proof) {
  case solver::ProofStatus::NotRun:
    return 0;
  case solver::ProofStatus::Equivalent:
    return 1;
  case solver::ProofStatus::Different:
    return 2;
  case solver::ProofStatus::Unknown:
    return 3;
  case solver::ProofStatus::Invalid:
    return 4;
  }
  return 4;
}

unsigned stopSeverity(TranslationSemanticStopV1 Stop) {
  return static_cast<unsigned>(Stop);
}

std::string snapshotFunction(const llvm::Function &Function) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  Function.print(Stream);
  return Text;
}

struct FunctionSemanticReport {
  bool Changed = false;
  uint64_t Rewrites = 0;
  uint64_t SearchWork = 0;
  solver::ProofStats ProofWork;
  unsigned Rounds = 0;
  TranslationSemanticStopV1 Stop = TranslationSemanticStopV1::Stable;
  solver::ProofStatus Proof = solver::ProofStatus::NotRun;
};

void mergeProofDisposition(FunctionSemanticReport &Total,
                           const SymSimplifyResult &Round) {
  addSaturating(Total.Rewrites, Round.Rewrites);
  addSaturating(Total.SearchWork, Round.SearchWork);
  addProofStats(Total.ProofWork, Round.ProofWork);
  if (proofSeverity(Round.Proof) > proofSeverity(Total.Proof))
    Total.Proof = Round.Proof;
}

void mergeFunctionReport(TranslationSemanticReportV1 &Module,
                         const FunctionSemanticReport &Function) {
  Module.Changed |= Function.Changed;
  addSaturating(Module.Rewrites, Function.Rewrites);
  addSaturating(Module.SearchWork, Function.SearchWork);
  addProofStats(Module.ProofWork, Function.ProofWork);
  addSaturating(Module.FunctionPassInvocations, 1);
  Module.MaxRounds = std::max(Module.MaxRounds, Function.Rounds);
  if (stopSeverity(Function.Stop) > stopSeverity(Module.Stop))
    Module.Stop = Function.Stop;
  if (proofSeverity(Function.Proof) > proofSeverity(Module.Proof))
    Module.Proof = Function.Proof;
}

/// Alternates target-aware LLVM canonicalization with the proof-gated semantic
/// pass until their exact joint state is stable or repeats.  With
/// RunCanonicalization false, this is the semantic-only policy and does not
/// introduce an LLVM optimization pipeline.
class TranslationSemanticConvergencePass final
    : public llvm::PassInfoMixin<TranslationSemanticConvergencePass> {
public:
  TranslationSemanticConvergencePass(
      TranslationSemanticPolicyV1 Policy, bool RunCanonicalization,
      std::shared_ptr<TranslationSemanticReportV1> Report)
      : Policy(std::move(Policy)), RunCanonicalization(RunCanonicalization),
        Report(std::move(Report)) {}

  llvm::PreservedAnalyses run(llvm::Function &Function,
                              llvm::FunctionAnalysisManager &FAM) {
    llvm::FunctionPassManager Canonicalize;
    if (RunCanonicalization)
      Canonicalize.addPass(llvm::InstCombinePass());

    FunctionSemanticReport Result;
    bool HaveCheckpoint = false;
    uint64_t CheckpointHash = 0;
    std::string CheckpointSnapshot;
    uint64_t CheckpointSpan = 2;
    uint64_t Distance = 0;

    for (;;) {
      bool Canonicalized = false;
      if (RunCanonicalization)
        Canonicalized = !Canonicalize.run(Function, FAM).areAllPreserved();

      SymSimplifyResult Semantic =
          SymSimplifyPass::simplifyWithResult(Function, Policy.Simplify);
      if (Semantic.Rewrites != 0)
        FAM.invalidate(Function, llvm::PreservedAnalyses::none());

      const bool Changed = Canonicalized || Semantic.Rewrites != 0;
      Result.Changed |= Changed;
      mergeProofDisposition(Result, Semantic);
      if (Result.Rounds != std::numeric_limits<unsigned>::max())
        ++Result.Rounds;

      if (!Changed) {
        Result.Stop = TranslationSemanticStopV1::Stable;
        break;
      }
      if (Policy.MaxRounds != 0 && Result.Rounds >= Policy.MaxRounds) {
        Result.Stop = TranslationSemanticStopV1::RoundBudgetExhausted;
        break;
      }

      const uint64_t CurrentHash =
          llvm::StructuralHash(Function, /*DetailedHash=*/true);
      if (!HaveCheckpoint) {
        CheckpointHash = CurrentHash;
        CheckpointSnapshot = snapshotFunction(Function);
        HaveCheckpoint = true;
        continue;
      }

      if (Distance != std::numeric_limits<uint64_t>::max())
        ++Distance;
      std::optional<std::string> CurrentSnapshot;
      if (CurrentHash == CheckpointHash) {
        CurrentSnapshot = snapshotFunction(Function);
        if (*CurrentSnapshot == CheckpointSnapshot) {
          Result.Stop = TranslationSemanticStopV1::CycleDetected;
          break;
        }
      }

      // Brent-style exponentially spaced exact checkpoints detect any finite
      // cycle while retaining one function snapshot.
      if (Distance >= CheckpointSpan) {
        if (!CurrentSnapshot)
          CurrentSnapshot = snapshotFunction(Function);
        CheckpointHash = CurrentHash;
        CheckpointSnapshot = std::move(*CurrentSnapshot);
        Distance = 0;
        if (CheckpointSpan <= std::numeric_limits<uint64_t>::max() / 2)
          CheckpointSpan *= 2;
        else
          CheckpointSpan = std::numeric_limits<uint64_t>::max();
      }
    }

    mergeFunctionReport(*Report, Result);
    return Result.Changed ? llvm::PreservedAnalyses::none()
                          : llvm::PreservedAnalyses::all();
  }

private:
  TranslationSemanticPolicyV1 Policy;
  bool RunCanonicalization;
  std::shared_ptr<TranslationSemanticReportV1> Report;
};

llvm::OptimizationLevel llvmOptimizationLevel(LLVMOptimizationLevel Level) {
  switch (Level) {
  case LLVMOptimizationLevel::O0:
    return llvm::OptimizationLevel::O0;
  case LLVMOptimizationLevel::O1:
    return llvm::OptimizationLevel::O1;
  case LLVMOptimizationLevel::O2:
    return llvm::OptimizationLevel::O2;
  case LLVMOptimizationLevel::O3:
    return llvm::OptimizationLevel::O3;
  }
  return llvm::OptimizationLevel::O0;
}

struct CanonicalPolicy {
  uint64_t StateSize = 0;
  std::vector<TranslationIRMemorySlot> StateSlots;
  std::vector<std::string> RequiredBlockSymbols;
  TranslationSemanticPolicyV1 Semantic;
};

auto memorySlotKey(const TranslationIRMemorySlot &Slot) {
  return std::tuple{static_cast<uint8_t>(Slot.Region), Slot.Offset, Slot.Size,
                    static_cast<uint8_t>(Slot.Access), Slot.Alignment};
}

llvm::Expected<CanonicalPolicy>
canonicalizePolicy(const llvm::Module &Module,
                   const TranslationObjectPolicyV1 &Policy,
                   const RuntimeSymbolRegistryV1 &RuntimeRegistry) {
  if (Policy.StateSize == 0)
    return failure(TranslationObjectCompilerErrorCode::InvalidRequest,
                   "state extent must be nonzero");
  if (Policy.RequiredBlockSymbols.empty())
    return failure(TranslationObjectCompilerErrorCode::InvalidArtifactPolicy,
                   "the block manifest is empty");
  if (Policy.Semantic.Simplify.Provider != ProofProvider::BuiltInSolver ||
      Policy.Semantic.Simplify.ProofCallback)
    return failure(TranslationObjectCompilerErrorCode::InvalidSemanticPolicy,
                   "object compilation requires the built-in proof provider");
  const solver::SatOptions &Sat = Policy.Semantic.Simplify.Solver.Sat;
  if (!std::isfinite(Sat.VarDecay) || Sat.VarDecay <= 0.0 ||
      Sat.VarDecay > 1.0 || !std::isfinite(Sat.ClauseDecay) ||
      Sat.ClauseDecay <= 0.0 || Sat.ClauseDecay > 1.0 ||
      !std::isfinite(Sat.LearnedFraction) || Sat.LearnedFraction < 0.0 ||
      !std::isfinite(Sat.LearnedGrowth) || Sat.LearnedGrowth < 1.0)
    return failure(TranslationObjectCompilerErrorCode::InvalidSemanticPolicy,
                   "SAT tuning values are outside their finite domains");

  CanonicalPolicy Result;
  Result.StateSize = Policy.StateSize;
  Result.StateSlots.assign(Policy.StateSlots.begin(), Policy.StateSlots.end());
  llvm::sort(Result.StateSlots, [](const auto &Left, const auto &Right) {
    return memorySlotKey(Left) < memorySlotKey(Right);
  });
  Result.Semantic = Policy.Semantic;

  llvm::StringSet<> Required;
  for (llvm::StringRef Name : Policy.RequiredBlockSymbols) {
    if (Name.empty() || Name.contains('\0') || !Required.insert(Name).second)
      return failure(
          TranslationObjectCompilerErrorCode::InvalidArtifactPolicy,
          "the block manifest contains an empty, embedded-NUL, or duplicate "
          "name");
    Result.RequiredBlockSymbols.push_back(Name.str());
  }
  llvm::sort(Result.RequiredBlockSymbols);

  llvm::StringSet<> RuntimeNames;
  for (const RuntimeSymbolEntryV1 &Entry : RuntimeRegistry.entries())
    RuntimeNames.insert(Entry.name());
  for (const std::string &Name : Result.RequiredBlockSymbols)
    if (RuntimeNames.contains(Name))
      return failure(
          TranslationObjectCompilerErrorCode::InvalidArtifactPolicy,
          "a block symbol overlaps the sealed runtime-helper registry");

  std::vector<std::string> Definitions;
  for (const llvm::Function &Function : Module)
    if (!Function.isDeclaration())
      Definitions.push_back(Function.getName().str());
  llvm::sort(Definitions);
  if (Definitions != Result.RequiredBlockSymbols)
    return failure(TranslationObjectCompilerErrorCode::InvalidArtifactPolicy,
                   "the block manifest is not the exact set of IR definitions");
  return Result;
}

std::vector<uint8_t> serializeBitcode(const llvm::Module &Module) {
  llvm::SmallVector<char, 4096> Storage;
  llvm::raw_svector_ostream Stream(Storage);
  llvm::WriteBitcodeToFile(Module, Stream,
                           /*ShouldPreserveUseListOrder=*/false);
  return std::vector<uint8_t>(Storage.begin(), Storage.end());
}

void hashSymbolNames(detail::StableHashWriter &Hash,
                     llvm::ArrayRef<std::string> Names) {
  Hash.addU64(Names.size());
  for (const std::string &Name : Names)
    Hash.addString(Name);
}

std::vector<std::string>
runtimeIRSymbolNames(const RuntimeSymbolRegistryV1 &Registry) {
  std::vector<std::string> Names;
  Names.reserve(Registry.entries().size());
  for (const RuntimeSymbolEntryV1 &Entry : Registry.entries())
    Names.push_back(Entry.name().str());
  return Names;
}

std::string createRequestCacheKey(llvm::ArrayRef<uint8_t> InputBitcode,
                                  const TranslationOptions &Options,
                                  const CanonicalPolicy &Policy,
                                  const ResolvedHostTarget &Target,
                                  const llvm::DataLayout &DataLayout,
                                  const RuntimeSymbolRegistryV1 &Registry) {
  detail::StableHashWriter Hash;
  Hash.addString("neverd.translation-object-request.v1");
  Hash.addU32(TranslationObjectArtifactV1::PipelineSchemaVersion);
  Hash.addU32(LLVM_VERSION_MAJOR);
  Hash.addU32(LLVM_VERSION_MINOR);
  Hash.addU32(LLVM_VERSION_PATCH);
  Hash.addString(LLVM_VERSION_STRING);
  Hash.addU32(TranslationObjectArtifactV1::CacheIdentityVersion);
  Hash.addU32(kRuntimeABIMagicV1);
  Hash.addU32(kRuntimeABIVersionV1);
  Hash.addU32(kRuntimeControlBlockSizeV1);
  Hash.addString(Registry.identity());
  Hash.addString("artifact-policy-v1");
  Hash.addString("reloc-static");
  Hash.addString("code-model-small");
  Hash.addString("exception-model-dwarf-cfi-no-uwtable");
  Hash.addString(DataLayout.getStringRepresentation());
  detail::hashTranslationOptions(Hash, Options, Target);
  Hash.addU64(Policy.StateSize);
  detail::hashMemorySlots(Hash, Policy.StateSlots);
  hashSymbolNames(Hash, Policy.RequiredBlockSymbols);
  const std::vector<std::string> RuntimeNames = runtimeIRSymbolNames(Registry);
  hashSymbolNames(Hash, RuntimeNames);
  detail::hashSemanticPolicy(Hash, Policy.Semantic);
  Hash.addBytes(InputBitcode);
  return Hash.finish("neverd.translation-object-request.v1.sha256:");
}

std::string mangleName(llvm::StringRef IRName,
                       const llvm::DataLayout &DataLayout) {
  llvm::SmallString<128> Storage;
  llvm::Mangler::getNameWithPrefix(Storage, IRName, DataLayout);
  return Storage.str().str();
}

std::string mangleGlobal(const llvm::GlobalValue &Global) {
  llvm::SmallString<128> Storage;
  llvm::Mangler Mangler;
  Mangler.getNameWithPrefix(Storage, &Global,
                            /*CannotUsePrivateLabel=*/false);
  return Storage.str().str();
}

llvm::Expected<std::vector<TranslationObjectSymbolV1>>
createBlockSymbolMappings(llvm::Module &Module,
                          llvm::ArrayRef<std::string> RequiredNames) {
  std::vector<TranslationObjectSymbolV1> Result;
  Result.reserve(RequiredNames.size());
  for (const std::string &IRName : RequiredNames) {
    llvm::Function *Function = Module.getFunction(IRName);
    if (!Function || Function->isDeclaration())
      return failure(TranslationObjectCompilerErrorCode::InvalidArtifactPolicy,
                     "the final IR no longer defines required block '" +
                         IRName + "'");
    Result.push_back({IRName, mangleGlobal(*Function)});
  }
  return Result;
}

std::vector<TranslationObjectSymbolV1>
createRuntimeSymbolMappings(const llvm::Module &Module,
                            const RuntimeSymbolRegistryV1 &Registry) {
  std::vector<TranslationObjectSymbolV1> Result;
  const std::vector<std::string> IRNames = runtimeIRSymbolNames(Registry);
  Result.reserve(IRNames.size());
  for (const std::string &IRName : IRNames) {
    if (const llvm::Function *Function = Module.getFunction(IRName))
      Result.push_back({IRName, mangleGlobal(*Function)});
    else
      Result.push_back({IRName, mangleName(IRName, Module.getDataLayout())});
  }
  return Result;
}

llvm::Error validateObjectSymbolMappings(
    llvm::ArrayRef<TranslationObjectSymbolV1> Blocks,
    llvm::ArrayRef<TranslationObjectSymbolV1> Runtime) {
  llvm::StringSet<> Names;
  for (const TranslationObjectSymbolV1 &Symbol : Blocks)
    if (Symbol.ObjectName.empty() || !Names.insert(Symbol.ObjectName).second)
      return failure(TranslationObjectCompilerErrorCode::InvalidArtifactPolicy,
                     "block symbols collide after target mangling");
  for (const TranslationObjectSymbolV1 &Symbol : Runtime)
    if (Symbol.ObjectName.empty() || !Names.insert(Symbol.ObjectName).second)
      return failure(TranslationObjectCompilerErrorCode::InvalidArtifactPolicy,
                     "runtime and block symbols collide after target mangling");
  return llvm::Error::success();
}

void mergeSemanticReport(TranslationSemanticReportV1 &Destination,
                         const TranslationSemanticReportV1 &Source) {
  Destination.Changed |= Source.Changed;
  addSaturating(Destination.Rewrites, Source.Rewrites);
  addSaturating(Destination.SearchWork, Source.SearchWork);
  addProofStats(Destination.ProofWork, Source.ProofWork);
  addSaturating(Destination.FunctionPassInvocations,
                Source.FunctionPassInvocations);
  Destination.MaxRounds = std::max(Destination.MaxRounds, Source.MaxRounds);
  if (stopSeverity(Source.Stop) > stopSeverity(Destination.Stop))
    Destination.Stop = Source.Stop;
  if (proofSeverity(Source.Proof) > proofSeverity(Destination.Proof))
    Destination.Proof = Source.Proof;
}

void runSemanticOnly(llvm::Module &Module,
                     const TranslationSemanticPolicyV1 &Policy,
                     TranslationSemanticReportV1 &Report) {
  auto Sink = std::make_shared<TranslationSemanticReportV1>();
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder Builder;
  Builder.registerModuleAnalyses(MAM);
  Builder.registerCGSCCAnalyses(CGAM);
  Builder.registerFunctionAnalyses(FAM);
  Builder.registerLoopAnalyses(LAM);
  Builder.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::FunctionPassManager Functions;
  Functions.addPass(TranslationSemanticConvergencePass(
      Policy, /*RunCanonicalization=*/false, Sink));
  llvm::ModulePassManager Pipeline;
  Pipeline.addPass(
      llvm::createModuleToFunctionPassAdaptor(std::move(Functions)));
  Pipeline.run(Module, MAM);
  mergeSemanticReport(Report, *Sink);
}

void runCombinedOptimization(llvm::Module &Module, llvm::TargetMachine &Machine,
                             const TranslationOptions &Options,
                             const TranslationSemanticPolicyV1 &Policy,
                             TranslationSemanticReportV1 &Report) {
  auto Sink = std::make_shared<TranslationSemanticReportV1>();
  llvm::PipelineTuningOptions Tuning;
  Tuning.CallGraphProfile = false;
  Tuning.UnifiedLTO = false;
  Tuning.MergeFunctions = false;
  Tuning.DevirtualizeSpeculatively = false;

  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder Builder(&Machine, Tuning);
  const llvm::OptimizationLevel Level =
      llvmOptimizationLevel(Options.LLVMLevel);

  if (Level != llvm::OptimizationLevel::O0) {
    Builder.registerPipelineEarlySimplificationEPCallback(
        [Policy, Sink](llvm::ModulePassManager &Pipeline,
                       llvm::OptimizationLevel, llvm::ThinOrFullLTOPhase) {
          llvm::FunctionPassManager Functions;
          Functions.addPass(TranslationSemanticConvergencePass(
              Policy, /*RunCanonicalization=*/true, Sink));
          Pipeline.addPass(
              llvm::createModuleToFunctionPassAdaptor(std::move(Functions)));
        });
    Builder.registerScalarOptimizerLateEPCallback(
        [Policy, Sink](llvm::FunctionPassManager &Functions,
                       llvm::OptimizationLevel) {
          Functions.addPass(TranslationSemanticConvergencePass(
              Policy, /*RunCanonicalization=*/true, Sink));
        });
  }

  Builder.registerModuleAnalyses(MAM);
  Builder.registerCGSCCAnalyses(CGAM);
  Builder.registerFunctionAnalyses(FAM);
  Builder.registerLoopAnalyses(LAM);
  Builder.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::ModulePassManager Pipeline;
  if (Level == llvm::OptimizationLevel::O0) {
    llvm::FunctionPassManager Before;
    Before.addPass(TranslationSemanticConvergencePass(
        Policy, /*RunCanonicalization=*/true, Sink));
    Pipeline.addPass(
        llvm::createModuleToFunctionPassAdaptor(std::move(Before)));
    Pipeline.addPass(Builder.buildO0DefaultPipeline(Level));
    llvm::FunctionPassManager After;
    After.addPass(TranslationSemanticConvergencePass(
        Policy, /*RunCanonicalization=*/true, Sink));
    Pipeline.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(After)));
  } else {
    Pipeline.addPass(Builder.buildPerModuleDefaultPipeline(Level));
  }
  Pipeline.run(Module, MAM);
  mergeSemanticReport(Report, *Sink);
}

/// Drop optimizer-inferred assumptions at the sealed generated-code boundary
/// and express the runtime registry as direct, non-interposable symbols.  This
/// only weakens optimization facts; it does not rewrite values or control flow.
void sealCodeGenerationIR(llvm::Module &Module) {
  for (llvm::Function &Function : Module) {
    if (Function.isIntrinsic()) {
      // The default optimization pipeline may infer declaration attributes
      // in addition to the intrinsic's semantic contract.  Rebuild the exact
      // canonical attribute list so the final verifier and cache boundary do
      // not depend on which inference passes happened to run.
      Function.setAttributes(llvm::Intrinsic::getAttributes(
          Module.getContext(), Function.getIntrinsicID(),
          Function.getFunctionType()));
    } else {
      Function.setAttributes(llvm::AttributeList());
      Function.addFnAttr(llvm::Attribute::NoUnwind);
      Function.setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
      Function.setDSOLocal(true);
      if (Function.isDeclaration() &&
          findRuntimeABIHelperSignatureV1(Function.getName()))
        Function.setVisibility(llvm::GlobalValue::HiddenVisibility);
    }
    if (Function.isDeclaration())
      continue;
    for (llvm::Instruction &Instruction : llvm::instructions(Function)) {
      // LLVM's target-aware pipeline may infer nowrap/exact/inbounds/range
      // facts that are valid only under LLVM poison semantics.  Translated
      // guest execution is total at this boundary, so retain the optimized
      // value graph while weakening every such annotation before the final
      // NeverD verifier and object emission.
      Instruction.dropPoisonGeneratingAnnotations();
      auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
      if (!Call)
        continue;
      Call->setAttributes(llvm::AttributeList());
      Call->addFnAttr(llvm::Attribute::NoUnwind);
      if (auto *CallInstruction = llvm::dyn_cast<llvm::CallInst>(Call))
        CallInstruction->setTailCallKind(llvm::CallInst::TCK_None);
    }
  }
}

llvm::Expected<std::vector<uint8_t>>
emitObject(llvm::Module &Module, llvm::TargetMachine &Machine,
           uint64_t GeneratedCodeByteBudget) {
  llvm::SmallVector<char, 4096> Storage;
  llvm::raw_svector_ostream Stream(Storage);
  llvm::legacy::PassManager Pipeline;
  if (Machine.addPassesToEmitFile(Pipeline, Stream, nullptr,
                                  llvm::CodeGenFileType::ObjectFile))
    return failure(
        TranslationObjectCompilerErrorCode::ObjectEmissionUnsupported,
        Machine.getTargetTriple().str());
  Pipeline.run(Module);
  if (Storage.empty())
    return failure(TranslationObjectCompilerErrorCode::ObjectEmissionFailed,
                   "LLVM emitted an empty object");

  const uint64_t Size = asStableSize(Storage.size());
  if (GeneratedCodeByteBudget != 0 && Size > GeneratedCodeByteBudget)
    return budgetFailure(
        TranslationObjectCompilerErrorCode::GeneratedCodeBudgetExceeded, Size,
        GeneratedCodeByteBudget,
        ("emitted " + llvm::Twine(Size) + " bytes for a " +
         llvm::Twine(GeneratedCodeByteBudget) + "-byte budget")
            .str());
  return std::vector<uint8_t>(Storage.begin(), Storage.end());
}

std::string
createArtifactCacheKey(llvm::StringRef RequestCacheKey,
                       llvm::ArrayRef<uint8_t> FinalIR,
                       llvm::ArrayRef<uint8_t> Object,
                       llvm::ArrayRef<TranslationObjectSymbolV1> Blocks,
                       llvm::ArrayRef<TranslationObjectSymbolV1> Runtime) {
  detail::StableHashWriter Hash;
  Hash.addString("neverd.translation-object-artifact.v1");
  Hash.addString(RequestCacheKey);
  Hash.addBytes(FinalIR);
  Hash.addBytes(Object);
  Hash.addU64(Blocks.size());
  for (const TranslationObjectSymbolV1 &Symbol : Blocks) {
    Hash.addString(Symbol.IRName);
    Hash.addString(Symbol.ObjectName);
  }
  Hash.addU64(Runtime.size());
  for (const TranslationObjectSymbolV1 &Symbol : Runtime) {
    Hash.addString(Symbol.IRName);
    Hash.addString(Symbol.ObjectName);
  }
  return Hash.finish("neverd.translation-object-artifact.v1.sha256:");
}

} // namespace

void TranslationObjectCompilerError::log(llvm::raw_ostream &OS) const {
  OS << "translation object compiler: " << errorCodeName(Code);
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code TranslationObjectCompilerError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}

TranslationSemanticPolicyV1 TranslationSemanticPolicyV1::bounded() {
  TranslationSemanticPolicyV1 Policy;
  Policy.Simplify = SymSimplifyOptions();
  Policy.Simplify.Provider = ProofProvider::BuiltInSolver;
  Policy.MaxRounds = 8;
  return Policy;
}

TranslationSemanticPolicyV1 TranslationSemanticPolicyV1::unlimited() {
  TranslationSemanticPolicyV1 Policy;
  Policy.Simplify = SymSimplifyOptions::aggressive();
  Policy.Simplify.MBA = symbolic::MBAOptions::unlimited();
  Policy.Simplify.Provider = ProofProvider::BuiltInSolver;
  // Candidate synthesis remains an explicit opt-in.  If enabled by a caller,
  // the resource ceilings below are already removed and every accepted
  // candidate still requires the built-in equivalence proof.
  Policy.Simplify.EnableSynthesis = false;
  Policy.Simplify.Synthesis.MaxWork = std::numeric_limits<size_t>::max();
  Policy.Simplify.Synthesis.UseStochasticFallback = false;
  Policy.Simplify.Synthesis.StochasticRestarts =
      std::numeric_limits<unsigned>::max();
  Policy.Simplify.Synthesis.StochasticIterations =
      std::numeric_limits<size_t>::max();
  Policy.Simplify.Solver = solver::SolverOptions::unlimited();
  Policy.MaxRounds = 0;
  return Policy;
}

llvm::Expected<TranslationObjectArtifactV1>
compileTranslationObjectWithTargetV1(const llvm::Module &Module,
                                     const TranslationOptions &Options,
                                     const TranslationObjectPolicyV1 &Policy,
                                     TranslationTargetMachineV1 &Target) {
  if (llvm::Error Error = validateTranslationOptions(Options))
    return failure(TranslationObjectCompilerErrorCode::InvalidRequest,
                   std::move(Error));
  if (!Target.matchesCodeGenerationOptions(Options))
    return failure(TranslationObjectCompilerErrorCode::InvalidRequest,
                   "target machine was not created from this request");

  llvm::Expected<RuntimeSymbolRegistryV1> RuntimeRegistry =
      RuntimeSymbolRegistryV1::create();
  if (!RuntimeRegistry)
    return failure(
        TranslationObjectCompilerErrorCode::RuntimeRegistryUnavailable,
        RuntimeRegistry.takeError());

  llvm::Expected<CanonicalPolicy> Canonical =
      canonicalizePolicy(Module, Policy, *RuntimeRegistry);
  if (!Canonical)
    return Canonical.takeError();

  const llvm::Triple HostTriple(Target.hostTarget().triple());
  const llvm::DataLayout &HostDataLayout = Target.dataLayout();

  if (llvm::Error Error = verifyRuntimeTranslationIRV1(
          Module, HostTriple, HostDataLayout, Canonical->StateSize,
          Canonical->StateSlots))
    return failure(
        TranslationObjectCompilerErrorCode::InputIRVerificationFailed,
        std::move(Error));

  const std::vector<uint8_t> InputBitcode = serializeBitcode(Module);
  const std::string RequestCacheKey = createRequestCacheKey(
      InputBitcode, Options, *Canonical, Target.hostTarget(), HostDataLayout,
      *RuntimeRegistry);

  // Every transform and every backend preparation step owns this clone.  The
  // const input remains byte-for-byte unchanged on all exits below.
  std::unique_ptr<llvm::Module> Working = llvm::CloneModule(Module);
  TranslationSemanticReportV1 SemanticReport;
  bool LLVMPipelineRan = false;
  switch (Options.Optimization) {
  case TranslationOptimizationPolicy::None:
    break;
  case TranslationOptimizationPolicy::ProvenSemantic:
    runSemanticOnly(*Working, Canonical->Semantic, SemanticReport);
    break;
  case TranslationOptimizationPolicy::ProvenSemanticAndLLVM:
    runCombinedOptimization(
        *Working,
        detail::TranslationObjectCompilerAccess::targetMachine(Target), Options,
        Canonical->Semantic, SemanticReport);
    LLVMPipelineRan = true;
    break;
  }

  sealCodeGenerationIR(*Working);
  if (llvm::Error Error = verifyRuntimeTranslationIRV1(
          *Working, HostTriple, HostDataLayout, Canonical->StateSize,
          Canonical->StateSlots))
    return failure(
        TranslationObjectCompilerErrorCode::FinalIRVerificationFailed,
        std::move(Error));

  llvm::Expected<std::vector<TranslationObjectSymbolV1>> BlockSymbols =
      createBlockSymbolMappings(*Working, Canonical->RequiredBlockSymbols);
  if (!BlockSymbols)
    return BlockSymbols.takeError();
  std::vector<TranslationObjectSymbolV1> RuntimeSymbols =
      createRuntimeSymbolMappings(*Working, *RuntimeRegistry);
  if (llvm::Error Error =
          validateObjectSymbolMappings(*BlockSymbols, RuntimeSymbols))
    return std::move(Error);

  const std::vector<uint8_t> FinalIR = serializeBitcode(*Working);
  llvm::Expected<std::vector<uint8_t>> Object = emitObject(
      *Working, detail::TranslationObjectCompilerAccess::targetMachine(Target),
      Options.GeneratedCodeByteBudget);
  if (!Object)
    return Object.takeError();

  llvm::SmallVector<llvm::StringRef, 16> RequiredObjectSymbols;
  for (const TranslationObjectSymbolV1 &Symbol : *BlockSymbols)
    RequiredObjectSymbols.push_back(Symbol.ObjectName);
  llvm::SmallVector<llvm::StringRef, 16> AllowedRuntimeSymbols;
  for (const TranslationObjectSymbolV1 &Symbol : RuntimeSymbols)
    AllowedRuntimeSymbols.push_back(Symbol.ObjectName);
  const TranslationArtifactPolicyV1 ArtifactPolicy(RequiredObjectSymbols,
                                                   AllowedRuntimeSymbols);
  if (llvm::Error Error =
          verifyTranslationArtifact(*Object, HostTriple, ArtifactPolicy))
    return failure(
        TranslationObjectCompilerErrorCode::ArtifactVerificationFailed,
        std::move(Error));

  const std::string ArtifactCacheKey = createArtifactCacheKey(
      RequestCacheKey, FinalIR, *Object, *BlockSymbols, RuntimeSymbols);
  return TranslationObjectArtifactV1(
      std::move(*Object), Target.hostTarget(), SemanticReport, LLVMPipelineRan,
      std::move(*BlockSymbols), std::move(RuntimeSymbols),
      RuntimeRegistry->identity().str(), RequestCacheKey, ArtifactCacheKey);
}

llvm::Expected<TranslationObjectArtifactV1>
compileTranslationObjectV1(const llvm::Module &Module,
                           const TranslationOptions &Options,
                           const TranslationObjectPolicyV1 &Policy) {
  llvm::Expected<TranslationTargetMachineV1> Target =
      createCompilerTarget(Options);
  if (!Target)
    return Target.takeError();
  return compileTranslationObjectWithTargetV1(Module, Options, Policy, *Target);
}

} // namespace neverd::translate
