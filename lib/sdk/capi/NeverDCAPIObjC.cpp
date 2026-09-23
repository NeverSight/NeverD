//===- NeverDCAPIObjC.cpp - Objective-C source projection JSON -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../ir/high/pass/HighDCEDetail.h"
#include "JSONText.h"
#include "NativePhaseTrace.h"
#include "ObjCBlockSources.h"
#include "ObjCImmutableStringCallbackSources.h"
#include "ObjCMetadataFactorySources.h"
#include "ObjCNativeDependencies.h"
#include "ObjCSourceBindings.h"
#include "ObjCSourceInputs.h"
#include "ObjCSourceProjection.h"
#include "ObjCSuperGetterSources.h"
#include "ObjCSwiftBooleanSources.h"
#include "ObjCSwiftOnceSources.h"
#include "SessionImpl.h"
#include "SourceProjectionEvidenceJSON.h"
#include "SourceRegisterCopyProjection.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/loader/ObjC/ObjCMetadataJSON.h"
#include "neverd/loader/ObjC/ObjCMethods.h"

#include "llvm/ADT/StringExtras.h"

#include <exception>
#include <map>

using namespace neverd;
using namespace neverd::sdk;

namespace {

std::string addressText(va_t Address) {
  return "0x" + llvm::utohexstr(Address, /*LowerCase=*/true);
}

llvm::json::Object methodIdentity(const ObjCMethod &Method) {
  return llvm::json::Object{
      {"class_name", jsonSafeText(Method.ClassName)},
      {"selector", jsonSafeText(Method.Selector)},
      {"class_method", Method.IsClassMethod},
      {"category_name", jsonSafeText(Method.CategoryName)},
      {"category_address", addressText(Method.CategoryAddress)},
      {"implementation", addressText(Method.Implementation)},
      {"type_encoding", jsonSafeText(Method.TypeEncoding)}};
}

const char *objcMethodsJSON(neverd_session_t Sess, size_t MaxFunctions,
                            bool IncludeSources) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  NativePhaseTrace Trace(NativePhaseTrace::Phase::ObjCExport);
  try {
    if (!S->Loaded || S->Img.Format != BinaryFormat::MachO) {
      S->setError("Objective-C source export requires a loaded Mach-O image");
      Trace.finish(false);
      return nullptr;
    }
    llvm::LLVMContext Context;
    PipelineOptions Options;
    S->applyAnalysisOptions(Options);
    Options.MaxFunctions = MaxFunctions;
    Options.EmitDumpOutput = false;
    Pipeline Engine;
    auto RunPipeline = [&](unsigned Iteration) {
      NativePhaseTrace PipelineTrace(NativePhaseTrace::Phase::Pipeline,
                                     Iteration);
      auto Result = Engine.run(S->Img, Context, Options, S->Dbg.get());
      PipelineTrace.finish(Result.Success);
      return Result;
    };
    auto Result = RunPipeline(0);
    if (!Result.Success) {
      S->setError(Result.Error.empty() ? "native source pipeline failed"
                                       : Result.Error);
      Trace.finish(false);
      return nullptr;
    }

    std::map<va_t, std::string> NativeDependencies;
    const ObjCBlockSourceContext BlockSource(S->Img);
    ObjCBlockSourcePlan BlockPlan;
    SwiftOnceSourcePlan OncePlan;
    std::set<va_t> OnceRoots;
    std::set<va_t> OnceCallOnlyTargets;
    for (unsigned Depth = 0; Depth < 16; ++Depth) {
      bool WitnessChanged = false;
      for (const auto &Function : Result.HighFuncs) {
        const auto Hint =
            objc_binding_detail::swiftWitnessAccessorCallHint(Function,
                                                               S->Img);
        if (!Hint || Options.SourceCalleeTypeHints.count(Function.Entry))
          continue;
        Options.SourceCalleeTypeHints.emplace(Function.Entry,
                                               Hint->Signature);
        WitnessChanged = true;
      }
      OncePlan = discoverSwiftOnceSources(S->Img, Result);
      const bool OnceChanged =
          applySwiftOnceSourceHints(OncePlan, Options) != 0;
      OnceRoots.clear();
      for (const auto &[Address, Hint] : OncePlan.CallbackHints)
        OnceRoots.insert(Address);
      OnceCallOnlyTargets.clear();
      for (const auto &[Address, Contract] : OncePlan.Addressors)
        OnceCallOnlyTargets.insert(Address);
      std::map<va_t, const HighFunc *> RefinementInputs;
      for (const auto &Function : Result.HighFuncs)
        RefinementInputs.emplace(Function.Entry, &Function);
      std::map<va_t, HighFunc> SourceRefinements;
      const ObjCProfileStorage RefinementStorage(S->Img);
      const auto RefinementTargets =
          walkObjCNativeDependencies(S->Img, Result, nullptr, OnceRoots);
      for (const auto &Function : Result.HighFuncs) {
        if (!RefinementTargets.count(Function.Entry) ||
            !Function.SourceTypeHint ||
            Function.SourceTypeHint->Origin !=
                SourceFunctionTypeHint::OriginKind::NativeAnalysis)
          continue;
        auto OnceBinding = bindSwiftOnceSourceReferences(
            Function, S->Img, OncePlan, RefinementInputs);
        auto Binding = bindObjCSourceReferences(
            OnceBinding.Function, S->Img, &RefinementStorage,
            &RefinementInputs);
        elimUnreadPrivateFrameStores(Binding.Function, S->Img.Arch);
        SourceRefinements.emplace(Function.Entry, std::move(Binding.Function));
      }
      const auto CalleeContracts =
          swiftOnceNativeCalleeContracts(S->Img, Result, OncePlan);
      const bool NativeChanged = inferObjCNativeDependencies(
          S->Img, Result, Options, NativeDependencies, OnceRoots,
          OnceCallOnlyTargets, &SourceRefinements, &CalleeContracts);
      BlockPlan = discoverObjCBlockSources(BlockSource, Result);
      const bool BlocksChanged =
          applyObjCBlockInvokeHints(BlockPlan, Options) != 0;
      if (!NativeChanged && !BlocksChanged && !OnceChanged && !WitnessChanged)
        break;
      Result = RunPipeline(Depth + 1);
      if (!Result.Success) {
        S->setError(Result.Error.empty()
                        ? "native dependency source pipeline failed"
                        : Result.Error);
        Trace.finish(false);
        return nullptr;
      }
    }

    // Stack expression identities belong to the final pipeline result.
    BlockPlan = discoverObjCBlockSources(BlockSource, Result);
    OncePlan = discoverSwiftOnceSources(S->Img, Result);
    OnceRoots.clear();
    for (const auto &[Address, Hint] : OncePlan.CallbackHints)
      OnceRoots.insert(Address);
    NativeSourceDependencyEvidence NativeEvidence;
    const auto NativeTargets =
        walkObjCNativeDependencies(S->Img, Result, &NativeEvidence, OnceRoots);

    CEmitterOptions COptions;
    COptions.TheArch = S->Img.Arch;
    COptions.Format = S->Img.Format;
    COptions.Image = &S->Img;
    COptions.EmitComments = false;
    // Each method source is one complete translation unit. The mobile source
    // assembler parses those units and validates shared definitions before it
    // combines them, so record macro guards are unnecessary and would weaken
    // its deliberately macro-free parsing boundary.
    COptions.EmitRecordGuards = false;
    COptions.UseDebugNames = false;
    HighCEmitter Emitter;
    std::string NativeSource;
    llvm::raw_string_ostream NativeOS(NativeSource);
    llvm::raw_null_ostream DiscardNative;
    llvm::raw_ostream &NativeOutput =
        IncludeSources ? static_cast<llvm::raw_ostream &>(NativeOS)
                       : DiscardNative;
    if (!Emitter.emit(Result.HighFuncs, NativeOutput, COptions)) {
      S->setError("native C source projection failed");
      Trace.finish(false);
      return nullptr;
    }
    std::map<va_t, const HighFunc *> Functions;
    size_t NativeFunctionCount = 0;
    for (const HighFunc &Func : Result.HighFuncs) {
      if (Func.Name.empty())
        continue;
      Functions.emplace(Func.Entry, &Func);
      ++NativeFunctionCount;
    }
    // Nested once callbacks retain their machine body until the final source
    // projection. Publish only a separately proved copy to source consumers;
    // Low/Med ABI inference and ordinary native calls keep the original body.
    std::map<va_t, ObjCSourceBindingResult> NestedOnceInputs;
    for (const auto &[Entry, Contract] : OncePlan.NestedCallbacks) {
      const auto Found = Functions.find(Entry);
      if (Found == Functions.end() || !Found->second)
        continue;
      auto Projection = projectSwiftOnceNestedCallback(*Found->second, S->Img,
                                                       OncePlan, Functions);
      if (Projection)
        NestedOnceInputs.emplace(Entry, std::move(*Projection));
    }
    for (const auto &[Entry, Projection] : NestedOnceInputs)
      Functions[Entry] = &Projection.Function;
    std::map<va_t, ObjCSourceBindingResult> ImmutableStringInputs;
    for (const auto &[Entry, Hint] : OncePlan.CallbackHints) {
      const auto Found = Functions.find(Entry);
      if (Found == Functions.end() || !Found->second)
        continue;
      auto Projection = projectObjCImmutableStringCallback(
          *Found->second, S->Img, Result, OncePlan);
      if (Projection)
        ImmutableStringInputs.emplace(Entry, std::move(*Projection));
    }
    for (const auto &[Entry, Projection] : ImmutableStringInputs)
      Functions[Entry] = &Projection.Function;
    std::map<va_t, const PipelineFunctionAudit *> Audits;
    for (const PipelineFunctionAudit &Audit : Result.FunctionAudits)
      Audits.emplace(Audit.Entry, &Audit);

    std::map<va_t, ObjCSourceBindingResult> Projections;
    const ObjCProfileStorage ProfileStorage(S->Img);
    const SourceRegisterCopyProjectionValidator RegisterCopies(S->Img, Result);
    std::map<va_t, ObjCBlockSourceBindingResult> BlockProjections;
    const auto SuperGetterPlan = discoverObjCSuperGetterSources(S->Img, Result);
    std::set<va_t> SuperGetterProjections;
    const auto MetadataFactoryPlan =
        discoverObjCMetadataFactorySources(S->Img, Result, ProfileStorage);
    std::set<va_t> MetadataFactoryProjections;
    std::map<va_t, std::string> ProjectionReasons;
    std::set<va_t> Closed;
    for (const auto &[Entry, Func] : Functions) {
      const bool ObjCOnceThunk = OncePlan.ObjCThunks.count(Entry) != 0;
      if (!Func->SourceTypeHint && !ObjCOnceThunk)
        continue;
      auto BlockBinding = bindObjCBlockSourceReferences(*Func, BlockSource,
                                                        BlockPlan, Functions);
      auto Inputs =
          snapshotObjCEntryInputs(BlockBinding.Function, S->Img, Functions);
      auto OnceBinding =
          bindSwiftOnceSourceReferences(Inputs, S->Img, OncePlan, Functions);
      if (ObjCOnceThunk && (!OnceBinding.SwiftOnceObjCThunks.count(Entry) ||
                            !finalizeSwiftOnceObjCThunkProjection(
                                OnceBinding.Function, OncePlan)))
        continue;
      if (const auto Nested = NestedOnceInputs.find(Entry);
          Nested != NestedOnceInputs.end()) {
        OnceBinding.Dependencies.insert(Nested->second.Dependencies.begin(),
                                        Nested->second.Dependencies.end());
        OnceBinding.LocalStorageExtents.insert(
            Nested->second.LocalStorageExtents.begin(),
            Nested->second.LocalStorageExtents.end());
      }
      auto SuperGetterBinding =
          projectObjCSuperGetter(OnceBinding.Function, S->Img, SuperGetterPlan);
      if (SuperGetterBinding.Projected)
        SuperGetterProjections.insert(Entry);
      auto MetadataFactoryBinding =
          projectObjCMetadataFactory(SuperGetterBinding.Function, S->Img,
                                     MetadataFactoryPlan, ProfileStorage);
      if (MetadataFactoryBinding.Projected)
        MetadataFactoryProjections.insert(Entry);
      // The immutable callback already carries all current source bindings.
      // Its proved scalar pool offsets may numerically overlap image code;
      // reinterpreting those generated offsets as raw machine addresses would
      // discard their caller-specific proof. Revalidate the entire body below.
      auto Binding = ImmutableStringInputs.count(Entry)
                         ? ImmutableStringInputs.at(Entry)
                         : bindObjCSourceReferences(
                               MetadataFactoryBinding.Function, S->Img,
                               &ProfileStorage, &Functions);
      if (const auto Immutable = ImmutableStringInputs.find(Entry);
          Immutable != ImmutableStringInputs.end()) {
        Binding.Function = MetadataFactoryBinding.Function;
        if (!objCImmutableStringCallbackValid(Binding.Function, S->Img, Result,
                                             OncePlan))
          Binding.Limitation =
              "immutable string callback proof is no longer valid";
      }
      Binding.Dependencies.insert(MetadataFactoryBinding.Dependencies.begin(),
                                  MetadataFactoryBinding.Dependencies.end());
      Binding.ProfileCounterSections.insert(
          MetadataFactoryBinding.ProfileSections.begin(),
          MetadataFactoryBinding.ProfileSections.end());
      Binding.Dependencies.insert(SuperGetterBinding.Dependencies.begin(),
                                  SuperGetterBinding.Dependencies.end());
      Binding.Dependencies.insert(OnceBinding.Dependencies.begin(),
                                  OnceBinding.Dependencies.end());
      Binding.SwiftOnceAccessors.insert(OnceBinding.SwiftOnceAccessors.begin(),
                                        OnceBinding.SwiftOnceAccessors.end());
      Binding.SwiftOnceObjCThunks.insert(
          OnceBinding.SwiftOnceObjCThunks.begin(),
          OnceBinding.SwiftOnceObjCThunks.end());
      for (const auto &[Address, Width] : OnceBinding.LocalStorageExtents)
        Binding.LocalStorageExtents[Address] =
            std::max(Binding.LocalStorageExtents[Address], Width);
      Binding.Dependencies.insert(BlockBinding.Dependencies.begin(),
                                  BlockBinding.Dependencies.end());
      std::string Reason = BlockBinding.Limitation.empty()
                               ? Binding.Limitation
                               : BlockBinding.Limitation;
      if (!RegisterCopies.valid(Binding.Function))
        Reason = "source register-copy proof is no longer valid";
      if (Reason.empty()) {
        auto ReadOnlyHelpers =
            readOnlyScalarSourceHelpers(Binding.Function, S->Img);
        const auto ObjectPointerHelpers =
            readOnlyObjectPointerSourceHelpers(Binding.Function, S->Img);
        ReadOnlyHelpers.insert(ObjectPointerHelpers.begin(),
                               ObjectPointerHelpers.end());
        const auto Audit = Audits.find(Entry);
        Reason = sourceBodyLimitation(
            Binding.Function, *Binding.Function.SourceTypeHint,
            Audit == Audits.end() ? nullptr : Audit->second,
            [&](const HighExpr &Expression) {
              return objcSourceCallBound(Expression, S->Img, Functions,
                                         &ProfileStorage, &ReadOnlyHelpers,
                                         &Binding.Function) ||
                     objCSwiftBooleanSourceCallBound(Expression, S->Img, Result,
                                                     Binding.Function) ||
                     objCMetadataFactorySourceCallBound(
                         Expression, S->Img, MetadataFactoryPlan,
                         ProfileStorage, Binding.Function, Functions) ||
                     objCSuperGetterSourceCallBound(
                         Expression, S->Img, SuperGetterPlan, Binding.Function,
                         Functions) ||
                     swiftOnceCallbackBound(Expression, S->Img, OncePlan,
                                            Functions) ||
                     swiftOnceAddressorBound(Expression, S->Img, OncePlan,
                                             Functions) ||
                     objcBlockSourceCallBound(Expression, BlockSource,
                                              BlockPlan, Functions);
            });
      }
      if (Reason.empty())
        Closed.insert(Entry);
      ProjectionReasons.emplace(Entry, std::move(Reason));
      Projections.emplace(Entry, std::move(Binding));
      BlockProjections.emplace(Entry, std::move(BlockBinding));
    }
    // Keep the local result distinct from failures propagated through callees.
    const auto LocalProjectionReasons = ProjectionReasons;
    bool Changed;
    do {
      Changed = false;
      for (const auto &[Entry, Projection] : Projections) {
        if (!Closed.count(Entry))
          continue;
        for (va_t Dependency : Projection.Dependencies)
          if (!Closed.count(Dependency)) {
            Closed.erase(Entry);
            ProjectionReasons[Entry] = "method depends on native source that "
                                       "was not completely recovered";
            Changed = true;
            break;
          }
      }
    } while (Changed);

    // Read evidence from the final production projections. LowIR CALL edges
    // alone omit Block dependencies and are not the source publication graph.
    std::set<va_t> EvidenceEntries = NativeTargets;
    EvidenceEntries.insert(NativeEvidence.Roots.begin(),
                           NativeEvidence.Roots.end());
    std::vector<va_t> EvidencePending(EvidenceEntries.begin(),
                                      EvidenceEntries.end());
    while (!EvidencePending.empty()) {
      const auto Entry = EvidencePending.back();
      EvidencePending.pop_back();
      const auto Projection = Projections.find(Entry);
      if (Projection == Projections.end())
        continue;
      for (const auto Dependency : Projection->second.Dependencies)
        if (EvidenceEntries.insert(Dependency).second)
          EvidencePending.push_back(Dependency);
    }
    llvm::json::Array ProjectionNodes;
    for (const auto Entry : EvidenceEntries) {
      const auto Projection = Projections.find(Entry);
      const bool HasProjection = Projection != Projections.end();
      llvm::json::Object Node{
          {"address", addressText(Entry)},
          {"name", jsonSafeText(S->Img.getFunctionNameAt(Entry))},
          {"has_typed_body", HasProjection},
          {"local_gate_passed",
           HasProjection && LocalProjectionReasons.at(Entry).empty()},
          {"closure_closed", Closed.count(Entry) != 0}};
      llvm::json::Array Dependencies;
      SourceProjectionDiagnostics Evidence;
      if (HasProjection) {
        const auto &Binding = Projection->second;
        const auto &Block = BlockProjections.at(Entry);
        const auto Audit = Audits.find(Entry);
        const auto ReadOnlyHelpers =
            readOnlyScalarSourceHelpers(Binding.Function, S->Img);
        Evidence = sourceBodyDiagnostics(
            Binding.Function, *Binding.Function.SourceTypeHint,
            Audit == Audits.end() ? nullptr : Audit->second,
            [&](const HighExpr &Expression) {
              return objcSourceCallBound(Expression, S->Img, Functions,
                                         &ProfileStorage, &ReadOnlyHelpers,
                                         &Binding.Function) ||
                     objCSwiftBooleanSourceCallBound(Expression, S->Img, Result,
                                                     Binding.Function) ||
                     objCMetadataFactorySourceCallBound(
                         Expression, S->Img, MetadataFactoryPlan,
                         ProfileStorage, Binding.Function, Functions) ||
                     objCSuperGetterSourceCallBound(
                         Expression, S->Img, SuperGetterPlan, Binding.Function,
                         Functions) ||
                     swiftOnceCallbackBound(Expression, S->Img, OncePlan,
                                            Functions) ||
                     swiftOnceAddressorBound(Expression, S->Img, OncePlan,
                                             Functions) ||
                     objcBlockSourceCallBound(Expression, BlockSource,
                                              BlockPlan, Functions);
            });
        if (ImmutableStringInputs.count(Entry) &&
            !objCImmutableStringCallbackValid(Binding.Function, S->Img, Result,
                                             OncePlan)) {
          Evidence.Complete = false;
          Evidence.add(SourceProjectionIssue::Body,
                       "immutable string callback proof is no longer valid");
        }
        Evidence.append(Binding.Diagnostics);
        if (!Block.Limitation.empty()) {
          Evidence.Complete = false;
          Evidence.add(SourceProjectionIssue::Dependency, Block.Limitation);
        }
        for (const auto Dependency : Binding.Dependencies)
          Dependencies.push_back(addressText(Dependency));
        Node["local_reason"] = jsonSafeText(LocalProjectionReasons.at(Entry));
        Node["closure_reason"] = jsonSafeText(ProjectionReasons.at(Entry));
      } else {
        Evidence.Complete = false;
        std::string Reason =
            "native function has no complete typed source body";
        if (const auto It = NativeDependencies.find(Entry);
            It != NativeDependencies.end() && !It->second.empty())
          Reason = It->second;
        Evidence.add(SourceProjectionIssue::Signature, Reason);
        Node["local_reason"] = jsonSafeText(Reason);
        Node["closure_reason"] = jsonSafeText(Reason);
      }
      Node["dependencies"] = std::move(Dependencies);
      Node["local_diagnostics"] = sourceProjectionEvidenceJSON(Evidence);
      ProjectionNodes.push_back(std::move(Node));
    }

    llvm::json::Array Methods;
    size_t Recovered = 0;
    for (const ObjCMethod &Method : S->Img.ObjCMethods) {
      auto Row = methodIdentity(Method);
      Row["metadata_address"] = addressText(Method.MetadataAddress);
      llvm::json::Array Diagnostics;
      for (const std::string &Diagnostic : Method.Diagnostics)
        Diagnostics.push_back(jsonSafeText(Diagnostic));
      Row["diagnostics"] = std::move(Diagnostics);
      std::string Reason;
      SourceProjectionDiagnostics Evidence;
      const HighExpr *UnboundCall = nullptr;
      const HighFunc *Func = nullptr;
      if (auto It = Functions.find(Method.Implementation);
          It != Functions.end())
        Func = It->second;
      if (Method.Status != "supported" || !Method.TypeHint) {
        Reason = "runtime method signature is not supported: " + Method.Status;
        Evidence.Complete = false;
        Evidence.add(SourceProjectionIssue::Signature, Reason);
      } else if (!Func || !Projections.count(Method.Implementation)) {
        Reason = "method has no complete typed source body (possibly limited "
                 "by max-func)";
        Evidence.Complete = false;
        Evidence.add(SourceProjectionIssue::Body, Reason);
      } else {
        auto It = Audits.find(Method.Implementation);
        const auto &Projection = Projections.at(Method.Implementation);
        const auto *Audit = It == Audits.end() ? nullptr : It->second;
        auto ReadOnlyHelpers =
            readOnlyScalarSourceHelpers(Projection.Function, S->Img);
        const auto ObjectPointerHelpers =
            readOnlyObjectPointerSourceHelpers(Projection.Function, S->Img);
        ReadOnlyHelpers.insert(ObjectPointerHelpers.begin(),
                               ObjectPointerHelpers.end());
        auto CallAllowed = [&](const HighExpr &Expression) {
          return objcSourceCallBound(Expression, S->Img, Functions,
                                     &ProfileStorage, &ReadOnlyHelpers,
                                     &Projection.Function) ||
                 objCSwiftBooleanSourceCallBound(Expression, S->Img, Result,
                                                 Projection.Function) ||
                 objCMetadataFactorySourceCallBound(
                     Expression, S->Img, MetadataFactoryPlan, ProfileStorage,
                     Projection.Function, Functions) ||
                 objCSuperGetterSourceCallBound(
                     Expression, S->Img, SuperGetterPlan, Projection.Function,
                     Functions) ||
                 swiftOnceCallbackBound(Expression, S->Img, OncePlan,
                                        Functions) ||
                 swiftOnceAddressorBound(Expression, S->Img, OncePlan,
                                         Functions) ||
                 objcBlockSourceCallBound(Expression, BlockSource, BlockPlan,
                                          Functions);
        };
        // Per-occurrence inventory may consume more evidence budget than the
        // compatibility gate's shared-expression check. Its resource limits
        // do not change the result of that already completed check.
        Reason = objcSourceBodyLimitation(Projection.Function, *Method.TypeHint,
                                          Audit, CallAllowed, &UnboundCall);
        Evidence = objcSourceBodyDiagnostics(
            Projection.Function, *Method.TypeHint, Audit, CallAllowed);
        if (ImmutableStringInputs.count(Method.Implementation) &&
            !objCImmutableStringCallbackValid(Projection.Function, S->Img,
                                             Result, OncePlan)) {
          Reason = "immutable string callback proof is no longer valid";
          Evidence.Complete = false;
          Evidence.add(SourceProjectionIssue::Body, Reason);
        }
        Evidence.append(Projection.Diagnostics);
        if (!RegisterCopies.valid(Projection.Function)) {
          Reason = "source register-copy proof is no longer valid";
          Evidence.Complete = false;
          Evidence.add(SourceProjectionIssue::Body, Reason);
        }
        const auto &Block = BlockProjections.at(Method.Implementation);
        if (!Block.Limitation.empty()) {
          // Block binding currently stops at its first failed proof.
          Evidence.Complete = false;
          Evidence.add(SourceProjectionIssue::Dependency, Block.Limitation);
        }
        for (va_t Dependency : Projection.Dependencies)
          if (!Closed.count(Dependency))
            Evidence.add(SourceProjectionIssue::Dependency,
                         "method depends on native source that was not "
                         "completely recovered",
                         0, nullptr, Dependency);
        if (Reason.empty()) {
          if (auto Projection = ProjectionReasons.find(Method.Implementation);
              Projection != ProjectionReasons.end())
            Reason = Projection->second;
          else
            Reason = "method has no complete relocatable source projection";
        }
      }
      if (!Reason.empty()) {
        if (Evidence.Items.empty()) {
          Evidence.Complete = false;
          Evidence.add(SourceProjectionIssue::Body, Reason);
        }
        Row["status"] = "unrecovered";
        Row["reason"] = std::move(Reason);
        if (UnboundCall)
          Row["unbound_call"] = sourceCallEvidenceJSON(*UnboundCall);
        Row["projection_diagnostics"] = sourceProjectionEvidenceJSON(Evidence);
        Methods.push_back(std::move(Row));
        continue;
      }

      HighFunc Projection = Projections.at(Method.Implementation).Function;
      Projection.Name =
          "neverd_objc_imp_" +
          llvm::utohexstr(Method.Implementation, /*LowerCase=*/true);
      Projection.DebugName.clear();
      Projection.SourceFile.clear();
      std::string Source;
      llvm::raw_string_ostream SourceOS(Source);
      std::vector<HighFunc> Unit{Projection};
      std::set<va_t> Included{Projection.Entry};
      std::vector<va_t> Pending(
          Projections.at(Projection.Entry).Dependencies.begin(),
          Projections.at(Projection.Entry).Dependencies.end());
      while (!Pending.empty()) {
        const va_t Entry = Pending.back();
        Pending.pop_back();
        if (!Included.insert(Entry).second)
          continue;
        const auto &Dependency = Projections.at(Entry);
        Unit.push_back(Dependency.Function);
        Pending.insert(Pending.end(), Dependency.Dependencies.begin(),
                       Dependency.Dependencies.end());
      }
      std::set<va_t> BlockDescriptors;
      std::set<va_t> BlockLiterals;
      for (va_t Entry : Included) {
        const auto &Descriptors = BlockProjections.at(Entry).Descriptors;
        BlockDescriptors.insert(Descriptors.begin(), Descriptors.end());
        const auto &Literals = BlockProjections.at(Entry).Literals;
        BlockLiterals.insert(Literals.begin(), Literals.end());
      }
      std::set<std::string> SharedBlockFunctions;
      const std::string BlockHelpers = renderObjCBlockSourceHelpers(
          BlockPlan, BlockDescriptors, BlockLiterals, SharedBlockFunctions);
      for (va_t Entry : Included)
        if (BlockPlan.InvokeHints.count(Entry))
          SharedBlockFunctions.insert(objcBlockInvokeName(Entry));
      std::set<va_t> AssociationKeys;
      std::set<va_t> KVOContexts;
      std::set<va_t> StaticIdentities;
      std::set<va_t> ClassReferenceCells;
      std::map<va_t, uint64_t> LocalStorageExtents;
      std::map<va_t, SourceCallTypeHint::SwiftTypeMetadataAddress>
          SwiftTypeMetadataPairs;
      std::map<va_t, va_t> SwiftWitnessCaches;
      std::set<va_t> SwiftOnceAccessors;
      std::set<va_t> ProfileSections;
      std::set<va_t> ConstantStrings;
      std::set<va_t> ConstantObjects;
      std::map<va_t, uint32_t> ConstantObjectTables;
      std::set<BorrowedByteRange> BorrowedBytes;
      std::set<va_t> CStringSections, CStringPointerSlots;
      for (va_t Entry : Included) {
        const auto &Keys = Projections.at(Entry).AssociationKeys;
        AssociationKeys.insert(Keys.begin(), Keys.end());
        const auto &Contexts = Projections.at(Entry).KVOContexts;
        KVOContexts.insert(Contexts.begin(), Contexts.end());
        const auto &Identities = Projections.at(Entry).StaticIdentities;
        StaticIdentities.insert(Identities.begin(), Identities.end());
        const auto &ClassReferences = Projections.at(Entry).ClassReferenceCells;
        ClassReferenceCells.insert(ClassReferences.begin(),
                                   ClassReferences.end());
        for (const auto &[Address, Width] :
             Projections.at(Entry).LocalStorageExtents)
          LocalStorageExtents[Address] =
              std::max(LocalStorageExtents[Address], Width);
        for (const auto &[Address, Pair] :
             Projections.at(Entry).SwiftTypeMetadataPairs) {
          const auto [It, Added] =
              SwiftTypeMetadataPairs.emplace(Address, Pair);
          if (!Added && It->second != Pair)
            throw std::runtime_error(
                "conflicting Swift type metadata source pairs");
        }
        for (const auto &[Address, Accessor] :
             Projections.at(Entry).SwiftWitnessCaches) {
          const auto [It, Added] =
              SwiftWitnessCaches.emplace(Address, Accessor);
          if (!Added && It->second != Accessor)
            throw std::runtime_error(
                "conflicting Swift witness cache source accessors");
        }
        const auto &OnceAccessors = Projections.at(Entry).SwiftOnceAccessors;
        SwiftOnceAccessors.insert(OnceAccessors.begin(), OnceAccessors.end());
        const auto &Sections = Projections.at(Entry).ProfileCounterSections;
        ProfileSections.insert(Sections.begin(), Sections.end());
        const auto &Strings = Projections.at(Entry).ConstantStrings;
        ConstantStrings.insert(Strings.begin(), Strings.end());
        const auto &Objects = Projections.at(Entry).ConstantObjects;
        ConstantObjects.insert(Objects.begin(), Objects.end());
        for (const auto &[Address, ByteCount] :
             Projections.at(Entry).ConstantObjectTables)
          ConstantObjectTables[Address] =
              std::max(ConstantObjectTables[Address], ByteCount);
        const auto &Bytes = Projections.at(Entry).BorrowedBytes;
        BorrowedBytes.insert(Bytes.begin(), Bytes.end());
        const auto &CStrings = Projections.at(Entry).CStringSections;
        CStringSections.insert(CStrings.begin(), CStrings.end());
        const auto &CStringSlots = Projections.at(Entry).CStringPointerSlots;
        CStringPointerSlots.insert(CStringSlots.begin(), CStringSlots.end());
      }
      std::set<std::string> SharedIdentityFunctions;
      for (const auto &[Address, Width] : LocalStorageExtents)
        if (const auto Target =
                objc_binding_detail::localStringPointerInitializer(
                    S->Img, Address, Width))
          ConstantStrings.insert(*Target);
      std::string IdentityHelpers = renderObjCAssociationKeyHelpers(
          AssociationKeys, SharedIdentityFunctions);
      IdentityHelpers +=
          renderObjCKVOContextHelpers(KVOContexts, SharedIdentityFunctions);
      IdentityHelpers += renderObjCStaticIdentityHelpers(
          StaticIdentities, SharedIdentityFunctions);
      IdentityHelpers += renderObjCClassReferenceHelpers(
          S->Img, ClassReferenceCells, SharedIdentityFunctions);
      IdentityHelpers += renderObjCConstantObjectHelpers(
          S->Img, ConstantObjects, ConstantStrings, SharedIdentityFunctions);
      IdentityHelpers += renderObjCConstantObjectTableHelpers(
          S->Img, ConstantObjectTables, SharedIdentityFunctions);
      IdentityHelpers += renderBorrowedByteHelpers(S->Img, BorrowedBytes,
                                                   SharedIdentityFunctions);
      IdentityHelpers += renderCStringStorageHelpers(S->Img, CStringSections,
                                                     SharedIdentityFunctions,
                                                     CStringPointerSlots);
      std::set<va_t> SuperGetters;
      for (const auto Entry : Included)
        if (SuperGetterProjections.count(Entry))
          SuperGetters.insert(Entry);
      IdentityHelpers += renderObjCSuperGetterHelpers(
          S->Img, SuperGetterPlan, SuperGetters, SharedIdentityFunctions);
      std::set<va_t> MetadataFactories;
      for (const auto Entry : Included)
        if (MetadataFactoryProjections.count(Entry))
          MetadataFactories.insert(Entry);
      IdentityHelpers += renderObjCMetadataFactoryHelpers(
          S->Img, MetadataFactoryPlan, ProfileStorage, MetadataFactories,
          SharedIdentityFunctions);
      std::set<std::string> SharedStorageFunctions;
      const std::string StorageHelpers =
          ProfileStorage.render(ProfileSections, SharedStorageFunctions) +
          renderObjCLocalStorageHelpers(S->Img, LocalStorageExtents,
                                        SharedStorageFunctions) +
          renderObjCSwiftTypeMetadataHelpers(S->Img, SwiftTypeMetadataPairs,
                                             SharedStorageFunctions) +
          renderObjCSwiftWitnessCacheHelpers(
              S->Img, SwiftWitnessCaches, Functions, SharedStorageFunctions) +
          renderSwiftOnceAddressorHelpers(S->Img, SwiftOnceAccessors, OncePlan,
                                          Functions, SharedStorageFunctions);
      const bool Emitted = Emitter.emit(Unit, SourceOS, COptions);
      SourceOS << BlockHelpers << IdentityHelpers << StorageHelpers;
      if (!Emitted) {
        Row["status"] = "unrecovered";
        Row["reason"] = "method C source projection failed";
      } else if (auto Limitation = objcSourceTextLimitation(Source);
                 !Limitation.empty()) {
        Row["status"] = "unrecovered";
        Row["reason"] = std::move(Limitation);
      } else {
        llvm::json::Array Parameters;
        for (const HighParam &Parameter : Projection.Params)
          Parameters.push_back(llvm::json::Object{
              {"name", Parameter.Name}, {"type", typeToC(Parameter.Type)}});
        Row["status"] = "recovered";
        std::set<std::string> LayoutClasses;
        std::set<std::string> RuntimeProtocols;
        for (va_t Entry : Included) {
          const auto &Classes = Projections.at(Entry).InstanceLayoutClasses;
          LayoutClasses.insert(Classes.begin(), Classes.end());
          const auto &Protocols = Projections.at(Entry).RuntimeProtocols;
          RuntimeProtocols.insert(Protocols.begin(), Protocols.end());
        }
        if (!Method.IsClassMethod && Method.ClassAddress)
          LayoutClasses.insert(Method.ClassName);
        llvm::json::Array Layouts;
        for (const auto &ClassName : LayoutClasses)
          Layouts.push_back(ClassName);
        Row["instance_layout_classes"] = std::move(Layouts);
        if (!RuntimeProtocols.empty()) {
          llvm::json::Array Protocols;
          for (const auto &Name : RuntimeProtocols)
            Protocols.push_back(Name);
          Row["runtime_protocols"] = std::move(Protocols);
        }
        Row["return_type"] = typeToC(Projection.ReturnType);
        Row["parameters"] = std::move(Parameters);
        Row["function_name"] = Projection.Name;
        // Rendering and the text guard above remain publication checks in
        // both modes. Only retaining/encoding the successful body is optional.
        if (IncludeSources)
          Row["source"] = jsonSafeText(Source);
        llvm::json::Array SharedFunctions;
        for (const auto &Name : SharedBlockFunctions)
          SharedFunctions.push_back(Name);
        Row["shared_block_functions"] = std::move(SharedFunctions);
        llvm::json::Array IdentityFunctions;
        for (const auto &Name : SharedIdentityFunctions)
          IdentityFunctions.push_back(Name);
        Row["shared_identity_functions"] = std::move(IdentityFunctions);
        llvm::json::Array StorageFunctions;
        for (const auto &Name : SharedStorageFunctions)
          StorageFunctions.push_back(Name);
        Row["shared_storage_functions"] = std::move(StorageFunctions);
        ++Recovered;
      }
      if (auto Failure = Row.getString("reason")) {
        Evidence.Complete = false;
        Evidence.add(SourceProjectionIssue::Body, Failure->str());
      }
      Row["projection_diagnostics"] = sourceProjectionEvidenceJSON(Evidence);
      Methods.push_back(std::move(Row));
    }
    llvm::json::Array Limitations;
    Limitations.push_back(
        "Recovered bodies are source projections, not a proof of semantic "
        "equivalence or original source text.");
    Limitations.push_back("Only fixed scalar/pointer Objective-C signatures "
                          "with supported ABI bindings are projected; variadic "
                          "tails are not described by runtime encodings.");
    Limitations.push_back(
        "Unresolved native dependencies and exception-dependent method bodies "
        "remain individually unrecovered.");
    Limitations.push_back(
        "Borrowed immutable byte ranges are copied only for proven bounded, "
        "read-only "
        "consumers that do not retain or compare their pointers. These buffers "
        "preserve contents, not original image addresses or pointer identity.");
    Limitations.push_back(
        "Complete immutable C-string literal sections use shared rebuilt "
        "storage, preserving all bytes, interior offsets and pointer lifetime. "
        "Link one definition of each shared_identity_functions helper; these "
        "addresses are independent of the original loaded image.");
    Limitations.push_back(
        "Verified Darwin constant strings preserve ASCII bytes or UTF-16 "
        "code units in rebuilt constant objects. Link Foundation and one "
        "definition of each shared_identity_functions helper. Equal original "
        "object addresses share one rebuilt object; these identities are "
        "independent of the original loaded image.");
    Limitations.push_back(
        "Bounded immutable Objective-C object-pointer tables rebuild every "
        "reachable slot from a verified constant object identity or exact "
        "null. Only proven in-range full-width loads use the shared table; "
        "its address is independent of the original loaded image.");
    Limitations.push_back(
        "Numeric profiling counters retain their captured initial bytes and "
        "updates in shared rebuilt storage. Link one definition of each "
        "shared_storage_functions helper across the participating sources. "
        "This storage is independent of the original image and its profiling "
        "runtime; ordered, escaping, or unresolved image-data accesses remain "
        "unrecovered.");
    Limitations.push_back(
        "Static associated-object keys use shared rebuilt identities. Link "
        "one definition of each shared_identity_functions helper across the "
        "participating method sources; the identities do not refer to storage "
        "in an already loaded original image.");
    Limitations.push_back(
        "Loader-authenticated self-pointer identities and exact scalar "
        "accesses rooted at uniquely named writable symbols use shared "
        "rebuilt storage. Link one definition of each listed shared identity "
        "or storage helper across participating method sources; the storage "
        "is initialized from the captured image and is independent of an "
        "already loaded original image.");
    llvm::json::Object Report{
        {"schema_version", 1},
        {"status", "success"},
        {"pointer_size", S->Img.Bits == Bitness::Bits64 ? 8 : 4},
        {"native_function_count", static_cast<int64_t>(NativeFunctionCount)},
        {"native_dependency_graph",
         nativeSourceDependencyEvidenceJSON(NativeEvidence)},
        {"source_projection_graph",
         llvm::json::Object{
             {"schema_version", 1},
             {"scope", "final_native_and_block_source_projections"},
             {"native_inventory_complete", NativeEvidence.InventoryComplete},
             {"closure_stage", "before_method_emission_and_text_checks"},
             {"nodes", std::move(ProjectionNodes)}}},
        {"objc_metadata", objcMetadataJSON(S->Img)},
        {"method_count", static_cast<int64_t>(S->Img.ObjCMethods.size())},
        {"recovered_method_count", static_cast<int64_t>(Recovered)},
        {"methods", std::move(Methods)},
        {"limitations", std::move(Limitations)}};
    if (IncludeSources)
      Report["native_source"] = jsonSafeText(NativeSource);
    else
      Report["sources_omitted"] = true;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    OS << llvm::json::Value(std::move(Report));
    char *Output = dupStr(Text);
    if (!Output)
      S->setError("unable to allocate Objective-C source report");
    Trace.finish(Output != nullptr);
    return Output;
  } catch (const std::exception &Error) {
    S->setError(std::string("Objective-C source export failed: ") +
                Error.what());
  } catch (...) {
    S->setError("Objective-C source export failed with an unknown exception");
  }
  Trace.finish(false);
  return nullptr;
}

} // namespace

const char *neverd_objc_methods_json(neverd_session_t Sess,
                                     size_t MaxFunctions) {
  return objcMethodsJSON(Sess, MaxFunctions, true);
}

const char *neverd_objc_methods_summary_json(neverd_session_t Sess,
                                             size_t MaxFunctions) {
  return objcMethodsJSON(Sess, MaxFunctions, false);
}
