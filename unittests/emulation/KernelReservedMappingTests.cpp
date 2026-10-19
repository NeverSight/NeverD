//===- KernelReservedMappingTests.cpp - Reserved system views ------------===//
// NeverD Decompiler
///
/// \file
/// Reserved system views.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
class KernelReservedMapping : public testing::Test {
protected:
  static constexpr uint64_t Tag = 0x53524552;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  template <class T> T take(llvm::Expected<T> V) {
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return {};
    }
    return std::move(*V);
  }
  void initialize(bool Paging = false) {
    Model.reset();
    Memory = take(UnicornBackend::create(16 * 1024 * 1024));
    Result = {};
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.PageTables = Paging;
    Model = std::make_unique<KernelModel>(*Memory, Result);
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void SetUp() override { initialize(); }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A) {
    auto R = Model->call(Name, A);
    if (!R) {
      ADD_FAILURE() << Name << ": " << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  void error(const char *Name, std::initializer_list<uint64_t> A,
             const char *Text) {
    auto R = Model->call(Name, A);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find(Text), std::string::npos);
  }
  uint64_t get(uint64_t A, unsigned Size = 8) {
    return take(Memory->readInteger(A, Size));
  }
  uint64_t pool() { return call("ExAllocatePoolWithTag", {0, 12288, Tag}); }
  uint64_t lock(uint64_t P, uint64_t Bytes) {
    auto MDL = call("IoAllocateMdl", {P, Bytes, 0, 0, 0});
    call("MmProbeAndLockPages", {MDL, 0, 1});
    return MDL;
  }
  void release(uint64_t MDL, uint64_t Pool) {
    call("MmUnlockPages", {MDL});
    call("IoFreeMdl", {MDL});
    call("ExFreePoolWithTag", {Pool, Tag});
  }
};

TEST_F(KernelReservedMapping,
       ReservationOccupiesNoRAMAndExcludesOrdinaryAliases) {
  for (bool Paging : {false, true}) {
    initialize(Paging);
    const auto R = call("MmAllocateMappingAddress", {8192, Tag});
    ASSERT_NE(R, 0u);
    EXPECT_EQ(R % 4096, 0u);
    EXPECT_FALSE(take(Memory->canAccess(R, 1, Read)));
    const auto P = pool(), MDL = lock(P, 4096);
    const auto Ordinary =
        call("MmMapLockedPagesSpecifyCache", {MDL, 0, 1, 0, 0, 16});
    EXPECT_GE(Ordinary, R + 8192);
    call("MmUnmapLockedPages", {Ordinary, MDL});
    call("MmFreeMappingAddress", {R, Tag});
    const auto Reused = call("MmAllocateMappingAddress", {4096, Tag});
    EXPECT_EQ(Reused, R);
    call("MmFreeMappingAddress", {Reused, Tag});
    release(MDL, P);
  }
}

