#include "../../../lib/sdk/capi/SwiftRuntimeProjection.h"
#include "gtest/gtest.h"

using namespace neverd;
using namespace neverd::sdk::swift_source;
namespace {
using Kind = SwiftRuntimeSourceKind;
SwiftSourceSignature signature(va_t Entry, std::string Symbol,
                               std::string Name = "value") {
  SwiftSourceSignature S;
  S.Entry = Entry;
  S.MangledSymbol = std::move(Symbol);
  S.Module = "Demo";
  S.ContextKind = "struct";
  S.ContextName = "Box";
  S.Name = std::move(Name);
  S.ContextLayoutKnown = true;
  S.ReturnType = {SwiftSourceType::Kind::Integer, "Int64", 64, true, nullptr};
  S.ContextFields = {{"value", S.ReturnType, 0, true, "neverd_storage_0"}};
  return S;
}
SwiftRuntimeSourceDependency dependency(std::string Kind,
                                        const SwiftSourceSignature &S,
                                        std::string Name = {}) {
  return {std::move(Kind), S.Module,        S.ContextKind,
          S.ContextName,   std::move(Name), {S.Entry, S.MangledSymbol}};
}
struct Fixture {
  std::vector<std::optional<SwiftSourceSignature>> Signatures{3};
  RuntimeRequests Requests{3};
  RuntimeProofs Proofs{3};
  Fixture() {
    Signatures[0] = signature(0x1000, "$s4Demo3BoxV5values5Int64Vvg");
    Signatures[0]->DeclarationKind = "getter";
    Requests[1] = SwiftRuntimeSourceRequest{
        Kind::ModifyAccessor,
        signature(0x1010, "$s4Demo3BoxV5values5Int64VvM"),
        {},
        {}};
    Requests[2] = SwiftRuntimeSourceRequest{
        Kind::ModifyResume,
        signature(0x1020, "$s4Demo3BoxV5values5Int64VvM.resume.0"),
        {},
        {}};
    EXPECT_TRUE(linkRuntimeRequests(Requests, Signatures).empty());
    for (size_t I : {1U, 2U}) {
      const auto &S = Requests[I]->Signature;
      SwiftRuntimeSourceProof P;
      P.Proven = true;
      P.ProjectionKind = runtimeProjectionKind(Requests[I]->Kind);
      P.Continuation = {Requests[2]->Signature.Entry,
                        Requests[2]->Signature.MangledSymbol};
      P.Dependencies = {
          dependency("context", S), dependency("property", S, "value"),
          dependency("compiler_entry", Requests[I == 1 ? 2 : 1]->Signature,
                     "value")};
      Proofs[I] = std::move(P);
    }
  }
  RuntimeProjectionPlan plan(std::set<size_t> Recovered = {0}) const {
    return planRuntimeProjections(Requests, Proofs, Signatures, Recovered);
  }
};
PipelineFunctionAudit audit(va_t Entry) {
  PipelineFunctionAudit A;
  A.Entry = Entry;
  A.Disposition = PipelineFunctionDisposition::Accepted;
  A.HasLowIR = A.HasMedIR = A.MedIRVerified = true;
  A.DecodedInstructions = A.LiftedInstructions = 4;
  return A;
}
} // namespace

TEST(SwiftRuntimeProjection,
     CanonicalModifyPairNeedsActualGetterAndBothNativeProofs) {
  Fixture F;
  auto P = F.plan();
  EXPECT_EQ(P.Recovered, (std::set<size_t>{1, 2}));
  ASSERT_EQ(P.ContextSources.size(), 1U);
  EXPECT_EQ(P.ContextSources.begin()->second.Modifiers,
            (std::map<std::string, uint64_t>{{"value", 0}}));
  EXPECT_TRUE(F.plan({}).Recovered.empty());
  F.Proofs[2]->Proven = false;
  EXPECT_TRUE(F.plan().Recovered.empty());
  F.Proofs[2]->Proven = true;
  F.Signatures[0]->DeclarationKind = "function";
  EXPECT_TRUE(F.plan().Recovered.empty());
}

