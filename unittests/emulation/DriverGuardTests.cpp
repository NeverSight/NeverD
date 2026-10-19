//===- DriverGuardTests.cpp - PE CFG validation and guest calls -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original compiler-generated guarded calls cover active CFG, unmodified
/// guest compatibility fallbacks, and malformed load configuration records.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/driver/GuardControlFlow.h"

#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ProcessObserver.h"
#include "neverd/emulation/WindowsDriverState.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace neverd::emulation {
namespace {
using Config = llvm::object::coff_load_configuration64;

std::filesystem::path guardFixture(const char *Name = "driver_guard") {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}

uint64_t imagePointer(const DriverImage &Image, uint64_t Address) {
  for (const auto &Region : Image.Regions)
    if (Address >= Region.Address &&
        Address - Region.Address <= Region.Bytes.size() &&
        8 <= Region.Bytes.size() - (Address - Region.Address))
      return llvm::support::endian::read64le(Region.Bytes.data() + Address -
                                             Region.Address);
  ADD_FAILURE() << "missing pointer storage";
  return 0;
}

TEST(DriverGuardLoader, ValidatesActiveTargetsAndRebasedPointerSlots) {
  for (uint64_t Base :
       {0x180000000ULL, 0x280000000ULL, 0xfffff80010000000ULL}) {
    auto Image = loadDriverImage(guardFixture(), 64 * 1024 * 1024, Base);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    ASSERT_TRUE(Image->Guard.Enabled);
    ASSERT_EQ(Image->Guard.ValidTargets.size(), 2u);
    EXPECT_NE(Image->Guard.CheckPointerAddress, 0u);
    EXPECT_NE(Image->Guard.DispatchPointerAddress, 0u);
    GuardControlFlow Policy(*Image);
    for (auto Address : Image->Guard.ValidTargets)
      EXPECT_FALSE(static_cast<bool>(Policy.validateTarget(Address)));
    auto Interior = Policy.validateTarget(Image->Entry + 1);
    EXPECT_TRUE(static_cast<bool>(Interior));
    llvm::consumeError(std::move(Interior));
    const uint64_t Check =
        imagePointer(*Image, Image->Guard.CheckPointerAddress);
    const uint64_t Dispatch =
        imagePointer(*Image, Image->Guard.DispatchPointerAddress);
    EXPECT_GE(Check, Base);
    EXPECT_LT(Check, Base + Image->Size);
    EXPECT_GE(Dispatch, Base);
    EXPECT_LT(Dispatch, Base + Image->Size);
    llvm::consumeError(Policy.validateTarget(Check));
  }
}

TEST(DriverGuardLoader, DormantInstrumentationKeepsGuestFallbackPointers) {
  auto Original =
      loadDriverImage(guardFixture("driver_guard_inactive"), 64 * 1024 * 1024);
  auto Rebased = loadDriverImage(guardFixture("driver_guard_inactive"),
                                 64 * 1024 * 1024, 0x280000000ULL);
  ASSERT_TRUE(static_cast<bool>(Original))
      << llvm::toString(Original.takeError());
  ASSERT_TRUE(static_cast<bool>(Rebased))
      << llvm::toString(Rebased.takeError());
  EXPECT_FALSE(Original->Guard.Enabled);
  EXPECT_TRUE(Original->Guard.ValidTargets.empty());
  const auto &A = Original->Guard;
  const auto &B = Rebased->Guard;
  EXPECT_EQ(imagePointer(*Rebased, B.CheckPointerAddress) - Rebased->Base,
            imagePointer(*Original, A.CheckPointerAddress) - Original->Base);
  EXPECT_EQ(imagePointer(*Rebased, B.DispatchPointerAddress) - Rebased->Base,
            imagePointer(*Original, A.DispatchPointerAddress) - Original->Base);
}

TEST(DriverGuardExecution, SessionFixupsInvalidateOriginalLoaderFacts) {
  class InitialObserver final : public ProcessObserver {
  public:
    std::shared_ptr<const ProcessRuntimeState> State;
    llvm::Expected<std::vector<ExecutionWatch>>
    started(ProcessView &P) override {
      auto Captured = P.runtimeState(false);
      if (!Captured)
        return Captured.takeError();
      State = std::move(*Captured);
      return std::vector<ExecutionWatch>{{P.inputModule()->Entry, 1}};
    }
    llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
    watched(ProcessView &, uint64_t) override {
      return std::nullopt;
    }
  };
  for (const char *Name : {"driver_guard", "driver_guard_inactive"}) {
    const auto Path = guardFixture(Name);
    DriverOptions Options;
    Options.LoadAddress = 0xfffff80010000000ULL;
    auto Image =
        loadDriverImage(Path, Options.MemoryLimit, Options.LoadAddress);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    InitialObserver Observer;
    auto Run = observeDriver(Path, Options, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
    ASSERT_TRUE(Observer.State);
    const auto &State =
        static_cast<const WindowsDriverState &>(*Observer.State);
    ASSERT_TRUE(State.Relocations);
    unsigned GuardSlots = 0;
    for (const auto &Field : State.Relocations->Fields) {
      const auto Address = Options.LoadAddress + Field.RVA;
      const bool GuardSlot = Address == Image->Guard.CheckPointerAddress ||
                             Address == Image->Guard.DispatchPointerAddress;
      GuardSlots += GuardSlot;
      EXPECT_EQ(Field.LoaderWritePreserved,
                !(Image->Guard.Enabled && GuardSlot));
      EXPECT_EQ(Field.LoadedValue, imagePointer(*Image, Address));
    }
    EXPECT_EQ(GuardSlots, 2u);
  }
}

TEST(DriverGuardLoader, ImagesCannotOccupyHelpersOrNestedCallbackStacks) {
  for (const char *Fixture : {"driver_guard", "driver_guard_inactive"})
    for (uint64_t Base :
         {profile::GuardThunkBase, profile::CallbackStackBase,
          profile::KernelModuleBase,
          profile::KernelModuleBase + 2 * profile::KernelModuleStride,
          (profile::CallbackStackBase +
           profile::MaxConcurrentCallbacks * profile::CallbackStackStride - 1) &
              ~uint64_t(65535)}) {
      auto Image = loadDriverImage(guardFixture(Fixture),
                                   profile::DefaultMemoryLimit, Base);
      ASSERT_FALSE(bool(Image));
      EXPECT_NE(llvm::toString(Image.takeError()).find("reserved emulation"),
                std::string::npos);
    }
}

TEST(GuardControlFlow, OnlyExplicitImageAndExportEntriesAreValid) {
  DriverImage Image;
  Image.Guard.Enabled = true;
  Image.Guard.ValidTargets = {0x180001000, 0x180001030};
  GuardControlFlow Policy(Image);
  EXPECT_FALSE(static_cast<bool>(Policy.validateTarget(0x180001030)));
  auto Missing = Policy.validateTarget(0x180001031);
  EXPECT_TRUE(static_cast<bool>(Missing));
  llvm::consumeError(std::move(Missing));
  EXPECT_FALSE(static_cast<bool>(Policy.registerExportTarget(0x71001000)));
  EXPECT_FALSE(static_cast<bool>(Policy.validateTarget(0x71001000)));
  auto Null = Policy.registerExportTarget(0);
  EXPECT_TRUE(static_cast<bool>(Null));
  llvm::consumeError(std::move(Null));
  auto Invalid = Policy.registerExportTarget(0x0000800000000000ULL);
  EXPECT_TRUE(static_cast<bool>(Invalid));
  llvm::consumeError(std::move(Invalid));
}

TEST(GuardControlFlow, DisabledImageNeverAcquiresPermissiveHelper) {
  DriverImage Image;
  GuardControlFlow Policy(Image);
  EXPECT_FALSE(static_cast<bool>(Policy.registerExportTarget(0x71001000)));
  auto Error = Policy.validateTarget(0x71001000);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("inactive image"),
            std::string::npos);
}
TEST(GuardControlFlow, PublicationDraftPreservesOwnerSuppressionAndWitnesses) {
  DriverImage Primary;
  GuardControlFlow Live(Primary);
  GuardControlFlow Draft = Live;
  DriverGuardControlFlow Active;
  Active.Enabled = true;
  Active.ValidTargets = {0x180001000};
  unsigned Checks = 0;
  ASSERT_FALSE(bool(Draft.addImage(Active, {0x180002000}, [&]() -> llvm::Error {
    ++Checks;
    return llvm::Error::success();
  })));
  auto Error = Live.validateTarget(0x180001000);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  Live.swap(Draft);
  EXPECT_TRUE(Live.enabled());
  EXPECT_FALSE(Draft.enabled());
  ASSERT_FALSE(bool(Live.validateTarget(0x180001000)));
  EXPECT_EQ(Checks, 1u);
  for (auto Target : {0x180001001ULL, 0x180002000ULL}) {
    auto E = Live.validateTarget(Target);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  EXPECT_EQ(Checks, 1u);
}

TEST(DriverGuardExecution, GuardedCheckAndDispatchPreserveWin64Arguments) {
  for (const char *Name : {"driver_guard", "driver_guard_inactive"}) {
    for (uint64_t Base : {0x180000000ULL, 0x280000000ULL}) {
      DriverOptions Options;
      Options.LoadAddress = Base;
      auto Result = emulateDriver(guardFixture(Name), Options);
      ASSERT_TRUE(static_cast<bool>(Result))
          << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_TRUE(Result->NTStatus);
      EXPECT_EQ(*Result->NTStatus, 0u);
    }
  }
}

TEST(DriverGuardExecution, UndeclaredTargetsFailAtCheckAndDispatch) {
  for (const char *Name :
       {"driver_guard_invalid_check", "driver_guard_invalid_dispatch"}) {
    auto Result = emulateDriver(guardFixture(Name));
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_NE(Result->Diagnostic.find("CFG"), std::string::npos);
    EXPECT_NE(Result->Diagnostic.find("declared entry point"),
              std::string::npos);
  }
}

TEST(DriverGuardExecution, DirectPreservesCheckDispatchAndTargetValidation) {
  auto Probe = createExecutionBackend(ExecutionBackendKind::KVM,
                                      ExecutionContract::DirectDriverX64,
                                      profile::DefaultMemoryLimit);
  if (!Probe) {
    auto Error = Probe.takeError();
    const bool Unavailable = Error.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(Error));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  for (const char *Name :
       {"driver_guard", "driver_guard_inactive", "driver_guard_invalid_check",
        "driver_guard_invalid_dispatch"}) {
    SCOPED_TRACE(Name);
    DriverOptions Options;
    Options.Backend = ExecutionBackendKind::KVM;
    Options.Contract = ExecutionContract::DirectDriverX64;
    Options.TraceMemoryWrites = false;
    auto Result = emulateDriver(guardFixture(Name), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    if (std::string_view(Name).find("invalid") == std::string_view::npos) {
      EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_EQ(Result->NTStatus, 0u);
    } else {
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
          << Result->Diagnostic;
      EXPECT_NE(Result->Diagnostic.find("declared entry point"),
                std::string::npos);
    }
  }
}

class DriverGuardMutation : public ::testing::Test {
protected:
  std::vector<uint8_t> Bytes;
  std::unique_ptr<llvm::object::COFFObjectFile> Object;
  std::filesystem::path Directory;
  size_t ConfigOffset = 0;

  void SetUp() override {
    auto Buffer = llvm::MemoryBuffer::getFile(guardFixture().string());
    ASSERT_TRUE(static_cast<bool>(Buffer));
    auto Data = (*Buffer)->getBuffer();
    Bytes.assign(Data.bytes_begin(), Data.bytes_end());
    auto Parsed = llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                        Bytes.size()),
        "guard-test"));
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError());
    Object = std::move(*Parsed);
    const auto *ConfigDirectory =
        Object->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
    ASSERT_NE(ConfigDirectory, nullptr);
    ConfigOffset = rvaOffset(ConfigDirectory->RelativeVirtualAddress);
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-guard", Temporary));
    Directory = Temporary.str().str();
  }
  void TearDown() override {
    std::error_code Ignored;
    std::filesystem::remove_all(Directory, Ignored);
  }
  size_t rvaOffset(uint64_t RVA) {
    uintptr_t Pointer = 0;
    if (auto Error = Object->getRvaPtr(RVA, Pointer)) {
      ADD_FAILURE() << llvm::toString(std::move(Error));
      return 0;
    }
    return reinterpret_cast<const uint8_t *>(Pointer) - Bytes.data();
  }
  uint64_t readField(size_t Offset) const {
    return llvm::support::endian::read64le(Bytes.data() + ConfigOffset +
                                           Offset);
  }
  void field(size_t Offset, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + ConfigOffset + Offset,
                                     Value);
  }
  void flags(uint32_t Value) {
    llvm::support::endian::write32le(
        Bytes.data() + ConfigOffset + offsetof(Config, GuardFlags), Value);
  }
  void rejects(llvm::StringRef Diagnostic, uint64_t Base = 0) {
    const auto Path = Directory / "modified.sys";
    std::ofstream Stream(Path, std::ios::binary);
    Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Stream.close();
    ASSERT_TRUE(Stream);
    auto Image = loadDriverImage(Path, 64 * 1024 * 1024, Base);
    ASSERT_FALSE(static_cast<bool>(Image));
    const auto Message = llvm::toString(Image.takeError());
    EXPECT_NE(Message.find(Diagnostic.str()), std::string::npos) << Message;
  }
  void replaceRelocation(uint64_t RVA, uint16_t NewEntry = 0) {
    const auto *Relocations =
        Object->getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE);
    ASSERT_NE(Relocations, nullptr);
    size_t Offset = rvaOffset(Relocations->RelativeVirtualAddress);
    const size_t End = Offset + Relocations->Size;
    while (Offset + 8 <= End) {
      const uint32_t Page =
          llvm::support::endian::read32le(Bytes.data() + Offset);
      const uint32_t Size =
          llvm::support::endian::read32le(Bytes.data() + Offset + 4);
      ASSERT_GE(Size, 8u);
      for (size_t Position = Offset + 8; Position + 2 <= Offset + Size;
           Position += 2) {
        const uint16_t Entry =
            llvm::support::endian::read16le(Bytes.data() + Position);
        if ((Entry >> 12) == llvm::COFF::IMAGE_REL_BASED_DIR64 &&
            Page + (Entry & 0xfff) == RVA) {
          llvm::support::endian::write16le(Bytes.data() + Position, NewEntry);
          return;
        }
      }
      Offset += Size;
    }
    FAIL() << "missing fixture relocation";
  }
  size_t tableOffset() {
    return rvaOffset(readField(offsetof(Config, GuardCFFunctionTable)) -
                     Object->getImageBase());
  }
};

