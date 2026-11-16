//===- X86ModeSegmentTests.inc - Mixed-mode selector transfers -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Full cache writes and retained operand effects across selector loads.
//===----------------------------------------------------------------------===//

class X86ModeSegmentTest : public X86ModeExecutionTest {
protected:
  using Mode = X86ExecutionMode;
  static constexpr Mode Modes[]{Mode::Protected16, Mode::Protected32,
                                Mode::Compatibility16, Mode::Compatibility32};
  static constexpr unsigned UnusableBits[]{3, 0, 1, 2, 4, 5};

  static bool code32(Mode M) {
    return M == Mode::Protected32 || M == Mode::Compatibility32;
  }

  void start(Mode M) {
    resetMode();
    Memory.reset();
    if (!code32(M))
      Machine.Core.Core.Core.CS.Attributes &= ~0x4000;
    if (M == Mode::Compatibility16 || M == Mode::Compatibility32)
      Controls = {0x80010031, 0x10000, 0x20, 0x500, 2};
    else
      for (unsigned V : {1u, 11u, 12u, 13u, 14u})
        put(0x9000 + V * 8,
            (0x4000 + V * 16) | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
    Options.Legacy.Segments.NullBasePolicy = X86NullSegmentBasePolicy::Clear;
    Options.Legacy.Segments.DescriptorCanonicality =
        X86DescriptorCanonicalityProfile::SelectorForRange;
    auto &R = registers();
    for (unsigned I = 0; I != 8; ++I) {
      R.Undefined[I] = uint64_t(0x12340000 + I) << 32;
      R.GPR[I] |= R.Undefined[I];
    }
    R.Undefined[4] = 0;
    R.GPR[4] = 0xf000;
    R.GPR[15] = 0x1122334455667788ULL;
    Machine.Core.Core.Core.Core.Core.KernelGSBase = 0xffffabcd12345678ULL;
  }

  X64ContinuationStateV1 &registers() {
    return Machine.Core.Core.Core.Core.Core.Core;
  }

  X64ContinuationSegment cache(unsigned Index) {
    auto &V = Machine.Core.Core;
    switch (Index) {
    case 0:
      return V.ES;
    case 1:
      return V.Core.CS;
    case 2:
      return V.Core.SS;
    case 3:
      return V.DS;
    case 4:
      return {registers().FSBase, V.FS.Limit, V.FS.Selector, V.FS.Attributes};
    default:
      return {registers().GSBase, V.GS.Limit, V.GS.Selector, V.GS.Attributes};
    }
  }

  void setCache(unsigned Index, X64ContinuationSegment C) {
    auto &V = Machine.Core.Core;
    switch (Index) {
    case 0:
      V.ES = C;
      break;
    case 1:
      V.Core.CS = C;
      break;
    case 2:
      V.Core.SS = C;
      break;
    case 3:
      V.DS = C;
      break;
    case 4:
      registers().FSBase = C.Base;
      V.FS = {C.Limit, C.Selector, C.Attributes};
      break;
    default:
      registers().GSBase = C.Base;
      V.GS = {C.Limit, C.Selector, C.Attributes};
      break;
    }
  }

  static uint64_t descriptor(uint32_t Base, uint32_t Limit, uint16_t A) {
    const uint32_t Raw = A & 0x8000 ? Limit >> 12 : Limit;
    return (Raw & 0xffff) | (uint64_t(Base & 0xffffff) << 16) |
           (uint64_t(A & 0xff) << 40) | (uint64_t(Raw & 0xf0000) << 32) |
           (uint64_t(A & 0xf000) << 40) | (uint64_t(Base >> 24) << 56);
  }

  std::vector<uint8_t> absolute(Mode M, std::vector<uint8_t> Opcode,
                                unsigned Register, uint32_t Address,
                                unsigned OperandBytes = 0) {
    std::vector<uint8_t> Bytes;
    if (!code32(M))
      Bytes.push_back(0x67);
    if (OperandBytes && (OperandBytes == 4) != code32(M))
      Bytes.push_back(0x66);
    Bytes.insert(Bytes.end(), Opcode.begin(), Opcode.end());
    Bytes.push_back(5 | (Register << 3));
    for (unsigned I = 0; I != 4; ++I)
      Bytes.push_back(Address >> (I * 8));
    return Bytes;
  }

  llvm::Expected<X86ModeExecutionResult> run(Mode M, std::vector<uint8_t> Bytes,
                                             uint64_t &Budget) {
    add(registers().PC, M, std::move(Bytes));
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    return X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
  }

  uint64_t physical(uint64_t Address, GuestScalarWidth Width) {
    const auto Value = Memory->physicalMemory().loadScalar(Address, Width);
    EXPECT_EQ(Value.Status, GuestMemoryAccessStatus::Completed);
    return Value.Value;
  }

  static void omitBacking(GuestState &Image, uint64_t Address, uint64_t Size) {
    auto Regions = std::move(Image.Memory);
    for (auto &Region : Regions) {
      const uint64_t Begin = Region.Address;
      const uint64_t End = Begin + Region.Bytes.size();
      if (End <= Address || Begin >= Address + Size) {
        Image.Memory.push_back(std::move(Region));
        continue;
      }
      if (Begin < Address)
        Image.Memory.push_back(
            {Begin, Region.Permissions, 0,
             std::vector<uint8_t>(Region.Bytes.begin(),
                                  Region.Bytes.begin() + (Address - Begin))});
      if (End > Address + Size)
        Image.Memory.push_back(
            {Address + Size, Region.Permissions, 0,
             std::vector<uint8_t>(Region.Bytes.begin() +
                                      (Address + Size - Begin),
                                  Region.Bytes.end())});
    }
  }

  void keepExtended() {
    EXPECT_EQ(registers().GPR[15], 0x1122334455667788ULL);
    EXPECT_EQ(Machine.Core.Core.Core.Core.Core.KernelGSBase,
              0xffffabcd12345678ULL);
  }
};

TEST_F(X86ModeSegmentTest, DescriptorWritesReplaceTheFullCacheInEveryMode) {
  for (Mode M : Modes)
    for (unsigned S : {0u, 2u, 3u, 4u, 5u})
      for (bool Operand : {false, true}) {
        SCOPED_TRACE((unsigned(M) * 6 + S) * 2 + Operand);
        start(M);
        const X64ContinuationSegment Expected{0x22000, UINT32_MAX, 32, 0xc093};
        setCache(S, {0xffffabcd00022000ULL, UINT32_MAX, 16, 0xc093});
        put(0x8020, descriptor(Expected.Base, Expected.Limit, 0xc092));
        registers().GPR[0] = 32;
        put(S == 3 ? 0x24000 : 0x2000, 32);
        auto Bytes = Operand
                         ? absolute(M, {0x8e}, S, 0x2000)
                         : std::vector<uint8_t>{0x8e, uint8_t(0xc0 | S << 3)};
        uint64_t Budget = 1;
        auto Got = run(M, Bytes, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        ASSERT_EQ(Got->Completions, 1u);
        EXPECT_EQ(cache(S), Expected);
        EXPECT_TRUE(physical(0x8020, GuestScalarWidth::I64) &
                    (uint64_t{1} << 40));
        EXPECT_FALSE(Machine.Core.Core.Unusable & (1u << UnusableBits[S]));
        EXPECT_EQ(Machine.SSBlocked, S == 2);
        keepExtended();
      }
}

TEST_F(X86ModeSegmentTest, SelectorReadsAndStackTransfersKeepTheirWidths) {
  for (Mode M : Modes)
    for (unsigned S = 0; S != 6; ++S)
      for (unsigned Bytes : {2u, 4u}) {
        SCOPED_TRACE((unsigned(M) * 6 + S) * 4 + Bytes);
        start(M);
        registers().GPR[0] = 0xabcdef12;
        std::vector<uint8_t> Code;
        if ((Bytes == 4) != code32(M))
          Code.push_back(0x66);
        Code.insert(Code.end(), {0x8c, uint8_t(0xc0 | S << 3)});
        const auto Before = cache(S);
        uint64_t Budget = 1;
        auto Got = run(M, Code, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Completions, 1u);
        EXPECT_EQ(uint32_t(registers().GPR[0]),
                  (Bytes == 4 ? 0 : 0xabcd0000) | Before.Selector);
        EXPECT_EQ(cache(S), Before);
        EXPECT_FALSE(Got->SegmentWritten);

        start(M);
        put(0xeff8, 0xaabbccddeeff0011ULL);
        Code.clear();
        if ((Bytes == 4) != code32(M))
          Code.push_back(0x66);
        if (S < 4)
          Code.push_back(6 + 8 * S);
        else
          Code.insert(Code.end(), {0x0f, uint8_t(S == 4 ? 0xa0 : 0xa8)});
        Budget = 1;
        Got = run(M, Code, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Completions, 1u);
        EXPECT_EQ(uint32_t(registers().GPR[4]), 0xf000 - Bytes);
        EXPECT_EQ(physical(0xf000 - Bytes, GuestScalarWidth::I16),
                  Before.Selector);
        if (Bytes == 4)
          EXPECT_EQ(physical(0xeffe, GuestScalarWidth::I16), 0xaabbu);
        EXPECT_FALSE(Got->SegmentWritten);
        if (S == 1)
          continue;
        const auto NextPC = registers().PC;
        Code.back() += 1;
        add(NextPC, M, Code);
        // Add the immutable POP bytes to the already-owned RAM image.
        ASSERT_EQ(Memory->physicalMemory().storeBytes(NextPC, Code).Status,
                  GuestMemoryAccessStatus::Completed);
        Budget = 1;
        Got = X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Completions, 1u);
        EXPECT_EQ(uint32_t(registers().GPR[4]), 0xf000u);
        EXPECT_EQ(cache(S).Selector, Before.Selector);
        EXPECT_EQ(Machine.SSBlocked, S == 2);
        keepExtended();
      }
}

TEST_F(X86ModeSegmentTest, FarPointersCommitBothDestinationsAndLSSHasNoShadow) {
  for (Mode M : Modes)
    for (unsigned S : {0u, 2u, 3u, 4u, 5u})
      for (unsigned Width : {2u, 4u}) {
        SCOPED_TRACE((unsigned(M) * 6 + S) * 4 + Width);
        start(M);
        put(0x8020, descriptor(0x22000, UINT32_MAX, 0xc092));
        put(0x2000, 0x89abcdef);
        RAM.Memory.front().Bytes[0x2000 + Width] = 32;
        RAM.Memory.front().Bytes[0x2001 + Width] = 0;
        registers().GPR[3] = 0x12345678;
        const std::vector<uint8_t> Opcode =
            S == 0   ? std::vector<uint8_t>{0xc4}
            : S == 3 ? std::vector<uint8_t>{0xc5}
                     : std::vector<uint8_t>{0x0f, uint8_t(S == 2   ? 0xb2
                                                          : S == 4 ? 0xb4
                                                                   : 0xb5)};
        uint64_t Budget = 1;
        auto Got = run(M, absolute(M, Opcode, 3, 0x2000, Width), Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Completions, 1u);
        EXPECT_EQ(uint32_t(registers().GPR[3]),
                  Width == 4 ? 0x89abcdef : 0x1234cdef);
        EXPECT_EQ(cache(S),
                  (X64ContinuationSegment{0x22000, UINT32_MAX, 32, 0xc093}));
        EXPECT_FALSE(Machine.SSBlocked);
        keepExtended();
      }
}

TEST_F(X86ModeSegmentTest, NullLoadsOwnOnlyTheirDeclaredFullBaseAction) {
  for (Mode M : Modes)
    for (unsigned S : {0u, 3u, 4u, 5u})
      for (auto Policy : {X86NullSegmentBasePolicy::Clear,
                          X86NullSegmentBasePolicy::Preserve}) {
        SCOPED_TRACE((unsigned(M) * 6 + S) * 2 + unsigned(Policy));
        start(M);
        const X64ContinuationSegment Before{0xffffabcd00022000ULL, 0x1234, 16,
                                            0x4093};
        setCache(S, Before);
        Options.Legacy.Segments.NullBasePolicy = Policy;
        registers().GPR[0] = 3;
        Machine.Core.Core.GDTR.Base = 0x800000000001ULL;
        uint64_t Budget = 1;
        auto Got = run(M, {0x8e, uint8_t(0xc0 | S << 3)}, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Completions, 1u);
        auto Expected = Before;
        Expected.Selector = 3;
        if (S >= 4 && Policy == X86NullSegmentBasePolicy::Clear)
          Expected.Base = 0;
        EXPECT_EQ(cache(S), Expected);
        EXPECT_TRUE(Machine.Core.Core.Unusable & (1u << UnusableBits[S]));
        keepExtended();
      }
}

TEST_F(X86ModeSegmentTest, MissingNullChoiceRefusesBeforeFetchOrEffects) {
  for (Mode M : Modes)
    for (unsigned S : {4u, 5u}) {
      start(M);
      Options.Legacy.Segments.NullBasePolicy.reset();
      registers().GPR[0] = 16;
      const auto Before = Machine;
      uint64_t Budget = 1;
      auto Got = run(M, {0x8e, uint8_t(0xc0 | S << 3)}, Budget);
      ASSERT_FALSE(bool(Got));
      llvm::consumeError(Got.takeError());
      EXPECT_EQ(Machine.Core.Core, Before.Core.Core);
      EXPECT_FALSE(Machine.Pending);
      EXPECT_EQ(Budget, 1u);
      EXPECT_EQ(Memory->controls(), Controls);
      EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
                RAM.Memory.front().Bytes);
    }
}

