//===- AndroidErrorTests.cpp - Guest error-text ownership and ABI --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent guest callers and guarded snapshots for Bionic borrowed text.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ExecutionBackend.h"
#include "neverd/emulation/ExecutionBudget.h"
#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000, BufferSize = 32768;
void word(std::vector<uint8_t> &Bytes, size_t Offset, uint64_t Value) {
  llvm::support::endian::write64le(Bytes.data() + Offset, Value);
}
uint64_t word(const std::vector<uint8_t> &Bytes, size_t Offset) {
  return llvm::support::endian::read64le(Bytes.data() + Offset);
}
void text(std::vector<uint8_t> &Bytes, size_t Offset, llvm::StringRef Text) {
  std::copy(Text.begin(), Text.end(), Bytes.begin() + Offset);
  Bytes[Offset + Text.size()] = 0;
}
class AndroidErrors : public testing::TestWithParam<
                          std::tuple<const char *, ExecutionBackendKind>> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android cross compiler unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->TraceLimit = 100000;
    Options.Android->Memory.push_back(
        {Buffer, BufferSize, std::vector<uint8_t>(BufferSize, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, BufferSize});
    Options.Android->Libraries["liberrors.so"] = {"strerror"};
    Options.Limits.Instructions = 100000;
    Options.InstructionQuantum = 7;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Arguments) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Arguments);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R)
      llvm::report_fatal_error(R.takeError());
    return std::move(*R);
  }
  void returned(const ProcessResult &R) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
  }
};

TEST_P(AndroidErrors, ErrorNumberWidthAndCompleteStrings) {
  const std::pair<uint64_t, const char *> Cases[] = {
      {0, "Success"},
      {2, "No such file or directory"},
      {11, "Try again"},
      {12, "Out of memory"},
      {41, "Unknown error 41"},
      {58, "Unknown error 58"},
      {134, "Unknown error 134"},
      {0x7fffffff, "Unknown error 2147483647"},
      {0x80000000, "Unknown error -2147483648"},
      {0xffffffff, "Unknown error -1"},
      {0xffffffff00000002, "No such file or directory"},
      {0x11223344ffffffff, "Unknown error -1"}};
  for (auto [Code, ExpectedText] : Cases) {
    for (uint64_t Route : {0u, 1u}) {
      SCOPED_TRACE(testing::Message() << Code << ":" << Route);
      auto R = run("error_query", {Buffer, Code, Route});
      returned(R);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      const auto &Bytes = R.MemorySnapshots[0].Bytes;
      ASSERT_EQ(Bytes.size(), BufferSize);
      const auto Pointer = word(Bytes, 0);
      ASSERT_NE(Pointer, 0u);
      unsigned Calls = 0;
      for (const auto &Call : R.NativeCalls) {
        if (Call.Name == "strerror") {
          EXPECT_EQ(Call.Arguments[0], Code);
          EXPECT_EQ(Call.Result, Pointer);
          EXPECT_EQ(Call.Library, Route ? "liberrors.so" : "");
          ++Calls;
        }
      }
      EXPECT_EQ(Calls, 1u);
      std::vector<uint8_t> Expected(BufferSize, 0xa5);
      word(Expected, 0, Pointer);
      word(Expected, 8, llvm::StringRef(ExpectedText).size());
      word(Expected, 16, 733);
      text(Expected, 32, ExpectedText);
      EXPECT_EQ(Bytes, Expected);
    }
  }
}

TEST_P(AndroidErrors, KnownAndUnknownBorrowsKeepDistinctLifetimes) {
  auto R = run("error_lifetime", {Buffer});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  ASSERT_EQ(Bytes.size(), BufferSize);
  EXPECT_NE(word(Bytes, 0), 0u);
  EXPECT_EQ(word(Bytes, 0), word(Bytes, 24));
  EXPECT_NE(word(Bytes, 0), word(Bytes, 8));
  EXPECT_EQ(word(Bytes, 8), word(Bytes, 16));
  EXPECT_NE(word(Bytes, 40), 0u);
  EXPECT_NE(word(Bytes, 40), word(Bytes, 0));
  std::vector<uint8_t> Expected(BufferSize, 0xa5);
  for (unsigned I : {0u, 1u, 2u, 3u, 5u})
    word(Expected, I * 8, word(Bytes, I * 8));
  word(Expected, 32, 733);
  word(Expected, 48, 0);
  text(Expected, 64, "Unknown error -12345");
  text(Expected, 192, "Unknown error -1");
  text(Expected, 320, "No such file or directory");
  auto End = std::find(Bytes.begin() + 512, Bytes.begin() + 608, 0);
  ASSERT_NE(End, Bytes.begin() + 608);
  const std::string Linker(Bytes.begin() + 512, End);
  EXPECT_FALSE(Linker.empty());
  text(Expected, 512, Linker);
  text(Expected, 640, Linker);
  EXPECT_EQ(Bytes, Expected);
}

