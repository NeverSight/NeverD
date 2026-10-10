#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace fs = std::filesystem;
using neverd::web::Error;
using neverd::web::Session;

class WebArtifacts : public ::testing::Test {
protected:
  fs::path Root;
  void SetUp() override {
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-web", Directory));
    Root = Directory.str().str();
  }
  void TearDown() override {
    std::error_code Error;
    fs::remove_all(Root, Error);
  }
  void write(const fs::path &Path, std::string_view Bytes) {
    fs::create_directories(Path.parent_path());
    std::ofstream Out(Path, std::ios::binary);
    Out.write(Bytes.data(), Bytes.size());
    ASSERT_TRUE(Out.good());
  }
  static llvm::json::Value parse(const std::string &Text) {
    auto Value = llvm::json::parse(Text);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return nullptr;
    }
    return std::move(*Value);
  }
  static std::string field(const llvm::json::Value &V, const char *Name) {
    return V.getAsObject()->getString(Name)->str();
  }
  std::string open(Session &S, const fs::path &Path) {
    const auto Preview = parse(S.preview(Path.string(), ""));
    return S.commit(field(Preview, "preview_token"));
  }
};

TEST(WebIdentity, LengthFramingAndDomainSeparation) {
  using neverd::web::identity;
  EXPECT_NE(identity("a", {"bc"}), identity("ab", {"c"}));
  EXPECT_NE(identity("a", {"b", "c"}), identity("a", {"bc"}));
  EXPECT_NE(identity("a", {"b"}), identity("b", {"b"}));
  EXPECT_EQ(neverd::web::sha256("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

#ifndef _WIN32
TEST_F(WebArtifacts, RelocationAndDirectoryOrderPreserveIdentity) {
  write(Root / "one/z.js", "same");
  write(Root / "one/a.js", "same");
  write(Root / "two/a.js", "same");
  write(Root / "two/z.js", "same");
  Session A, B;
  const auto MetaA = parse(open(A, Root / "one"));
  const auto MetaB = parse(open(B, Root / "two"));
  EXPECT_EQ(field(MetaA, "project_id"), field(MetaB, "project_id"));
  auto Page = parse(A.artifacts("1", 0, 512));
  auto *Items = Page.getAsObject()->getArray("items");
  ASSERT_EQ(Items->size(), 3);
  EXPECT_NE(field((*Items)[1], "artifact_id"),
            field((*Items)[2], "artifact_id"));
  EXPECT_EQ(field((*Items)[1], "blob_sha256"),
            field((*Items)[2], "blob_sha256"));
  EXPECT_EQ(field((*Items)[1], "parent_id"), field((*Items)[0], "artifact_id"));
  EXPECT_EQ(field(Page, "analysis_status"), "not_analyzed");
}

TEST_F(WebArtifacts, RootFilenameDoesNotBecomeContentIdentity) {
  write(Root / "one.js", "const x = 1;");
  write(Root / "two.js", "const x = 1;");
  Session A, B;
  EXPECT_EQ(field(parse(open(A, Root / "one.js")), "project_id"),
            field(parse(open(B, Root / "two.js")), "project_id"));
}

TEST_F(WebArtifacts, ChangedPreviewCannotReplacePublishedSnapshot) {
  write(Root / "input.js", "first");
  Session S;
  auto Meta = open(S, Root / "input.js");
  auto Preview = parse(S.preview((Root / "input.js").string(), ""));
  const auto Token = field(Preview, "preview_token");
  write(Root / "input.js", "other");
  EXPECT_THROW(S.commit(Token), Error);
  EXPECT_EQ(field(parse(S.metadata()), "project_id"),
            field(parse(Meta), "project_id"));
  EXPECT_EQ(field(parse(S.metadata()), "revision"), "1");
  EXPECT_THROW(S.commit(Token), Error);
}

TEST_F(WebArtifacts, PaginationAndRevisionAreIndependentOfCoverage) {
  write(Root / "a", "a");
  write(Root / "b", "b");
  Session S;
  open(S, Root);
  auto First = parse(S.artifacts("1", 0, 2));
  EXPECT_EQ(First.getAsObject()->getBoolean("page_complete"), false);
  EXPECT_EQ(First.getAsObject()->getInteger("next_offset"), 2);
  auto Last = parse(S.artifacts("1", 2, 2));
  EXPECT_EQ(Last.getAsObject()->getBoolean("page_complete"), true);
  EXPECT_EQ(field(Last, "analysis_status"), "not_analyzed");
  open(S, Root);
  EXPECT_THROW(S.artifacts("1", 2, 2), Error);
  EXPECT_THROW(S.artifacts("2", 0, 513), Error);
  EXPECT_THROW(S.artifacts("2", UINT64_MAX, 2), Error);
}

TEST_F(WebArtifacts, AggregateBudgetsApplyToManySmallFiles) {
  write(Root / "a", "1234");
  write(Root / "b", "5678");
  Session S;
  EXPECT_THROW(
      S.preview(Root.string(), R"({"schema_version":1,"max_input_bytes":7})"),
      Error);
  EXPECT_NO_THROW(
      S.preview(Root.string(), R"({"schema_version":1,"max_input_bytes":8})"));
  EXPECT_THROW(
      S.preview(Root.string(), R"({"schema_version":1,"max_entries":2})"),
      Error);
  EXPECT_THROW(
      S.preview(Root.string(), R"({"schema_version":1,"max_member_bytes":3})"),
      Error);
}

TEST_F(WebArtifacts, RejectsLinksSpecialFilesAndPortableCollisions) {
  write(Root / "outside", "sensitive");
  fs::create_directory(Root / "input");
  fs::create_symlink(Root / "outside", Root / "input/link");
  Session S;
  EXPECT_THROW(S.preview((Root / "input").string(), ""), Error);
  fs::remove(Root / "input/link");
  write(Root / "input/CON.js", "x");
  EXPECT_THROW(S.preview((Root / "input").string(), ""), Error);
  fs::remove(Root / "input/CON.js");
  write(Root / "input/a", "x");
  // Some host filesystems cannot represent the case-collision fixture.
  write(Root / "input/A", "y");
  if (std::distance(fs::directory_iterator(Root / "input"),
                    fs::directory_iterator()) == 2)
    EXPECT_THROW(S.preview((Root / "input").string(), ""), Error);
}

TEST_F(WebArtifacts, MetadataPolicyDoesNotExposeNamesOrContent) {
  const std::string Canary = "SECRET_CANARY_6fd90";
  write(Root / Canary / (Canary + ".js"), Canary);
  Session S;
  auto Preview = S.preview((Root / Canary).string(), "");
  EXPECT_EQ(Preview.find(Canary), Preview.npos);
  auto Committed = S.commit(field(parse(Preview), "preview_token"));
  EXPECT_EQ(Committed.find(Canary), Committed.npos);
  auto Page = S.artifacts("1", 0, 512);
  EXPECT_EQ(Page.find(Canary), Page.npos);
  EXPECT_EQ(S.metadata().find(Canary), std::string::npos);
}

TEST_F(WebArtifacts, RootLinkCannotBypassNoFollowWithTrailingComponents) {
  write(Root / "real/a", "private");
  fs::create_directory_symlink(Root / "real", Root / "alias");
  Session S;
  for (const auto Suffix : {"", "/", "///", "/.", "/./"})
    EXPECT_THROW(S.preview((Root / "alias").string() + Suffix, ""), Error);
  EXPECT_NO_THROW(S.preview((Root / "real").string() + "/", ""));
}

TEST_F(WebArtifacts, HardLinksCannotAliasFilesOutsideTheSelectedRoot) {
  write(Root / "outside", "private");
  fs::create_directory(Root / "input");
  fs::create_hard_link(Root / "outside", Root / "input/alias");
  Session S;
  EXPECT_THROW(S.preview((Root / "input").string(), ""), Error);
  EXPECT_THROW(S.preview((Root / "input/alias").string(), ""), Error);
}

TEST_F(WebArtifacts, FailedNewPreviewRevokesOldToken) {
  write(Root / "a", "a");
  Session S;
  auto Preview = parse(S.preview((Root / "a").string(), ""));
  EXPECT_THROW(S.preview((Root / "missing").string(), ""), Error);
  EXPECT_THROW(S.commit(field(Preview, "preview_token")), Error);
}

TEST_F(WebArtifacts, RejectsDuplicateUnknownAndUnboundedOptions) {
  write(Root / "a", "a");
  Session S;
  for (const auto *Options :
       {R"({"schema_version":1,"schema_version":1})",
        R"({"schema_version":1,"schema_\u0076ersion":1})",
        R"({"schema_version":1,"max_depth":-1})", R"({"schema_version":2})",
        R"({"schema_version":1,"eval":1})",
        R"({"schema_version":1,"max_depth":[]})",
        R"({"schema_version":1,"max_input_bytes":9223372036854775807})"})
    EXPECT_THROW(S.preview((Root / "a").string(), Options), Error) << Options;
}
#else
TEST_F(WebArtifacts, UnsupportedReaderFailsExplicitly) {
  Session S;
  EXPECT_THROW(S.preview(Root.string(), ""), Error);
}
#endif
} // namespace