TEST(SwiftRuntimeProjection, RelatedCyclesDoNotManufactureSourceOrIdentity) {
  for (int Mutation = 0; Mutation < 7; ++Mutation) {
    Fixture F;
    switch (Mutation) {
    case 0:
      F.Proofs[1]->Dependencies.back().Identity = F.Proofs[1]->Continuation;
      F.Requests[2]->RelatedEntry = F.Proofs[1]->Continuation;
      break;
    case 1:
      F.Proofs[2]->Continuation.Entry = 0x9999;
      break;
    case 2:
      F.Proofs[1]->FieldOffset = 8;
      break;
    case 3:
      F.Proofs[2]->Dependencies[0].Module = "Other";
      break;
    case 4:
      F.Proofs[1]->Dependencies.erase(F.Proofs[1]->Dependencies.begin() + 1);
      break;
    case 5:
      F.Proofs[2]->Dependencies.back().Kind = "unknown";
      break;
    case 6:
      F.Proofs[2]->ProjectionKind = "type_metadata_accessor";
      break;
    }
    EXPECT_TRUE(F.plan().Recovered.empty()) << Mutation;
  }
}

TEST(SwiftRuntimeProjection,
     MetadataCannotSubstituteForMutablePrivatePropertyStorage) {
  for (int Mutation = 0; Mutation < 4; ++Mutation) {
    Fixture F;
    auto &Field = F.Signatures[0]->ContextFields[0];
    if (Mutation == 0)
      Field.BackingName.clear();
    if (Mutation == 1)
      Field.IsMutable = false;
    if (Mutation == 2)
      Field.Offset = 16;
    if (Mutation == 3)
      Field.Type = {SwiftSourceType::Kind::Floating, "Double", 64, false,
                    nullptr};
    EXPECT_TRUE(F.plan().Recovered.empty()) << Mutation;
  }
}

TEST(SwiftRuntimeProjection,
     NativeAuditsCoverEveryReferencedBodyAndMustBeComplete) {
  Fixture F;
  auto &R = *F.Requests[1];
  std::vector<PipelineFunctionAudit> Audits{audit(R.Signature.Entry),
                                            audit(R.RelatedEntry->Entry)};
  std::vector<LowFunc> Functions(3);
  Functions[0].Entry = R.Signature.Entry;
  Functions[1].Entry = R.RelatedEntry->Entry;
  Functions[2].Entry = F.Signatures[0]->Entry;
  auto Check = [&](const std::vector<PipelineFunctionAudit> &A) {
    return runtimeAuditLimitation(R, indexRuntimeNativeEvidence(A, Functions));
  };
  EXPECT_TRUE(Check(Audits).empty());
  R.Initializer = *F.Signatures[0];
  EXPECT_FALSE(Check(Audits).empty());
  Audits.push_back(audit(R.Initializer->Entry));
  for (int Mutation = 0; Mutation < 8; ++Mutation) {
    auto Bad = Audits;
    auto &A = Bad.back();
    switch (Mutation) {
    case 0:
      A.Disposition = PipelineFunctionDisposition::Candidate;
      break;
    case 1:
      A.MedIRVerified = false;
      break;
    case 2:
      A.LiftedInstructions--;
      break;
    case 3:
      A.DecodeFailures = {0x1000};
      break;
    case 4:
      A.UnsupportedInstructions = {0x1000};
      break;
    case 5:
      A.TruncatedPaths = {0x1000};
      break;
    case 6:
      A.HasLowIR = false;
      break;
    case 7:
      Bad.push_back(A);
      break;
    }
    EXPECT_FALSE(Check(Bad).empty()) << Mutation;
  }
  Functions.pop_back();
  EXPECT_FALSE(Check(Audits).empty());
  Functions.push_back(Functions[0]);
  EXPECT_FALSE(Check(Audits).empty());
}

