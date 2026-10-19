//===- KernelSectionTests.cpp - Physical section handles and shared RAM
//----===//
// NeverD Decompiler
///
/// \file
/// Physical section handles and shared RAM.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/driver/DriverSectionsJSON.h"
#include "os/windows/kernel/KernelException.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation {
namespace {
class KernelSection : public ::testing::Test {
protected:
  static constexpr uint64_t S = 0x22000000, PC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  void
  initialize(uint64_t Budget = 8 * 1024 * 1024,
             std::optional<uint32_t> ClockIncrement = std::nullopt,
             std::optional<DriverSectionNamespace> Sections = std::nullopt) {
    Model.reset();
    auto M = UnicornBackend::create(Budget);
    ASSERT_TRUE(bool(M));
    Memory = std::move(*M);
    ok(Memory->map(S, 0x10000, Read | Write));
    ok(Memory->map(PC, 0x1000, Read | Execute));
    Result = DriverResult{};
    DriverOptions O;
    O.MemoryLimit = Budget;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.PageTables = true;
    O.SystemClockIncrement100ns = ClockIncrement;
    O.SectionNamespace = std::move(Sections);
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = PC;
    I.Size = 0x3000;
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void SetUp() override { initialize(); }
  void put(uint64_t A, uint64_t V, unsigned W = 8) {
    ok(Memory->writeInteger(A, V, W));
  }
  uint64_t get(uint64_t A, unsigned W = 8) {
    auto V = Memory->readInteger(A, W);
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return 0;
    }
    return *V;
  }
  uint64_t call(llvm::StringRef Name,
                std::initializer_list<uint64_t> Args = {}) {
    auto V = Model->call(Name.str(), Args);
    if (!V) {
      ADD_FAILURE() << Name.str() << ": " << llvm::toString(V.takeError());
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
  void name(llvm::StringRef Text = "\\device\\physicalmemory",
            uint32_t Flags = 0x40) {
    for (size_t I = 0; I <= Text.size(); ++I)
      put(S + 0x200 + I * 2, I == Text.size() ? 0 : uint8_t(Text[I]), 2);
    put(S + 0x100, Text.size() * 2, 2);
    put(S + 0x102, (Text.size() + 1) * 2, 2);
    put(S + 0x108, S + 0x200);
    put(S + 0x300, 48, 4);
    put(S + 0x308, 0);
    put(S + 0x310, S + 0x100);
    put(S + 0x318, Flags, 4);
    put(S + 0x320, 0);
    put(S + 0x328, 0);
  }
  uint64_t open(uint32_t Access = 0xf001f, uint32_t Flags = 0x40) {
    name("\\device\\physicalmemory", Flags);
    EXPECT_EQ(call("ZwOpenSection", {S, Access, S + 0x300}), 0u);
    return get(S);
  }
  uint64_t pool(uint64_t Size = 0x2000) {
    return call("ExAllocatePoolWithTag", {0, Size, 0x54455354});
  }
  uint64_t phys(uint64_t P) { return call("MmGetPhysicalAddress", {P}); }
  uint64_t map(uint64_t H, uint64_t Offset, uint64_t Size = 0x1000,
               uint32_t Protect = 4, uint32_t Inherit = 2, uint64_t Base = 0,
               uint32_t Allocation = 0) {
    put(S + 0x400, Base);
    put(S + 0x408, Size);
    put(S + 0x410, Offset);
    return call("ZwMapViewOfSection",
                {H, UINT64_MAX, S + 0x400, 0, 0, S + 0x410, S + 0x408, Inherit,
                 Allocation, Protect});
  }
  void unmap(uint64_t V) {
    EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, V}), 0u);
  }
  void free(uint64_t P) {
    EXPECT_EQ(call("ExFreePoolWithTag", {P, 0x54455354}), 0u);
  }
};
TEST_F(KernelSection, ContiguousAllocationSharesPhysicalPagesAndViewPins) {
  // Synchronizing paging publishes the reserved arena's unowned frames first.
  ASSERT_NE(phys(S), 0u);
  const auto P = call("MmAllocateContiguousMemory", {8193, UINT64_MAX});
  ASSERT_NE(P, 0u);
  EXPECT_EQ(P % 4096, 0u);
  const auto PA = phys(P);
  EXPECT_EQ(phys(P + 4096), PA + 4096);
  EXPECT_EQ(phys(P + 8192), PA + 8192);
  EXPECT_TRUE(llvm::cantFail(Memory->canAccess(P, 12288, Execute)));
  put(P + 4096, 0x1122334455667788);
  const auto H = open();
  ASSERT_EQ(map(H, PA, 12288), 0u);
  const auto V = get(S + 0x400);
  EXPECT_EQ(get(V + 4096), 0x1122334455667788u);
  put(V + 8192, 0x8877665544332211);
  EXPECT_EQ(get(P + 8192), UINT64_C(0x8877665544332211));
  auto FreePinned = Model->call("MmFreeContiguousMemory", {P});
  ASSERT_FALSE(bool(FreePinned));
  llvm::consumeError(FreePinned.takeError());
  error("ExFreePoolWithTag", {P, 0}, "MmFreeContiguousMemory");
  error("MmFreeContiguousMemory", {P + 4096}, "exact live allocation base");
  unmap(V);
  call("ZwClose", {H});
  EXPECT_EQ(call("MmFreeContiguousMemory", {P}), 0u);
  EXPECT_FALSE(llvm::cantFail(Memory->canAccess(P, 1, Read)));
  error("MmFreeContiguousMemory", {P}, "exact live allocation base");
}

