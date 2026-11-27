//===- LuaRecoveryTests.cpp - Lua-family recovery qualification ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Use owned, toolchain-identified fixtures and independently mutated boundary
/// cases.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/LuaRecovery.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Base64.h"
#include "llvm/Support/MemoryBuffer.h"
using namespace llvm;
using namespace neverd::lua;
namespace {
json::Array fixtures() {
  auto File = MemoryBuffer::getFile(LUA_DUMP_MANIFEST);
  if (!File)
    return {};
  auto Document = json::parse((*File)->getBuffer());
  if (!Document) {
    consumeError(Document.takeError());
    return {};
  }
  return std::move(*Document->getAsObject()->getArray("fixtures"));
}
std::string bytes(const json::Object &Row) {
  std::vector<char> Data;
  if (auto E = decodeBase64(*Row.getString("bytes"), Data)) {
    consumeError(std::move(E));
    return {};
  }
  return {Data.begin(), Data.end()};
}
json::Object analyze(StringRef B, StringRef Profile = "auto") {
  return recover(B, ".bin", Profile, [] { return false; });
}
std::string sample(StringRef Id) {
  for (auto &V : fixtures())
    if (V.getAsObject()->getString("id") == Id)
      return bytes(*V.getAsObject());
  return {};
}
void word(std::string &B, size_t Offset, uint32_t Word) {
  for (unsigned I = 0; I < 4; ++I)
    B.at(Offset + I) = char(Word >> (8 * I));
}
TEST(LuaRecovery, OwnedCompilerDumps) {
  auto Rows = fixtures();
  ASSERT_EQ(Rows.size(), 51u);
  for (const auto &V : Rows) {
    auto &Row = *V.getAsObject();
    auto Id = *Row.getString("id");
    SCOPED_TRACE(Id.str());
    auto Data = bytes(Row);
    auto Report = analyze(Data, *Row.getString("profile"));
    ASSERT_EQ(Report.getString("status"), "reconstructed_source");
    EXPECT_EQ(Report.getString("input_sha256"), Row.getString("sha256"));
    ASSERT_TRUE(Report.getInteger("root"));
    auto Fns = Report.getArray("functions");
    ASSERT_FALSE(Fns->empty());
    for (const auto &Item : *Fns) {
      auto &F = *Item.getAsObject();
      if (Id == "luau/typed/debug" && F.getInteger("parameters") == 2)
        ASSERT_NE(F.getObject("type_info"), nullptr);
      if (Id.ends_with("/capture/debug") &&
          (Id.starts_with("5.1") || Id.starts_with("5.3") ||
           Id.starts_with("5.4"))) {
        auto Name = Fns->front().getAsObject()->getString("source_name");
        ASSERT_TRUE(Name);
        EXPECT_EQ(F.getString("source_name"), Name);
      }
      EXPECT_EQ(F.getString("register_names"), "generated");
      EXPECT_EQ(F.getString("line_info"),
                Id.ends_with("stripped") ? "unavailable" : "available");
      ASSERT_TRUE(F.getString("source"));
      auto Text = *F.getString("source");
      EXPECT_TRUE(Text.contains("return"));
      for (const auto &Insn : *F.getArray("instructions")) {
        auto &I = *Insn.getAsObject();
        EXPECT_LE(*I.getInteger("offset") + 4 * *I.getInteger("words"),
                  int64_t(Data.size()));
        if (I.getString("opcode") == "FORPREP") {
          auto Edges = I.getArray("successors");
          ASSERT_NE(Edges, nullptr);
          bool Legacy = Id.starts_with("5.1") || Id.starts_with("5.2") ||
                        Id.starts_with("5.3");
          ASSERT_EQ(Edges->size(), Legacy ? 1u : 2u);
          if (Legacy)
            EXPECT_GT(*Edges->front().getAsInteger(), *I.getInteger("pc") + 1);
          else
            EXPECT_EQ(Edges->front().getAsInteger(), *I.getInteger("pc") + 1);
        }
        if (auto Line = I.getInteger("source_line"))
          EXPECT_LE(*Line, Text.count('\n'));
      }
    }
  }
}
TEST(LuaRecovery, SourceProfilesAndCancellation) {
  std::string Source = "\xEF\xBB\xBF-- owned\r\nreturn 7\r\n";
  auto R = recover(Source, ".lua", "auto", [] { return false; });
  ASSERT_EQ(R.getString("status"), "extracted_source");
  EXPECT_EQ((*R.getArray("functions"))[0].getAsObject()->getString("source"),
            Source);
  auto Jit = sample("jit-X/values/debug");
  ASSERT_FALSE(Jit.empty());
  EXPECT_EQ(analyze(Jit, "puc").getString("status"), "unsupported");
  EXPECT_EQ(analyze(Jit, "luau-0.659").getString("status"), "unsupported");
  auto Luau = sample("luau/capture/debug");
  EXPECT_EQ(analyze(Luau).getString("status"), "unsupported");
  Luau[0] = 99;
  EXPECT_EQ(analyze(Luau, "luau-0.659").getString("status"), "unsupported");
  Luau[0] = 6;
  Luau[1] = 99;
  EXPECT_EQ(analyze(Luau, "luau-0.659").getString("status"), "unsupported");
  unsigned Checks = 0;
  EXPECT_EQ(recover(Jit, ".bin", "auto", [&] { return ++Checks == 7; })
                .getString("status"),
            "cancelled");
  EXPECT_EQ(analyze(StringRef("\x1bLu", 3)).getString("status"), "malformed");
  EXPECT_EQ(analyze(std::string(513 * 1024, 'a')).getString("status"),
            "budget_exceeded");
}
TEST(LuaRecovery, MalformedOperandsAndAuxiliaryBoundaries) {
  auto Data = sample("luau/values/debug");
  ASSERT_FALSE(Data.empty());
  auto Good = analyze(Data, "luau-0.659");
  auto &Insns =
      *(*Good.getArray("functions"))[0].getAsObject()->getArray("instructions");
  ASSERT_GT(Insns.size(), 2u);
  auto Offset = [&](size_t I) {
    return *Insns[I].getAsObject()->getInteger("offset");
  };
  auto Mutate = [&](int64_t At, uint32_t W) {
    auto Changed = Data;
    word(Changed, At, W);
    return analyze(Changed, "luau-0.659");
  };
  EXPECT_EQ(Mutate(Offset(1), 5 | (0xffffu << 16)).getString("status"),
            "malformed");
  EXPECT_EQ(Mutate(Offset(1), 6 | (255u << 16)).getString("status"),
            "malformed");
  EXPECT_EQ(Mutate(Offset(Insns.size() - 1), 66).getString("status"),
            "malformed"); // LOADKX needs AUX.
  auto Changed = Data;
  word(Changed, Offset(0), 23u | (1u << 16)); // JUMP targets the AUX of LOADKX.
  word(Changed, Offset(1), 66u);
  word(Changed, Offset(2), 0);
  EXPECT_EQ(analyze(Changed, "luau-0.659").getString("status"), "malformed");
  Data.pop_back();
  auto Truncated = analyze(Data, "luau-0.659");
  EXPECT_EQ(Truncated.getString("status"), "malformed");
  EXPECT_FALSE(Truncated.getInteger("root"));
  EXPECT_FALSE(Truncated.getArray("functions")->empty());
}
TEST(LuaRecovery, UnknownChildKeepsSiblingAndByteEvidence) {
  auto Data = sample("5.4.8/capture/debug");
  ASSERT_FALSE(Data.empty());
  auto Good = analyze(Data);
  auto &Child = *(*Good.getArray("functions"))[2].getAsObject();
  auto Offset =
      *(*Child.getArray("instructions"))[0].getAsObject()->getInteger("offset");
  Data[Offset] = char(127);
  auto R = analyze(Data);
  EXPECT_EQ(R.getString("status"), "partial_reconstruction");
  ASSERT_EQ(R.getArray("functions")->size(), 3u);
  auto &F = *(*R.getArray("functions"))[2].getAsObject();
  EXPECT_TRUE(F.getString("source")->starts_with("-- partial_reconstruction"));
  EXPECT_EQ((*F.getArray("diagnostics"))[0].getAsObject()->getInteger("offset"),
            Offset);
}
TEST(LuaRecovery, BigEndianPUC51AndCustomNumericHeaders) {
  // Independent 32-bit size_t, big-endian 5.1 chunk: LOADK 3.25, RETURN.
  auto Data = fromHex("1b4c75615100000404040800"
                      "00000000000000000000000000000202"
                      "00000002000000010100001e"
                      "0000000103400a000000000000"
                      "00000000000000000000000000000000");
  auto Report = analyze(Data);
  ASSERT_EQ(Report.getString("status"), "reconstructed_source");
  EXPECT_EQ(Report.getObject("evidence")->getString("byte_order"), "big");
  EXPECT_EQ(Report.getObject("evidence")->getInteger("size_t_bytes"), 4);
  EXPECT_TRUE((*Report.getArray("functions"))[0]
                  .getAsObject()
                  ->getString("source")
                  ->contains("3.25"));
  Data[11] = 1;
  EXPECT_EQ(analyze(Data).getString("status"), "unsupported");
  Data[5] = 1;
  EXPECT_EQ(analyze(Data).getString("status"), "unsupported");
  Data[4] = 0x56;
  EXPECT_EQ(analyze(Data).getString("status"), "unsupported");
}
TEST(LuaRecovery, EveryTruncatedRecordFailsClosed) {
  for (StringRef Id :
       {"5.1.5/capture/debug", "5.2.4/capture/debug", "5.3.6/capture/debug",
        "5.4.8/capture/debug", "5.5.0/capture/debug", "jit-X/capture/debug",
        "luau/capture/debug"}) {
    auto Data = sample(Id);
    ASSERT_FALSE(Data.empty());
    for (size_t N = 0; N < Data.size(); ++N) {
      auto R = analyze(StringRef(Data).take_front(N),
                       Id.starts_with("luau/") ? "luau-0.659" : "auto");
      auto S = *R.getString("status");
      ASSERT_TRUE(S == "malformed" || S == "unsupported" ||
                  S == "budget_exceeded")
          << Id.str() << " prefix " << N << " " << S.str();
    }
  }
}
} // namespace