TEST(SwiftRuntimeProjection,
     IndexedRelatedMatchesRetainContextRoleAndNormalizedAmbiguity) {
  Fixture F;
  F.Requests[2]->Signature.MangledSymbol =
      "_" + F.Requests[2]->Signature.MangledSymbol;
  auto WrongContext = *F.Requests[2];
  WrongContext.Signature.Module = "Other";
  auto WrongRole = *F.Requests[2];
  WrongRole.Kind = Kind::TypeMetadataAccessor;
  F.Requests.push_back(WrongContext);
  F.Requests.push_back(WrongRole);
  auto Errors = linkRuntimeRequests(F.Requests, F.Signatures);
  EXPECT_FALSE(Errors.count(1));
  ASSERT_TRUE(F.Requests[1]->RelatedEntry);
  EXPECT_EQ(F.Requests[1]->RelatedEntry->MangledSymbol,
            F.Requests[2]->Signature.MangledSymbol);
  auto Duplicate = *F.Requests[2];
  Duplicate.Signature.MangledSymbol =
      runtimeSymbol(Duplicate.Signature.MangledSymbol);
  F.Requests.push_back(Duplicate);
  F.Requests.push_back(Duplicate);
  EXPECT_TRUE(linkRuntimeRequests(F.Requests, F.Signatures).count(1));
  EXPECT_FALSE(F.Requests[1]->RelatedEntry);
}

TEST(SwiftRuntimeProjection,
     NativeEvidenceIndexRejectsDuplicateBodiesAndEveryUnmodeledException) {
  Fixture F;
  auto R = *F.Requests[1];
  R.Initializer = *F.Signatures[0];
  std::vector<LowFunc> Functions(3);
  Functions[0].Entry = R.Signature.Entry;
  Functions[1].Entry = R.RelatedEntry->Entry;
  Functions[2].Entry = R.Initializer->Entry;
  std::vector<PipelineFunctionAudit> Audits;
  for (const auto &Fn : Functions)
    Audits.push_back(audit(Fn.Entry));
  const auto Index = indexRuntimeNativeEvidence(Audits, Functions);
  EXPECT_TRUE(runtimeAuditLimitation(R, Index).empty());
  auto Duplicate = Functions;
  Duplicate.push_back(Functions[0]);
  Duplicate.push_back(Functions[0]);
  EXPECT_FALSE(
      runtimeAuditLimitation(R, indexRuntimeNativeEvidence(Audits, Duplicate))
          .empty());
  for (size_t Referenced = 0; Referenced < Functions.size(); ++Referenced) {
    for (auto Encoding :
         {ExceptionEncoding::CompactUnwind, ExceptionEncoding::DwarfFDE}) {
      auto Plain = Functions;
      auto &EH = Plain[Referenced].ExceptionMetadata.emplace();
      EH.Encoding = Encoding;
      if (Encoding == ExceptionEncoding::CompactUnwind)
        EH.Compact.emplace();
      else
        EH.Dwarf.emplace();
      EXPECT_TRUE(
          runtimeAuditLimitation(R, indexRuntimeNativeEvidence(Audits, Plain))
              .empty());
    }
    for (int Mutation = 0; Mutation < 6; ++Mutation) {
      auto Bad = Functions;
      auto &EH = Bad[Referenced].ExceptionMetadata.emplace();
      EH.Encoding = ExceptionEncoding::CompactUnwind;
      EH.Compact.emplace();
      switch (Mutation) {
      case 0:
        EH.ParseStatus = ExceptionParseStatus::Partial;
        break;
      case 1:
        EH.Personality = ExceptionPersonality::ObjCPersonalityV0;
        break;
      case 2:
        EH.PersonalityName = "__unknown_handler";
        break;
      case 3:
        EH.Compact->LSDAVA = 0x4444;
        break;
      case 4:
        EH.Dwarf.emplace().LSDAVA = 0x5555;
        break;
      case 5:
        EH = ExceptionFunction();
        break;
      }
      EXPECT_FALSE(
          runtimeAuditLimitation(R, indexRuntimeNativeEvidence(Audits, Bad))
              .empty())
          << Referenced << ":" << Mutation;
    }
  }
}

