//===- DarwinSystemTests.cpp - sysctl widths and ordered copies -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinSystemTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinEntropy.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"
#include "os/darwin/kernel/DarwinSystem.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
namespace {
class DarwinSystemTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000, Output = Base + 128,
                            Length = Base + 256;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinSystemOptions> Options = darwin_test::systemOptions();
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only test"};
  uint64_t Page;
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 2, Read | Write | UserAccessible)));
    fill();
  }
  void fill() {
    ASSERT_FALSE(
        bool(Space->write(Base, std::vector<uint8_t>(Page * 2, 0xa5))));
  }
  void put(uint64_t Address, llvm::StringRef Bytes) {
    ASSERT_FALSE(
        bool(Space->write(Address, llvm::arrayRefFromStringRef(Bytes))));
  }
  void capacity(uint64_t Value) {
    ASSERT_FALSE(bool(Space->writeInteger(Length, Value, 8)));
  }
  uint64_t length() { return llvm::cantFail(Space->readInteger(Length, 8)); }
  std::string bytes(uint64_t Address, size_t Count) {
    std::vector<uint8_t> Data(Count);
    auto E = Space->read(Address, Data);
    EXPECT_FALSE(bool(E));
    llvm::consumeError(std::move(E));
    return std::string(Data.begin(), Data.end());
  }
  std::optional<ServiceResult> invoke(ServiceKind Kind,
                                      std::array<uint64_t, 6> Arguments) {
    auto Out = systemService(*Space, Page, Kind,
                             {0, 0, Arguments, std::nullopt}, Options, Result);
    EXPECT_TRUE(bool(Out)) << (Out ? "" : llvm::toString(Out.takeError()));
    return Out ? *Out : std::nullopt;
  }
  std::optional<ServiceResult> named(llvm::StringRef Name,
                                     uint64_t Old = Output,
                                     uint64_t Len = Length, uint64_t New = 0,
                                     uint64_t NewLen = 0) {
    put(Base, Name);
    return invoke(ServiceKind::SysctlByName,
                  {Base, Name.size(), Old, Len, New, NewLen});
  }
  std::optional<ServiceResult> mib(uint32_t Root, uint32_t Leaf,
                                   uint64_t Old = Output) {
    EXPECT_FALSE(bool(Space->writeInteger(Base, Root, 4)));
    EXPECT_FALSE(bool(Space->writeInteger(Base + 4, Leaf, 4)));
    return invoke(ServiceKind::Sysctl, {Base, 2, Old, Length});
  }
  void result(std::optional<ServiceResult> Out, uint64_t Error = 0) {
    ASSERT_TRUE(Out) << Result.Diagnostic;
    EXPECT_EQ(Out->Error, Error != 0);
    EXPECT_EQ(Out->Value, Error);
  }
};

TEST_P(DarwinSystemTest, NamedAndStableNumericBindingsHaveIndependentBytes) {
  struct Sample {
    const char *Name;
    uint32_t Root, Leaf;
    const char *Hex;
  };
  const Sample Samples[] = {
      {"kern.ostype", 1, 1, "44617277696e00"},
      {"kern.osrelease", 1, 2, "32342e7465737400"},
      {"kern.osrevision", 1, 3, "00000080"},
      {"kern.version", 1, 4, "4e6576657244207669727475616c206b65726e656c00"},
      {"kern.osversion", 1, 65, "56343200"},
      {"hw.machine", 6, 1, "7669727475616c363400"},
      {"hw.model", 6, 2, "5669727475616c4d6f64656c00"},
      {"hw.ncpu", 6, 3, "07000000"},
      {"hw.memsize", 6, 24, "1032547698badcfe"}};
  std::string Combined;
  for (const auto &S : Samples) {
    SCOPED_TRACE(S.Name);
    const auto Expected = llvm::fromHex(S.Hex);
    Combined += Expected;
    for (bool Named : {true, false}) {
      fill();
      capacity(Expected.size());
      result(Named ? named(S.Name, Output + 1)
                   : mib(S.Root, S.Leaf, Output + 1));
      EXPECT_EQ(length(), Expected.size());
      EXPECT_EQ(bytes(Output + 1, Expected.size()), Expected);
      EXPECT_EQ(bytes(Output, 1), std::string(1, '\xa5'));
      EXPECT_EQ(bytes(Output + 1 + Expected.size(), 8), std::string(8, '\xa5'));
    }
  }
  EXPECT_EQ(Combined, llvm::fromHex(darwin_test::SystemHex));
}

TEST_P(DarwinSystemTest, PagePolicyDistinguishesNamedQuadFromLegacyInteger) {
  Options.reset();
  const auto PageHex = Page == 4096 ? "00100000" : "00400000";
  for (unsigned Cap : {0, 3, 4, 5, 7, 8, 9}) {
    for (unsigned Kind : {0, 1, 2}) {
      fill();
      capacity(Cap);
      const unsigned Size = Kind != 0 || Cap == 4 ? 4 : 8;
      result(Kind == 0   ? named("hw.pagesize")
             : Kind == 1 ? named("hw.pagesize_compat")
                         : mib(6, 7),
             Cap < Size ? 12 : 0);
      EXPECT_EQ(length(), Cap < Size ? 0u : Size);
      EXPECT_EQ(bytes(Output, Size),
                Cap < Size ? std::string(Size, '\xa5')
                           : llvm::fromHex(PageHex) + std::string(Size - 4, 0));
      EXPECT_EQ(bytes(Output + Size, 8), std::string(8, '\xa5'));
    }
  }
  capacity(4);
  result(named("hw.pagesize", 0));
  EXPECT_EQ(length(), 8u);
}

TEST_P(DarwinSystemTest,
       QuadNarrowingUsesSignedBitsAndRangeErrorsPreserveLength) {
  for (uint64_t Value :
       {0ULL, 0x7fffffffULL, 0xffffffff80000000ULL, 0xffffffffffffffffULL,
        0x80000000ULL, 0x100000000ULL, 0xffffffff7fffffffULL}) {
    SCOPED_TRACE(Value);
    Options->MemorySize = Value;
    const bool Fits = Value == 0 || Value == INT32_MAX ||
                      Value == 0xffffffff80000000ULL || Value == UINT64_MAX;
    for (bool Named : {true, false}) {
      fill();
      capacity(4);
      result(Named ? named("hw.memsize") : mib(6, 24), Fits ? 0 : 34);
      EXPECT_EQ(length(), 4u);
      EXPECT_EQ(llvm::cantFail(Space->readInteger(Output, 4)),
                Fits ? uint32_t(Value) : 0xa5a5a5a5u);
      EXPECT_EQ(bytes(Output + 4, 8), std::string(8, '\xa5'));
      capacity(7);
      result(named("hw.memsize"), 12);
      EXPECT_EQ(length(), 0u);
      capacity(8);
      result(named("hw.memsize"));
      EXPECT_EQ(llvm::cantFail(Space->readInteger(Output, 8)), Value);
    }
  }
}

TEST_P(DarwinSystemTest, StringCapacitiesSizeOnlyAndNullLengthFollowCopyOrder) {
  for (unsigned Cap = 0; Cap != 10; ++Cap) {
    fill();
    capacity(Cap);
    result(named("kern.ostype"), Cap < 7 ? 12 : 0);
    EXPECT_EQ(length(), Cap < 7 ? 0u : 7u);
    EXPECT_EQ(bytes(Output, 7),
              Cap < 7 ? std::string(7, '\xa5') : std::string("Darwin\0", 7));
    EXPECT_EQ(bytes(Output + 7, 8), std::string(8, '\xa5'));
    capacity(Cap);
    result(named("kern.ostype", 0));
    EXPECT_EQ(length(), 7u);
  }
  fill();
  result(named("kern.ostype", Output, 0), 12);
  result(named("kern.ostype", 0, 0));
  EXPECT_EQ(bytes(Output, 16), std::string(16, '\xa5'));
  Options->OSType = "";
  capacity(1);
  result(named("kern.ostype"));
  EXPECT_EQ(length(), 1u);
  EXPECT_EQ(bytes(Output, 2), std::string("\0\xa5", 2));
  Options->OSType = std::string(1023, 'x');
  capacity(1024);
  result(named("kern.ostype", Base + 4093));
  EXPECT_EQ(length(), 1024u);
  EXPECT_EQ(bytes(Base + 4093, 1025),
            std::string(1023, 'x') + std::string("\0\xa5", 2));
}

TEST_P(DarwinSystemTest,
       MissingObservationsAndUnknownKeysNeverAcquireDefaults) {
  Options.reset();
  for (bool Present : {false, true}) {
    if (Present)
      Options.emplace();
    capacity(100);
    EXPECT_FALSE(named("kern.ostype"));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemObservation);
    EXPECT_FALSE(named("kern.ostype", 0, 0));
    EXPECT_FALSE(mib(6, 24));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemObservation);
    EXPECT_FALSE(named("kern.unmodeled"));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemKey);
    EXPECT_FALSE(mib(0, 0));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemKey);
    EXPECT_EQ(length(), 100u);
    EXPECT_EQ(bytes(Output, 16), std::string(16, '\xa5'));
  }
  Options->OSType = "";
  Options->MemorySize = 0;
  Options->OSRevision = 0;
  result(named("kern.ostype"));
  EXPECT_EQ(length(), 1u);
  capacity(8);
  result(mib(6, 24));
  EXPECT_EQ(bytes(Output, 8), std::string(8, 0));
  result(mib(1, 3));
  EXPECT_EQ(length(), 4u);
}

TEST_P(DarwinSystemTest, WritesRequireBothNewArgumentsAndPrecedeValueOrOutput) {
  for (const char *Name :
       {"kern.ostype", "kern.osversion", "hw.memsize", "hw.pagesize"}) {
    capacity(32);
    result(named(Name, Output, Length, UINT64_MAX, 0));
    capacity(32);
    result(named(Name, Output, Length, 0, UINT64_MAX));
    for (bool Missing : {false, true}) {
      if (Missing)
        Options.reset();
      capacity(0);
      const auto Before = bytes(Output, 32);
      result(named(Name, UINT64_MAX, Length, UINT64_MAX, 1), 1);
      EXPECT_EQ(length(), 0u);
      EXPECT_EQ(bytes(Output, 32), Before);
    }
    Options = darwin_test::systemOptions();
  }
}

TEST_P(DarwinSystemTest, LengthLimitsAndHighCarriersPrecedeInputMemory) {
  for (auto Count : {0ULL, 1ULL, 13ULL, 0xffffffffffffffffULL})
    result(invoke(ServiceKind::Sysctl, {UINT64_MAX, Count, 1, 1, 1, 1}), 22);
  for (auto Count : {1024ULL, 0x100000000ULL, 0xffffffffffffffffULL})
    result(invoke(ServiceKind::SysctlByName, {UINT64_MAX, Count, 1, 1, 1, 1}),
           63);
  for (auto Bad : {uint64_t(1), value::UserLimit, UINT64_MAX}) {
    result(invoke(ServiceKind::Sysctl, {Bad, 2, 1, 1, 1, 1}), 14);
    result(invoke(ServiceKind::SysctlByName, {Bad, 1, 1, 1, 1, 1}), 14);
    capacity(8);
    result(invoke(ServiceKind::SysctlByName, {Bad, 0, 1, Length, 1, 1}), 2);
    EXPECT_EQ(length(), 8u);
  }
  capacity(4);
  result(mib(6, 3));
  capacity(4);
  result(invoke(ServiceKind::Sysctl,
                {Base, 0xfedcba9800000002ULL, Output, Length}));
  EXPECT_EQ(bytes(Output, 4), llvm::fromHex("07000000"));
  capacity(8);
  EXPECT_FALSE(invoke(ServiceKind::Sysctl, {Base, 12, Output, Length}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemKey);
  EXPECT_EQ(length(), 8u);
}

TEST_P(DarwinSystemTest, FirstNULAndOneTrailingDotStillRequireAllInputBytes) {
  for (auto Name :
       {std::string("kern.ostype."), std::string("kern.ostype\0junk", 16),
        std::string("kern.ostype.\0junk", 17)}) {
    capacity(8);
    result(named(Name));
    EXPECT_EQ(bytes(Output, 7), std::string("Darwin\0", 7));
  }
  capacity(8);
  EXPECT_FALSE(named("kern.ostype.."));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemKey);
  std::string Full("kern.ostype\0", 12);
  Full.resize(1023, 'x');
  result(named(Full));
  const auto End = Base + 2 * Page;
  put(End - 12, std::string("kern.ostype\0", 12));
  capacity(8);
  EXPECT_FALSE(
      invoke(ServiceKind::SysctlByName, {End - 12, 13, Output, Length}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemPartialInput);
  EXPECT_EQ(length(), 8u);
}

TEST_P(DarwinSystemTest,
       InaccessibleLengthIsExplicitlyOutsideSupportedBoundary) {
  for (auto Bad :
       {uint64_t(1), value::UserLimit, UINT64_MAX, Base + 2 * Page - 4}) {
    EXPECT_FALSE(named("kern.ostype", Output, Bad, 1, 1));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemLengthMemory);
    EXPECT_EQ(bytes(Output, 16), std::string(16, '\xa5'));
  }
  for (auto Permissions :
       {unsigned(Read | UserAccessible), unsigned(Write | UserAccessible),
        unsigned(Read | Write)}) {
    ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Permissions)));
    EXPECT_FALSE(named("kern.ostype", Output, Base + Page));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemLengthMemory);
    ASSERT_FALSE(
        bool(Space->protect(Base + Page, Page, Read | Write | UserAccessible)));
  }
}

