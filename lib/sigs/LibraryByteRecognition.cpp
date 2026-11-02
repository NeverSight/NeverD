//===- LibraryByteRecognition.cpp - Gated existing byte matcher ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolSpelling.h"
#include "neverd/sigs/Limits.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/sigs/SignatureMatcher.h"

#include <map>
#include <set>

using namespace neverd;
using namespace neverd::sigs;

LibraryRecognition
neverd::sigs::describeLibraryRecognition(va_t Function,
                                         const LibraryFeaturePack &Pack,
                                         const LibraryFeatureRule &Rule) {
  LibraryRecognition R;
  R.Function = Function;
  R.Pack = Pack.Id;
  R.PackSHA256 = Pack.SHA256;
  R.ProfileSHA256 = Pack.ProfileSHA256;
  R.EvidenceSHA256 = Pack.EvidenceSHA256;
  R.Rule = Rule.Id;
  R.RuleRevision = Rule.Revision;
  R.Family = Rule.Family;
  R.Operation = Rule.Operation;
  R.ReceiverType = Rule.ReceiverType;
  R.SourceOrigin = Pack.SourceOrigin;
  R.SourceRevision = Pack.SourceRevision;
  R.DisplayName = Rule.ReceiverType.empty()
                      ? Rule.Operation
                      : Rule.ReceiverType + "::" + Rule.Operation;
  return R;
}

std::vector<LibraryRecognition> SignatureDB::recognizeFeatureBytes(
    const BinaryImage &Image, llvm::ArrayRef<uint64_t> Entries,
    const FeatureIdentityVerifier &Identity) const {
  return recognizeLibraryFeatureBytes(Image, FeaturePacks, Entries, Identity);
}

std::vector<LibraryRecognition> neverd::sigs::recognizeLibraryFeatureBytes(
    const BinaryImage &Image,
    const std::map<std::string, LibraryFeaturePack> &Packs,
    llvm::ArrayRef<uint64_t> Entries,
    const SignatureDB::FeatureIdentityVerifier &Identity) {
  if (!Identity)
    return {};
  struct Candidate {
    const LibraryFeaturePack *Pack;
    const LibraryFeatureRule *Rule;
    uint32_t Length;
  };
  std::vector<Candidate> Candidates;
  for (const auto &[ID, Pack] : Packs) {
    (void)ID;
    if (!Pack.accepts(Image))
      continue;
    for (const auto &Rule : Pack.Rules) {
      if (Rule.PatternKind != LibraryFeatureRule::Kind::BytePattern)
        continue;
      auto Parsed = PatternParser::parseText(Rule.PatternText);
      if (!Parsed) {
        llvm::consumeError(Parsed.takeError());
        continue;
      }
      if (Parsed->size() != 1 ||
          !SignatureMatcher::isFullyVerified(Parsed->front()) ||
          SignatureMatcher::fixedByteCount(Parsed->front()) <
              SignatureMatcher::MinStatedBytes)
        continue;
      Candidates.push_back({&Pack, &Rule, Parsed->front().TotalLen});
    }
  }
  // Entries with the same independently admitted rule set share one index.
  // Ineligible rules cannot create a conflicting name, nor can a byte hit
  // manufacture the type/symbol fact needed to admit its own rule.
  std::map<std::vector<size_t>, std::vector<uint64_t>> Groups;
  std::map<std::pair<va_t, size_t>, std::string> Evidence;
  if (!Candidates.empty() &&
      Entries.size() >
          ::neverd::limits::sigs::kLibraryByteRecognitionMaxEligibilityChecks /
              Candidates.size())
    return {};
  for (va_t Entry : Entries) {
    std::vector<size_t> Admitted;
    for (size_t I = 0; I < Candidates.size(); ++I) {
      auto Proof = Identity(*Candidates[I].Pack, *Candidates[I].Rule, Entry);
      if (Proof.empty())
        continue;
      Admitted.push_back(I);
      Evidence.emplace(std::make_pair(Entry, I), std::move(Proof));
    }
    if (!Admitted.empty())
      Groups[Admitted].push_back(Entry);
  }
  std::map<va_t, std::set<std::string>> StatedSymbols;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc && Symbol.Origin == NameOrigin::Stated)
      StatedSymbols[Symbol.Addr].insert(Symbol.Name);
  std::vector<LibraryRecognition> Results;
  for (const auto &[Indices, Addresses] : Groups) {
    SignatureDB ByteDB;
    std::map<std::string, size_t> Sources;
    for (size_t I : Indices) {
      const auto &C = Candidates[I];
      const auto Source = C.Pack->Id + ':' + C.Rule->Id;
      if (auto Error = ByteDB.loadPatternText(C.Rule->PatternText, Source)) {
        llvm::consumeError(std::move(Error));
        return {};
      }
      Sources.emplace(Source, I);
    }
    ByteDB.apply(Image, Addresses);
    // Identity was independently admitted above; byte settlement selects among
    // those eligible rules without granting identities to ordinary byte hits.
    const auto Settled = ByteDB.buildCandidateNameMap();
    std::map<va_t, std::set<size_t>> Winners;
    for (const auto &M : ByteDB.matches()) {
      const auto At = Settled.find(M.Address);
      const auto Source = Sources.find(M.LibraryName);
      if (At == Settled.end() || At->second != M.Name ||
          Source == Sources.end())
        continue;
      Winners[M.Address].insert(Source->second);
    }
    for (const auto &[Address, Choices] : Winners) {
      // Reference confirmation settles a linkage identity, never the exact
      // source revision. Conflicting feature profiles still abstain.
      if (Choices.size() != 1)
        continue;
      const auto I = *Choices.begin();
      const auto &C = Candidates[I];
      auto Match = describeLibraryRecognition(Address, *C.Pack, *C.Rule);
      Match.IdentityEvidence = Evidence.at({Address, I});
      Match.Scope = LibraryFeatureScope::WholeFunction;
      Match.ByteLength = C.Length;
      if (const auto At = StatedSymbols.find(Address);
          At != StatedSymbols.end() && At->second.contains(C.Rule->LinkageName))
        Match.LinkageName = C.Rule->LinkageName;
      Match.DisplayName = displaySymbolName(C.Rule->LinkageName);
      Results.push_back(std::move(Match));
    }
  }
  return Results;
}
