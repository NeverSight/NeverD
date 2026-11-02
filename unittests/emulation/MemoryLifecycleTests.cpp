//===- MemoryLifecycleTests.cpp - Independent RAM, space and CPU owners ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <cstdlib>
#include <thread>

namespace neverd::emulation {
namespace {
#define NEVERD_MEMORY_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MEMORY_TEST_X64(Name, ...)                                      \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_MEMORY_TEST_ARM(Name, ...)                                      \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#define NEVERD_MEMORY_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "MemoryLifecycleCases.def"
#undef NEVERD_MEMORY_TEST_VALUE
#undef NEVERD_MEMORY_TEST_X64
#undef NEVERD_MEMORY_TEST_ARM
#undef NEVERD_MEMORY_TEST_TEXT

std::shared_ptr<PhysicalMemory> ram(uint64_t Budget = Limit) {
  return llvm::cantFail(PhysicalMemory::create(Budget));
}
std::shared_ptr<AddressSpace> space(std::shared_ptr<PhysicalMemory> RAM,
                                    uint64_t Budget = Limit) {
  return llvm::cantFail(AddressSpace::create(std::move(RAM), Budget));
}
std::vector<uint8_t> instructionBytes(GuestArchitecture ISA,
                                      llvm::ArrayRef<uint8_t> X64,
                                      llvm::ArrayRef<uint32_t> ARM) {
  if (ISA == GuestArchitecture::X64)
    return {X64.begin(), X64.end()};
  std::vector<uint8_t> Bytes(ARM.size() * sizeof(uint32_t));
  for (size_t N = 0; N < ARM.size(); ++N)
    llvm::support::endian::write32le(Bytes.data() + N * sizeof(uint32_t),
                                     ARM[N]);
  return Bytes;
}
TEST(MemoryLifecycle, UnmappedBackingRetainsCapacityAndIdentity) {
  auto RAM = ram(PageSize * 2);
  auto A = space(RAM), B = space(RAM);
  const auto Generation = A->mappingGeneration();
  auto View = llvm::cantFail(A->allocateBacking(PageSize * 2));
  EXPECT_EQ(View.addressSpaceIdentity(), A->identity());
  EXPECT_EQ(A->mappedBytes(), 0u);
  EXPECT_EQ(A->mappingGeneration(), Generation);
  std::vector<uint8_t> Bytes(PageSize * 2, 0xff);
  ASSERT_EQ(llvm::toString(View.read(0, Bytes)), "");
  EXPECT_EQ(Bytes, std::vector<uint8_t>(PageSize * 2));
  auto Full = B->allocateBacking(PageSize);
  ASSERT_FALSE(bool(Full));
  auto Error = Full.takeError();
  EXPECT_TRUE(Error.isA<GuestMemoryLimitError>());
  llvm::consumeError(std::move(Error));
  auto Invalid = A->allocateBacking(1);
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  {
    auto Slice = View.slices().front();
    ASSERT_EQ(llvm::toString(
                  B->mapRegion(Data, Slice.Region, 0, PageSize, Read | Write)),
              "");
  }
  const uint8_t Value[] = {1, 2, 3, 4};
  ASSERT_EQ(llvm::toString(View.write(0, Value)), "");
  EXPECT_EQ(llvm::cantFail(B->readInteger(Data, 4)), 0x04030201u);
  View = {};
  A.reset();
  EXPECT_EQ(RAM->allocatedBytes(), PageSize * 2);
  ASSERT_EQ(llvm::toString(B->unmap(Data, PageSize)), "");
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  auto Reused = llvm::cantFail(B->allocateBacking(PageSize));
  ASSERT_EQ(llvm::toString(
                Reused.read(0, llvm::MutableArrayRef(Bytes).take_front(4))),
            "");
  EXPECT_EQ(std::vector<uint8_t>(Bytes.begin(), Bytes.begin() + 4),
            std::vector<uint8_t>(4));
}

TEST(MemoryLifecycle, DistinctSpacesAndSharedRegionsHaveIndependentLifetimes) {
  auto RAM = ram();
  auto A = space(RAM), B = space(RAM);
  ASSERT_EQ(llvm::toString(A->map(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(B->map(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->writeInteger(Data, Value, sizeof(uint64_t))), "");
  EXPECT_EQ(*B->readInteger(Data, sizeof(uint64_t)), 0u);
  auto Shared = llvm::cantFail(RAM->allocate(PageSize));
  ASSERT_EQ(
      llvm::toString(A->mapRegion(Alias, Shared, 0, PageSize, Read | Write)),
      "");
  ASSERT_EQ(
      llvm::toString(B->mapRegion(Other, Shared, 0, PageSize, Read | Write)),
      "");
  ASSERT_EQ(llvm::toString(A->writeInteger(Alias, Updated, sizeof(uint64_t))),
            "");
  EXPECT_EQ(*B->readInteger(Other, sizeof(uint64_t)), Updated);
  std::weak_ptr<PhysicalMemory> Owner = RAM;
  Shared.reset();
  A.reset();
  RAM.reset();
  EXPECT_FALSE(Owner.expired());
  EXPECT_EQ(*B->readInteger(Other, sizeof(uint64_t)), Updated);
  B.reset();
  EXPECT_TRUE(Owner.expired());
}
TEST(MemoryLifecycle, CanonicalUnmapLeavesAliasesAndReclaimsOnlyTheFinalOwner) {
  auto RAM = ram(PageSize);
  auto A = space(RAM);
  ASSERT_EQ(llvm::toString(A->map(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->mapAlias(Alias, Data, PageSize, Read | Write)),
            "");
  ASSERT_EQ(llvm::toString(A->writeInteger(Data, Value, sizeof(uint64_t))), "");
  ASSERT_EQ(llvm::toString(A->unmap(Data, PageSize)), "");
  EXPECT_EQ(*A->readInteger(Alias, sizeof(uint64_t)), Value);
  auto Full = RAM->allocate(PageSize);
  ASSERT_FALSE(bool(Full));
  auto E = Full.takeError();
  EXPECT_TRUE(E.isA<GuestMemoryLimitError>());
  llvm::consumeError(std::move(E));
  ASSERT_EQ(llvm::toString(A->unmapAlias(Alias, PageSize)), "");
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  ASSERT_EQ(llvm::toString(A->map(Data, PageSize, Read | Write)), "");
  EXPECT_EQ(*A->readInteger(Data, sizeof(uint64_t)), 0u);
}
TEST(MemoryLifecycle,
     MappingSnapshotsDescribeRightsAndDevicesWithoutPinningRAM) {
  auto RAM = ram();
  auto A = space(RAM);
  ASSERT_EQ(llvm::toString(A->map(Data, 2 * PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->mapAlias(Alias, Data, PageSize, Read)), "");
  GuestMMIOCallbacks IO;
  IO.Validate = [](uint64_t, uint64_t, bool) { return llvm::Error::success(); };
  IO.Read = [](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    return Value;
  };
  IO.Write = [](uint64_t, unsigned, uint64_t) {
    return llvm::Error::success();
  };
  ASSERT_EQ(llvm::toString(A->mapMMIO(Other, PageSize, std::move(IO))), "");
  const auto Snapshot = llvm::cantFail(A->mappings());
  ASSERT_EQ(Snapshot.size(), 3u);
  EXPECT_EQ(Snapshot[0].Address, Data);
  EXPECT_EQ(Snapshot[0].Size, 2 * PageSize);
  EXPECT_EQ(Snapshot[0].Permissions, Read | Write);
  EXPECT_FALSE(Snapshot[0].Device);
  EXPECT_EQ(Snapshot[1].Address, Alias);
  EXPECT_EQ(Snapshot[1].Permissions, Read);
  EXPECT_EQ(Snapshot[2].Address, Other);
  EXPECT_TRUE(Snapshot[2].Device);
  ASSERT_EQ(llvm::toString(A->protect(Data + PageSize, PageSize, Read)), "");
  const auto Split = llvm::cantFail(A->mappings());
  ASSERT_EQ(Split.size(), 4u);
  EXPECT_EQ(Split[0].Size, PageSize);
  EXPECT_EQ(Split[1].Address, Data + PageSize);
  EXPECT_EQ(Split[1].Permissions, Read);
  EXPECT_EQ(Snapshot[0].Size, 2 * PageSize);
  ASSERT_EQ(llvm::toString(A->unmap(Data, 2 * PageSize)), "");
  ASSERT_EQ(llvm::toString(A->unmapAlias(Alias, PageSize)), "");
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  ASSERT_EQ(llvm::toString(A->unmapMMIO(Other, PageSize)), "");
  EXPECT_TRUE(llvm::cantFail(A->mappings()).empty());
}
TEST(MemoryLifecycle, FailedTransactionsPreserveMappingGenerationAndBudgets) {
  auto RAM = ram();
  auto A = space(RAM, 2 * PageSize);
  auto Region = llvm::cantFail(RAM->allocate(PageSize));
  ASSERT_EQ(
      llvm::toString(A->mapRegion(Data, Region, 0, PageSize, Read | Write)),
      "");
  ASSERT_EQ(llvm::toString(A->mapAlias(Alias, Data, PageSize, Read)), "");
  const auto Generation = A->mappingGeneration();
  EXPECT_NE(llvm::toString(A->replaceAliases({{Alias, PageSize}},
                                             {{Other, Data, PageSize, Read},
                                              {Alias, Other, PageSize, Read}})),
            "");
  EXPECT_EQ(A->mappingGeneration(), Generation);
  EXPECT_EQ(A->mappedBytes(), 2 * PageSize);
  EXPECT_EQ(RAM->allocatedBytes(), PageSize);
  EXPECT_TRUE(*A->canAccess(Alias, PageSize, Read));
  EXPECT_FALSE(*A->canAccess(Other, PageSize, Read));
  auto Foreign = llvm::cantFail(ram()->allocate(PageSize));
  EXPECT_NE(llvm::toString(A->mapRegion(Other, Foreign, 0, PageSize, Read)),
            "");
  EXPECT_EQ(A->mappingGeneration(), Generation);
  EXPECT_NE(llvm::toString(A->map(Data + 1, PageSize, Read)), "");
  EXPECT_NE(llvm::toString(A->unmap(Data, 3 * PageSize)), "");
  EXPECT_EQ(A->mappingGeneration(), Generation);
}
TEST(MemoryLifecycle,
     RetainedViewsSurviveAddressReuseAndAddressSpaceDestruction) {
  auto RAM = ram();
  auto A = space(RAM);
  std::weak_ptr<AddressSpace> Original = A;
  ASSERT_EQ(llvm::toString(A->map(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->mapAlias(Alias, Data, PageSize, Read)), "");
  ASSERT_EQ(llvm::toString(A->writeInteger(Data, Value, sizeof(uint64_t))), "");
  auto View = llvm::cantFail(A->pinBacking(Data, sizeof(uint64_t)));
  auto AliasView = llvm::cantFail(A->pinBacking(Alias, sizeof(uint64_t)));
  EXPECT_TRUE(View.describesSameBytes(AliasView));
  EXPECT_TRUE(AliasView.overlaps(View));
  EXPECT_EQ(View.slices().front().Region, AliasView.slices().front().Region);
  EXPECT_EQ(View.slices().front().Offset, AliasView.slices().front().Offset);
  ASSERT_EQ(llvm::toString(A->unmap(Data, PageSize)), "");
  ASSERT_EQ(llvm::toString(A->unmapAlias(Alias, PageSize)), "");
  ASSERT_EQ(llvm::toString(A->map(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->writeInteger(Data, Updated, sizeof(uint64_t))),
            "");
  auto Reused = llvm::cantFail(A->pinBacking(Data, sizeof(uint64_t)));
  EXPECT_FALSE(View.describesSameBytes(Reused));
  EXPECT_FALSE(View.overlaps(Reused));
  Reused = {};
  std::array<uint8_t, sizeof(uint64_t)> Bytes{};
  ASSERT_EQ(llvm::toString(View.read(0, Bytes)), "");
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), Value);
  llvm::support::endian::write64le(Bytes.data(), Value + 1);
  ASSERT_EQ(llvm::toString(View.write(0, Bytes)), "");
  EXPECT_EQ(*A->readInteger(Data, sizeof(uint64_t)), Updated);
  A.reset();
  EXPECT_TRUE(Original.expired());
  ASSERT_EQ(llvm::toString(AliasView.read(0, Bytes)), "");
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), Value + 1);
  EXPECT_EQ(RAM->allocatedBytes(), PageSize);
  View = {};
  AliasView = {};
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
}
TEST(MemoryLifecycle, ViewSubrangesPreflightEveryAllocationBeforeEffects) {
  auto A = space(ram());
  ASSERT_EQ(llvm::toString(A->map(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->map(Data + PageSize, PageSize, Read | Write)),
            "");
  const uint64_t Start = Data + PageSize - sizeof(uint32_t);
  auto View = llvm::cantFail(A->pinBacking(Start, sizeof(uint64_t)));
  ASSERT_EQ(View.slices().size(), 2u);
  ASSERT_EQ(llvm::toString(A->protect(Data, 2 * PageSize, 0)), "");
  std::array<uint8_t, sizeof(uint64_t)> Bytes{};
  llvm::support::endian::write64le(Bytes.data(), Updated);
  ASSERT_EQ(llvm::toString(View.write(0, Bytes)), "");
  auto Before = Bytes;
  EXPECT_NE(llvm::toString(View.read(1, Bytes)), "");
  EXPECT_EQ(Bytes, Before);
  EXPECT_NE(llvm::toString(View.write(UINT64_MAX, Bytes)), "");
  auto Invalid = View.subview(1, sizeof(uint64_t));
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  auto Part = llvm::cantFail(View.subview(1, sizeof(uint32_t)));
  EXPECT_TRUE(View.overlaps(Part));
  EXPECT_FALSE(View.describesSameBytes(Part));
  EXPECT_TRUE(Part.describesSameBytes(
      llvm::cantFail(A->pinBacking(Start + 1, sizeof(uint32_t)))));
  auto Left = llvm::cantFail(View.subview(0, sizeof(uint32_t)));
  auto Right = llvm::cantFail(View.subview(sizeof(uint32_t), sizeof(uint32_t)));
  EXPECT_FALSE(Left.overlaps(Right));
  EXPECT_FALSE(Left.describesSameBytes(Right));
  auto Empty = llvm::cantFail(View.subview(View.size(), 0));
  EXPECT_FALSE(Empty.overlaps(View));
  EXPECT_FALSE(MemoryView().describesSameBytes(Empty));
  EXPECT_NE(llvm::toString(View.validateAccess(View.size(), 1)), "");
  EXPECT_EQ(llvm::toString(A->validatePinned(View, View.size(), 0)), "");
  ASSERT_EQ(llvm::toString(
                Part.read(0, llvm::MutableArrayRef<uint8_t>(Bytes).take_front(
                                 sizeof(uint32_t)))),
            "");
  EXPECT_EQ(Bytes[0], Before[1]);
  auto Foreign = space(ram());
  EXPECT_NE(llvm::toString(Foreign->validatePinned(View, 0, Before.size())),
            "");
  EXPECT_NE(llvm::toString(Foreign->writePinned(View, 0, Before)), "");
  EXPECT_FALSE(*A->canAccess(Start, sizeof(uint64_t), Read));
  ASSERT_EQ(llvm::toString(View.read(0, Bytes)), "");
  EXPECT_EQ(Bytes, Before);
}
TEST(MemoryLifecycle, PartialAliasRemovalLeavesExactRetirableFragments) {
  auto RAM = ram();
  auto A = space(RAM);
  ASSERT_EQ(llvm::toString(A->map(Data, 3 * PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->mapAlias(Alias, Data, 3 * PageSize, Read)), "");
  ASSERT_EQ(llvm::toString(A->unmap(Alias + PageSize, PageSize)), "");
  ASSERT_EQ(llvm::toString(A->unmapAlias(Alias, PageSize)), "");
  ASSERT_EQ(llvm::toString(A->unmapAlias(Alias + 2 * PageSize, PageSize)), "");
  EXPECT_EQ(A->mappedBytes(), 3 * PageSize);
  EXPECT_EQ(RAM->allocatedBytes(), 3 * PageSize);
}
TEST(MemoryLifecycle,
     ProjectionFailureIsLocalAndDoesNotRollBackPublishedMappings) {
  const auto Software = llvm::cantFail(queryExecutionBackendBuild(
      ExecutionBackendKind::Unicorn, GuestArchitecture::X64));
  if (Software.Availability != BackendAvailability::Available)
    GTEST_SKIP() << Software.Reason;
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    SCOPED_TRACE(guestArchitectureName(ISA));
    auto RAM = ram();
    auto Sparse = space(RAM, (SparsePages + 1) * PageSize);
    auto Healthy = space(RAM);
    auto Page = llvm::cantFail(RAM->allocate(PageSize));
    ASSERT_EQ(
        llvm::toString(Sparse->map(Code, PageSize, Read | Write | Execute)),
        "");
    ASSERT_EQ(
        llvm::toString(Healthy->map(Code, PageSize, Read | Write | Execute)),
        "");
    auto Bytes = instructionBytes(ISA, FirstX64, FirstARM);
    ASSERT_EQ(llvm::toString(Sparse->write(Code, Bytes)), "");
    ASSERT_EQ(llvm::toString(Healthy->write(Code, Bytes)), "");
    for (uint64_t N = 1; N <= SparsePages; ++N)
      ASSERT_EQ(llvm::toString(Sparse->mapRegion(N * SparseStride, Page, 0,
                                                 PageSize, Read | Write)),
                "");
    const auto Contract = ISA == GuestArchitecture::X64
                              ? ExecutionContract::CheckedX64
                              : ExecutionContract::CheckedAArch64;
    auto Failed = llvm::cantFail(createExecutionBackend(
        ExecutionBackendKind::Unicorn, Contract, Sparse, ISA));
    auto Good = llvm::cantFail(createExecutionBackend(
        ExecutionBackendKind::Unicorn, Contract, Healthy, ISA));
    const auto Generation = Sparse->mappingGeneration();
    const auto Used = Sparse->mappedBytes();
    EXPECT_NE(llvm::toString(Failed.CPU->run(Code, Timeout)), "");
    ASSERT_TRUE(Failed.CPU->fault());
    EXPECT_EQ(Failed.CPU->fault()->Kind, BackendFaultKind::UnhandledException);
    EXPECT_NE(llvm::toString(Failed.CPU->run(Code, Timeout)), "");
    EXPECT_EQ(Sparse->mappingGeneration(), Generation);
    EXPECT_EQ(Sparse->mappedBytes(), Used);
    EXPECT_TRUE(
        *Sparse->canAccess(SparsePages * SparseStride, PageSize, Write));
    unsigned Seen = 0;
    BackendHooks H;
    H.Instruction = [&](uint64_t, uint32_t) {
      if (Seen++)
        Good.CPU->stop();
    };
    ASSERT_EQ(llvm::toString(Good.CPU->installHooks(std::move(H))), "");
    ASSERT_EQ(llvm::toString(Good.CPU->run(Code, Timeout)), "");
    auto R = ISA == GuestArchitecture::X64 ? CPURegister::X64AX
                                           : CPURegister::AArch64X0;
    EXPECT_EQ((*Good.CPU->readRegister(R))[0], Value);
  }
}
TEST(MemoryLifecycle, SharedDeviceRetirementReleasesCallbacksBeforeCPUsResume) {
  const auto Software = llvm::cantFail(queryExecutionBackendBuild(
      ExecutionBackendKind::Unicorn, GuestArchitecture::X64));
  if (Software.Availability != BackendAvailability::Available)
    GTEST_SKIP() << Software.Reason;
  auto A = space(ram());
  auto First = llvm::cantFail(createExecutionBackend(
      ExecutionBackendKind::Unicorn, ExecutionContract::Software, A));
  auto Second = llvm::cantFail(createExecutionBackend(
      ExecutionBackendKind::Unicorn, ExecutionContract::Software, A));
  auto Lifetime = std::make_shared<uint64_t>(Value);
  std::weak_ptr<uint64_t> Weak = Lifetime;
  // Destroy caller-held callbacks before testing the mapping's ownership.
  {
    GuestMMIOCallbacks IO;
    IO.Validate = [&](uint64_t, uint64_t, bool) {
      // Host MMIO validation participates in the same physical-owner lease.
      EXPECT_NE(llvm::toString(A->unmapMMIO(Data, PageSize)), "");
      EXPECT_NE(llvm::toString(Second.CPU->run(Code, Timeout)), "");
      return llvm::Error::success();
    };
    IO.Read = [Lifetime](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      return *Lifetime;
    };
    IO.Write = [](uint64_t, unsigned, uint64_t) {
      return llvm::Error::success();
    };
    Lifetime.reset();
    ASSERT_EQ(llvm::toString(A->mapMMIO(Data, PageSize, std::move(IO))), "");
  }
  EXPECT_FALSE(Weak.expired());
  EXPECT_EQ(*First.CPU->readInteger(Data, sizeof(uint32_t)), Value);
  EXPECT_EQ(*Second.CPU->readInteger(Data, sizeof(uint32_t)), Value);
  ASSERT_EQ(llvm::toString(A->unmapMMIO(Data, PageSize)), "");
  EXPECT_TRUE(Weak.expired());
  ASSERT_EQ(llvm::toString(A->map(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->writeInteger(Data, Updated, sizeof(uint32_t))),
            "");
  EXPECT_EQ(*First.CPU->readInteger(Data, sizeof(uint32_t)), Updated);
  EXPECT_EQ(*Second.CPU->readInteger(Data, sizeof(uint32_t)), Updated);
}
struct CPUProfile {
  const char *Name;
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
  GuestArchitecture ISA;
};
void PrintTo(const CPUProfile &Profile, std::ostream *OS) {
  *OS << Profile.Name;
}
#if defined(_WIN32)
constexpr auto Native = ExecutionBackendKind::WHP;
#elif defined(__APPLE__)
constexpr auto Native = ExecutionBackendKind::HVF;
#else
constexpr auto Native = ExecutionBackendKind::KVM;
#endif
class SharedCPU : public testing::TestWithParam<CPUProfile> {
protected:
  std::shared_ptr<PhysicalMemory> RAM;
  std::shared_ptr<AddressSpace> A, B;
  std::unique_ptr<ExecutionBackend> First, Second;
  void SetUp() override {
    RAM = ram();
    A = space(RAM);
    B = space(RAM);
    const auto P = GetParam();
    auto Result = createExecutionBackend(P.Backend, P.Contract, A, P.ISA);
    if (!Result) {
      auto E = Result.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(P.Backend, P.ISA) &&
          !(P.ISA == GuestArchitecture::AArch64 && std::getenv(RequireNative)))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    First = std::move(Result->CPU);
    auto Next = createExecutionBackend(P.Backend, P.Contract, B, P.ISA);
    ASSERT_TRUE(bool(Next)) << llvm::toString(Next.takeError());
    Second = std::move(Next->CPU);
    auto Instructions = llvm::cantFail(RAM->allocate(PageSize));
    auto SharedData = llvm::cantFail(RAM->allocate(PageSize));
    ASSERT_EQ(llvm::toString(A->mapRegion(Code, Instructions, 0, PageSize,
                                          Read | Write | Execute)),
              "");
    ASSERT_EQ(llvm::toString(B->mapRegion(Code, Instructions, 0, PageSize,
                                          Read | Write | Execute)),
              "");
    ASSERT_EQ(llvm::toString(
                  A->mapRegion(Data, SharedData, 0, PageSize, Read | Write)),
              "");
    ASSERT_EQ(llvm::toString(
                  B->mapRegion(Alias, SharedData, 0, PageSize, Read | Write)),
              "");
  }
  llvm::Error program(llvm::ArrayRef<uint8_t> X64,
                      llvm::ArrayRef<uint32_t> ARM) {
    return A->write(Code, instructionBytes(GetParam().ISA, X64, ARM));
  }

  CPURegister resultRegister() const {
    return GetParam().ISA == GuestArchitecture::X64 ? CPURegister::X64AX
                                                    : CPURegister::AArch64X0;
  }
  llvm::Error dataRegister(ExecutionBackend &CPU, uint64_t Address) {
    return CPU.writeRegister(GetParam().ISA == GuestArchitecture::X64
                                 ? CPURegister::X64CX
                                 : CPURegister::AArch64X1,
                             {Address, 0});
  }
  llvm::Error steps(ExecutionBackend &CPU, unsigned Count) {
    unsigned Seen = 0;
    BackendHooks H;
    H.Instruction = [&](uint64_t, uint32_t) {
      if (Seen++ == Count)
        CPU.stop();
    };
    if (auto E = CPU.installHooks(std::move(H)))
      return E;
    auto E = CPU.run(Code, Timeout);
    llvm::consumeError(CPU.installHooks({}));
    return E;
  }
};
TEST_P(SharedCPU, CPUsShareBytesAndSurviveDestructionOfTheirPeer) {
  ASSERT_EQ(llvm::toString(program(IncrementX64, IncrementARM)), "");
  ASSERT_EQ(llvm::toString(dataRegister(*First, Data)), "");
  ASSERT_EQ(llvm::toString(dataRegister(*Second, Alias)), "");
  ASSERT_EQ(llvm::toString(A->writeInteger(Data, Value, sizeof(uint64_t))), "");
  ASSERT_EQ(llvm::toString(steps(*First, 3)), "");
  EXPECT_EQ((*First->readRegister(resultRegister()))[0], Value + 1);
  EXPECT_EQ((*Second->readRegister(resultRegister()))[0], 0u);
  ASSERT_EQ(llvm::toString(steps(*Second, 3)), "");
  EXPECT_EQ(*A->readInteger(Data, sizeof(uint64_t)), Value + 2);
  First.reset();
  A.reset();
  ASSERT_EQ(llvm::toString(steps(*Second, 3)), "");
  EXPECT_EQ(*B->readInteger(Alias, sizeof(uint64_t)), Value + 3);
}
TEST_P(SharedCPU, SharedCodeWritesInvalidateWarmTranslationsAndSavedContexts) {
  ASSERT_EQ(llvm::toString(program(FirstX64, FirstARM)), "");
  auto Context = First->saveContext();
  ASSERT_TRUE(bool(Context));
  ASSERT_EQ(llvm::toString(steps(*First, 1)), "");
  ASSERT_EQ(llvm::toString(steps(*Second, 1)), "");
  EXPECT_EQ((*First->readRegister(resultRegister()))[0], Value);
  ASSERT_EQ(llvm::toString(program(SecondX64, SecondARM)), "");
  ASSERT_EQ(llvm::toString(First->restoreContext(**Context)), "");
  ASSERT_EQ(llvm::toString(steps(*First, 1)), "");
  ASSERT_EQ(llvm::toString(steps(*Second, 1)), "");
  EXPECT_EQ((*First->readRegister(resultRegister()))[0], Updated);
  EXPECT_EQ((*Second->readRegister(resultRegister()))[0], Updated);
}
TEST_P(SharedCPU, RebindingUsesSpaceIdentityEvenWithMatchingGenerations) {
  ASSERT_EQ(llvm::toString(program(IncrementX64, IncrementARM)), "");
  ASSERT_EQ(llvm::toString(B->unmap(Alias, PageSize)), "");
  ASSERT_EQ(llvm::toString(B->map(Data, PageSize, Read | Write)), "");
  // Equal generations with different translations must not skip
  // synchronization.
  ASSERT_EQ(llvm::toString(A->protect(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->protect(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(A->mappingGeneration(), B->mappingGeneration());
  ASSERT_EQ(llvm::toString(A->writeInteger(Data, Value, sizeof(uint64_t))), "");
  ASSERT_EQ(llvm::toString(B->writeInteger(Data, Updated, sizeof(uint64_t))),
            "");
  ASSERT_EQ(llvm::toString(dataRegister(*First, Data)), "");
  auto Context = First->saveContext();
  ASSERT_TRUE(bool(Context));
  ASSERT_EQ(llvm::toString(steps(*First, 3)), "");
  ASSERT_EQ(llvm::toString(First->bindAddressSpace(B)), "");
  EXPECT_NE(llvm::toString(First->restoreContext(**Context)), "");
  ASSERT_EQ(llvm::toString(steps(*First, 3)), "");
  EXPECT_EQ((*First->readRegister(resultRegister()))[0], Updated + 1);
  EXPECT_EQ(*A->readInteger(Data, sizeof(uint64_t)), Value + 1);
  ASSERT_EQ(llvm::toString(First->bindAddressSpace(A)), "");
  ASSERT_EQ(llvm::toString(First->restoreContext(**Context)), "");
  ASSERT_EQ(llvm::toString(steps(*First, 3)), "");
  EXPECT_EQ((*First->readRegister(resultRegister()))[0], Value + 2);
  EXPECT_NE(llvm::toString(First->bindAddressSpace(space(ram()))), "");
  EXPECT_EQ(First->addressSpace(), A);
}
TEST_P(SharedCPU, OwnerLeaseRejectsMutationAndRecursiveExecutionAcrossSpaces) {
  ASSERT_EQ(llvm::toString(program(FirstX64, FirstARM)), "");
  auto UnmappedRegion = llvm::cantFail(RAM->allocate(PageSize));
  auto View = llvm::cantFail(First->pinBacking(Data, sizeof(uint64_t)));
  BackendHooks H;
  H.Instruction = [&](uint64_t, uint32_t) {
    std::array<uint8_t, sizeof(uint64_t)> Bytes{};
    EXPECT_NE(llvm::toString(View.write(0, Bytes)), "");
    EXPECT_NE(llvm::toString(View.validateAccess(0, Bytes.size())), "");
    EXPECT_NE(llvm::toString(A->validatePinned(View, 0, Bytes.size())), "");
    EXPECT_NE(llvm::toString(First->validatePinned(View, 0, Bytes.size())), "");
    EXPECT_NE(llvm::toString(First->readPinned(View, 0, Bytes)), "");
    auto Observation = A->pinBacking(Data, sizeof(uint64_t));
    ASSERT_TRUE(bool(Observation));
    EXPECT_TRUE(Observation->describesSameBytes(View));
    EXPECT_TRUE(*A->canAccess(Code, PageSize, Execute));
    EXPECT_TRUE(*First->canAccess(Code, PageSize, Execute));
    EXPECT_NE(llvm::toString(B->writeInteger(Alias, Updated, sizeof(uint64_t))),
              "");
    EXPECT_NE(llvm::toString(B->protect(Alias, PageSize, Read)), "");
    EXPECT_NE(llvm::toString(B->unmap(Alias, PageSize)), "");
    EXPECT_NE(llvm::toString(Second->run(Code, Timeout)), "");
    std::thread Concurrent([&] {
      UnmappedRegion.reset();
      EXPECT_EQ(RAM->allocatedBytes(), 2 * PageSize);
      EXPECT_EQ(A->mappedBytes(), 2 * PageSize);
      EXPECT_NE(
          llvm::toString(B->writeInteger(Alias, Updated, sizeof(uint64_t))),
          "");
      EXPECT_NE(llvm::toString(Second->run(Code, Timeout)), "");
      auto Query = B->canAccess(Alias, PageSize, Read);
      ASSERT_FALSE(bool(Query));
      llvm::consumeError(Query.takeError());
      auto Pin = B->pinBacking(Alias, sizeof(uint64_t));
      ASSERT_FALSE(bool(Pin));
      llvm::consumeError(Pin.takeError());
    });
    Concurrent.join();
    First->stop();
  };
  ASSERT_EQ(llvm::toString(First->installHooks(std::move(H))), "");
  ASSERT_EQ(llvm::toString(First->run(Code, Timeout)), "");
  EXPECT_EQ(*A->readInteger(Data, sizeof(uint64_t)), 0u);
  EXPECT_FALSE(First->fault());
  EXPECT_FALSE(Second->fault());
}
TEST_P(SharedCPU,
       CPUViewAccessKeepsAllocationIdentityAndRespectsTerminalFaults) {
  ASSERT_EQ(llvm::toString(program(IncrementX64, IncrementARM)), "");
  auto View = llvm::cantFail(First->pinBacking(Data, sizeof(uint64_t)));
  ASSERT_EQ(llvm::toString(First->bindAddressSpace(B)), "");
  std::array<uint8_t, sizeof(uint64_t)> Bytes{};
  llvm::support::endian::write64le(Bytes.data(), Updated);
  ASSERT_EQ(llvm::toString(First->writePinned(View, 0, Bytes)), "");
  EXPECT_EQ(*B->readInteger(Alias, sizeof(uint64_t)), Updated);
  ASSERT_EQ(llvm::toString(dataRegister(*First, Alias)), "");
  ASSERT_EQ(llvm::toString(steps(*First, 3)), "");
  EXPECT_EQ(*B->readInteger(Alias, sizeof(uint64_t)), Updated + 1);
  EXPECT_NE(llvm::toString(First->read(Data, Bytes)), "");
  ASSERT_TRUE(First->fault());
  EXPECT_NE(llvm::toString(First->writePinned(View, 0, Bytes)), "");
  EXPECT_NE(llvm::toString(First->validatePinned(View, 0, Bytes.size())), "");
  EXPECT_NE(llvm::toString(First->readPinned(View, 0, Bytes)), "");
  // Fault state belongs to that CPU; it cannot poison shared RAM or its peer.
  ASSERT_EQ(llvm::toString(Second->readPinned(View, 0, Bytes)), "");
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), Updated + 1);
}
TEST_P(SharedCPU, StaleProjectionsPinRAMUntilTheyAreRetired) {
  ASSERT_EQ(llvm::toString(program(FirstX64, FirstARM)), "");
  ASSERT_EQ(llvm::toString(steps(*First, 1)), "");
  ASSERT_EQ(llvm::toString(steps(*Second, 1)), "");
  ASSERT_EQ(llvm::toString(A->unmap(Data, PageSize)), "");
  ASSERT_EQ(llvm::toString(B->unmap(Alias, PageSize)), "");
  EXPECT_EQ(RAM->allocatedBytes(), 2 * PageSize);
  ASSERT_EQ(llvm::toString(steps(*First, 1)), "");
  EXPECT_EQ(RAM->allocatedBytes(), 2 * PageSize);
  Second.reset();
  EXPECT_EQ(RAM->allocatedBytes(), PageSize);
  ASSERT_EQ(llvm::toString(A->map(Data, PageSize, Read | Write)), "");
  EXPECT_EQ(*A->readInteger(Data, sizeof(uint64_t)), 0u);
}
TEST_P(SharedCPU, ChecksExternalMappingsAtAttachmentAndBeforeExecution) {
  if (GetParam().Contract == ExecutionContract::Software)
    GTEST_SKIP();
  const uint64_t Invalid =
      GetParam().ISA == GuestArchitecture::AArch64 ? ArmGateway : NonCanonical;
  auto Bad = space(RAM);
  ASSERT_EQ(llvm::toString(Bad->map(Invalid, PageSize, Read | Execute)), "");
  auto Attach = createExecutionBackend(GetParam().Backend, GetParam().Contract,
                                       Bad, GetParam().ISA);
  EXPECT_FALSE(bool(Attach));
  llvm::consumeError(Attach.takeError());
  EXPECT_NE(llvm::toString(First->bindAddressSpace(Bad)), "");
  ASSERT_EQ(llvm::toString(program(FirstX64, FirstARM)), "");
  ASSERT_EQ(llvm::toString(steps(*First, 1)), "");
  EXPECT_EQ((*First->readRegister(resultRegister()))[0], Value);
  ASSERT_EQ(
      llvm::toString(First->writeRegister(resultRegister(), {Updated, 0})), "");
  ASSERT_EQ(llvm::toString(A->map(Invalid, PageSize, Read | Execute)), "");
  // A peer can add an inadmissible mapping after a successful run. Repeated
  // failures on that unchanged mapping must never authorize execution.
  for (unsigned Attempt = 0; Attempt < 2; ++Attempt) {
    EXPECT_NE(llvm::toString(First->run(Code, Timeout)), "");
    EXPECT_EQ((*First->readRegister(resultRegister()))[0], Updated);
  }
  ASSERT_EQ(llvm::toString(A->unmap(Invalid, PageSize)), "");
  ASSERT_EQ(llvm::toString(steps(*First, 1)), "");
  EXPECT_EQ((*First->readRegister(resultRegister()))[0], Value);
  // Bind a valid space one epoch behind A, then let a peer add an invalid
  // page. Its epoch now matches A's successful run, but its mappings do not.
  ASSERT_EQ(llvm::toString(B->protect(Alias, PageSize, Read | Write)), "");
  ASSERT_EQ(B->mappingGeneration() + 1, A->mappingGeneration());
  ASSERT_EQ(llvm::toString(First->bindAddressSpace(B)), "");
  ASSERT_EQ(llvm::toString(B->map(Invalid, PageSize, Read | Execute)), "");
  ASSERT_EQ(B->mappingGeneration(), A->mappingGeneration());
  ASSERT_EQ(
      llvm::toString(First->writeRegister(resultRegister(), {Updated, 0})), "");
  EXPECT_NE(llvm::toString(First->run(Code, Timeout)), "");
  EXPECT_EQ((*First->readRegister(resultRegister()))[0], Updated);
  ASSERT_EQ(llvm::toString(B->unmap(Invalid, PageSize)), "");
  ASSERT_EQ(llvm::toString(steps(*First, 1)), "");
  EXPECT_EQ((*First->readRegister(resultRegister()))[0], Value);
}
constexpr auto Portable = ExecutionBackendKind::Unicorn;
const CPUProfile Profiles[] = {
#define NEVERD_MEMORY_TEST_PROFILE(Name, Backend, Contract, ISA)               \
  {#Name, Backend, ExecutionContract::Contract, GuestArchitecture::ISA},
#include "MemoryLifecycleCases.def"
#undef NEVERD_MEMORY_TEST_PROFILE
};
INSTANTIATE_TEST_SUITE_P(PortableAndNative, SharedCPU,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<CPUProfile> &Info) {
                           return Info.param.Name;
                         });

} // namespace
} // namespace neverd::emulation