TEST_P(AndroidErrors, UnknownStorageBelongsToEachThread) {
  Options.Android->ThreadLimit = 3;
  auto R = run("error_threads", {Buffer});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  ASSERT_EQ(Bytes.size(), BufferSize);
  const uint64_t Pointers[] = {word(Bytes, 0), word(Bytes, 256),
                               word(Bytes, 512)};
  for (unsigned I = 0; I != 3; ++I) {
    EXPECT_NE(Pointers[I], 0u);
    for (unsigned J = 0; J != I; ++J)
      EXPECT_NE(Pointers[I], Pointers[J]);
  }
  EXPECT_NE(word(Bytes, 16), 0u);
  EXPECT_EQ(word(Bytes, 16), word(Bytes, 264));
  EXPECT_EQ(word(Bytes, 16), word(Bytes, 520));
  std::vector<uint8_t> Expected(BufferSize, 0xa5);
  for (size_t Offset : {size_t(0), size_t(16), size_t(256), size_t(264),
                        size_t(512), size_t(520)})
    word(Expected, Offset, word(Bytes, Offset));
  word(Expected, 8, 77);
  word(Expected, 272, 81);
  word(Expected, 528, 82);
  text(Expected, 64, "Unknown error -1");
  text(Expected, 320, "Unknown error 41");
  text(Expected, 576, "Unknown error 58");
  EXPECT_EQ(Bytes, Expected);
  ASSERT_EQ(R.NativeThreads.size(), 3u);
  for (const auto &Thread : R.NativeThreads) {
    EXPECT_TRUE(Thread.Finished);
    EXPECT_FALSE(Thread.Waiting);
  }
}

TEST_P(AndroidErrors, ClosedProviderCannotCallSavedAddress) {
  auto R = run("error_query", {Buffer, 2, 2});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "strerror");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(BufferSize, 0xa5));
}

TEST_P(AndroidErrors, KnownStringsAreReadOnly) {
  auto R = run("error_write_known", {});
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ReturnValue);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "strerror");
  EXPECT_TRUE(R.NativeCalls.back().Result);
}

TEST_P(AndroidErrors, KnownPointerReturnDoesNotReadUnmappedStringBytes) {
  auto R = run("error_known_unmapped", {});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(BufferSize, 0xa5));
  uint64_t Pointer = 0;
  unsigned Calls = 0;
  for (const auto &Call : R.NativeCalls) {
    if (Call.Name != "strerror")
      continue;
    ASSERT_TRUE(Call.Result);
    if (Calls)
      EXPECT_EQ(Call.Result, Pointer);
    Pointer = *Call.Result;
    ++Calls;
  }
  EXPECT_EQ(Calls, 2u);
}

TEST_P(AndroidErrors, ProviderOnlyImportInitializesKnownStringStorage) {
  auto Filename = Path.filename().string();
  Filename.replace(0, 6, "errors_dynamic");
  Path = Path.parent_path() / Filename;
  auto R = run("error_query", {Buffer, 2, 1});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  ASSERT_EQ(Bytes.size(), BufferSize);
  const uint64_t Pointer = word(Bytes, 0);
  EXPECT_NE(Pointer, 0u);
  std::vector<uint8_t> Expected(BufferSize, 0xa5);
  word(Expected, 0, Pointer);
  word(Expected, 8, 25);
  word(Expected, 16, 733);
  text(Expected, 32, "No such file or directory");
  EXPECT_EQ(Bytes, Expected);
  unsigned Calls = 0;
  for (const auto &Call : R.NativeCalls) {
    if (Call.Name != "strerror")
      continue;
    EXPECT_EQ(Call.Library, "liberrors.so");
    EXPECT_EQ(Call.Result, Pointer);
    ++Calls;
  }
  EXPECT_EQ(Calls, 1u);
}

