//===- X86LegacyExchangeTests.inc - Protected exchanges and LOCK ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Register overlaps, failed writeback, paired results and atomic admission.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, ExchangeEveryRegisterLaneAndAccumulatorForm) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u})
      for (unsigned Destination = 0; Destination != 8; ++Destination)
        for (unsigned Source = 0; Source != 8; ++Source) {
          S.EIP = 0x1000;
          S.EFlags = 0x10ed7;
          S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
          for (unsigned R = 0; R != 8; ++R)
            S.Core.GPR[R] = 0x12345678 + R * 0x11111111;
          auto Expected = S.Core.GPR;
          const unsigned D = Width == 1 ? Destination % 4 : Destination;
          const unsigned T = Width == 1 ? Source % 4 : Source;
          const unsigned DS = Width == 1 && Destination >= 4 ? 8 : 0;
          const unsigned TS = Width == 1 && Source >= 4 ? 8 : 0;
          const uint32_t Mask = UINT32_MAX >> (32 - Width * 8);
          const uint32_t A = (Expected[D] >> DS) & Mask;
          const uint32_t B = (Expected[T] >> TS) & Mask;
          Expected[D] = (Expected[D] & ~(Mask << DS)) | (B << DS);
          Expected[T] = (Expected[T] & ~(Mask << TS)) | (A << TS);
          std::vector<uint8_t> Bytes{0x64, 0x67};
          if (Width != 1 && (Width == 4) != Code32)
            Bytes.push_back(0x66);
          Bytes.insert(Bytes.end(),
                       {uint8_t(Width == 1 ? 0x86 : 0x87),
                        uint8_t(0xc0 | Source << 3 | Destination)});
          const auto Original = S.Core.GPR;
          ASSERT_TRUE(run(Bytes).InstructionCompleted);
          EXPECT_EQ(S.Core.GPR, Expected);
          EXPECT_EQ(S.EFlags, 0xed7u);
          if (Destination == 0 && Width != 1) {
            S.Core.GPR = Original;
            Bytes.erase(Bytes.end() - 2, Bytes.end());
            Bytes.push_back(0x90 | Source);
            ASSERT_TRUE(run(Bytes).InstructionCompleted);
            EXPECT_EQ(S.Core.GPR, Expected);
          }
        }
}

TEST_F(X86LegacyExecutionTest, ExchangeAddAliasedSourceAndAddress) {
  S.Core.GPR[0] = 0x12347f80;
  ASSERT_TRUE(run({0x0f, 0xc0, 0xe0}).InstructionCompleted); // XADD AL, AH.
  EXPECT_EQ(S.Core.GPR[0], 0x123480ffu);
  EXPECT_EQ(S.EFlags & 0x8d5, 0x84u);
  S.Core.GPR[1] = 0x80000000;
  ASSERT_TRUE(run({0x0f, 0xc1, 0xc9}).InstructionCompleted); // Same register.
  EXPECT_EQ(S.Core.GPR[1], 0u);
  EXPECT_EQ(S.EFlags & 0x8d5, 0x845u);
  S.Core.GPR[3] = 0xc100;
  put(0xc100, 7, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x0f, 0xc1, 0x1b}).InstructionCompleted); // XADD [EBX], EBX.
  EXPECT_EQ(S.Core.GPR[3], 7u);
  EXPECT_EQ(get(0xc100), 0xc107u);
}

