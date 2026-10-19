//===- KernelRTLInputTests.cpp - Native RTL string and identifier inputs
//---===//
// NeverD Decompiler
///
/// \file
/// Native RTL string and identifier inputs.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "llvm/ADT/StringExtras.h"

#include <filesystem>

namespace neverd::emulation {
namespace {
class KernelRTLInput : public testing::Test {
protected:
  static constexpr uint64_t S = 0x22000000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void SetUp() override {
    Memory = llvm::cantFail(UnicornBackend::create(4 * 1024 * 1024));
    ok(Memory->map(S, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    ok(Model->initialize(I, DriverOptions{}));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void put(uint64_t At, uint64_t Value, unsigned Width = 8) {
    ok(Memory->writeInteger(At, Value, Width));
  }
  void text(uint64_t Header, uint64_t Buffer, llvm::StringRef Hex,
            unsigned Capacity = 0) {
    auto Bytes = llvm::fromHex(Hex);
    put(Header, Bytes.size(), 2);
    put(Header + 2, Capacity ? Capacity : Bytes.size() + 2, 2);
    put(Header + 4, 0x5a5a5a5a, 4);
    put(Header + 8, Buffer);
    ok(Memory->write(
        Buffer,
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Bytes.data()),
                                Bytes.size())));
    put(Buffer + Bytes.size(), 0, 2);
  }
};
TEST_F(KernelRTLInput, NativeMatrixMatchesOutputsIncludingFailures) {
  struct Case {
    const char *Kind, *Input, *Other;
    uint32_t Parameter, Status, Length;
    const char *Output;
  };
  static constexpr Case Cases[] = {
#define NEVERD_RTL_INPUT_CASE(Kind, Input, Other, Parameter, Status, Length,   \
                              Output)                                          \
  {#Kind, Input, Other, Parameter, Status, Length, Output},
#include "RTLInputCases.def"
#undef NEVERD_RTL_INPUT_CASE
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(::testing::Message() << C.Kind << " input=" << C.Input
                                      << " parameter=" << C.Parameter);
    std::vector<uint8_t> Guard(256, 0xa5);
    ok(Memory->write(S + 0x3000, Guard));
    text(S + 0x100, S + 0x1000, C.Input);
    const llvm::StringRef Kind = C.Kind;
    llvm::Expected<uint64_t> Value = uint64_t(0);
    if (Kind == "integer") {
      Value = Model->call("RtlUnicodeStringToInteger",
                          {S + 0x100, C.Parameter, S + 0x3000});
      EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + 0x3000, 4)), C.Length);
    } else if (Kind == "guid") {
      Value = Model->call("RtlGUIDFromString", {S + 0x100, S + 0x3000});
    } else if (Kind == "prefix") {
      text(S + 0x200, S + 0x2000, C.Other);
      Value = Model->call("RtlPrefixUnicodeString",
                          {S + 0x100, S + 0x200, C.Parameter});
    } else {
      auto Current = llvm::fromHex(C.Input);
      ok(Memory->write(S + 0x3000,
                       llvm::ArrayRef<uint8_t>(
                           reinterpret_cast<const uint8_t *>(Current.data()),
                           Current.size())));
      put(S + 0x3040, 0, 1);
      put(S + 0x100, Current.size(), 2);
      put(S + 0x102, C.Parameter, 2);
      put(S + 0x108, S + 0x3000);
      text(S + 0x200, S + 0x2000,
           llvm::StringRef(C.Other) == "NULL" ? "" : C.Other);
      Value = Model->call(
          "RtlAppendUnicodeToString",
          {S + 0x100, llvm::StringRef(C.Other) == "NULL" ? 0 : S + 0x2000});
      EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + 0x100, 2)), C.Length);
      EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + 0x102, 2)), C.Parameter);
    }
    ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
    EXPECT_EQ(*Value, C.Status);
    if (*C.Output) {
      std::vector<uint8_t> Bytes(llvm::StringRef(C.Output).size() / 2);
      ok(Memory->read(S + 0x3000, Bytes));
      EXPECT_EQ(llvm::toHex(Bytes, true), C.Output);
    }
  }
}
TEST_F(KernelRTLInput, AdditionalNativeCharacterAndSizeMatrix) {
  struct Case {
    const char *Kind, *Bytes;
    uint32_t Parameter, Status, Value;
  };
  static constexpr Case Cases[] = {
#define NEVERD_RTL_ADDITIONAL_CASE(Kind, Bytes, Parameter, Status, Value)      \
  {#Kind, Bytes, Parameter, Status, Value},
#include "RTLAdditionalCases.def"
#undef NEVERD_RTL_ADDITIONAL_CASE
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(::testing::Message()
                 << C.Kind << " " << C.Bytes << " " << C.Parameter);
    auto Raw = llvm::fromHex(C.Bytes);
    ok(Memory->write(
        S + 0x1000,
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Raw.data()),
                                Raw.size())));
    put(S + 0x3000, 0xa5a5a5a5, 4);
    llvm::Expected<uint64_t> R = uint64_t(0);
    if (llvm::StringRef(C.Kind) == "character") {
      R = Model->call("RtlCharToInteger",
                      {S + 0x1000, C.Parameter, S + 0x3000});
      EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + 0x3000, 4)), C.Value);
    } else {
      put(S + 0x100, Raw.size(), 2);
      put(S + 0x102, C.Parameter, 2);
      put(S + 0x108, S + 0x1000);
      R = Model->call("RtlxAnsiStringToUnicodeSize", {S + 0x100});
    }
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(*R, C.Status);
  }
  auto InvalidRadix = Model->call("RtlCharToInteger", {0, 3, 0});
  ASSERT_TRUE(bool(InvalidRadix)) << llvm::toString(InvalidRadix.takeError());
  EXPECT_EQ(*InvalidRadix, 0xc000000du);
  put(S + 0x100, 0, 2);
  put(S + 0x108, 0);
  EXPECT_EQ(
      llvm::cantFail(Model->call("RtlxAnsiStringToUnicodeSize", {S + 0x100})),
      2u);
  put(S + 0x100, 1, 2);
  put(S + 0x108, S + 0x1000);
  put(S + 0x1000, 0xe9, 1);
  auto NLS = Model->call("RtlxAnsiStringToUnicodeSize", {S + 0x100});
  ASSERT_FALSE(bool(NLS));
  EXPECT_NE(llvm::toString(NLS.takeError()).find("code page"),
            std::string::npos);
}

