//===- SymParseLexerTests.cpp - Symbolic token boundary contracts
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check maximal munch, progress, and diagnostics for symbolic punctuators.

#include "../../lib/symbolic/parse/SymParseDetail.h"
#include "gtest/gtest.h"

#include <string>
#include <string_view>

using namespace neverd::symbolic;

namespace {

TEST(SymParseLexer, RepeatedLongCallsCheckTheFullTextAndItsNesting) {
  const std::string Prefix = "ror(" + std::string(80, ' ');
  const std::string A = Prefix + "rol(x,2),3)";
  const std::string B = Prefix + "rol(y,2),4)";
  SymParseOptions Options;
  Options.MaxNesting = 2;
  for (const auto &Fields : {A + "*" + B, B + "*" + A}) {
    const auto Product = "x*y*z*" + Fields + "*" + A;
    for (const auto &Text : {Product, Product + " * (" + A + ")",
                             Product + "*" + Prefix + "rol(x,2),!]",
                             Product + "*" + A.substr(0, A.size() - 1)}) {
      SymContext Plain, Reused;
      const auto Expected =
          detail::Parser(Plain, Text, 16, Options, false).run();
      const auto Actual = detail::Parser(Reused, Text, 16, Options).run();
      EXPECT_EQ(Actual.ok(), Text == Product);
      ASSERT_EQ(Expected.ok(), Actual.ok());
      if (Actual.ok())
        EXPECT_EQ(Plain.toString(Expected.Root), Reused.toString(Actual.Root));
      else {
        EXPECT_EQ(Expected.Error, Actual.Error);
        EXPECT_EQ(Expected.ErrorOffset, Actual.ErrorOffset);
      }
    }
  }
}

TEST(SymParseLexer, EveryPunctuationPairKeepsMaximalMunchAndOffsets) {
  const std::string Punctuation = "!\"#$%&'()*+,-./:;<=>?@[\\]^`{|}~";
  const std::string_view Pairs[] = {
      "<<", ">>", "<=", ">=", "==", "!=", "&&", "||"};
  for (char A : Punctuation)
    for (char B : Punctuation) {
      std::string Pair{A, B};
      const bool Combined = llvm::is_contained(Pairs, Pair);
      for (bool AtEnd : {false, true}) {
        std::string Text = " \t" + Pair + (AtEnd ? "" : " x");
        detail::Lexer Lex(Text);
        auto First = Lex.take();
        ASSERT_EQ(First.K, detail::Token::Punct);
        EXPECT_EQ(First.Offset, 2u);
        EXPECT_EQ(First.Text.str(), Combined ? Pair : Pair.substr(0, 1));
        if (!Combined) {
          auto Second = Lex.take();
          EXPECT_EQ(Second.K, detail::Token::Punct);
          EXPECT_EQ(Second.Offset, 3u);
          EXPECT_EQ(Second.Text.str(), Pair.substr(1));
        }
        if (!AtEnd) {
          auto Last = Lex.take();
          EXPECT_EQ(Last.K, detail::Token::Ident);
          EXPECT_EQ(Last.Text, "x");
          EXPECT_EQ(Last.Offset, 5u);
        }
        EXPECT_EQ(Lex.take().K, detail::Token::End);
        EXPECT_EQ(Lex.take().Offset, Text.size());
      }
    }
}

TEST(SymParseLexer, UnknownBytesAdvanceAndKeepParserDiagnostics) {
  for (unsigned Byte : {0u, 0x80u, 0xffu}) {
    std::string Text(1, static_cast<char>(Byte));
    detail::Lexer Lex(Text);
    auto Token = Lex.take();
    EXPECT_EQ(Token.K, detail::Token::Punct);
    EXPECT_EQ(Token.Text.size(), 1u);
    EXPECT_EQ(Token.Offset, 0u);
    EXPECT_EQ(Lex.take().K, detail::Token::End);
    EXPECT_EQ(Lex.take().Offset, 1u);
    SymContext C;
    auto Parsed = parseSymExpr(C, Text, 64);
    EXPECT_FALSE(Parsed.ok());
    EXPECT_EQ(Parsed.ErrorOffset, 0u);
  }
  for (const char *Text : {"x === y", "x !== y", "x &&& y", "x <== y"}) {
    SymContext C;
    auto Parsed = parseSymExpr(C, Text, 64);
    EXPECT_FALSE(Parsed.ok());
    EXPECT_EQ(Parsed.ErrorOffset, 4u) << Text;
  }
}

} // namespace