TEST_P(DarwinSystemTest,
       OutputFaultsPreserveLengthAndShortCopyAvoidsOutputFault) {
  Options->MemorySize = 0x80000000;
  capacity(4);
  result(named("hw.memsize", UINT64_MAX), 34);
  EXPECT_EQ(length(), 4u);
  EXPECT_EQ(bytes(Output, 8), std::string(8, '\xa5'));
  for (auto Bad : {uint64_t(1), value::UserLimit, UINT64_MAX}) {
    capacity(8);
    result(named("kern.ostype", Bad), 14);
    EXPECT_EQ(length(), 8u);
    capacity(6);
    result(named("kern.ostype", Bad), 12);
    EXPECT_EQ(length(), 0u);
    capacity(8);
    result(named("kern.ostype", Bad, Length, UINT64_MAX, 0), 14);
    EXPECT_EQ(length(), 8u);
    capacity(6);
    result(named("kern.ostype", Bad, Length, UINT64_MAX, 0), 12);
    EXPECT_EQ(length(), 0u);
  }
  const auto End = Base + 2 * Page;
  capacity(8);
  EXPECT_FALSE(named("kern.ostype", End - 4));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemPartialOutput);
  EXPECT_EQ(length(), 8u);
  EXPECT_EQ(bytes(End - 4, 4), std::string(4, '\xa5'));
  for (auto Permissions :
       {unsigned(Read | UserAccessible), unsigned(Read | Write)}) {
    ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Permissions)));
    result(named("kern.ostype", Base + Page), 14);
    EXPECT_EQ(length(), 8u);
    ASSERT_FALSE(
        bool(Space->protect(Base + Page, Page, Read | Write | UserAccessible)));
  }
}

TEST_P(DarwinSystemTest, AliasesCaptureInputAndCapacityBeforeDataThenLength) {
  capacity(64);
  result(named("kern.version", Length, Length));
  EXPECT_EQ(length(), 22u);
  EXPECT_EQ(bytes(Length + 8, 14), std::string("irtual kernel\0", 14));
  EXPECT_EQ(bytes(Length + 22, 8), std::string(8, '\xa5'));
  capacity(64);
  result(named("kern.ostype", Base));
  EXPECT_EQ(bytes(Base, 7), std::string("Darwin\0", 7));
  capacity(64);
  result(mib(6, 24, Base));
  EXPECT_EQ(bytes(Base, 8), llvm::fromHex("1032547698badcfe"));
  put(Base, "kern.ostype");
  // Both MIB words also encode a large capacity. The final length overwrites
  // them only after [6,3] has selected the CPU count and copied its value.
  ASSERT_FALSE(bool(Space->writeInteger(Base + 32, 0x300000006ULL, 8)));
  result(invoke(ServiceKind::Sysctl, {Base + 32, 2, Output, Base + 32}));
  EXPECT_EQ(bytes(Output, 4), llvm::fromHex("07000000"));
  EXPECT_EQ(bytes(Base + 32, 8), llvm::fromHex("0400000000000000"));
}

// Failures are transport errors, not invented guest errno. Delegation keeps
// earlier real copies observable when the later length write fails.
class FailingSystemMemory : public GuestMemory {
  GuestMemory &Memory;

public:
  unsigned FailAccess = 0, FailRead = 0, FailWrite = 0;
  mutable unsigned Accesses = 0;
  unsigned Reads = 0, Writes = 0;
  explicit FailingSystemMemory(GuestMemory &Memory) : Memory(Memory) {}
  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return failure("map");
  }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return failure("protect");
  }
  llvm::Expected<bool> canAccess(uint64_t A, uint64_t N,
                                 unsigned P) const override {
    if (++Accesses == FailAccess)
      return failure("transport access");
    return Memory.canAccess(A, N, P);
  }
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) override {
    if (++Reads == FailRead)
      return failure("transport read");
    return Memory.read(A, B);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    if (++Writes == FailWrite)
      return failure("transport write");
    return Memory.write(A, B);
  }
};
TEST_P(DarwinSystemTest,
       ThreadIdentityIsExplicitFullWidthAndMemoryIndependent) {
  FailingSystemMemory Memory(*Space);
  Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
  const auto Before = bytes(Base, Page * 2);
  const std::array<uint64_t, 6> Arguments = {
      UINT64_MAX, uint64_t(1) << 63, Output,
      Length,     value::UserLimit,  0x123456789abcdef0ULL};
  for (bool Present : {false, true}) {
    Options.reset();
    if (Present)
      Options.emplace();
    auto R = systemService(Memory, Page, ServiceKind::ThreadSelfID,
                           {0, 372, Arguments, std::nullopt}, Options, Result);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_FALSE(*R);
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, diagnostic::ThreadIDObservation);
  }
  for (uint64_t ID : {0ULL, 1ULL, 0x100000001ULL, 0x8000000000000000ULL,
                      0xfedcba9876543210ULL, 0xffffffffffffffffULL}) {
    SCOPED_TRACE(ID);
    Options->ThreadID = ID;
    EXPECT_FALSE(bool(validateSystemOptions(*Options)));
    for (unsigned Repeat = 0; Repeat != 3; ++Repeat) {
      auto R =
          systemService(Memory, Page, ServiceKind::ThreadSelfID,
                        {0, 372, Arguments, std::nullopt}, Options, Result);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_TRUE(*R);
      EXPECT_EQ((**R).Value, ID);
      EXPECT_FALSE((**R).Error);
      EXPECT_EQ((**R).Convention, ServiceConvention::BSD);
      EXPECT_EQ(Options->ThreadID, ID);
    }
    std::optional<DarwinSystemOptions> Other =
        darwin_test::threadIdentityOptions();
    auto R = systemService(Memory, Page, ServiceKind::ThreadSelfID,
                           {0, 372, Arguments, std::nullopt}, Other, Result);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_TRUE(*R);
    EXPECT_EQ((**R).Value, 0xfedcba9876543210ULL);
    EXPECT_EQ(Options->ThreadID, ID);
  }
  EXPECT_EQ(Memory.Accesses, 0u);
  EXPECT_EQ(Memory.Reads, 0u);
  EXPECT_EQ(Memory.Writes, 0u);
  EXPECT_EQ(bytes(Base, Page * 2), Before);
}

TEST_P(DarwinSystemTest,
       TransportFailuresKeepCompletedCopiesAndDoNotBecomeErrno) {
  for (unsigned Phase = 0; Phase != 3; ++Phase) {
    for (unsigned At = 1; At <= (Phase == 0 ? 4u : 2u); ++At) {
      fill();
      capacity(8);
      put(Base, "kern.ostype");
      FailingSystemMemory Memory(*Space);
      (Phase == 0   ? Memory.FailAccess
       : Phase == 1 ? Memory.FailRead
                    : Memory.FailWrite) = At;
      auto Out = systemService(
          Memory, Page, ServiceKind::SysctlByName,
          {0, 274, {Base, 11, Output, Length}, std::nullopt}, Options, Result);
      ASSERT_FALSE(bool(Out));
      EXPECT_EQ(llvm::toString(Out.takeError()), Phase == 0 ? "transport access"
                                                 : Phase == 1
                                                     ? "transport read"
                                                     : "transport write");
      const bool Copied = (Phase == 0 && At == 4) || (Phase == 2 && At == 2);
      EXPECT_EQ(bytes(Output, 7),
                Copied ? std::string("Darwin\0", 7) : std::string(7, '\xa5'));
      EXPECT_EQ(length(), 8u);
    }
  }
}

TEST(DarwinSystemOptions, TypedFieldsUseTheSameBoundedContract) {
  auto O = darwin_test::systemOptions();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  for (auto Member :
       {&DarwinSystemOptions::OSType, &DarwinSystemOptions::OSRelease,
        &DarwinSystemOptions::OSVersion, &DarwinSystemOptions::KernelVersion,
        &DarwinSystemOptions::Machine, &DarwinSystemOptions::Model}) {
    for (const auto &Bad : {std::string(1024, 'x'), std::string("x\0y", 3)}) {
      O.*Member = Bad;
      EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
                diagnostic::SystemString);
    }
    O.*Member = std::string(1023, 'x');
    EXPECT_FALSE(bool(validateSystemOptions(O)));
    O.*Member = "";
    EXPECT_FALSE(bool(validateSystemOptions(O)));
  }
  for (auto Bad : {0u, 0x80000000u, UINT32_MAX}) {
    O.CPUCount = Bad;
    EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
              diagnostic::SystemCPUCount);
  }
  O.CPUCount = INT32_MAX;
  O.MemorySize = UINT64_MAX;
  EXPECT_FALSE(bool(validateSystemOptions(O)));
}

TEST_P(DarwinSystemTest, HostNameTruncatesPositiveCapacityWithNamedMIBParity) {
  Options->HostName = "abcd";
  struct Sample {
    uint64_t Capacity;
    const char *Hex;
    uint64_t Error;
  };
  for (const Sample &S :
       {Sample{0, "", 12}, Sample{1, "00", 0}, Sample{2, "6100", 0},
        Sample{3, "616200", 0}, Sample{4, "61626300", 0},
        Sample{5, "6162636400", 0}, Sample{6, "6162636400", 0},
        Sample{UINT64_MAX, "6162636400", 0}}) {
    for (bool Named : {false, true}) {
      SCOPED_TRACE(S.Capacity);
      fill();
      capacity(S.Capacity);
      result(Named ? named("kern.hostname", Output + 1)
                   : mib(1, 10, Output + 1),
             S.Error);
      const auto Expected = llvm::fromHex(S.Hex);
      EXPECT_EQ(length(), Expected.size());
      EXPECT_EQ(bytes(Output, 1), std::string(1, '\xa5'));
      EXPECT_EQ(bytes(Output + 1, Expected.size()), Expected);
      EXPECT_EQ(bytes(Output + 1 + Expected.size(), 8), std::string(8, '\xa5'));
      capacity(S.Capacity);
      result(Named ? named("kern.hostname", 0) : mib(1, 10, 0));
      EXPECT_EQ(length(), 5u);
    }
  }
  // The other string nodes retain their existing short-buffer refusal.
  capacity(1);
  result(named("kern.ostype"), 12);
  EXPECT_EQ(length(), 0u);
}

TEST_P(DarwinSystemTest, HostNameMissingEmptyAndMaximumRemainIndependent) {
  for (bool Present : {false, true}) {
    Options.reset();
    if (Present)
      Options.emplace();
    fill();
    capacity(8);
    EXPECT_FALSE(named("kern.hostname"));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemObservation);
    EXPECT_EQ(length(), 8u);
    EXPECT_EQ(bytes(Output, 8), std::string(8, '\xa5'));
  }
  Options->HostName = "";
  capacity(1);
  result(mib(1, 10));
  EXPECT_EQ(length(), 1u);
  EXPECT_EQ(bytes(Output, 2), std::string("\0\xa5", 2));
  capacity(0);
  result(named("kern.hostname"), 12);
  EXPECT_EQ(length(), 0u);
  Options->HostName = std::string(255, 'x');
  fill();
  capacity(UINT64_MAX);
  result(named("kern.hostname", Base + Page - 3));
  EXPECT_EQ(length(), 256u);
  EXPECT_EQ(bytes(Base + Page - 3, 257),
            std::string(255, 'x') + std::string("\0\xa5", 2));
}