TEST(SwiftRuntimeProjection,
     AllocatorRequiresExactConstructorIdentityAndRecoveredSource) {
  Fixture F;
  F.Requests = {std::nullopt, std::nullopt, std::nullopt};
  F.Proofs = {std::nullopt, std::nullopt, std::nullopt};
  auto &S = *F.Signatures[0];
  S.ContextKind = "class";
  S.Name = "init";
  S.DeclarationKind = "initializer";
  S.MangledSymbol = "$s4Demo3BoxCys5Int64Vcfc";
  auto Alloc = S;
  Alloc.Entry = 0x1050;
  Alloc.MangledSymbol = "$s4Demo3BoxCys5Int64VcfC";
  F.Requests[1] =
      SwiftRuntimeSourceRequest{Kind::AllocatingInitializer, Alloc, {}, {}};
  EXPECT_TRUE(linkRuntimeRequests(F.Requests, F.Signatures).empty());
  ASSERT_TRUE(F.Requests[1]->Initializer);
  EXPECT_EQ(runtimeIdentity(*F.Requests[1]->Initializer), runtimeIdentity(S));
  SwiftRuntimeSourceProof Proof;
  Proof.Proven = true;
  Proof.ProjectionKind = "allocating_initializer";
  Proof.Dependencies = {dependency("context", Alloc),
                        dependency("method", S, "init")};
  F.Proofs[1] = Proof;
  EXPECT_EQ(F.plan().Recovered, (std::set<size_t>{1}));
  EXPECT_TRUE(F.plan({}).Recovered.empty());
  F.Proofs[1]->Dependencies[1].Identity.Entry++;
  EXPECT_TRUE(F.plan().Recovered.empty());
  F.Signatures[2] = S;
  EXPECT_TRUE(linkRuntimeRequests(F.Requests, F.Signatures).count(1));
}

TEST(SwiftRuntimeProjection, DeallocatorWaitsForTheActualTrivialDestructor) {
  Fixture F;
  F.Requests = {std::nullopt, std::nullopt, std::nullopt};
  F.Proofs = {std::nullopt, std::nullopt, std::nullopt};
  auto &S = *F.Signatures[0];
  S.ContextKind = "class";
  S.DeclarationKind = "initializer";
  S.Name = "init";
  auto D = S;
  D.Entry = 0x1060;
  D.Name = "deinit";
  D.MangledSymbol = "$s4Demo3BoxCfd";
  auto Free = D;
  Free.Entry = 0x1070;
  Free.MangledSymbol = "$s4Demo3BoxCfD";
  F.Requests[1] =
      SwiftRuntimeSourceRequest{Kind::DeallocatingDestructor, Free, {}, {}};
  F.Requests[2] = SwiftRuntimeSourceRequest{Kind::TrivialDestructor, D, {}, {}};
  EXPECT_TRUE(linkRuntimeRequests(F.Requests, F.Signatures).empty());
  for (size_t I : {1U, 2U}) {
    SwiftRuntimeSourceProof P;
    P.Proven = true;
    P.ProjectionKind = runtimeProjectionKind(F.Requests[I]->Kind);
    P.Dependencies = {dependency("context", F.Requests[I]->Signature)};
    if (I == 1)
      P.Dependencies.push_back(dependency("compiler_entry", D, "deinit"));
    F.Proofs[I] = P;
  }
  auto P = F.plan();
  EXPECT_EQ(P.Recovered, (std::set<size_t>{1, 2}));
  EXPECT_TRUE(P.ContextSources.begin()->second.TrivialDestructor);
  F.Proofs[2]->Proven = false;
  EXPECT_TRUE(F.plan().Recovered.empty());
}

TEST(SwiftRuntimeProjection,
     RuntimeInventoryShapeAndDuplicateIdentityCannotCollapseRows) {
  Fixture F;
  F.Proofs.pop_back();
  EXPECT_THROW(F.plan(), std::invalid_argument);
  F = Fixture();
  F.Requests[2]->Signature = F.Requests[1]->Signature;
  F.Requests[2]->Signature.MangledSymbol =
      "_" + F.Requests[2]->Signature.MangledSymbol;
  EXPECT_THROW(F.plan(), std::invalid_argument);
}

namespace {
struct EmptyStructFixture {
  std::vector<SwiftRecoveredType> Types{1};
  std::vector<std::optional<SwiftSourceSignature>> Signatures{2};
  RuntimeRequests Requests{2};
  RuntimeProofs Proofs{2};

