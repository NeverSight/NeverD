//===- KernelCrtStringTests.cpp - Kernel CRT byte and return contracts
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Kernel CRT byte and return contracts.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelReferenceImage.h"
#include "os/windows/kernel/KernelReferenceInitializer.h"

#include <cstdlib>

namespace neverd::emulation {
namespace {
class KernelCrtString : public testing::Test {
protected:
  static constexpr uint64_t S = 0x20000000, D = S + 0x1000, Source = S + 0x2000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  void initialize(bool Version = true) {
    Model.reset();
    auto M = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(M)) << llvm::toString(M.takeError());
    Memory = std::move(*M);
    ok(Memory->map(S, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverOptions O;
    if (Version)
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 0x1000;
    I.Size = 0x3000;
    ok(Model->initialize(I, O));
    Model->enterExecution(profile::StackBase);
  }
  void SetUp() override { initialize(); }
  static std::vector<uint8_t> wide(std::u16string_view Text, size_t Units = 0) {
    std::vector<uint8_t> B((Units ? Units : Text.size() + 1) * 2, 0xcc);
    for (size_t I = 0; I <= Text.size(); ++I) {
      const uint16_t C = I < Text.size() ? Text[I] : 0;
      B[I * 2] = uint8_t(C);
      B[I * 2 + 1] = uint8_t(C >> 8);
    }
    return B;
  }
  uint64_t get(uint64_t At, unsigned Size = 2) {
    auto V = Memory->readInteger(At, Size);
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return UINT64_MAX;
    }
    return *V;
  }
  std::vector<uint8_t> bytes(uint64_t At, size_t Size) {
    std::vector<uint8_t> B(Size);
    ok(Memory->read(At, B));
    return B;
  }
  uint64_t call(llvm::StringRef Name, std::initializer_list<uint64_t> A) {
    auto V = Model->call(Name.str(), A);
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return UINT64_MAX;
    }
    return *V;
  }
  void error(llvm::StringRef Name, std::initializer_list<uint64_t> A,
             llvm::StringRef Text) {
    auto V = Model->call(Name.str(), A);
    ASSERT_FALSE(bool(V));
    EXPECT_NE(llvm::toString(V.takeError()).find(Text.str()),
              std::string::npos);
  }
};

TEST_F(KernelCrtString, WideConcatExactFitPreservesRawCodeUnitsAndTail) {
  ok(Memory->write(D, wide(u"A", 10)));
  ok(Memory->write(Source, wide(u"\u4e2d\U0001f600")));
  EXPECT_EQ(call("wcscat_s", {D, 5, Source}), 0u);
  EXPECT_EQ(bytes(D, 10), wide(u"A\u4e2d\U0001f600"));
  EXPECT_EQ(get(D + 10), 0xccccu);
  ok(Memory->write(Source, wide(u"")));
  EXPECT_EQ(call("wcscat_s", {D, 5, Source}), 0u);
}

TEST_F(KernelCrtString,
       RangeErrorPreservesPartialSuffixBeforeClearingFirstWord) {
  ok(Memory->write(D, wide(u"AB", 10)));
  ok(Memory->write(Source, wide(u"CDE")));
  EXPECT_EQ(call("wcscat_s", {D, 4, Source}), 34u);
  EXPECT_EQ(get(D), 0u);
  EXPECT_EQ(get(D + 2), uint64_t('B'));
  EXPECT_EQ(get(D + 4), uint64_t('C'));
  EXPECT_EQ(get(D + 6), uint64_t('D'));
  EXPECT_EQ(get(D + 8), 0xccccu);
}

TEST_F(KernelCrtString, InvalidParameterHasKernelProfileReturnAndPreciseClear) {
  ok(Memory->write(D, wide(u"AB", 10)));
  ok(Memory->write(Source, wide(u"X")));
  EXPECT_EQ(call("wcscat_s", {0, 8, Source}), 22u);
  EXPECT_EQ(call("wcscat_s", {D, 0, Source}), 22u);
  EXPECT_EQ(get(D), uint64_t('A'));
  EXPECT_EQ(call("wcscat_s", {D, 8, 0}), 22u);
  EXPECT_EQ(get(D), 0u);
  EXPECT_EQ(get(D + 2), uint64_t('B'));
  ok(Memory->write(D, wide(u"AB", 10)));
  EXPECT_EQ(call("wcscat_s", {D, 2, 0xdead0000}), 22u);
  EXPECT_EQ(get(D), 0u);
  EXPECT_EQ(get(D + 2), uint64_t('B'));
}

TEST_F(KernelCrtString, UndefinedOverlapAndInaccessibleOutputsFailClearly) {
  ok(Memory->write(D, wide(u"AB", 10)));
  auto Before = bytes(D, 20);
  error("wcscat_s", {D, 8, D}, "overlapping");
  EXPECT_EQ(bytes(D, 20), Before);
  EXPECT_EQ(call("wcscat_s", {D, 65536, Source}), 0u);
  EXPECT_EQ(bytes(D, 20), Before);
  ok(Memory->write(Source, wide(u"X")));
  ok(Memory->protect(D, 4096, Read));
  error("wcscat_s", {D, 8, Source}, "write fault");
  EXPECT_EQ(bytes(D, 20), Before);
}

TEST_F(KernelCrtString, LastWideCharacterIncludesTerminatorAndUsesSixteenBits) {
  ok(Memory->write(D, wide(u"A\u4e2dB\u4e2d")));
  EXPECT_EQ(call("wcsrchr", {D, 0x4e2d}), D + 6);
  EXPECT_EQ(call("wcsrchr", {D, 0}), D + 8);
  EXPECT_EQ(call("wcsrchr", {D, 0x100004e2d}), D + 6);
  EXPECT_EQ(call("wcsrchr", {D, 'Z'}), 0u);
}

TEST_F(KernelCrtString,
       KernelCLocaleByteMappingAndEofAreIndependentOfHostLocale) {
  for (uint32_t C = 0; C < 256; ++C)
    EXPECT_EQ(call("tolower", {C}), C >= 'A' && C <= 'Z' ? C + 32 : C);
  EXPECT_EQ(call("tolower", {UINT64_MAX}), uint64_t(UINT32_MAX));
  error("tolower", {256}, "unsigned byte or EOF");
}

TEST_F(KernelCrtString, VersionScopedErrorPolicyIsExplicit) {
  initialize(false);
  EXPECT_EQ(call("wcscat_s", {0, 0, 0}), 22u);
  error("tolower", {'A'}, "19041");
  ok(Memory->write(D, wide(u"")));
  EXPECT_EQ(call("wcsrchr", {D, 0}), D);
}

TEST_F(KernelCrtString,
       ActualKernelConcatMatchesStatusesAndCompleteBufferImages) {
  const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE");
  const char *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "explicit matched kernel and PDB are required";
  auto Ref = KernelReferenceImage::load(Image, 64 * 1024 * 1024, PDB);
  ASSERT_TRUE(bool(Ref)) << llvm::toString(Ref.takeError());
  struct Case {
    std::u16string Dest, Src;
    uint64_t Capacity;
    bool NullDest = false, NullSource = false;
  };
  for (const auto &C : std::vector<Case>{{u"AB", u"CD", 8},
                                         {u"AB", u"CD", 5},
                                         {u"AB", u"CDE", 4},
                                         {u"AB", u"X", 2},
                                         {u"", u"", 1},
                                         {u"", u"X", 1},
                                         {u"AB", u"X", 0},
                                         {u"AB", u"X", 8, true},
                                         {u"AB", u"X", 8, false, true},
                                         {u"A", u"\u4e2d", 8}}) {
    auto Dest = wide(C.Dest, 10), Src = wide(C.Src);
    ok(Memory->write(D, Dest));
    ok(Memory->write(Source, Src));
    KernelInitializerPlan Plan;
    Plan.StackBase = 0x52000000;
    Plan.StackSize = 4096;
    Plan.ReturnPC = 0x53000000;
    Plan.MemoryLimit = 64 * 1024 * 1024;
    Plan.Writable = {{D, Dest.size()}};
    Plan.Outputs = {{D, Dest.size()}};
    Plan.Inputs = {{D, Dest}, {Source, Src}};
    Plan.Calls = {{Ref->exports().at("wcscat_s"),
                   {C.NullDest ? 0 : D, C.Capacity, C.NullSource ? 0 : Source},
                   false}};
    auto Native = initializeFromKernelReference(*Ref, Plan);
    ASSERT_TRUE(bool(Native)) << llvm::toString(Native.takeError());
    ASSERT_EQ(Native->ReturnValues.size(), 1u);
    ASSERT_EQ(Native->Outputs.size(), 1u);
    auto Value = call("wcscat_s", {C.NullDest ? 0 : D, C.Capacity,
                                   C.NullSource ? 0 : Source});
    EXPECT_EQ(Value, Native->ReturnValues[0]);
    EXPECT_EQ(bytes(D, Dest.size()), Native->Outputs[0].Bytes);
  }
}
} // namespace
} // namespace neverd::emulation