TEST_P(DarwinSystemTest, HostNameFaultsCheckOnlyActualSpanAndPreserveLength) {
  Options->HostName = "abcd";
  for (auto Bad : {uint64_t(1), value::UserLimit, UINT64_MAX}) {
    fill();
    capacity(1);
    result(named("kern.hostname", Bad), 14);
    EXPECT_EQ(length(), 1u);
    capacity(0);
    result(named("kern.hostname", Bad), 12);
    EXPECT_EQ(length(), 0u);
    EXPECT_EQ(bytes(Output, 8), std::string(8, '\xa5'));
  }
  const auto End = Base + 2 * Page;
  fill();
  capacity(1);
  result(named("kern.hostname", End - 1));
  EXPECT_EQ(length(), 1u);
  EXPECT_EQ(bytes(End - 2, 2), std::string("\xa5\0", 2));
  fill();
  capacity(UINT64_MAX);
  result(named("kern.hostname", End - 5));
  EXPECT_EQ(length(), 5u);
  EXPECT_EQ(bytes(End - 6, 6), std::string("\xa5"
                                           "abcd\0",
                                           6));
  fill();
  capacity(2);
  EXPECT_FALSE(named("kern.hostname", End - 1));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemPartialOutput);
  EXPECT_EQ(bytes(End - 1, 1), std::string(1, '\xa5'));
  EXPECT_EQ(length(), 2u);
  for (auto Permissions :
       {unsigned(Read | UserAccessible), unsigned(Read | Write)}) {
    ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Permissions)));
    capacity(2);
    result(named("kern.hostname", Base + Page), 14);
    EXPECT_EQ(length(), 2u);
    ASSERT_FALSE(
        bool(Space->protect(Base + Page, Page, Read | Write | UserAccessible)));
  }
}

TEST_P(DarwinSystemTest, HostNameReadNullsAliasesAndWritesKeepPreflightOrder) {
  Options->HostName = "abcd";
  for (auto Cap : {uint64_t(1), uint64_t(2), uint64_t(5), UINT64_MAX}) {
    fill();
    capacity(Cap);
    result(named("kern.hostname", Length, Length));
    EXPECT_EQ(length(), std::min(Cap, uint64_t(5)));
    EXPECT_EQ(bytes(Length + 8, 8), std::string(8, '\xa5'));
  }
  fill();
  result(named("kern.hostname", Output, 0), 12);
  result(named("kern.hostname", 0, 0));
  EXPECT_EQ(bytes(Output, 8), std::string(8, '\xa5'));
  for (bool Root : {false, true}) {
    for (bool Missing : {false, true}) {
      Options.emplace();
      if (!Missing)
        Options->HostName = "abcd";
      if (Root)
        Options->Credentials.emplace();
      fill();
      capacity(2);
      auto Out = named("kern.hostname", UINT64_MAX, Length, UINT64_MAX, 1);
      if (Root) {
        EXPECT_FALSE(Out);
        EXPECT_EQ(Result.Diagnostic, diagnostic::SystemPrivilegedWrite);
      } else
        result(Out, 1);
      EXPECT_EQ(length(), 2u);
      EXPECT_EQ(bytes(Output, 8), std::string(8, '\xa5'));
      EXPECT_FALSE(
          named("kern.hostname", Output, Base + 2 * Page - 4, UINT64_MAX, 1));
      EXPECT_EQ(Result.Diagnostic, diagnostic::SystemLengthMemory);
      auto Input = invoke(ServiceKind::SysctlByName,
                          {UINT64_MAX, 13, Output, Length, UINT64_MAX, 1});
      result(Input, 14);
    }
  }
  Options.emplace().HostName = "abcd";
  for (const auto &[New, NewLen] : {std::pair{uint64_t(0), uint64_t(0)},
                                    std::pair{uint64_t(0), uint64_t(1)},
                                    std::pair{UINT64_MAX, uint64_t(0)}}) {
    capacity(2);
    result(named("kern.hostname", Output, Length, New, NewLen));
    EXPECT_EQ(bytes(Output, 3), std::string("a\0\xa5", 3));
    EXPECT_EQ(length(), 2u);
  }
}

TEST_P(DarwinSystemTest, HostNameTransportFailuresKeepCompletedDataCopy) {
  Options->HostName = "abcd";
  for (unsigned Phase = 0; Phase != 3; ++Phase) {
    for (unsigned At = 1; At <= (Phase == 0 ? 4u : 2u); ++At) {
      fill();
      capacity(2);
      put(Base, "kern.hostname");
      FailingSystemMemory Memory(*Space);
      (Phase == 0   ? Memory.FailAccess
       : Phase == 1 ? Memory.FailRead
                    : Memory.FailWrite) = At;
      auto Out = systemService(
          Memory, Page, ServiceKind::SysctlByName,
          {0, 274, {Base, 13, Output, Length}, std::nullopt}, Options, Result);
      ASSERT_FALSE(bool(Out));
      EXPECT_EQ(llvm::toString(Out.takeError()), Phase == 0 ? "transport access"
                                                 : Phase == 1
                                                     ? "transport read"
                                                     : "transport write");
      const bool Copied = (Phase == 0 && At == 4) || (Phase == 2 && At == 2);
      EXPECT_EQ(bytes(Output, 2),
                Copied ? std::string("a\0", 2) : std::string(2, '\xa5'));
      EXPECT_EQ(length(), 2u);
      EXPECT_EQ(Memory.Writes, Copied && Phase == 0 ? 1u
                               : Phase == 2         ? At
                                                    : 0u);
    }
  }
  fill();
  capacity(2);
  put(Base, "kern.hostname");
  FailingSystemMemory Memory(*Space);
  auto Out = systemService(Memory, Page, ServiceKind::SysctlByName,
                           {0, 274, {Base, 13, Output, Length}, std::nullopt},
                           Options, Result);
  ASSERT_TRUE(bool(Out));
  ASSERT_TRUE(*Out);
  EXPECT_FALSE((**Out).Error);
  EXPECT_EQ(Memory.Writes, 2u); // One complete data transaction, then length.
}

TEST(DarwinSystemOptions, HostNameHasItsOwnByteLimitWithoutOtherDefaults) {
  DarwinSystemOptions O;
  for (const auto &Good : {std::string(), std::string(255, 'x')}) {
    O.HostName = Good;
    EXPECT_FALSE(bool(validateSystemOptions(O)));
    EXPECT_FALSE(O.Machine);
    EXPECT_FALSE(O.Credentials);
  }
  for (const auto &Bad : {std::string(256, 'x'), std::string("x\0y", 3)}) {
    O.HostName = Bad;
    EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
              diagnostic::SystemHostName);
  }
}

TEST_P(DarwinSystemTest, ProcessScalarsRequireOnlyTheirOwnObservation) {
  for (unsigned Configuration = 0; Configuration != 6; ++Configuration) {
    Options.reset();
    if (Configuration) {
      Options.emplace();
      if (Configuration == 2)
        Options->ProcessGroupID = 7;
      if (Configuration == 3)
        Options->SessionID = 9;
      if (Configuration >= 4)
        Options->ProcessTainted = Configuration == 5;
    }
    struct Sample {
      ServiceKind Kind;
      unsigned Number;
      uint64_t PID;
      std::optional<uint64_t> Expected;
      const char *Missing;
    };
    const Sample Samples[] = {
        {ServiceKind::GetPgrp, 81, UINT64_MAX,
         Configuration == 2 ? std::optional<uint64_t>(7) : std::nullopt,
         "Darwin process group observation is not configured"},
        {ServiceKind::GetPGID, 151, 0xffffffff000003e8ULL,
         Configuration == 2 ? std::optional<uint64_t>(7) : std::nullopt,
         "Darwin process group observation is not configured"},
        {ServiceKind::GetSID, 310, 0x100000000ULL,
         Configuration == 3 ? std::optional<uint64_t>(9) : std::nullopt,
         "Darwin process session observation is not configured"},
        {ServiceKind::IsSetUGID, 327, UINT64_MAX,
         Configuration >= 4 ? std::optional<uint64_t>(Configuration == 5)
                            : std::nullopt,
         "Darwin process taint observation is not configured"}};
    for (const auto &S : Samples) {
      SCOPED_TRACE(Configuration);
      SCOPED_TRACE(S.Number);
      Result = {ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                ExecutionBackendKind::Unicorn, "independent process scalar"};
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
      auto Out = systemService(
          Memory, Page, S.Kind,
          {0, S.Number, {S.PID, 1, UINT64_MAX, Output, Length, UINT64_MAX}, {}},
          Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      if (S.Expected) {
        ASSERT_TRUE(*Out) << Result.Diagnostic;
        EXPECT_EQ((**Out).Value, *S.Expected);
        EXPECT_FALSE((**Out).Error);
      } else {
        EXPECT_FALSE(*Out);
        EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
        EXPECT_EQ(Result.Diagnostic, S.Missing);
      }
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
      EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
    }
  }
}

TEST_P(DarwinSystemTest, ProcessPIDQueriesUseSignedLow32BeforeObservation) {
  const uint64_t Self[] = {0,
                           1000,
                           0x100000000ULL,
                           0xffffffff00000000ULL,
                           0x12345678000003e8ULL,
                           0xffffffff000003e8ULL};
  const uint64_t Negative[] = {0xffffffffULL, UINT64_MAX, 0x80000000ULL,
                               0xffffffff80000000ULL};
  const uint64_t Peer[] = {1, 4096, INT32_MAX, 0x100000001ULL,
                           0xffffffff00001000ULL};
  for (bool Known : {false, true}) {
    Options.reset();
    if (Known) {
      Options.emplace().ProcessGroupID = 1;
      Options->SessionID = INT32_MAX;
    }
    for (bool Session : {false, true})
      for (unsigned Domain = 0; Domain != 3; ++Domain) {
        llvm::ArrayRef<uint64_t> Targets =
            Domain == 0   ? llvm::ArrayRef<uint64_t>(Self)
            : Domain == 1 ? llvm::ArrayRef<uint64_t>(Negative)
                          : llvm::ArrayRef<uint64_t>(Peer);
        for (auto PID : Targets) {
          SCOPED_TRACE(PID);
          FailingSystemMemory Memory(*Space);
          Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
          Result = {ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                    ExecutionBackendKind::Unicorn, "signed pid_t oracle"};
          auto Out = systemService(Memory, Page,
                                   Session ? ServiceKind::GetSID
                                           : ServiceKind::GetPGID,
                                   {0,
                                    Session ? 310u : 151u,
                                    {PID, UINT64_MAX, 1, Output, Length},
                                    {}},
                                   Options, Result);
          ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
          if (Domain == 1 || (Domain == 0 && Known)) {
            ASSERT_TRUE(*Out) << Result.Diagnostic;
            EXPECT_EQ((**Out).Value, Domain == 1 ? 3u
                                     : Session   ? uint32_t(INT32_MAX)
                                                 : 1u);
            EXPECT_EQ((**Out).Error, Domain == 1);
          } else {
            EXPECT_FALSE(*Out);
            EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
            EXPECT_EQ(
                Result.Diagnostic,
                Domain == 2 ? "Darwin other-process queries are not modeled"
                : Session
                    ? "Darwin process session observation is not configured"
                    : "Darwin process group observation is not configured");
          }
          EXPECT_EQ(Memory.Accesses, 0u);
          EXPECT_EQ(Memory.Reads, 0u);
          EXPECT_EQ(Memory.Writes, 0u);
        }
      }
  }
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
}

TEST_P(DarwinSystemTest, ProcessTaintIsIndependentOfCredentialsAndAuthority) {
  for (bool Tainted : {false, true})
    for (unsigned Credentials = 0; Credentials != 3; ++Credentials) {
      Options.emplace().ProcessTainted = Tainted;
      if (Credentials)
        Options->Credentials = Credentials == 1
                                   ? DarwinCredentials{0, 0, 0, 0, {}}
                                   : DarwinCredentials{0, 7, 9, 11, {}};
      auto Out = invoke(ServiceKind::IsSetUGID, {UINT64_MAX, 1, UINT64_MAX});
      ASSERT_TRUE(Out);
      EXPECT_EQ(Out->Value, Tainted ? 1u : 0u);
      EXPECT_FALSE(Out->Error);
      EXPECT_EQ(credentialID(ServiceKind::GetEUID, Options),
                Credentials == 0   ? 1000u
                : Credentials == 1 ? 0u
                                   : 7u);
      Options->HostName = "x";
      capacity(1);
      Out = named("kern.hostname", Output, Length, Base, 1);
      if (Credentials == 1) {
        EXPECT_FALSE(Out);
        EXPECT_EQ(Result.Diagnostic,
                  "Darwin privileged system write is not modeled");
      } else {
        ASSERT_TRUE(Out);
        EXPECT_EQ(Out->Value, 1u);
        EXPECT_TRUE(Out->Error);
      }
      EXPECT_FALSE(Options->ProcessGroupID);
      EXPECT_FALSE(Options->SessionID);
    }
}

TEST(DarwinSystemOptions,
     ProcessNiceUsesSignedBoundsWithoutConstrainingRevision) {
  DarwinSystemOptions O;
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  for (int32_t Nice = -20; Nice <= 20; ++Nice) {
    O.ProcessNice = Nice;
    O.OSRevision = Nice < 0 ? INT32_MIN : INT32_MAX;
    EXPECT_FALSE(bool(validateSystemOptions(O)));
  }
  for (int32_t Bad : {-21, 21, INT32_MIN, INT32_MAX}) {
    O.ProcessNice = Bad;
    auto E = validateSystemOptions(O);
    ASSERT_TRUE(bool(E));
    EXPECT_EQ(llvm::toString(std::move(E)),
              "Darwin system nice must be between -20 and 20");
  }
}

TEST_P(DarwinSystemTest, ProcessNicePreservesEverySignedValueAndFullCarriers) {
  struct Boundary {
    int32_t Nice;
    uint64_t Raw;
  };
  const Boundary Boundaries[] = {{-20, 0xffffffffffffffecULL},
                                 {-1, 0xffffffffffffffffULL},
                                 {0, 0},
                                 {20, 20}};
  const auto Query = [&](uint64_t Which, uint64_t Who, uint64_t Expected) {
    FailingSystemMemory Memory(*Space);
    Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
    auto Out = systemService(Memory, Page, ServiceKind::GetPriority,
                             {0,
                              100,
                              {Which, Who, 0xffffffffffffffffULL, Output,
                               Length, 0xffffffffffffffffULL},
                              {}},
                             Options, Result);
    ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
    ASSERT_TRUE(*Out) << Result.Diagnostic;
    EXPECT_EQ((**Out).Value, Expected);
    EXPECT_FALSE((**Out).Error);
    EXPECT_EQ(Memory.Accesses, 0u);
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, 0u);
  };
  for (int32_t Nice = -20; Nice <= 20; ++Nice) {
    SCOPED_TRACE(Nice);
    Options.emplace().ProcessNice = Nice;
    for (uint64_t Which : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL})
      for (uint64_t Who : {0ULL, 1000ULL, 0x100000000ULL, 0x12345678000003e8ULL,
                           0xffffffff00000000ULL, 0xffffffff000003e8ULL})
        Query(Which, Who,
              Nice < 0 ? 0xffffffffffffffffULL - uint64_t(-Nice - 1)
                       : uint64_t(Nice));
  }
  for (const auto &B : Boundaries) {
    Options->ProcessNice = B.Nice;
    Query(0, 0, B.Raw);
  }
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
}

