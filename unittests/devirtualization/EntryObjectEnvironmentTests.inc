//===- EntryObjectEnvironmentTests.inc - Bounded symbolic entry objects --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent native fixtures for object/frame domain recovery.
/// Included after the raw code and machine-exit fixture helpers.
///
//===----------------------------------------------------------------------===//

SpecializationOptions entryObjectOptions() {
  SpecializationOptions O;
  O.ExplicitMachineState = O.NormalNonfaultingExecution = O.X64CetDisabled =
      true;
  O.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 8};
  O.EntryFrameExcludedObjects = {{{x86reg::RCX, 8}, 0, 32}};
  O.ProveStoreFrameSeparation = true;
  O.RequireRestoredFrameAtReturn = true;
  // Retain the explicit branch boundary to exercise fresh-root projection.
  O.MaxChainedTransfers = 0;
  return O;
}

std::vector<uint8_t> entryObjectCode() {
  return {0x48, 0x89, 0x64, 0x24, 0xf0, // mov [rsp-16],rsp
          0x48, 0x89, 0x4c, 0x24, 0xe8, // mov [rsp-24],rcx
          0x48, 0x8d, 0x59, 0x08,       // lea rbx,[rcx+8]
          0xeb, 0x00,                   // jmp next (a projection boundary)
          0x48, 0xc7, 0xc1, 0,    0,    0, 0, // mov rcx,0
          0x48, 0x8b, 0x4c, 0x24, 0xe8,       // mov rcx,[rsp-24]
          0x48, 0x8d, 0x49, 0x10,             // lea rcx,[rcx+16]
          0x48, 0x89, 0x13,                   // mov [rbx],rdx
          0x48, 0x89, 0x11,                   // mov [rcx],rdx
          0x48, 0x8b, 0x64, 0x24, 0xf0,       // mov rsp,[rsp-16]
          0xb8, 42,   0,    0,    0,          // mov eax,42
          0xc3};                              // ret
}

TEST_F(PEFixedImageTest, EntryObjectsPreserveSymbolicCopiesAndSpills) {
  auto Loaded = load(machineExitFixture(entryObjectCode()));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto O = entryObjectOptions();
  ASSERT_FALSE(O.ExternalStoresDisjointEntryFrame);
  ASSERT_FALSE(O.ExternalStoresPreserveEntryReturnSlot);
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_TRUE(R.RawEnvironment);
  ASSERT_GE(R.Residual.Blocks.size(), 2U);
  size_t Stores = 0;
  for (const auto &B : R.Residual.Blocks)
    for (const auto &Op : B.Ops)
      Stores += Op.Opcode == NdOp::STORE;
  EXPECT_EQ(Stores, 4U);
  const std::pair<uint64_t, uint64_t> Cases[] = {
      {0x1000, 0x2000},
      {0x1000, 0x1008},
      {0x1000, 0xfc0},
      {0x1000, 0},
      {0x1000, UINT64_MAX - 31},
      {UINT64_C(0xffff900000001000), UINT64_C(0xffff900000001008)}};
  for (const auto &[Stack, Object] : Cases) {
    SCOPED_TRACE(Stack);
    SCOPED_TRACE(Object);
    auto Actual = machineExitState(Stack);
    Actual.write(NdVar::reg(x86reg::RCX, 8), Object);
    for (unsigned I = 0; I < 32; ++I)
      Actual.Memory[Object + I] = uint8_t(I ^ 0x93);
    auto Expected = Actual;
    const auto Data = Actual.read(NdVar::reg(x86reg::RDX, 8));
    Expected.memory(Stack - 16, Stack, 8);
    Expected.memory(Stack - 24, Object, 8);
    Expected.memory(Object + 8, Data, 8);
    Expected.memory(Object + 16, Data, 8);
    Expected.write(NdVar::reg(x86reg::RBX, 8), Object + 8);
    Expected.write(NdVar::reg(x86reg::RCX, 8), Object + 16);
    Expected.write(NdVar::reg(x86reg::RAX, 8), 42);
    ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
    std::erase_if(Actual.Registers, [](const auto &V) {
      return V.first.first == VnodeSpace::TEMP;
    });
    EXPECT_EQ(Actual.Registers, Expected.Registers);
    EXPECT_EQ(Actual.Memory, Expected.Memory);
  }
  for (bool Declaration : {false, true}) {
    auto Missing = O;
    if (Declaration)
      Missing.ProveStoreFrameSeparation = false;
    else
      Missing.EntryFrameExcludedObjects.clear();
    const auto Refused =
        specializeBinaryInterpreter(*Loaded, Base + 0x1000, Missing);
    EXPECT_FALSE(Refused.complete());
    EXPECT_TRUE(Refused.Residual.Blocks.empty());
  }
  auto Exact = O;
  Exact.MaxOperations = R.EvaluatedOperations;
  EXPECT_TRUE(
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, Exact).complete());
  ASSERT_GT(Exact.MaxOperations, 0U);
  --Exact.MaxOperations;
  const auto Short = specializeBinaryInterpreter(*Loaded, Base + 0x1000, Exact);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
}