TEST_F(KernelReservedMapping, OffsetAndUnmapMetadataMatchNative19041) {
  struct NativeCase {
    uint16_t InitialFlags;
    uint32_t Offset, Length;
    bool Mapped;
    uint16_t MappedFlags, UnmappedFlags;
  };
  constexpr NativeCase Cases[] = {
#define NEVERD_RESERVED_MAPPING_CASE(Initial, Offset, Length, Mapped, Flags,   \
                                     After)                                    \
  {Initial, Offset, Length, Mapped, Flags, After},
#include "ReservedMappingCases.def"
#undef NEVERD_RESERVED_MAPPING_CASE
  };
  for (bool Paging : {false, true})
    for (const auto &Case : Cases) {
      const auto Offset = Case.Offset, Length = Case.Length;
      SCOPED_TRACE(std::to_string(Paging) + ":" + std::to_string(Offset) + ":" +
                   std::to_string(Length));
      initialize(Paging);
      const auto R = call("MmAllocateMappingAddress", {8192, Tag});
      const auto P = pool();
      const bool Partial = Case.InitialFlags == MDLPartial;
      const auto Root = Partial ? lock(P, 12288) : 0;
      const auto MDL =
          Partial ? call("IoAllocateMdl", {P + Offset, Length, 0, 0, 0})
                  : lock(P + Offset, Length);
      if (Partial)
        call("IoBuildPartialMdl", {Root, MDL, P + Offset, Length});
      const auto Alias =
          call("MmMapLockedPagesWithReservedMapping", {R, Tag, MDL, 1});
      if (!Case.Mapped) {
        EXPECT_EQ(Alias, 0u);
        EXPECT_EQ(get(MDL + MDLMappedSystemVAOffset), 0u);
        EXPECT_EQ(get(MDL + MDLFlagsOffset, 2), Case.InitialFlags);
      } else {
        ASSERT_EQ(Alias, R + Offset);
        EXPECT_EQ(get(MDL + MDLMappedSystemVAOffset), Alias);
        EXPECT_EQ(get(MDL + MDLFlagsOffset, 2), Case.MappedFlags);
        EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {MDL, 16}), Alias);
        EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Execute)));
        ok(Memory->writeInteger(Alias, 0x53, 1));
        EXPECT_EQ(get(P + Offset, 1), 0x53u);
        call("MmUnmapReservedMapping", {Alias, Tag, MDL});
        EXPECT_EQ(get(MDL + MDLMappedSystemVAOffset), Alias);
        EXPECT_EQ(get(MDL + MDLFlagsOffset, 2), Case.UnmappedFlags);
        EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
      }
      if (Partial) {
        call("IoFreeMdl", {MDL});
        release(Root, P);
      } else
        release(MDL, P);
      call("MmFreeMappingAddress", {R, Tag});
    }
}

TEST_F(KernelReservedMapping, TagsAndWrongReleasePathsPreserveLiveMapping) {
  const auto R = call("MmAllocateMappingAddress", {4096, Tag});
  const auto P = pool(), MDL = lock(P, 100);
  error("MmMapLockedPagesWithReservedMapping", {R, Tag + 1, MDL, 1},
        "base and tag");
  const auto Alias =
      call("MmMapLockedPagesWithReservedMapping", {R, Tag, MDL, 1});
  error("MmFreeMappingAddress", {R, Tag}, "unmapped before free");
  error("MmFreeMappingAddress", {R, Tag + 1}, "base and tag");
  error("MmMapLockedPagesWithReservedMapping", {R, Tag, MDL, 1},
        "live mapping");
  error("MmUnmapLockedPages", {Alias, MDL}, "MmUnmapReservedMapping");
  error("MmUnlockPages", {MDL}, "MmUnmapReservedMapping");
  error("IoFreeMdl", {MDL}, "MmUnmapReservedMapping");
  error("MmUnmapReservedMapping", {Alias, Tag + 1, MDL}, "does not match");
  ok(Memory->writeInteger(Alias, 0x55, 1));
  EXPECT_EQ(get(P, 1), 0x55u);
  call("MmUnmapReservedMapping", {Alias, Tag, MDL});
  EXPECT_EQ(call("MmMapLockedPagesWithReservedMapping", {R, Tag, MDL, 1}),
            Alias);
  call("MmUnmapReservedMapping", {R, Tag, MDL});
  release(MDL, P);
  call("MmFreeMappingAddress", {R, Tag});
  error("MmFreeMappingAddress", {R, Tag}, "base and tag");
}

TEST_F(KernelReservedMapping,
       IndependentPagesAndLockedPartialsRetainDependencies) {
  const auto R = call("MmAllocateMappingAddress", {8192, Tag});
  const auto Pages = call("MmAllocatePagesForMdl", {0, UINT64_MAX, 0, 8192});
  ASSERT_NE(Pages, 0u);
  ASSERT_EQ(call("MmMapLockedPagesWithReservedMapping", {R, Tag, Pages, 1}), R);
  error("MmFreePagesFromMdl", {Pages}, "MmUnmapReservedMapping");
  call("MmUnmapReservedMapping", {R, Tag, Pages});
  call("MmFreePagesFromMdl", {Pages});
  call("ExFreePool", {Pages});
  const auto P = pool(), Root = lock(P, 8192);
  const auto Partial = call("IoAllocateMdl", {P + 7, 100, 0, 0, 0});
  call("IoBuildPartialMdl", {Root, Partial, P + 7, 100});
  ASSERT_EQ(call("MmMapLockedPagesWithReservedMapping", {R, Tag, Partial, 1}),
            R + 7);
  error("MmUnlockPages", {Root}, "dependent partial");
  error("IoFreeMdl", {Partial}, "MmUnmapReservedMapping");
  call("MmUnmapReservedMapping", {R + 7, Tag, Partial});
  call("IoFreeMdl", {Partial});
  release(Root, P);
  call("MmFreeMappingAddress", {R, Tag});
}