TEST_F(X86LegacyExecutionTest, CompareExchangeCapturesAccumulatorAndHighBytes) {
  S.Core.GPR[0] = 0x12348080;
  S.Core.GPR[1] = 0xabcdef7f;
  ASSERT_TRUE(run({0x0f, 0xb0, 0xcc}).InstructionCompleted); // CMPXCHG AH, CL.
  EXPECT_EQ(S.Core.GPR[0], 0x12347f80u);
  EXPECT_EQ(S.EFlags & 0x8d5, 0x44u);
  ASSERT_TRUE(run({0x0f, 0xb0, 0xcc}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0x12347f7fu);
  EXPECT_EQ(S.EFlags & 0x8d5, 0x810u); // 0x80 - 0x7f = 1, OF and AF.
  S.Core.GPR[0] = 0xc100;
  S.Core.GPR[1] = 0x76543210;
  put(0xc100, 0x12345678, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x0f, 0xb1, 0x08}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  EXPECT_EQ(get(0xc100), 0x12345678u);
  EXPECT_EQ(get(0x11030) & 0x60, 0x60u); // Failure still dirties the operand.
}

TEST_F(X86LegacyExecutionTest, PairCompareExchangeUsesWholeOperandAndOnlyZF) {
  for (bool Equal : {false, true})
    for (bool Override : {false, true}) {
      S.EIP = 0x1000;
      S.EFlags = 0x10ed7;
      S.Core.GPR[0] = 0xc100; // Also the address base; snapshot before failure.
      S.Core.GPR[2] = 0x12345678;
      S.Core.GPR[1] = 0xaabbccdd;
      S.Core.GPR[3] = 0x87654321;
      const uint64_t Original =
          Equal ? 0x123456780000c100ULL : 0x8877665544332211ULL;
      put(0xc100, Original);
      std::vector<uint8_t> Bytes;
      if (Override)
        Bytes.push_back(
            0x66); // Ignored: CMPXCHG8B always accesses eight bytes.
      Bytes.insert(Bytes.end(), {0x0f, 0xc7, 0x08});
      ASSERT_TRUE(run(Bytes).InstructionCompleted);
      EXPECT_EQ(get(0xc100, GuestScalarWidth::I64),
                Equal ? 0xaabbccdd87654321ULL : Original);
      EXPECT_EQ(S.Core.GPR[0], Equal ? 0xc100u : 0x44332211u);
      EXPECT_EQ(S.Core.GPR[2], Equal ? 0x12345678u : 0x88776655u);
      EXPECT_EQ(S.EFlags, Equal ? 0xed7u : 0xe97u);
    }
}

TEST_F(X86LegacyExecutionTest, FailedCompareExchangeStillRequiresWriteAccess) {
  O.Atomics = X86LegacyAtomicProfile::ExclusiveRAM;
  for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0x87, 0x0b},
                                           {0x0f, 0xc1, 0x0b},
                                           {0x0f, 0xb1, 0x0b},
                                           {0xf0, 0x0f, 0xb1, 0x0b},
                                           {0x0f, 0xc7, 0x0b},
                                           {0xf0, 0x0f, 0xc7, 0x0b}}) {
    SetUp();
    S.EFlags = 0xed7;
    S.Core.GPR[0] = 0x12345678;
    S.Core.GPR[1] = 0xabcdef01;
    S.Core.GPR[2] = 0x87654321;
    S.Core.GPR[3] = 0xc100;
    put(0xc100, 0xffffeeee11112222ULL);
    put(0x11030, 0xc005, GuestScalarWidth::I32);
    const auto Original = S.Core.GPR;
    const auto R = run(Bytes);
    EXPECT_EQ(R.DeliveredVector, 14);
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(S.CR2, 0xc100u);
    EXPECT_EQ(get(S.Core.GPR[4]), 7u);
    EXPECT_EQ(get(S.Core.GPR[4] + 12) & 0x8d5, 0x8d5u);
    for (unsigned I : {0u, 1u, 2u, 3u})
      EXPECT_EQ(S.Core.GPR[I], Original[I]);
    EXPECT_EQ(get(0xc100, GuestScalarWidth::I64), 0xffffeeee11112222ULL);
  }
}

