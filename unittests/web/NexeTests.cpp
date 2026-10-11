//===- NexeTests.cpp - Offline nexe reader contracts --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact byte coverage, aliases, hostile framing and bounded private indexes.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "Internal.h"
#include "JsonReader.h"
#include "NativeFixture.h"
#include "NexeFixture.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/Nexe.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace {
using namespace neverd::web;
using namespace neverd::web::nexe_test;
class WebNexe : public ::testing::Test {
protected:
  void SetUp() override {
    if (!nexeAvailable())
      GTEST_SKIP() << "Pinned portable namespace policy unavailable";
  }
  Artifact input(std::string_view B, std::string_view Occurrence = "one") {
    BlobStore Store(MaxNexeInputBytes);
    Store.append(B);
    Store.seal();
    Artifact A;
    A.Content = Store.whole();
    A.BlobHash = sha256(B);
    A.ID = identity("test-nexe-occurrence", {Occurrence, A.BlobHash});
    return A;
  }
  void refuse(std::string_view B, const char *Code) {
    try {
      (void)extractNexe(input(B), NexeProfile);
      FAIL() << Code;
    } catch (const Error &E) {
      EXPECT_STREQ(E.what(), Code);
    }
  }
};
TEST_F(WebNexe, ExactPartitionAliasesAndUnreferencedBytes) {
  const auto B = image(
      R"({"resources":{"./main.js":[0,3],"./alias.js":[0,3],"./empty":[1,0]}})",
      "abc!\0");
  const auto A = input(B);
  const auto E = extractNexe(A, NexeProfile);
  ASSERT_EQ(E.Regions.size(), 4U);
  uint64_t At = 0;
  for (const auto &R : E.Regions) {
    EXPECT_EQ(R.Offset, At);
    EXPECT_EQ(R.Content.read(0, R.Content.size()),
              B.substr(At, R.Content.size()));
    EXPECT_EQ(R.BlobHash, sha256(B.substr(At, R.Content.size())));
    At += R.Content.size();
  }
  EXPECT_EQ(At, B.size());
  ASSERT_EQ(E.Members.size(), 3U);
  EXPECT_EQ(E.Members[0].Path, "alias.js");
  EXPECT_EQ(E.Members[2].Path, "main.js");
  EXPECT_NE(E.Members[0].ID, E.Members[2].ID);
  EXPECT_EQ(E.Members[0].RangeID, E.Members[2].RangeID);
  EXPECT_EQ(E.Members[0].BlobHash,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(E.Members[1].BlobHash,
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(E.ReferencedBytes, 3U);
  EXPECT_EQ(E.ResourceSize - E.ReferencedBytes, 1U);
  EXPECT_EQ(E.UniqueMemberRanges, 2U);
  EXPECT_EQ(E.HashedBytes, B.size() + 3);
  EXPECT_EQ(E.ID, extractNexe(A, NexeProfile).ID);
}
TEST_F(WebNexe, EmptyResourcesAndBinaryMembersRemainExact) {
  const auto E =
      extractNexe(input(image(R"({"resources":{}})", "")), NexeProfile);
  EXPECT_TRUE(E.Members.empty());
  EXPECT_EQ(E.ResourceSize, 0U);
  const std::string Binary("\0\xff\xfe\1", 4);
  const auto F = extractNexe(input(bundle({{"x.bin", Binary}})), NexeProfile);
  EXPECT_EQ(F.Members[0].Content.read(0, 4), Binary);
  EXPECT_FALSE(F.Regions[0].selectable());
  EXPECT_TRUE(F.Regions[1].selectable());
  EXPECT_FALSE(F.Regions[2].selectable());
}
TEST_F(WebNexe, SeparateOccurrencesOwnSeparateNamespaces) {
  const auto B = bundle({{"package.json", "{}"}, {"main.js", "x();"}});
  const auto E = extractNexe(input(B, "one"), NexeProfile);
  const auto F = extractNexe(input(B, "two"), NexeProfile);
  EXPECT_NE(E.ID, F.ID);
  for (unsigned I = 0; I < E.Members.size(); ++I) {
    EXPECT_EQ(E.Members[I].BlobHash, F.Members[I].BlobHash);
    EXPECT_NE(E.Members[I].ID, F.Members[I].ID);
  }
  const auto S = nexeNamespace(E);
  ASSERT_EQ(S.Artifacts.size(), 3U);
  EXPECT_TRUE(S.Artifacts[0].Directory);
  EXPECT_EQ(S.Artifacts[1].ParentID, E.ID);
  EXPECT_EQ(S.Artifacts[1].MemberPath, "main.js");
}
TEST_F(WebNexe, OpaquePrefixesDoNotRestrictContainerArithmeticToX64) {
  using namespace neverd::web::test;
  auto PEArm = nativePE();
  nativePut(PEArm, 132, 0xaa64, 2);
  auto MachOArm = nativeMachO();
  nativePut(MachOArm, 4, 0x100000c, 4);
  // These inert header fixtures test prefix preservation, not native loading,
  // architecture authentication or ARM64 producer/runtime qualification.
  for (const auto &Prefix : {nativeELF(), nativeELF(true), nativePE(), PEArm,
                             nativeMachO(), MachOArm}) {
    const auto E = extractNexe(
        input(bundle({{"main.js", "var n=42;"}}, Prefix)), NexeProfile);
    EXPECT_EQ(E.Regions[0].Content.read(0, Prefix.size()), Prefix);
    EXPECT_EQ(E.Members[0].Content.read(0, 9), "var n=42;");
  }
}
TEST_F(WebNexe, FooterTruncationsTrailingDataAndHashMismatchRefuse) {
  const auto B = bundle({{"x.js", "var x=1;"}});
  for (uint64_t I = 0; I < B.size(); ++I)
    EXPECT_THROW(
        extractNexe(input(std::string_view(B).substr(0, I)), NexeProfile),
        Error)
        << I;
  refuse(B + "signature", "nexe_footer_not_found");
  auto A = input(B);
  A.BlobHash = std::string(64, '0');
  EXPECT_THROW(extractNexe(A, NexeProfile), Error);
  EXPECT_THROW(extractNexe(A, "nexe-latest"), Error);
}
TEST_F(WebNexe, Binary64LengthsAreCheckedBeforeConversions) {
  const auto Good = image(R"({"resources":{}})", "");
  for (const auto Bits :
       {UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000000000000),
        UINT64_C(0xfff0000000000000), UINT64_C(0x8000000000000000),
        UINT64_C(0xbff0000000000000), UINT64_C(0x3fe0000000000000),
        UINT64_C(0x7fefffffffffffff)})
    for (const auto Back : {8U, 16U}) {
      auto B = Good;
      put(B, B.size() - Back, Bits);
      refuse(B, "nexe_invalid_length");
    }
  auto B = Good;
  put(B, B.size() - 16, 0);
  refuse(B, "nexe_bootstrap_budget_exceeded");
  B = Good;
  put(B, B.size() - 8, std::bit_cast<uint64_t>(double(B.size() - 32)));
  refuse(B, "nexe_invalid_partition");
  refuse(image(R"({"resources":{}})", "", ""), "nexe_invalid_partition");
}
TEST_F(WebNexe, FirstSentinelMatchesTheExactUpstreamSearchWindow) {
  auto B = image(R"({"resources":{}})", std::string(20000, 'x'));
  const auto Start = B.size() - 16000;
  B.replace(Start, 16, "<nexe~~sentinel>");
  refuse(B, "nexe_ambiguous_sentinel");
  B.replace(Start, 16, std::string(16, 'x'));
  B.replace(Start - 1, 16, "<nexe~~sentinel>");
  EXPECT_NO_THROW(extractNexe(input(B), NexeProfile));
}
TEST_F(WebNexe, OnlyTheExactFirstShimAndStrictJSONAreAccepted) {
  refuse(envelope(";" + bootstrap(R"({"resources":{}})"), ""),
         "nexe_unsupported_bootstrap");
  refuse(image(R"({"resources":{},"extra":1})", ""), "nexe_invalid_index");
  refuse(image(R"({"resources":[]})", ""), "nexe_invalid_index");
  for (const auto *Text :
       {R"({"resources":{"./x":[0,0],"./\u0078":[0,0]}})",
        R"({"resources":{"./x":[0,0],}})", R"({"resources":{"./x":[NaN,0]}})",
        R"({"resources":{"./\ud800":[0,0]}})"})
    EXPECT_THROW(extractNexe(input(image(Text, "")), NexeProfile), Error);
}
TEST_F(WebNexe, PortablePathsAndNamespaceCollisionsRefuse) {
  for (const auto *Path :
       {"x", "./../x", "./a/../../x", "./C:/x", "./CON", "./a//x", "./a/",
        "./a\\\\x", "./a\\u0000x", "/x", "././x", "./lpt¹", "./a. "}) {
    const auto JSON = "{\"resources\":{\"" + std::string(Path) + "\":[0,0]}}";
    EXPECT_THROW(extractNexe(input(image(JSON, "")), NexeProfile), Error)
        << Path;
  }
  for (const auto *Text : {R"({"resources":{"./x":[0,0],"./X":[0,0]}})",
                           R"({"resources":{"./x":[0,0],"./x/a":[0,0]}})",
                           R"({"resources":{"./é":[0,0],"./e\u0301":[0,0]}})"})
    EXPECT_THROW(extractNexe(input(image(Text, "")), NexeProfile), Error);
}
TEST_F(WebNexe, MemberRangesRejectPartialOverlapAndUnsupportedValues) {
  refuse(image(R"({"resources":{"./a":[0,3],"./b":[2,2]}})", "abcd"),
         "nexe_partial_member_overlap");
  refuse(image(R"({"resources":{"./a":[0,4],"./b":[1,2]}})", "abcd"),
         "nexe_partial_member_overlap");
  for (const auto *Range : {"[0,5]", "[-1,0]", "[0,-1]", "[0.5,0]", "[0,0,0]",
                            "[0]", "[\"0\",0]", "[5,0]", "[1e99,0]"})
    refuse(
        image("{\"resources\":{\"./x\":" + std::string(Range) + "}}", "abcd"),
        "nexe_invalid_member_range");
}
TEST_F(WebNexe, AliasCountDoesNotMultiplyResourceReads) {
  std::string JSON = "{\"resources\":{";
  constexpr uint64_t Bytes = 2 * 1024 * 1024;
  for (unsigned I = 0; I < MaxNexeMembers; ++I) {
    if (I)
      JSON += ',';
    JSON +=
        "\"./a" + std::to_string(I) + "\":[0," + std::to_string(Bytes) + "]";
  }
  const auto B = image(JSON + "}}", std::string(Bytes, 'x'));
  const auto E = extractNexe(input(B), NexeProfile);
  EXPECT_EQ(E.Members.size(), MaxNexeMembers);
  EXPECT_EQ(E.UniqueMemberRanges, 1U);
  EXPECT_EQ(E.ReferencedBytes, Bytes);
  EXPECT_EQ(E.HashedBytes, B.size() + Bytes);
  std::set<std::string> IDs;
  for (const auto &M : E.Members)
    IDs.insert(M.ID);
  EXPECT_EQ(IDs.size(), MaxNexeMembers);
}
TEST_F(WebNexe, BootstrapIndexMemberAndComparisonBudgetsRefuse) {
  refuse(envelope(std::string(MaxNexeBootstrapBytes + 1, 'x'), ""),
         "nexe_bootstrap_budget_exceeded");
  refuse(image(std::string(MaxNexeIndexBytes + 1, ' '), ""),
         "nexe_index_budget_exceeded");
  std::vector<std::pair<std::string, std::string>> Files;
  for (unsigned I = 0; I <= MaxNexeMembers; ++I)
    Files.emplace_back("x" + std::to_string(I), "");
  refuse(bundle(Files), "nexe_member_budget_exceeded");
  Files.clear();
  for (unsigned I = 0; I < 700; ++I)
    Files.emplace_back(std::string(600, 'a') + std::to_string(I), "");
  // Component size is bounded by path admission. Use nested short components
  // for long common prefixes without exceeding the component policy.
  for (auto &[Path, Bytes] : Files)
    for (unsigned I = 100; I < 600; I += 100)
      Path[I] = '/';
  refuse(bundle(Files), "nexe_work_budget_exceeded");
  Files.clear();
  std::string Parent;
  for (unsigned I = 0; I < 60; ++I)
    Parent += std::string(63, 'a') + '/';
  for (unsigned I = 0; I < 40; ++I)
    Files.emplace_back(Parent + std::to_string(I), "");
  refuse(bundle(Files), "nexe_metadata_budget_exceeded");
}
TEST_F(WebNexe, OptionalPinnedProducerImagesPreserveIndependentInputs) {
  const auto *Root = std::getenv("NEVERD_NEXE_4RC7_CORPUS");
  if (!Root)
    GTEST_SKIP()
        << "Set NEVERD_NEXE_4RC7_CORPUS to the pinned producer outputs";
  std::ifstream F(std::filesystem::path(NEVERD_WEB_FIXTURE_DIR) /
                  "nexe/manifest.json");
  ASSERT_TRUE(F.good());
  const std::string Text{std::istreambuf_iterator<char>(F), {}};
  const auto V = parseBoundedJSON(Text);
  const auto &Manifest = *V.getAsObject();
  for (const auto &Image : *Manifest.getArray("images")) {
    const auto &C = *Image.getAsObject();
    const auto Name = C.getString("name")->str();
    SCOPED_TRACE(Name);
    const auto A =
        capture((std::filesystem::path(Root) / Name).string(), Limits{})
            .Artifacts.at(0);
    ASSERT_EQ(A.BlobHash, C.getString("sha256"));
    ASSERT_EQ(std::to_string(A.Content.size()), C.getString("size"));
    const auto E = extractNexe(A, NexeProfile);
    ASSERT_EQ(E.Regions.size(), 4U);
    EXPECT_EQ(E.Regions[0].BlobHash, C.getString("base_sha256"));
    EXPECT_EQ(std::to_string(E.Regions[0].Content.size()),
              C.getString("base_size"));
    EXPECT_EQ(E.Regions[1].BlobHash, Manifest.getString("bootstrap_sha256"));
    EXPECT_EQ(std::to_string(E.Regions[1].Content.size()),
              Manifest.getString("bootstrap_size"));
    EXPECT_EQ(E.Regions[2].BlobHash, Manifest.getString("resource_sha256"));
    EXPECT_EQ(std::to_string(E.ResourceSize),
              Manifest.getString("resource_size"));
    const auto &Inputs = *Manifest.getArray("inputs");
    ASSERT_EQ(E.Members.size(), Inputs.size());
    uint64_t At = E.ResourceOffset;
    for (size_t I = 0; I < Inputs.size(); ++I) {
      const auto &Input = *Inputs[I].getAsObject();
      const auto &M = E.Members[I];
      EXPECT_EQ(M.Path, Input.getString("name"));
      EXPECT_EQ(M.BlobHash, Input.getString("sha256"));
      EXPECT_EQ(M.Offset, At);
      EXPECT_EQ(M.Content.read(0, M.Content.size()), Input.getString("bytes"));
      At += M.Content.size();
    }
    EXPECT_EQ(E.ReferencedBytes, E.ResourceSize);
    EXPECT_EQ(E.HashedBytes, A.Content.size() + E.ResourceSize);
  }
}
} // namespace
