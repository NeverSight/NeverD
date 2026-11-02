//===- WindowsAtomicProcessTests.cpp - Complete LSE guest process results -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/SHA256.h"

#include <array>
#include <ostream>

namespace neverd::emulation {
namespace {
#define NEVERD_ATOMIC_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_ATOMIC_ORACLE_VALUE NEVERD_ATOMIC_VALUE
#define NEVERD_ATOMIC_RESULT_VALUE NEVERD_ATOMIC_VALUE
#define NEVERD_ATOMIC_ORACLE_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_ATOMIC_RESULT_TEXT NEVERD_ATOMIC_ORACLE_TEXT
#include "aarch64/AArch64AtomicCases.def"
#include "fixtures/AArch64AtomicOracle.def"
#include "fixtures/WindowsAtomicResults.def"
#undef NEVERD_ATOMIC_VALUE
#undef NEVERD_ATOMIC_ORACLE_VALUE
#undef NEVERD_ATOMIC_RESULT_VALUE
#undef NEVERD_ATOMIC_ORACLE_TEXT
#undef NEVERD_ATOMIC_RESULT_TEXT
enum Field {
#define NEVERD_ATOMIC_ORACLE_FIELD(Name) Field##Name,
#include "fixtures/AArch64AtomicOracle.def"
#undef NEVERD_ATOMIC_ORACLE_FIELD
  FieldCount
};
constexpr struct {
  const char *Name;
  unsigned Records;
  const char *Digest;
} Groups[] = {
#define NEVERD_ATOMIC_RESULT_GROUP(Name, Records, Digest)                      \
  {#Name, Records, Digest},
#include "fixtures/WindowsAtomicResults.def"
#undef NEVERD_ATOMIC_RESULT_GROUP
};
struct Profile {
  ExecutionBackendKind Backend;
  const char *Name;
};
void PrintTo(const Profile &Value, std::ostream *OS) { *OS << Value.Name; }
constexpr Profile Profiles[] = {
#define NEVERD_ATOMIC_RESULT_BACKEND(Name) {ExecutionBackendKind::Name, #Name},
#include "fixtures/WindowsAtomicResults.def"
#undef NEVERD_ATOMIC_RESULT_BACKEND
};
class WindowsAtomic : public testing::TestWithParam<Profile> {};

TEST_P(WindowsAtomic,
       PreservesAllOriginalResultsFaultContextsAndRAMFootprints) {
#ifndef NEVERD_WINDOWS_PROCESS_FIXTURE_DIR
  GTEST_SKIP() << MissingTools;
#else
  ExecutionConfiguration Config;
  Config.Architecture = GuestArchitecture::AArch64;
  Config.Backend = GetParam().Backend;
  Config.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Config);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  ProcessOptions Options;
  Options.Backend = GetParam().Backend;
  Options.Limits = {InstructionLimit, EventLimit, TimeoutMicroseconds};
  Options.OutputLimit = OutputLimit;
  const auto Path =
      std::filesystem::path(NEVERD_WINDOWS_PROCESS_FIXTURE_DIR) / Program;
  auto Result = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  ASSERT_EQ(Result->ExitStatus, 0u) << llvm::toHex(Result->StandardError);
  ASSERT_TRUE(Result->StandardError.empty())
      << llvm::toHex(Result->StandardError);
  uint64_t ExpectedSize = 0;
  for (const auto &G : Groups)
    ExpectedSize += G.Records * FieldCount * sizeof(uint64_t);
  llvm::StringRef Bytes(Result->StandardOutput);
  ASSERT_EQ(Bytes.size(), ExpectedSize);
  for (const auto &G : Groups) {
    SCOPED_TRACE(G.Name);
    llvm::SHA256 Digest;
    for (unsigned N = 0; N < G.Records; ++N) {
      SCOPED_TRACE(N);
      std::array<uint64_t, FieldCount> Row;
      for (auto &Value : Row) {
        Value = llvm::support::endian::read64le(Bytes.data());
        Bytes = Bytes.drop_front(sizeof(uint64_t));
      }
      ASSERT_NE(Row[FieldAtomicPC], 0u);
      ASSERT_EQ(Row[FieldAtomicPC] % InstructionBytes, 0u);
      ASSERT_NE(Row[FieldMemoryBase], 0u);
      ASSERT_EQ(Row[FieldMemoryBase] % Granule, 0u);
      ASSERT_GE(Row[FieldReturn4], Row[FieldMemoryBase]);
      Row[FieldReturn4] -= Row[FieldMemoryBase];
      for (auto F : {FieldExceptionPC, FieldContextPC})
        EXPECT_EQ(Row[F], Row[FieldFaults] ? Row[FieldAtomicPC] : 0);
      if (Row[FieldFaults]) {
        ASSERT_GE(Row[FieldContext4], Row[FieldMemoryBase]);
        Row[FieldContext4] -= Row[FieldMemoryBase];
      }
      if (Row[FieldCode] == AccessViolation) {
        ASSERT_EQ(Row[FieldParameterCount], 2u);
        ASSERT_GE(Row[FieldParameter1], Row[FieldMemoryBase]);
        Row[FieldParameter1] -= Row[FieldMemoryBase];
      }
      for (auto F :
           {FieldAtomicPC, FieldExceptionPC, FieldContextPC, FieldMemoryBase})
        Row[F] = 0;
      std::array<uint8_t, FieldCount * sizeof(uint64_t)> Normalized;
      for (unsigned I = 0; I < FieldCount; ++I)
        llvm::support::endian::write64le(
            Normalized.data() + I * sizeof(uint64_t), Row[I]);
      Digest.update(Normalized);
    }
    EXPECT_EQ(llvm::toHex(Digest.final(), true), G.Digest);
  }
  EXPECT_TRUE(Bytes.empty());
#endif
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsAtomic, testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });
} // namespace
} // namespace neverd::emulation