TEST_F(X86ModeSegmentTest, NonCodeCachesDoNotSelectExecutionModeFromLAndD) {
  for (Mode M : Modes)
    for (unsigned S : {0u, 2u, 3u, 4u, 5u})
      for (uint16_t A : {0xa093u, 0xe093u, 0xa09bu, 0xe09bu}) {
        if (S == 2 && (A & 8))
          continue;
        SCOPED_TRACE((unsigned(M) * 6 + S) * 0x10000 + A);
        start(M);
        put(0x8020, descriptor(0x22000, UINT32_MAX, A));
        registers().GPR[0] = 32;
        uint64_t Budget = 1;
        auto Got = run(M, {0x8e, uint8_t(0xc0 | S << 3)}, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Completions, 1u);
        EXPECT_EQ(cache(S).Attributes, A);
        EXPECT_EQ(
            llvm::cantFail(X86ModeExecution(*Memory, Options).mode(Machine)),
            M);
        keepExtended();
      }
}

TEST_F(X86ModeSegmentTest, DescriptorTablesUseTheCommittedAddressDomain) {
  for (Mode M : Modes)
    for (bool Local : {false, true}) {
      SCOPED_TRACE(unsigned(M) * 2 + Local);
      start(M);
      auto &V = Machine.Core.Core;
      put(0x10800, 0x11003);
      if (Local) {
        V.LDTR = {0xffff800000008000ULL, 0x3f, 48, 0x82};
        V.Unusable &= ~64;
      } else {
        V.GDTR.Base = 0xffff800000008000ULL;
      }
      put(0x8020, descriptor(0x22000, UINT32_MAX, 0xc092));
      registers().GPR[0] = Local ? 36 : 32;
      uint64_t Budget = 1;
      auto Got = run(M, {0x8e, 0xe0}, Budget);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      ASSERT_EQ(Got->Completions, 1u);
      EXPECT_EQ(registers().FSBase, 0x22000u);
      EXPECT_EQ(V.FS.Selector, Local ? 36 : 32);
      EXPECT_EQ(Local ? V.LDTR.Base : V.GDTR.Base, 0xffff800000008000ULL);
      keepExtended();
    }
}

