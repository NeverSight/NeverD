//===- BinaryDiffReviewTests.cpp - Manual assessment history contracts
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owned comparisons qualify bounded journal binding and independent history,
/// not the truth, author identity or security interpretation of an assessment.
//===----------------------------------------------------------------------===//

#include "neverd/diff/Comparison.h"
#include "neverd/diff/ComparisonJSON.h"
#include "neverd/diff/ReviewJournal.h"

#include <gtest/gtest.h>

using namespace neverd::diff;
namespace {
Program program(uint64_t Base, char Hash) {
  ProgramIdentity ID;
  ID.Architecture = "x86_64";
  ID.Bits = 64;
  ID.ABI = "owned-review-abi";
  ID.Revision = "owned-review-revision";
  ID.SHA256 = std::string(64, Hash);
  ID.ImageBase = Base;
  ProgramBuilder B(ID);
  for (unsigned I = 0; I < 2; ++I) {
    const auto Entry = Base + 16 * I;
    B.beginFunction(Entry, 1, I ? "other" : "reviewed", NameSource::Symbol, 0);
    B.addBlock(Entry);
    B.addInstruction(Entry, {0xc3}, "ret", 1, 1, InstructionFlow::Return);
  }
  return B.finish();
}
ReviewDraft draft() {
  ReviewDraft D;
  D.Primary = 0x1000;
  D.Secondary = 0x8000;
  D.Case = "owned-case-42";
  D.Author = "Fixture analyst";
  D.Note = "Compare the original guard and its callers.\nNo security proof "
           "attached.";
  D.Reference = "owned-advisory:42";
  return D;
}
} // namespace

TEST(BinaryDiffReview, RecordsRemainSeparateFromEngineFactsAndMappingUndo) {
  const auto P = program(0x1000, 'a'), S = program(0x8000, 'b');
  auto R = compare(P, S, {});
  const auto Before = R.Matches[0].Changes;
  auto D = draft();
  ASSERT_FALSE(appendReview(R, P, S, D, ""));
  ASSERT_EQ(R.Reviews.Records.size(), 1u);
  const auto First = R.Reviews.Records[0].ID;
  D.Assessment = ReviewAssessment::PatchPresent;
  D.Note = "Manual fixture assessment; deliberately unvalidated.";
  ASSERT_FALSE(appendReview(R, P, S, D, First));
  EXPECT_EQ(R.Reviews.Records[1].Supersedes, First);
  EXPECT_EQ(R.Reviews.Records[1].Previous, First);
  EXPECT_EQ(R.Matches[0].Changes, Before);
  const auto Journal = R.Reviews;
  OverrideEffect Effect;
  ASSERT_FALSE(
      applyOverride(R, P, S, {OverrideKind::Unmatch, 0x1000, 0x8000}, &Effect));
  EXPECT_FALSE(reviewPairActive(R.Reviews.Records.back(), R, P, S));
  EXPECT_TRUE(appendReview(R, P, S, D, R.Reviews.head()));
  EXPECT_EQ(R.Reviews, Journal);
  revertOverride(R, P, S, Effect);
  EXPECT_TRUE(reviewPairActive(R.Reviews.Records.back(), R, P, S));
  EXPECT_EQ(R.Reviews, Journal);
  const auto JSON = reviewRecordJSON(R.Reviews.Records.back());
  EXPECT_EQ(JSON.getString("source"), "analyst");
  EXPECT_EQ(JSON.getString("semantic_validation"), "not_performed");
}

TEST(BinaryDiffReview, StaleHeadsAndInvalidDraftsCannotPartiallyAppend) {
  const auto P = program(0x1000, 'a'), S = program(0x8000, 'b');
  auto R = compare(P, S, {});
  auto D = draft();
  ASSERT_FALSE(appendReview(R, P, S, D, ""));
  const auto Journal = R.Reviews;
  EXPECT_TRUE(appendReview(R, P, S, D, ""));
  D.Author.clear();
  EXPECT_TRUE(appendReview(R, P, S, D, R.Reviews.head()));
  D = draft();
  D.Note.assign(neverd::limits::diff::kMaxReviewNoteBytes + 1, 'x');
  EXPECT_TRUE(appendReview(R, P, S, D, R.Reviews.head()));
  D = draft();
  D.Case.push_back('\0');
  EXPECT_TRUE(appendReview(R, P, S, D, R.Reviews.head()));
  D = draft();
  D.Assessment = ReviewAssessment(255);
  EXPECT_TRUE(appendReview(R, P, S, D, R.Reviews.head()));
  EXPECT_EQ(R.Reviews, Journal);
}

