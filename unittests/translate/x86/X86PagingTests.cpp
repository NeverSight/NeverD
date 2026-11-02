//===- X86PagingTests.cpp - x86 page-walk and fault contracts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Mode boundaries, page sizes, access rights and fault-side effects.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/X86/X86PagedMemory.h"
#include "neverd/translate/X86/X86Paging.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using namespace neverd::translate;

namespace {

class X86PagingTest : public testing::Test {
protected:
  X86PagingFeatures F{.PhysicalAddressBits = 48,
                      .PSE = true,
                      .PAE = true,
                      .LongMode = true,
                      .PSE36 = true,
                      .PAT = true,
                      .Page1GB = true,
                      .LA57 = true,
                      .NX = true,
                      .SMEP = true,
                      .SMAP = true,
                      .PKU = true,
                      .PKS = true};
  X86PagingState S{.CR0 = 0x80010001,
                   .CR3 = 0x1000,
                   .CR4 = 0x20,
                   .EFER = 0xd00,
                   .RFlags = 2};
  X86LinearAccess A{.Address = 0x123};
  std::unique_ptr<GuestMemoryRuntime> Memory;

  void SetUp() override { reset(); }

  void reset(MemoryPermission Permissions = MemoryPermission::Read |
                                            MemoryPermission::Write) {
    auto State =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    State.Memory.push_back({0, Permissions, 10, std::vector<uint8_t>(0x10000)});
    Memory = llvm::cantFail(GuestMemoryRuntime::create(State));
  }

  void put(uint64_t Address, uint64_t Value, bool Narrow = false) {
    const auto Result = Memory->storeScalar(
        Address, Narrow ? GuestScalarWidth::I32 : GuestScalarWidth::I64, Value);
    ASSERT_NE(Result.Status, GuestMemoryAccessStatus::Fault);
  }

  uint64_t get(uint64_t Address, bool Narrow = false) {
    auto R = Memory->loadScalar(Address, Narrow ? GuestScalarWidth::I32
                                                : GuestScalarWidth::I64);
    EXPECT_EQ(R.Status, GuestMemoryAccessStatus::Completed);
    return R.Value;
  }

  void fourLevels(uint64_t Leaf = 0x9007) {
    put(0x1000, 0x2007);
    put(0x2000, 0x3007);
    put(0x3000, 0x4007);
    put(0x4000, Leaf);
  }

  X86PageWalkResult walk() {
    return llvm::cantFail(walkX86PageTables(*Memory, F, S, A));
  }

  std::unique_ptr<X86PagedMemory> paged() {
    auto RAM =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory = Memory->snapshotMemoryRegions();
    return llvm::cantFail(X86PagedMemory::create(RAM, F, S));
  }