TEST_F(X86ModeSegmentTest, AccessedWriteBackingRetainsTheCompleteInstruction) {
  for (Mode M : Modes) {
    start(M);
    put(0x8020, descriptor(0x22000, UINT32_MAX, 0xc092));
    registers().GPR[0] = registers().Undefined[0] | 32;
    setCache(4, {0xffffabcd00022000ULL, UINT32_MAX, 16, 0xc093});
    add(0x1000, M, {0x8e, 0xe0});
    const auto Bytes = RAM.Memory.front().Bytes;
    RAM.Memory = {
        {0, MemoryPermission::Read | MemoryPermission::Write, 0,
         std::vector<uint8_t>(Bytes.begin(), Bytes.begin() + 0x8020)},
        {0x8020, MemoryPermission::Read, 0,
         std::vector<uint8_t>(Bytes.begin() + 0x8020, Bytes.begin() + 0x8028)},
        {0x8028, MemoryPermission::Read | MemoryPermission::Write, 0,
         std::vector<uint8_t>(Bytes.begin() + 0x8028, Bytes.end())}};
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    const auto Before = Machine;
    uint64_t Budget = 1;
    X86ModeExecution E(*Memory, Options);
    auto Got = E.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
    ASSERT_TRUE(Machine.Pending);
    EXPECT_EQ(Machine.Core.Core, Before.Core.Core);
    EXPECT_FALSE(Got->SegmentWritten);
    EXPECT_EQ(Got->Attempts, 1u);
    EXPECT_EQ(Budget, 0u);
    Got = E.run(Machine, {}, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
    EXPECT_EQ(Got->Attempts, 0u);
    auto Changed = Machine;
    Changed.Core.Core.Core.Core.Core.Core.GSBase ^= uint64_t{1} << 32;
    auto Bad = E.run(Changed, {}, Budget);
    ASSERT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
    auto Snapshot = RAM;
    Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
    Snapshot.Memory[1].Permissions =
        MemoryPermission::Read | MemoryPermission::Write;
    auto Repaired = llvm::cantFail(
        X86PagedMemory::create(Snapshot, Features, Memory->controls()));
    auto Different = Options;
    Different.Legacy.Segments.NullBasePolicy.reset();
    Different.Legacy.Segments.Profile.reset();
    Different.Legacy.Segments.DescriptorCanonicality.reset();
    Got = X86ModeExecution(*Repaired, Different).run(Machine, {}, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    EXPECT_EQ(Got->Attempts, 0u);
    EXPECT_EQ(Got->Completions, 1u);
    EXPECT_TRUE(Got->SegmentWritten);
    EXPECT_FALSE(Machine.Pending);
    EXPECT_EQ(registers().FSBase, 0x22000u);
    EXPECT_EQ(registers().PC, 0x1002u);
    keepExtended();
  }
}

TEST_F(X86ModeSegmentTest, FarPointerRetainsOffsetAcrossAliasedPagingEffects) {
  for (Mode M : {Mode::Compatibility16, Mode::Compatibility32})
    for (unsigned Width : {2u, 4u}) {
      SCOPED_TRACE(unsigned(M) * 4 + Width);
      start(M);
      put(0x12000, 0x13003);
      for (unsigned I = 0; I != 64; ++I)
        put(0x13000 + I * 8, I * 4096 | 3);
      put(0x13100, 0x13003);
      const uint64_t Missing = Width == 4 ? 0x1800002000ULL : 0x180000ULL;
      put(0x13040, Missing | 3);
      const auto Code = absolute(M, {0x0f, 0xb4}, 0, 0x20040, Width);
      add(0x1000, M, Code);
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      uint64_t Budget = 1;
      auto Got =
          X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
      ASSERT_TRUE(Machine.Pending);
      EXPECT_EQ(physical(0x13040, GuestScalarWidth::I64), Missing | 0x23);
      EXPECT_EQ(registers().PC, 0x1000u);
      auto Snapshot = RAM;
      Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
      std::vector<uint8_t> Page(4096);
      const uint64_t D = descriptor(0x22000, UINT32_MAX, 0xc093);
      for (unsigned I = 0; I != 8; ++I)
        Page[0x18 + I] = D >> (I * 8);
      Snapshot.Memory.push_back(
          {Missing, MemoryPermission::Read | MemoryPermission::Write, 0, Page});
      auto Repaired = llvm::cantFail(
          X86PagedMemory::create(Snapshot, Features, Memory->controls()));
      Got = X86ModeExecution(*Repaired, Options).run(Machine, {}, Budget);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
      EXPECT_EQ(Got->Completions, 1u);
      EXPECT_EQ(Got->Attempts, 0u);
      EXPECT_EQ(uint32_t(registers().GPR[0]), Width == 4 ? 0x2003u : 3u);
      EXPECT_EQ(registers().FSBase, 0x22000u);
      EXPECT_FALSE(Machine.Pending);
    }
}

TEST_F(X86ModeSegmentTest,
       RetainedSegmentDoesNotFetchCodeChangedByItsPageWalk) {
  start(Mode::Compatibility32);
  put(0x12000, 0x13003);
  for (unsigned I = 0; I != 64; ++I)
    put(0x13000 + I * 8, I * 4096 | 3);
  put(0x13008, 0x13003);
  put(0x13100, 0x21003);
  registers().PC = 0x1040;
  const std::vector<uint8_t> Code{0x0f, 0xb4, 0x05, 0, 0, 2, 0};
  add(0x1040, Mode::Compatibility32, Code);
  std::copy(Code.begin(), Code.end(),
            RAM.Memory.front().Bytes.begin() + 0x13040);
  put(0x21000, uint64_t(16) << 32 | 0x12345678);
  constexpr uint64_t Missing = 0x2000005b000ULL;
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  uint64_t Budget = 1;
  auto Got = X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
  ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
  ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(Machine.Pending);
  EXPECT_EQ(physical(0x13040, GuestScalarWidth::I8), 0x2fu);
  auto Snapshot = RAM;
  Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
  std::vector<uint8_t> Page(4096);
  const uint64_t D = descriptor(0x22000, UINT32_MAX, 0xc093);
  for (unsigned I = 0; I != 8; ++I)
    Page[0x10 + I] = D >> (I * 8);
  Snapshot.Memory.push_back(
      {Missing, MemoryPermission::Read | MemoryPermission::Write, 0, Page});
  auto Repaired = llvm::cantFail(
      X86PagedMemory::create(Snapshot, Features, Memory->controls()));
  Got = X86ModeExecution(*Repaired, Options).run(Machine, {}, Budget);
  ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
  ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
  EXPECT_EQ(Got->Completions, 1u);
  EXPECT_EQ(Got->Attempts, 0u);
  EXPECT_EQ(uint32_t(registers().GPR[0]), 0x12345678u);
  EXPECT_EQ(registers().FSBase, 0x22000u);
  EXPECT_FALSE(Machine.Pending);
}

TEST_F(X86ModeSegmentTest, FirstOperandBackingRetainsAnEmptyPaidJournal) {
  for (Mode M : Modes) {
    start(M);
    put(0x2000, 16);
    const auto Code = absolute(M, {0x8e}, 4, 0x2000);
    add(0x1000, M, Code);
    const auto Bytes = RAM.Memory.front().Bytes;
    RAM.Memory = {{0, MemoryPermission::Read | MemoryPermission::Write, 0,
                   std::vector<uint8_t>(Bytes.begin(), Bytes.begin() + 0x2000)},
                  {0x2002, MemoryPermission::Read | MemoryPermission::Write, 0,
                   std::vector<uint8_t>(Bytes.begin() + 0x2002, Bytes.end())}};
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    uint64_t Budget = 1;
    auto Got = X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
    ASSERT_TRUE(Machine.Pending);
    EXPECT_EQ(Got->Attempts, 1u);
    EXPECT_EQ(Budget, 0u);
    Got = X86ModeExecution(*Memory, Options).run(Machine, {}, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    EXPECT_EQ(Got->Attempts, 0u);
    EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
    auto Snapshot = RAM;
    Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
    Snapshot.Memory.push_back(
        {0x2000, MemoryPermission::Read | MemoryPermission::Write, 0, {16, 0}});
    auto Repaired = llvm::cantFail(
        X86PagedMemory::create(Snapshot, Features, Memory->controls()));
    Got = X86ModeExecution(*Repaired, Options).run(Machine, {}, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    EXPECT_EQ(Got->Attempts, 0u);
    EXPECT_EQ(Got->Completions, 1u);
    EXPECT_FALSE(Machine.Pending);
    EXPECT_EQ(registers().PC, 0x1000 + Code.size());
    EXPECT_EQ(cache(4).Selector, 16);
  }
}

TEST_F(X86ModeSegmentTest, RetainedWatchpointAndSSShadowCompleteExactlyOnce) {
  for (Mode M : Modes)
    for (unsigned S : {2u, 4u})
      for (bool High : {false, true}) {
        SCOPED_TRACE((unsigned(M) * 6 + S) * 2 + High);
        start(M);
        put(0x2000, 32);
        put(0x8020, descriptor(0, UINT32_MAX, 0xc092));
        Machine.Debug.emplace();
        Machine.Debug->DR[0] = (High ? uint64_t{1} << 32 : 0) | 0x2000;
        Machine.Debug->DR7 = 0x70401;
        Options.Legacy.Debug = X86InstructionDebugProfile::Watchpoints;
        Options.Long.DebugProfile = X86InstructionDebugProfile::Watchpoints;
        const auto Code = absolute(M, {0x8e}, S, 0x2000);
        const auto Next = add(0x1000, M, Code);
        add(Next, M, {0x90});
        const auto Bytes = RAM.Memory.front().Bytes;
        RAM.Memory = {
            {0, MemoryPermission::Read | MemoryPermission::Write, 0,
             std::vector<uint8_t>(Bytes.begin(), Bytes.begin() + 0x8020)},
            {0x8020, MemoryPermission::Read, 0,
             std::vector<uint8_t>(Bytes.begin() + 0x8020,
                                  Bytes.begin() + 0x8028)},
            {0x8028, MemoryPermission::Read | MemoryPermission::Write, 0,
             std::vector<uint8_t>(Bytes.begin() + 0x8028, Bytes.end())}};
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        uint64_t Budget = 1;
        auto Got =
            X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
        ASSERT_TRUE(Machine.Pending);
        EXPECT_EQ(Machine.Debug->DR6 & 1, 0u);
        EXPECT_FALSE(Machine.SSBlocked);
        auto Snapshot = RAM;
        Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
        Snapshot.Memory[1].Permissions =
            MemoryPermission::Read | MemoryPermission::Write;
        auto Repaired = llvm::cantFail(
            X86PagedMemory::create(Snapshot, Features, Memory->controls()));
        Got = X86ModeExecution(*Repaired, Options).run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        EXPECT_EQ(Got->Attempts, 0u);
        EXPECT_EQ(Got->Completions, 1u);
        EXPECT_EQ(cache(S).Selector, 32);
        ASSERT_TRUE(Got->SegmentWritten);
        EXPECT_EQ(unsigned(Got->SegmentWritten->Register), S);
        if (S == 2) {
          EXPECT_TRUE(Machine.SSBlocked);
          EXPECT_EQ(registers().PC, Next);
          EXPECT_EQ(Machine.Debug->PendingData, High ? 0 : 1);
          Budget = 1;
          Got = X86ModeExecution(*Repaired, Options)
                    .run(Machine, Program, Budget);
          ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
          ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
          EXPECT_EQ(Got->Attempts, 1u);
          EXPECT_EQ(Got->Completions, 1u);
          EXPECT_FALSE(Got->SegmentWritten);
        }
        EXPECT_EQ(Got->EventAccepted, !High);
        EXPECT_EQ(bool(Machine.Debug->DR6 & 1), !High);
        EXPECT_EQ(Machine.Debug->PendingData, 0);
        EXPECT_FALSE(Machine.SSBlocked);
        if (!High)
          EXPECT_EQ(Got->DeliveredVector, 1);
        EXPECT_FALSE(Machine.Pending);
      }
}

TEST_F(X86ModeSegmentTest,
       CompletedSegmentReceiptIsNotRepeatedAcrossEventBacking) {
  for (Mode M : Modes) {
    SCOPED_TRACE(unsigned(M));
    start(M);
    put(0x2000, 32);
    put(0x8020, descriptor(0x22000, UINT32_MAX, 0xc093));
    Machine.Debug.emplace();
    Machine.Debug->DR[0] = 0x2000;
    Machine.Debug->DR7 = 0x70401;
    Options.Legacy.Debug = X86InstructionDebugProfile::Watchpoints;
    Options.Long.DebugProfile = X86InstructionDebugProfile::Watchpoints;
    add(0x1000, M, absolute(M, {0x8e}, 4, 0x2000));
    const unsigned EntryBytes = Controls.EFER & 0x400 ? 16 : 8;
    const uint64_t Entry = 0x9000 + EntryBytes;
    const auto Bytes = RAM.Memory.front().Bytes;
    omitBacking(RAM, Entry, EntryBytes);
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    uint64_t Budget = 1;
    auto Got = X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
    EXPECT_EQ(Got->Attempts, 1u);
    EXPECT_EQ(Got->Completions, 1u);
    EXPECT_TRUE(Got->InstructionCompleted);
    EXPECT_TRUE(Got->EventAccepted);
    ASSERT_TRUE(Got->SegmentWritten);
    EXPECT_EQ(Got->SegmentWritten->Register, X86SegmentRegister::FS);
    EXPECT_EQ(registers().FSBase, 0x22000u);
    ASSERT_TRUE(Machine.Pending);
    EXPECT_EQ(Budget, 0u);
    auto Snapshot = RAM;
    Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
    Snapshot.Memory.push_back(
        {Entry, MemoryPermission::Read | MemoryPermission::Write, 0,
         std::vector<uint8_t>(Bytes.begin() + Entry,
                              Bytes.begin() + Entry + EntryBytes)});
    auto Repaired = llvm::cantFail(
        X86PagedMemory::create(Snapshot, Features, Memory->controls()));
    Got = X86ModeExecution(*Repaired, Options).run(Machine, {}, Budget);
    ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
    ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
    EXPECT_EQ(Got->Attempts, 0u);
    EXPECT_EQ(Got->Completions, 0u);
    EXPECT_FALSE(Got->InstructionCompleted);
    EXPECT_FALSE(Got->SegmentWritten);
    EXPECT_FALSE(Got->EventAccepted);
    EXPECT_EQ(Got->DeliveredVector, 1);
    EXPECT_EQ(registers().PC, 0x4010u);
    EXPECT_EQ(registers().FSBase, 0x22000u);
    EXPECT_FALSE(Machine.Pending);
  }
}

TEST_F(X86ModeSegmentTest, ResumedPointerFaultRetiresBeforeEventBacking) {
  for (Mode M : Modes)
    for (unsigned S : {2u, 4u})
      for (bool NotPresent : {false, true}) {
        SCOPED_TRACE((unsigned(M) * 6 + S) * 2 + NotPresent);
        start(M);
        const unsigned Vector = NotPresent ? (S == 2 ? 12 : 11) : 13;
        const auto Before = cache(S);
        const auto BeforeGPR = registers().GPR[0];
        put(0x2000, uint64_t(32) << 32 | 0x12345678);
        put(0x8020,
            descriptor(0x22000, UINT32_MAX, NotPresent ? 0xc013 : 0xc082));
        add(0x1000, M,
            absolute(M, {0x0f, uint8_t(S == 2 ? 0xb2 : 0xb4)}, 0, 0x2000, 4));
        const unsigned EntryBytes = Controls.EFER & 0x400 ? 16 : 8;
        const uint64_t Entry = 0x9000 + EntryBytes * Vector;
        const auto Bytes = RAM.Memory.front().Bytes;
        omitBacking(RAM, 0x8020, 8);
        omitBacking(RAM, Entry, EntryBytes);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        uint64_t Budget = 1;
        auto Got =
            X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
        EXPECT_EQ(Got->Attempts, 1u);
        EXPECT_EQ(Got->Completions, 0u);
        EXPECT_FALSE(Got->EventAccepted);
        EXPECT_FALSE(Got->SegmentWritten);
        EXPECT_EQ(cache(S), Before);
        ASSERT_TRUE(Machine.Pending);
        auto Snapshot = RAM;
        Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
        Snapshot.Memory.push_back(
            {0x8020, MemoryPermission::Read | MemoryPermission::Write, 0,
             std::vector<uint8_t>(Bytes.begin() + 0x8020,
                                  Bytes.begin() + 0x8028)});
        auto DescriptorReady = llvm::cantFail(
            X86PagedMemory::create(Snapshot, Features, Memory->controls()));
        Got = X86ModeExecution(*DescriptorReady, Options)
                  .run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
        ASSERT_TRUE(Got->Exception);
        EXPECT_EQ(Got->Exception->Vector, Vector);
        EXPECT_EQ(Got->Exception->ErrorCode, 32u);
        EXPECT_EQ(Got->Attempts, 0u);
        EXPECT_EQ(Got->Completions, 0u);
        EXPECT_TRUE(Got->EventAccepted);
        EXPECT_FALSE(Got->SegmentWritten);
        EXPECT_EQ(cache(S), Before);
        EXPECT_EQ(registers().GPR[0], BeforeGPR);
        EXPECT_FALSE(Machine.SSBlocked);
        ASSERT_TRUE(Machine.Pending);
        Snapshot.Memory =
            DescriptorReady->physicalMemory().snapshotMemoryRegions();
        // Event resumption must not revisit the retired descriptor/operand
        // prefix.
        omitBacking(Snapshot, 0x8020, 8);
        omitBacking(Snapshot, 0x2000, 6);
        Snapshot.Memory.push_back(
            {Entry, MemoryPermission::Read | MemoryPermission::Write, 0,
             std::vector<uint8_t>(Bytes.begin() + Entry,
                                  Bytes.begin() + Entry + EntryBytes)});
        auto EventReady = llvm::cantFail(X86PagedMemory::create(
            Snapshot, Features, DescriptorReady->controls()));
        Got = X86ModeExecution(*EventReady, Options).run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        EXPECT_EQ(Got->Status, X86LegacyExecutionStatus::EventDelivered);
        EXPECT_EQ(Got->DeliveredVector, Vector);
        EXPECT_EQ(Got->Attempts, 0u);
        EXPECT_EQ(Got->Completions, 0u);
        EXPECT_FALSE(Got->EventAccepted);
        EXPECT_FALSE(Got->SegmentWritten);
        if (S != 2 || !(Controls.EFER & 0x400))
          EXPECT_EQ(cache(S), Before);
        EXPECT_EQ(registers().GPR[0], BeforeGPR);
        EXPECT_EQ(registers().PC, 0x4000 + Vector * 16);
        EXPECT_FALSE(Machine.SSBlocked);
        EXPECT_FALSE(Machine.Pending);
      }
}

TEST_F(X86ModeSegmentTest, DescriptorFaultProfileIsRequiredBeforeFetch) {
  for (Mode M : {Mode::Compatibility16, Mode::Compatibility32})
    for (bool Invalid : {false, true})
      for (const std::vector<uint8_t> Code :
           {std::vector<uint8_t>{0x8e, 0xd0},
            {0x8e, 0xe0},
            {0x0f, 0xa9},
            absolute(M, {0x0f, 0xb4}, 0, 0x2000, 4)}) {
        start(M);
        Options.Legacy.Segments.DescriptorCanonicality.reset();
        if (Invalid)
          Options.Legacy.Segments.DescriptorCanonicality =
              static_cast<X86DescriptorCanonicalityProfile>(255);
        const auto Before = Machine;
        uint64_t Budget = 1;
        auto Got = run(M, Code, Budget);
        ASSERT_FALSE(bool(Got));
        EXPECT_NE(
            llvm::toString(Got.takeError()).find("descriptor fault profile"),
            std::string::npos);
        EXPECT_EQ(Budget, 1u);
        EXPECT_EQ(Machine.Core.Core, Before.Core.Core);
        EXPECT_FALSE(Machine.Pending);
        EXPECT_EQ(Memory->controls(), Controls);
        EXPECT_EQ(
            Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAM.Memory.front().Bytes);
      }
}

TEST_F(X86ModeSegmentTest,
       DescriptorFaultProfilesStayBoundAcrossOperandBacking) {
  for (Mode M : {Mode::Compatibility16, Mode::Compatibility32})
    for (auto Profile : {X86DescriptorCanonicalityProfile::SelectorForStart,
                         X86DescriptorCanonicalityProfile::SelectorForRange})
      for (unsigned Case = 0; Case != 4; ++Case) {
        SCOPED_TRACE((unsigned(M) * 2 + unsigned(Profile)) * 4 + Case);
        start(M);
        Options.Legacy.Segments.DescriptorCanonicality = Profile;
        const uint64_t Address = Case == 0  ? 0x0000800000000000ULL
                                 : Case < 3 ? 0x00007ffffffffffcULL
                                            : 0xffff800000008020ULL;
        Machine.Core.Core.LDTR = {Address - 0x20, 0x3f, 0x30, 0x82};
        Machine.Core.Core.Unusable &= ~64;
        constexpr uint64_t OldCR2 = 0x1122334455667788ULL;
        Machine.Core.Core.Core.Core.CR2 = OldCR2;
        put(0x107f8, 0x14003);
        put(0x14ff8, 0x15003);
        put(0x15ff8, 0x16003);
        put(0x16ff8, Case == 2 ? 0x17002 : 0x17003);
        put(0x2000, uint64_t(0x24) << 32 | 0x12345678);
        const auto Before = cache(4);
        const auto OldGPR = registers().GPR[0];
        add(0x1000, M, absolute(M, {0x0f, 0xb4}, 0, 0x2000, 4));
        const auto Bytes = RAM.Memory.front().Bytes;
        omitBacking(RAM, 0x2000, 4);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        uint64_t Budget = 1;
        auto Got =
            X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::BackingFault);
        EXPECT_EQ(Got->Attempts, 1u);
        EXPECT_EQ(Got->Completions, 0u);
        EXPECT_FALSE(Got->SegmentWritten);
        ASSERT_TRUE(Machine.Pending);
        auto Snapshot = RAM;
        Snapshot.Memory = Memory->physicalMemory().snapshotMemoryRegions();
        Snapshot.Memory.push_back(
            {0x2000, MemoryPermission::Read | MemoryPermission::Write, 0,
             std::vector<uint8_t>(Bytes.begin() + 0x2000,
                                  Bytes.begin() + 0x2004)});
        auto Repaired = llvm::cantFail(
            X86PagedMemory::create(Snapshot, Features, Memory->controls()));
        auto Different = Options;
        Different.Legacy.Segments.DescriptorCanonicality.reset();
        Got = X86ModeExecution(*Repaired, Different).run(Machine, {}, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        ASSERT_TRUE(Got->Exception);
        EXPECT_EQ(Got->Exception->Vector, Case == 3 ? 14 : 13);
        const bool SelectorError =
            Case == 0 ||
            (Case < 3 &&
             Profile == X86DescriptorCanonicalityProfile::SelectorForRange);
        EXPECT_EQ(Got->Exception->ErrorCode, SelectorError ? 0x24u : 0u);
        EXPECT_EQ(Machine.Core.Core.Core.Core.CR2,
                  Case == 3 ? Address : OldCR2);
        EXPECT_EQ(cache(4), Before);
        EXPECT_EQ(registers().GPR[0], OldGPR);
        EXPECT_EQ(Got->Attempts, 0u);
        EXPECT_EQ(Got->Completions, 0u);
        EXPECT_FALSE(Got->SegmentWritten);
        EXPECT_FALSE(Machine.Pending);
        EXPECT_EQ(Repaired->physicalMemory()
                      .loadScalar(0x107f8, GuestScalarWidth::I64)
                      .Value,
                  0x14003u);
        EXPECT_EQ(Repaired->physicalMemory()
                      .loadScalar(0x16ff8, GuestScalarWidth::I64)
                      .Value,
                  Case == 2 ? 0x17002u : 0x17003u);
      }
}