TEST_F(PEFixedImageTest, EntryObjectsProveCellsBeyondDeclaredFrameSeparately) {
  for (bool SavedPointer : {false, true}) {
    for (uint8_t Bias : {0U, 7U, 8U, 240U}) {
      SCOPED_TRACE(SavedPointer);
      SCOPED_TRACE(unsigned(Bias));
      // The cell at +8 is outside the declared frame. Its current value needs
      // a separate complete-footprint proof even after whole-frame separation.
      std::vector<uint8_t> Code =
          SavedPointer
              ? std::vector<uint8_t>{0x48, 0x89, 0x64, 0x24, 8}
              : std::vector<uint8_t>{0x48, 0xc7, 0x44, 0x24, 8, 42, 0, 0, 0};
      const uint8_t Address[] = {
          0x49, 0x89, 0xc8,                                   // mov r8,rcx
          0x49, 0xf7, 0xd0,                                   // not r8
          0x49, 0x89, 0xc9,                                   // mov r9,rcx
          0x49, 0x81, 0xf1, Bias,           0,    0,    0,    // xor r9,Bias
          0x4d, 0x89, 0xca,                                   // mov r10,r9
          0x49, 0xf7, 0xd2,                                   // not r10
          0x49, 0x81, 0xca, uint8_t(~Bias), 0xff, 0xff, 0xff, // or r10,~Bias
          0x49, 0xf7, 0xd2,                                   // not r10
          0x4d, 0x89, 0xcb,                                   // mov r11,r9
          0x4d, 0x09, 0xd3,                                   // or r11,r10
          0x4d, 0x89, 0xdc,                                   // mov r12,r11
          0x49, 0xf7, 0xd4,                                   // not r12
          0x4d, 0x09, 0xc4,                                   // or r12,r8
          0x49, 0xf7, 0xd4,                                   // not r12
          0x49, 0x89, 0xcd,                                   // mov r13,rcx
          0x4d, 0x31, 0xdd,                                   // xor r13,r11
          0x4d, 0x31, 0xe5,                                   // xor r13,r12
          0x4d, 0x89, 0xce,                                   // mov r14,r9
          0x4d, 0x09, 0xd6,                                   // or r14,r10
          0x4d, 0x09, 0xee,                                   // or r14,r13
          0x49, 0xf7, 0xd6,                                   // not r14
          0x4d, 0x31, 0xc6,                                   // xor r14,r8
          0x4d, 0x09, 0xc6,                                   // or r14,r8
          0x49, 0xf7, 0xd6,                                   // not r14
          0x4d, 0x8d, 0xb6, Bias,           0,    0,    0, // lea r14,[r14+Bias]
          0x49, 0x89, 0x16};                               // mov [r14],rdx
      Code.insert(Code.end(), std::begin(Address), std::end(Address));
      if (SavedPointer)
        Code.insert(Code.end(),
                    {0x48, 0x8b, 0x64, 0x24, 8, 0xb8, 42, 0, 0, 0, 0xc3});
      else
        Code.insert(Code.end(), {0x48, 0x8b, 0x44, 0x24, 8, 0xc3});
      auto Loaded = load(machineExitFixture(Code));
      ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
      auto O = entryObjectOptions();
      O.EntryFrameBounds->Begin = -4096;
      O.EntryFrameExcludedObjects[0].End = 256;
      O.ProveStoreReturnSlotSeparation = true;
      const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
      if (SavedPointer && Bias < 8) {
        EXPECT_FALSE(R.complete());
        EXPECT_TRUE(R.Residual.Blocks.empty());
        continue;
      }
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      for (uint64_t Object : {0x8008U, 0x8010U, 0x9000U, 0x6000U}) {
        SCOPED_TRACE(Object);
        auto Actual = machineExitState(0x8000);
        Actual.write(NdVar::reg(x86reg::RCX, 8), Object);
        auto Expected = Actual;
        Expected.memory(0x8008, SavedPointer ? 0x8000 : 42, 8);
        Expected.memory(Object + Bias, Actual.read(NdVar::reg(x86reg::RDX, 8)),
                        8);
        const auto Return =
            SavedPointer ? 42 : machineExitWord(Expected, 0x8008);
        ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
        EXPECT_EQ(Actual.read(NdVar::reg(x86reg::RAX, 8)), Return);
        EXPECT_EQ(Actual.read(NdVar::reg(x86reg::RSP, 8)), 0x8000U);
        EXPECT_EQ(Actual.read(NdVar::reg(x86reg::R14, 8)), Object + Bias);
        EXPECT_EQ(Actual.Memory, Expected.Memory);
      }
    }
  }
}

