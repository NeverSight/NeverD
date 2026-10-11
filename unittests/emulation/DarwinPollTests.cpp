//===- DarwinPollTests.cpp - Literal immediate poll contracts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
namespace {
class DarwinPollTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinSystemOptions> System;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only poll"};
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 2, Read | Write | UserAccessible)));
    Options.emplace().Files["/data"] = {'a', 'b', 'c'};
    Options->WritableFiles.insert("/data");
    Options->SymbolicLinks["/alias"] = {'d', 'a', 't', 'a'};
    System.emplace().ResourceLimits[8] = {10240, 10240};
    Files = std::make_unique<DarwinFiles>(*Space, Options);
  }
  std::optional<ServiceResult> invoke(ServiceKind K,
                                      std::array<uint64_t, 6> A) {
    Result =
        ProcessResult{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                      ExecutionBackendKind::Unicorn, "memory-only poll"};
    auto Out = Files->handle(K, {0, 0, A, {}}, Result, System);
    EXPECT_TRUE(bool(Out)) << (Out ? "" : llvm::toString(Out.takeError()));
    return Out ? *Out : std::nullopt;
  }
  void returned(std::array<uint64_t, 6> A, uint64_t Value, bool Error = false) {
    auto O = invoke(ServiceKind::Poll, A);
    ASSERT_TRUE(O) << Result.Diagnostic;
    EXPECT_EQ(O->Value, Value);
    EXPECT_EQ(O->Error, Error);
  }
  void refused(std::array<uint64_t, 6> A, const char *Reason) {
    EXPECT_FALSE(invoke(ServiceKind::Poll, A));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, Reason);
  }
  uint32_t open(uint32_t Flags = 0, llvm::StringRef Path = "/data") {
    const auto Text = Path.str() + '\0';
    llvm::cantFail(Space->write(
        Base + Page,
        llvm::ArrayRef<uint8_t>((const uint8_t *)Text.data(), Text.size())));
    auto O = invoke(ServiceKind::Open, {Base + Page, Flags});
    EXPECT_TRUE(O) << Result.Diagnostic;
    EXPECT_FALSE(O && O->Error);
    return O ? uint32_t(O->Value) : UINT32_MAX;
  }
  void records(std::initializer_list<std::pair<int32_t, uint16_t>> Rows,
               uint64_t Address = Base + 32) {
    std::vector<uint8_t> B(Rows.size() * 8, 0x5a);
    size_t I = 0;
    for (const auto &[FD, Events] : Rows) {
      llvm::support::endian::write32le(B.data() + I * 8, uint32_t(FD));
      llvm::support::endian::write16le(B.data() + I * 8 + 4, Events);
      ++I;
    }
    llvm::cantFail(Space->write(Address, B));
  }
  void ready(std::initializer_list<std::pair<int32_t, uint16_t>> Rows,
             llvm::ArrayRef<uint16_t> Expected, uint64_t Count,
             uint64_t High = 0) {
    std::array<uint8_t, 64> B;
    B.fill(0xa5);
    llvm::cantFail(Space->write(Base + 16, B));
    records(Rows);
    llvm::cantFail(Space->read(Base + 16, B));
    returned({Base + 32, High | Rows.size(), High}, Count);
    std::array<uint8_t, 64> A;
    llvm::cantFail(Space->read(Base + 16, A));
    ASSERT_EQ(Rows.size(), Expected.size());
    for (size_t I = 0; I != A.size(); ++I) {
      if (I >= 16 && I < 16 + Rows.size() * 8 && (I - 16) % 8 >= 6)
        continue;
      EXPECT_EQ(A[I], B[I]) << I;
    }
    for (size_t I = 0; I != Expected.size(); ++I)
      EXPECT_EQ(llvm::support::endian::read16le(A.data() + 16 + I * 8 + 6),
                Expected[I])
          << I;
  }
};
TEST_P(DarwinPollTest,
       ResourceAdmissionKeepsCountPrivilegeAndBudgetIndependent) {
  Options->DescriptorLimit = 64;
  System.reset();
  returned({UINT64_MAX, 10241, 1}, 22, true);
  returned({UINT64_MAX, UINT32_MAX, 0}, 22, true);
  refused({UINT64_MAX, 1, 0}, diagnostic::PollNoFile);
  refused({UINT64_MAX, 1, 1}, diagnostic::PollNoFile);
  System.emplace();
  System->MaxFilesPerProcess = 10240;
  refused({UINT64_MAX, 1, 0}, diagnostic::PollNoFile);
  System->ResourceLimits[8] = {2, 10240};
  returned({UINT64_MAX, 2, 0}, 14, true);
  refused({UINT64_MAX, 1024, 0}, diagnostic::PollCredentials);
  refused({UINT64_MAX, 3, 0}, diagnostic::PollCredentials);
  returned({UINT64_MAX, 1025, 0}, 22, true);
  System->Credentials = DarwinCredentials{0, 501, 20, 20, {}};
  returned({UINT64_MAX, 3, 0}, 22, true);
  System->Credentials->RealUID = 501;
  System->Credentials->EffectiveUID = 0;
  returned({UINT64_MAX, 3, 0}, 14, true);
  returned({UINT64_MAX, 1024, 0}, 14, true);
  returned({UINT64_MAX, 1025, 0}, 22, true);
  const auto FD = open();
  ASSERT_GT(FD, 2u);
  ready({{int32_t(FD), 1}}, {1}, 1);
  EXPECT_EQ(Options->DescriptorLimit, 64u);
  EXPECT_EQ(System->ResourceLimits.at(8).Current, 2u);
}
TEST_P(DarwinPollTest, EmptyAndTimeoutKeepCarrierAndPointerRules) {
  System.reset();
  for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    returned({UINT64_MAX, High, High}, 0);
    returned({0, High, High}, 0);
    refused({0, High, High | 1}, diagnostic::PollWait);
    refused({0, High, High | UINT32_MAX}, diagnostic::PollWait);
    returned({UINT64_MAX, High | 10241, High | 1}, 22, true);
  }
  System.emplace().ResourceLimits[8] = {10240, 10240};
  refused({UINT64_MAX, 1, 1}, diagnostic::PollWait);
  returned({UINT64_MAX, 0xffffffff00000001ULL, 0xffffffff00000000ULL}, 14,
           true);
}
TEST_P(DarwinPollTest,
       EveryShortMaskIgnoresNegativeAndClassifiesClosedRegistrations) {
  // Independent published poll.h ABI literals, not the production mask owner.
  for (uint32_t Mask = 0; Mask != 65536; ++Mask) {
    for (int32_t FD : {-1, 12345}) {
      records({{FD, uint16_t(Mask)}});
      const uint16_t Expected = FD < 0 ? 0 : (Mask & 0x1fd7) ? 32 : 0;
      auto O = invoke(ServiceKind::Poll, {Base + 32, 1, 0});
      ASSERT_TRUE(O) << Result.Diagnostic;
      ASSERT_FALSE(O->Error);
      ASSERT_EQ(O->Value, Expected ? 1u : 0u) << Mask;
      auto Bytes = llvm::cantFail(Space->readInteger(Base + 32 + 6, 2));
      ASSERT_EQ(Bytes, Expected) << Mask;
    }
  }
  ready(
      {{-2147483647 - 1, 0xffff}, {12345, 0xe028}, {12345, 2}, {12345, 0x1e00}},
      {0, 0, 32, 32}, 2);
}
TEST_P(DarwinPollTest,
       RegularMasksDoNotInferReadinessFromAccessOffsetsOrFlags) {
  auto &Metadata = Options->Metadata["/data"];
  Metadata.Mode = 0100600;
  Metadata.Inode = 42;
  Metadata.LinkCount = 1;
  Metadata.Size = 3;
  Metadata.BlockSize = 512;
  Metadata.Blocks = 1;
  Metadata.AccessTime = {7, 8};
  Metadata.ModificationTime = {9, 10};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  const auto FD = open();
  const auto Snapshot = [&] {
    auto Stat = invoke(ServiceKind::Fstat64, {FD, Base + Page + 128});
    EXPECT_TRUE(Stat);
    EXPECT_FALSE(Stat && Stat->Error);
    std::array<uint8_t, 144> Bytes;
    llvm::cantFail(Space->read(Base + Page + 128, Bytes));
    return Bytes;
  };
  const auto OriginalStat = Snapshot();
  for (const auto &[Mask, Bits] :
       {std::pair{0u, 0u}, std::pair{1u, 1u}, std::pair{0x40u, 0x40u},
        std::pair{0x41u, 0x41u}, std::pair{4u, 4u}, std::pair{0x100u, 0x100u},
        std::pair{0x104u, 0x104u}, std::pair{0x145u, 0x145u},
        std::pair{16u, 0u}, std::pair{0xe028u, 0u}})
    ready({{int32_t(FD), uint16_t(Mask)}}, {uint16_t(Bits)}, Bits ? 1 : 0);
  auto Seek = invoke(ServiceKind::Lseek, {FD, 3, 0});
  ASSERT_TRUE(Seek);
  ASSERT_EQ(Seek->Value, 3u);
  auto Flags = invoke(ServiceKind::Fcntl, {FD, 4, 4});
  ASSERT_TRUE(Flags);
  ASSERT_FALSE(Flags->Error);
  ready({{int32_t(FD), 0x145}}, {0x145}, 1, 0x1234567800000000ULL);
  Seek = invoke(ServiceKind::Lseek, {FD, 0, 1});
  ASSERT_TRUE(Seek);
  EXPECT_EQ(Seek->Value, 3u);
  Flags = invoke(ServiceKind::Fcntl, {FD, 3});
  ASSERT_TRUE(Flags);
  EXPECT_EQ(Flags->Value, 4u);
  EXPECT_EQ(Options->Files.at("/data"), (std::vector<uint8_t>{'a', 'b', 'c'}));
  EXPECT_EQ(Snapshot(), OriginalStat);
  const auto Write = open(1);
  ready({{int32_t(Write), 1}}, {1}, 1);
}
TEST_P(DarwinPollTest, NumericDescriptorFiltersRetainSeparateLastIndices) {
  const auto FD = open();
  auto Dup = invoke(ServiceKind::Dup, {FD});
  ASSERT_TRUE(Dup);
  ASSERT_FALSE(Dup->Error);
  ready({{int32_t(FD), 1}, {int32_t(FD), 1}}, {0, 1}, 1);
  ready({{int32_t(FD), 1}, {int32_t(FD), 4}}, {1, 4}, 2);
  ready({{int32_t(FD), 5}, {int32_t(FD), 5}}, {0, 5}, 1);
  ready({{int32_t(FD), 5}, {int32_t(FD), 1}}, {4, 1}, 2);
  ready({{int32_t(FD), 1}, {int32_t(FD), 16}}, {0, 0}, 0);
  ready({{int32_t(FD), 1}, {int32_t(Dup->Value), 1}}, {1, 1}, 2);
  ready({{int32_t(FD), 1}, {int32_t(FD), 0}, {int32_t(FD), 4}}, {1, 0, 4}, 2);
  ready({{12345, 1}, {12345, 1}}, {32, 32}, 2);
}
TEST_P(DarwinPollTest, CloseReuseAndReplacementUseCurrentDescriptions) {
  const auto FD = open();
  auto Dup = invoke(ServiceKind::Dup, {FD});
  ASSERT_TRUE(Dup);
  auto Closed = invoke(ServiceKind::Close, {FD});
  ASSERT_TRUE(Closed);
  ASSERT_FALSE(Closed->Error);
  ready({{int32_t(FD), 1}, {int32_t(Dup->Value), 1}}, {32, 1}, 2);
  const auto Reused = open();
  ASSERT_EQ(Reused, FD);
  ready({{int32_t(FD), 1}}, {1}, 1);
  auto Replace = invoke(ServiceKind::Dup2, {1, FD});
  ASSERT_TRUE(Replace);
  ASSERT_FALSE(Replace->Error);
  records({{int32_t(FD), 4}});
  refused({Base + 32, 1, 0}, diagnostic::PollObject);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 38, 2)), 0x5a5au);
  ready({{int32_t(Dup->Value), 1}}, {1}, 1);
}
TEST_P(DarwinPollTest, WholeInputAndOutputPhasesPreserveUnsupportedPrefixes) {
  returned({UINT64_MAX, 1, 0}, 14, true);
  const uint64_t Tail = Base + Page * 2 - 8;
  records({{-1, 1}}, Tail);
  returned({Tail, 2, 0}, 14, true);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail + 6, 2)), 0x5a5au);
  records({{-1, 1}});
  llvm::cantFail(Space->protect(Base, Page, Read | UserAccessible));
  returned({Base + 32, 1, 0}, 14, true);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 38, 2)), 0x5a5au);
  llvm::cantFail(Space->protect(Base, Page, Read | Write | UserAccessible));
  const uint64_t Span = Base + Page - 8;
  records({{-1, 1}, {-1, 4}}, Span);
  llvm::cantFail(Space->protect(Base + Page, Page, Read | UserAccessible));
  refused({Span, 2, 0}, diagnostic::PollPartialOutput);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Span + 6, 2)), 0x5a5au);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Span + 14, 2)), 0x5a5au);
}
TEST_P(DarwinPollTest,
       UnknownObjectsEventsAndAuthorizationPublishNoReadyPrefix) {
  const auto FD = open();
  for (uint16_t Mask : {2u, 0x80u, 0x200u, 0x400u, 0x800u, 0x1000u}) {
    records({{int32_t(FD), 1}, {int32_t(FD), Mask}});
    refused({Base + 32, 2, 0}, diagnostic::PollEvents);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 38, 2)), 0x5a5au);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 46, 2)), 0x5a5au);
  }
  for (uint32_t Unknown :
       {0u, 1u, 2u, open(0, "/"), open(0x200000, "/alias")}) {
    records({{int32_t(FD), 1}, {int32_t(Unknown), 1}});
    refused({Base + 32, 2, 0}, diagnostic::PollObject);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 38, 2)), 0x5a5au);
    ready({{int32_t(Unknown), 0xe028}}, {0}, 0);
  }
  for (auto Auth : {DarwinFileAuthorization::StaticOwnerQueries,
                    DarwinFileAuthorization::StaticOrdinaryQueries}) {
    Options->WritableFiles.clear();
    Options->Authorization = Auth;
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    const auto *Reason = Auth == DarwinFileAuthorization::StaticOwnerQueries
                             ? diagnostic::FileAuthorizationScope
                             : diagnostic::FileOrdinaryAuthorizationScope;
    refused({UINT64_MAX, 0, 0}, Reason);
    refused({UINT64_MAX, 10241, 1}, Reason);
  }
}
INSTANTIATE_TEST_SUITE_P(OSPages, DarwinPollTest, testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