TEST_F(DriverGuardMutation, AcceptsByteAlignedPackedTargetTables) {
  const auto Original = Bytes;
  const uint64_t OriginalTable =
      readField(offsetof(Config, GuardCFFunctionTable));
  const size_t Offset = tableOffset();
  const uint64_t Count = readField(offsetof(Config, GuardCFFunctionCount));
  ASSERT_EQ(Count, 2u);
  std::vector<uint64_t> Expected;
  for (uint64_t Index = 0; Index < Count; ++Index)
    Expected.push_back(
        llvm::support::endian::read32le(Original.data() + Offset + Index * 4));
  // LLD's packed RVA-table chunk has byte alignment. Its referenced code
  // remains independently aligned; table storage need not be a DWORD boundary.
  for (uint64_t Shift : {1, 2, 3}) {
    std::copy(Original.begin(), Original.end(), Bytes.begin());
    const uint64_t TableRVA = OriginalTable - Object->getImageBase() + Shift;
    field(offsetof(Config, GuardCFFunctionTable), OriginalTable + Shift);
    std::copy_n(Original.data() + Offset, Count * 4,
                Bytes.data() + Offset + Shift);
    for (const auto &Reference : Object->sections()) {
      const auto *Section = Object->getCOFFSection(Reference);
      if (TableRVA < Section->VirtualAddress ||
          TableRVA - Section->VirtualAddress >= Section->SizeOfRawData)
        continue;
      const size_t Position =
          reinterpret_cast<const uint8_t *>(&Section->VirtualSize) -
          Bytes.data();
      llvm::support::endian::write32le(
          Bytes.data() + Position,
          std::max<uint64_t>(Section->VirtualSize,
                             TableRVA - Section->VirtualAddress + Count * 4));
      break;
    }
    const auto Path = Directory / "unaligned-table.sys";
    std::ofstream Stream(Path, std::ios::binary);
    Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Stream.close();
    for (uint64_t Base : {0x180000000ULL, 0x280000000ULL}) {
      SCOPED_TRACE(Shift);
      SCOPED_TRACE(Base);
      auto Image = loadDriverImage(Path, 64 * 1024 * 1024, Base);
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      ASSERT_EQ(Image->Guard.ValidTargets.size(), Expected.size());
      for (size_t Index = 0; Index != Expected.size(); ++Index)
        EXPECT_EQ(Image->Guard.ValidTargets[Index], Base + Expected[Index]);
    }
  }
}

