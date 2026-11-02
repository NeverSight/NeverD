//===- TextEncodingTests.cpp - External text normalization ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks that bulk validation preserves the byte-wise escape contract.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/TextEncoding.h"

#include <string>
#include <utility>
#include <vector>

TEST(TextEncodingAcceleration,
     PreservesValidBytesIncludingNULAndNoncharacters) {
  const std::string Unit =
      std::string("x\0y", 3) + "\xe4\xb8\xad\xef\xbf\xbf\xf4\x8f\xbf\xbf";
  for (size_t Count : {0u, 1u, 4u, 8u, 1000u}) {
    std::string Text;
    for (size_t I = 0; I < Count; ++I)
      Text += Unit;
    EXPECT_EQ(neverd::escapeInvalidUTF8(Text), Text);
    const auto Expected = Text;
    const auto *Storage = Text.data();
    const auto Capacity = Text.capacity();
    neverd::escapeInvalidUTF8InPlace(Text);
    EXPECT_EQ(Text, Expected);
    EXPECT_EQ(Text.data(), Storage);
    EXPECT_EQ(Text.capacity(), Capacity);
  }
}

TEST(TextEncodingAcceleration,
     BadBytesKeepTheirIdentityAtEveryFastPathBoundary) {
  const std::vector<std::pair<std::string, std::string>> Cases = {
      {"\xff", "\\xFF"},
      {"\xc0\x80", "\\xC0\\x80"},
      {"\xed\xa0\x80", "\\xED\\xA0\\x80"},
      {"\xe1\x80", "\\xE1\\x80"},
      {"\xf4\x90\x80\x80", "\\xF4\\x90\\x80\\x80"},
      {"\xc3(", "\\xC3("}};
  for (size_t Prefix : {0u, 1u, 31u, 32u, 63u, 64u, 65u, 255u, 4096u})
    for (const auto &[Bad, Escaped] : Cases) {
      const auto Start = std::string(Prefix, 'x');
      EXPECT_EQ(neverd::escapeInvalidUTF8(Start + Bad), Start + Escaped);
      EXPECT_EQ(neverd::escapeInvalidUTF8(Start + Bad + "valid\xe4\xb8\xad"),
                Start + Escaped + "valid\xe4\xb8\xad");
      for (const auto *Suffix : {"", "valid\xe4\xb8\xad"}) {
        auto Text = Start + Bad + Suffix;
        neverd::escapeInvalidUTF8InPlace(Text);
        EXPECT_EQ(Text, Start + Escaped + Suffix);
      }
    }
}

TEST(TextEncodingAcceleration, EverySingleByteHasTheSameLosslessEscape) {
  static constexpr char Hex[] = "0123456789ABCDEF";
  for (unsigned Byte = 0; Byte < 256; ++Byte) {
    std::string Text(100, 'a'), Want = Text;
    Text.push_back(static_cast<char>(Byte));
    if (Byte < 128)
      Want.push_back(static_cast<char>(Byte));
    else {
      Want += "\\x";
      Want += Hex[Byte >> 4];
      Want += Hex[Byte & 15];
    }
    EXPECT_EQ(neverd::escapeInvalidUTF8(Text), Want);
    neverd::escapeInvalidUTF8InPlace(Text);
    EXPECT_EQ(Text, Want);
  }
}