TEST_P(DarwinSystemTest, PriorityKnownArgumentErrorsPrecedeAllObservations) {
  for (unsigned Config = 0; Config != 3; ++Config) {
    Options.reset();
    if (Config == 1)
      Options.emplace();
    if (Config == 2) {
      Options = darwin_test::systemOptions();
      Options->Credentials.emplace().GroupAccessList =
          std::vector<uint32_t>{0, 1000, 1000};
      Options->ProcessGroupID = 1000;
      Options->SessionID = 1000;
      Options->ProcessTainted = false;
      Options->LoginNameBytes.emplace(255, 0);
      Options->ResourceLimits =
          darwin_test::resourceLimitOptions().ResourceLimits;
      Options->ResourceUsageSelf.emplace();
      Options->ProcessNice = -1;
    }
    const auto Invalid = [&](uint64_t Which, uint64_t Who) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
      auto Out = systemService(
          Memory, Page, ServiceKind::GetPriority,
          {0, 100, {Which, Who, 0xffffffffffffffffULL, Output, Length, 1}, {}},
          Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      ASSERT_TRUE(*Out) << Result.Diagnostic;
      EXPECT_EQ((**Out).Value, 22u);
      EXPECT_TRUE((**Out).Error);
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
    };
    for (uint64_t Which :
         {0ULL, 1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 7ULL, 8ULL, 9ULL, 0x1000ULL,
          0x1234567800000000ULL, 0xffffffffffffffffULL})
      for (uint64_t Who : {0x80000000ULL, 0xffffffffULL, 0x1234567880000000ULL,
                           0xffffffffffffffffULL})
        Invalid(Which, Who);
    for (uint64_t Which : {5ULL, 9ULL, 0x1000ULL, 0xffffffffULL, 0x80000000ULL,
                           0x1234567800000005ULL})
      for (uint64_t Who :
           {0ULL, 1ULL, 1000ULL, 0x7fffffffULL, 0xffffffff00000000ULL})
        Invalid(Which, Who);
    for (uint64_t Who : {1ULL, 1000ULL, 0x7fffffffULL, 0x1234567800000001ULL,
                         0xffffffff000003e8ULL})
      Invalid(0x1234567800000003ULL, Who);
  }
}

TEST_P(DarwinSystemTest, PrioritySelfPeerAndSelectedUnknownStayIndependent) {
  for (unsigned Config = 0; Config != 4; ++Config) {
    Options.reset();
    if (Config == 1)
      Options.emplace();
    if (Config >= 2) {
      Options = darwin_test::systemOptions();
      Options->Credentials.emplace().GroupAccessList =
          std::vector<uint32_t>{0, 1000, 1000};
      Options->ProcessGroupID = 1000;
      Options->SessionID = 1000;
      Options->ProcessTainted = true;
      Options->HostName = "abcd";
      Options->LoginNameBytes.emplace(255, 0);
      Options->ResourceLimits =
          darwin_test::resourceLimitOptions().ResourceLimits;
      Options->ResourceUsageSelf.emplace();
      if (Config == 3)
        Options->ProcessNice = 0;
    }
    const auto Query = [&](uint64_t Which, uint64_t Who, const char *Reason) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
      Result = {ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                ExecutionBackendKind::Unicorn, "priority scope oracle"};
      auto Out = systemService(Memory, Page, ServiceKind::GetPriority,
                               {0,
                                100,
                                {Which, Who, 0xffffffffffffffffULL, Output,
                                 Length, 0xffffffffffffffffULL},
                                {}},
                               Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      if (Reason) {
        EXPECT_FALSE(*Out);
        EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
        EXPECT_EQ(Result.Diagnostic, Reason);
      } else {
        ASSERT_TRUE(*Out);
        EXPECT_EQ((**Out).Value, 0u);
        EXPECT_FALSE((**Out).Error);
      }
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
    };
    for (uint64_t Who :
         {0ULL, 1000ULL, 0x12345678000003e8ULL, 0xffffffff00000000ULL})
      Query(0, Who,
            Config == 3 ? nullptr
                        : "Darwin process nice observation is not configured");
    for (uint64_t Who : {1ULL, 999ULL, 1001ULL, 0x1000ULL, 0x7fffffffULL,
                         0x1234567800000001ULL})
      Query(0, Who, "Darwin other-process queries are not modeled");
    for (uint64_t Which :
         {1ULL, 2ULL, 4ULL, 6ULL, 7ULL, 8ULL, 0x1234567800000001ULL})
      for (uint64_t Who : {0ULL, 1000ULL, 0x7fffffffULL, 0xffffffff00000000ULL})
        Query(Which, Who,
              "Darwin selected priority observation is not modeled");
    for (uint64_t Who : {0ULL, 0x100000000ULL, 0xffffffff00000000ULL})
      Query(0x1234567800000003ULL, Who,
            "Darwin selected priority observation is not modeled");
  }
}

TEST_P(DarwinSystemTest, ProcessNiceAddsNoSysctlBindingOrWriteAuthority) {
  Options = darwin_test::priorityOptions();
  Options->Credentials.emplace();
  capacity(8);
  auto Out = named("kern.nice");
  EXPECT_FALSE(Out);
  EXPECT_EQ(Result.Diagnostic, "unsupported Darwin sysctl key");
  EXPECT_EQ(length(), 8u);
  EXPECT_EQ(bytes(Output, 8), std::string(8, '\xa5'));
}

TEST_P(DarwinSystemTest,
       LoginBufferCopiesExactPrefixesWithUnsignedLow32Length) {
  Options = darwin_test::loginBufferOptions();
  const std::string Expected =
      std::string("L\0\xff", 3) + std::string(251, '\xa5') + "~";
  ASSERT_EQ(Expected.size(), 255u);
  const uint64_t Sizes[] = {0,
                            1,
                            2,
                            254,
                            255,
                            256,
                            UINT32_MAX,
                            0x80000000,
                            0x100000000ULL,
                            0xffffffff00000000ULL,
                            0x100000001ULL,
                            0xffffffff00000001ULL,
                            0x12345678000000ffULL,
                            UINT64_MAX};
  for (uint64_t Size : Sizes) {
    SCOPED_TRACE(Size);
    fill();
    const size_t Count = std::min(uint32_t(Size), uint32_t(255));
    FailingSystemMemory Memory(*Space);
    auto Out = systemService(
        Memory, Page, ServiceKind::GetLogin,
        {0,
         49,
         {Output + 3, Size, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX},
         {}},
        Options, Result);
    ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
    result(*Out);
    EXPECT_EQ(bytes(Output + 3, Count), Expected.substr(0, Count));
    EXPECT_EQ(bytes(Output, 3), std::string(3, '\xa5'));
    EXPECT_EQ(bytes(Output + 3 + Count, 8), std::string(8, '\xa5'));
    EXPECT_EQ(Memory.Accesses, Count ? 1u : 0u);
    EXPECT_EQ(Memory.Writes, Count ? 1u : 0u);
    EXPECT_EQ(Memory.Reads, 0u);
  }
  Options->LoginNameBytes->assign(255, 0);
  fill();
  result(invoke(ServiceKind::GetLogin, {Output, 255}));
  EXPECT_EQ(bytes(Output, 255), std::string(255, '\0'));
  EXPECT_EQ(bytes(Output + 255, 8), std::string(8, '\xa5'));
}

TEST_P(DarwinSystemTest, LoginBufferZeroLengthNeedsNoObservationOrMemory) {
  for (unsigned Config = 0; Config != 4; ++Config) {
    Options.reset();
    if (Config == 1)
      Options.emplace();
    if (Config == 2) {
      Options = darwin_test::processObservationOptions();
      Options->Credentials.emplace();
      Options->HostName = "abcd";
    }
    if (Config == 3)
      Options = darwin_test::loginBufferOptions();
    for (uint64_t Pointer : std::array<uint64_t, 8>{
             0, 1, Output, 0x0000800000000000ULL, 0x8000000000000000ULL,
             0xffff000000000000ULL, UINT64_MAX, UINT64_MAX - 100}) {
      for (uint64_t Size :
           std::array<uint64_t, 3>{0, 0x100000000ULL, 0xffffffff00000000ULL}) {
        FailingSystemMemory Memory(*Space);
        Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
        auto Out = systemService(Memory, Page, ServiceKind::GetLogin,
                                 {0, 49, {Pointer, Size, UINT64_MAX}, {}},
                                 Options, Result);
        ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
        result(*Out);
        EXPECT_EQ(Memory.Accesses, 0u);
        EXPECT_EQ(Memory.Reads, 0u);
        EXPECT_EQ(Memory.Writes, 0u);
      }
    }
  }
}

