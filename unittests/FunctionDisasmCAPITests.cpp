//===- FunctionDisasmCAPITests.cpp - Recovered native instruction pages ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Public SDK boundaries use the recovered CFG without changing raw decode.

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

class FunctionDisasmCAPI : public testing::Test {
protected:
  neverd_session_t Session = nullptr;
  llvm::SmallString<128> Path;
  static constexpr uint64_t Entry = 0x400078;

  void SetUp() override {
    ASSERT_FALSE(
        llvm::sys::fs::createTemporaryFile("neverd-cfg-view", "elf", Path));
    // One executable ELF segment and a computed branch over an unused E9.
    std::vector<uint8_t> Image(120, 0);
    const uint8_t Code[] = {0xb8, 0x78, 0x00, 0x40, 0x00, 0x83, 0xc0, 0x0b,
                            0xff, 0xe0, 0xe9, 0xc3, 0x90, 0x90, 0x90, 0x90};
    Image.insert(Image.end(), std::begin(Code), std::end(Code));
    const auto Put = [&](size_t Offset, uint64_t Value, unsigned Bytes) {
      for (unsigned I = 0; I < Bytes; ++I)
        Image[Offset + I] = uint8_t(Value >> (I * 8));
    };
    Image[0] = 0x7f;
    Image[1] = 'E';
    Image[2] = 'L';
    Image[3] = 'F';
    Image[4] = 2;
    Image[5] = 1;
    Image[6] = 1;
    Put(16, 2, 2);
    Put(18, 62, 2);
    Put(20, 1, 4);
    Put(24, Entry, 8);
    Put(32, 64, 8);
    Put(52, 64, 2);
    Put(54, 56, 2);
    Put(56, 1, 2);
    Put(64, 1, 4);
    Put(68, 5, 4);
    Put(80, 0x400000, 8);
    Put(88, 0x400000, 8);
    Put(96, Image.size(), 8);
    Put(104, Image.size(), 8);
    Put(112, 4096, 8);
    std::error_code Error;
    llvm::raw_fd_ostream Output(Path, Error);
    ASSERT_FALSE(Error);
    Output.write(reinterpret_cast<const char *>(Image.data()), Image.size());
    Output.close();
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  }

  void TearDown() override {
    neverd_session_destroy(Session);
    if (!Path.empty())
      (void)llvm::sys::fs::remove(Path);
  }

  llvm::json::Value take(const char *Raw) {
    EXPECT_NE(Raw, nullptr);
    if (!Raw)
      return nullptr;
    auto Parsed = llvm::json::parse(Raw);
    neverd_free_string(Raw);
    EXPECT_TRUE(bool(Parsed));
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      return nullptr;
    }
    return std::move(*Parsed);
  }
};

TEST_F(FunctionDisasmCAPI, PagesFollowTheCFGAndRawDecodeStaysLinear) {
  auto First = take(
      neverd_function_disasm_json(Session, Entry, 0, 3, NEVERD_DISASM_FLOW));
  const auto *Page = First.getAsObject();
  ASSERT_NE(Page, nullptr);
  EXPECT_EQ(Page->getString("next_address"), "0x400083");
  EXPECT_EQ(Page->getBoolean("lift_complete"), true);
  const auto *Rows = Page->getArray("instructions");
  ASSERT_NE(Rows, nullptr);
  ASSERT_EQ(Rows->size(), 3u);
  const auto *Branch = Rows->back().getAsObject();
  ASSERT_NE(Branch, nullptr);
  EXPECT_EQ(Branch->getString("flow"), "jump");
  EXPECT_EQ(Branch->getString("target"), "0x400083");
  auto Last = take(neverd_function_disasm_json(Session, Entry, Entry + 10,
                                               std::numeric_limits<int>::max(),
                                               NEVERD_DISASM_FLOW));
  ASSERT_NE(Last.getAsObject(), nullptr);
  const auto *Tail = Last.getAsObject()->getArray("instructions");
  ASSERT_NE(Tail, nullptr);
  ASSERT_EQ(Tail->size(), 1u);
  EXPECT_EQ(Tail->front().getAsObject()->getString("mnemonic"), "ret");
  EXPECT_EQ(Tail->front().getAsObject()->getString("addr"), "0x400083");
  EXPECT_TRUE(Last.getAsObject()->get("next_address")->kind() ==
              llvm::json::Value::Null);
  auto Raw =
      take(neverd_disasm_json_ex(Session, Entry + 10, 1, NEVERD_DISASM_FLOW));
  ASSERT_NE(Raw.getAsArray(), nullptr);
  ASSERT_EQ(Raw.getAsArray()->size(), 1u);
  EXPECT_EQ(Raw.getAsArray()->front().getAsObject()->getString("mnemonic"),
            "jmp");
  auto One = take(neverd_function_disasm_json(Session, Entry, 0, 0, 0));
  ASSERT_NE(One.getAsObject(), nullptr);
  EXPECT_EQ(One.getAsObject()->getArray("instructions")->size(), 1u);
}

TEST(FunctionDisasmFailure, RejectsNullAndUnloadedSessions) {
  EXPECT_EQ(neverd_function_disasm_json(nullptr, 0, 0, 1, 0), nullptr);
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  EXPECT_EQ(neverd_function_disasm_json(Session, 0, 0, 1, 0), nullptr);
  const char *Error = neverd_last_error(Session);
  ASSERT_NE(Error, nullptr);
  EXPECT_NE(std::string(Error).find("loaded native"), std::string::npos);
  neverd_free_string(Error);
  neverd_session_destroy(Session);
}

} // namespace