TEST_F(X86LegacyExecutionTest, PairCompareExchangePreflightsEntireStore) {
  for (bool Locked : {false, true}) {
    SetUp();
    O.Atomics = X86LegacyAtomicProfile::ExclusiveRAM;
    put(0x5804, 0xf000, GuestScalarWidth::I32);
    S.Core.GPR[0] = 0x44332211;
    S.Core.GPR[2] = 0x88776655;
    S.Core.GPR[3] = 0xcffc;
    S.EFlags = 0xed7;
    put(0xcffc, 0x8877665544332211ULL);
    put(0x11034, 0xd006, GuestScalarWidth::I32);
    std::vector<uint8_t> Bytes{0x0f, 0xc7, 0x0b};
    if (Locked)
      Bytes.insert(Bytes.begin(), 0xf0);
    const auto R = run(Bytes);
    EXPECT_EQ(R.DeliveredVector, 14);
    EXPECT_EQ(S.CR2, 0xd000u);
    EXPECT_EQ(get(S.Core.GPR[4]), 6u);
    EXPECT_EQ(S.Core.GPR[0], 0x44332211u);
    EXPECT_EQ(S.Core.GPR[2], 0x88776655u);
    EXPECT_EQ(get(0xcffc, GuestScalarWidth::I64), 0x8877665544332211ULL);
  }
}

TEST_F(X86LegacyExecutionTest, CompareExchangeReadsPreparedPageTableEffects) {
  O.Atomics = X86LegacyAtomicProfile::ExclusiveRAM;
  S.Core.GPR[3] = 0x11044; // This PTE maps its own page-table page.
  S.Core.GPR[0] = 0x11067; // A and D are visible before the operand read.
  S.Core.GPR[1] = 0x13007;
  ASSERT_EQ(get(0x11044), 0x11007u);
  ASSERT_TRUE(run({0xf0, 0x0f, 0xb1, 0x0b}).InstructionCompleted);
  EXPECT_EQ(get(0x11044), 0x13007u);
  EXPECT_EQ(S.Core.GPR[0], 0x11067u);
  EXPECT_EQ(S.EFlags & 0x8d5, 0x44u);
}

TEST_F(X86LegacyExecutionTest, AtomicBindingsRefuseBeforeOriginalFetch) {
  for (bool Invalid : {false, true}) {
    if (Invalid)
      O.Atomics = static_cast<X86LegacyAtomicProfile>(255);
    for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0x87, 0x0b},
                                             {0xf0, 0x83, 0x03, 1},
                                             {0xf0, 0x0f, 0xab, 0x0b},
                                             {0xf0, 0x0f, 0xc7, 0x0b}}) {
      auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
      ASSERT_FALSE(bool(R));
      EXPECT_NE(llvm::toString(R.takeError()).find("exclusive-RAM"),
                std::string::npos);
      EXPECT_EQ(S.EIP, 0x1000u);
      EXPECT_EQ(get(0x11004) & 0x20, 0u);
    }
  }
}

TEST_F(X86LegacyExecutionTest, LockAndExchangeMalformedFormsRefuse) {
  O.Atomics = X86LegacyAtomicProfile::ExclusiveRAM;
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0xf0, 0x87, 0xc1},
        {0xf0, 0x91},
        {0xf0, 0x03, 0x03},
        {0xf0, 0x83, 0x3b, 1},
        {0xf0, 0x0f, 0xa3, 0x0b},
        {0xf0, 0xd1, 0x23},
        {0xf2, 0xf0, 0x87, 0x0b},
        {0xf3, 0x0f, 0xc1, 0x0b},
        {0xf0, 0xf0, 0x87, 0x0b},
        {0x0f, 0xc7, 0xc8},
        {0x0f, 0xc7, 0x03},
        {0x0f, 0xc7},
        {0x0f, 0xb0},
        {0x0f, 0xc1},
        {0x87},
        {0x91, 0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}

TEST_F(X86LegacyExecutionTest, PlainNOPRetainsUnboundInterface) {
  O.Scalars.reset();
  O.Segments.Profile.reset();
  O.Segments.AlignmentPriority.reset();
  EXPECT_TRUE(run({0x90}).InstructionCompleted);
}
