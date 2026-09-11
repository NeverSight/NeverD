#ifndef NEVERD_SDK_CAPI_SWIFTRUNTIMEPROJECTION_H
#define NEVERD_SDK_CAPI_SWIFTRUNTIMEPROJECTION_H

#include "ObjCSourceProjection.h"
#include "SwiftSourceIdentity.h"
#include "SwiftSourceNamespace.h"
#include "SwiftSourceProperties.h"
#include "SwiftSourceSignatures.h"

#include "neverd/loader/Swift/SwiftRuntimeSource.h"
#include "neverd/pipeline/Pipeline.h"

namespace neverd::sdk::swift_source {
using RuntimeRequests = std::vector<std::optional<SwiftRuntimeSourceRequest>>;
using RuntimeProofs = std::vector<std::optional<SwiftRuntimeSourceProof>>;
using EmptyStructSourceContexts = std::map<PropertyContext, SwiftRecoveredType>;
using RuntimeIdentity = std::pair<va_t, std::string>;
inline RuntimeIdentity runtimeIdentity(const SwiftSourceSignature &S) {
  return sourceIdentity(S.Entry, S.MangledSymbol);
}
inline RuntimeIdentity runtimeIdentity(const SwiftRuntimeSourceIdentity &S) {
  return sourceIdentity(S.Entry, S.MangledSymbol);
}
inline std::string runtimeSymbol(std::string S) {
  return normalizedSourceSymbol(std::move(S));
}
inline std::string runtimeProjectionKind(SwiftRuntimeSourceKind K) {
  switch (K) {
  case SwiftRuntimeSourceKind::AllocatingInitializer:
    return "allocating_initializer";
  case SwiftRuntimeSourceKind::TrivialDestructor:
    return "trivial_destructor";
  case SwiftRuntimeSourceKind::DeallocatingDestructor:
    return "deallocating_destructor";
  case SwiftRuntimeSourceKind::TypeMetadataAccessor:
    return "type_metadata_accessor";
  case SwiftRuntimeSourceKind::ModifyAccessor:
    return "modify_accessor";
  case SwiftRuntimeSourceKind::ModifyResume:
    return "modify_resume";
  }
  return {};
}

inline PropertyContext nominalContext(const SwiftRecoveredType &T) {
  return {T.Module, T.Kind, T.Name};
}

inline bool isEmptyStructSourceContext(const SwiftRecoveredType &T) {
  return T.Status == "recovered" && T.Reason.empty() && T.Kind == "struct" &&
         identifier(T.Module) && identifier(T.Name) && T.Descriptor &&
         T.Metadata && T.Size == 0 && T.Alignment == 1 && T.Fields.empty();
}

/// This is a source dependency, never an ordinary method or callable ABI hint.
/// The existing native proof owns the exact accessor identity and effects.
inline bool bindsEmptyStructSourceContext(const SwiftRuntimeSourceRequest &R,
                                          const SwiftRuntimeSourceProof &P,
                                          const SwiftRecoveredType &T) {
  return R.Kind == SwiftRuntimeSourceKind::TypeMetadataAccessor && P.Proven &&
         P.ProjectionKind == "type_metadata_accessor" &&
         isEmptyStructSourceContext(T) &&
         propertyContext(R.Signature) == nominalContext(T) &&
         P.Descriptor == T.Descriptor && P.Metadata == T.Metadata;
}

inline EmptyStructSourceContexts
collectEmptyStructSourceContexts(const RuntimeRequests &Requests,
                                 const RuntimeProofs &Proofs,
                                 const std::vector<SwiftRecoveredType> &Types) {
  if (Requests.size() != Proofs.size())
    throw std::invalid_argument(
        "Swift nominal source inventory dimensions disagree");
  std::map<PropertyContext, const SwiftRecoveredType *> NativeTypes;
  for (const auto &T : Types) {
    auto [It, Unique] = NativeTypes.emplace(nominalContext(T), &T);
    if (!Unique)
      It->second = nullptr;
  }
  EmptyStructSourceContexts Contexts;
  for (size_t I = 0; I < Requests.size(); ++I) {
    if (!Requests[I] || !Proofs[I])
      continue;
    const auto Context = propertyContext(Requests[I]->Signature);
    const auto Found = NativeTypes.find(Context);
    if (Found != NativeTypes.end() && Found->second &&
        bindsEmptyStructSourceContext(*Requests[I], *Proofs[I], *Found->second))
      Contexts.emplace(Context, *Found->second);
  }
  return Contexts;
}

inline std::string assembleEmptyStructContext(const SwiftRecoveredType &T) {
  if (!isEmptyStructSourceContext(T) || T.Name == "Swift")
    throw std::invalid_argument("invalid Swift empty nominal source context");
  return "struct `" + T.Name + "` {\n}\n";
}

/// Resolve identities from the supplied native inventory. The native proof
/// still checks their exact compiler role and all parameter/effect details.
inline std::map<size_t, std::string> linkRuntimeRequests(
    RuntimeRequests &Requests,
    const std::vector<std::optional<SwiftSourceSignature>> &Signatures) {
  using Kind = SwiftRuntimeSourceKind;
  using Key = std::tuple<PropertyContext, std::string, std::string>;
  std::map<Key, const SwiftSourceSignature *> Index;
  auto Insert = [&](const SwiftSourceSignature &S, std::string Role) {
    auto [It, Unique] =
        Index.emplace(Key{propertyContext(S), runtimeSymbol(S.MangledSymbol),
                          std::move(Role)},
                      &S);
    if (!Unique)
      It->second = nullptr; // Ambiguity is sticky, including a third match.
  };
  for (const auto &S : Signatures)
    if (S)
      Insert(*S, S->DeclarationKind);
  for (const auto &R : Requests)
    if (R)
      Insert(R->Signature, runtimeProjectionKind(R->Kind));
  auto Lookup = [&](const SwiftSourceSignature &S, const std::string &Symbol,
                    const std::string &Role) {
    auto It = Index.find({propertyContext(S), Symbol, Role});
    return It == Index.end() ? nullptr : It->second;
  };
  std::map<size_t, std::string> Errors;
  for (size_t I = 0; I < Requests.size(); ++I) {
    if (!Requests[I])
      continue;
    auto &R = *Requests[I];
    R.Initializer.reset();
    R.RelatedEntry.reset();
    const auto Symbol = runtimeSymbol(R.Signature.MangledSymbol);
    if (R.Kind == Kind::AllocatingInitializer) {
      const auto Expected = Symbol.ends_with("cfC")
                                ? Symbol.substr(0, Symbol.size() - 1) + "c"
                                : std::string();
      const auto *Match = Expected.empty()
                              ? nullptr
                              : Lookup(R.Signature, Expected, "initializer");
      if (!Match)
        Errors[I] = "Swift allocator has no unique actual "
                    "initializing-constructor identity";
      else
        R.Initializer = *Match;
      continue;
    }
    std::optional<Kind> Related;
    std::string Expected;
    if (R.Kind == Kind::DeallocatingDestructor) {
      Related = Kind::TrivialDestructor;
      if (Symbol.ends_with("fD"))
        Expected = Symbol.substr(0, Symbol.size() - 1) + "d";
    } else if (R.Kind == Kind::ModifyAccessor) {
      Related = Kind::ModifyResume;
      Expected = Symbol + ".resume.0";
    } else if (R.Kind == Kind::ModifyResume) {
      Related = Kind::ModifyAccessor;
      if (Symbol.ends_with(".resume.0"))
        Expected = Symbol.substr(0, Symbol.size() - 9);
    }
    if (!Related)
      continue;
    const auto *Match =
        Expected.empty()
            ? nullptr
            : Lookup(R.Signature, Expected, runtimeProjectionKind(*Related));
    if (!Match ||
        ((R.Kind == Kind::ModifyAccessor || R.Kind == Kind::ModifyResume) &&
         Match->Name != R.Signature.Name))
      Errors[I] =
          "Swift compiler projection has no unique related native identity";
    else
      R.RelatedEntry =
          SwiftRuntimeSourceIdentity{Match->Entry, Match->MangledSymbol};
  }
  return Errors;
}

struct RuntimeNativeEvidence {
  const PipelineFunctionAudit *Audit = nullptr;
  const LowFunc *Function = nullptr;
  bool AmbiguousAudit = false, AmbiguousFunction = false;
};
using RuntimeNativeEvidenceIndex = std::map<va_t, RuntimeNativeEvidence>;
/// References remain valid only while this pipeline result is alive.
inline RuntimeNativeEvidenceIndex
indexRuntimeNativeEvidence(const std::vector<PipelineFunctionAudit> &Audits,
                           const std::vector<LowFunc> &Functions) {
  RuntimeNativeEvidenceIndex Index;
  for (const auto &A : Audits) {
    auto &E = Index[A.Entry];
    if (E.Audit)
      E.AmbiguousAudit = true;
    else
      E.Audit = &A;
  }
  for (const auto &F : Functions) {
    auto &E = Index[F.Entry];
    if (E.Function)
      E.AmbiguousFunction = true;
    else
      E.Function = &F;
  }
  return Index;
}
inline std::string
runtimeAuditLimitation(const SwiftRuntimeSourceRequest &R,
                       const RuntimeNativeEvidenceIndex &Index) {
  std::set<va_t> Entries{R.Signature.Entry};
  if (R.Initializer)
    Entries.insert(R.Initializer->Entry);
  if (R.RelatedEntry)
    Entries.insert(R.RelatedEntry->Entry);
  for (va_t Entry : Entries) {
    auto It = Index.find(Entry);
    if (It == Index.end() || !It->second.Function)
      return "Swift runtime native body is missing";
    const auto &Evidence = It->second;
    if (Evidence.AmbiguousAudit || Evidence.AmbiguousFunction)
      return "Swift runtime native evidence is ambiguous";
    const auto *Found = Evidence.Audit;
    if (!Found || Found->Disposition != PipelineFunctionDisposition::Accepted ||
        !Found->HasLowIR || !Found->HasMedIR || !Found->MedIRVerified ||
        !Found->DecodedInstructions ||
        Found->DecodedInstructions != Found->LiftedInstructions ||
        !Found->DecodeFailures.empty() ||
        !Found->UnsupportedInstructions.empty() ||
        !Found->TruncatedPaths.empty())
      return "Swift runtime native decoding, lifting, or IR verification is "
             "incomplete";
    const auto &EH = Evidence.Function->ExceptionMetadata;
    if (EH && !objc_projection_detail::isPlainUnwind(*EH))
      return "Swift runtime native exception metadata is unsupported or "
             "incomplete";
  }
  return {};
}

struct RuntimeProjectionPlan {
  std::set<size_t> Recovered;
  std::map<size_t, std::string> Reasons;
  std::map<PropertyContext, PropertyRuntimeSource> ContextSources;
  EmptyStructSourceContexts NominalContexts;
};

/// A native proof is not itself a recovered source body. Root this graph in
/// actual emitted declarations; only the canonical modify/resume pair may
/// satisfy a cycle, after both proofs and the concrete property are present.
inline RuntimeProjectionPlan planRuntimeProjections(
    const RuntimeRequests &Requests, const RuntimeProofs &Proofs,
    const std::vector<std::optional<SwiftSourceSignature>> &Signatures,
    const std::set<size_t> &OrdinaryRecovered,
    const EmptyStructSourceContexts &Nominals = {}) {
  using Kind = SwiftRuntimeSourceKind;
  if (Requests.size() != Proofs.size() || Requests.size() != Signatures.size())
    throw std::invalid_argument(
        "Swift compiler projection inventory dimensions disagree");
  RuntimeProjectionPlan Plan;
  std::map<RuntimeIdentity, size_t> RuntimeRows;
  std::map<PropertyContext, const SwiftSourceSignature *> Contexts;
  std::map<RuntimeIdentity, const SwiftSourceSignature *> Methods;
  std::map<PropertyIdentity, const SwiftSourceSignature *> Getters;
  for (size_t I : OrdinaryRecovered) {
    if (I >= Signatures.size() || !Signatures[I])
      throw std::invalid_argument(
          "Swift source recovery has no declaration identity");
    const auto &S = *Signatures[I];
    if (!Methods.emplace(runtimeIdentity(S), &S).second)
      throw std::invalid_argument(
          "Swift emitted method identity is duplicated");
    if (S.ContextKind != "global")
      Contexts.emplace(propertyContext(S), &S);
    if (S.DeclarationKind == "getter")
      Getters.emplace(propertyIdentity(S), &S);
  }
  std::set<size_t> Candidates;
  for (size_t I = 0; I < Requests.size(); ++I) {
    if (!Requests[I])
      continue;
    if (Methods.count(runtimeIdentity(Requests[I]->Signature)) ||
        !RuntimeRows.emplace(runtimeIdentity(Requests[I]->Signature), I).second)
      throw std::invalid_argument(
          "Swift compiler projection identity is duplicated");
    if (!Proofs[I] || !Proofs[I]->Proven)
      Plan.Reasons[I] =
          Proofs[I] && !Proofs[I]->Reason.empty()
              ? Proofs[I]->Reason
              : "Swift compiler entry has no complete native projection proof";
    else if (Proofs[I]->ProjectionKind !=
             runtimeProjectionKind(Requests[I]->Kind))
      Plan.Reasons[I] = "Swift compiler projection proof has a different role";
    else
      Candidates.insert(I);
  }
  auto Nominal = [&](size_t I) -> const SwiftRecoveredType * {
    const auto Found = Nominals.find(propertyContext(Requests[I]->Signature));
    return Found != Nominals.end() && Proofs[I] &&
                   bindsEmptyStructSourceContext(*Requests[I], *Proofs[I],
                                                 Found->second)
               ? &Found->second
               : nullptr;
  };
  auto Dependency = [&](size_t I, const SwiftRuntimeSourceDependency &D,
                        const std::set<size_t> &Group) {
    const auto &R = *Requests[I];
    const PropertyContext Context{D.Module, D.ContextKind, D.ContextName};
    if (Context != propertyContext(R.Signature))
      return false;
    if (D.Kind == "context")
      return Contexts.count(Context) != 0 || Nominal(I);
    if (D.Kind == "method") {
      auto It = Methods.find(runtimeIdentity(D.Identity));
      return It != Methods.end() && propertyContext(*It->second) == Context &&
             It->second->Name == D.Name;
    }
    if (D.Kind == "compiler_entry") {
      auto It = RuntimeRows.find(runtimeIdentity(D.Identity));
      return It != RuntimeRows.end() &&
             propertyContext(Requests[It->second]->Signature) == Context &&
             (Plan.Recovered.count(It->second) || Group.count(It->second));
    }
    if (D.Kind == "property") {
      auto It = Getters.find({Context, false, D.Name});
      if (It == Getters.end())
        return false;
      const auto &S = *It->second;
      for (const auto &F : S.ContextFields)
        if (F.Name == D.Name && F.Offset == Proofs[I]->FieldOffset &&
            F.IsMutable && !F.BackingName.empty() && propertyType(S) &&
            typeSpelling(F.Type) == typeSpelling(*propertyType(S)))
          return true;
    }
    return false;
  };
  auto Ready = [&](size_t I, const std::set<size_t> &Group) {
    if (!Contexts.count(propertyContext(Requests[I]->Signature)) && !Nominal(I))
      return false;
    std::map<std::string, unsigned> Counts;
    for (const auto &D : Proofs[I]->Dependencies) {
      if (!Dependency(I, D, Group))
        return false;
      ++Counts[D.Kind];
    }
    if (Counts["context"] != 1)
      return false;
    const auto &R = *Requests[I];
    if (R.Kind == Kind::AllocatingInitializer) {
      if (!R.Initializer || Counts["method"] != 1)
        return false;
      for (const auto &D : Proofs[I]->Dependencies)
        if (D.Kind == "method" &&
            runtimeIdentity(D.Identity) != runtimeIdentity(*R.Initializer))
          return false;
    }
    if (R.Kind == Kind::DeallocatingDestructor) {
      if (!R.RelatedEntry || Counts["compiler_entry"] != 1)
        return false;
      for (const auto &D : Proofs[I]->Dependencies)
        if (D.Kind == "compiler_entry" &&
            runtimeIdentity(D.Identity) != runtimeIdentity(*R.RelatedEntry))
          return false;
    }
    if ((R.Kind == Kind::ModifyAccessor || R.Kind == Kind::ModifyResume) &&
        (Counts["property"] != 1 || Counts["compiler_entry"] != 1))
      return false;
    return true;
  };
  auto Pair = [&](size_t I) -> std::optional<size_t> {
    const auto &R = *Requests[I];
    if (R.Kind != Kind::ModifyAccessor || !R.RelatedEntry)
      return std::nullopt;
    auto Other = RuntimeRows.find(runtimeIdentity(*R.RelatedEntry));
    if (Other == RuntimeRows.end() || !Candidates.count(Other->second))
      return std::nullopt;
    const size_t J = Other->second;
    const auto &Resume = *Requests[J];
    const auto &A = *Proofs[I], &B = *Proofs[J];
    if (Resume.Kind != Kind::ModifyResume || !Resume.RelatedEntry ||
        runtimeIdentity(*Resume.RelatedEntry) != runtimeIdentity(R.Signature) ||
        propertyIdentity(R.Signature) != propertyIdentity(Resume.Signature) ||
        A.FieldOffset != B.FieldOffset ||
        runtimeIdentity(A.Continuation) != runtimeIdentity(Resume.Signature) ||
        runtimeIdentity(B.Continuation) != runtimeIdentity(Resume.Signature))
      return std::nullopt;
    // Exactly one related-entry edge per side; arbitrary cycles are not proof.
    for (size_t Side : {I, J}) {
      size_t Count = 0;
      for (const auto &D : Proofs[Side]->Dependencies)
        if (D.Kind == "compiler_entry") {
          if (runtimeIdentity(D.Identity) !=
              runtimeIdentity(Requests[Side == I ? J : I]->Signature))
            return std::nullopt;
          ++Count;
        }
      if (Count != 1)
        return std::nullopt;
    }
    return J;
  };
  bool Changed;
  do {
    Changed = false;
    for (size_t I : Candidates) {
      if (Plan.Recovered.count(I))
        continue;
      if (Requests[I]->Kind == Kind::ModifyAccessor) {
        auto J = Pair(I);
        if (J && Ready(I, {I, *J}) && Ready(*J, {I, *J})) {
          Plan.Recovered.insert(I);
          Plan.Recovered.insert(*J);
          Changed = true;
        }
      } else if (Requests[I]->Kind != Kind::ModifyResume && Ready(I, {})) {
        Plan.Recovered.insert(I);
        Changed = true;
      }
    }
  } while (Changed);
  for (size_t I : Candidates) {
    if (!Plan.Recovered.count(I)) {
      Plan.Reasons[I] = "Swift compiler projection depends on source or "
                        "related native entries that were not recovered";
      continue;
    }
    if (!Contexts.count(propertyContext(Requests[I]->Signature)))
      if (const auto *T = Nominal(I))
        Plan.NominalContexts.emplace(nominalContext(*T), *T);
    auto &Source = Plan.ContextSources[propertyContext(Requests[I]->Signature)];
    if (Requests[I]->Kind == Kind::TrivialDestructor)
      Source.TrivialDestructor = true;
    if (Requests[I]->Kind == Kind::ModifyAccessor) {
      if (!Source.Modifiers
               .emplace(Requests[I]->Signature.Name, Proofs[I]->FieldOffset)
               .second)
        throw std::invalid_argument(
            "Swift property has duplicate compiler modifiers");
    }
  }
  return Plan;
}
} // namespace neverd::sdk::swift_source
#endif
