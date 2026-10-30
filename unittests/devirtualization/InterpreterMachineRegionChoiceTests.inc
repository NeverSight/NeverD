//===- InterpreterMachineRegionChoiceTests.inc - Arbitrary bit paths
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent instruction programs for dynamic architectural choices.

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoiceKeepsBothBranches) {
  // ADD establishes AF=1, XOR produces arbitrary AF, PUSHFQ saves that value,
  // TEST/JNZ selects one of two RETs. Selected LowIR keeps only the second RET.
  auto Image =
      loadRegion({0xb8, 15,   0,    0,    0,  0x83, 0xc0, 1,    0x31, 0xc0,
                  0x9c, 0x59, 0xf6, 0xc1, 16, 0x75, 6,    0xb8, 1,    0,
                  0,    0,    0xc3, 0xb8, 2,  0,    0,    0,    0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 22, ExitKind::Return);
  Options.X64MachineExits.push_back({1, Entry + 28, ExitKind::Return});
  const auto Selected = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Selected.complete()) << Selected.Diagnostic;
  ASSERT_EQ(Selected.ExternalExits.size(), 1U);
  EXPECT_EQ(Selected.ExternalExits.front().Original.Origin.Address, Entry + 28);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  std::set<va_t> Exits;
  for (const auto &Exit : Result.ExternalExits)
    Exits.insert(Exit.Original.Origin.Address);
  EXPECT_EQ(Exits, (std::set<va_t>{Entry + 22, Entry + 28}));
  unsigned Productions = 0;
  for (const auto &Block : Result.Residual.Blocks)
    for (const auto &Op : Block.Ops)
      Productions += isLowArchitecturalValue(Op);
  EXPECT_GE(Productions, 2U);
  auto Source = wrapInterpreterMachineRegionSourceX64(Result, Options);
  EXPECT_FALSE(bool(Source));
  if (!Source)
    EXPECT_FALSE(llvm::toString(Source.takeError()).empty());
  auto Changed = Options;
  Changed.UndefinedModel = SpecializationUndefinedModel::SelectedLowIR;
  auto Error = validateSpecializationRawEnvironment(Result, Changed);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  auto Bare = Result.Residual;
  for (auto &Block : Bare.Blocks)
    std::erase_if(Block.Ops, [](const auto &Op) {
      return Op.Opcode == NdOp::INTRINSIC &&
             isRawEnvironmentRequirementId(Op.Inputs[0].Offset);
    });
  EXPECT_THROW(LowToMedConverter().convert(Bare, Arch::X64),
               LowToMedConversionError);
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoicesAreFreshAtRepeatedNativeAddress) {
  // ADD EDI,0 sets AF=0. The loop samples XOR's AF twice from the same PC;
  // EDX collects both bits. Equal per-PC choices cannot reach the mixed RET.
  auto Image = loadRegion(
      {0xb9, 2,    0,    0,    0,    0x31, 0xd2, 0x83, 0xc7, 0,    0x31, 0xc0,
       0x9c, 0x5b, 0x83, 0xe3, 16,   0xd1, 0xe2, 0x09, 0xda, 0xff, 0xc9, 0x75,
       0xf1, 0x83, 0xfa, 16,   0x74, 11,   0x83, 0xfa, 32,   0x74, 6,    0xb8,
       1,    0,    0,    0,    0xc3, 0xb8, 2,    0,    0,    0,    0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 40, ExitKind::Return);
  Options.X64MachineExits.push_back({1, Entry + 46, ExitKind::Return});
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  std::set<va_t> Exits;
  for (const auto &Exit : Result.ExternalExits)
    Exits.insert(Exit.Original.Origin.Address);
  EXPECT_EQ(Exits, (std::set<va_t>{Entry + 40, Entry + 46}));
  unsigned Occurrences = 0;
  for (const auto &Origin : Result.Origins)
    Occurrences += Origin.NativeInstruction.Address == Entry + 10;
  EXPECT_GE(Occurrences, 2U);
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoicesRequireAnExplicitRegionAndBudget) {
  auto Image = loadRegion({0x31, 0xc0, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 2, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  Options.MaxOperations = 1;
  const auto Bounded = specializeBinaryInterpreter(*Image, Entry, Options);
  EXPECT_FALSE(Bounded.complete());
  EXPECT_TRUE(Bounded.Residual.Blocks.empty());
  Options.MaxOperations = SpecializationOptions{}.MaxOperations;
  Options.X64MachineExits.clear();
  const auto Undeclared = specializeBinaryInterpreter(*Image, Entry, Options);
  EXPECT_FALSE(Undeclared.complete());
  EXPECT_TRUE(Undeclared.Residual.Blocks.empty());
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoicesKeepCopiesAndStackSpillsCorrelated) {
  // Two snapshots of the same production must agree. A new choice on each
  // PUSHFQ read would incorrectly make the second RET reachable.
  auto Image = loadRegion({0x31, 0xc0, 0x9c, 0x59, 0x9c, 0x5a, 0x31, 0xd1, 0xf6,
                           0xc1, 16,   0x75, 6,    0xb8, 1,    0,    0,    0,
                           0xc3, 0xb8, 2,    0,    0,    0,    0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 18, ExitKind::Return);
  Options.X64MachineExits.push_back({1, Entry + 24, ExitKind::Return});
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  ASSERT_EQ(Result.ExternalExits.size(), 1U);
  EXPECT_EQ(Result.ExternalExits.front().Original.Origin.Address, Entry + 18);
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoicesPreserveGuardedShiftOutputs) {
  // A masked zero shift preserves AF=1. Nonzero shifts produce arbitrary AF.
  // POP RCX also ensures the shift's guard cannot reread the later CL value.
  auto Image =
      loadRegion({0xb8, 15,   0,    0,    0,  0x83, 0xc0, 1,    0xd3, 0xe0,
                  0x9c, 0x59, 0xf6, 0xc1, 16, 0x75, 6,    0xb8, 1,    0,
                  0,    0,    0xc3, 0xb8, 2,  0,    0,    0,    0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (uint64_t Count : {0U, 1U, 2U, 32U, 33U}) {
    SCOPED_TRACE(Count);
    auto Options = regionOptions(Entry + 22, ExitKind::Return);
    Options.X64MachineExits.push_back({1, Entry + 28, ExitKind::Return});
    Options.EntryConstants.push_back({NdVar::reg(x86reg::RCX, 8), Count});
    Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    std::set<va_t> Exits;
    for (const auto &Exit : Result.ExternalExits)
      Exits.insert(Exit.Original.Origin.Address);
    const std::set<va_t> Expected = Count % 32
                                        ? std::set<va_t>{Entry + 22, Entry + 28}
                                        : std::set<va_t>{Entry + 28};
    EXPECT_EQ(Exits, Expected);
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoicesStopBeforeAuthenticatedTrap) {
  auto Image = loadRegion({0x31, 0xc0, 0xcc});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 2, ExitKind::Trap);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  ASSERT_EQ(Result.ExternalExits.size(), 1U);
  EXPECT_EQ(Result.ExternalExits.front().Original.Origin.Address, Entry + 2);
  auto Error = validateSpecializationRawEnvironment(Result, Options);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}

class ArchitecturalChoiceProvider : public SpecializationProvider {
public:
  std::shared_ptr<const SpecializationRawEnvironment> Domain;
  std::map<va_t, SpecializationInstruction> Code;

  std::shared_ptr<const SpecializationRawEnvironment>
  rawEnvironment() const override {
    return Domain;
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto It = Code.find(Cursor.Address);
    if (It == Code.end())
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "choice fixture has no instruction");
    return It->second;
  }
};

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoicesRefuseMissingStaleAndMalformedEffects) {
  auto Image = loadRegion({0x31, 0xc0, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 2, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Native = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Native.complete()) << Native.Diagnostic;
  ASSERT_TRUE(Native.RawEnvironment);
  ASSERT_EQ(Native.ExternalExits.size(), 1U);
  ArchitecturalChoiceProvider Provider;
  Provider.Domain = Native.RawEnvironment->Domain;
  Provider.Code[Entry + 2] = Native.ExternalExits.front().Original;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X64));
  Decode.setStrict(true);
  const uint8_t Bytes[]{0x31, 0xc0};
  DecodedInsn Decoded{};
  ASSERT_EQ(Decode.decodeOneForLift(Bytes, sizeof(Bytes), Entry, Decoded),
            sizeof(Bytes));
  SpecializationInstruction Original;
  Decode.liftToLow(Decoded, Original.Ops, {}, {}, &Original.UndefinedEffects);
  Original.Origin.Address = Entry;
  Original.Origin.Size = sizeof(Bytes);
  Original.Origin.OpCount = Original.Ops.size();
  Original.Fallthrough = {Entry + sizeof(Bytes)};
  Original.NativeBytes.assign(std::begin(Bytes), std::end(Bytes));
  ASSERT_FALSE(Original.UndefinedEffects.Effects.empty());
  Provider.Code[Entry] = Original;
  const auto Good = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 12; ++Kind) {
    SCOPED_TRACE(Kind);
    auto Changed = Original;
    auto &Description = Changed.UndefinedEffects;
    auto &Effect = Description.Effects.front();
    if (Kind == 0)
      Description.Coverage = LowUndefinedCoverage::Missing;
    else if (Kind == 1)
      Description.Coverage = LowUndefinedCoverage::Unsupported;
    else if (Kind == 2)
      Description.OperationDigest.clear();
    else if (Kind == 3)
      ++Description.OpCount;
    else if (Kind == 4)
      Effect.AfterOp = Description.OpCount + 1;
    else if (Kind == 5)
      Effect.BitCount = 0;
    else if (Kind == 6)
      Effect.BitOffset = Effect.Output.Size * 8;
    else if (Kind == 7)
      Effect.Output = NdVar::scalar(0, 1);
    else if (Kind == 8)
      Effect.Output = NdVar::tmp(UINT64_MAX, 1);
    else if (Kind == 9)
      Effect.When = NdVar::scalar(2, 1);
    else if (Kind == 10)
      Effect.When = NdVar::tmp(UINT64_MAX, 1);
    else
      Effect.When = NdVar::tmp(uint64_t{1} << 48, 1);
    Provider.Code[Entry] = Changed;
    const auto Bad = specializeInterpreter(Provider, {Entry}, Options);
    EXPECT_FALSE(Bad.complete()) << Bad.Diagnostic;
    EXPECT_TRUE(Bad.Residual.Blocks.empty());
    EXPECT_FALSE(Bad.RawEnvironment);
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoicesReuseExactDisabledCetPreservation) {
  // RDSSPD EAX and RDSSPQ RAX both preserve the full pre-existing RAX when
  // shadow stacks are explicitly disabled; the 32-bit form is not an EAX
  // write that clears its upper half. The following comparison observes it.
  for (const std::vector<uint8_t> Read :
       {std::vector<uint8_t>{0xf3, 0x0f, 0x1e, 0xc8},
        std::vector<uint8_t>{0xf3, 0x48, 0x0f, 0x1e, 0xc8}}) {
    std::vector<uint8_t> Code{
        0x48, 0xb8, 1, 2,    3,    4,   5,
        6,    7,    8, 0x48, 0x89, 0xc2}; // MOVABS RAX; MOV RDX,RAX
    Code.insert(Code.end(), Read.begin(), Read.end());
    const size_t Compare = Code.size();
    const uint8_t Tail[]{0x48, 0x39, 0xd0, 0x75, 6, 0xb8, 1, 0,   0,
                         0,    0xc3, 0xb8, 2,    0, 0,    0, 0xc3};
    Code.insert(Code.end(), std::begin(Tail), std::end(Tail));
    auto Image = loadRegion(Code);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Options = regionOptions(Entry + Compare + 10, ExitKind::Return);
    Options.X64MachineExits.push_back(
        {1, Entry + Compare + 16, ExitKind::Return});
    Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    ASSERT_EQ(Result.ExternalExits.size(), 1U);
    EXPECT_EQ(Result.ExternalExits.front().Original.Origin.Address,
              Entry + Compare + 10);
    for (const auto &Block : Result.Residual.Blocks)
      for (const auto &Op : Block.Ops)
        EXPECT_FALSE(isLowArchitecturalValue(Op));
    Options.X64CetDisabled = false;
    const auto Unsupported =
        specializeBinaryInterpreter(*Image, Entry, Options);
    EXPECT_FALSE(Unsupported.complete());
    EXPECT_TRUE(Unsupported.Residual.Blocks.empty());
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoicesRefuseForgedDisabledCetProjection) {
  auto Image = loadRegion({0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Exit = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Exit.complete()) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.RawEnvironment);
  ASSERT_EQ(Exit.ExternalExits.size(), 1U);
  ArchitecturalChoiceProvider Provider;
  Provider.Domain = Exit.RawEnvironment->Domain;
  Provider.Code[Entry] = Exit.ExternalExits.front().Original;
  const va_t Start = Entry - 4;
  SpecializationInstruction Original;
  Original.NativeBytes = {0xf3, 0x0f, 0x1e, 0xc8};
  Original.Origin.Address = Start;
  Original.Origin.Size = 4;
  Original.Origin.OpCount = 1;
  Original.Fallthrough = {Entry};
  Original.ProfileProjection =
      InterpreterProfileProjection::CetDisabledReadShadowStackV1;
  Original.Ops = {LowOp{.Opcode = NdOp::NOP, .Addr = Start}};
  Original.UndefinedEffects.Coverage = LowUndefinedCoverage::Missing;
  Original.UndefinedEffects.OpCount = 1;
  Original.UndefinedEffects.OperationDigest =
      lowUndefinedOperationDigest(Original.Ops);
  Provider.Code[Start] = Original;
  const auto Good = specializeInterpreter(Provider, {Start}, Options);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 9; ++Kind) {
    SCOPED_TRACE(Kind);
    auto Changed = Original;
    if (Kind == 0)
      Changed.NativeBytes.back() = 0x08; // A memory encoding is not RDSSP.
    else if (Kind == 1)
      Changed.ProfileProjection = InterpreterProfileProjection::None;
    else if (Kind == 2)
      Changed.UndefinedEffects.Coverage = LowUndefinedCoverage::Complete;
    else if (Kind == 3)
      Changed.UndefinedEffects.Effects.push_back(
          {1, NdVar::reg(x86reg::RAX, 8), 0, 64, std::nullopt});
    else if (Kind == 4) {
      Changed.Ops.front() = operation(NdOp::COPY, NdVar::reg(x86reg::RAX, 8),
                                      {NdVar::scalar(0, 8)});
      Changed.Ops.front().Addr = Start;
      Changed.UndefinedEffects.OperationDigest =
          lowUndefinedOperationDigest(Changed.Ops);
    } else if (Kind == 5)
      Changed.UndefinedEffects.OperationDigest.clear();
    else if (Kind == 6)
      ++Changed.Origin.Size;
    else if (Kind == 7)
      ++Changed.Fallthrough.Address;
    else
      Changed.Fallthrough.Mode = InstructionMode::Thumb;
    Provider.Code[Start] = Changed;
    const auto Bad = specializeInterpreter(Provider, {Start}, Options);
    EXPECT_FALSE(Bad.complete()) << Bad.Diagnostic;
    EXPECT_TRUE(Bad.Residual.Blocks.empty());
    EXPECT_FALSE(Bad.RawEnvironment);
  }
}