TEST_P(DarwinSystemTest, LoginBufferAbsencePrecedesOutputChecks) {
  for (unsigned Config = 0; Config != 3; ++Config) {
    Options.reset();
    if (Config == 1)
      Options.emplace();
    if (Config == 2) {
      Options = darwin_test::processObservationOptions();
      Options->Credentials = darwin_test::credentialOptions().Credentials;
      Options->HostName = "abcd";
    }
    for (uint64_t Pointer : std::array<uint64_t, 3>{0, Output, UINT64_MAX}) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
      auto Out =
          systemService(Memory, Page, ServiceKind::GetLogin,
                        {0, 49, {Pointer, UINT64_MAX}, {}}, Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      EXPECT_FALSE(*Out);
      EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(Result.Diagnostic, diagnostic::LoginNameObservation);
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
      EXPECT_EQ(bytes(Output, 256), std::string(256, '\xa5'));
    }
  }
}

TEST_P(DarwinSystemTest, LoginBufferClampPrecedesPageEndAccessAdmission) {
  Options = darwin_test::loginBufferOptions();
  const uint64_t End = Base + Page * 2;
  ASSERT_FALSE(bool(Space->map(End, Page, Read | Write | UserAccessible)));
  ASSERT_FALSE(bool(Space->write(End, std::vector<uint8_t>(Page, 0xa5))));
  ASSERT_FALSE(bool(Space->protect(End, Page, Read | UserAccessible)));
  FailingSystemMemory Memory(*Space);
  Memory.FailAccess = 2;
  auto Out =
      systemService(Memory, Page, ServiceKind::GetLogin,
                    {0, 49, {End - 255, UINT64_MAX}, {}}, Options, Result);
  ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
  result(*Out);
  EXPECT_EQ(bytes(End - 255, 255),
            std::string("L\0\xff", 3) + std::string(251, '\xa5') + "~");
  EXPECT_EQ(bytes(End - 256, 1), std::string(1, '\xa5'));
  EXPECT_EQ(bytes(End, 8), std::string(8, '\xa5'));
  EXPECT_EQ(Memory.Accesses, 1u);
  EXPECT_EQ(Memory.Writes, 1u);
  EXPECT_EQ(Memory.Reads, 0u);
}

TEST_P(DarwinSystemTest, LoginBufferFaultAndPartialRangePublishNoBytes) {
  Options = darwin_test::loginBufferOptions();
  for (uint64_t Pointer : std::array<uint64_t, 5>{
           0, 1, Base + Page * 2, value::UserLimit, UINT64_MAX}) {
    FailingSystemMemory Memory(*Space);
    auto Out = systemService(Memory, Page, ServiceKind::GetLogin,
                             {0, 49, {Pointer, 255}, {}}, Options, Result);
    ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
    result(*Out, 14);
    EXPECT_EQ(Memory.Writes, 0u);
    EXPECT_EQ(Memory.Reads, 0u);
  }
  FailingSystemMemory Memory(*Space);
  auto Out =
      systemService(Memory, Page, ServiceKind::GetLogin,
                    {0, 49, {Base + Page * 2 - 127, 255}, {}}, Options, Result);
  ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
  EXPECT_FALSE(*Out);
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Result.Diagnostic, diagnostic::LoginNamePartialOutput);
  EXPECT_EQ(Memory.Accesses, 2u);
  EXPECT_EQ(Memory.Writes, 0u);
  EXPECT_EQ(bytes(Base + Page * 2 - 256, 256), std::string(256, '\xa5'));
}

TEST_P(DarwinSystemTest,
       LoginBufferCrossPageCopyAndTransportErrorsKeepOwnerRules) {
  Options = darwin_test::loginBufferOptions();
  const uint64_t Destination = Base + Page - 127;
  for (unsigned Phase = 0; Phase != 3; ++Phase) {
    fill();
    FailingSystemMemory Memory(*Space);
    if (Phase == 1)
      Memory.FailAccess = 2;
    if (Phase == 2)
      Memory.FailWrite = 1;
    auto Out = systemService(Memory, Page, ServiceKind::GetLogin,
                             {0, 49, {Destination, 255}, {}}, Options, Result);
    if (Phase) {
      ASSERT_FALSE(bool(Out));
      EXPECT_EQ(llvm::toString(Out.takeError()),
                Phase == 1 ? "transport access" : "transport write");
      // This injected write fails before delegation. Arbitrary backend errors
      // do not have a rollback guarantee.
      EXPECT_EQ(bytes(Destination - 1, 257), std::string(257, '\xa5'));
    } else {
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      result(*Out);
      EXPECT_EQ(bytes(Destination, 255),
                std::string("L\0\xff", 3) + std::string(251, '\xa5') + "~");
      EXPECT_EQ(bytes(Destination - 1, 1), std::string(1, '\xa5'));
      EXPECT_EQ(bytes(Destination + 255, 1), std::string(1, '\xa5'));
    }
    EXPECT_EQ(Memory.Accesses, 2u);
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, Phase == 1 ? 0u : 1u);
  }
  fill();
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  FailingSystemMemory Memory(*Space);
  auto Out = systemService(Memory, Page, ServiceKind::GetLogin,
                           {0, 49, {Destination, 255}, {}}, Options, Result);
  ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
  EXPECT_FALSE(*Out);
  EXPECT_EQ(Result.Diagnostic, diagnostic::LoginNamePartialOutput);
  EXPECT_EQ(Memory.Writes, 0u);
  EXPECT_EQ(bytes(Destination - 1, 257), std::string(257, '\xa5'));
}

TEST(DarwinSystemOptions, LoginBufferRequiresEveryByteWithoutStringRules) {
  DarwinSystemOptions O;
  EXPECT_FALSE(O.LoginNameBytes);
  for (size_t Size : {0u, 1u, 254u, 256u, 510u}) {
    O.LoginNameBytes.emplace(Size, 0);
    EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
              diagnostic::LoginNameOption);
  }
  for (unsigned char Byte : {0u, 0xffu, 0xa5u}) {
    O.LoginNameBytes.emplace(255, Byte);
    EXPECT_FALSE(bool(validateSystemOptions(O)));
    EXPECT_FALSE(O.Credentials);
    EXPECT_FALSE(O.ProcessGroupID);
    EXPECT_FALSE(O.SessionID);
    EXPECT_FALSE(O.ProcessTainted);
    EXPECT_FALSE(O.HostName);
  }
  O = darwin_test::loginBufferOptions();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
}

TEST(DarwinSystemOptions, ProcessIDsUseFullBoundsAndTaintHasNoDefaults) {
  DarwinSystemOptions O;
  EXPECT_FALSE(O.ProcessGroupID);
  EXPECT_FALSE(O.SessionID);
  EXPECT_FALSE(O.ProcessTainted);
  for (auto Member : {&DarwinSystemOptions::ProcessGroupID,
                      &DarwinSystemOptions::SessionID}) {
    for (uint32_t ID : {1u, uint32_t(INT32_MAX)}) {
      O = {};
      O.*Member = ID;
      EXPECT_FALSE(bool(validateSystemOptions(O)));
    }
    for (uint32_t ID : {0u, uint32_t(INT32_MAX) + 1, UINT32_MAX}) {
      O = {};
      O.*Member = ID;
      EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
                "Darwin system process_group_id and session_id must be between "
                "1 and INT32_MAX");
    }
  }
  for (bool Tainted : {false, true}) {
    O = {};
    O.ProcessTainted = Tainted;
    EXPECT_FALSE(bool(validateSystemOptions(O)));
    EXPECT_TRUE(O.ProcessTainted.has_value());
    EXPECT_FALSE(O.Credentials);
  }
}

TEST_P(DarwinSystemTest, DescriptorTableNeedsBothObservationsWithoutMemory) {
  for (unsigned Configuration = 0; Configuration != 7; ++Configuration) {
    SCOPED_TRACE(Configuration);
    Options.reset();
    if (Configuration) {
      Options.emplace();
      if (Configuration == 2 || Configuration == 6)
        Options->ResourceLimits[8] = {Configuration == 6 ? 0u : 256u,
                                      uint64_t(INT64_MAX)};
      if (Configuration >= 3 && Configuration <= 5)
        Options->MaxFilesPerProcess = Configuration == 4 ? 0u : 64u;
      if (Configuration == 5)
        Options->ResourceLimits[7] = {0, uint64_t(INT64_MAX)};
    }
    FailingSystemMemory Memory(*Space);
    Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
    auto Out =
        systemService(Memory, Page, ServiceKind::GetDTableSize,
                      {0,
                       89,
                       {UINT64_MAX, 1, value::UserLimit, 0, Output, Length},
                       std::nullopt},
                      Options, Result);
    ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
    EXPECT_FALSE(*Out);
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, diagnostic::DescriptorTableObservation);
    EXPECT_EQ(Memory.Accesses, 0u);
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, 0u);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
}

TEST_P(DarwinSystemTest, DescriptorTableClipsFullCurrentAndIgnoresArguments) {
  struct Sample {
    uint64_t Current, Maximum;
    uint32_t Cap, Expected;
  };
  const Sample Samples[] = {
      {0, 0, 64, 0},
      {1, 1024, 64, 1},
      {256, 1024, 64, 64},
      {0x100000001ULL, uint64_t(INT64_MAX), 64, 64},
      {uint64_t(INT64_MAX), uint64_t(INT64_MAX), INT32_MAX, INT32_MAX},
      {256, 1024, 0, 0},
      {0, uint64_t(INT64_MAX), 0, 0}};
  for (const auto &S : Samples) {
    SCOPED_TRACE(S.Current);
    Options = DarwinSystemOptions{};
    Options->ResourceLimits[8] = {S.Current, S.Maximum};
    Options->ResourceLimits[7] = {0, 0};
    Options->MaxFilesPerProcess = S.Cap;
    ASSERT_FALSE(bool(validateSystemOptions(*Options)));
    FailingSystemMemory Memory(*Space);
    Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
    auto Out = systemService(
        Memory, Page, ServiceKind::GetDTableSize,
        {0,
         0x1234567800000059ULL,
         {UINT64_MAX, Output, value::UserLimit, 1, Length, UINT64_MAX},
         std::nullopt},
        Options, Result);
    ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
    ASSERT_TRUE(*Out) << Result.Diagnostic;
    EXPECT_EQ((**Out).Value, S.Expected);
    EXPECT_FALSE((**Out).Error);
    EXPECT_EQ(Memory.Accesses, 0u);
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, 0u);
    EXPECT_EQ(Options->ResourceLimits.at(8).Current, S.Current);
    EXPECT_EQ(Options->ResourceLimits.at(8).Maximum, S.Maximum);
    EXPECT_EQ(Options->MaxFilesPerProcess, S.Cap);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
}

TEST_P(DarwinSystemTest, DescriptorCapHasIndependentFourByteSysctlReads) {
  Options = DarwinSystemOptions{};
  struct Sample {
    uint32_t Cap;
    const char *Hex;
  };
  for (const Sample &S :
       {Sample{0, "00000000"}, Sample{1, "01000000"},
        Sample{0x01020304, "04030201"}, Sample{INT32_MAX, "ffffff7f"}}) {
    Options->MaxFilesPerProcess = S.Cap;
    for (bool Named : {false, true}) {
      fill();
      capacity(8);
      result(Named ? named("kern.maxfilesperproc", Output + 1)
                   : mib(1, 29, Output + 1));
      EXPECT_EQ(bytes(Output + 1, 4), llvm::fromHex(S.Hex));
      EXPECT_EQ(bytes(Output - 8, 9), std::string(9, '\xa5'));
      EXPECT_EQ(bytes(Output + 5, 16), std::string(16, '\xa5'));
      EXPECT_EQ(length(), 4u);
    }
  }
  EXPECT_TRUE(Options->ResourceLimits.empty());
  fill();
  capacity(3);
  result(named("kern.maxfilesperproc"), value::NoMemory);
  EXPECT_EQ(length(), 0u);
  EXPECT_EQ(bytes(Output, 32), std::string(32, '\xa5'));
  capacity(4);
  result(named("kern.maxfilesperproc", 0));
  EXPECT_EQ(length(), 4u);
  EXPECT_EQ(bytes(Output, 32), std::string(32, '\xa5'));
}