TEST_F(PEFixedImageTest, EntryObjectsRejectMalformedBindingsBeforeEvaluation) {
  auto Loaded = load(machineExitFixture({0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  for (unsigned Case = 0; Case != 14; ++Case) {
    SCOPED_TRACE(Case);
    auto O = entryObjectOptions();
    auto &Object = O.EntryFrameExcludedObjects.front();
    switch (Case) {
    case 0:
      Object.Register.Bytes = 4;
      break;
    case 1:
      Object.Register.Offset = x86reg::RIP;
      break;
    case 2:
      Object.Register.Offset = x86reg::XMM0;
      break;
    case 3:
      ++Object.Register.Offset;
      break;
    case 4:
      Object.Register.Offset = x86reg::RSP;
      break;
    case 5:
      Object.End = Object.Begin;
      break;
    case 6:
      Object.Begin = Object.End + 1;
      break;
    case 7:
      O.EntryFrameBounds.reset();
      break;
    case 8:
      O.EntryFrameBounds->End = O.EntryFrameBounds->Begin;
      break;
    case 9:
      O.EntryFrameExcludedObjects.push_back(Object);
      break;
    case 10:
      O.EntryConstants = {{NdVar::reg(x86reg::RCX + 7, 1), 3}};
      break;
    case 11:
      O.ExplicitMachineState = false;
      break;
    case 12:
      O.NormalNonfaultingExecution = false;
      break;
    case 13:
      O.ByteOrder = llvm::endianness::big;
      break;
    }
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    // Generic option checks reject these two cases before image preparation.
    // The image provider reports all other malformed raw declarations through
    // its established Unsupported preparation contract.
    const auto Expected = Case == 11 || Case == 13
                              ? SpecializationStatus::InvalidInput
                              : SpecializationStatus::Unsupported;
    EXPECT_EQ(R.Status, Expected) << R.Diagnostic;
    EXPECT_FALSE(R.Diagnostic.empty());
    EXPECT_EQ(R.EvaluatedOperations, 0U);
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_FALSE(R.RawEnvironment);
  }
}

TEST_F(PEFixedImageTest, EntryObjectsBindEveryDeclarationFieldAndOrder) {
  auto Loaded = load(machineExitFixture({0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = entryObjectOptions();
  O.EntryFrameExcludedObjects.push_back({{x86reg::RDX, 8}, -16, 16});
  const auto Good = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  ASSERT_TRUE(Good.RawEnvironment);
  for (unsigned Case = 0; Case != 6; ++Case) {
    SCOPED_TRACE(Case);
    auto Changed = O;
    auto &Objects = Changed.EntryFrameExcludedObjects;
    switch (Case) {
    case 0:
      Objects[0].Register.Offset = x86reg::RBX;
      break;
    case 1:
      Objects[0].Register.Bytes = 4;
      break;
    case 2:
      --Objects[0].Begin;
      break;
    case 3:
      ++Objects[0].End;
      break;
    case 4:
      std::swap(Objects[0], Objects[1]);
      break;
    case 5:
      Objects.clear();
      break;
    }
    auto Error = validateSpecializationRawEnvironment(Good, Changed);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    auto Forged = Good;
    auto Domain = std::make_shared<SpecializationRawEnvironment>(
        *Good.RawEnvironment->Domain);
    Domain->Declaration = Changed;
    Forged.RawEnvironment->Domain = Domain;
    Error = validateSpecializationRawEnvironment(Forged, O);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

TEST_F(PEFixedImageTest,
       EntryObjectsRequireEveryCompleteExecutedWriteToMissFrame) {
  struct Case {
    std::vector<uint8_t> Write;
    int64_t Begin;
    int64_t End;
    bool Segmented;
    bool Complete;
  };
  const Case Cases[] = {
      {{0x48, 0x89, 0x51, 0x00}, 0, 32, false, true},
      {{0x48, 0x89, 0x51, 0x18}, 0, 32, false, true},
      {{0x48, 0x89, 0x51, 0x19}, 0, 32, false, false},
      {{0x48, 0x89, 0x51, 0x00}, 32, 40, false, false},
      {{0x48, 0x89, 0x51, 0xf0}, -16, 0, false, true},
      {{0x48, 0x89, 0x51, 0xf8}, -16, 0, false, true},
      {{0xf3, 0x0f, 0x7f, 0x41, 0x10}, 0, 32, false, true},
      {{0xf3, 0x0f, 0x7f, 0x41, 0x18}, 0, 32, false, false},
      {{0x48, 0x0f, 0xb1, 0x51, 0x18}, 0, 32, false, true},
      {{0x48, 0x0f, 0xb1, 0x51, 0x19}, 0, 32, false, false},
      {{0x64, 0x48, 0x89, 0x51, 0x08}, 0, 32, true, false},
      {{0x65, 0x48, 0x89, 0x51, 0x08}, 0, 32, true, false},
  };
  for (size_t I = 0; I < std::size(Cases); ++I) {
    SCOPED_TRACE(I);
    const auto &C = Cases[I];
    // Save the frame, perform the complete write, restore and return. The
    // wide/atomic controls establish supported access forms independently of
    // the straddling negatives. Segment bases remain arbitrary runtime inputs.
    std::vector<uint8_t> Code{0x48, 0x89, 0x64, 0x24, 0xf0};
    Code.insert(Code.end(), C.Write.begin(), C.Write.end());
    Code.insert(Code.end(), {0x48, 0x8b, 0x64, 0x24, 0xf0, 0xc3});
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = entryObjectOptions();
    O.EntryFrameExcludedObjects[0].Begin = C.Begin;
    O.EntryFrameExcludedObjects[0].End = C.End;
    O.X64SegmentedWrites = C.Segmented;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_EQ(R.complete(), C.Complete) << R.Diagnostic;
    if (R.complete()) {
      auto Error = validateSpecializationRawEnvironment(R, O);
      EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    } else {
      EXPECT_TRUE(R.Residual.Blocks.empty());
      EXPECT_FALSE(R.RawEnvironment);
    }
  }
}

TEST_F(PEFixedImageTest, EntryObjectsNeverRebindOverwrittenOrJoinedPointers) {
  const std::vector<std::vector<uint8_t>> Changes = {
      {0x48, 0x8b, 0x0a, 0xeb, 0}, // mov rcx,[rdx]; jmp next
      {0x88, 0xd1, 0xeb, 0},       // mov cl,dl; jmp next
      // test eax,eax; je original; mov rcx,r8; jmp join;
      // original: mov rcx,rcx; join: ...  R8 has no object declaration.
      {0x85, 0xc0, 0x74, 5, 0x4c, 0x89, 0xc1, 0xeb, 3, 0x48, 0x89, 0xc9},
  };
  for (size_t I = 0; I < Changes.size(); ++I) {
    SCOPED_TRACE(I);
    std::vector<uint8_t> Code{0x48, 0x89, 0x64, 0x24, 0xf0};
    Code.insert(Code.end(), Changes[I].begin(), Changes[I].end());
    Code.insert(Code.end(), {0x48, 0x89, 0x11,             // mov [rcx],rdx
                             0x48, 0x8b, 0x64, 0x24, 0xf0, // restore rsp
                             0xc3});
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    for (bool DeclareOther : {false, true}) {
      if (DeclareOther && I != 2)
        continue;
      auto O = entryObjectOptions();
      if (DeclareOther)
        O.EntryFrameExcludedObjects.push_back({{x86reg::R8, 8}, 32, 64});
      // The second root's offset zero can hit the saved frame even though
      // its declared [32,64) range is disjoint. Root identity matters at joins.
      const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
      EXPECT_FALSE(R.complete());
      EXPECT_TRUE(R.Residual.Blocks.empty());
      EXPECT_FALSE(R.RawEnvironment);
    }
  }
}

TEST_F(PEFixedImageTest, EntryObjectsRetainAliasingBetweenDeclaredObjects) {
  // Both arbitrary entry pointers may identify the same or overlapping
  // objects. The load through RDX must see the earlier store through RCX.
  for (bool Project : {false, true}) {
    SCOPED_TRACE(Project);
    std::vector<uint8_t> Code{0x48, 0x89, 0x64, 0x24, 0xf0, // save rsp
                              0x48, 0xc7, 0x02, 0x44, 0x33,
                              0x22, 0x11,        // mov qword [rdx],0x11223344
                              0x4c, 0x89, 0x01}; // mov [rcx],r8
    if (Project)
      Code.insert(Code.end(), {0xeb, 0});
    Code.insert(Code.end(), {0x48, 0x8b, 0x02,             // mov rax,[rdx]
                             0x48, 0x8b, 0x64, 0x24, 0xf0, // restore rsp
                             0xc3});
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = entryObjectOptions();
    O.EntryFrameExcludedObjects.push_back({{x86reg::RDX, 8}, 0, 32});
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    for (uint64_t Other : {0x2000U, 0x2001U, 0x2007U, 0x2020U}) {
      SCOPED_TRACE(Other);
      auto Actual = machineExitState(0x1000);
      Actual.write(NdVar::reg(x86reg::RCX, 8), 0x2000);
      Actual.write(NdVar::reg(x86reg::RDX, 8), Other);
      for (unsigned I = 0; I < 64; ++I)
        Actual.Memory[0x2000 + I] = uint8_t(I ^ 0x69);
      auto Expected = Actual;
      Expected.memory(0xff0, 0x1000, 8);
      Expected.memory(Other, 0x11223344, 8);
      Expected.memory(0x2000, Expected.read(NdVar::reg(x86reg::R8, 8)), 8);
      Expected.write(NdVar::reg(x86reg::RAX, 8),
                     machineExitWord(Expected, Other));
      ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
      std::erase_if(Actual.Registers, [](const auto &V) {
        return V.first.first == VnodeSpace::TEMP;
      });
      EXPECT_EQ(Actual.Registers, Expected.Registers);
      EXPECT_EQ(Actual.Memory, Expected.Memory);
    }
  }
}

TEST_F(PEFixedImageTest, EntryObjectNodeLimitsNeverPublishPartialRecovery) {
  auto Loaded = load(machineExitFixture(entryObjectCode()));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  for (uint64_t Nodes : {0U, 1U, 16U}) {
    SCOPED_TRACE(Nodes);
    auto O = entryObjectOptions();
    O.MaxSymbolicNodes = Nodes;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_EQ(R.Status, Nodes ? SpecializationStatus::BudgetExceeded
                              : SpecializationStatus::InvalidInput)
        << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_TRUE(R.Origins.empty());
    EXPECT_TRUE(R.ExternalExits.empty());
    EXPECT_FALSE(R.RawEnvironment);
  }
}

TEST_F(PEFixedImageTest, EntryObjectRootsMayEqualTheFrameRoot) {
  const uint8_t Code[] = {
      0x48, 0x39, 0xe1,             // cmp rcx,rsp
      0x75, 6,                      // jne different
      0xb8, 1,    0,    0, 0, 0xc3, // mov eax,1; ret
      0xb8, 2,    0,    0, 0, 0xc3, // different: mov eax,2; ret
  };
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = entryObjectOptions();
  // Equal roots still describe adjacent, disjoint complete ranges.
  O.EntryFrameExcludedObjects[0].Begin = 8;
  O.EntryFrameExcludedObjects[0].End = 16;
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Object : {0x1000U, 0x2000U}) {
    SCOPED_TRACE(Object);
    auto Actual = machineExitState(0x1000);
    Actual.write(NdVar::reg(x86reg::RCX, 8), Object);
    auto Expected = Actual;
    Expected.write(NdVar::reg(x86reg::RAX, 8), Object == 0x1000 ? 1 : 2);
    for (uint64_t Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                          x86reg::SF, x86reg::OF})
      Expected.write(NdVar::reg(Flag, 1),
                     Flag == x86reg::PF ||
                         (Flag == x86reg::ZF && Object == 0x1000));
    ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
    std::erase_if(Actual.Registers, [](const auto &V) {
      return V.first.first == VnodeSpace::TEMP;
    });
    EXPECT_EQ(Actual.Registers, Expected.Registers);
    EXPECT_EQ(Actual.Memory, Expected.Memory);
  }
}

TEST_F(PEFixedImageTest, EntryObjectsRemainARawOnlyDomainWhenUnused) {
  auto Loaded = load(machineExitFixture({0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  Image = std::move(*Loaded);
  Options = entryObjectOptions();
  const auto R = specializeBinaryInterpreter(Image, Base + 0x1000, Options);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_TRUE(R.RawEnvironment);
  ASSERT_FALSE(R.Residual.Blocks.empty());
  ASSERT_FALSE(R.Residual.Blocks.front().Ops.empty());
  EXPECT_TRUE(lowRawEnvironmentRequirementShapeIsValid(
      R.Residual.Blocks.front().Ops.front()));
  EXPECT_EQ(proof().Proof.Status, LowIRIndependenceStatus::Unsupported);
  const auto Native = checkBinaryLowIRRefinement(Image, Base + 0x1000, Options,
                                                 R.Residual, Contract);
  EXPECT_EQ(Native.Proof.Status, LowIRRefinementStatus::Unsupported);
  EXPECT_FALSE(Native.Certificate);
  const auto LLVM = checkBinaryLLVMRefinement(
      Image, Base + 0x1000, Options, R.Residual, "", "model", *Contract.Frame);
  EXPECT_EQ(LLVM.Native.Proof.Status, LowIRRefinementStatus::Unsupported);
  for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                      InterpreterMachineStateLayout::SharedGPRExit}) {
    auto Wrapped = wrapInterpreterMachineStateX64(
        R.Residual, BinaryFormat::COFF,
        InterpreterMachineStateProfile::UserX64NoFaultV1, std::nullopt, Layout);
    ASSERT_FALSE(bool(Wrapped));
    EXPECT_NE(llvm::toString(Wrapped.takeError()).find("raw environment"),
              std::string::npos);
  }
}

TEST_F(PEFixedImageTest, EntryObjectsKeepCompleteCopiedSpillsAcrossNodes) {
  for (bool Clobber : {false, true}) {
    SCOPED_TRACE(Clobber);
    std::vector<uint8_t> Code{
        0x48, 0x89, 0x64, 0x24, 0xf0, // save rsp
        0x48, 0x89, 0x4c, 0x24, 0xe8, // spill rcx at rsp-24
        0x48, 0x8d, 0x74, 0x24, 0xe8, // lea rsi,[rsp-24]
        0x48, 0x8d, 0x7c, 0x24, 0xd8, // lea rdi,[rsp-40]
        0xb9, 1,    0,    0,    0,    // mov ecx,1
        0xfc,                         // cld
        0xf3, 0x48, 0xa5};            // rep movsq
    if (Clobber)
      Code.insert(Code.end(), {0xc6, 0x44, 0x24, 0xd8, 0});
    Code.insert(Code.end(), {0xeb, 0, // projection boundary
                             0x48, 0x8b, 0x4c, 0x24, 0xd8, // reload object
                             0x48, 0x89, 0x51, 0x08,       // mov [rcx+8],rdx
                             0x48, 0x8b, 0x64, 0x24, 0xf0, // restore rsp
                             0xc3});
    auto Loaded = load(machineExitFixture(Code));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = entryObjectOptions();
    O.EntryFrameBounds->Begin = -64;
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    if (Clobber) {
      EXPECT_FALSE(R.complete());
      EXPECT_TRUE(R.Residual.Blocks.empty());
      continue;
    }
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    auto Actual = machineExitState(0x1000);
    Actual.write(NdVar::reg(x86reg::RCX, 8), 0x3008);
    for (unsigned I = 0; I < 32; ++I)
      Actual.Memory[0x3008 + I] = uint8_t(I ^ 0x93);
    auto Expected = Actual;
    Expected.memory(0xff0, 0x1000, 8);
    Expected.memory(0xfe8, 0x3008, 8);
    Expected.memory(0xfd8, 0x3008, 8);
    Expected.memory(0x3010, Expected.read(NdVar::reg(x86reg::RDX, 8)), 8);
    Expected.write(NdVar::reg(x86reg::RSI, 8), 0xff0);
    Expected.write(NdVar::reg(x86reg::RDI, 8), 0xfe0);
    Expected.write(NdVar::reg(x86reg::DF, 1), 0);
    ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
    std::erase_if(Actual.Registers, [](const auto &V) {
      return V.first.first == VnodeSpace::TEMP;
    });
    EXPECT_EQ(Actual.Registers, Expected.Registers);
    EXPECT_EQ(Actual.Memory, Expected.Memory);
  }
}

TEST_F(PEFixedImageTest, EntryObjectsPreserveRuntimeLoopWrites) {
  const uint8_t Code[] = {0x48, 0x89, 0x64, 0x24, 0xf0, // save rsp
                          0xeb, 0,                      // enter loop
                          0x48, 0x89, 0x11,             // loop: mov [rcx],rdx
                          0x48, 0x83, 0xe8, 1,          // sub rax,1
                          0x75, 0xf7,                   // jnz loop
                          0x48, 0x8b, 0x64, 0x24, 0xf0, // restore rsp
                          0xc3};
  auto Loaded = load(machineExitFixture(Code));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto O = entryObjectOptions();
  const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_GE(R.Residual.Blocks.size(), 3U);
  for (uint64_t Count : {1U, 2U, 5U}) {
    auto Actual = machineExitState(0x1000);
    Actual.write(NdVar::reg(x86reg::RCX, 8), 0x2000);
    Actual.write(NdVar::reg(x86reg::RAX, 8), Count);
    for (unsigned I = 0; I < 32; ++I)
      Actual.Memory[0x2000 + I] = uint8_t(I ^ 0x93);
    auto Expected = Actual;
    Expected.memory(0xff0, 0x1000, 8);
    Expected.memory(0x2000, Expected.read(NdVar::reg(x86reg::RDX, 8)), 8);
    Expected.write(NdVar::reg(x86reg::RAX, 8), 0);
    for (uint64_t Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                          x86reg::SF, x86reg::OF})
      Expected.write(NdVar::reg(Flag, 1),
                     Flag == x86reg::PF || Flag == x86reg::ZF);
    ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
    std::erase_if(Actual.Registers, [](const auto &V) {
      return V.first.first == VnodeSpace::TEMP;
    });
    EXPECT_EQ(Actual.Registers, Expected.Registers);
    EXPECT_EQ(Actual.Memory, Expected.Memory);
  }
  auto Exact = O;
  Exact.MaxOperations = R.EvaluatedOperations;
  EXPECT_TRUE(
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, Exact).complete());
  ASSERT_GT(Exact.MaxOperations, 0U);
  --Exact.MaxOperations;
  const auto Short = specializeBinaryInterpreter(*Loaded, Base + 0x1000, Exact);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
}