TEST_F(KernelRTLInput, UnsupportedNLSAndAliasedParserBuffersFailClearly) {
  text(S + 0x100, S + 0x1000, "e900");
  text(S + 0x200, S + 0x2000, "c900");
  auto Prefix =
      Model->call("RtlPrefixUnicodeString", {S + 0x100, S + 0x200, 1});
  ASSERT_FALSE(bool(Prefix));
  EXPECT_NE(llvm::toString(Prefix.takeError()).find("NLS"), std::string::npos);
  text(S + 0x100, S + 0x1000, "3100320033003400");
  ok(Memory->replaceAliases({},
                            {{S + 0x20000, S + 0x1000, 4096, Read | Write}}));
  auto Parse =
      Model->call("RtlUnicodeStringToInteger", {S + 0x100, 10, S + 0x20000});
  ASSERT_FALSE(bool(Parse));
  EXPECT_NE(llvm::toString(Parse.takeError()).find("aliased"),
            std::string::npos);
  EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + 0x1000, 4)), 0x00320031u);
}
TEST(DriverRTLInput, CompiledGuestABI) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Contract) << " " << Preemptive);
      DriverOptions O;
      O.Unload = true;
      O.Contract = Contract;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_rtl_inputs.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    }
}
} // namespace
} // namespace neverd::emulation
