//===- X64PagedUndefinedOpcodeTests.inc - Original code64 fetch faults -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Paged source, original processor fetch and the x86 instruction-length limit.
//===----------------------------------------------------------------------===//

TEST_F(X64PagedExecutionTest, PagedEntriesEndAtPrefixFaults) {
  constexpr auto Request =
      R"({"profile":"kernel-events-v6","mxcsr_mask":"0xffff","paging_address_bits":48,"paged_memory":true,"entries":[4096],"follow_direct_jumps":true})";
  for (auto Fault : {std::vector<uint8_t>{0xf0, 0x0f, 0xff},
                     std::vector<uint8_t>(x86::kMaxInstructionLen, 0x66)}) {
    for (bool StartsAtFault : {false, true}) {
      auto Code = Fault;
      Code.push_back(0xc3);
      if (!StartsAtFault)
        Code.insert(Code.begin(), 0x90);
      auto Export = exportX64Continuation(image(Code), Request, false);
      ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
      const auto Manifest = llvm::cantFail(llvm::json::parse(Export->Manifest));
      const auto *Units = Manifest.getAsObject()->getArray("units");
      ASSERT_NE(Units, nullptr);
      ASSERT_EQ(Units->size(), StartsAtFault ? 1u : 2u);
      EXPECT_EQ(Units->back().getAsObject()->getString("pc"),
                StartsAtFault ? "0x1000" : "0x1001");
      EXPECT_EQ(Manifest.getAsObject()->getInteger("native_requests"), 0);
    }
  }
  // Truncation before the second opcode byte does not prove a fault.
  auto Bad = exportX64Continuation(image({0xf0, 0x0f}), Request, false);
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
  auto Relocated = image({0xf0, 0x0f, 0xff});
  Relocated.UnappliedCodeRelocations.emplace(0x1001, 1);
  auto Unapplied = exportX64Continuation(Relocated, Request, false);
  ASSERT_FALSE(bool(Unapplied));
  llvm::consumeError(Unapplied.takeError());
  auto Split = image({0xf0, 0x0f, 0xff});
  Section Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 1;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Section Data;
  Data.Name = ".rodata";
  Data.VA = 0x1001;
  Data.Size = Data.FileSz = 2;
  Data.Flags = SegmentFlags::Readable;
  Split.Sections = {Text, Data};
  auto CrossesOwner = exportX64Continuation(Split, Request, false);
  ASSERT_FALSE(bool(CrossesOwner));
  llvm::consumeError(CrossesOwner.takeError());
}

TEST(X64PagedUD0PrefixTest, QualifiedPrefixesMatchTheDecoder) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X64));
  constexpr std::array<uint8_t, 26> Prefixes{
      0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf2,
      0xf3, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
      0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
  unsigned Checked = 0;
  for (unsigned PrefixCount = 0; PrefixCount <= 2; ++PrefixCount) {
    const unsigned FirstCount = PrefixCount ? Prefixes.size() : 1;
    const unsigned SecondCount = PrefixCount == 2 ? Prefixes.size() : 1;
    for (unsigned First = 0; First != FirstCount; ++First)
      for (unsigned Second = 0; Second != SecondCount; ++Second) {
        std::vector<uint8_t> Code;
        if (PrefixCount)
          Code.push_back(Prefixes[First]);
        if (PrefixCount == 2)
          Code.push_back(Prefixes[Second]);
        Code.insert(Code.end(), {0x0f, 0xff, 0xc0});
        SCOPED_TRACE(testing::Message()
                     << "first=" << First << " second=" << Second);
        const auto Offset =
            x86PagedCode64UD0OpcodeOffset(Code.data(), Code.size() - 1);
        ASSERT_TRUE(Offset);
        EXPECT_EQ(*Offset, PrefixCount);
        const auto WithAddressTail =
            x86PagedCode64UD0OpcodeOffset(Code.data(), Code.size());
        ASSERT_TRUE(WithAddressTail);
        EXPECT_EQ(*WithAddressTail, PrefixCount);
        DecodedInsn Instruction;
        ASSERT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x1000,
                                          Instruction),
                  PrefixCount + X86PagedCode64UD0OpcodeBytes);
        EXPECT_EQ(Instruction.Id, X86_INS_UD0);
        ++Checked;
      }
  }
  EXPECT_EQ(Checked, 703u);
  for (const auto &Code : {std::vector<uint8_t>{0xf0, 0x0f, 0xff},
                           std::vector<uint8_t>{0xf0, 0x66, 0x0f, 0xff}}) {
    EXPECT_EQ(x86PagedCode64UD0OpcodeOffset(Code.data(), Code.size()),
              Code.size() - X86PagedCode64UD0OpcodeBytes);
    DecodedInsn Instruction;
    EXPECT_EQ(
        Decode.decodeOneForLift(Code.data(), Code.size(), 0x1000, Instruction),
        0);
  }
}