TEST_P(AndroidErrors, ReadonlyThreadBufferRefusesUnknownFormatting) {
  auto R = run("error_readonly_tls", {});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("invalid guest pointer"), std::string::npos);
  EXPECT_FALSE(R.ReturnValue);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "strerror");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidErrors, ReentrantVariantRemainsUnsupported) {
  auto R = run("error_reentrant_not_admitted", {Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_EQ(R.Diagnostic, "unmodeled Android import: strerror_r");
  EXPECT_FALSE(R.ReturnValue);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(BufferSize, 0xa5));
}

INSTANTIATE_TEST_SUITE_P(
    API28, AndroidErrors,
    testing::Combine(testing::Values("errors-O0-none", "errors-O0-android",
                                     "errors-O0-relr", "errors-O2-none",
                                     "errors-O2-android", "errors-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));

TEST(AndroidErrorMemory, UnknownRefusalsPreserveCompleteThreadBacking) {
  ExecutionConfiguration C;
  C.Backend = ExecutionBackendKind::Unicorn;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(C);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Backend = createExecutionBackend(C, 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  auto &Space = *CPU.addressSpace();
  constexpr uint64_t TLS = android_model::TLSAddress;
  llvm::cantFail(Space.map(TLS, 4096, Read | Write | UserAccessible));
  std::vector<uint8_t> Before(4096, 0xa5);
  llvm::cantFail(CPU.write(TLS, Before));
  ProcessOptions Options;
  Options.Android.emplace();
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer, Options, Result);
  android_model::LinkedImage Linked{};
  Linked.Imports.emplace(1, "strerror");
  llvm::cantFail(android_model::initializeErrorStrings(Space, Linked));
  for (unsigned Mode : {0u, 1u, 2u}) {
    Options.MemoryLimit = Mode ? 4096 : 8;
    llvm::cantFail(Space.protect(
        TLS, 4096, Read | UserAccessible | (Mode == 1 ? 0 : Write)));
    auto Budget =
        Mode == 2 ? llvm::cantFail(ExecutionBudget::create(
                        {1, 1, 1}, ExecutionBudget::Clock::time_point::min()))
                  : llvm::cantFail(ExecutionBudget::create(Options.Limits));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = "strerror";
    Call.Arguments = {UINT64_MAX};
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    const std::string Message = llvm::toString(Value.takeError());
    EXPECT_NE(Message.find(Mode == 0   ? "memory limit"
                           : Mode == 1 ? "invalid guest pointer"
                                       : "timed out"),
              std::string::npos);
    EXPECT_EQ(Model.timedOut(), Mode == 2);
    std::vector<uint8_t> After(4096);
    llvm::cantFail(Space.snapshotBacking(TLS, After));
    EXPECT_EQ(After, Before);
  }
  // A shorter unknown result changes only its bytes and terminator. Known
  // calls preserve the prior unknown buffer, errno, dlerror and all guards.
  Options.MemoryLimit = 4096;
  llvm::cantFail(Space.protect(TLS, 4096, Read | Write | UserAccessible));
  auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
  android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                              Linked);
  auto Expected = Before;
  uint64_t Unknown = 0;
  for (auto [Code, Message] : {std::pair<uint64_t, const char *>{
                                   0x80000000, "Unknown error -2147483648"},
                               {UINT64_MAX, "Unknown error -1"},
                               {2, nullptr}}) {
    NativeCallEvent Call{};
    Call.Name = "strerror";
    Call.Arguments = {Code};
    auto Value = Model.invoke(Call);
    ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
    ASSERT_TRUE(Value->has_value());
    const uint64_t Pointer = std::get<uint64_t>(**Value);
    if (Message) {
      ASSERT_GE(Pointer, TLS);
      ASSERT_LT(Pointer + llvm::StringRef(Message).size(), TLS + 4096);
      if (Unknown)
        EXPECT_EQ(Pointer, Unknown);
      Unknown = Pointer;
      text(Expected, Pointer - TLS, Message);
    } else {
      EXPECT_NE(Pointer, Unknown);
    }
    std::vector<uint8_t> After(4096);
    llvm::cantFail(Space.snapshotBacking(TLS, After));
    EXPECT_EQ(After, Expected);
  }
  // Returning a known borrowed pointer performs no guest byte access, even
  // when the caller later unmaps the text or chooses a tiny copy-work budget.
  Options.MemoryLimit = 1;
  NativeCallEvent Known{};
  Known.Name = "strerror";
  Known.Arguments = {2};
  auto First = llvm::cantFail(Model.invoke(Known));
  ASSERT_TRUE(First);
  const uint64_t Pointer = std::get<uint64_t>(*First);
  llvm::cantFail(Space.unmap(Pointer & ~uint64_t(4095), 4096));
  auto Again = llvm::cantFail(Model.invoke(Known));
  ASSERT_TRUE(Again);
  EXPECT_EQ(std::get<uint64_t>(*Again), Pointer);
  std::vector<uint8_t> After(4096);
  llvm::cantFail(Space.snapshotBacking(TLS, After));
  EXPECT_EQ(After, Expected);
}
} // namespace
} // namespace neverd::emulation