TEST_F(KernelSection,
       ContiguousHighestAddressUsesInclusivePFNAndFiniteCapacity) {
  EXPECT_EQ(
      call("MmAllocateContiguousMemory", {4096, physical::PhysicalBase - 1}),
      0u);
  const auto P =
      call("MmAllocateContiguousMemory", {1, physical::PhysicalBase + 1});
  ASSERT_NE(P, 0u);
  EXPECT_EQ(phys(P), physical::PhysicalBase);
  EXPECT_EQ(
      call("MmAllocateContiguousMemory", {8192, physical::PhysicalBase + 1}),
      0u);
  EXPECT_EQ(call("MmAllocateContiguousMemory", {UINT64_MAX, UINT64_MAX}), 0u);
  error("MmAllocateContiguousMemory", {0, UINT64_MAX}, "nonzero byte count");
  const auto Other = pool();
  error("MmFreeContiguousMemory", {Other}, "exact live allocation base");
  free(Other);
  EXPECT_EQ(call("MmFreeContiguousMemory", {P}), 0u);
}

TEST_F(KernelSection, ContiguousAllocationWorksWithNonpagedAndLockedMDLs) {
  const auto P = call("MmAllocateContiguousMemory", {8192, UINT64_MAX});
  ASSERT_NE(P, 0u);
  auto MDL = call("IoAllocateMdl", {P, 8192, 0, 0, 0});
  ASSERT_NE(MDL, 0u);
  call("MmBuildMdlForNonPagedPool", {MDL});
  EXPECT_EQ(get(MDL + windows::MDLSize) * 4096, phys(P));
  EXPECT_EQ(get(MDL + windows::MDLSize + 8) * 4096, phys(P + 4096));
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {MDL, 16}), P);
  call("IoFreeMdl", {MDL});
  MDL = call("IoAllocateMdl", {P, 8192, 0, 0, 0});
  call("MmProbeAndLockPages", {MDL, 0, 1});
  auto E = Model->call("MmFreeContiguousMemory", {P});
  ASSERT_FALSE(bool(E));
  llvm::consumeError(E.takeError());
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
  EXPECT_EQ(call("MmFreeContiguousMemory", {P}), 0u);
}

TEST_F(KernelSection, IndependentPageCacheSurvivesPriorPagingPublication) {
  ASSERT_NE(phys(S), 0u);
  auto Allocated = Model->call("MmAllocatePagesForMdlEx",
                               {0, UINT64_MAX, 0, 8192, windows::MmNonCached,
                                windows::MmAllocateFullyRequired});
  ASSERT_TRUE(bool(Allocated)) << llvm::toString(Allocated.takeError());
  const auto MDL = *Allocated;
  ASSERT_NE(MDL, 0u);
  const auto Alias = call("MmGetSystemAddressForMdlSafe", {MDL, 16});
  ASSERT_NE(Alias, 0u);
  const auto PA = phys(Alias), H = open();
  put(S + 0x400, 0);
  put(S + 0x408, 8192);
  put(S + 0x410, PA);
  error("ZwMapViewOfSection",
        {H, UINT64_MAX, S + 0x400, 0, 0, S + 0x410, S + 0x408, 2, 0, 4},
        "cache conflicts");
  ASSERT_EQ(map(H, PA, 8192, 4 | 0x200), 0u);
  const auto View = get(S + 0x400);
  put(View + 4096, 0x1122334455667788);
  EXPECT_EQ(get(Alias + 4096), UINT64_C(0x1122334455667788));
  auto Pinned = Model->call("MmFreePagesFromMdl", {MDL});
  ASSERT_FALSE(bool(Pinned));
  llvm::consumeError(Pinned.takeError());
  unmap(View);
  call("ZwClose", {H});
  call("MmFreePagesFromMdl", {MDL});
  call("ExFreePool", {MDL});
}