TEST(X64PagedUD0PrefixTest, ExtendedPrefixesAndLengthBoundaryMatchTheDecoder) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X64));
  constexpr std::array<uint8_t, 26> RepeatedPrefixes{
      0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf2,
      0xf3, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
      0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
  constexpr std::array<
      std::array<uint8_t, X86PagedCode64UD0QualifiedPrefixLimit>, 5>
      Patterns{{
          {{0x66, 0x67, 0x66, 0x67, 0x66, 0x67, 0x66, 0x67, 0x66, 0x67, 0x66,
            0x67, 0x66}},
          {{0x66, 0x67, 0xf2, 0x64, 0x4f, 0x2e, 0xf3, 0x40, 0x65, 0x3e, 0x48,
            0x36, 0x26}},
          {{0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
            0x4b, 0x4c}},
          {{0xf2, 0xf3, 0xf2, 0xf3, 0xf2, 0xf3, 0xf2, 0xf3, 0xf2, 0xf3, 0xf2,
            0xf3, 0xf2}},
          {{0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x26, 0x2e, 0x36, 0x3e, 0x64,
            0x65, 0x26}},
      }};
  EXPECT_EQ(X86PagedCode64UD0QualifiedPrefixLimit, 13u);
  unsigned Checked = 0;
  const auto Check = [&](const std::vector<uint8_t> &Prefix) {
    std::vector<uint8_t> Code = Prefix;
    Code.insert(Code.end(), {0x0f, 0xff, 0xc0});
    SCOPED_TRACE(testing::Message() << "count=" << Prefix.size()
                                    << " first=" << unsigned(Prefix.front()));
    const auto Offset = x86PagedCode64UD0OpcodeOffset(Code.data(), Code.size());
    ASSERT_TRUE(Offset);
    EXPECT_EQ(*Offset, Prefix.size());
    DecodedInsn Instruction;
    ASSERT_EQ(
        Decode.decodeOneForLift(Code.data(), Code.size(), 0x1000, Instruction),
        Prefix.size() + X86PagedCode64UD0OpcodeBytes);
    EXPECT_EQ(Instruction.Id, X86_INS_UD0);
    ++Checked;
  };
  for (uint8_t Byte : RepeatedPrefixes)
    for (unsigned PrefixCount = 3;
         PrefixCount <= X86PagedCode64UD0QualifiedPrefixLimit; ++PrefixCount)
      Check(std::vector<uint8_t>(PrefixCount, Byte));
  for (const auto &Pattern : Patterns)
    for (unsigned PrefixCount = 3;
         PrefixCount <= X86PagedCode64UD0QualifiedPrefixLimit; ++PrefixCount)
      Check(
          std::vector<uint8_t>(Pattern.begin(), Pattern.begin() + PrefixCount));
  EXPECT_EQ(Checked, (RepeatedPrefixes.size() + Patterns.size()) *
                         (X86PagedCode64UD0QualifiedPrefixLimit - 2));
  std::vector<uint8_t> Overlong(X86PagedCode64UD0QualifiedPrefixLimit + 1,
                                0x66);
  Overlong.insert(Overlong.end(), {0x0f, 0xff});
  EXPECT_FALSE(x86PagedCode64UD0OpcodeOffset(Overlong.data(), Overlong.size()));
  for (unsigned LockPosition : {0u, 6u, 12u}) {
    std::vector<uint8_t> Locked(X86PagedCode64UD0QualifiedPrefixLimit, 0x66);
    Locked[LockPosition] = 0xf0;
    Locked.insert(Locked.end(), {0x0f, 0xff});
    EXPECT_EQ(x86PagedCode64UD0OpcodeOffset(Locked.data(), Locked.size()),
              X86PagedCode64UD0QualifiedPrefixLimit);
  }
  // A 15-byte instruction can still fit when byte 15 is a complete opcode.
  Overlong.resize(x86::kMaxInstructionLen);
  Overlong.back() = 0x90;
  EXPECT_FALSE(x86PagedCode64FaultEncoding(Overlong.data(), Overlong.size()));
  Overlong.back() = 0x0f;
  EXPECT_FALSE(
      x86PagedCode64FaultEncoding(Overlong.data(), Overlong.size() - 1));
  auto IncompleteOpcode =
      x86PagedCode64FaultEncoding(Overlong.data(), Overlong.size());
  ASSERT_TRUE(IncompleteOpcode);
  EXPECT_EQ(IncompleteOpcode->Operation,
            X86PagedCode64FaultEncoding::PrefixLengthFault);
  EXPECT_EQ(IncompleteOpcode->Size, x86::kMaxInstructionLen);
}