  EmptyStructFixture() {
    auto &T = Types.front();
    T.Module = "Demo";
    T.Kind = "struct";
    T.Name = "Empty";
    T.Descriptor = 0x1800;
    T.Metadata = 0x1900;
    T.Size = 0;
    T.Alignment = 1;
    T.Status = "recovered";
    auto S = signature(0x3000, "_$s4Demo5EmptyVMa", "typeMetadata");
    S.ContextName = "Empty";
    S.ContextFields.clear();
    S.DeclarationKind = "runtime";
    S.ReturnType = {};
    Requests[0] =
        SwiftRuntimeSourceRequest{Kind::TypeMetadataAccessor, S, {}, {}};
    SwiftRuntimeSourceProof P;
    // These are explicit planner inputs, not a claim of native proof execution.
    P.Proven = true;
    P.ProjectionKind = "type_metadata_accessor";
    P.Descriptor = 0x1800;
    P.Metadata = 0x1900;
    P.Dependencies = {{"context", "Demo", "struct", "Empty", {}, {}}};
    Proofs[0] = P;
  }

  EmptyStructSourceContexts collect() const {
    return collectEmptyStructSourceContexts(Requests, Proofs, Types);
  }

  RuntimeProjectionPlan plan(const EmptyStructSourceContexts &Contexts) const {
    return planRuntimeProjections(Requests, Proofs, Signatures, {}, Contexts);
  }
};
} // namespace

TEST(SwiftRuntimeProjection,
     EmptyStructMetadataUsesAnExplicitContextWithoutAnOrdinaryMethod) {
  EmptyStructFixture F;
  EXPECT_TRUE(F.plan({}).Recovered.empty());
  auto Contexts = F.collect();
  ASSERT_EQ(Contexts.size(), 1U);
  const PropertyContext Context{"Demo", "struct", "Empty"};
  ASSERT_TRUE(Contexts.count(Context));
  EXPECT_EQ(Contexts.at(Context).Descriptor, 0x1800U);
  EXPECT_EQ(Contexts.at(Context).Metadata, 0x1900U);
  const auto P = F.plan(Contexts);
  EXPECT_EQ(P.Recovered, (std::set<size_t>{0}));
  EXPECT_TRUE(P.Reasons.empty());
  ASSERT_EQ(P.NominalContexts.size(), 1U);
  EXPECT_EQ(assembleEmptyStructContext(P.NominalContexts.at(Context)),
            "struct `Empty` {\n}\n");
  ASSERT_EQ(F.Signatures.size(), 2U);
  EXPECT_FALSE(F.Signatures[0]);
  EXPECT_FALSE(F.Signatures[1]);
  EXPECT_EQ(F.Requests[0]->Signature.Entry, 0x3000U);
  EXPECT_EQ(F.Requests[0]->Signature.MangledSymbol, "_$s4Demo5EmptyVMa");
  EXPECT_FALSE(F.Requests[1]);
  EXPECT_TRUE(P.ContextSources.at(Context).Modifiers.empty());
  EXPECT_FALSE(P.ContextSources.at(Context).TrivialDestructor);
}