  void expectPageFault(unsigned Code) {
    const auto R = walk();
    ASSERT_EQ(R.Status, X86PageWalkStatus::Exception);
    ASSERT_TRUE(R.Exception);
    EXPECT_EQ(R.Exception->Vector, 14);
    EXPECT_EQ(R.Exception->ErrorCode, Code);
    EXPECT_EQ(R.Exception->CR2, A.Address);
    EXPECT_FALSE(R.BackingFault);
  }
};

TEST_F(X86PagingTest, FourAndFiveLevelsTranslateBothCanonicalHalves) {
  for (bool Five : {false, true}) {
    S.CR4 = 0x20 | (Five ? 0x1000 : 0);
    const unsigned Levels = Five ? 5 : 4;
    for (bool High : {false, true}) {
      A.Address = High ? UINT64_MAX - 0xedc : 0x123;
      for (unsigned I = 0; I != Levels; ++I) {
        const uint64_t Entry = (I + 1) * 0x1000 + (High ? 511 * 8 : 0);
        put(Entry, (I + 1 == Levels ? 0x9000 : (I + 2) * 0x1000) | 7);
      }
      const auto R = walk();
      ASSERT_EQ(R.Status, X86PageWalkStatus::Completed);
      EXPECT_EQ(R.PhysicalAddress, 0x9123u);
      EXPECT_EQ(R.PageSize, 4096u);
    }
  }
}

TEST_F(X86PagingTest, NoncanonicalStackAndDataFaultBeforeRAM) {
  for (bool Five : {false, true}) {
    S.CR4 = 0x20 | (Five ? 0x1000 : 0);
    for (uint64_t Address :
         {uint64_t(1) << (Five ? 56 : 47),
          UINT64_MAX ^ ((uint64_t(1) << (Five ? 56 : 47)))}) {
      A.Address = Address;
      for (bool Stack : {false, true}) {
        A.Stack = Stack;
        const auto R = walk();
        ASSERT_EQ(R.Status, X86PageWalkStatus::Exception);
        ASSERT_TRUE(R.Exception);
        EXPECT_EQ(R.Exception->Vector, Stack ? 12 : 13);
        EXPECT_EQ(R.Exception->ErrorCode, 0u);
        EXPECT_FALSE(R.Exception->CR2);
        EXPECT_EQ(get(0x1000), 0u);
      }
    }
  }
}

TEST_F(X86PagingTest, NoncanonicalOperandTailPrecedesFirstPageFault) {
  for (bool Five : {false, true}) {
    S.CR4 = 0x20 | (Five ? 0x1000 : 0);
    A.Address = (uint64_t(1) << (Five ? 56 : 47)) - 4;
    // The first byte is canonical but its page is absent. The whole-operand
    // canonical check still wins, with no page-table or output publication.
    for (bool Stack : {false, true})
      for (auto Kind : {MemoryAccessKind::Read, MemoryAccessKind::Write}) {
        A.Stack = Stack;
        A.Kind = Kind;
        auto P = paged();
        const auto Before = P->physicalMemory().snapshotMemoryRegions();
        std::array<uint8_t, 8> Bytes;
        Bytes.fill(0xa5);
        auto R =
            llvm::cantFail(Kind == MemoryAccessKind::Write ? P->store(A, Bytes)
                                                           : P->load(A, Bytes));
        ASSERT_EQ(R.Status, X86PageWalkStatus::Exception);
        ASSERT_TRUE(R.Exception);
        EXPECT_EQ(R.Exception->Vector, Stack ? 12 : 13);
        EXPECT_EQ(R.Exception->ErrorCode, 0u);
        EXPECT_FALSE(R.Exception->CR2);
        EXPECT_EQ(P->physicalMemory().snapshotMemoryRegions().front().Bytes,
                  Before.front().Bytes);
        EXPECT_TRUE(std::all_of(Bytes.begin(), Bytes.end(),
                                [](uint8_t V) { return V == 0xa5; }));
      }
  }
}

TEST_F(X86PagingTest, EveryLevelContributesPermissionsAndNX) {
  for (unsigned Level = 0; Level != 4; ++Level) {
    for (MemoryAccessKind Kind :
         {MemoryAccessKind::Read, MemoryAccessKind::Write,
          MemoryAccessKind::Execute}) {
      fourLevels();
      A.Kind = Kind;
      A.CPL = 3;
      const uint64_t Address = (Level + 1) * 0x1000;
      uint64_t Entry = get(Address);
      if (Kind == MemoryAccessKind::Read)
        Entry &= ~uint64_t(4);
      else if (Kind == MemoryAccessKind::Write)
        Entry &= ~uint64_t(2);
      else
        Entry |= uint64_t(1) << 63;
      put(Address, Entry);
      expectPageFault(5 | (Kind == MemoryAccessKind::Write ? 2 : 0) |
                      (Kind == MemoryAccessKind::Execute ? 16 : 0));
      EXPECT_EQ(get(0x4000) & 64, 0u);
    }
  }
}

TEST_F(X86PagingTest, NotPresentPrecedesReservedAndPreservesOffendingEntry) {
  for (unsigned Level = 0; Level != 4; ++Level) {
    fourLevels();
    A.CPL = 3;
    A.Kind = MemoryAccessKind::Write;
    const uint64_t Address = (Level + 1) * 0x1000;
    put(Address, uint64_t(1) << 49);
    expectPageFault(6);
    EXPECT_EQ(get(Address), uint64_t(1) << 49);
    put(Address, (uint64_t(1) << 49) | 7);
    expectPageFault(15);
    EXPECT_EQ(get(Address), (uint64_t(1) << 49) | 7);
  }
}

TEST_F(X86PagingTest, NXReservedAndFetchErrorBitDependOnEnabledControls) {
  fourLevels((uint64_t(1) << 63) | 0x9007);
  S.EFER = 0x500;
  A.Kind = MemoryAccessKind::Execute;
  expectPageFault(9);
  S.CR4 |= uint64_t(1) << 20;
  expectPageFault(25);
  S.CR4 = 0x20;
  S.EFER = 0xd00;
  expectPageFault(17);
  fourLevels(0);
  expectPageFault(16);
}

TEST_F(X86PagingTest, WriteProtectAndDirtyCommit) {
  fourLevels(0x9005);
  A.Kind = MemoryAccessKind::Write;
  expectPageFault(3);
  S.CR0 &= ~uint64_t(0x10000);
  ASSERT_EQ(walk().Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(get(0x4000), 0x9065u);
  A.CPL = 3;
  expectPageFault(7);
}

TEST_F(X86PagingTest, SMEPAndSMAPDistinguishImplicitSupervisorAccesses) {
  fourLevels();
  S.CR4 |= uint64_t(3) << 20;
  A.Kind = MemoryAccessKind::Execute;
  expectPageFault(17);
  A.Kind = MemoryAccessKind::Read;
  expectPageFault(1);
  S.RFlags |= uint64_t(1) << 18;
  ASSERT_EQ(walk().Status, X86PageWalkStatus::Completed);
  A.ImplicitSupervisor = true;
  expectPageFault(1);
  A.CPL = 3;
  expectPageFault(1);
  A.ImplicitSupervisor = false;
  ASSERT_EQ(walk().Status, X86PageWalkStatus::Completed);
}

TEST_F(X86PagingTest, ProtectionKeysSetPKWithOtherPermissionFailures) {
  const uint64_t Key = uint64_t(11) << 59;
  for (bool UserPage : {false, true}) {
    for (unsigned CPL : {0u, 3u}) {
      for (unsigned Pair : {0u, 1u, 2u, 3u}) {
        for (bool Write : {false, true}) {
          fourLevels(0x9003 | (UserPage ? 4 : 0) | Key);
          S.CR4 = 0x20 | (uint64_t(1) << (UserPage ? 22 : 24));
          S.PKRU = S.PKRS = Pair << 22;
          A.CPL = CPL;
          A.Kind = Write ? MemoryAccessKind::Write : MemoryAccessKind::Read;
          const bool KeyDenied = (Pair & 1) || ((Pair & 2) && Write);
          if (KeyDenied || (CPL == 3 && !UserPage))
            expectPageFault(1 | (KeyDenied ? 32 : 0) | (Write ? 2 : 0) |
                            (CPL == 3 ? 4 : 0));
          else
            EXPECT_EQ(walk().Status, X86PageWalkStatus::Completed);
        }
      }
    }
  }
  A.CPL = 0;
  A.Kind = MemoryAccessKind::Write;
  S.CR0 &= ~uint64_t(0x10000);
  S.PKRU = 2u << 22;
  S.CR4 = 0x400020;
  fourLevels(0x9007 | Key);
  EXPECT_EQ(walk().Status, X86PageWalkStatus::Completed);
}

TEST_F(X86PagingTest, LargePagesCheckAlignmentPATAndCPUID) {
  for (bool Gigabyte : {false, true}) {
    fourLevels();
    const uint64_t EntryAddress = Gigabyte ? 0x2000 : 0x3000;
    const uint64_t Base = Gigabyte ? 0x80000000 : 0xa00000;
    put(EntryAddress, Base | 0x1087);
    auto R = walk();
    ASSERT_EQ(R.Status, X86PageWalkStatus::Completed);
    EXPECT_EQ(R.PhysicalAddress, Base + A.Address);
    EXPECT_EQ(R.PageSize, Gigabyte ? 0x40000000u : 0x200000u);
    F.PAT = false;
    expectPageFault(9);
    F.PAT = true;
    put(EntryAddress, Base | 0x2087);
    expectPageFault(9);
    put(EntryAddress, Base | 0x87);
    if (Gigabyte) {
      F.Page1GB = false;
      expectPageFault(9);
    }
  }
  fourLevels();
  put(0x1000, 0x2087);
  expectPageFault(9);
}

TEST_F(X86PagingTest, LegacyPagingAndPSE36) {
  S.EFER = 0;
  S.CR4 = 0;
  put(0x1000, 0x2087, true); // PS is ignored with PSE=0.
  put(0x2000, 0x9007, true);
  EXPECT_EQ(walk().PhysicalAddress, 0x9123u);
  S.CR4 = 0x10;
  put(0x1000, 0x12145087, true);
  auto R = walk();
  ASSERT_EQ(R.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(R.PhysicalAddress, 0xa212000123ULL);
  EXPECT_EQ(R.PageSize, 0x400000u);
  F.PSE36 = false;
  expectPageFault(9);
  F.PSE36 = true;
  F.PhysicalAddressBits = 36;
  expectPageFault(9);
  put(0x1000, 0x12012087, true);
  EXPECT_EQ(walk().PhysicalAddress, 0x912000123ULL);
}

TEST_F(X86PagingTest, PAEUsesCachedPDPTEAndDoesNotGiveItAccessRights) {
  S.EFER = 0x800;
  S.PDPTE[0] = 0x1001;
  S.CR3 = 0xf000;
  put(0xf000, UINT64_MAX); // This is not consulted during a PAE access.
  put(0x1000, 0x2007);
  put(0x2000, 0x9007);
  A.CPL = 3;
  EXPECT_EQ(walk().PhysicalAddress, 0x9123u);
  EXPECT_EQ(S.PDPTE[0], 0x1001u);
  EXPECT_EQ(get(0xf000), UINT64_MAX);
  put(0x2000, 0x9007 | (uint64_t(1) << 59));
  expectPageFault(13); // Ignored in IA-32e, reserved in PAE.
  S.PDPTE[0] = 0;
  expectPageFault(4);
}

TEST_F(X86PagingTest, PAERegisterReloadIsTransactionalAndUsesGP) {
  const std::array<uint64_t, 4> Old{0x1001, 0x2001, 0x3001, 0x4001};
  auto Bank = Old;
  put(0x8000, 0x5001);
  put(0x8008, 0x6001);
  put(0x8010, 0x7003); // PDPTE.R/W is reserved.
  put(0x8018, 0);
  auto R = llvm::cantFail(loadX86PAEPDPTE(*Memory, F, 0x801f, Bank));
  ASSERT_EQ(R.Status, X86PageWalkStatus::Exception);
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 13);
  EXPECT_FALSE(R.Exception->CR2);
  EXPECT_EQ(Bank, Old);
  put(0x8010, UINT64_MAX - 1); // Non-present reserved bits are ignored.
  R = llvm::cantFail(loadX86PAEPDPTE(*Memory, F, 0x801f, Bank));
  EXPECT_EQ(R.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(Bank[0], 0x5001u);
  EXPECT_EQ(Bank[2], UINT64_MAX - 1);
  const auto Loaded = Bank;
  R = llvm::cantFail(loadX86PAEPDPTE(*Memory, F, 0x10000, Bank));
  EXPECT_EQ(R.Status, X86PageWalkStatus::BackingFault);
  EXPECT_EQ(Bank, Loaded);
  F.PAE = false;
  auto Unsupported = loadX86PAEPDPTE(*Memory, F, 0x8000, Bank);
  EXPECT_FALSE(bool(Unsupported));
  llvm::consumeError(Unsupported.takeError());
  EXPECT_EQ(Bank, Loaded);
}

TEST_F(X86PagingTest, PhysicalBackingFaultIsNeverInventedAsPageFault) {
  put(0x1000, 0x100007);
  const auto R = walk();
  ASSERT_EQ(R.Status, X86PageWalkStatus::BackingFault);
  EXPECT_FALSE(R.Exception);
  ASSERT_TRUE(R.BackingFault);
  EXPECT_EQ(R.BackingFault->Kind, RuntimeMemoryFaultKindV1::Unmapped);
  EXPECT_EQ(R.BackingFault->Exit.Address, 0x100000u);
  EXPECT_EQ(get(0x1000), 0x100027u);
}

TEST_F(X86PagingTest, AccessedDirtyAndExecutableInvalidationsRemainVisible) {
  reset(MemoryPermission::Read | MemoryPermission::Write |
        MemoryPermission::Execute);
  fourLevels();
  A.Kind = MemoryAccessKind::Write;
  auto R = walk();
  EXPECT_EQ(R.Status, X86PageWalkStatus::Completed);
  ASSERT_EQ(R.SelfModifications.size(), 5u);
  EXPECT_EQ(get(0x1000), 0x2027u);
  EXPECT_EQ(get(0x2000), 0x3027u);
  EXPECT_EQ(get(0x3000), 0x4027u);
  EXPECT_EQ(get(0x4000), 0x9067u);
  for (size_t I = 1; I < R.SelfModifications.size(); ++I)
    EXPECT_EQ(R.SelfModifications[I].OldGeneration,
              R.SelfModifications[I - 1].NewGeneration);
  EXPECT_TRUE(walk().SelfModifications.empty());
  put(0x1000, 0x2007);
  put(0x2000, 0);
  R = walk();
  EXPECT_EQ(R.Status, X86PageWalkStatus::Exception);
  ASSERT_EQ(R.SelfModifications.size(), 1u);
  EXPECT_EQ(get(0x1000), 0x2027u);
  EXPECT_EQ(get(0x2000), 0u);
}

TEST_F(X86PagingTest, DisabledPagingProjectsNoPermissionsOrPageWalk) {
  S.CR0 = 1;
  S.EFER = 0;
  A.Address = 0xffffffff;
  A.Kind = MemoryAccessKind::Write;
  A.CPL = 3;
  const auto R = walk();
  EXPECT_EQ(R.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(R.PhysicalAddress, 0xffffffffu);
  EXPECT_EQ(get(0x1000), 0u);
}

TEST_F(X86PagingTest, InvalidProfilesHaveNoMemoryEffects) {
  fourLevels();
  const auto Original = S;
  for (uint64_t Unsupported :
       {uint64_t(1) << 23, uint64_t(1) << 27, uint64_t(1) << 28}) {
    S.CR4 = Original.CR4 | Unsupported;
    auto R = walkX86PageTables(*Memory, F, S, A);
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x1000), 0x2007u);
  }
  S = Original;
  S.CR3 |= uint64_t(1) << 49;
  auto R = walkX86PageTables(*Memory, F, S, A);
  EXPECT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(0x1000), 0x2007u);
}

TEST_F(X86PagingTest, CrossPageOperandsUseDiscontiguousPhysicalFrames) {
  fourLevels();
  put(0x4008, 0xb007);
  for (unsigned Size : {2u, 4u, 8u, 16u, 32u, 64u}) {
    for (unsigned Prefix = 1; Prefix != Size; ++Prefix) {
      SCOPED_TRACE(testing::Message()
                   << "size=" << Size << " prefix=" << Prefix);
      auto P = paged();
      std::vector<uint8_t> Input(Size), Output(Size, 0xff);
      for (unsigned I = 0; I != Size; ++I)
        Input[I] = uint8_t(I * 7 + 13);
      A.Address = 4096 - Prefix;
      A.Kind = MemoryAccessKind::Write;
      ASSERT_EQ(llvm::cantFail(P->store(A, Input)).Status,
                X86PageWalkStatus::Completed);
      std::vector<uint8_t> First(Prefix), Second(Size - Prefix);
      ASSERT_EQ(P->physicalMemory().loadBytes(0xa000 - Prefix, First).Status,
                GuestMemoryAccessStatus::Completed);
      ASSERT_EQ(P->physicalMemory().loadBytes(0xb000, Second).Status,
                GuestMemoryAccessStatus::Completed);
      EXPECT_TRUE(std::equal(First.begin(), First.end(), Input.begin()));
      EXPECT_TRUE(
          std::equal(Second.begin(), Second.end(), Input.begin() + Prefix));
      A.Kind = MemoryAccessKind::Read;
      ASSERT_EQ(llvm::cantFail(P->load(A, Output)).Status,
                X86PageWalkStatus::Completed);
      EXPECT_EQ(Output, Input);
    }
  }
}

TEST_F(X86PagingTest, SecondPageFaultDoesNotPublishDataOrLoadResult) {
  fourLevels();
  put(0x9ff8, 0x12345678abcdef90ULL);
  const std::array<uint8_t, 16> Input{1, 2, 3, 4, 5, 6, 7, 8};
  for (MemoryAccessKind Kind : {MemoryAccessKind::Read, MemoryAccessKind::Write,
                                MemoryAccessKind::Execute}) {
    A = {0xff8, Kind, 3};
    auto P = paged();
    std::array<uint8_t, 16> Output;
    Output.fill(0xa5);
    auto R =
        llvm::cantFail(Kind == MemoryAccessKind::Write ? P->store(A, Input)
                                                       : P->load(A, Output));
    ASSERT_EQ(R.Status, X86PageWalkStatus::Exception);
    ASSERT_TRUE(R.Exception);
    EXPECT_EQ(R.Exception->Vector, 14);
    EXPECT_EQ(R.Exception->CR2, 0x1000u);
    EXPECT_EQ(R.Exception->ErrorCode, Kind == MemoryAccessKind::Read    ? 4u
                                      : Kind == MemoryAccessKind::Write ? 6u
                                                                        : 20u);
    EXPECT_TRUE(std::all_of(Output.begin(), Output.end(),
                            [](uint8_t V) { return V == 0xa5; }));
    EXPECT_EQ(
        P->physicalMemory().loadScalar(0x9ff8, GuestScalarWidth::I64).Value,
        0x12345678abcdef90ULL);
    EXPECT_NE(
        P->physicalMemory().loadScalar(0x4000, GuestScalarWidth::I64).Value &
            32,
        0u);
  }
}

TEST_F(X86PagingTest, PhysicalBackingFailureAlsoPreservesTheWholeOperand) {
  fourLevels();
  put(0x4008, 0x100007); // Present guest PTE, absent physical RAM.
  put(0x9ff8, 0x12345678abcdef90ULL);
  auto P = paged();
  std::array<uint8_t, 16> Bytes{};
  A = {0xff8, MemoryAccessKind::Write, 0};
  auto R = llvm::cantFail(P->store(A, Bytes));
  ASSERT_EQ(R.Status, X86PageWalkStatus::BackingFault);
  EXPECT_FALSE(R.Exception);
  EXPECT_EQ(P->physicalMemory().loadScalar(0x9ff8, GuestScalarWidth::I64).Value,
            0x12345678abcdef90ULL);
  Bytes.fill(0xa5);
  A.Kind = MemoryAccessKind::Read;
  R = llvm::cantFail(P->load(A, Bytes));
  EXPECT_EQ(R.Status, X86PageWalkStatus::BackingFault);
  EXPECT_TRUE(std::all_of(Bytes.begin(), Bytes.end(),
                          [](uint8_t V) { return V == 0xa5; }));
}

TEST_F(X86PagingTest, GuestFetchUsesNXInsteadOfPhysicalExecutePermission) {
  fourLevels();
  put(0x9000, 0x12345678);
  A = {0, MemoryAccessKind::Execute, 3};
  std::array<uint8_t, 4> Bytes{};
  auto P = paged();
  EXPECT_EQ(llvm::cantFail(P->load(A, Bytes)).Status,
            X86PageWalkStatus::Completed);
  EXPECT_EQ(Bytes, (std::array<uint8_t, 4>{0x78, 0x56, 0x34, 0x12}));
  P->physicalMemory().storeScalar(0x4000, GuestScalarWidth::I64,
                                  (uint64_t(1) << 63) | 0x9007);
  const auto Before = Bytes;
  const auto R = llvm::cantFail(P->load(A, Bytes));
  ASSERT_EQ(R.Status, X86PageWalkStatus::Exception);
  EXPECT_EQ(R.Exception->ErrorCode, 21u);
  EXPECT_EQ(Bytes, Before);
}

TEST_F(X86PagingTest, InvalidRebindingPreservesThePreviousPagingControls) {
  fourLevels();
  auto P = paged();
  auto Bad = S;
  Bad.CR4 |= uint64_t(1) << 23;
  auto Error = P->bindControls(Bad);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(P->controls().CR4, S.CR4);
}

TEST_F(X86PagingTest, PhysicalExecutePermissionsCannotMasqueradeAsPaging) {
  auto RAM = llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  RAM.Memory.push_back({0, MemoryPermission::Read | MemoryPermission::Execute,
                        0, std::vector<uint8_t>(4096)});
  auto P = X86PagedMemory::create(RAM, F, S);
  EXPECT_FALSE(bool(P));
  llvm::consumeError(P.takeError());
}

} // namespace