TEST_F(X64PagedExecutionTest, UD0CrossPageFetchUsesTheBoundProcessor) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  const std::vector<std::vector<uint8_t>> EncodedAddresses{
      {0x00}, {0x04, 0x20}, {0x84, 0x20, 0x00, 0x50, 0x34, 0x12}};
  for (unsigned PrefixCount : {0u, 11u, 12u, 13u})
    for (unsigned Form = 0; Form != EncodedAddresses.size(); ++Form) {
      const uint64_t PC = 0x1ffe - Form - PrefixCount;
      const auto &Address = EncodedAddresses[Form];
      const size_t Offset = PC - 0x1000;
      std::vector<uint8_t> Code(
          Offset + PrefixCount + X86PagedCode64UD0OpcodeBytes + Address.size(),
          0x90);
      std::fill_n(Code.begin() + Offset, PrefixCount, 0x66);
      Code[Offset + PrefixCount] = 0x0f;
      Code[Offset + PrefixCount + 1] = 0xff;
      std::copy(Address.begin(), Address.end(),
                Code.begin() + Offset + PrefixCount +
                    X86PagedCode64UD0OpcodeBytes);
      for (const auto &Compiler : Compilers)
        for (bool LLVM : {false, true})
          for (const std::string Optimization : {"-O0", "-O2"}) {
            Run Driver = nullptr;
            ASSERT_NO_FATAL_FAILURE(
                compile(Code, LLVM, Optimization, Driver, {PC}, Compiler));
            for (auto Profile : {X86UD0FetchProfile::OpcodeOnly,
                                 X86UD0FetchProfile::CompleteModRM}) {
              SCOPED_TRACE(testing::Message()
                           << Compiler << ' ' << LLVM << ' ' << Optimization
                           << " address_form=" << Form << " prefixes="
                           << PrefixCount << " profile=" << unsigned(Profile));
              reset(Code);
              State.Core.Core.Core.Core.PC = PC;
              put(0x13010, 0); // ModR/M page is absent.
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              X64PagedExecutionOptions Options;
              Options.UD0Fetch = Profile;
              X64PagedExecution Context(*Memory, Options);
              uint64_t Budget = 1;
              const auto Exit = Driver(
                  &State, reinterpret_cast<uint8_t *>(&Context), &Budget);
              ASSERT_TRUE(Context.lastError().empty()) << Context.lastError();
              EXPECT_EQ(Exit, 7u);
              EXPECT_EQ(Budget, 0u);
              const unsigned Vector =
                  Profile == X86UD0FetchProfile::OpcodeOnly ? 6
                  : x86UD0FetchExceedsInstructionLength(
                        PrefixCount + X86PagedCode64UD0OpcodeBytes + Form)
                      ? 13
                      : 14;
              ASSERT_TRUE(Context.lastResult().Exception);
              EXPECT_EQ(Context.lastResult().Exception->Vector, Vector);
              EXPECT_EQ(Context.lastResult().Exception->ErrorCode,
                        Vector == 14 ? 0x10u : 0u);
              EXPECT_FALSE(Context.lastResult().InstructionCompleted);
              EXPECT_EQ(State.Core.Core.CR2, Vector == 14 ? 0x2000u : 0u);
              EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x4000u + Vector * 16u);
            }
          }
    }
}