TEST_F(KernelSection, VirtualQueryUsesMappedViewIdentityAndRetirement) {
  const auto P = pool(), H = open();
  ASSERT_EQ(map(H, phys(P), 8192), 0u);
  const auto V = get(S + 0x400);
  EXPECT_EQ(call("ZwQueryVirtualMemory",
                 {UINT64_MAX, V + 4096 + 3, 0, S + 0x500, 48, S + 0x600}),
            0u);
  EXPECT_EQ(get(S + 0x500), V + 4096);
  EXPECT_EQ(get(S + 0x508), V);
  EXPECT_EQ(get(S + 0x510, 4), 4u);
  EXPECT_EQ(get(S + 0x518), 4096u);
  EXPECT_EQ(get(S + 0x520, 4), 0x1000u);
  EXPECT_EQ(get(S + 0x528, 4), 0x40000u);
  EXPECT_EQ(get(S + 0x600), 48u);
  EXPECT_EQ(call("ZwQueryVirtualMemory", {H, V, 0, S + 0x500, 48, 0}),
            0xc0000024u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(call("ZwQueryVirtualMemory", {UINT64_MAX, V, 0, S + 0x500, 48, 0}),
            0u);
  unmap(V);
  error("ZwQueryVirtualMemory", {UINT64_MAX, V, 0, S + 0x500, 48, 0},
        "no modeled virtual allocation");
  free(P);
}
TEST(DriverSectionNamespace, StrictParserAndNativeOptionsShareValidation) {
  for (
      auto Text :
      {R"({"directories":[],"sections":[],"extra":0})",
       R"({"directories":[],"sections":[{"name":"\\X\\Y","size":4096}]})",
       R"({"directories":["\\X","\\x"],"sections":[]})",
       R"({"directories":[""],"sections":[]})",
       R"({"directories":["\\Device"],"sections":[]})",
       R"({"directories":["\\X"],"sections":[{"name":"\\x\\Y","size":4096}]})",
       R"({"directories":["\\X"],"sections":[],"aliases":{"\\X\\A":"\\X\\A"}})",
       R"({"directories":["\\X"],"sections":[{"name":"\\X\\Y","size":0}]})",
       R"({"directories":["\\X"],"sections":[{"name":"\\X\\Y","size":1,"bytes_hex":"0011"}]})",
       R"({"directories":["\\X"],"sections":[{"name":"\\X\\Y","size":4096,"protection":8}]})",
       R"({"directories":["\\X"],"sections":[{"name":"\\X\\Y","size":4096,"unknown":1}]})",
       R"({"directories":["\\X"],"sections":[{"name":"\\X\\Y","size":4096,"bytes_hex":"gg"}]})"}) {
    auto V = llvm::json::parse(Text);
    ASSERT_TRUE(bool(V));
    auto N = parseDriverSectionNamespace(*V);
    ASSERT_FALSE(bool(N)) << Text;
    llvm::consumeError(N.takeError());
  }
  DriverSectionNamespace N{{"\\X"}, {{"\\X\\Y", 4096, {}, 4}}};
  EXPECT_FALSE(bool(validateDriverSectionNamespace(N)));
  N.Sections.push_back({"\\x\\y", 4096, {}, 4});
  auto E = validateDriverSectionNamespace(N);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST_F(KernelSection, NamedSharedViewsRetainBytesIdentityAndSurviveClose) {
  initialize(8 * 1024 * 1024, std::nullopt,
             DriverSectionNamespace{
                 {"\\BaseNamedObjects"},
                 {{"\\BaseNamedObjects\\Shared", 0x11000, {0x12, 0x34}, 4}}});
  name("\\basenamedobjects\\shared");
  ASSERT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0u);
  const auto H = get(S);
  ASSERT_EQ(map(H, 0, 0), 0u);
  const auto A = get(S + 0x400);
  EXPECT_EQ(get(S + 0x408), 0x11000u);
  EXPECT_EQ(get(A, 2), 0x3412u);
  EXPECT_EQ(get(A + 0x10000), 0u);
  ASSERT_EQ(map(H, 0, 0x1000, 2), 0u);
  const auto B = get(S + 0x400);
  put(A, 0x7654, 2);
  EXPECT_EQ(get(B, 2), 0x7654u);
  ok(Model->validateGuestAccess(A, 2, true));
  ok(Model->validateGuestAccess(B, 2, false));
  EXPECT_EQ(call("ZwQueryObject", {H, 1, S + 0x600, 512, S + 0x500}), 0u);
  EXPECT_EQ(get(S + 0x600, 2), 24u * 2);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(get(B, 2), 0x7654u);
  unmap(A);
  unmap(B);
  // The provider retains the shared object after all client handles close.
  name("\\BaseNamedObjects\\Shared", 0);
  ASSERT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0u);
  const auto H2 = get(S);
  ASSERT_EQ(map(H2, 0), 0u);
  const auto C = get(S + 0x400);
  EXPECT_EQ(get(C, 2), 0x7654u);
  unmap(C);
  EXPECT_EQ(call("ZwClose", {H2}), 0u);
}
TEST_F(KernelSection, NamedInventoryDistinguishesUnknownMissingPathAndType) {
  initialize(8 * 1024 * 1024, std::nullopt,
             DriverSectionNamespace{{"\\BaseNamedObjects"}, {}});
  name("\\BaseNamedObjects\\Missing");
  put(S, 0xdead);
  EXPECT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0xc0000034u);
  EXPECT_EQ(get(S), 0u);
  name("\\BaseNamedObjects\\Missing\\Child");
  EXPECT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0xc000003au);
  name("\\BaseNamedObjects");
  EXPECT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0xc0000024u);
  name("\\Other\\Missing");
  error("ZwOpenSection", {S, 6, S + 0x300}, "inventory is unknown");
}
TEST_F(KernelSection,
       SnapshotRetainsCanonicalWritesAfterViewsAndHandlesRetire) {
  initialize(8 * 1024 * 1024, std::nullopt,
             DriverSectionNamespace{
                 {"\\BaseNamedObjects"},
                 {{"\\BaseNamedObjects\\Shared", 4097, {0x12, 0x34}, 4}},
                 {{"\\BaseNamedObjects\\Global", "\\BaseNamedObjects"}}});
  name("\\BaseNamedObjects\\Global\\Shared");
  ASSERT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0u);
  const auto H = get(S);
  ASSERT_EQ(map(H, 0, 0), 0u);
  const auto A = get(S + 0x400);
  ASSERT_EQ(map(H, 0, 0x1000, 2), 0u);
  const auto B = get(S + 0x400);
  put(A, 0x7654, 2);
  EXPECT_EQ(get(B, 2), 0x7654u);
  put(S + 0x500, 0xbbaa, 2);
  EXPECT_EQ(call("RtlCopyMemory", {A + 4096, S + 0x500, 2}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  unmap(A);
  unmap(B);
  ok(Model->snapshot());
  ASSERT_TRUE(Result.SectionNamespace);
  const auto &Snapshot = *Result.SectionNamespace;
  ASSERT_EQ(Snapshot.Sections.size(), 1u);
  EXPECT_EQ(Snapshot.Aliases.at("\\BaseNamedObjects\\Global"),
            "\\BaseNamedObjects");
  const auto &Bytes = Snapshot.Sections[0].Bytes;
  EXPECT_EQ(Snapshot.Sections[0].Size, 8192u);
  EXPECT_EQ(Snapshot.Sections[0].Protection, 4u);
  ASSERT_EQ(Bytes.size(), 8192u);
  EXPECT_EQ(Bytes[0], 0x54u);
  EXPECT_EQ(Bytes[1], 0x76u);
  EXPECT_EQ(Bytes[4096], 0xaau);
  EXPECT_EQ(Bytes[4097], 0xbbu);
  EXPECT_EQ(Bytes.back(), 0u);
  // A new observation refreshes bytes; previously returned copies stay stable.
  const auto First = Bytes;
  name("\\BaseNamedObjects\\Shared");
  ASSERT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0u);
  const auto H2 = get(S);
  ASSERT_EQ(map(H2, 0), 0u);
  put(get(S + 0x400), 0x9988, 2);
  ok(Model->snapshot());
  EXPECT_EQ(First[0], 0x54u);
  EXPECT_EQ(Result.SectionNamespace->Sections[0].Bytes[0], 0x88u);
  unmap(get(S + 0x400));
  EXPECT_EQ(call("ZwClose", {H2}), 0u);
}
TEST_F(KernelSection, SnapshotDistinguishesUnknownAndExplicitlyEmptyNamespace) {
  ok(Model->snapshot());
  EXPECT_FALSE(Result.SectionNamespace);
  initialize(8 * 1024 * 1024, std::nullopt,
             DriverSectionNamespace{{"\\BaseNamedObjects"}, {}});
  ok(Model->snapshot());
  ASSERT_TRUE(Result.SectionNamespace);
  EXPECT_TRUE(Result.SectionNamespace->Sections.empty());
  EXPECT_EQ(Result.SectionNamespace->Directories,
            std::vector<std::string>{"\\BaseNamedObjects"});
}
TEST_F(KernelSection, GlobalDirectoryAliasUsesTheSameObjectAndLinkOwner) {
  DriverSectionNamespace N{
      {"\\BaseNamedObjects"},
      {{"\\BaseNamedObjects\\Shared", 4096, {0x55}, 4}},
      {{"\\BaseNamedObjects\\Global", "\\BaseNamedObjects"}}};
  initialize(8 * 1024 * 1024, std::nullopt, N);
  name("\\BaseNamedObjects\\Global\\Shared");
  ASSERT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0u);
  const auto H = get(S);
  ASSERT_EQ(map(H, 0), 0u);
  const auto A = get(S + 0x400);
  put(A, 0x6655, 2);
  name("\\BaseNamedObjects\\Shared");
  ASSERT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0u);
  const auto H2 = get(S);
  ASSERT_EQ(map(H2, 0, 4096, 2), 0u);
  const auto B = get(S + 0x400);
  EXPECT_EQ(get(B, 2), 0x6655u);
  name("\\BaseNamedObjects\\Global");
  EXPECT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0xc0000024u);
  ASSERT_EQ(call("ZwOpenSymbolicLinkObject", {S, 1, S + 0x300}), 0u);
  const auto Link = get(S);
  error("IoDeleteSymbolicLink", {S + 0x100}, "provider-owned");
  EXPECT_EQ(call("ZwClose", {Link}), 0u);
  name("\\BaseNamedObjects\\Global\\Missing");
  EXPECT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0xc0000034u);
  name("\\BaseNamedObjects\\Shared\\Child");
  EXPECT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0xc0000024u);
  name("\\basenamedobjects\\Shared", 0);
  EXPECT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0xc000003au);
  name("\\BaseNamedObjects\\shared", 0);
  EXPECT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0xc0000034u);
  name("\\basenamedobjects", 0);
  EXPECT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0xc0000034u);
  unmap(A);
  unmap(B);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(call("ZwClose", {H2}), 0u);
}
TEST_F(KernelSection, NamedViewProtectionOffsetAndBoundsFailuresLeaveOutputs) {
  initialize(8 * 1024 * 1024, std::nullopt,
             DriverSectionNamespace{
                 {"\\BaseNamedObjects"},
                 {{"\\BaseNamedObjects\\Readonly", 0x11000, {0xaa}, 2}}});
  name("\\BaseNamedObjects\\Readonly");
  ASSERT_EQ(call("ZwOpenSection", {S, 6, S + 0x300}), 0u);
  const auto H = get(S);
  EXPECT_EQ(map(H, 0, 0x1000, 4), 0xc000004eu);
  EXPECT_EQ(get(S + 0x400), 0u);
  EXPECT_EQ(map(H, 0x1000, 0x1000, 2), 0xc0000220u);
  EXPECT_EQ(get(S + 0x410), 0x1000u);
  EXPECT_EQ(map(H, 0x10000, 0x2000, 2), 0xc000001fu);
  ASSERT_EQ(map(H, 0x10000, 0, 2), 0u);
  const auto V = get(S + 0x400);
  EXPECT_EQ(get(S + 0x408), 0x1000u);
  auto W = Memory->canAccess(V, 1, Write);
  ASSERT_TRUE(bool(W));
  EXPECT_FALSE(*W);
  unmap(V);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}
