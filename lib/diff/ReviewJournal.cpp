//===- ReviewJournal.cpp - Bounded manual assessment history --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Manual statements have their own sequence and input binding. They neither
/// alter correspondence nor promote computed observations into proof claims.
//===----------------------------------------------------------------------===//

#include "neverd/diff/ReviewJournal.h"

#include "neverd/diff/Comparison.h"
#include "neverd/diff/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <map>
#include <stdexcept>
#include <tuple>

namespace neverd::diff {
namespace {
using CaseKey = std::tuple<std::string, uint64_t, uint64_t>;
CaseKey key(const ReviewDraft &D) { return {D.Case, D.Primary, D.Secondary}; }
std::optional<std::string> draftError(const ReviewDraft &D) {
  if (reviewAssessmentName(D.Assessment).empty())
    return "unknown analyst assessment";
  for (const auto &[Text, Max, Empty, Multiline] :
       {std::tuple{&D.Case, limits::diff::kMaxReviewCaseBytes, false, false},
        {&D.Author, limits::diff::kMaxReviewAuthorBytes, false, false},
        {&D.Note, limits::diff::kMaxReviewNoteBytes, false, true},
        {&D.Reference, limits::diff::kMaxReviewReferenceBytes, true, false}}) {
    if (Text->size() > Max ||
        (!Empty && llvm::StringRef(*Text).trim().empty()) ||
        !llvm::json::isUTF8(*Text))
      return "review text is empty, too long or invalid UTF-8";
    for (unsigned char C : *Text)
      if ((C < 32 || C == 127) && !(Multiline && (C == '\n' || C == '\t')))
        return "review text contains an unsupported control character";
  }
  return std::nullopt;
}
uint64_t textSize(const ReviewDraft &D) {
  return D.Case.size() + D.Author.size() + D.Note.size() + D.Reference.size();
}
std::optional<std::string> identityError(const ProgramIdentity &P,
                                         const ProgramIdentity &S) {
  for (const auto *I : {&P, &S}) {
    if (I->SHA256.size() != limits::diff::kReviewDigestHexBytes ||
        !llvm::all_of(I->SHA256, llvm::isHexDigit) || I->Revision.empty())
      return "review history requires exact input hashes and analysis "
             "revisions";
    uint64_t Bytes = 0;
    for (const auto *Text :
         {&I->SHA256, &I->Format, &I->Architecture, &I->ABI, &I->Normalization,
          &I->Revision, &I->Endian, &I->Analysis}) {
      if (!llvm::json::isUTF8(*Text))
        return "review input identity is not valid UTF-8";
      Bytes += Text->size();
      if (Bytes > limits::diff::kMaxReviewIdentityBytes)
        return "review input identity exceeds its text allowance";
    }
  }
  return std::nullopt;
}
llvm::json::Array identity(const ProgramIdentity &I) {
  // Path is a location hint. A copied input with identical content/analysis may
  // reopen its journal; every semantic identity field still participates.
  return llvm::json::Array{I.SHA256,     I.Format,        I.Architecture,
                           I.ABI,        I.Normalization, I.Revision,
                           I.Bits,       I.Endian,        I.ImageBase,
                           I.EntryPoint, I.Analysis,      I.DiscoveryComplete};
}
std::string digest(const ReviewRecord &R, const ProgramIdentity &P,
                   const ProgramIdentity &S) {
  // Ordered arrays give a canonical hash independent of JSON object ordering.
  llvm::json::Array Fields{ReviewJournalProfile,
                           identity(P),
                           identity(S),
                           R.Sequence,
                           R.Previous,
                           R.Supersedes,
                           R.Primary,
                           R.Secondary,
                           R.Case,
                           R.Author,
                           R.Note,
                           R.Reference,
                           reviewAssessmentName(R.Assessment)};
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(std::move(Fields));
  llvm::SHA256 Hash;
  Hash.update(Text);
  return llvm::toHex(Hash.final(), true);
}
std::string hex(uint64_t Value) { return "0x" + llvm::utohexstr(Value, true); }
void require(bool OK, const char *Why) {
  if (!OK)
    throw std::invalid_argument(Why);
}
void fields(const llvm::json::Object &O,
            std::initializer_list<llvm::StringRef> Allowed) {
  for (const auto &Field : O)
    require(
        llvm::is_contained(Allowed, static_cast<llvm::StringRef>(Field.first)),
        "unknown review field");
}
std::string text(const llvm::json::Object &O, llvm::StringRef Name) {
  const auto S = O.getString(Name);
  require(S && S->size() <= limits::diff::kMaxReviewNoteBytes,
          "review field must be a bounded string");
  return S->str();
}
uint64_t address(const llvm::json::Object &O, llvm::StringRef Name) {
  auto S = O.getString(Name);
  uint64_t Value = 0;
  require(S && S->starts_with("0x") && S->size() > 2 &&
              !S->drop_front(2).getAsInteger(16, Value),
          "review address must be a hexadecimal string");
  return Value;
}
} // namespace

llvm::StringRef reviewAssessmentName(ReviewAssessment Assessment) {
  switch (Assessment) {
  case ReviewAssessment::Investigating:
    return "investigating";
  case ReviewAssessment::RelevantChange:
    return "relevant_change";
  case ReviewAssessment::UnrelatedChange:
    return "unrelated_change";
  case ReviewAssessment::CandidateFix:
    return "candidate_fix";
  case ReviewAssessment::PatchPresent:
    return "patch_present";
  case ReviewAssessment::PatchAbsent:
    return "patch_absent";
  case ReviewAssessment::PartialPatch:
    return "partial_patch";
  case ReviewAssessment::RevertedPatch:
    return "reverted_patch";
  case ReviewAssessment::Inconclusive:
    return "inconclusive";
  }
  return {};
}
std::optional<ReviewAssessment> reviewAssessmentNamed(llvm::StringRef Name) {
  for (auto A :
       {ReviewAssessment::Investigating, ReviewAssessment::RelevantChange,
        ReviewAssessment::UnrelatedChange, ReviewAssessment::CandidateFix,
        ReviewAssessment::PatchPresent, ReviewAssessment::PatchAbsent,
        ReviewAssessment::PartialPatch, ReviewAssessment::RevertedPatch,
        ReviewAssessment::Inconclusive})
    if (Name == reviewAssessmentName(A))
      return A;
  return std::nullopt;
}
std::optional<std::string>
validateReviewJournal(const ReviewJournal &Journal, const ProgramIdentity &P,
                      const ProgramIdentity &S,
                      const ComparisonControl &Control) {
  Control.check();
  if (Journal.Records.size() > limits::diff::kMaxReviewRecords)
    return "review history exceeds its record allowance";
  if (!Journal.Records.empty())
    if (const auto Error = identityError(P, S))
      return Error;
  uint64_t Bytes = 0;
  std::string Previous;
  std::map<CaseKey, std::string> Latest;
  for (uint32_t I = 0; I < Journal.Records.size(); ++I) {
    Control.check();
    const auto &R = Journal.Records[I];
    if (const auto Error = draftError(R))
      return Error;
    Bytes += textSize(R);
    if (Bytes > limits::diff::kMaxReviewTextBytes)
      return "review history exceeds its text allowance";
    const auto K = key(R);
    if (R.Sequence != I || R.Previous != Previous || R.Supersedes != Latest[K])
      return "review history has inconsistent sequence or case links";
    if (R.ID != digest(R, P, S))
      return "review history digest does not bind to its input identities and "
             "content";
    Previous = R.ID;
    Latest[K] = R.ID;
  }
  return std::nullopt;
}
bool reviewPairActive(const ReviewRecord &Record, const ComparisonResult &R,
                      const Program &P, const Program &S) {
  const auto PF = P.functionAt(Record.Primary),
             SF = S.functionAt(Record.Secondary);
  return PF && SF && R.secondaryOf(*PF) == *SF;
}
std::optional<std::string> appendReview(ComparisonResult &R, const Program &P,
                                        const Program &S, const ReviewDraft &D,
                                        llvm::StringRef ExpectedHead) {
  if (const auto Error =
          validateReviewJournal(R.Reviews, P.Identity, S.Identity))
    return Error;
  if (R.Reviews.head() != ExpectedHead)
    return "review history changed; reload its head before recording";
  if (const auto Error = draftError(D))
    return Error;
  if (const auto Error = identityError(P.Identity, S.Identity))
    return Error;
  ReviewRecord Next;
  static_cast<ReviewDraft &>(Next) = D;
  if (!reviewPairActive(Next, R, P, S))
    return "a new review requires an accepted function pair";
  if (R.Reviews.Records.size() == limits::diff::kMaxReviewRecords)
    return "review history exceeds its record allowance";
  uint64_t Bytes = textSize(D);
  for (const auto &Old : R.Reviews.Records) {
    Bytes += textSize(Old);
    if (key(Old) == key(D))
      Next.Supersedes = Old.ID;
  }
  if (Bytes > limits::diff::kMaxReviewTextBytes)
    return "review history exceeds its text allowance";
  Next.Sequence = R.Reviews.Records.size();
  Next.Previous = R.Reviews.head().str();
  Next.ID = digest(Next, P.Identity, S.Identity);
  R.Reviews.Records.push_back(std::move(Next));
  return std::nullopt;
}
llvm::json::Object reviewRecordJSON(const ReviewRecord &R) {
  return llvm::json::Object{{"id", R.ID},
                            {"sequence", R.Sequence},
                            {"previous", R.Previous},
                            {"supersedes", R.Supersedes},
                            {"primary", hex(R.Primary)},
                            {"secondary", hex(R.Secondary)},
                            {"case", R.Case},
                            {"author", R.Author},
                            {"note", R.Note},
                            {"reference", R.Reference},
                            {"assessment", reviewAssessmentName(R.Assessment)},
                            {"source", "analyst"},
                            {"semantic_validation", "not_performed"}};
}
llvm::json::Object reviewJournalJSON(const ReviewJournal &J) {
  llvm::json::Array Records;
  for (const auto &R : J.Records)
    Records.push_back(reviewRecordJSON(R));
  return llvm::json::Object{{"profile", ReviewJournalProfile},
                            {"head", J.head()},
                            {"author_authentication", "not_performed"},
                            {"records", std::move(Records)}};
}
std::optional<ReviewJournal>
readReviewJournal(const llvm::json::Value &Value, const ProgramIdentity &P,
                  const ProgramIdentity &S, std::string &Error,
                  const ComparisonControl &Control) {
  Control.check();
  Error.clear();
  try {
    const auto *O = Value.getAsObject();
    require(O, "review history must be an object");
    fields(*O, {"profile", "head", "author_authentication", "records"});
    require(text(*O, "profile") == ReviewJournalProfile,
            "unknown review profile");
    require(text(*O, "author_authentication") == "not_performed",
            "unsupported author authentication claim");
    const auto *Records = O->getArray("records");
    require(Records && Records->size() <= limits::diff::kMaxReviewRecords,
            "review records must be a bounded array");
    ReviewJournal Out;
    uint64_t Bytes = 0;
    for (const auto &V : *Records) {
      Control.check();
      const auto *Row = V.getAsObject();
      require(Row, "review record must be an object");
      fields(*Row, {"id", "sequence", "previous", "supersedes", "primary",
                    "secondary", "case", "author", "note", "reference",
                    "assessment", "source", "semantic_validation"});
      const auto Sequence = Row->getInteger("sequence");
      require(Sequence && *Sequence >= 0 &&
                  uint64_t(*Sequence) == Out.Records.size(),
              "review sequence is out of order");
      require(text(*Row, "source") == "analyst" &&
                  text(*Row, "semantic_validation") == "not_performed",
              "manual review cannot claim engine validation");
      ReviewRecord R;
      R.Sequence = *Sequence;
      R.ID = text(*Row, "id");
      R.Previous = text(*Row, "previous");
      R.Supersedes = text(*Row, "supersedes");
      require(R.ID.size() == limits::diff::kReviewDigestHexBytes &&
                  R.Previous.size() <= limits::diff::kReviewDigestHexBytes &&
                  R.Supersedes.size() <= limits::diff::kReviewDigestHexBytes,
              "invalid review digest length");
      R.Primary = address(*Row, "primary");
      R.Secondary = address(*Row, "secondary");
      R.Case = text(*Row, "case");
      R.Author = text(*Row, "author");
      R.Note = text(*Row, "note");
      R.Reference = text(*Row, "reference");
      const auto Assessment = reviewAssessmentNamed(text(*Row, "assessment"));
      require(Assessment.has_value(), "unknown analyst assessment");
      R.Assessment = *Assessment;
      if (const auto E = draftError(R))
        throw std::invalid_argument(*E);
      Bytes += textSize(R);
      require(Bytes <= limits::diff::kMaxReviewTextBytes,
              "review text allowance exceeded");
      Out.Records.push_back(std::move(R));
    }
    require(text(*O, "head") == Out.head(),
            "review head does not name its last record");
    if (const auto E = validateReviewJournal(Out, P, S, Control))
      throw std::invalid_argument(*E);
    return Out;
  } catch (const ComparisonInterrupted &) {
    throw;
  } catch (const std::exception &E) {
    Error = E.what();
    return std::nullopt;
  }
}
} // namespace neverd::diff