TEST_F(X64PagedExecutionTest, UD0AddressExtentUsesLiveBytesAfterTheOpcode) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool Addr32 : {false, true}) {
    const uint64_t PC = 0x1ffc - Addr32;
    std::vector<uint8_t> Code(PC - 0x1000 + 2 + Addr32, 0x90);
    Code.back() = 0xff;
    Code[Code.size() - 2] = 0x0f;
    if (Addr32)
      Code[Code.size() - 3] = 0x67;
    for (bool LLVM : {false, true}) {
      Run Driver = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {PC}));
      for (uint8_t ModRM : {uint8_t{0xc0}, uint8_t{0x84}, uint8_t{0x00}})
        for (auto Profile : {X86UD0FetchProfile::OpcodeOnly,
                             X86UD0FetchProfile::CompleteModRM}) {
          SCOPED_TRACE(testing::Message()
                       << "route=" << LLVM << " addr32=" << Addr32 << " modrm="
                       << unsigned(ModRM) << " profile=" << unsigned(Profile));
          reset(Code);
          State.Core.Core.Core.Core.PC = PC;
          // A register-indirect UD0 never resolves or reads its data operand.
          State.Core.Core.Core.Core.GPR[0] = 0x0000800000000000ULL;
          RAM.Memory.front().Bytes[0x1ffe] = ModRM;
          RAM.Memory.front().Bytes[0x1fff] =
              0x20; // SIB before the absent page.
          put(0x13010, 0);
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecutionOptions Options;
          Options.UD0Fetch = Profile;
          X64PagedExecution Context(*Memory, Options);
          uint64_t Budget = 1;
          const auto Exit =
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
          ASSERT_TRUE(Context.lastError().empty()) << Context.lastError();
          EXPECT_EQ(Exit, 7u);
          EXPECT_EQ(Budget, 0u);
          ASSERT_TRUE(Context.lastResult().Exception);
          const bool PageFault =
              ModRM == 0x84 && Profile == X86UD0FetchProfile::CompleteModRM;
          EXPECT_EQ(Context.lastResult().Exception->Vector, PageFault ? 14 : 6);
          EXPECT_EQ(Context.lastResult().Exception->ErrorCode,
                    PageFault ? 0x10u : 0u);
          EXPECT_EQ(State.Core.Core.CR2, PageFault ? 0x2000u : 0u);
          EXPECT_EQ(State.Core.Core.Core.Core.PC,
                    0x4000u + (PageFault ? 14u : 6u) * 16u);
        }
    }
  }
}

TEST_F(X64PagedExecutionTest, UD0RefusesMissingOrInvalidFetchProfile) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<uint8_t> Long(X86PagedCode64UD0QualifiedPrefixLimit, 0x66);
  Long.insert(Long.end(), {0x0f, 0xff, 0xc0});
  for (const auto &Code :
       {std::vector<uint8_t>{0x0f, 0xff, 0xc0},
        std::vector<uint8_t>{0x67, 0x0f, 0xff, 0xc0},
        std::vector<uint8_t>{0x66, 0x0f, 0xff, 0xc0},
        std::vector<uint8_t>{0xf0, 0x0f, 0xff, 0xc0},
        std::vector<uint8_t>{0x66, 0x67, 0x0f, 0xff, 0xc0}, Long}) {
    for (bool LLVM : {false, true}) {
      Run Driver = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver));
      for (auto Profile : {std::optional<X86UD0FetchProfile>{},
                           std::optional{X86UD0FetchProfile(0xff)}}) {
        reset(Code);
        const auto Before = State;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecutionOptions Options;
        Options.UD0Fetch = Profile;
        X64PagedExecution Context(*Memory, Options);
        uint64_t Budget = 1;
        EXPECT_EQ(
            Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget), 5u);
        EXPECT_EQ(Budget, 1u);
        EXPECT_NE(Context.lastError().find("bound instruction-fetch profile"),
                  std::string::npos);
        EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
        EXPECT_EQ(
            Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAM.Memory.front().Bytes);
      }
    }
  }
}

