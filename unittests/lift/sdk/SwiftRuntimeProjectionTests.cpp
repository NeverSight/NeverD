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
