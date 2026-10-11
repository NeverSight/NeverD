//===- JsonReaderTests.cpp - Bounded JSON admission tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded JSON admission tests.
///
//===----------------------------------------------------------------------===//

#include "JsonReader.h"
#include "gtest/gtest.h"

#include "neverd/web/Session.h"

namespace {
using namespace neverd::web;

TEST(WebJson, DecodedDuplicateKeysCannotReplaceEvidence) {
  for (const auto Text : {R"({"a":1,"a":2})", R"({"a":1,"\u0061":2})",
                          R"({"outer":{"key":1,"key":2}})"}) {
    try {
      (void)parseBoundedJSON(Text);
      FAIL() << "Duplicate key was admitted";
    } catch (const Error &E) {
      EXPECT_STREQ(E.what(), "duplicate_json_key");
    }
  }
  EXPECT_NO_THROW(parseBoundedJSON(R"([{"key":1},{"key":2}])"));
}

TEST(WebJson, BudgetsApplyBeforeDomAllocationIncludingObjectKeys) {
  JsonLimits Limits;
  Limits.MaxDepth = 2;
  EXPECT_NO_THROW(parseBoundedJSON("[[1]]", Limits));
  EXPECT_THROW(parseBoundedJSON("[[[]]]", Limits), Error);
  Limits.MaxNodes = 3;
  EXPECT_NO_THROW(parseBoundedJSON(R"({"a":1})", Limits));
  EXPECT_THROW(parseBoundedJSON("[1,2,3]", Limits), Error);
  EXPECT_THROW(parseBoundedJSON(R"({"a":1,"b":2})", Limits), Error);
  Limits.MaxBytes = 5;
  EXPECT_THROW(parseBoundedJSON("[1,2,3]", Limits), Error);
  Limits.MaxBytes = 100;
  Limits.MaxStringBytes = 4;
  EXPECT_NO_THROW(parseBoundedJSON(R"("abc")", Limits));
  EXPECT_NO_THROW(parseBoundedJSON(R"("abcd")", Limits));
  EXPECT_THROW(parseBoundedJSON(R"("abcde")", Limits), Error);
}

TEST(WebJson, MalformedInputsReturnCodesWithoutSensitiveText) {
  for (const auto Text :
       {"", "[1,]", R"({"a":1,})", "true false", R"({"a" 1})",
        R"({"a":"\uD800"})", R"({"a":"\q"})", "[01]", "[+1]", "[.2]", "[1.]",
        "[NaN]", "[Infinity]", "SECRET_CANARY[[]]"}) {
    SCOPED_TRACE(Text);
    try {
      (void)parseBoundedJSON(Text);
      FAIL() << "Malformed JSON was admitted";
    } catch (const Error &E) {
      EXPECT_EQ(std::string(E.what()).find("SECRET_CANARY"), std::string::npos);
    }
  }
  EXPECT_THROW(parseBoundedJSON(std::string("\xff", 1)), Error);
}

TEST(WebJson, SurrogatesAreNotSilentlyReplacedOrMergedWithOtherKeys) {
  for (const auto Text : {R"("\uDFFF")", R"("\uD800a")", R"("\uD800\uD800")",
                          R"("\uDC00\uD800")", R"({"\uD800":1,"\uFFFD":2})"})
    EXPECT_THROW(parseBoundedJSON(Text), Error) << Text;
  EXPECT_EQ(parseBoundedJSON(R"("\uD83D\uDE00")").getAsString(),
            "\xf0\x9f\x98\x80");
  EXPECT_NO_THROW(parseBoundedJSON(R"("\uD7FF\uE000")"));
  EXPECT_THROW(parseBoundedJSON(R"({"\uD83D\uDE00":1,"😀":2})"), Error);
}

TEST(WebJson, JsonPunctuationInsideStringsIsNotStructure) {
  const auto Value = parseBoundedJSON(
      R"({"punctuation":"[{\"}]}","values":[null,true,-1.25e2]})");
  ASSERT_NE(Value.getAsObject(), nullptr);
  EXPECT_EQ(Value.getAsObject()->getString("punctuation"), "[{\"}]}");
  EXPECT_EQ(Value.getAsObject()->getArray("values")->size(), 3);
}
} // namespace