TEST_F(X64PagedExecutionTest,
       PrefixLengthFaultAuthenticatesOnlyTheFirst15Bytes) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<uint8_t> Code(x86::kMaxInstructionLen - 1, 0x66);
  Code.insert(Code.end(), {0x0f, 0xff});
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver));
    for (bool Bound : {false, true}) {
      for (unsigned Offset : {14u, 15u}) {
        reset(Code);
        RAM.Memory.front().Bytes[0x1000 + Offset] = 0x90;
        const auto Before = State;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecutionOptions Options;
        Options.UD0Fetch = X86UD0FetchProfile::OpcodeOnly;
        Options.PrefixLengthFaultAfter15Bytes = Bound;
        X64PagedExecution Context(*Memory, Options);
        uint64_t Budget = 1;
        const auto Exit =
            Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
        if (!Bound) {
          EXPECT_EQ(Exit, 5u);
          EXPECT_NE(
              Context.lastError().find("instruction-length fault priority"),
              std::string::npos);
          EXPECT_EQ(
              Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
              RAM.Memory.front().Bytes);
        } else if (Offset == 14) {
          EXPECT_EQ(Exit, uint64_t(X64PagedExit::CodeChanged));
          EXPECT_TRUE(Context.lastError().empty());
        } else {
          EXPECT_EQ(Exit, 7u);
          ASSERT_TRUE(Context.lastResult().Exception);
          EXPECT_EQ(Context.lastResult().Exception->Vector, 13u);
          EXPECT_EQ(Context.lastResult().Exception->ErrorCode, 0u);
          EXPECT_EQ(Budget, 0u);
          continue;
        }
        EXPECT_EQ(Budget, 1u);
        EXPECT_FALSE(Context.lastResult().InstructionAttempted);
        EXPECT_EQ(std::memcmp(&Before, &State, sizeof(State)), 0);
      }
    }
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, UD0FetchPriorityMatchesOriginalHybridCPU) {
  if (!neverd::test::hostCpuIsIntel() || !hasCrossTargetClang())
    GTEST_SKIP() << "Intel KVM and test Clang are required";
  X64PagedNativeCPU CPU;
  CPU.pinForReturnComparison();
  if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
    GTEST_SKIP() << "UD0 fetch policy is unqualified for this core type";
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  const auto Profile = CPU.ReturnCoreType == 0x20
                           ? X86UD0FetchProfile::OpcodeOnly
                           : X86UD0FetchProfile::CompleteModRM;
  RecordProperty("core_type", CPU.ReturnCoreType);
  const std::vector<std::vector<uint8_t>> EncodedAddresses{
      {0x00}, {0x04, 0x20}, {0x84, 0x20, 0x00, 0x50, 0x34, 0x12}};
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  std::vector<std::vector<uint8_t>> Prefixes{
      {},           {0x66},       {0x67},      {0xf2}, {0xf3},
      {0x2e},       {0x65},       {0x40},      {0x4f}, {0x66, 0x66},
      {0x66, 0x67}, {0x67, 0x66}, {0x67, 0x67}};
  for (unsigned Count : {3u, 11u, 12u, 13u})
    Prefixes.emplace_back(Count, 0x66);
  Prefixes.push_back({0x66, 0x67, 0xf2, 0x64, 0x4f, 0x2e, 0xf3});
  Prefixes.push_back(
      {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49});
  Prefixes.push_back({0x66, 0x67, 0xf2, 0x64, 0x4f, 0x2e, 0xf3, 0x40, 0x65,
                      0x3e, 0x48, 0x36, 0x26});
  Prefixes.push_back({0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
                      0x49, 0x4a, 0x4b, 0x4c});
  Prefixes.push_back({0xf0});
  Prefixes.push_back({0x66, 0xf0});
  Prefixes.push_back({0xf0, 0x66});
  Prefixes.emplace_back(13, 0xf0);
  Prefixes.push_back({0x66, 0x67, 0xf2, 0x64, 0x4f, 0x2e, 0xf3, 0x40, 0x65,
                      0x3e, 0x48, 0x36, 0xf0});
  for (unsigned Count : {14u, 15u})
    for (uint8_t Prefix : {0x66, 0xf0, 0x4f})
      Prefixes.emplace_back(Count, Prefix);
  unsigned Comparisons = 0;
  for (const auto &Prefix : Prefixes) {
    for (unsigned Form = 0; Form != EncodedAddresses.size(); ++Form) {
      const bool Overlong = Prefix.size() >= 14;
      // Move the page boundary across bytes 15 and 16 of an overlong stream.
      const unsigned Available =
          Overlong ? 14 + Form : Prefix.size() + 2 + Form;
      const unsigned Vector =
          !Overlong && Profile == X86UD0FetchProfile::OpcodeOnly ? 6
          : Available < 15                                       ? 14
                                                                 : 13;
      const uint64_t PC = 0x2000 - Available;
      const auto &Address = EncodedAddresses[Form];
      const size_t Offset = PC - 0x1000;
      std::vector<uint8_t> Code(Offset + Prefix.size() + 2 + Address.size(),
                                0x90);
      std::copy(Prefix.begin(), Prefix.end(), Code.begin() + Offset);
      Code[Offset + Prefix.size()] = 0x0f;
      Code[Offset + Prefix.size() + 1] = 0xff;
      std::copy(Address.begin(), Address.end(),
                Code.begin() + Offset + Prefix.size() + 2);
      reset(Code);
      State.Core.Core.Core.Core.PC = PC;
      State.Core.Core.Core.Core.GPR[0] = 0x0000800000000000ULL;
      put(0x13010, 0);
      X64ContinuationStateV6 Native;
      bool Handler = false;
      ASSERT_NO_FATAL_FAILURE(CPU.execute(
          RAM.Memory.front().Bytes, State, Controls.CR0,
          Prefix.size() + 2 + Address.size(), Native, Handler, true));
      ASSERT_TRUE(Handler);
      EXPECT_EQ(Native.Core.Core.Core.Core.PC, 0x4000u + Vector * 16u);
      EXPECT_EQ(Native.Core.Core.CR2, Vector == 14 ? 0x2000u : 0u);
      const uint64_t NativeSP = Native.Core.Core.Core.Core.GPR[4];
      const bool HasErrorCode = Vector != 6;
      ASSERT_LE(NativeSP + (HasErrorCode ? 16u : 8u),
                uint64_t(X64PagedNativeCPU::Size));
      const auto FrameWord = [&](unsigned Offset) {
        uint64_t Value;
        std::memcpy(&Value, CPU.RAM + NativeSP + Offset, sizeof(Value));
        return Value;
      };
      EXPECT_EQ(FrameWord(HasErrorCode ? 8 : 0), PC);
      if (HasErrorCode)
        EXPECT_EQ(FrameWord(0), Vector == 14 ? 0x10u : 0u);
      for (const auto &Compiler : Compilers)
        for (bool LLVM : {false, true})
          for (const std::string Optimization : {"-O0", "-O2"}) {
            SCOPED_TRACE(testing::Message()
                         << "core=" << CPU.ReturnCoreType << " form=" << Form
                         << " prefix=" << Prefix.size() << ' ' << Compiler
                         << ' ' << LLVM << ' ' << Optimization);
            Run Driver = nullptr;
            ASSERT_NO_FATAL_FAILURE(
                compile(Code, LLVM, Optimization, Driver, {PC}, Compiler));
            reset(Code);
            State.Core.Core.Core.Core.PC = PC;
            State.Core.Core.Core.Core.GPR[0] = 0x0000800000000000ULL;
            put(0x13010, 0);
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecutionOptions Options;
            if (Overlong)
              Options.PrefixLengthFaultAfter15Bytes = true;
            else
              Options.UD0Fetch = Profile;
            X64PagedExecution Context(*Memory, Options);
            uint64_t Budget = 1;
            const auto Exit =
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
            ASSERT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Exit, 7u);
            EXPECT_EQ(Budget, 0u);
            ASSERT_TRUE(Context.lastResult().Exception);
            EXPECT_EQ(Context.lastResult().Exception->Vector, Vector);
            EXPECT_EQ(Context.lastResult().Exception->ErrorCode,
                      Vector == 14 ? 0x10u : 0u);
            EXPECT_FALSE(Context.lastResult().InstructionCompleted);
            const auto &Core = State.Core.Core.Core.Core;
            const auto &Original = Native.Core.Core.Core.Core;
            EXPECT_EQ(Core.PC, Original.PC);
            EXPECT_EQ(Core.RFlags, Original.RFlags);
            for (unsigned I = 0; I != 16; ++I)
              EXPECT_EQ(Core.GPR[I], Original.GPR[I]) << I;
            EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
            EXPECT_EQ(State.Core.CS.Selector, Native.Core.CS.Selector);
            EXPECT_EQ(State.Core.SS.Selector, Native.Core.SS.Selector);
            const auto Actual =
                Memory->physicalMemory().snapshotMemoryRegions();
            EXPECT_EQ(
                std::memcmp(Actual.front().Bytes.data(), CPU.RAM, 0x10000), 0);
            EXPECT_EQ(std::memcmp(Actual.front().Bytes.data() + 0x14000,
                                  CPU.RAM + 0x14000,
                                  X64PagedNativeCPU::Size - 0x14000),
                      0);
            ++Comparisons;
          }
    }
  }
  RecordProperty("cpu_comparisons", Comparisons);
}
#endif