TEST(DarwinSystemOptions, DescriptorCapAdmitsOnlyNonnegativeIntObservations) {
  DarwinSystemOptions O;
  ASSERT_FALSE(bool(validateSystemOptions(O)));
  for (auto Cap : {0u, 1u, uint32_t(INT32_MAX)}) {
    O.MaxFilesPerProcess = Cap;
    EXPECT_FALSE(bool(validateSystemOptions(O)));
  }
  for (auto Cap : {0x80000000u, UINT32_MAX}) {
    O.MaxFilesPerProcess = Cap;
    EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
              diagnostic::SystemMaxFilesPerProcess);
  }
}

TEST_P(DarwinSystemTest, ResourceLimitsKeepBothWordsFlagsAndUnalignedGuards) {
  Options = darwin_test::resourceLimitOptions();
  const auto Expected = llvm::fromHex(darwin_test::ResourceLimitsHex);
  ASSERT_EQ(Expected.size(), 144u);
  for (uint32_t Resource = 0; Resource != 9; ++Resource) {
    for (uint32_t Flag : {0u, 0x1000u}) {
      SCOPED_TRACE(Resource | Flag);
      fill();
      result(invoke(ServiceKind::GetRlimit, {Resource | Flag, Output + 1}));
      EXPECT_EQ(bytes(Output + 1, 16), Expected.substr(Resource * 16, 16));
      EXPECT_EQ(bytes(Output, 1), std::string(1, '\xa5'));
      EXPECT_EQ(bytes(Output + 17, 16), std::string(16, '\xa5'));
    }
  }
  EXPECT_EQ(Options->ResourceLimits.size(), 9u);
  EXPECT_EQ(Options->ResourceLimits.at(0).Current, 0u);
  EXPECT_EQ(Options->ResourceLimits.at(0).Maximum, 0u);
  EXPECT_EQ(Options->ResourceLimits.at(1).Current, uint64_t(INT64_MAX));
  Options->MaxFilesPerProcess.reset();
  fill();
  result(invoke(ServiceKind::GetRlimit, {8, Output}));
  EXPECT_EQ(bytes(Output, 16), Expected.substr(128, 16));
}

TEST_P(DarwinSystemTest, ResourceSelectorsAndMissingValuesPrecedeAllMemory) {
  for (unsigned Configuration = 0; Configuration != 3; ++Configuration) {
    if (Configuration == 0)
      Options.reset();
    else if (Configuration == 1)
      Options = DarwinSystemOptions{};
    else
      Options = darwin_test::resourceLimitOptions();
    for (uint64_t Selector : {uint64_t(9), uint64_t(0x1009), uint64_t(0x2000),
                              uint64_t(UINT32_MAX)}) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailWrite = 1;
      auto Out = systemService(Memory, Page, ServiceKind::GetRlimit,
                               {0, 194, {Selector, Output}, std::nullopt},
                               Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      result(*Out, value::InvalidArgument);
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
    }
    if (Configuration == 2)
      Options->ResourceLimits.erase(8);
    for (uint64_t Selector : {uint64_t(8), uint64_t(0x1008)}) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailWrite = 1;
      auto Out = systemService(Memory, Page, ServiceKind::GetRlimit,
                               {0, 194, {Selector, Output}, std::nullopt},
                               Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      EXPECT_FALSE(*Out);
      EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(Result.Diagnostic, diagnostic::ResourceLimitObservation);
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
    }
  }
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
}

TEST_P(DarwinSystemTest, ResourceSelectorUsesLow32BitsAndOnlyThePosixFlag) {
  Options = darwin_test::resourceLimitOptions();
  const uint64_t Selectors[] = {8, 0x1008, 0x100000008ULL,
                                0xffffffff00001008ULL};
  const auto Expected =
      llvm::fromHex(darwin_test::ResourceLimitsHex).substr(128);
  for (auto Selector : Selectors) {
    fill();
    result(invoke(ServiceKind::GetRlimit, {Selector, Output}));
    EXPECT_EQ(bytes(Output, 16), Expected);
    EXPECT_EQ(bytes(Output - 8, 8), std::string(8, '\xa5'));
    EXPECT_EQ(bytes(Output + 16, 8), std::string(8, '\xa5'));
  }
  fill();
  result(invoke(ServiceKind::GetRlimit, {0x2008, Output}),
         value::InvalidArgument);
  EXPECT_EQ(bytes(Output, 16), std::string(16, '\xa5'));
}

TEST_P(DarwinSystemTest,
       ResourcePairsCrossMappedPagesAndRejectUnwritableOutput) {
  Options = darwin_test::resourceLimitOptions();
  const auto Expected =
      llvm::fromHex(darwin_test::ResourceLimitsHex).substr(128);
  result(invoke(ServiceKind::GetRlimit, {8, Base + Page - 7}));
  EXPECT_EQ(bytes(Base + Page - 7, 16), Expected);
  EXPECT_EQ(bytes(Base + Page - 15, 8), std::string(8, '\xa5'));
  EXPECT_EQ(bytes(Base + Page + 9, 8), std::string(8, '\xa5'));
  fill();
  const uint64_t Faults[] = {0, 1, value::UserLimit, UINT64_MAX,
                             Base + Page * 2};
  for (auto Fault : Faults) {
    result(invoke(ServiceKind::GetRlimit, {8, Fault}), value::BadAddress);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  result(invoke(ServiceKind::GetRlimit, {8, Base + Page}), value::BadAddress);
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
}

TEST_P(DarwinSystemTest, PartialResourcePairNeverPublishesEitherWord) {
  Options = darwin_test::resourceLimitOptions();
  for (bool ReadOnly : {false, true}) {
    fill();
    if (ReadOnly)
      ASSERT_FALSE(
          bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
    const auto Output = ReadOnly ? Base + Page - 8 : Base + Page * 2 - 8;
    auto Out = invoke(ServiceKind::GetRlimit, {8, Output});
    EXPECT_FALSE(Out);
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, diagnostic::ResourceLimitPartialOutput);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
}

TEST_P(DarwinSystemTest, ResourcePairTransportFailureDoesNotInventGuestErrno) {
  Options = darwin_test::resourceLimitOptions();
  for (unsigned Failure = 0; Failure != 3; ++Failure) {
    fill();
    FailingSystemMemory Memory(*Space);
    if (Failure < 2)
      Memory.FailAccess = Failure + 1;
    else
      Memory.FailWrite = 1;
    auto Out = systemService(Memory, Page, ServiceKind::GetRlimit,
                             {0, 194, {8, Base + Page - 8}, std::nullopt},
                             Options, Result);
    ASSERT_FALSE(bool(Out));
    EXPECT_EQ(llvm::toString(Out.takeError()),
              Failure < 2 ? "transport access" : "transport write");
    EXPECT_EQ(Memory.Accesses, Failure < 2 ? Failure + 1 : 2u);
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, Failure < 2 ? 0u : 1u);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
  FailingSystemMemory Memory(*Space);
  auto Out = systemService(Memory, Page, ServiceKind::GetRlimit,
                           {0, 194, {8, Base + Page - 8}, std::nullopt},
                           Options, Result);
  ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
  result(*Out);
  EXPECT_EQ(Memory.Accesses, 2u);
  EXPECT_EQ(Memory.Reads, 0u);
  EXPECT_EQ(Memory.Writes, 1u);
  std::string Expected(Page * 2, '\xa5');
  Expected.replace(Page - 8, 16,
                   llvm::fromHex(darwin_test::ResourceLimitsHex).substr(128));
  EXPECT_EQ(bytes(Base, Page * 2), Expected);
}

TEST(DarwinSystemOptions, ResourcePairsRequireCanonicalKeysAndBoundedValues) {
  for (auto Resource : {9u, 4096u, UINT32_MAX}) {
    auto O = darwin_test::resourceLimitOptions();
    O.ResourceLimits[Resource] = {0, 0};
    EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
              diagnostic::ResourceLimitOption);
  }
  for (const DarwinResourceLimit Bad :
       {DarwinResourceLimit{1, 0},
        DarwinResourceLimit{0, uint64_t(INT64_MAX) + 1},
        DarwinResourceLimit{UINT64_MAX, UINT64_MAX}}) {
    auto O = darwin_test::resourceLimitOptions();
    O.ResourceLimits[8] = Bad;
    EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
              diagnostic::ResourceLimitOption);
  }
  auto O = darwin_test::resourceLimitOptions();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.ResourceLimits.clear();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
}

TEST_P(DarwinSystemTest, UsagePreservesSignedWordsPaddingAndUnalignedGuards) {
  Options = darwin_test::resourceUsageOptions();
  const auto Encoded = llvm::fromHex(darwin_test::ResourceUsageHex);
  ASSERT_EQ(Encoded.size(), 288u);
  const uint64_t Selectors[] = {0, UINT32_MAX, 0x100000000ULL,
                                0x12345678ffffffffULL};
  for (auto Selector : Selectors) {
    SCOPED_TRACE(Selector);
    fill();
    result(invoke(ServiceKind::GetRusage, {Selector, Output + 1}));
    std::string Expected(Page * 2, '\xa5');
    Expected.replace(Output + 1 - Base, 144,
                     Encoded.substr(uint32_t(Selector) == 0 ? 0 : 144, 144));
    EXPECT_EQ(bytes(Base, Page * 2), Expected);
    EXPECT_EQ(bytes(Output + 13, 4), std::string(4, '\0'));
    EXPECT_EQ(bytes(Output + 29, 4), std::string(4, '\0'));
  }
}

TEST_P(DarwinSystemTest, UsageSnapshotsAreIndependentAndNeverDefaultMissing) {
  for (bool AbsentSystem : {true, false}) {
    if (AbsentSystem)
      Options.reset();
    else
      Options = DarwinSystemOptions{};
    for (auto Selector : {uint64_t(0), uint64_t(UINT32_MAX)}) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailWrite = 1;
      auto Out = systemService(Memory, Page, ServiceKind::GetRusage,
                               {0, 117, {Selector, Output}, std::nullopt},
                               Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      EXPECT_FALSE(*Out);
      EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(Result.Diagnostic, diagnostic::ResourceUsageObservation);
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
    }
  }
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  for (bool Self : {true, false}) {
    Options = darwin_test::resourceUsageOptions();
    if (Self)
      Options->ResourceUsageChildren.reset();
    else
      Options->ResourceUsageSelf.reset();
    fill();
    result(invoke(ServiceKind::GetRusage,
                  {Self ? 0u : uint64_t(UINT32_MAX), Output}));
    EXPECT_EQ(bytes(Output, 144), llvm::fromHex(darwin_test::ResourceUsageHex)
                                      .substr(Self ? 0 : 144, 144));
    fill();
    FailingSystemMemory Memory(*Space);
    Memory.FailAccess = Memory.FailWrite = 1;
    auto Out = systemService(
        Memory, Page, ServiceKind::GetRusage,
        {0, 117, {Self ? uint64_t(UINT32_MAX) : 0u, Output}, std::nullopt},
        Options, Result);
    ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
    EXPECT_FALSE(*Out);
    EXPECT_EQ(Result.Diagnostic, diagnostic::ResourceUsageObservation);
    EXPECT_EQ(Memory.Accesses, 0u);
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, 0u);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
    Options = DarwinSystemOptions{};
    (Self ? Options->ResourceUsageSelf : Options->ResourceUsageChildren) =
        DarwinResourceUsage{};
    result(invoke(ServiceKind::GetRusage,
                  {Self ? 0u : uint64_t(UINT32_MAX), Output}));
    std::string Expected(Page * 2, '\xa5');
    Expected.replace(Output - Base, 144, std::string(144, '\0'));
    EXPECT_EQ(bytes(Base, Page * 2), Expected);
  }
}

TEST_P(DarwinSystemTest, InvalidUsageSelectorsPrecedeEveryMemoryAccess) {
  const uint64_t Selectors[] = {1, 0x1000, uint64_t(UINT32_MAX) - 1,
                                0xffffffff00000001ULL};
  for (unsigned Configuration = 0; Configuration != 3; ++Configuration) {
    if (Configuration == 0)
      Options.reset();
    else if (Configuration == 1)
      Options = DarwinSystemOptions{};
    else
      Options = darwin_test::resourceUsageOptions();
    for (auto Selector : Selectors) {
      for (auto OutputAddress : {uint64_t(0), Output}) {
        FailingSystemMemory Memory(*Space);
        Memory.FailAccess = Memory.FailWrite = 1;
        auto Out = systemService(
            Memory, Page, ServiceKind::GetRusage,
            {0, 117, {Selector, OutputAddress}, std::nullopt}, Options, Result);
        ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
        result(*Out, value::InvalidArgument);
        EXPECT_EQ(Memory.Accesses, 0u);
        EXPECT_EQ(Memory.Reads, 0u);
        EXPECT_EQ(Memory.Writes, 0u);
      }
    }
  }
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
}

