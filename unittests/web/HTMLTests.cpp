//===- HTMLTests.cpp - Captured HTML script evidence tests -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Captured HTML script evidence tests.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/HTML.h"
#include "neverd/web/Session.h"

#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

namespace {
using namespace neverd::web;
std::string decode(std::string_view Text) {
  uint64_t Steps = 0, Bytes = 0;
  return decodeHTMLAttribute(Text, Steps, Bytes);
}
TEST(WebHTML, EveryPinnedReferenceMatchesIndependentCodePoints) {
  const auto File = llvm::MemoryBuffer::getFile(
      std::string(NEVERD_WEB_FIXTURE_DIR) + "/html/entities.json");
  ASSERT_TRUE(File);
  const auto Raw = (*File)->getBuffer();
  EXPECT_EQ(Raw.size(), 145897u);
  EXPECT_EQ(sha256(Raw.str()),
            "d741d877ac77c4194c4ad526b5b4a19aef8dfe411ab840a466891cdbb9f362e6");
  auto Parsed = llvm::json::parse(Raw);
  if (!Parsed)
    FAIL() << llvm::toString(Parsed.takeError());
  ASSERT_NE(Parsed->getAsObject(), nullptr);
  ASSERT_EQ(Parsed->getAsObject()->size(), 2231u);
  for (const auto &Pair : *Parsed->getAsObject()) {
    const auto *Record = Pair.second.getAsObject();
    ASSERT_NE(Record, nullptr);
    const auto Text = Record->getString("characters");
    const auto *Points = Record->getArray("codepoints");
    ASSERT_TRUE(Text);
    ASSERT_NE(Points, nullptr);
    EXPECT_EQ(decode(Pair.first.str()), Text->str()) << Pair.first.str();
    const auto *Cursor = reinterpret_cast<const llvm::UTF8 *>(Text->data());
    const auto *End = Cursor + Text->size();
    for (const auto &Point : *Points) {
      llvm::UTF32 Scalar = 0;
      ASSERT_EQ(llvm::convertUTF8Sequence(&Cursor, End, &Scalar,
                                          llvm::strictConversion),
                llvm::conversionOK);
      EXPECT_EQ(Scalar, Point.getAsInteger());
    }
    EXPECT_EQ(Cursor, End);
  }
}
TEST(WebHTML, ReferencesRespectAttributeAmbiguityAndNumericReplacement) {
  EXPECT_EQ(decode("&notit; &amp=1 &ampx &unknown; &amp;=1"),
            "&notit; &amp=1 &ampx &unknown; &=1");
  EXPECT_EQ(decode("&amp! &NotEqualTilde; &#65 &#x41; &#X1F600;"),
            "&! \xe2\x89\x82\xcc\xb8 A A \xf0\x9f\x98\x80");
  EXPECT_EQ(decode("&#0;&#xD800;&#1114112;&#999999999999999999999999;"),
            std::string("\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"));
  EXPECT_EQ(decode("&#128; &#x9f; &#x; &#;"), "\xe2\x82\xac \xc5\xb8 &#x; &#;");
  EXPECT_EQ(decode("a\r\nb\rc"), "a\nb\nc");
  uint64_t Steps = MaxHTMLSteps, Bytes = 0;
  EXPECT_THROW(decodeHTMLAttribute("x", Steps, Bytes), Error);
  Steps = 0;
  Bytes = MaxHTMLDecodedBytes;
  EXPECT_THROW(decodeHTMLAttribute("x", Steps, Bytes), Error);
  EXPECT_THROW(decode(std::string(MaxHTMLAttributeBytes + 1, 'a')), Error);
}
TEST(WebHTML, TagsKeepOriginalSpansAndFirstDuplicateAttributes) {
  const std::string Text =
      "<SCRIPT SRC='x&amp;.js' src=ignored TYPE=MoDuLe async defer nomodule "
      "integrity='secret' crossorigin>ignored</sCrIpT ><script>const "
      "x='&amp;';\r\n</script>";
  const auto A = inspectHTML("document", Text);
  ASSERT_EQ(A.Scripts.size(), 2u);
  const auto &S = A.Scripts[0];
  EXPECT_EQ(S.SourceType, "module");
  EXPECT_EQ(S.Reference, "x&.js");
  EXPECT_EQ(Text.substr(S.ReferenceStart, S.ReferenceLength), "x&amp;.js");
  EXPECT_TRUE(S.Async && S.Defer && S.NoModule && S.Integrity &&
              S.CrossOrigin && S.DuplicateAttributes);
  EXPECT_EQ(Text.substr(S.BodyStart, S.BodyEnd - S.BodyStart), "ignored");
  EXPECT_EQ(S.BodyStatus, "external_script_body_ignored");
  const auto &Inline = A.Scripts[1];
  EXPECT_EQ(Text.substr(Inline.BodyStart, Inline.BodyEnd - Inline.BodyStart),
            "const x='&amp;';\r\n");
  EXPECT_EQ(Inline.BodyHash, sha256("const x='&amp;';\r\n"));
  EXPECT_FALSE(Inline.InlineArtifactID.empty());
  EXPECT_EQ(A.ID, inspectHTML("document", Text).ID);
  EXPECT_NE(A.ID, inspectHTML("other-document", Text).ID);
}
TEST(WebHTML, TypeRulesDoNotTreatWhitespaceOrMIMEParametersAsModules) {
  const auto A = inspectHTML(
      "d", "<script type=' text/javascript '>x</script><script type=' module "
           "'>x</script><script "
           "type='text/javascript;charset=utf-8'>x</script><script "
           "language=JavaScript>x</script><script language=' JavaScript "
           "'>x</script><script type='' language=vbscript>x</script><script "
           "type=importmap>{}</script><script "
           "type=speculationrules>{}</script><script type=' '>x</script>");
  ASSERT_EQ(A.Scripts.size(), 9u);
  const std::vector<std::string> Kinds{
      "classic", "data_block", "data_block",       "classic",   "data_block",
      "classic", "importmap",  "speculationrules", "data_block"};
  for (size_t I = 0; I < Kinds.size(); ++I)
    EXPECT_EQ(A.Scripts[I].Kind, Kinds[I]);
}
TEST(WebHTML, CommentsTextContainersTemplatesAndForeignContentStayDistinct) {
  const auto Bogus =
      inspectHTML("d", "</1<script>bad</script><script>good</script>");
  ASSERT_EQ(Bogus.Scripts.size(), 1u);
  EXPECT_EQ(Bogus.Scripts[0].BodyHash, sha256("good"));
  const auto A = inspectHTML(
      "d",
      "<!--<script>bad</script>--!><style><script>bad</script></"
      "style><textarea><script>bad</script></textarea><title><script>bad</"
      "script></title><noscript><script>bad</script></"
      "noscript><template><script>template</script><base "
      "href=ignored/></template><svg><![CDATA[<script>bad</script>]]><script/"
      "></svg><script>good</script><plaintext><script>bad</script>");
  ASSERT_EQ(A.Scripts.size(), 3u);
  EXPECT_EQ(A.Scripts[0].Context, "template_content");
  EXPECT_TRUE(A.Scripts[0].InlineArtifactID.empty());
  EXPECT_EQ(A.Scripts[1].Context, "foreign_content_unverified");
  EXPECT_TRUE(A.Scripts[1].InlineArtifactID.empty());
  EXPECT_EQ(A.Scripts[2].BodyStatus, "raw_inline_source_candidate");
  ASSERT_EQ(A.Bases.size(), 1u);
  EXPECT_FALSE(A.Bases[0].Selected);
}
TEST(WebHTML, ScriptEscapeStatesAndLiteralClosingTagsOwnTheByteBoundary) {
  const std::string Text = "<script><!--<script>double</script>escaped--></"
                           "script><script>let a='</script>'; tail";
  const auto A = inspectHTML("d", Text);
  ASSERT_EQ(A.Scripts.size(), 2u);
  EXPECT_EQ(Text.substr(A.Scripts[0].BodyStart,
                        A.Scripts[0].BodyEnd - A.Scripts[0].BodyStart),
            "<!--<script>double</script>escaped-->");
  EXPECT_EQ(Text.substr(A.Scripts[1].BodyStart,
                        A.Scripts[1].BodyEnd - A.Scripts[1].BodyStart),
            "let a='");
  for (const auto Prefix : {"<!-->", "<!--->", "<!---->", "<!-- -->"}) {
    const auto B = inspectHTML("d", std::string("<script>") + Prefix +
                                        "<script>data</script>");
    ASSERT_EQ(B.Scripts.size(), 1u);
    EXPECT_TRUE(B.Scripts[0].Closed) << Prefix;
  }
  const auto B = inspectHTML("d", "<script><!--<script>double</script>");
  ASSERT_EQ(B.Scripts.size(), 1u);
  EXPECT_FALSE(B.Scripts[0].Closed);
  EXPECT_TRUE(B.Scripts[0].InlineArtifactID.empty());
}
TEST(WebHTML, QualifiedDoctypesDoNotHideMarkupInMalformedIdentifiers) {
  for (const auto Prefix :
       {"<!DOCTYPE html>",
        "<!doctype html PUBLIC '-//W3C//DTD HTML 4.01//EN' 'local.dtd'>",
        "<!DOCTYPE html SYSTEM 'about:legacy-compat'>"}) {
    const auto A =
        inspectHTML("d", std::string(Prefix) + "<script>0;</script>");
    EXPECT_EQ(A.Status, "partial");
    EXPECT_EQ(A.Scripts.size(), 1u);
  }
  for (const auto Prefix : {"<!doctype html 'x'>",
                            "<!doctype html SYSTEM 'x><script>bad</script>'>",
                            "<!doctype html bogus>", "<!doctypeX 'quoted>'>"}) {
    const auto A =
        inspectHTML("d", std::string(Prefix) + "<script>0;</script>");
    EXPECT_EQ(A.Status, "unavailable");
    EXPECT_TRUE(A.Scripts.empty());
  }
}
TEST(WebHTML, UnsupportedInputAndBudgetFailuresPublishNoPartialSources) {
  for (const auto Text :
       {"<script>x</script><a x='", "<!-- missing",
        "<meta charset=windows-1252><script>x</script>",
        "<meta http-equiv=content-type><script>x</script>", "<a x='1'y='2'>"}) {
    const auto A = inspectHTML("d", Text);
    EXPECT_EQ(A.Status, "unavailable") << Text;
    EXPECT_TRUE(A.Scripts.empty());
  }
  EXPECT_EQ(inspectHTML("d", std::string("x\0y", 3)).Status, "unavailable");
  EXPECT_EQ(inspectHTML("d", "\xff").Status, "unavailable");
  const auto Huge =
      inspectHTML("d", "<!--" + std::string(MaxHTMLBytes - 7, 'x') + "-->");
  EXPECT_EQ(Huge.Status, "budget_exceeded");
  EXPECT_TRUE(Huge.Scripts.empty());
  std::string Many;
  for (uint64_t I = 0; I <= MaxHTMLRecords; ++I)
    Many += "<script></script>";
  const auto Count = inspectHTML("d", Many);
  EXPECT_EQ(Count.Status, "budget_exceeded");
  EXPECT_TRUE(Count.Scripts.empty());
  EXPECT_THROW(inspectHTML("d", std::string(MaxHTMLBytes + 1, 'x')), Error);
}
TEST(WebHTML, LocalURLReferencesAreBoundedExactAndNeverHostLookups) {
  auto R = resolveHTMLLocalURL("app/ui/index.html", false,
                               " ../%E4%B8%AD.js?private#token ");
  EXPECT_EQ(R.Path, "app/中.js");
  EXPECT_EQ(R.Status, "local_url_candidate");
  EXPECT_TRUE(R.Query && R.Fragment);
  EXPECT_FALSE(R.Directory);
  R = resolveHTMLLocalURL("app/ui/index.html", false, "../assets/");
  EXPECT_EQ(R.Path, "app/assets");
  EXPECT_TRUE(R.Directory);
  EXPECT_EQ(resolveHTMLLocalURL(R.Path, R.Directory, "a.js").Path,
            "app/assets/a.js");
  EXPECT_EQ(resolveHTMLLocalURL("app/index.html", false, "%2e%2E/x.js").Path,
            "x.js");
  EXPECT_EQ(resolveHTMLLocalURL("app/index.html", false, "?x#f").Path,
            "app/index.html");
  EXPECT_EQ(resolveHTMLLocalURL("app/index.html", false, "../../x").Status,
            "outside_snapshot_root");
  for (const auto Ref :
       {"/x", "//host/x", "https://x", "file:x", "C:x", "x\\a", "%2froot",
        "x%5Cy", "x%00y", "%C0%AF", "x%", "x%2", "x%2G", "a//b", "CON", "x\ny"})
    EXPECT_NE(resolveHTMLLocalURL("app/index.html", false, Ref).Status,
              "local_url_candidate")
        << Ref;
  EXPECT_NE(resolveHTMLLocalURL("../x", false, "x").Status,
            "local_url_candidate");
}
Snapshot snapshot(std::string_view Text) {
  Snapshot S;
  S.ID = "namespace";
  S.Artifacts.push_back(Artifact{"root", {}, {}, {}, "directory", {}, true});
  S.Artifacts.push_back(
      Artifact{"doc", sha256(Text), "root", "app/index.html"});
  S.Artifacts.push_back(Artifact{"local", {}, "root", "app/a.js"});
  S.Artifacts.push_back(Artifact{"base-local", {}, "root", "assets/a.js"});
  return S;
}
TEST(WebHTML, LinksUseThePrecedingFirstBaseAndCapturedOccurrence) {
  const std::string Text =
      "<script src=a.js></script><base href=../assets/><base "
      "href=https://ignored/><script src='a.js?key#fragment'></script><script "
      "type=module>import './x.js'</script><script "
      "src=missing></script><script src=''></script>";
  const auto A = inspectHTML("doc", Text);
  auto S = snapshot(Text);
  const auto L = linkHTMLScripts(A, S);
  ASSERT_EQ(L.Scripts.size(), 5u);
  EXPECT_EQ(L.Scripts[0].ArtifactID, "local");
  EXPECT_EQ(L.Scripts[1].ArtifactID, "base-local");
  EXPECT_TRUE(L.Scripts[1].Query && L.Scripts[1].Fragment);
  EXPECT_EQ(L.Scripts[2].ArtifactID, A.Scripts[2].InlineArtifactID);
  EXPECT_EQ(L.Scripts[2].Base.Path, "assets");
  EXPECT_TRUE(L.Scripts[2].Base.Directory);
  EXPECT_EQ(L.Scripts[3].Status, "not_in_snapshot");
  EXPECT_EQ(L.Scripts[4].Status, "empty_source_reference");
  S.Artifacts[3].Directory = true;
  EXPECT_EQ(linkHTMLScripts(A, S).Scripts[1].Status, "directory_target");
  S.Artifacts[1].BlobHash = "wrong";
  EXPECT_THROW(linkHTMLScripts(A, S), Error);
}
TEST(WebHTML, RemoteBasesAndMissingNamespacesNeverLinkByBasename) {
  const auto Unsafe = "<script src='./&#10;bad?query#fragment'></script>";
  const auto Invalid =
      linkHTMLScripts(inspectHTML("doc", Unsafe), snapshot(Unsafe));
  ASSERT_EQ(Invalid.Scripts.size(), 1u);
  EXPECT_EQ(Invalid.Scripts[0].Status, "unsupported_local_url");
  EXPECT_TRUE(Invalid.Scripts[0].Query && Invalid.Scripts[0].Fragment);
  const auto Text = "<base href=https://private.invalid/><script "
                    "src=a.js></script><script>local</script>";
  const auto A = inspectHTML("doc", Text);
  auto S = snapshot(Text);
  auto L = linkHTMLScripts(A, S);
  ASSERT_EQ(L.Scripts.size(), 2u);
  EXPECT_EQ(L.Scripts[0].Status, "base_url_unavailable");
  EXPECT_FALSE(L.Scripts[1].ArtifactID.empty());
  S.Artifacts.erase(S.Artifacts.begin());
  S.Artifacts[0].MemberPath.clear();
  L = linkHTMLScripts(A, S);
  EXPECT_EQ(L.Scripts[0].BaseStatus, "directory_origin_unavailable");
}
TEST(WebHTML, RepeatedInlineBaseContextsChargeTheAggregateLinkBudget) {
  std::string Text;
  for (unsigned I = 0; I < 1200; ++I)
    Text += "<script>0;</script>";
  const auto A = inspectHTML("doc", Text);
  ASSERT_EQ(A.Scripts.size(), 1200u);
  auto S = snapshot(Text);
  std::string Directory;
  for (unsigned I = 0; I < 15; ++I)
    Directory += std::string(250, 'x') + '/';
  S.Artifacts[1].MemberPath = Directory + "index.html";
  const auto L = linkHTMLScripts(A, S);
  EXPECT_EQ(L.Status, "budget_exceeded");
  EXPECT_TRUE(L.Scripts.empty());
  EXPECT_LE(L.Steps, MaxHTMLLinkSteps);
  EXPECT_EQ(A.Scripts[0].BodyStatus, "raw_inline_source_candidate");
}
} // namespace