TEST_F(DriverGuardMutation, RejectsMissingActiveFlags) {
  flags(guard::Instrumented);
  rejects("requires instrumented and function-table flags");
}
TEST_F(DriverGuardMutation, RejectsActiveXfg) {
  flags(guard::Instrumented | guard::FunctionTablePresent |
        guard::XfgInstrumented);
  rejects("unsupported CFG GuardFlags");
}
TEST_F(DriverGuardMutation, RejectsUnsupportedStride) {
  flags(guard::Instrumented | guard::FunctionTablePresent |
        guard::FunctionTableTwoExtraBytes);
  rejects("metadata stride");
}
TEST_F(DriverGuardMutation, RejectsMissingCheckSlot) {
  field(offsetof(Config, GuardCFCheckFunction), 0);
  rejects("requires a check pointer slot");
}
TEST_F(DriverGuardMutation, RejectsExternalCheckSlot) {
  field(offsetof(Config, GuardCFCheckFunction), 0x71001000);
  rejects("outside image storage");
}
TEST_F(DriverGuardMutation, RejectsSlotAliasing) {
  field(offsetof(Config, GuardCFCheckDispatch),
        readField(offsetof(Config, GuardCFCheckFunction)));
  rejects("overlapping CFG metadata storage");
}
TEST_F(DriverGuardMutation, RejectsExternalFallbackTarget) {
  const auto Offset =
      rvaOffset(readField(offsetof(Config, GuardCFCheckFunction)) -
                Object->getImageBase());
  llvm::support::endian::write64le(Bytes.data() + Offset, 0x71001000);
  rejects("fallback target");
}
TEST_F(DriverGuardMutation, RejectsWritableGuardStorage) {
  for (const auto &Reference : Object->sections()) {
    const auto *Section = Object->getCOFFSection(Reference);
    const uint64_t RVA = readField(offsetof(Config, GuardCFCheckFunction)) -
                         Object->getImageBase();
    if (RVA < Section->VirtualAddress ||
        RVA - Section->VirtualAddress >= Section->VirtualSize)
      continue;
    const size_t Offset =
        reinterpret_cast<const uint8_t *>(&Section->Characteristics) -
        Bytes.data();
    llvm::support::endian::write32le(Bytes.data() + Offset,
                                     Section->Characteristics |
                                         llvm::COFF::IMAGE_SCN_MEM_WRITE);
    rejects("read-only nonexecutable");
    return;
  }
  FAIL() << "missing guard storage section";
}
TEST_F(DriverGuardMutation, RejectsMissingCount) {
  field(offsetof(Config, GuardCFFunctionCount), 0);
  rejects("inconsistent CFG table pointer and count");
}
TEST_F(DriverGuardMutation, RejectsOversizedTableBeforeReading) {
  field(offsetof(Config, GuardCFFunctionCount), guard::MaximumTargets + 1);
  rejects("target count exceeds");
}
TEST_F(DriverGuardMutation, RejectsDuplicateTargets) {
  auto Offset = tableOffset();
  llvm::support::endian::write32le(
      Bytes.data() + Offset + 4,
      llvm::support::endian::read32le(Bytes.data() + Offset));
  rejects("unique and sorted");
}
TEST_F(DriverGuardMutation, RejectsNonExecutableTarget) {
  llvm::support::endian::write32le(Bytes.data() + tableOffset(), 0x2000);
  rejects("file-backed executable code");
}
TEST_F(DriverGuardMutation, HonorsExplicitlyDeclaredFallbackTargets) {
  const auto Slot =
      rvaOffset(readField(offsetof(Config, GuardCFCheckFunction)) -
                Object->getImageBase());
  const uint64_t Fallback =
      llvm::support::endian::read64le(Bytes.data() + Slot);
  const uint32_t FallbackRVA = Fallback - Object->getImageBase();
  const uint32_t Entry = Object->getPE32PlusHeader()->AddressOfEntryPoint;
  const auto Offset = tableOffset();
  llvm::support::endian::write32le(Bytes.data() + Offset,
                                   std::min(FallbackRVA, Entry));
  llvm::support::endian::write32le(Bytes.data() + Offset + 4,
                                   std::max(FallbackRVA, Entry));
  const auto Path = Directory / "declared-fallback.sys";
  std::ofstream Stream(Path, std::ios::binary);
  Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  Stream.close();
  auto Image = loadDriverImage(Path, 64 * 1024 * 1024);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  GuardControlFlow Policy(*Image);
  EXPECT_FALSE(static_cast<bool>(Policy.validateTarget(Fallback)));
}
TEST_F(DriverGuardMutation, RejectsNonzeroDormantExtension) {
  const auto Slot =
      rvaOffset(readField(offsetof(Config, CastGuardOsDeterminedFailureMode)) -
                Object->getImageBase());
  llvm::support::endian::write64le(Bytes.data() + Slot, 1);
  rejects("unsupported active load configuration guard extension");
}
TEST_F(DriverGuardMutation, RejectsMissingSlotRelocationOnRebase) {
  replaceRelocation(readField(offsetof(Config, GuardCFCheckFunction)) -
                    Object->getImageBase());
  rejects("requires complete DIR64 relocations", 0x280000000ULL);
}
TEST_F(DriverGuardMutation, RejectsMissingFieldRelocationOnRebase) {
  const auto *Directory =
      Object->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
  replaceRelocation(Directory->RelativeVirtualAddress +
                    offsetof(Config, GuardCFCheckFunction));
  rejects("requires complete DIR64 relocations", 0x280000000ULL);
}
TEST_F(DriverGuardMutation, RejectsRelocationIntoTargetTable) {
  const auto *Directory =
      Object->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
  const uint64_t RVA = Directory->RelativeVirtualAddress +
                       offsetof(Config, GuardCFFunctionTable);
  const uint64_t Table = readField(offsetof(Config, GuardCFFunctionTable)) -
                         Object->getImageBase();
  ASSERT_EQ(RVA & ~0xfffULL, Table & ~0xfffULL);
  replaceRelocation(RVA, (llvm::COFF::IMAGE_REL_BASED_DIR64 << 12) |
                             (Table & 0xfff));
  rejects("DIR64 relocation overlaps CFG metadata");
}
TEST_F(DriverGuardMutation, RejectsPartialGuardField) {
  llvm::support::endian::write32le(Bytes.data() + ConfigOffset,
                                   offsetof(Config, GuardFlags) + 1);
  rejects("partial load configuration GuardFlags");
}
TEST_F(DriverGuardMutation, HonorsSuppressedFunctionMetadata) {
  const uint64_t Table = readField(offsetof(Config, GuardCFFunctionTable)) -
                         Object->getImageBase();
  const size_t Offset = tableOffset();
  const uint32_t First = llvm::support::endian::read32le(Bytes.data() + Offset);
  const uint32_t Second =
      llvm::support::endian::read32le(Bytes.data() + Offset + 4);
  const uint32_t Entry = Object->getPE32PlusHeader()->AddressOfEntryPoint;
  ASSERT_TRUE(First == Entry || Second == Entry);
  for (const auto &Reference : Object->sections()) {
    const auto *Section = Object->getCOFFSection(Reference);
    if (Table < Section->VirtualAddress ||
        Table - Section->VirtualAddress >= Section->VirtualSize)
      continue;
    const size_t Position =
        reinterpret_cast<const uint8_t *>(&Section->VirtualSize) - Bytes.data();
    llvm::support::endian::write32le(
        Bytes.data() + Position,
        std::max<uint64_t>(Section->VirtualSize,
                           Table - Section->VirtualAddress + 10));
  }
  flags(guard::Instrumented | guard::FunctionTablePresent |
        guard::FunctionTableOneExtraByte);
  Bytes[Offset + 4] = First == Entry ? 0 : guard::FunctionSuppressed;
  llvm::support::endian::write32le(Bytes.data() + Offset + 5, Second);
  Bytes[Offset + 9] = Second == Entry ? 0 : guard::FunctionSuppressed;
  const auto Path = Directory / "suppressed.sys";
  std::ofstream Stream(Path, std::ios::binary);
  Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  Stream.close();
  auto Image = loadDriverImage(Path, 64 * 1024 * 1024);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Guard.ValidTargets.size(), 1u);
  EXPECT_EQ(Image->Guard.ValidTargets.front(), Image->Entry);
}
TEST_F(DriverGuardMutation, RejectsUnknownNonzeroExtensionBytes) {
  llvm::support::endian::write32le(Bytes.data() + ConfigOffset,
                                   guard::KnownLoadConfigurationSize + 8);
  rejects("unsupported nonzero load configuration extension");
}
} // namespace
} // namespace neverd::emulation