TEST_F(KernelSection, OpenCopiesNullOnFailureAndEnforcesIdentityAndIRQL) {
  EXPECT_EQ(Model->argumentCount("ZwOpenSection"), 3u);
  EXPECT_EQ(Model->argumentCount("ZwMapViewOfSection"), 10u);
  EXPECT_EQ(Model->argumentCount("ZwUnmapViewOfSection"), 2u);
  name("\\device\\physicalmemory", 0);
  put(S, 0xaaaa);
  EXPECT_EQ(call("ZwOpenSection", {S, 1, S + 0x300}), 0xc0000034u);
  EXPECT_EQ(get(S), 0u);
  name();
  put(S, 0xaaaa);
  EXPECT_EQ(call("ZwOpenSection", {S, 0x20, S + 0x300}), 0xc0000022u);
  EXPECT_EQ(get(S), 0u);
  name("\\BaseNamedObjects\\UnknownSection");
  error("ZwOpenSection", {S, 1, S + 0x300}, "inventory");
  error("ZwOpenSection", {S, 1, S + 0x300},
        "\\BaseNamedObjects\\UnknownSection");
  name();
  call("KfRaiseIrql", {1});
  error("ZwOpenSection", {S, 1, S + 0x300}, "PASSIVE_LEVEL");
  call("KeLowerIrql", {0});
}
TEST_F(KernelSection,
       ProviderNameCannotBecomeAGuestDeviceOrSymbolicLinkObject) {
  name("\\Device\\PhysicalMemory");
  EXPECT_EQ(call("IoCreateDevice",
                 {Model->driverObject(), 0, S + 0x100, 7, 0, 0, S + 0x500}),
            0xc0000035u);
  EXPECT_EQ(call("ZwOpenSymbolicLinkObject", {S, 1, S + 0x300}), 0xc0000024u);
}
TEST_F(KernelSection, TwoViewsAliasTheOriginalPagesAndOutliveTheHandle) {
  const auto P = pool(), PA = phys(P), H = open();
  put(P, 0x12345678, 4);
  ASSERT_EQ(map(H, PA & ~uint64_t(0xfff), 17), 0u);
  const auto A = get(S + 0x400);
  EXPECT_EQ(get(S + 0x408), 0x1000u);
  EXPECT_EQ(A & 0xffff, (PA & ~uint64_t(0xfff)) & 0xffff);
  ASSERT_EQ(map(H, PA & ~uint64_t(0xfff), 17, 2), 0u);
  const auto B = get(S + 0x400);
  EXPECT_NE(A, B);
  EXPECT_EQ(get(A + (PA & 0xfff), 4), 0x12345678u);
  put(A + (PA & 0xfff), 0x87654321, 4);
  EXPECT_EQ(get(P, 4), 0x87654321u);
  EXPECT_EQ(get(B + (PA & 0xfff), 4), 0x87654321u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(get(B + (PA & 0xfff), 4), 0x87654321u);
  error("ExFreePoolWithTag", {P, 0x54455354}, "pin");
  unmap(A + 9);
  error("ExFreePoolWithTag", {P, 0x54455354}, "pin");
  unmap(B + 17);
  free(P);
}
TEST_F(KernelSection, AccessRightsGenericMappingsAndReadonlyPermissions) {
  const auto P = pool(), PA = phys(P);
  const auto H = open(0x80000000);
  EXPECT_EQ(map(H, PA & ~uint64_t(0xfff), 0x1000, 4), 0xc0000022u);
  ASSERT_EQ(map(H, PA & ~uint64_t(0xfff), 0x1000, 2), 0u);
  const auto V = get(S + 0x400);
  auto R = Memory->canAccess(V, 4, Read), W = Memory->canAccess(V, 4, Write);
  ASSERT_TRUE(bool(R));
  ASSERT_TRUE(bool(W));
  EXPECT_TRUE(*R);
  EXPECT_FALSE(*W);
  auto Probe = Model->call("ProbeForWrite", {V + (PA & 0xfff), 4, 4});
  ASSERT_FALSE(bool(Probe));
  auto E = Probe.takeError();
  EXPECT_TRUE(E.isA<KernelGuestException>());
  llvm::consumeError(std::move(E));
  // Consume the typed error explicitly; the CPU mapping remains unchanged.
  unmap(V);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  free(P);
}
TEST_F(KernelSection, KernelHandleScopeAndProcessViewsSwitchAtomically) {
  const auto P = pool(0x20000), PA = phys(P), H = open(0xf001f, 0x240);
  put(P, 0x1111, 4);
  put(P + 0x10000, 0x2222, 4);
  ASSERT_EQ(map(H, PA & ~uint64_t(0xfff)), 0u);
  const auto A = get(S + 0x400);
  ok(Model->setUserRequestContext(true, 100));
  auto Missing = Memory->canAccess(A, 1, Read);
  ASSERT_TRUE(bool(Missing));
  EXPECT_FALSE(*Missing);
  ASSERT_EQ(map(H, (PA & ~uint64_t(0xfff)) + 0x10000), 0u);
  const auto B = get(S + 0x400);
  EXPECT_EQ(A, B);
  EXPECT_EQ(get(B + (PA & 0xfff), 4), 0x2222u);
  EXPECT_EQ(call("ProbeForRead", {B + (PA & 0xfff), 4, 4}), 0u);
  ok(Model->setUserRequestContext(false));
  EXPECT_EQ(get(A + (PA & 0xfff), 4), 0x1111u);
  unmap(A);
  ok(Model->setUserRequestContext(true, 100));
  EXPECT_EQ(get(B + (PA & 0xfff), 4), 0x2222u);
  unmap(B);
  ok(Model->setUserRequestContext(false));
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  free(P);
}
TEST_F(KernelSection, OrdinaryHandleCannotBeUsedInAnotherProcess) {
  const auto H = open();
  ok(Model->setUserRequestContext(true, 100));
  EXPECT_EQ(call("ZwClose", {H}), 0xc0000008u);
  name();
  error("ZwOpenSection", {S, 1, S + 0x300}, "OBJ_KERNEL_HANDLE");
  ok(Model->setUserRequestContext(false));
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}
TEST_F(KernelSection, MappingFailuresPreserveCapturedOutputsAndCachePolicy) {
  const auto P = pool(), PA = phys(P), H = open();
  put(S + 0x400, 0);
  put(S + 0x408, 17);
  put(S + 0x410, PA & ~uint64_t(0xfff));
  error("ZwMapViewOfSection",
        {H, UINT64_MAX, S + 0x400, 0, 0, S + 0x410, S + 0x408, 2, 0, 0x204},
        "cache");
  EXPECT_EQ(get(S + 0x400), 0u);
  EXPECT_EQ(get(S + 0x408), 17u);
  EXPECT_EQ(map(H, PA & ~uint64_t(0xfff), 17, 3), 0xc0000045u);
  put(S + 0x400, 0);
  put(S + 0x408, 17);
  put(S + 0x410, 0x12345000);
  error("ZwMapViewOfSection",
        {H, UINT64_MAX, S + 0x400, 0, 0, S + 0x410, S + 0x408, 2, 0, 4}, "PFN");
  EXPECT_EQ(get(S + 0x400), 0u);
  EXPECT_EQ(get(S + 0x408), 17u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  free(P);
}
TEST_F(KernelSection, IndependentMDLPagesShareCacheAndRetainSystemProtection) {
  const auto MDL = call("MmAllocatePagesForMdl", {0, UINT64_MAX, 0, 0x2000});
  ASSERT_NE(MDL, 0u);
  const auto PA = get(MDL + windows::MDLSize) * profile::PageSize;
  const auto H = open();
  ASSERT_EQ(map(H, PA, 0x2000, 0x204), 0u);
  const auto V = get(S + 0x400);
  put(V, 0x12345678, 4);
  put(V + 0x1000, 0xabcdef01, 4);
  const auto System = call("MmGetSystemAddressForMdlSafe", {MDL, 16});
  ASSERT_NE(System, 0u);
  EXPECT_EQ(get(System, 4), 0x12345678u);
  EXPECT_EQ(get(System + 0x1000, 4), 0xabcdef01u);
  EXPECT_EQ(call("MmProtectMdlSystemAddress", {MDL, 2}), 0u);
  put(V, 0x87654321, 4);
  EXPECT_EQ(get(System, 4), 0x87654321u);
  auto Writable = Memory->canAccess(System, 4, Write);
  ASSERT_TRUE(bool(Writable));
  EXPECT_FALSE(*Writable);
  ok(Model->validateGuestAccess(V, 4, true));
  put(S + 0x400, 0);
  put(S + 0x408, 0x2000);
  put(S + 0x410, PA);
  error("ZwMapViewOfSection",
        {H, UINT64_MAX, S + 0x400, 0, 0, S + 0x410, S + 0x408, 2, 0, 4},
        "cache");
  error("MmFreePagesFromMdl", {MDL}, "pin");
  EXPECT_EQ(get(V, 4), 0x87654321u);
  unmap(V);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(call("MmFreePagesFromMdl", {MDL}), 0u);
  EXPECT_EQ(call("ExFreePool", {MDL}), 0u);
}
TEST_F(KernelSection,
       PlacementConflictAndTopDownAllocationUseActualViewRanges) {
  const auto P = pool(), PA = phys(P), H = open();
  ASSERT_EQ(map(H, PA & ~uint64_t(0xfff)), 0u);
  const auto A = get(S + 0x400);
  EXPECT_EQ(map(H, PA & ~uint64_t(0xfff), 0x1000, 4, 2, A), 0xc0000018u);
  ASSERT_EQ(map(H, PA & ~uint64_t(0xfff), 0x1000, 4, 2, 0, 0x100000), 0u);
  const auto B = get(S + 0x400);
  EXPECT_GT(B, A);
  put(S + 0x400, 0);
  put(S + 0x408, 0x1000);
  put(S + 0x410, PA & ~uint64_t(0xfff));
  error("ZwMapViewOfSection",
        {H, UINT64_MAX, S + 0x400, 1, 0, S + 0x410, S + 0x408, 2, 0, 4},
        "ZeroBits");
  EXPECT_EQ(get(S + 0x400), 0u);
  EXPECT_EQ(call("ZwMapViewOfSection", {H, UINT64_MAX, S + 0x400, 21, 0,
                                        S + 0x410, S + 0x408, 2, 0, 4}),
            0xc000000du);
  unmap(A);
  unmap(B);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  free(P);
}
TEST_F(KernelSection,
       QueryNameRetainsTypeAndCloseDoesNotRequireFilesystemInventory) {
  const auto H = open();
  const std::u16string Name = u"\\Device\\PhysicalMemory";
  put(S + 0x900, 0xaaaaaaaa);
  for (uint64_t Size :
       {uint64_t(15), uint64_t(16), 16 + (Name.size() + 1) * 2 - 1}) {
    put(S + 0x600, 0xdeadbeef);
    EXPECT_EQ(call("ZwQueryObject", {H, 1, S + 0x600, Size, S + 0x500}),
              0xc0000004u);
    EXPECT_EQ(get(S + 0x600), 0xdeadbeefu);
  }
  EXPECT_EQ(get(S + 0x500, 4), 16 + (Name.size() + 1) * 2);
  ASSERT_EQ(call("ZwQueryObject", {H, 1, S + 0x600, 512, S + 0x500}), 0u);
  EXPECT_EQ(get(S + 0x600, 2), Name.size() * 2);
  for (size_t I = 0; I < Name.size(); ++I)
    EXPECT_EQ(get(get(S + 0x608) + I * 2, 2), Name[I]);
  EXPECT_EQ(call("ZwQueryInformationThread", {H, 9, S + 0x700, 8, 0}),
            0xc0000024u);
  EXPECT_EQ(call("ZwReadFile", {H, 0, 0, 0, S + 0x700, S + 0x900, 4, 0, 0}),
            0xc0000024u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(call("ZwQueryObject", {H, 1, S + 0x600, 512, 0}), 0xc0000008u);
}
TEST_F(KernelSection, AliasAccessRetainsOpaqueObjectBoundaries) {
  const auto P = pool(), H = open(), PA = phys(Model->driverObject());
  ASSERT_EQ(map(H, PA & ~uint64_t(0xfff)), 0u);
  const auto V = get(S + 0x400), B = V + (PA & 0xfff);
  auto E = Model->validateGuestAccess(B + windows::DriverFlagsOffset, 4, false);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  ok(Model->validateGuestAccess(B, 2, false));
  E = Model->validateGuestAccess(B + windows::DriverFlagsOffset, 4, true);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  unmap(V);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  free(P);
}
TEST_F(KernelSection, SharedClockAliasesRetainOpacityAndObserveOwnerUpdates) {
  initialize(8 * 1024 * 1024, 156250);
  ok(Model->synchronizeGuestPageTables());
  const auto PA = phys(windows::SharedClockDataBase), H = open();
  ASSERT_EQ(map(H, PA, 0x1000, 2), 0u);
  const auto View = get(S + 0x400);
  const auto Tick = View + windows::SharedTickCountOffset;
  ok(Model->validateGuestAccess(Tick, 8, false));
  EXPECT_EQ(get(Tick), 0u);
  auto Opaque = Model->validateGuestAccess(Tick - 1, 8, false);
  ASSERT_TRUE(bool(Opaque));
  EXPECT_NE(llvm::toString(std::move(Opaque)).find("shared clock data"),
            std::string::npos);
  ok(Model->advanceExecutionTo100ns(2000000));
  EXPECT_EQ(get(Tick), 12u);
  EXPECT_EQ(get(windows::SharedClockDataBase + windows::SharedTickCountOffset),
            12u);
  auto Writable = Memory->canAccess(Tick, 8, Write);
  ASSERT_TRUE(bool(Writable));
  EXPECT_FALSE(*Writable);
  unmap(View);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelSection, MemoryQuotaFailureRollsBackEveryNewPin) {
  initialize(4 * 1024 * 1024);
  const auto P = pool(0x40000), PA = phys(P), H = open();
  std::vector<uint64_t> Views;
  uint64_t Status = 0;
  for (unsigned I = 0; I < 64; ++I) {
    Status = map(H, PA & ~uint64_t(0xfff), 0x40000);
    if (Status)
      break;
    Views.push_back(get(S + 0x400));
  }
  ASSERT_FALSE(Views.empty());
  EXPECT_EQ(Status, 0xc0000017u);
  EXPECT_EQ(get(S + 0x400), 0u);
  EXPECT_EQ(get(S + 0x408), 0x40000u);
  for (auto V : Views)
    unmap(V);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  free(P);
}
TEST_F(KernelSection, UnloadRequiresBothHandlesAndViewsToBeReleased) {
  const auto P = pool(), PA = phys(P), H = open();
  ASSERT_EQ(map(H, PA & ~uint64_t(0xfff)), 0u);
  const auto V = get(S + 0x400);
  put(Model->driverObject() + windows::DriverUnloadOffset, PC);
  ok(Model->finishEntry());
  auto U = Model->beginUnload();
  ASSERT_TRUE(bool(U));
  auto E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  unmap(V);
  free(P);
  ok(Model->finishUnload());
}
TEST(KernelSectionDriver, ContiguousPagesReachCompiledDriverAndReleaseCleanly) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Contract) << " " << Preemptive);
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      O.PageTables = true;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_contiguous.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    }
}