TEST_F(KernelReservedMapping, MetadataFaultDoesNotPublishOrDropAnAlias) {
  const auto R = call("MmAllocateMappingAddress", {4096, Tag});
  const auto P = pool(), MDL = lock(P, 100);
  const auto MetadataPage = MDL & ~uint64_t(4095);
  ok(Memory->protect(MetadataPage, 4096, Read));
  error("MmMapLockedPagesWithReservedMapping", {R, Tag, MDL, 1},
        "writable MDL");
  EXPECT_FALSE(take(Memory->canAccess(R, 1, Read)));
  ok(Memory->protect(MetadataPage, 4096, Read | Write));
  ASSERT_EQ(call("MmMapLockedPagesWithReservedMapping", {R, Tag, MDL, 1}), R);
  ok(Memory->protect(MetadataPage, 4096, Read));
  error("MmUnmapReservedMapping", {R, Tag, MDL}, "writable MDL");
  EXPECT_TRUE(take(Memory->canAccess(R, 1, Read)));
  EXPECT_EQ(get(MDL + MDLFlagsOffset, 2), MDLPagesLocked | MDLMappedToSystemVA);
  ok(Memory->protect(MetadataPage, 4096, Read | Write));
  call("MmUnmapReservedMapping", {R, Tag, MDL});
  release(MDL, P);
  call("MmFreeMappingAddress", {R, Tag});
}

TEST_F(KernelReservedMapping, AllocationBoundsAndIRQLHaveNoPartialEffects) {
  EXPECT_EQ(call("MmAllocateMappingAddress", {4096, 0}), 0u);
  EXPECT_EQ(call("MmAllocateMappingAddress", {UINT64_MAX, Tag}), 0u);
  error("MmAllocateMappingAddress", {0, Tag}, "nonzero size");
  const auto R =
      call("MmAllocateMappingAddress", {1, UINT64_C(0xabcdef0053524552)});
  const auto P = pool(), MDL = lock(P, 1);
  call("KfRaiseIrql", {2});
  error("MmAllocateMappingAddress", {4096, Tag}, "IRQL");
  error("MmFreeMappingAddress", {R, Tag}, "IRQL");
  EXPECT_EQ(call("MmMapLockedPagesWithReservedMapping", {R, Tag, MDL, 1}), R);
  call("MmUnmapReservedMapping", {R, Tag, MDL});
  call("KeLowerIrql", {0});
  release(MDL, P);
  call("MmFreeMappingAddress", {R, Tag});
}

TEST(KernelReservedMappingDriver,
     CompiledMappingsAndLeakedReservationHaveRealLifetimes) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Leak : {false, true}) {
        SCOPED_TRACE(std::to_string(unsigned(Contract)) + ":" +
                     std::to_string(Preemptive) + ":" + std::to_string(Leak));
        DriverOptions O;
        O.Contract = Contract;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.PageTables = true;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        const auto Path = std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                          (Leak ? "driver_reserved_mapping_leak.sys"
                                : "driver_reserved_mapping.sys");
        auto R = emulateDriver(Path, O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        EXPECT_EQ(R->Stop, Leak ? DriverStopReason::ModelError
                                : DriverStopReason::Returned)
            << R->Diagnostic;
        EXPECT_EQ(R->UnloadCompleted, !Leak) << R->Diagnostic;
        if (Leak)
          EXPECT_NE(R->Diagnostic.find("live mapping reservations"),
                    std::string::npos);
      }
}
} // namespace
} // namespace neverd::emulation