TEST(SwiftRuntimeProjection,
     EmptyStructContextRequiresUniqueNativeLayoutAndItsOwnProof) {
  for (unsigned Mutation = 0; Mutation < 24; ++Mutation) {
    SCOPED_TRACE(Mutation);
    EmptyStructFixture F;
    auto &T = F.Types.front();
    switch (Mutation) {
    case 0:
      F.Proofs[0].reset();
      break;
    case 1:
      F.Proofs[0]->Proven = false;
      break;
    case 2:
      F.Proofs[0]->ProjectionKind = "modify_accessor";
      break;
    case 3:
      F.Requests[0]->Kind = Kind::ModifyAccessor;
      break;
    case 4:
      F.Proofs[0]->Descriptor = 0x1804;
      break;
    case 5:
      F.Proofs[0]->Metadata = 0x1908;
      break;
    case 6:
      F.Types.clear();
      break;
    case 7:
      F.Types.push_back(T);
      break;
    case 8: {
      const auto Original = T;
      auto Other = T;
      Other.Status = "unrecovered";
      F.Types.push_back(Other);
      F.Types.push_back(Original);
      break;
    }
    case 9:
      T.Status = "unrecovered";
      break;
    case 10:
      T.Reason = "native layout is incomplete";
      break;
    case 11:
      T.Size = 1;
      break;
    case 12:
      T.Alignment = 0;
      break;
    case 13:
      T.Alignment = 8;
      break;
    case 14:
      T.Fields.push_back({"value",
                          {SwiftSourceType::Kind::Integer, "Int64", 64, true},
                          0,
                          true});
      break;
    case 15:
      T.Kind = "class";
      F.Requests[0]->Signature.ContextKind = "class";
      break;
    case 16:
      T.Module = "Other";
      break;
    case 17:
      T.Name = "Other";
      break;
    case 18:
      T.Descriptor = 0;
      F.Proofs[0]->Descriptor = 0;
      break;
    case 19:
      T.Metadata = 0;
      F.Proofs[0]->Metadata = 0;
      break;
    case 20:
      T.Module = "Not.Safe";
      F.Requests[0]->Signature.Module = T.Module;
      break;
    case 21:
      T.Name = "Bad`Name";
      F.Requests[0]->Signature.ContextName = T.Name;
      break;
    case 22:
      T.Name.clear();
      F.Requests[0]->Signature.ContextName.clear();
      break;
    case 23:
      F.Requests[0].reset();
      break;
    }
    const auto Contexts = F.collect();
    EXPECT_TRUE(Contexts.empty());
    EXPECT_TRUE(F.plan(Contexts).Recovered.empty());
  }
  EmptyStructFixture F;
  F.Proofs.pop_back();
  EXPECT_THROW(F.collect(), std::invalid_argument);
}

TEST(SwiftRuntimeProjection,
     NominalContextAuthorizesOnlyItsBoundMetadataAccessor) {
  for (auto Role : {Kind::AllocatingInitializer, Kind::TrivialDestructor,
                    Kind::DeallocatingDestructor, Kind::ModifyAccessor,
                    Kind::ModifyResume, Kind::TypeMetadataAccessor}) {
    SCOPED_TRACE(static_cast<unsigned>(Role));
    EmptyStructFixture F;
    F.Requests[1] = F.Requests[0];
    F.Requests[1]->Kind = Role;
    F.Requests[1]->Signature.Entry = 0x3100;
    F.Requests[1]->Signature.MangledSymbol = "_$s4Demo5EmptyVother";
    F.Proofs[1] = F.Proofs[0];
    F.Proofs[1]->ProjectionKind = runtimeProjectionKind(Role);
    if (Role == Kind::TypeMetadataAccessor)
      F.Proofs[1]->Metadata = 0x1910;
    const auto Contexts = F.collect();
    ASSERT_EQ(Contexts.size(), 1U);
    const auto P = F.plan(Contexts);
    EXPECT_EQ(P.Recovered, (std::set<size_t>{0}));
    EXPECT_TRUE(P.Reasons.count(1));
    ASSERT_EQ(P.NominalContexts.size(), 1U);
    EXPECT_EQ(assembleEmptyStructContext(P.NominalContexts.begin()->second),
              "struct `Empty` {\n}\n");
  }
}