TEST(KernelSectionDriver, NamedViewsCompleteWithBothExecutionContracts) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.Contract = Contract;
      O.Unload = true;
      O.SectionNamespace = DriverSectionNamespace{
          {"\\BaseNamedObjects"},
          {{"\\BaseNamedObjects\\Shared", 0x11000, {0x12, 0x34}, 4}}};
      O.ProcessEvents = {{DriverProcessEvent::Kind::Create, 100, 4, 4, 0},
                         {DriverProcessEvent::Kind::Create, 200, 100, 100, 0},
                         {DriverProcessEvent::Kind::Exit, 200, 100, 4, 0},
                         {DriverProcessEvent::Kind::Exit, 100, 4, 4, 0}};
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_named_sections.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      ASSERT_TRUE(R->SectionNamespace);
      ASSERT_EQ(R->SectionNamespace->Sections.size(), 1u);
      const auto &Bytes = R->SectionNamespace->Sections[0].Bytes;
      ASSERT_EQ(Bytes.size(), 0x11000u);
      EXPECT_EQ(llvm::support::endian::read32le(Bytes.data()), 0x76543210u);
      EXPECT_EQ(R->Configuration.SectionNamespace->Sections[0].Bytes,
                (std::vector<uint8_t>{0x12, 0x34}));
    }
}
TEST(KernelSectionDriver, RealGuestViewsInheritAcrossProcessesAndRetireAtExit) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      SCOPED_TRACE(executionContractName(Contract));
      SCOPED_TRACE(Preemptive);
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      O.PageTables = true;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      O.ProcessEvents = {{DriverProcessEvent::Kind::Create, 100, 4, 4, 0},
                         {DriverProcessEvent::Kind::Create, 200, 100, 100, 0},
                         {DriverProcessEvent::Kind::Exit, 200, 100, 4, 0},
                         {DriverProcessEvent::Kind::Exit, 100, 4, 4, 0}};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_sections.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      ASSERT_EQ(R->ProcessEvents.size(), 4u);
      for (const auto &E : R->ProcessEvents) {
        EXPECT_TRUE(E.Completed);
        EXPECT_EQ(E.DeliveredRoutines.size(), 1u);
      }
    }
}
} // namespace
} // namespace neverd::emulation