TEST(BinaryDiffReview, InvalidIdentityFailsBeforeCanonicalEncoding) {
  auto P = program(0x1000, 'a'), S = program(0x8000, 'b');
  auto R = compare(P, S, {});
  P.Identity.SHA256.clear();
  EXPECT_TRUE(appendReview(R, P, S, draft(), ""));
  P.Identity.SHA256.assign(neverd::limits::diff::kReviewDigestHexBytes, 'a');
  P.Identity.ABI.assign(1, char(0xff));
  EXPECT_TRUE(appendReview(R, P, S, draft(), ""));
  P.Identity.ABI = "owned-review-abi";
  P.Identity.Analysis.assign(neverd::limits::diff::kMaxReviewIdentityBytes + 1,
                             'x');
  EXPECT_TRUE(appendReview(R, P, S, draft(), ""));
  EXPECT_TRUE(R.Reviews.Records.empty());
}

TEST(BinaryDiffReview, CanonicalRecordsBindContentAnalysisAndCaseLinks) {
  auto P = program(0x1000, 'a'), S = program(0x8000, 'b');
  auto R = compare(P, S, {});
  auto D = draft();
  ASSERT_FALSE(appendReview(R, P, S, D, ""));
  const auto First = R.Reviews.head().str();
  D.Case = "other-case";
  ASSERT_FALSE(appendReview(R, P, S, D, R.Reviews.head()));
  EXPECT_TRUE(R.Reviews.Records.back().Supersedes.empty());
  D = draft();
  ASSERT_FALSE(appendReview(R, P, S, D, R.Reviews.head()));
  EXPECT_EQ(R.Reviews.Records.back().Supersedes, First);
  const llvm::json::Value JSON = reviewJournalJSON(R.Reviews);
  std::string Error;
  const auto Parsed = readReviewJournal(JSON, P.Identity, S.Identity, Error);
  ASSERT_TRUE(Parsed) << Error;
  EXPECT_EQ(*Parsed, R.Reviews);
  P.Identity.Path = "/copied/owned-input";
  EXPECT_FALSE(validateReviewJournal(R.Reviews, P.Identity, S.Identity));
  P.Identity.SHA256[0] = 'f';
  EXPECT_TRUE(validateReviewJournal(R.Reviews, P.Identity, S.Identity));
  P.Identity.SHA256[0] = 'a';
  S.Identity.Revision += "-different";
  EXPECT_TRUE(validateReviewJournal(R.Reviews, P.Identity, S.Identity));
}

TEST(BinaryDiffReview, MalformedHistoryCannotClaimProofOrChangeAnnotations) {
  const auto P = program(0x1000, 'a'), S = program(0x8000, 'b');
  auto R = compare(P, S, {});
  ASSERT_FALSE(appendReview(R, P, S, draft(), ""));
  std::string Error;
  for (const auto *Field : {"note", "id", "source", "semantic_validation",
                            "previous", "supersedes"}) {
    auto JSON = reviewJournalJSON(R.Reviews);
    (*JSON.getArray("records"))[0].getAsObject()->operator[](Field) = "altered";
    EXPECT_FALSE(readReviewJournal(llvm::json::Value(std::move(JSON)),
                                   P.Identity, S.Identity, Error))
        << Field;
    EXPECT_FALSE(Error.empty());
  }
  auto JSON = reviewJournalJSON(R.Reviews);
  JSON["author_authentication"] = "verified";
  EXPECT_FALSE(readReviewJournal(llvm::json::Value(std::move(JSON)), P.Identity,
                                 S.Identity, Error));
  JSON = reviewJournalJSON(R.Reviews);
  JSON["typo"] = true;
  EXPECT_FALSE(readReviewJournal(llvm::json::Value(std::move(JSON)), P.Identity,
                                 S.Identity, Error));
  ReviewJournal TooMany;
  TooMany.Records.resize(neverd::limits::diff::kMaxReviewRecords + 1);
  EXPECT_TRUE(validateReviewJournal(TooMany, P.Identity, S.Identity));
}

TEST(BinaryDiffReview, SavedHistoryRebindsWhileLegacyReportsHaveNoAssessments) {
  const auto P = program(0x1000, 'a'), S = program(0x8000, 'b');
  auto R = compare(P, S, {});
  const auto Document = [&] {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    writeComparison(OS, R, P, S);
    return Text;
  };
  auto Legacy = llvm::json::parse(Document());
  ASSERT_TRUE(bool(Legacy));
  (*Legacy->getAsObject())["schema_version"] = OldestComparisonSchemaVersion;
  Legacy->getAsObject()->erase("reviews");
  std::string OldText;
  llvm::raw_string_ostream(OldText) << *Legacy;
  std::string Error;
  const auto Old = readComparison(OldText, Error);
  ASSERT_TRUE(Old) << Error;
  EXPECT_TRUE(Old->Reviews.Records.empty());
  ASSERT_FALSE(appendReview(R, P, S, draft(), ""));
  auto Saved = readComparison(Document(), Error);
  ASSERT_TRUE(Saved) << Error;
  ComparisonResult Reopened;
  ASSERT_FALSE(bindComparison(*Saved, P, S, Reopened));
  EXPECT_EQ(Reopened.Reviews, R.Reviews);
  Saved->Reviews.Records[0].Note += " changed";
  EXPECT_TRUE(bindComparison(*Saved, P, S, Reopened));
  EXPECT_EQ(Reopened.Reviews, R.Reviews);
}