TEST(SwiftRuntimeProjection,
     NominalMetadataRetainsEverySourceDependencyAndRejectsChangedSeeds) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    EmptyStructFixture F;
    auto Contexts = F.collect();
    ASSERT_EQ(Contexts.size(), 1U);
    auto &Dependencies = F.Proofs[0]->Dependencies;
    switch (Mutation) {
    case 0:
      Dependencies.clear();
      break;
    case 1:
      Dependencies.front().Module = "Other";
      break;
    case 2:
      Dependencies.front().ContextName = "Other";
      break;
    case 3:
      Dependencies.push_back(Dependencies.front());
      break;
    case 4:
      Dependencies.push_back(
          {"method", "Demo", "struct", "Empty", "missing", {0x3100, "x"}});
      break;
    case 5:
      Contexts.begin()->second.Metadata = 0x1910;
      break;
    case 6:
      Contexts.begin()->second.Size = 1;
      break;
    case 7:
      F.Proofs[0]->Proven = false;
      break;
    }
    const auto P = F.plan(Contexts);
    EXPECT_TRUE(P.Recovered.empty());
    EXPECT_TRUE(P.NominalContexts.empty());
    EXPECT_TRUE(P.Reasons.count(0));
  }
  EmptyStructFixture F;
  auto Contexts = F.collect();
  Contexts.begin()->second.Name = "Injected`Name";
  EXPECT_THROW(assembleEmptyStructContext(Contexts.begin()->second),
               std::invalid_argument);
  Contexts = F.collect();
  Contexts.begin()->second.Size = 1;
  EXPECT_THROW(assembleEmptyStructContext(Contexts.begin()->second),
               std::invalid_argument);
}

TEST(SwiftRuntimeProjection,
     NominalSourceNamesRejectBothSidesOfTopLevelCollisions) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto S = signature(0x3100, "_$s4Demo5EmptySiyF", "Empty");
    S.ContextFields.clear();
    S.ContextKind = "global";
    S.ContextName.clear();
    NominalSourceContext Nominal{"Demo", "struct", "Empty"};
    if (Mutation == 1) {
      S.ContextKind = "struct";
      S.ContextName = "Empty";
      S.Module = "Other";
    }
    if (Mutation == 2) {
      S.ContextKind = "class";
      S.ContextName = "Empty";
    }
    if (Mutation == 3) {
      S.Name = "Swift";
      Nominal.Name = "Swift";
    }
    auto Unrelated = signature(0x3200, "_$s4Demo5OtherV5values5Int64Vvg");
    Unrelated.ContextName = "Other";
    const auto C = namespaceConflicts({S, Unrelated}, {Nominal});
    EXPECT_EQ(C.Methods, (std::set<size_t>{0}));
    EXPECT_EQ(C.Nominals, (std::set<size_t>{0}));
  }
  const auto C = namespaceConflicts({}, {{"Demo", "struct", "Empty"},
                                         {"Other", "struct", "Empty"},
                                         {"Demo", "struct", "Independent"}});
  EXPECT_TRUE(C.Methods.empty());
  EXPECT_EQ(C.Nominals, (std::set<size_t>{0, 1}));
}

TEST(SwiftRuntimeProjection,
     ExistingOrdinaryContextAndOverloadRulesDoNotBecomeNominalShells) {
  EmptyStructFixture F;
  auto S = signature(0x3100, "_$s4Demo5EmptyV5values5Int64VF");
  S.ContextName = "Empty";
  S.ContextFields.clear();
  F.Signatures[1] = S;
  const std::vector<NominalSourceContext> Nominals{{"Demo", "struct", "Empty"}};
  auto C = namespaceConflicts(F.Signatures, Nominals);
  EXPECT_TRUE(C.Methods.empty());
  EXPECT_TRUE(C.Nominals.empty());
  const auto P = planRuntimeProjections(F.Requests, F.Proofs, F.Signatures, {1},
                                        F.collect());
  EXPECT_EQ(P.Recovered, (std::set<size_t>{0}));
  EXPECT_TRUE(P.NominalContexts.empty());
  const std::string Member = "    func value() -> Swift.Int64 { return 7 }\n";
  EXPECT_EQ(assemblePropertyContext(S, {{&S, Member}}),
            "struct `Empty` {\n" + Member + "\n}\n");

  auto Overload = S;
  Overload.Entry = 0x3200;
  Overload.Parameters.push_back({"arg0", S.ReturnType});
  Overload.Labels.push_back("_");
  C = namespaceConflicts({S, Overload}, Nominals);
  EXPECT_TRUE(C.Methods.empty());
  EXPECT_TRUE(C.Nominals.empty());
  C = namespaceConflicts({S, S}, Nominals);
  EXPECT_EQ(C.Methods, (std::set<size_t>{0, 1}));
  EXPECT_TRUE(C.Nominals.empty());
  EXPECT_EQ(namespaceConflicts({S, S}), C.Methods);
}