TEST_P(DarwinSystemTest, UsageCrossesPagesAndRejectsWhollyUnwritableOutput) {
  Options = darwin_test::resourceUsageOptions();
  for (auto Selector : {uint64_t(0), uint64_t(UINT32_MAX)}) {
    fill();
    result(invoke(ServiceKind::GetRusage, {Selector, Base + Page - 71}));
    std::string Expected(Page * 2, '\xa5');
    Expected.replace(Page - 71, 144,
                     llvm::fromHex(darwin_test::ResourceUsageHex)
                         .substr(Selector == 0 ? 0 : 144, 144));
    EXPECT_EQ(bytes(Base, Page * 2), Expected);
    fill();
    const uint64_t Faults[] = {0, 1, value::UserLimit, UINT64_MAX,
                               Base + Page * 2};
    for (auto Fault : Faults) {
      result(invoke(ServiceKind::GetRusage, {Selector, Fault}),
             value::BadAddress);
      EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
    }
  }
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  for (auto Selector : {uint64_t(0), uint64_t(UINT32_MAX)}) {
    result(invoke(ServiceKind::GetRusage, {Selector, Base + Page}),
           value::BadAddress);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
}

TEST_P(DarwinSystemTest, PartialUsageNeverPublishesEvenANativeWritablePrefix) {
  Options = darwin_test::resourceUsageOptions();
  for (bool ReadOnly : {false, true}) {
    fill();
    if (ReadOnly)
      ASSERT_FALSE(
          bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
    const auto Address = ReadOnly ? Base + Page - 72 : Base + Page * 2 - 72;
    for (auto Selector : {uint64_t(0), uint64_t(UINT32_MAX)}) {
      auto Out = invoke(ServiceKind::GetRusage, {Selector, Address});
      EXPECT_FALSE(Out);
      EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(Result.Diagnostic, diagnostic::ResourceUsagePartialOutput);
      EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
    }
  }
}

TEST_P(DarwinSystemTest, UsageTransportErrorsRemainErrorsAndSuccessCopiesOnce) {
  Options = darwin_test::resourceUsageOptions();
  for (unsigned Failure = 0; Failure != 3; ++Failure) {
    fill();
    FailingSystemMemory Memory(*Space);
    if (Failure < 2)
      Memory.FailAccess = Failure + 1;
    else
      Memory.FailWrite = 1;
    auto Out = systemService(Memory, Page, ServiceKind::GetRusage,
                             {0, 117, {0, Base + Page - 71}, std::nullopt},
                             Options, Result);
    ASSERT_FALSE(bool(Out));
    EXPECT_EQ(llvm::toString(Out.takeError()),
              Failure < 2 ? "transport access" : "transport write");
    EXPECT_EQ(Memory.Accesses, Failure < 2 ? Failure + 1 : 2u);
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, Failure < 2 ? 0u : 1u);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
  FailingSystemMemory Memory(*Space);
  auto Out = systemService(Memory, Page, ServiceKind::GetRusage,
                           {0, 117, {0, Base + Page - 71}, std::nullopt},
                           Options, Result);
  ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
  result(*Out);
  EXPECT_EQ(Memory.Accesses, 2u);
  EXPECT_EQ(Memory.Reads, 0u);
  EXPECT_EQ(Memory.Writes, 1u);
  std::string Expected(Page * 2, '\xa5');
  Expected.replace(Page - 71, 144,
                   llvm::fromHex(darwin_test::ResourceUsageHex).substr(0, 144));
  EXPECT_EQ(bytes(Base, Page * 2), Expected);
}

TEST(DarwinSystemOptions,
     UsageMicrosecondsAreBoundedAndSignedWordsStayLossless) {
  auto O = darwin_test::resourceUsageOptions();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  EXPECT_EQ(O.ResourceUsageSelf->UserSeconds, INT64_MIN);
  EXPECT_EQ(O.ResourceUsageSelf->Counters[0], INT64_MAX);
  EXPECT_EQ(O.ResourceUsageSelf->Counters[1], INT64_MIN);
  for (bool Self : {true, false}) {
    for (bool User : {true, false}) {
      for (auto Bad : {1000000u, UINT32_MAX}) {
        O = darwin_test::resourceUsageOptions();
        auto &Usage = Self ? *O.ResourceUsageSelf : *O.ResourceUsageChildren;
        (User ? Usage.UserMicroseconds : Usage.SystemMicroseconds) = Bad;
        EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
                  diagnostic::ResourceUsageOption);
      }
    }
  }
  O = DarwinSystemOptions{};
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.ResourceUsageSelf = DarwinResourceUsage{};
  O.ResourceUsageChildren = DarwinResourceUsage{};
  EXPECT_FALSE(bool(validateSystemOptions(O)));
}

TEST_P(DarwinSystemTest, CredentialScalarsShareSelectionWithoutMemoryAccess) {
  const ServiceKind Kinds[] = {ServiceKind::GetUID, ServiceKind::GetEUID,
                               ServiceKind::GetGID, ServiceKind::GetEGID};
  for (unsigned Configuration = 0; Configuration != 4; ++Configuration) {
    if (!Configuration)
      Options.reset();
    else
      Options = DarwinSystemOptions{};
    if (Configuration == 2)
      Options->Credentials = DarwinCredentials{0, 7, 9, 11, {}};
    if (Configuration == 3)
      Options->Credentials = DarwinCredentials{};
    const uint32_t Expected[] = {0, 7, 9, 11};
    for (unsigned I = 0; I != 4; ++I) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
      auto Out = systemService(Memory, Page, Kinds[I],
                               {0, 0, {UINT64_MAX, UINT64_MAX}, std::nullopt},
                               Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      ASSERT_TRUE(*Out);
      EXPECT_FALSE((**Out).Error);
      EXPECT_EQ((**Out).Value, Configuration < 2    ? 1000u
                               : Configuration == 2 ? Expected[I]
                                                    : 0u);
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
    }
  }
}
TEST_P(DarwinSystemTest, GroupsPreserveDuplicatesAndOnlyCopyActualCount) {
  Options = darwin_test::credentialOptions();
  const auto Data = llvm::fromHex(darwin_test::GroupsHex);
  ASSERT_EQ(Data.size(), 20u);
  for (auto Capacity : {5ULL, 16ULL, 0x1000ULL, 0x7fffffffULL,
                        0x1234567800000005ULL, 0x1234567800001000ULL})
    for (auto OutAddress : {Output + 1, Base + Page - 9}) {
      fill();
      auto Out = invoke(ServiceKind::GetGroups, {Capacity, OutAddress});
      ASSERT_TRUE(Out);
      EXPECT_FALSE(Out->Error);
      EXPECT_EQ(Out->Value, 5u);
      std::string Expected(Page * 2, '\xa5');
      Expected.replace(OutAddress - Base, Data.size(), Data);
      EXPECT_EQ(bytes(Base, Page * 2), Expected);
    }
  EXPECT_EQ(Options->Credentials->EffectiveGID, 404u);
  EXPECT_EQ(*Options->Credentials->GroupAccessList,
            (std::vector<uint32_t>{404, 0, INT32_MAX, 7, 7}));
}
TEST_P(DarwinSystemTest, GroupsMaximumAndExplicitRootAreNotDefaulted) {
  for (bool Maximum : {false, true}) {
    Options = DarwinSystemOptions{};
    Options->Credentials = DarwinCredentials{};
    Options->Credentials->EffectiveGID = Maximum ? INT32_MAX : 0;
    Options->Credentials->GroupAccessList =
        Maximum ? std::vector<uint32_t>{INT32_MAX, 0, 1, 2,  3,  4,  5,  6,
                                        7,         8, 9, 10, 11, 12, 13, 14}
                : std::vector<uint32_t>{0};
    ASSERT_FALSE(bool(validateSystemOptions(*Options)));
    const auto Data = llvm::fromHex(
        Maximum
            ? "ffffff7f00000000010000000200000003000000040000000500000006000000"
              "0700000008000000090000000a0000000b0000000c0000000d0000000e000000"
            : "00000000");
    fill();
    auto Out = invoke(ServiceKind::GetGroups, {0x1000, Output + 1});
    ASSERT_TRUE(Out);
    EXPECT_FALSE(Out->Error);
    EXPECT_EQ(Out->Value, Maximum ? 16u : 1u);
    std::string Expected(Page * 2, '\xa5');
    Expected.replace(Output + 1 - Base, Data.size(), Data);
    EXPECT_EQ(bytes(Base, Page * 2), Expected);
  }
}
TEST_P(DarwinSystemTest, GroupEarlyDecisionsNeverAccessOutput) {
  for (unsigned Configuration = 0; Configuration != 4; ++Configuration) {
    if (!Configuration)
      Options.reset();
    else
      Options = DarwinSystemOptions{};
    if (Configuration == 2)
      Options->Credentials = DarwinCredentials{};
    if (Configuration == 3)
      Options = darwin_test::credentialOptions();
    for (auto Capacity :
         {0ULL, 0xffffffff00000000ULL, 1ULL, 4ULL, 0x1234567800000004ULL,
          0xffffffffULL, 0x80000000ULL, 0xffffffff80000000ULL})
      for (auto Address : {uint64_t(0), Output, UINT64_MAX}) {
        FailingSystemMemory Memory(*Space);
        Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
        auto Out = systemService(Memory, Page, ServiceKind::GetGroups,
                                 {0, 79, {Capacity, Address}, std::nullopt},
                                 Options, Result);
        ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
        bool Invalid = uint32_t(Capacity) & 0x80000000u;
        bool Short = Configuration == 3 && uint32_t(Capacity) != 0 &&
                     uint32_t(Capacity) < 5;
        if (Invalid || Short) {
          ASSERT_TRUE(*Out);
          EXPECT_TRUE((**Out).Error);
          EXPECT_EQ((**Out).Value, 22u);
        } else if (Configuration != 3) {
          EXPECT_FALSE(*Out);
          EXPECT_EQ(Result.Diagnostic, diagnostic::GroupObservation);
        } else {
          ASSERT_TRUE(*Out);
          EXPECT_FALSE((**Out).Error);
          EXPECT_EQ((**Out).Value, 5u);
        }
        EXPECT_EQ(Memory.Accesses, 0u);
        EXPECT_EQ(Memory.Reads, 0u);
        EXPECT_EQ(Memory.Writes, 0u);
      }
  }
}
TEST_P(DarwinSystemTest, GroupFaultAndPartialCopiesNeverPublishBytes) {
  Options = darwin_test::credentialOptions();
  for (auto Address : {uint64_t(0), uint64_t(1), value::UserLimit, UINT64_MAX,
                       Base + Page * 2}) {
    fill();
    result(invoke(ServiceKind::GetGroups, {0x1000, Address}), 14);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
  fill();
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  result(invoke(ServiceKind::GetGroups, {0x1000, Base + Page}), 14);
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  EXPECT_FALSE(invoke(ServiceKind::GetGroups, {0x1000, Base + Page - 10}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::GroupPartialOutput);
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  ASSERT_FALSE(bool(Space->unmap(Base + Page, Page)));
  EXPECT_FALSE(invoke(ServiceKind::GetGroups, {16, Base + Page - 10}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::GroupPartialOutput);
  EXPECT_EQ(bytes(Base, Page), std::string(Page, '\xa5'));
}
TEST_P(DarwinSystemTest, GroupTransportErrorsAndSingleSuccessfulCopy) {
  Options = darwin_test::credentialOptions();
  for (unsigned Failure = 0; Failure != 3; ++Failure) {
    fill();
    FailingSystemMemory Memory(*Space);
    if (Failure < 2)
      Memory.FailAccess = Failure + 1;
    else
      Memory.FailWrite = 1;
    auto Out = systemService(Memory, Page, ServiceKind::GetGroups,
                             {0, 79, {5, Base + Page - 9}, std::nullopt},
                             Options, Result);
    ASSERT_FALSE(bool(Out));
    EXPECT_EQ(llvm::toString(Out.takeError()),
              Failure < 2 ? "transport access" : "transport write");
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, Failure < 2 ? 0u : 1u);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
  fill();
  FailingSystemMemory Memory(*Space);
  auto Out = systemService(Memory, Page, ServiceKind::GetGroups,
                           {0, 79, {0x1000, Base + Page - 9}, std::nullopt},
                           Options, Result);
  ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
  ASSERT_TRUE(*Out);
  EXPECT_FALSE((**Out).Error);
  EXPECT_EQ((**Out).Value, 5u);
  EXPECT_EQ(Memory.Accesses, 2u);
  EXPECT_EQ(Memory.Reads, 0u);
  EXPECT_EQ(Memory.Writes, 1u);
  std::string Expected(Page * 2, '\xa5');
  Expected.replace(Base + Page - 9 - Base, 20,
                   llvm::fromHex(darwin_test::GroupsHex));
  EXPECT_EQ(bytes(Base, Page * 2), Expected);
}
TEST_P(DarwinSystemTest,
       SysctlWriteDecisionUsesEffectiveIdentityAfterPreflight) {
  for (bool DescriptorCap : {false, true})
    for (unsigned Identity = 0; Identity != 3; ++Identity)
      for (bool Missing : {false, true})
        for (bool Named : {false, true}) {
          Options = darwin_test::systemOptions();
          Options->MaxFilesPerProcess = 64;
          if (Identity)
            Options->Credentials = DarwinCredentials{
                Identity == 1 ? 0u : 7u, Identity == 1 ? 7u : 0u, 0, 0, {}};
          if (Missing) {
            if (DescriptorCap)
              Options->MaxFilesPerProcess.reset();
            else
              Options->OSVersion.reset();
          }
          fill();
          capacity(0);
          std::optional<ServiceResult> Out;
          if (Named)
            Out =
                named(DescriptorCap ? "kern.maxfilesperproc" : "kern.osversion",
                      UINT64_MAX, Length, UINT64_MAX, 1);
          else {
            ASSERT_FALSE(bool(Space->writeInteger(Base, 1, 4)));
            ASSERT_FALSE(bool(
                Space->writeInteger(Base + 4, DescriptorCap ? 29 : 65, 4)));
            Out = invoke(ServiceKind::Sysctl,
                         {Base, 2, UINT64_MAX, Length, UINT64_MAX, 1});
          }
          if (Identity == 2) {
            EXPECT_FALSE(Out);
            EXPECT_EQ(Result.Diagnostic, diagnostic::SystemPrivilegedWrite);
          } else
            result(Out, 1);
          EXPECT_EQ(length(), 0u);
          EXPECT_EQ(bytes(Output, 32), std::string(32, '\xa5'));
          capacity(32);
          result(named("kern.ostype", UINT64_MAX, Length, UINT64_MAX, 1), 1);
        }
  Options = darwin_test::systemOptions();
  Options->Credentials = DarwinCredentials{};
  capacity(8);
  result(named("kern.osversion", Output, Length, UINT64_MAX, 0));
  EXPECT_EQ(bytes(Output, 4), std::string("V42\0", 4));
  Options->MaxFilesPerProcess = 64;
  capacity(8);
  result(named("kern.maxfilesperproc", Output, Length, UINT64_MAX, 0));
  EXPECT_EQ(bytes(Output, 4), llvm::fromHex("40000000"));
  EXPECT_EQ(length(), 4u);
}
TEST_P(DarwinSystemTest, RootSysctlCannotSkipNameOrLengthPreflight) {
  Options = darwin_test::systemOptions();
  Options->Credentials = DarwinCredentials{};
  result(invoke(ServiceKind::SysctlByName,
                {UINT64_MAX, 14, UINT64_MAX, Length, UINT64_MAX, 1}),
         14);
  result(invoke(ServiceKind::Sysctl,
                {UINT64_MAX, 2, UINT64_MAX, Length, UINT64_MAX, 1}),
         14);
  for (const auto &[Name, Leaf] : {std::pair{"kern.osversion", 65u},
                                   std::pair{"kern.maxfilesperproc", 29u}}) {
    EXPECT_FALSE(named(Name, UINT64_MAX, 1, UINT64_MAX, 1));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemLengthMemory);
    ASSERT_FALSE(bool(Space->writeInteger(Base, 1, 4)));
    ASSERT_FALSE(bool(Space->writeInteger(Base + 4, Leaf, 4)));
    EXPECT_FALSE(
        invoke(ServiceKind::Sysctl, {Base, 2, UINT64_MAX, 1, UINT64_MAX, 1}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemLengthMemory);
  }
  EXPECT_EQ(bytes(Output, 32), std::string(32, '\xa5'));
}
TEST(DarwinSystemOptions, CredentialsRequireBoundedCoherentGroups) {
  auto O = darwin_test::credentialOptions();
  ASSERT_FALSE(bool(validateSystemOptions(O)));
  for (auto Member :
       {&DarwinCredentials::RealUID, &DarwinCredentials::EffectiveUID,
        &DarwinCredentials::RealGID, &DarwinCredentials::EffectiveGID})
    for (auto ID : {0x80000000u, UINT32_MAX}) {
      auto Bad = O;
      (*Bad.Credentials).*Member = ID;
      EXPECT_EQ(llvm::toString(validateSystemOptions(Bad)),
                diagnostic::CredentialOption);
    }
  for (auto Groups : {std::vector<uint32_t>{}, std::vector<uint32_t>(17, 404),
                      std::vector<uint32_t>{0, 404},
                      std::vector<uint32_t>{404, UINT32_MAX}}) {
    auto Bad = O;
    Bad.Credentials->GroupAccessList = Groups;
    EXPECT_EQ(llvm::toString(validateSystemOptions(Bad)),
              diagnostic::CredentialOption);
  }
  O.Credentials =
      DarwinCredentials{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX,
                        std::vector<uint32_t>{INT32_MAX, 0, INT32_MAX}};
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.Credentials->GroupAccessList.reset();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.Credentials = DarwinCredentials{0, 0, 0, 0, std::vector<uint32_t>{0}};
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.Credentials.reset();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
}

class SystemQueryCPU final : public ExecutionBackend {
  std::shared_ptr<AddressSpace> Space;
  GuestArchitecture ISA;

public:
  std::map<CPURegister, RegisterValue> Registers;

  SystemQueryCPU(std::shared_ptr<AddressSpace> Space, GuestArchitecture ISA)
      : Space(std::move(Space)), ISA(ISA) {}
  GuestArchitecture architecture() const override { return ISA; }
  std::shared_ptr<AddressSpace> addressSpace() const override { return Space; }
  llvm::Error bindAddressSpace(std::shared_ptr<AddressSpace>) override {
    return failure("unexpected bind");
  }
  llvm::Expected<RegisterValue> readRegister(CPURegister R) override {
    return Registers[R];
  }
  llvm::Error writeRegister(CPURegister R,
                            const RegisterValue &Value) override {
    Registers[R] = Value;
    return llvm::Error::success();
  }
  llvm::Error map(uint64_t A, uint64_t N, unsigned P) override {
    return Space->map(A, N, P);
  }
  llvm::Error protect(uint64_t A, uint64_t N, unsigned P) override {
    return Space->protect(A, N, P);
  }
  llvm::Expected<bool> canAccess(uint64_t A, uint64_t N,
                                 unsigned P) const override {
    return Space->canAccess(A, N, P);
  }
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) override {
    return Space->read(A, B);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    return Space->write(A, B);
  }
  llvm::Error fetch(uint64_t, llvm::MutableArrayRef<uint8_t>) override {
    return failure("unexpected fetch");
  }
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override {
    return failure("unexpected save");
  }
  llvm::Error saveContext(BackendContext &) override {
    return failure("unexpected save");
  }
  llvm::Error restoreContext(const BackendContext &) override {
    return failure("unexpected restore");
  }
  llvm::Error installHooks(BackendHooks) override {
    return failure("unexpected hooks");
  }
  bool timedOut() const override { return false; }
  void stop() override {}
  bool hasMemoryFault() const override { return false; }
  bool hasDeviceError() const override { return false; }
  std::optional<BackendFault> fault() const override { return std::nullopt; }
  std::optional<BackendFault> takeRecoverableFault() override {
    return std::nullopt;
  }
  bool executable(uint64_t) const override { return false; }
};

TEST_P(DarwinSystemTest, ThreadIdentityTraversesRawBindingAndBothReturnABIs) {
  using enum CPURegister;
  const auto Before = bytes(Base, Page * 2);
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    const bool X64 = ISA == GuestArchitecture::X64;
    ProcessOptions O;
    O.DarwinSystem.emplace();
    SystemQueryCPU CPU(Space, ISA);
    DarwinMemory Memory(*Space, {Page, value::MinimumAddress, {}}, O);
    DarwinFiles Files(CPU, O.DarwinFiles);
    DarwinEntropy Entropy(O.DarwinSystem);
    const ServiceRequest Request{X64 ? ServiceRequestKind::X64Syscall
                                     : ServiceRequestKind::AArch64SVC,
                                 0x200000, 0x200004, uint16_t(X64 ? 0 : 0x80)};
    for (uint64_t ID :
         {0ULL, 0x100000001ULL, 0x8000000000000000ULL, 0xffffffffffffffffULL}) {
      O.DarwinSystem->ThreadID = ID;
      for (uint64_t Prefix :
           {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
        const uint64_t Number = Prefix | (X64 ? 0x2000174 : 372);
        CPU.Registers[X64 ? X64AX : AArch64X16] = {Number, 0};
        CPU.Registers[X64 ? X64DI : AArch64X0] = {UINT64_MAX, 0};
        CPU.Registers[X64 ? X64SI : AArch64X1] = {0x8000000000000000ULL, 0};
        CPU.Registers[X64 ? X64DX : AArch64X2] = {0x123456789abcdef0ULL, 0};
        CPU.Registers[X64 ? X64FLAGS : AArch64NZCV] = {
            X64 ? 0x41ULL : 0xf0000000ULL, 0};
        auto Event = readService(CPU, Request);
        ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
        EXPECT_EQ(Event->Number, Number);
        EXPECT_EQ(Event->Arguments[0], UINT64_MAX);
        EXPECT_EQ(Event->Arguments[1], 0x8000000000000000ULL);
        EXPECT_EQ(Event->Arguments[2], 0x123456789abcdef0ULL);
        EXPECT_FALSE(Event->ThreadID);
        auto R = handleService(CPU, Memory, Files, Entropy, *Event, O, Result);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        ASSERT_TRUE(*R);
        EXPECT_EQ((**R).Value, ID);
        EXPECT_FALSE((**R).Error);
        EXPECT_EQ((**R).Convention, ServiceConvention::BSD);
        ASSERT_FALSE(bool(returnService(CPU, Request, **R)));
        EXPECT_EQ(CPU.Registers[X64 ? X64AX : AArch64X0][0], ID);
        EXPECT_EQ(CPU.Registers[X64 ? X64DX : AArch64X1][0], 0u);
        EXPECT_EQ(CPU.Registers[X64 ? X64FLAGS : AArch64NZCV][0],
                  X64 ? 0x40ULL : 0xd0000000ULL);
        EXPECT_EQ(CPU.Registers[X64 ? X64PC : AArch64PC][0], Request.NextPC);
        EXPECT_FALSE(Event->ThreadID);
        EXPECT_EQ(Event->Number, Number);
        Event->Number = X64 ? 0x1234567801000174ULL
                            : 0x1234567800000000ULL | uint32_t(-372);
        auto Mach =
            handleService(CPU, Memory, Files, Entropy, *Event, O, Result);
        ASSERT_TRUE(bool(Mach)) << llvm::toString(Mach.takeError());
        EXPECT_FALSE(*Mach);
        EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
        EXPECT_EQ(O.DarwinSystem->ThreadID, ID);
      }
    }
  }
  EXPECT_EQ(bytes(Base, Page * 2), Before);
}

INSTANTIATE_TEST_SUITE_P(OSPages, DarwinSystemTest,
                         testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
