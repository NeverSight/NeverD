#include "JSONText.h"
#include "ObjCSourceProjection.h"
#include "SessionImpl.h"
#include "SwiftABIProjectionPlan.h"
#include "SwiftRuntimeProjection.h"
#include "SwiftRuntimeSignatures.h"
#include "SwiftSourceNamespace.h"
#include "SwiftSourceProperties.h"
#include "SwiftSourceSignatures.h"

#include "neverd/backend/swift/HighSwiftEmitter.h"
#include "neverd/loader/Swift/SwiftABI.h"
#include "neverd/loader/Swift/SwiftMetadata.h"

#include "llvm/ADT/StringExtras.h"

#include <cstring>
#include <map>
#include <set>

using namespace neverd;
using namespace neverd::sdk;

const char *neverd_swift_methods_json(neverd_session_t Sess,
                                      const char *SignaturesJson,
                                      size_t MaxFunctions) {
  auto *Session = static_cast<sdk::Session *>(Sess);
  if (!Session)
    return nullptr;
  Session->clearError();
  try {
    if (!Session->Loaded || Session->Img.Format != BinaryFormat::MachO ||
        Session->Img.Bits != Bitness::Bits64 || !SignaturesJson)
      throw std::invalid_argument("Swift source export requires a loaded "
                                  "64-bit Mach-O and signature JSON");
    llvm::StringRef Input(SignaturesJson);
    if (Input.size() > 32 * 1024 * 1024)
      throw std::invalid_argument(
          "Swift signature input exceeds its byte budget");
    auto Parsed = llvm::json::parse(Input);
    if (!Parsed)
      throw std::invalid_argument(llvm::toString(Parsed.takeError()));
    auto Object = Parsed->getAsObject();
    auto Methods = Object ? Object->getArray("methods") : nullptr;
    if (!Object || Object->getInteger("schema_version") != 1 || !Methods ||
        Methods->size() > 65536)
      throw std::invalid_argument("unsupported Swift source signature schema");
    const auto Types = recoverSwiftTypes(Session->Img);
    std::map<va_t, std::set<std::string>> NativeAliases;
    for (const auto &Symbol : Session->Img.Symbols) {
      if (!Symbol.IsFunc)
        continue;
      llvm::StringRef Name(Symbol.Name);
      Name.consume_front("_");
      NativeAliases[Symbol.Addr].insert(Name.str());
    }
    auto UnambiguousEntry = [&](va_t Entry) {
      auto Found = NativeAliases.find(Entry);
      return Found != NativeAliases.end() && Found->second.size() == 1;
    };
    PipelineOptions Options;
    Session->applyAnalysisOptions(Options);
    Options.MaxFunctions = MaxFunctions;
    Options.EmitDumpOutput = false;
    llvm::json::Array Rows;
    std::vector<std::optional<SwiftSourceSignature>> Signatures;
    std::map<va_t, std::vector<size_t>> Entries;
    std::set<std::pair<va_t, std::string>> Identities;
    std::vector<std::optional<SourceFunctionTypeHint>> Hints;
    swift_source::RuntimeRequests RuntimeRequests;
    for (const auto &Value : *Methods) {
      auto Method = Value.getAsObject();
      if (!Method)
        throw std::invalid_argument(
            "Swift method inventory entry is not an object");
      llvm::json::Object Row{{"status", "unrecovered"}};
      Row["entry"] = Method->getString("entry").value_or("").str();
      Row["mangled_symbol"] =
          Method->getString("mangled_symbol").value_or("").str();
      std::optional<SwiftSourceSignature> Signature;
      std::optional<SourceFunctionTypeHint> RowHint;
      std::optional<SwiftRuntimeSourceRequest> RuntimeRequest;
      try {
        if (Method->getBoolean("requires_runtime_source_proof")
                .value_or(false)) {
          RuntimeRequest =
              swift_source::runtimeRequest(*Method, Session->Img, Types);
          if (!Identities
                   .insert(
                       swift_source::runtimeIdentity(RuntimeRequest->Signature))
                   .second)
            throw std::invalid_argument("duplicate Swift method identity");
          Row["module"] = RuntimeRequest->Signature.Module;
          Row["context_kind"] = RuntimeRequest->Signature.ContextKind;
          Row["context_name"] = RuntimeRequest->Signature.ContextName;
          Row["name"] = RuntimeRequest->Signature.Name;
          Row["declaration_kind"] = "runtime";
        } else {
          const bool SelfCandidate =
              Method->getBoolean("requires_self_abi_proof").value_or(false) &&
              Method->getString("context_kind") == "struct" &&
              Method->getBoolean("is_static") == false &&
              ((Method->getString("node_kind") == "Function" &&
                Method->getString("declaration_kind").value_or("function") ==
                    "function") ||
               (Method->getString("node_kind") == "Getter" &&
                Method->getString("declaration_kind") == "getter") ||
               (Method->getString("node_kind") == "Setter" &&
                Method->getString("declaration_kind") == "setter"));
          const bool StorageCandidate =
              Method->getBoolean("requires_storage_abi_proof")
                  .value_or(false) &&
              Method->getString("context_kind") == "struct" &&
              Method->getBoolean("is_static") == false &&
              (Method->getString("node_kind") == "Allocator" ||
               Method->getString("node_kind") == "Constructor") &&
              Method->getString("declaration_kind") == "initializer";
          if (Method->getString("status") != "supported" && !SelfCandidate &&
              !StorageCandidate) {
            auto Reason = Method->getString("reason").value_or("");
            throw std::invalid_argument(Reason.empty()
                                            ? "source signature is unsupported"
                                            : Reason.str());
          }
          Signature = swift_source::signature(*Method, Session->Img, Types);
          const bool NeedsSelfProof = Signature->ContextKind == "struct" &&
                                      !Signature->IsStatic &&
                                      !Signature->ReturnsContextValue;
          auto Hint =
              swift_source::hint(*Signature, Session->Img.Arch, NeedsSelfProof);
          if (!Identities.insert(swift_source::runtimeIdentity(*Signature))
                   .second)
            throw std::invalid_argument("duplicate Swift method identity");
          Entries[Signature->Entry].push_back(Rows.size());
          if (!NeedsSelfProof)
            RowHint = std::move(Hint);
        }
      } catch (const std::exception &Error) {
        Row["reason"] = jsonSafeText(Error.what());
        Signature.reset();
        RuntimeRequest.reset();
      }
      Rows.emplace_back(std::move(Row));
      Signatures.push_back(std::move(Signature));
      Hints.push_back(std::move(RowHint));
      RuntimeRequests.push_back(std::move(RuntimeRequest));
    }
    // An address alone cannot select a declaration when compiler folding has
    // coalesced different source ABIs. Keep these bodies as separate
    // projections; callers may use only declarations with an unambiguous target
    // identity.
    for (const auto &[Entry, Indices] : Entries)
      if (Indices.size() == 1 && UnambiguousEntry(Entry) &&
          Hints[Indices.front()])
        Options.SourceTypeHints.emplace(Entry, *Hints[Indices.front()]);
    llvm::LLVMContext Context;
    Pipeline Engine;
    auto Result =
        Engine.run(Session->Img, Context, Options, Session->Dbg.get());
    if (!Result.Success)
      throw std::runtime_error(
          Result.Error.empty() ? "Swift native pipeline failed" : Result.Error);
    bool AddedSelfBindings = false;
    for (size_t Index = 0; Index < Signatures.size(); ++Index) {
      auto &Signature = Signatures[Index];
      if (!Signature || Signature->ContextKind != "struct" ||
          Signature->IsStatic || Signature->ReturnsContextValue)
        continue;
      auto Low = std::find_if(Result.LowFuncs.begin(), Result.LowFuncs.end(),
                              [&](const LowFunc &Function) {
                                return Function.Entry == Signature->Entry;
                              });
      if (Low == Result.LowFuncs.end()) {
        Signature->UnsupportedReason =
            "Swift value-type method has no decoded native body";
        continue;
      }
      auto Hint = swift_source::hint(*Signature, Session->Img.Arch, true);
      const auto Proof = recoverSwiftSelfABI(*Signature, Session->Img, *Low,
                                             Hint, Result.LowFuncs);
      if (!Proof.Proven) {
        Signature->UnsupportedReason = Proof.Reason;
        continue;
      }
      Hint.Parameters.insert(Hint.Parameters.end(),
                             Proof.SelfParameters.begin(),
                             Proof.SelfParameters.end());
      std::string Diagnostic;
      if (!validateSourceABI(Hint, Diagnostic)) {
        Signature->UnsupportedReason = Diagnostic;
        continue;
      }
      Signature->SelfConvention = Proof.SelfConvention;
      Signature->IsMutating = Proof.IsMutating;
      Signature->IsMutatingKnown = true;
      Hints[Index] = std::move(Hint);
      if (Entries.at(Signature->Entry).size() == 1 &&
          UnambiguousEntry(Signature->Entry)) {
        Options.SourceTypeHints.emplace(Signature->Entry, *Hints[Index]);
        AddedSelfBindings = true;
      }
      llvm::json::Array Evidence;
      for (const auto &Item : Proof.Evidence)
        Evidence.push_back(Item);
      (*Rows[Index].getAsObject())["self_abi_evidence"] = std::move(Evidence);
    }
    if (AddedSelfBindings) {
      Result = Engine.run(Session->Img, Context, Options, Session->Dbg.get());
      if (!Result.Success)
        throw std::runtime_error(Result.Error.empty()
                                     ? "Swift source ABI pipeline failed"
                                     : Result.Error);
    }
    // Runtime effects are proven against the complete base native inventory,
    // before releasing its IR. They never become ordinary callable ABI hints.
    swift_source::RuntimeProofs RuntimeProofs(RuntimeRequests.size());
    const auto RuntimeLinkErrors =
        swift_source::linkRuntimeRequests(RuntimeRequests, Signatures);
    {
      const auto RuntimeEvidence = swift_source::indexRuntimeNativeEvidence(
          Result.FunctionAudits, Result.LowFuncs);
      for (size_t I = 0; I < RuntimeRequests.size(); ++I) {
        if (!RuntimeRequests[I])
          continue;
        SwiftRuntimeSourceProof Proof;
        if (auto Error = RuntimeLinkErrors.find(I);
            Error != RuntimeLinkErrors.end())
          Proof.Reason = Error->second;
        else if (Result.SourceImage != &Session->Img)
          Proof.Reason =
              "Swift compiler projection has a different native image";
        else if (auto Limitation = swift_source::runtimeAuditLimitation(
                     *RuntimeRequests[I], RuntimeEvidence);
                 !Limitation.empty())
          Proof.Reason = std::move(Limitation);
        else
          Proof = recoverSwiftRuntimeSource(*RuntimeRequests[I], Session->Img,
                                            Result.LowFuncs);
        RuntimeProofs[I] = std::move(Proof);
      }
    }
    swift_source::planPropertyStorage(Signatures);
    const auto NamespaceConflicts =
        swift_source::namespaceConflicts(Signatures);
    HighSwiftEmitter Emitter;
    std::vector<SwiftSourceSignature> Callees;
    for (const auto &Signature : Signatures)
      if (Signature && Signature->UnsupportedReason.empty() &&
          Entries.at(Signature->Entry).size() == 1 &&
          Options.SourceTypeHints.count(Signature->Entry))
        Callees.push_back(*Signature);
    std::vector<std::optional<SwiftEmissionResult>> Emissions(
        Signatures.size());
    std::set<size_t> Recoverable;
    std::vector<swift_source::ABIProjectionRow> ProjectionRows;
    for (size_t Index = 0; Index < Signatures.size(); ++Index) {
      if (!Signatures[Index])
        continue;
      auto &Row = *Rows[Index].getAsObject();
      const auto &Signature = *Signatures[Index];
      Row["module"] = Signature.Module;
      Row["context_kind"] = Signature.ContextKind;
      Row["context_name"] = Signature.ContextName;
      Row["name"] = Signature.Name;
      Row["declaration_kind"] = Signature.DeclarationKind;
      std::string Reason;
      if (NamespaceConflicts.count(Index))
        Reason = "Swift declarations collide in the emitted source namespace";
      else if (!Signature.UnsupportedReason.empty())
        Reason = Signature.UnsupportedReason;
      else if (Signature.ContextKind != "global" &&
               !Signature.ContextLayoutKnown)
        Reason =
            "Swift declaration context has no complete native storage layout";
      else if (!Hints[Index])
        Reason = "Swift method has no recovered native body (possibly limited "
                 "by max-func)";
      if (!Reason.empty()) {
        Row["reason"] = std::move(Reason);
        continue;
      }
      ProjectionRows.push_back(
          {Index, Signature.Entry, Signature.MangledSymbol, *Hints[Index]});
    }
    const auto ProjectionPlan = swift_source::planABIProjections(
        ProjectionRows, Options.SourceTypeHints);
    auto CallAllowed = [&](const HighExpr &Expression) {
      if (!Expression.SourceCallHint ||
          Expression.IntrinsicId != Intrinsic::None ||
          Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Expression.MemoryOrdering != NdMemoryOrdering::None ||
          Expression.SourceCallHint->CallKind !=
              SourceCallTypeHint::Kind::Native)
        return false;
      const auto &Binding = *Expression.SourceCallHint;
      // Batch body overrides cannot resolve which source declaration a call
      // to a coalesced native address meant. Only the original unambiguous
      // callee inventory may supply that identity.
      auto Found = ProjectionPlan.BaseCalleeHints.find(Binding.TargetAddress);
      return Found != ProjectionPlan.BaseCalleeHints.end() &&
             Expression.Operands.size() ==
                 Binding.Signature.Parameters.size() &&
             objc_projection_detail::sameHint(Binding.Signature, Found->second);
    };
    auto EmitRows = [&](const std::vector<size_t> &Indices,
                        const PipelineResult &Projection) {
      std::map<va_t, const HighFunc *> Functions;
      std::map<va_t, const PipelineFunctionAudit *> Audits;
      for (const auto &Function : Projection.HighFuncs)
        if (!Function.Name.empty())
          Functions.emplace(Function.Entry, &Function);
      for (const auto &Audit : Projection.FunctionAudits)
        Audits.emplace(Audit.Entry, &Audit);
      for (size_t Index : Indices) {
        auto &Row = *Rows[Index].getAsObject();
        const auto &Signature = *Signatures[Index];
        const auto FoundFunction = Functions.find(Signature.Entry);
        const HighFunc *Function =
            FoundFunction == Functions.end() ? nullptr : FoundFunction->second;
        const auto FoundAudit = Audits.find(Signature.Entry);
        const PipelineFunctionAudit *Audit =
            FoundAudit == Audits.end() ? nullptr : FoundAudit->second;
        std::string Reason;
        if (!Projection.Success)
          Reason = Projection.Error.empty()
                       ? "Swift alias ABI projection failed"
                       : Projection.Error;
        else if (!Function)
          Reason = "Swift source projection has no recovered native body "
                   "(possibly limited by max-func)";
        else
          Reason = sourceBodyLimitation(*Function, *Hints[Index], Audit,
                                        CallAllowed);
        if (!Reason.empty()) {
          Row["reason"] = std::move(Reason);
          continue;
        }
        auto Emission = Emitter.emit(*Function, Signature, Callees);
        if (!Emission.Recovered || Emission.Source.empty() ||
            (Signature.ContextKind != "global" &&
             Emission.MemberSource.empty())) {
          Row["reason"] = Emission.Reason.empty()
                              ? "Swift source declaration is incomplete"
                              : Emission.Reason;
          continue;
        }
        Recoverable.insert(Index);
        Emissions[Index] = std::move(Emission);
      }
    };
    EmitRows(ProjectionPlan.BaseRows, Result);
    // Nothing below needs the initial IR. Keep only one projection result in
    // memory while publishing independent rows from a shared ABI body.
    Result = PipelineResult();
    for (const auto &Batch : ProjectionPlan.Batches) {
      auto ProjectionOptions = Options;
      for (const auto &[Entry, Hint] : Batch.BodyHints)
        ProjectionOptions.SourceTypeHints[Entry] = Hint;
      const auto Projection = Engine.run(Session->Img, Context,
                                         ProjectionOptions, Session->Dbg.get());
      EmitRows(Batch.Rows, Projection);
    }
    // Close the dependency graph before reporting a body as independently
    // recovered. A callee or designated initializer that failed invalidates
    // its callers; recursion within a complete component remains supported.
    bool Changed;
    do {
      Changed = false;
      for (size_t Index = 0; Index < Signatures.size(); ++Index) {
        if (!Emissions[Index] || !Recoverable.count(Index))
          continue;
        const auto &Signature = *Signatures[Index];
        std::string Reason;
        for (va_t Dependency : Emissions[Index]->Dependencies) {
          const auto Found = Entries.find(Dependency);
          const bool Bound = Dependency == Signature.Entry
                                 ? Recoverable.count(Index)
                                 : Found != Entries.end() &&
                                       Found->second.size() == 1 &&
                                       Recoverable.count(Found->second.front());
          if (!Bound) {
            Reason =
                "Swift source depends on a method whose body was not recovered";
            break;
          }
        }
        if (Reason.empty() && Signature.ContextKind == "class" &&
            !Signature.ContextFields.empty()) {
          bool HasInitializer = false;
          for (size_t OtherIndex = 0; OtherIndex < Signatures.size();
               ++OtherIndex) {
            const auto &Other = Signatures[OtherIndex];
            if (Other && Other->Module == Signature.Module &&
                Other->ContextKind == Signature.ContextKind &&
                Other->ContextName == Signature.ContextName &&
                Other->DeclarationKind == "initializer" &&
                Recoverable.count(OtherIndex))
              HasInitializer = true;
          }
          if (!HasInitializer)
            Reason = "Swift stored properties have no recovered designated "
                     "initializer";
        }
        if (!Reason.empty()) {
          Rows[Index].getAsObject()->operator[]("reason") = std::move(Reason);
          Recoverable.erase(Index);
          Changed = true;
        }
      }
      for (const auto &[Index, Reason] :
           swift_source::incompleteProperties(Signatures, Recoverable)) {
        (*Rows[Index].getAsObject())["reason"] = Reason;
        Recoverable.erase(Index);
        Changed = true;
      }
    } while (Changed);

    const auto RuntimePlan = swift_source::planRuntimeProjections(
        RuntimeRequests, RuntimeProofs, Signatures, Recoverable);
    for (const auto &[Index, Reason] : RuntimePlan.Reasons)
      (*Rows[Index].getAsObject())["reason"] = Reason;

    auto TypeName = swift_source::typeSpelling;
    auto AddressText = [](va_t Address) {
      return "0x" + llvm::utohexstr(Address, true);
    };
    llvm::json::Array Units;
    std::string Source;
    auto AddUnit = [&](llvm::StringRef Kind, llvm::StringRef Module,
                       llvm::StringRef Name, std::string Text,
                       const std::vector<size_t> &Indices) {
      llvm::json::Array UnitEntries, UnitIdentities;
      for (size_t Index : Indices) {
        const auto &Signature = Signatures[Index]
                                    ? *Signatures[Index]
                                    : RuntimeRequests[Index]->Signature;
        auto Entry = AddressText(Signature.Entry);
        UnitEntries.push_back(Entry);
        UnitIdentities.push_back(
            llvm::json::Object{{"entry", std::move(Entry)},
                               {"mangled_symbol", Signature.MangledSymbol}});
      }
      Source += Text + "\n";
      Units.push_back(
          llvm::json::Object{{"kind", Kind.str()},
                             {"module", Module.str()},
                             {"name", Name.str()},
                             {"source", std::move(Text)},
                             {"method_entries", std::move(UnitEntries)},
                             {"method_identities", std::move(UnitIdentities)}});
    };
    using ContextKey = std::tuple<std::string, std::string, std::string>;
    std::map<ContextKey, std::vector<size_t>> Members;
    size_t Recovered = 0;
    for (size_t Index = 0; Index < Signatures.size(); ++Index) {
      if (!Emissions[Index] || !Recoverable.count(Index))
        continue;
      auto &Row = *Rows[Index].getAsObject();
      const auto &Signature = *Signatures[Index];
      const auto &Emission = *Emissions[Index];
      Row["status"] = "recovered";
      Row["source_representation"] = "native-method-body";
      Row["source"] = Emission.Source;
      Row["member_source"] = Emission.MemberSource;
      if (Signature.ContextKind == "global")
        AddUnit("function", Signature.Module, Signature.Name, Emission.Source,
                {Index});
      else
        Members[{Signature.Module, Signature.ContextKind,
                 Signature.ContextName}]
            .push_back(Index);
      ++Recovered;
    }
    const size_t SourceBodyMethodCount = Recovered;
    const size_t CompilerProjectionMethodCount = RuntimePlan.Recovered.size();
    for (size_t Index : RuntimePlan.Recovered) {
      const auto &Signature = RuntimeRequests[Index]->Signature;
      const auto &Proof = *RuntimeProofs[Index];
      auto &Row = *Rows[Index].getAsObject();
      Row["status"] = "recovered";
      Row["source_representation"] = "compiler-generated-from-type";
      Row["compiler_projection_kind"] = Proof.ProjectionKind;
      llvm::json::Array Evidence;
      for (const auto &Item : Proof.Evidence)
        Evidence.push_back(Item);
      Row["compiler_projection_evidence"] = std::move(Evidence);
      Members[{Signature.Module, Signature.ContextKind, Signature.ContextName}]
          .push_back(Index);
      ++Recovered;
    }
    if (SourceBodyMethodCount + CompilerProjectionMethodCount != Recovered)
      throw std::logic_error(
          "Swift source recovery representation counts disagree");
    for (const auto &[Context, Indices] : Members) {
      const auto &[Module, Kind, Name] = Context;
      auto Ordinary =
          std::find_if(Indices.begin(), Indices.end(),
                       [&](size_t I) { return bool(Signatures[I]); });
      if (Ordinary == Indices.end())
        throw std::logic_error(
            "Swift compiler projection has no emitted source context");
      const auto &Signature = *Signatures[*Ordinary];
      std::vector<swift_source::PropertySourceMember> SourceMembers;
      for (size_t Index : Indices)
        if (Signatures[Index])
          SourceMembers.push_back(
              {&*Signatures[Index], Emissions[Index]->MemberSource});
      const auto Runtime = RuntimePlan.ContextSources.find(Context);
      auto Text = swift_source::assemblePropertyContext(
          Signature, SourceMembers,
          Runtime == RuntimePlan.ContextSources.end()
              ? swift_source::PropertyRuntimeSource()
              : Runtime->second);
      for (size_t Index : Indices)
        if (RuntimeRequests[Index])
          (*Rows[Index].getAsObject())["source"] = Text;
      AddUnit("type", Module, Name, std::move(Text), Indices);
    }
    llvm::json::Array TypeRows;
    for (const auto &Type : Types) {
      llvm::json::Array Fields;
      for (const auto &Field : Type.Fields)
        Fields.push_back(
            llvm::json::Object{{"name", Field.Name},
                               {"type", TypeName(Field.Type)},
                               {"offset", static_cast<int64_t>(Field.Offset)},
                               {"mutable", Field.IsMutable}});
      TypeRows.push_back(llvm::json::Object{
          {"module", Type.Module},
          {"name", Type.Name},
          {"kind", Type.Kind},
          {"status", Type.Status},
          {"reason", Type.Reason},
          {"size", static_cast<int64_t>(Type.Size)},
          {"alignment", static_cast<int64_t>(Type.Alignment)},
          {"fields", std::move(Fields)}});
    }
    const size_t Count = Rows.size();
    llvm::json::Object Report{
        {"schema_version", 1},
        {"status", "success"},
        {"source", std::move(Source)},
        {"source_units", std::move(Units)},
        {"types", std::move(TypeRows)},
        {"method_count", static_cast<int64_t>(Count)},
        {"recovered_method_count", static_cast<int64_t>(Recovered)},
        {"source_body_method_count",
         static_cast<int64_t>(SourceBodyMethodCount)},
        {"compiler_projection_method_count",
         static_cast<int64_t>(CompilerProjectionMethodCount)},
        {"unrecovered_method_count", static_cast<int64_t>(Count - Recovered)},
        {"coverage_status", Count == 0           ? "no-methods"
                            : Recovered == 0     ? "unrecovered"
                            : Recovered == Count ? "recovered"
                                                 : "partial"},
        {"methods", std::move(Rows)},
        {"limitations",
         llvm::json::Array{
             "Swift source signatures are projection hints, not authenticated "
             "ABI or semantic-equivalence certificates.",
             "Compiler-entry rows represent native-proven effects regenerated "
             "from the emitted type, initializer, property, or deinitializer; "
             "their original helper bodies and ABI are not source methods.",
             "Source coverage describes the supplied callable inventory; "
             "stripped or unclassified symbols can conceal additional methods.",
             "Only proven fixed storage layouts and complete source dependency "
             "groups are published; generic, resilient, async, throwing, or "
             "unknown ABI paths remain individually unrecovered."}}};
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    Stream << llvm::json::Value(std::move(Report));
    auto Output = dupStr(Text);
    if (!Output)
      Session->setError("unable to allocate Swift source report");
    return Output;
  } catch (const std::exception &Error) {
    Session->setError(std::string("Swift source export failed: ") +
                      Error.what());
  } catch (...) {
    Session->setError("Swift source export failed with an unknown exception");
  }
  return nullptr;
}
