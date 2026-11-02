//===- RawCodeEnvironmentTests.inc - Owned native byte domains -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Included in the independently authored fixed-PE fixture's test namespace.

SpecializationOptions rawOptions(va_t Address = 0x400000) {
  SpecializationOptions O;
  O.ExplicitMachineState = O.NormalNonfaultingExecution = O.X64CetDisabled =
      true;
  O.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
  O.RequireRestoredFrameAtReturn = true;
  O.MaxChainedTransfers = 32;
  O.X64CodeRegions.push_back(
      {1, Address, {0xc3}, "independent code fixture v1"});
  return O;
}

std::vector<uint8_t> returningImportFixture() {
  auto B = terminalImportFixture();
  const uint8_t Caller[] = {
      0x48, 0x8d, 0x64, 0x24, 0xc0,        // lea rsp,[rsp-64]
      0x48, 0x8d, 0x4c, 0x24, 0x20,        // lea rcx,[rsp+32]
      0xff, 0x15, 0,    0,    0,    0,     // call [rip+slot]
      0x48, 0x8b, 0x54, 0x24, 0x20,        // mov rdx,[rsp+32]
      0x48, 0x8d, 0x64, 0x24, 0x40,        // lea rsp,[rsp+64]
      0xb8, 42,   0,    0,    0,    0xc3}; // mov eax,42; ret
  std::copy(std::begin(Caller), std::end(Caller), B.begin() + Text);
  put32(B, Text + 12, 0x3080 - 0x1010);
  // Independent plain entry for unused declarations.
  const uint8_t Plain[] = {0xb8, 19, 0, 0, 0, 0xc3};
  std::copy(std::begin(Plain), std::end(Plain), B.begin() + Text + 0x100);
  return B;
}

SpecializationOptions returningRawOptions(va_t Address) {
  auto O = rawOptions(Address);
  O.X64ImportBindings.push_back({1, Base + 0x3080, "example.dll", "StopFixture",
                                 Address,
                                 "independent stable import fixture v1"});
  O.X64CodeRegions[0].Bytes = {
      0x53,                         // push rbx
      0x48, 0x8b, 0x5c, 0x24, 0x08, // mov rbx,[rsp+8] (physical native return
                                    // word)
      0x48, 0x89, 0x19,             // mov [rcx],rbx
      0x44, 0x8b, 0x05, 0, 0, 0, 0, // mov r8d,[rip+global]
      0x44, 0x89, 0x41, 0x08,       // mov [rcx+8],r8d
      0x41, 0xba, 0x34, 0x12, 0, 0, // mov r10d,0x1234
      0xb8, 0, 0, 0, 0,             // mov eax,0
      0x5b, 0xc3};                  // pop rbx; ret
  put32(O.X64CodeRegions[0].Bytes, 12, 0x1000 - 16);
  return O;
}

// Independent byte store and small scalar LowOp oracle for the authored
// instruction sequence above. It shares neither symbolic expressions nor the
// solver. Only this test consumer handles the entry requirement, and only
// after the entire result is validated against independent expected options.
struct RawByteOracle {
  using Key = std::pair<VnodeSpace, uint64_t>;
  std::map<Key, uint8_t> Registers;
  std::map<va_t, uint8_t> Memory;
  std::optional<uint64_t> NextPC;
  bool Valid = true;
  std::string Diagnostic;

  uint64_t read(NdVar V) {
    if (!V.Size || V.Size > 8) {
      Valid = false;
      return 0;
    }
    if (V.isConst())
      return V.Size == 8 ? V.Offset
                         : V.Offset & ((uint64_t{1} << (V.Size * 8)) - 1);
    uint64_t N = 0;
    for (unsigned I = 0; I < V.Size; ++I) {
      const auto It = Registers.find({V.Space, V.Offset + I});
      if (It == Registers.end())
        Valid = false;
      else
        N |= uint64_t{It->second} << (I * 8);
    }
    return N;
  }
  void write(NdVar V, uint64_t N) {
    if (!V.Size || V.Size > 8 || (!V.isReg() && !V.isTemp())) {
      Valid = false;
      return;
    }
    for (unsigned I = 0; I < V.Size; ++I)
      Registers[{V.Space, V.Offset + I}] = N >> (I * 8);
  }
  void memory(va_t Address, uint64_t Value, unsigned Bytes) {
    for (unsigned I = 0; I < Bytes; ++I)
      Memory[Address + I] = Value >> (I * 8);
  }
  bool run(const SpecializationResult &R, const SpecializationOptions &O) {
    if (auto Error = validateSpecializationRawEnvironment(R, O)) {
      Diagnostic = llvm::toString(std::move(Error));
      return false;
    }
    const auto &F = R.Residual;
    int Block = 0;
    for (unsigned Step = 0; Step < 256; ++Step) {
      const auto &B = F.Blocks.at(Block);
      int Next = -1;
      for (const auto &Op : B.Ops) {
        Diagnostic = std::string(ndOpName(Op.Opcode));
        switch (Op.Opcode) {
        case NdOp::INTRINSIC:
          if (Block != 0 || &Op != &B.Ops.front() ||
              !lowRawEnvironmentRequirementShapeIsValid(Op))
            return false;
          break;
        case NdOp::NOP:
          break;
        case NdOp::COPY:
        case NdOp::INT_ZEXT:
          write(Op.Output, read(Op.Inputs[0]));
          break;
        case NdOp::INT_ADD:
          write(Op.Output, read(Op.Inputs[0]) + read(Op.Inputs[1]));
          break;
        case NdOp::INT_SUB:
          write(Op.Output, read(Op.Inputs[0]) - read(Op.Inputs[1]));
          break;
        case NdOp::INT_MULT:
          write(Op.Output, read(Op.Inputs[0]) * read(Op.Inputs[1]));
          break;
        case NdOp::INT_LEFT: {
          const uint64_t Value = read(Op.Inputs[0]);
          const uint64_t Count = read(Op.Inputs[1]);
          if (!Valid)
            return false;
          write(Op.Output, Count < Op.Inputs[0].Size * 8 ? Value << Count : 0);
          break;
        }
        case NdOp::INT_AND:
          write(Op.Output, read(Op.Inputs[0]) & read(Op.Inputs[1]));
          break;
        case NdOp::INT_OR:
          write(Op.Output, read(Op.Inputs[0]) | read(Op.Inputs[1]));
          break;
        case NdOp::INT_XOR:
          write(Op.Output, read(Op.Inputs[0]) ^ read(Op.Inputs[1]));
          break;
        case NdOp::INT_NOT:
          write(Op.Output, ~read(Op.Inputs[0]));
          break;
        case NdOp::BOOL_NOT:
          write(Op.Output, read(Op.Inputs[0]) == 0);
          break;
        case NdOp::SELECT:
          write(Op.Output,
                read(Op.Inputs[0]) ? read(Op.Inputs[1]) : read(Op.Inputs[2]));
          break;
        case NdOp::INT_EQUAL:
        case NdOp::INT_NOTEQUAL:
          write(Op.Output, (read(Op.Inputs[0]) == read(Op.Inputs[1])) ==
                               (Op.Opcode == NdOp::INT_EQUAL));
          break;
        case NdOp::INT_LESS:
          write(Op.Output, read(Op.Inputs[0]) < read(Op.Inputs[1]));
          break;
        case NdOp::INT_SLESS:
        case NdOp::INT_SBOR: {
          if (!Op.Inputs[0].Size || Op.Inputs[0].Size > 8 ||
              Op.Inputs[0].Size != Op.Inputs[1].Size)
            return false;
          const uint64_t A = read(Op.Inputs[0]), B = read(Op.Inputs[1]);
          const uint64_t Sign = uint64_t{1} << (Op.Inputs[0].Size * 8 - 1);
          write(Op.Output, Op.Opcode == NdOp::INT_SLESS
                               ? (A ^ Sign) < (B ^ Sign)
                               : ((A ^ B) & (A ^ (A - B)) & Sign) != 0);
          break;
        }
        case NdOp::SUBBYTES: {
          const auto Byte = read(Op.Inputs[1]);
          write(Op.Output, Byte < 8 ? read(Op.Inputs[0]) >> (8 * Byte) : 0);
          break;
        }
        case NdOp::POPCOUNT: {
          auto Bits = read(Op.Inputs[0]);
          uint64_t Count = 0;
          for (; Bits; Bits >>= 1)
            Count += Bits & 1;
          write(Op.Output, Count);
          break;
        }
        case NdOp::LOAD:
        case NdOp::STORE: {
          const auto M = lowMemoryOperands(Op);
          const va_t Address = read(*M.Address);
          if (Op.Opcode == NdOp::STORE)
            memory(Address, read(*M.StoredValue), M.AccessSize);
          else {
            uint64_t Value = 0;
            for (unsigned I = 0; I < M.AccessSize; ++I) {
              const auto It = Memory.find(Address + I);
              if (It == Memory.end())
                return false;
              Value |= uint64_t{It->second} << (I * 8);
            }
            write(Op.Output, Value);
          }
          break;
        }
        case NdOp::COND_BR: {
          const auto Target = read(Op.Inputs[0]);
          const bool Taken = read(Op.Inputs[1]) != 0;
          for (int Id : B.Succs)
            if ((F.Blocks.at(Id).StartAddr == Target) == Taken)
              Next = Id;
          if (Next < 0)
            return false;
          break;
        }
        case NdOp::BRANCH: {
          const auto Target = read(Op.Inputs[0]);
          for (int Id : B.Succs)
            if (F.Blocks.at(Id).StartAddr == Target)
              Next = Id;
          if (Next < 0)
            return false;
          break;
        }
        case NdOp::RETURN:
          return Valid;
        case NdOp::INDIR_BR:
          NextPC = read(Op.Inputs[0]);
          return Valid;
        default:
          return false;
        }
        if (!Valid)
          return false;
      }
      if (Next < 0) {
        if (B.Succs.size() != 1)
          return false;
        Next = B.Succs.front();
      }
      Block = Next;
    }
    return false;
  }
};

TEST_F(PEFixedImageTest, RawCodePreservesFullStateMemoryAndPhysicalReturnWord) {
  auto Loaded = load(returningImportFixture());
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  Image = std::move(*Loaded);
  const uint64_t GPRs[] = {x86reg::RAX, x86reg::RBX, x86reg::RCX, x86reg::RDX,
                           x86reg::RSI, x86reg::RDI, x86reg::RBP, x86reg::RSP,
                           x86reg::R8,  x86reg::R9,  x86reg::R10, x86reg::R11,
                           x86reg::R12, x86reg::R13, x86reg::R14, x86reg::R15};
  for (va_t Address : {va_t{0x400000}, va_t{0xffff800010000000}}) {
    Options = returningRawOptions(Address);
    const auto R = specializeBinaryInterpreter(Image, Base + 0x1000, Options);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_TRUE(R.RawEnvironment);
    EXPECT_EQ(R.RawEnvironment->Domain->Declaration.X64CodeRegions,
              Options.X64CodeRegions);
    ASSERT_FALSE(R.Reads.empty());
    EXPECT_TRUE(llvm::any_of(R.Origins, [&](const auto &O) {
      return O.NativeInstruction.Address == Address;
    }));
    EXPECT_FALSE(llvm::any_of(R.Reads, [&](const auto &Read) {
      return Read.Address == Address + 0x1000;
    }));
    for (uint32_t Value : {uint32_t{0}, uint32_t{17}, UINT32_MAX})
      for (va_t Stack : {va_t{0x800000}, va_t{0xffff900020000000}}) {
        RawByteOracle Actual;
        for (uint64_t GPR : GPRs)
          Actual.write(NdVar::reg(GPR, 8), 0xaabbccdd00000000 + GPR);
        for (uint64_t Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                              x86reg::SF, x86reg::DF, x86reg::OF})
          Actual.write(NdVar::reg(Flag, 1), Flag & 1);
        Actual.write(NdVar::reg(x86reg::RSP, 8), Stack);
        for (unsigned I = 0; I < 256; ++I)
          Actual.Memory[Stack - 128 + I] = I ^ 0x96;
        Actual.memory(Address + 0x1000, Value, 4);
        Actual.memory(Base + 0x3080, Address, 8);
        auto Expected = Actual;
        const uint64_t SavedRBX = Expected.read(NdVar::reg(x86reg::RBX, 8));
        Expected.memory(Stack - 72, Base + 0x1010, 8);
        Expected.memory(Stack - 80, SavedRBX, 8);
        Expected.memory(Stack - 32, Base + 0x1010, 8);
        Expected.memory(Stack - 24, Value, 4);
        Expected.write(NdVar::reg(x86reg::RAX, 8), 42);
        Expected.write(NdVar::reg(x86reg::RDX, 8), Base + 0x1010);
        Expected.write(NdVar::reg(x86reg::RCX, 8), Stack - 32);
        Expected.write(NdVar::reg(x86reg::R8, 8), Value);
        Expected.write(NdVar::reg(x86reg::R10, 8), 0x1234);
        ASSERT_TRUE(Actual.run(R, Options)) << Actual.Diagnostic;
        std::erase_if(Actual.Registers, [](const auto &V) {
          return V.first.first == VnodeSpace::TEMP;
        });
        EXPECT_EQ(Actual.Registers, Expected.Registers);
        EXPECT_EQ(Actual.Memory, Expected.Memory);
      }
    auto Absent = Options;
    Absent.X64CodeRegions.clear();
    Absent.X64ImportBindings.clear();
    EXPECT_FALSE(
        specializeBinaryInterpreter(Image, Base + 0x1000, Absent).complete());
  }
}

TEST_F(PEFixedImageTest,
       RawCodeUnusedDeclarationsRetainRequirementAndRefuseProof) {
  Options = rawOptions();
  const auto R = specializeBinaryInterpreter(Image, Base + 0x1000, Options);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_FALSE(bool(validateSpecializationRawEnvironment(R, Options)));
  EXPECT_TRUE(R.Reads.empty());
  EXPECT_TRUE(hasLowRawEnvironmentRequirement(R.Residual));
  for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                      InterpreterMachineStateLayout::SharedGPRExit}) {
    auto Wrapped = wrapInterpreterMachineStateX64(
        R.Residual, BinaryFormat::COFF,
        InterpreterMachineStateProfile::UserX64NoFaultV1, std::nullopt, Layout);
    EXPECT_FALSE(bool(Wrapped));
    if (!Wrapped)
      EXPECT_NE(llvm::toString(Wrapped.takeError()).find("raw environment"),
                std::string::npos);
  }
  EXPECT_EQ(proof().Proof.Status, LowIRIndependenceStatus::Unsupported);
  const auto Native = checkBinaryLowIRRefinement(Image, Base + 0x1000, Options,
                                                 R.Residual, Contract);
  EXPECT_EQ(Native.Proof.Status, LowIRRefinementStatus::Unsupported);
  EXPECT_FALSE(Native.Certificate);
  const auto LLVM = checkBinaryLLVMRefinement(
      Image, Base + 0x1000, Options, R.Residual, "", "model", *Contract.Frame);
  EXPECT_EQ(LLVM.Stage, InterpreterLLVMRefinementStage::Native);
  EXPECT_EQ(LLVM.Native.Proof.Status, LowIRRefinementStatus::Unsupported);
}

TEST_F(PEFixedImageTest,
       RawCodeRejectsMalformedAndUnusedDomainsBeforeExecution) {
  auto Loaded = load(returningImportFixture());
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  Image = std::move(*Loaded);
  const auto Good = returningRawOptions(0x400000);
  for (unsigned Case = 0; Case != 27; ++Case) {
    SCOPED_TRACE(Case);
    auto O = Good;
    switch (Case) {
    case 0:
      O.X64CodeRegions[0].Version = 0;
      break;
    case 1:
      O.X64CodeRegions[0].Bytes.clear();
      break;
    case 2:
      O.X64CodeRegions[0].Address = InvalidVA - 1;
      break;
    case 3:
      O.X64CodeRegions[0].Evidence.clear();
      break;
    case 4:
      O.X64CodeRegions[0].Evidence = std::string(4097, 'x');
      break;
    case 5:
      O.X64CodeRegions[0].Evidence[0] = '\0';
      break;
    case 6:
      O.X64CodeRegions.push_back(O.X64CodeRegions[0]);
      break;
    case 7:
      O.X64CodeRegions[0].Address = Base + 0x100;
      break;
    case 8:
      O.X64CodeRegions[0].Address = Base + 0x800;
      break;
    case 9:
      O.X64CodeRegions[0].Address = Base + 0x1100;
      break;
    case 10:
      O.X64CodeRegions[0].Address = Base + 0x4fff;
      break;
    case 11:
      O.X64ImportBindings[0].Version = 2;
      break;
    case 12:
      O.X64ImportBindings[0].Slot = InvalidVA;
      break;
    case 13:
      O.X64ImportBindings[0].Module = "different.dll";
      break;
    case 14:
      O.X64ImportBindings[0].Symbol = "Different";
      break;
    case 15:
      O.X64ImportBindings[0].Target += O.X64CodeRegions[0].Bytes.size();
      break;
    case 16:
      O.X64ImportBindings[0].Evidence.clear();
      break;
    case 17:
      O.X64ImportBindings[0].Module[0] = '\0';
      break;
    case 18:
      O.X64ImportBindings.push_back(O.X64ImportBindings[0]);
      break;
    case 19:
      O.X64TerminalImports.push_back(
          {1, Base + 0x3081, "example.dll", "StopFixture"});
      break;
    case 20:
      O.ExplicitMachineState = false;
      break;
    case 21:
      O.NormalNonfaultingExecution = false;
      break;
    case 22:
      O.X64CetDisabled = false;
      break;
    case 23:
      O.FrameBaseRegister.reset();
      break;
    case 24:
      O.FrameBaseRegister->Offset = x86reg::RAX;
      break;
    case 25:
      O.RequireRestoredFrameAtReturn = false;
      break;
    case 26:
      O.ByteOrder = llvm::endianness::big;
      break;
    }
    // The plain sibling never reaches any supplemental declaration.
    const auto R = specializeBinaryInterpreter(Image, Base + 0x1100, O);
    EXPECT_FALSE(R.complete());
    EXPECT_EQ(R.EvaluatedOperations, 0u);
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_FALSE(R.RawEnvironment);
  }
}

TEST_F(PEFixedImageTest,
       RawCodeExcludesFullPrimaryExtentAndRequiresCompletePE) {
  const auto Good = rawOptions();
  for (va_t Address :
       {Base + 0x100, Base + 0x800, Base + 0x1100, Base + 0x4fff}) {
    auto O = Good;
    O.X64CodeRegions[0].Address = Address;
    const auto R = specializeBinaryInterpreter(Image, Base + 0x1000, O);
    EXPECT_FALSE(R.complete());
    EXPECT_NE(R.Diagnostic.find("overlaps"), std::string::npos);
    EXPECT_EQ(R.EvaluatedOperations, 0u);
  }
  const auto Original = Image;
  for (unsigned Case = 0; Case != 3; ++Case) {
    Image = Original;
    if (Case == 0)
      Image.Raw.clear();
    if (Case == 1)
      Image.Format = BinaryFormat::ELF;
    if (Case == 2)
      Image.LoadOnlyFunctionEntries.insert(Base + 0x1000);
    const auto R = specializeBinaryInterpreter(Image, Base + 0x1000, Good);
    EXPECT_FALSE(R.complete());
    EXPECT_EQ(R.EvaluatedOperations, 0u);
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST_F(PEFixedImageTest, FixedImageOwnsCompleteHeaderDeclaredExtent) {
  auto View = PEFixedImageView::create(Image);
  ASSERT_TRUE(bool(View)) << llvm::toString(View.takeError());
  EXPECT_EQ(View->imageExtent(), (std::pair<va_t, va_t>{Base, Base + 0x5000}));
  for (uint32_t Size : {0u, 1u, 0x3ffu, 0x4100u}) {
    put32(Image.Raw, Optional + 56, Size);
    EXPECT_NE(refusal().find("extent"), std::string::npos);
  }
  put32(Image.Raw, Optional + 56, 0x5000);
  Image.Base = InvalidVA - 0x4fff;
  put64(Image.Raw, Optional + 24, Image.Base);
  EXPECT_NE(refusal().find("extent"), std::string::npos);
}

TEST_F(PEFixedImageTest, RawCodeMissingBytesAndUnknownSuccessorsRefuse) {
  auto Loaded = load(returningImportFixture());
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  Image = std::move(*Loaded);
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{{0x48, 0xb8, 0},
                                                             {0xeb, 0x7f},
                                                             {0xff, 0xe0},
                                                             {0x0f, 0x0b},
                                                             {0x66, 0xc3}}) {
    auto O = returningRawOptions(0x400000);
    O.X64CodeRegions[0].Bytes = Bytes;
    const auto R = specializeBinaryInterpreter(Image, Base + 0x1000, O);
    EXPECT_FALSE(R.complete());
    EXPECT_GT(R.EvaluatedOperations, 0u);
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_FALSE(R.RawEnvironment);
  }
}

TEST_F(PEFixedImageTest, RawCodeBindingRequiresAnExactFullSlotRead) {
  for (const auto &Code : std::vector<std::vector<uint8_t>>{
           {0x8b, 0x05, 0x7a, 0x20, 0, 0, 0xff,
            0xe0}, // mov eax,[slot]; jmp rax
           {0x48, 0x8b, 0x05, 0x7a, 0x20, 0, 0, 0xff, 0xe0}}) { // slot+1
    auto Bytes = returningImportFixture();
    std::copy(Code.begin(), Code.end(), Bytes.begin() + Text);
    auto Loaded = load(Bytes);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = returningRawOptions(0x400000);
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    EXPECT_EQ(R.Status, SpecializationStatus::UnresolvedControl)
        << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST_F(PEFixedImageTest,
       RawCodeImmutableReadAndCallToFallthroughKeepNativeValues) {
  for (va_t Address : {va_t{0x400000}, va_t{0xffff800010000000}}) {
    auto O = rawOptions(Address);
    O.X64CodeRegions[0].Bytes = {0xe8, 0,    0,   0,
                                 0,    0x58, 0xc3}; // call next; pop rax; ret
    const auto R = specializeBinaryInterpreter(Image, Address, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    RawByteOracle Oracle;
    Oracle.write(NdVar::reg(x86reg::RSP, 8), 0x800000);
    ASSERT_TRUE(Oracle.run(R, O)) << Oracle.Diagnostic;
    EXPECT_EQ(Oracle.read(NdVar::reg(x86reg::RAX, 8)), Address + 5);
    EXPECT_EQ(Oracle.read(NdVar::reg(x86reg::RSP, 8)), 0x800000u);
    for (unsigned I = 0; I < 8; ++I)
      EXPECT_EQ(Oracle.Memory.at(0x800000 - 8 + I),
                uint8_t((Address + 5) >> (I * 8)));
    O.X64CodeRegions[0].Bytes = {0x48, 0x8b, 0x05, 1,    0,    0,
                                 0,    0xc3, 0xef, 0xcd, 0xab, 0x89,
                                 0x67, 0x45, 0x23, 0x01};
    const auto Read = specializeBinaryInterpreter(Image, Address, O);
    ASSERT_TRUE(Read.complete()) << Read.Diagnostic;
    ASSERT_EQ(Read.Reads.size(), 1u);
    EXPECT_EQ(Read.Reads[0].Address, Address + 8);
    EXPECT_NE(
        Read.Reads[0].Evidence.find(Read.RawEnvironment->Domain->DomainDigest),
        std::string::npos);
    RawByteOracle ReadOracle;
    ReadOracle.write(NdVar::reg(x86reg::RSP, 8), 0x800000);
    ASSERT_TRUE(ReadOracle.run(Read, O)) << ReadOracle.Diagnostic;
    EXPECT_EQ(ReadOracle.read(NdVar::reg(x86reg::RAX, 8)), 0x0123456789abcdefu);
  }
}

TEST_F(PEFixedImageTest, RawCodeReceiptBindsFrameExclusions) {
  auto Loaded = load(returningImportFixture());
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  Options = returningRawOptions(0x400000);
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{-128, 8};
  Options.EntryFrameBounds->ExcludedAddressRanges = {{0x8000, 0x8100}};
  Options.ProveStoreFrameSeparation = true;
  const auto Good =
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, Options);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Case = 0; Case != 5; ++Case) {
    SCOPED_TRACE(Case);
    auto O = Options;
    auto &Ranges = O.EntryFrameBounds->ExcludedAddressRanges;
    switch (Case) {
    case 0:
      O.ProveStoreFrameSeparation = false;
      break;
    case 1:
      ++Ranges[0].Begin;
      break;
    case 2:
      ++Ranges[0].End;
      break;
    case 3:
      Ranges.clear();
      break;
    case 4:
      Ranges.push_back({0x9000, 0x9100});
      break;
    }
    auto Error = validateSpecializationRawEnvironment(Good, O);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

TEST_F(PEFixedImageTest, RawCodeReceiptRejectsEveryMissingOrChangedSurface) {
  auto Loaded = load(returningImportFixture());
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  Options = returningRawOptions(0x400000);
  const auto Good =
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, Options);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Case = 0; Case != 23; ++Case) {
    SCOPED_TRACE(Case);
    auto R = Good;
    auto O = Options;
    auto &F = R.Residual;
    switch (Case) {
    case 0:
      R.RawEnvironment.reset();
      break;
    case 1:
      R.RawEnvironment->Version = 2;
      break;
    case 2:
      R.RawEnvironment->Domain.reset();
      break;
    case 3:
      F.Blocks[0].Ops.erase(F.Blocks[0].Ops.begin());
      break;
    case 4:
      std::swap(F.Blocks[0].Ops[0], F.Blocks[0].Ops[1]);
      break;
    case 5:
      F.Blocks.back().Ops.push_back(F.Blocks[0].Ops[0]);
      break;
    case 6:
      ++F.Blocks[0].Ops[0].Inputs[2].Offset;
      break;
    case 7:
      F.Blocks[0].Ops[0].NumInputs = 0;
      break;
    case 8:
      F.Blocks[0].Ops[0].Output = NdVar::reg(x86reg::RAX, 8);
      break;
    case 9:
      ++F.Blocks[0].Ops[1].Inputs[5].Offset;
      break;
    case 10:
      F.ModuleAnalysisRoots.insert(0x55);
      break;
    case 11:
      F.OrdinaryModuleAnalysisRoots.clear();
      break;
    case 12:
      F.FunctionTemporaries.push_back({0x55, 8});
      break;
    case 13:
      F.Blocks[0].Succs.clear();
      break;
    case 14:
      ++F.Blocks[1].InstructionBoundaries[0].OpCount;
      break;
    case 15:
      ++R.Origins[0].NativeInstruction.Address;
      break;
    case 16:
      R.Reads.clear();
      break;
    case 17:
      R.RawEnvironment->GraphDigest[0] ^= 1;
      break;
    case 18:
      R.RawEnvironment->EvidenceDigest[0] ^= 1;
      break;
    case 19:
      O.X64CodeRegions[0].Bytes[0] ^= 1;
      break;
    case 20:
      O.X64ImportBindings[0].Evidence[0] ^= 1;
      break;
    case 21:
      O.ExternalStoresPreserveEntryReturnSlot = true;
      break;
    case 22:
      O.ControlRegisters.push_back({x86reg::RAX, 8});
      break;
    }
    auto Error = validateSpecializationRawEnvironment(R, O);
    EXPECT_TRUE(bool(Error));
    if (Error)
      llvm::consumeError(std::move(Error));
  }
  auto Copy = Good;
  EXPECT_FALSE(bool(validateSpecializationRawEnvironment(Copy, Options)));
  for (unsigned Case = 0; Case != 6; ++Case) {
    auto R = Good;
    auto Domain = std::make_shared<SpecializationRawEnvironment>(
        *R.RawEnvironment->Domain);
    R.RawEnvironment->Domain = Domain;
    if (Case == 0)
      Domain->Declaration.X64CodeRegions[0].Bytes[0] ^= 1;
    if (Case == 1)
      Domain->PrimaryImageDigest[0] ^= 1;
    if (Case == 2)
      ++Domain->PrimaryImageEnd;
    if (Case == 3)
      ++Domain->Version;
    if (Case == 4)
      Domain->DomainDigest[0] ^= 1;
    if (Case == 5)
      Domain->Declaration.MaxImagePreparationRecords = UINT64_MAX;
    auto Error = validateSpecializationRawEnvironment(R, Options);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  auto Orphan = Options;
  Orphan.X64CodeRegions.clear();
  Orphan.X64ImportBindings.clear();
  auto Error = validateSpecializationRawEnvironment(Good, Orphan);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}

TEST_F(PEFixedImageTest, RawCodeProviderMustSupplyMatchingPreparedDomain) {
  struct Provider final : SpecializationProvider {
    unsigned Fetches = 0;
    std::shared_ptr<const SpecializationRawEnvironment> Domain;
    std::shared_ptr<const SpecializationRawEnvironment>
    rawEnvironment() const override {
      return Domain;
    }
    llvm::Expected<SpecializationInstruction>
    instruction(SpecializationCursor) override {
      ++Fetches;
      return llvm::createStringError(llvm::errc::not_supported,
                                     "test fetch refused");
    }
  } P;
  Options = rawOptions();
  auto Missing = specializeInterpreter(P, {Base + 0x1000}, Options);
  EXPECT_EQ(Missing.Status, SpecializationStatus::InvalidInput);
  EXPECT_EQ(P.Fetches, 0u);
  const auto Good = specializeBinaryInterpreter(Image, Base + 0x1000, Options);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  P.Domain = Good.RawEnvironment->Domain;
  auto Unexpected = specializeInterpreter(P, {Base + 0x1000}, {});
  EXPECT_EQ(Unexpected.Status, SpecializationStatus::InvalidInput);
  EXPECT_EQ(P.Fetches, 0u);
  auto Changed = Options;
  Changed.MaxSolverQueries++;
  auto Stale = specializeInterpreter(P, {Base + 0x1000}, Changed);
  EXPECT_EQ(Stale.Status, SpecializationStatus::InvalidInput);
  EXPECT_EQ(P.Fetches, 0u);
  auto Valid = specializeInterpreter(P, {Base + 0x1000}, Options);
  EXPECT_EQ(Valid.Status, SpecializationStatus::Unsupported);
  EXPECT_EQ(P.Fetches, 1u);
}

TEST_F(PEFixedImageTest, RawCodePreparationResultAndOperationBudgetsAreExact) {
  auto Loaded = load(returningImportFixture());
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto Image = std::move(*Loaded);
  auto O = rawOptions();
  // Terminal declaration is unused but shares the raw preparation allowance.
  O.X64TerminalImports.push_back(
      {1, Base + 0x3080, "example.dll", "StopFixture"});
  const auto Run = [&](const auto &Options) {
    return specializeBinaryInterpreter(Image, Base + 0x1100, Options);
  };
  ASSERT_TRUE(Run(O).complete());
  for (auto Member : {&SpecializationOptions::MaxImagePreparationBytes,
                      &SpecializationOptions::MaxImagePreparationRecords,
                      &SpecializationOptions::MaxRawEnvironmentResultBytes,
                      &SpecializationOptions::MaxRawEnvironmentResultRecords}) {
    uint64_t Low = 0, High = O.*Member;
    while (Low + 1 < High) {
      auto Probe = O;
      Probe.*Member = Low + (High - Low) / 2;
      const auto R = Run(Probe);
      if (R.complete())
        High = Probe.*Member;
      else {
        ASSERT_EQ(R.Status, SpecializationStatus::BudgetExceeded)
            << R.Diagnostic;
        EXPECT_TRUE(R.Residual.Blocks.empty());
        EXPECT_FALSE(R.RawEnvironment);
        Low = Probe.*Member;
      }
    }
    auto Exact = O;
    Exact.*Member = High;
    EXPECT_TRUE(Run(Exact).complete());
    --(Exact.*Member);
    EXPECT_EQ(Run(Exact).Status, SpecializationStatus::BudgetExceeded);
  }
  auto Exact = O;
  Exact.MaxOperations = Run(O).EvaluatedOperations;
  EXPECT_TRUE(Run(Exact).complete());
  --Exact.MaxOperations;
  const auto Short = Run(Exact);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
  EXPECT_FALSE(Short.RawEnvironment);
}

std::vector<uint8_t> multiImportFixture(unsigned Count) {
  auto B = returningImportFixture();
  put32(B, IData, 0x30c0);
  directory(B, IAT, 0x3080, (Count + 1) * 8);
  std::fill_n(B.begin() + IData + 0x80, (Count + 1) * 8, 0);
  std::fill_n(B.begin() + IData + 0xc0, (Count + 1) * 8, 0);
  for (unsigned I = 0; I < Count; ++I) {
    put64(B, IData + 0x80 + I * 8, 0x3120);
    put64(B, IData + 0xc0 + I * 8, 0x3120);
  }
  putName(B, IData + 0x122, "StopFixture");
  return B;
}

TEST_F(PEFixedImageTest, RawCodeManyMappingsAndBindingsSharePreparationLimit) {
  constexpr unsigned Sections = 128;
  constexpr uint32_t TextRVA = (Sections + 1) * 0x1000;
  auto B = multiImportFixture(2);
  std::vector<uint8_t> Many(0x2000 + Sections * SectionBytes, 0);
  std::copy_n(B.begin(), Text, Many.begin());
  std::copy(B.begin() + Text, B.end(), Many.begin() + 0x2000);
  put16(Many, 0x86, Sections);
  put32(Many, Optional + 60, 0x2000);
  // The enlarged section table also occupies the old text RVA. Move text
  // above every other section so both raw and virtual owners remain disjoint.
  put32(Many, Optional + 16, TextRVA);
  put32(Many, Optional + 20, TextRVA);
  put32(Many, Optional + 56, (Sections + 2) * 0x1000);
  put32(Many, SectionTable + 12, TextRVA);
  for (unsigned I = 0; I < Sections; ++I) {
    const size_t At = SectionTable + I * 40;
    if (I >= 4) {
      putName(Many, At, ".extra");
      put32(Many, At + 8, SectionBytes);
      put32(Many, At + 12, (I + 1) * 0x1000);
      put32(Many, At + 16, SectionBytes);
      put32(Many, At + 36, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    }
    put32(Many, At + 20, 0x2000 + I * SectionBytes);
  }
  auto Loaded = load(Many);
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto View = PEFixedImageView::create(*Loaded);
  ASSERT_TRUE(bool(View)) << llvm::toString(View.takeError());
  for (va_t Slot : {Base + 0x3080, Base + 0x3088}) {
    auto Identity = View->importIdentity(Slot);
    ASSERT_TRUE(Identity);
    EXPECT_EQ(Identity->Symbol, "StopFixture");
  }
  EXPECT_FALSE(View->importIdentity(Base + 0x3090));
  auto O = returningRawOptions(0x400000);
  O.X64ImportBindings.push_back(O.X64ImportBindings[0]);
  O.X64ImportBindings.back().Slot += 8;
  const auto Run = [&](const auto &Options) {
    return specializeBinaryInterpreter(*Loaded, Base + TextRVA + 0x100,
                                       Options);
  };
  ASSERT_TRUE(Run(O).complete());
  uint64_t Low = 0, High = O.MaxImagePreparationRecords;
  while (Low + 1 < High) {
    auto Probe = O;
    Probe.MaxImagePreparationRecords = Low + (High - Low) / 2;
    const auto R = Run(Probe);
    if (R.complete())
      High = Probe.MaxImagePreparationRecords;
    else {
      ASSERT_EQ(R.Status, SpecializationStatus::BudgetExceeded) << R.Diagnostic;
      EXPECT_EQ(R.EvaluatedOperations, 0u);
      Low = Probe.MaxImagePreparationRecords;
    }
  }
  O.MaxImagePreparationRecords = High;
  EXPECT_TRUE(Run(O).complete());
  --O.MaxImagePreparationRecords;
  EXPECT_EQ(Run(O).Status, SpecializationStatus::BudgetExceeded);
}

TEST_F(PEFixedImageTest,
       RawCodeMixedExitsBindFullEvidenceWithinExpectedLimits) {
  for (unsigned Count : {2u, 4u}) {
    auto B = multiImportFixture(Count);
    const uint8_t Code[] = {0x85, 0xc9, 0x74, 6,    0xff, 0x15, 0, 0,
                            0,    0,    0xff, 0x15, 0,    0,    0, 0};
    std::copy(std::begin(Code), std::end(Code), B.begin() + Text);
    put32(B, Text + 6, 0x3080 - 0x100a);
    put32(B, Text + 12, 0x3088 - 0x1010);
    auto Loaded = load(B);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = rawOptions();
    for (unsigned I = 0; I < Count; ++I)
      O.X64TerminalImports.push_back(
          {1, Base + 0x3080 + I * 8, "example.dll", "StopFixture"});
    const auto Run = [&](const auto &Options) {
      return specializeBinaryInterpreter(*Loaded, Base + 0x1000, Options);
    };
    const auto Good = Run(O);
    ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
    ASSERT_EQ(Good.ExternalExits.size(), 2u);
    EXPECT_FALSE(bool(validateSpecializationRawEnvironment(Good, O)));
    auto Changed = Good;
    // The last inactive slot v1 encodes and every extension slot.
    for (size_t Slot = kLowUndefinedV1InputSlots - 1;
         Slot != kLowOperationInputs; ++Slot) {
      SCOPED_TRACE(Slot);
      Changed = Good;
      Changed.ExternalExits[0].Original.Ops[0].Inputs[Slot].Offset ^= 1;
      auto Error = validateSpecializationRawEnvironment(Changed, O);
      ASSERT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
    Changed = Good;
    Changed.ExternalExits[0].Original.UndefinedEffects.Diagnostic = "changed";
    auto Error = validateSpecializationRawEnvironment(Changed, O);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    uint64_t Low = 0, High = O.MaxRawEnvironmentResultRecords;
    while (Low + 1 < High) {
      auto Probe = O;
      Probe.MaxRawEnvironmentResultRecords = Low + (High - Low) / 2;
      const auto R = Run(Probe);
      if (R.complete())
        High = Probe.MaxRawEnvironmentResultRecords;
      else {
        ASSERT_EQ(R.Status, SpecializationStatus::BudgetExceeded)
            << R.Diagnostic;
        EXPECT_TRUE(R.Residual.Blocks.empty());
        EXPECT_FALSE(R.RawEnvironment);
        Low = Probe.MaxRawEnvironmentResultRecords;
      }
    }
    O.MaxRawEnvironmentResultRecords = High;
    EXPECT_TRUE(Run(O).complete());
    --O.MaxRawEnvironmentResultRecords;
    EXPECT_EQ(Run(O).Status, SpecializationStatus::BudgetExceeded);
  }
}

TEST_F(PEFixedImageTest, RawCodeStoredBudgetsCannotAuthorizeOversizedEvidence) {
  Options = rawOptions();
  const auto Good = specializeBinaryInterpreter(Image, Base + 0x1000, Options);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Case = 0; Case != 5; ++Case) {
    auto R = Good;
    auto Expected = Options;
    Expected.MaxRawEnvironmentResultRecords = 128;
    if (Case == 0)
      R.Origins.resize(129);
    if (Case == 1)
      R.ExternalExits.resize(129);
    if (Case == 2)
      R.Reads.resize(129);
    if (Case == 3)
      R.Residual.Blocks.resize(129);
    if (Case == 4)
      R.Residual.Blocks[0].Ops.resize(129);
    auto Error = validateSpecializationRawEnvironment(R, Expected);
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("validation budget"),
              std::string::npos);
  }
  auto R = Good;
  auto Domain =
      std::make_shared<SpecializationRawEnvironment>(*R.RawEnvironment->Domain);
  Domain->Declaration.MaxImagePreparationBytes = UINT64_MAX;
  Domain->Declaration.X64CodeRegions[0].Evidence.assign(16384, 'x');
  R.RawEnvironment->Domain = Domain;
  auto Error = validateSpecializationRawEnvironment(R, Options);
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("expected budgets"),
            std::string::npos);
}

TEST_F(PEFixedImageTest, RawCodeDoesNotInsertAnImplicitExternalStoreContract) {
  auto B = returningImportFixture();
  const uint8_t Code[] = {0x48, 0x89, 0x01, 0xc3}; // mov [rcx],rax; ret
  std::copy(std::begin(Code), std::end(Code), B.begin() + Text);
  auto Loaded = load(B);
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = rawOptions();
  const auto Refused = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  EXPECT_FALSE(Refused.complete());
  EXPECT_TRUE(Refused.Residual.Blocks.empty());
  O.ExternalStoresPreserveEntryReturnSlot = true;
  const auto Declared = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Declared.complete()) << Declared.Diagnostic;
  EXPECT_TRUE(Declared.RawEnvironment->Domain->Declaration
                  .ExternalStoresPreserveEntryReturnSlot);
  auto Ordinary = rawOptions();
  Ordinary.X64CodeRegions.clear();
  // The pre-existing ordinary binary interface keeps its historical domain.
  EXPECT_TRUE(
      specializeBinaryInterpreter(*Loaded, Base + 0x1000, Ordinary).complete());
}

TEST_F(PEFixedImageTest, RawCodeRefinementKeepsTheOriginalCallerDeclaration) {
  constexpr va_t Address = 0x400000;
  auto O = rawOptions(Address);
  // Project each decoded instruction separately. This makes both assignments
  // reach the shared indirect-transfer node before a control field is learned.
  O.MaxChainedTransfers = 0;
  O.DiscoverControlState = true;
  auto &B = O.X64CodeRegions[0].Bytes;
  B.assign(0x446, 0xcc);
  const uint8_t Test[] = {0x48, 0x85, 0xc9, 0x0f, 0x84, 0, 0, 0, 0};
  std::copy(std::begin(Test), std::end(Test), B.begin());
  put32(B, 5, 0x100 - 9);
  const auto Jump = [&](size_t From, size_t To) {
    B[From] = 0xe9;
    put32(B, From + 1, uint32_t(To - (From + 5)));
  };
  Jump(9, 0x200);
  Jump(0x100, 0x140);
  Jump(0x200, 0x240);
  Jump(0x240, 0x280);
  Jump(0x280, 0x2c0);
  for (const auto [At, Target] :
       {std::pair{0x140, 0x400}, std::pair{0x2c0, 0x440}}) {
    B[At] = 0x49;
    B[At + 1] = 0xba; // movabs r10, target
    put64(B, At + 2, Address + Target);
    Jump(At + 10, 0x300);
  }
  B[0x300] = 0x41;
  B[0x301] = 0xff;
  B[0x302] = 0xe2; // jmp r10
  for (const auto [At, Value] : {std::pair{0x400, 11}, std::pair{0x440, 29}}) {
    B[At] = 0xb8;
    put32(B, At + 1, Value);
    B[At + 5] = 0xc3;
  }
  const auto R = specializeBinaryInterpreter(Image, Address, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_GT(R.ControlRefinements, 0u);
  EXPECT_TRUE(R.RawEnvironment->Domain->Declaration.ControlRegisters.empty());
  EXPECT_TRUE(R.RawEnvironment->Domain->Declaration.ControlFrameSlots.empty());
  EXPECT_FALSE(bool(validateSpecializationRawEnvironment(R, O)));
  for (va_t Return : {Address + 0x405, Address + 0x445})
    EXPECT_TRUE(llvm::any_of(R.Origins, [&](const auto &Origin) {
      return Origin.NativeInstruction.Address == Return;
    }));
}

SpecializationOptions rawReturnStoreOptions(va_t Address, va_t Destination) {
  auto O = rawOptions(Address);
  O.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
  O.ExternalStoresPreserveEntryReturnSlot = true;
  O.ProveStoreReturnSlotSeparation = true;
  auto &B = O.X64CodeRegions[0].Bytes;
  B.assign(0x52, 0xcc);
  const uint8_t Reserve[] = {0x48, 0x8d, 0x64, 0x24, 0xf8};
  const uint8_t Restore[] = {0x48, 0x8d, 0x64, 0x24, 0x08};
  for (size_t At : {size_t{0}, size_t{0x20}}) {
    std::copy(std::begin(Reserve), std::end(Reserve), B.begin() + At);
    B[At + 5] = 0xe8;
    put32(B, At + 6, 0x20 - 10);
    std::copy(std::begin(Restore), std::end(Restore), B.begin() + At + 10);
    B[At + 15] = 0xc3;
  }
  B[0x40] = 0x48;
  B[0x41] = 0xba; // movabs rdx, destination
  put64(B, 0x42, Destination);
  B[0x4a] = 0x89;
  B[0x4b] = 0x0a; // mov [rdx],ecx
  B[0x4c] = 0xb8;
  put32(B, 0x4d, 37);
  B[0x51] = 0xc3;
  return O;
}

TEST_F(PEFixedImageTest,
       RawCodeNativeReturnCellsSurviveOnlyProvedDisjointStore) {
  constexpr va_t Address = 0x400000;
  for (unsigned Variant = 0; Variant != 3; ++Variant) {
    SCOPED_TRACE(Variant);
    const bool DiscardAliasingCell = Variant == 1;
    const bool Address32 = Variant == 2;
    for (va_t Destination : {va_t{0x8000}, va_t{0xfffff80000408000}}) {
      auto O = rawReturnStoreOptions(Address, Destination);
      const va_t StoreAddress = Address32 ? uint32_t(Destination) : Destination;
      if (Address32)
        // The decoder/lifter truncates EDX then zero-extends the actual
        // address.
        O.X64CodeRegions[0].Bytes.insert(
            O.X64CodeRegions[0].Bytes.begin() + 0x4a, 0x67);
      if (DiscardAliasingCell) {
        // The inner return word may alias the store. This path deliberately
        // discards it and jumps back; only the outer saved word must survive.
        auto &B = O.X64CodeRegions[0].Bytes;
        B[0x24] = B[0x2e] = 0;
        B.resize(0x5b);
        const uint8_t Discard[] = {0x48, 0x8d, 0x64, 0x24, 0x08, 0xe9};
        std::copy(std::begin(Discard), std::end(Discard), B.begin() + 0x51);
        put32(B, 0x57, uint32_t(0x2a - 0x5b));
      }
      const auto R = specializeBinaryInterpreter(Image, Address, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      EXPECT_FALSE(bool(validateSpecializationRawEnvironment(R, O)));
      EXPECT_FALSE(O.ExternalStoresDisjointEntryFrame);
      EXPECT_FALSE(O.EntryFrameBounds);
      for (va_t Stack : {va_t{0x200008}, va_t{0xffff900020000008},
                         StoreAddress + 24, va_t{8}, va_t{UINT64_MAX - 7}})
        for (uint32_t Value : {uint32_t{0}, uint32_t{17}, UINT32_MAX}) {
          RawByteOracle Actual;
          for (uint64_t Reg = 0; Reg < 128; Reg += 8)
            Actual.write(NdVar::reg(Reg, 8), 0xaabbccdd00000000 + Reg);
          for (uint64_t Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                                x86reg::SF, x86reg::DF, x86reg::OF})
            Actual.write(NdVar::reg(Flag, 1), Flag & 1);
          Actual.write(NdVar::reg(x86reg::RSP, 8), Stack);
          Actual.write(NdVar::reg(x86reg::RCX, 8), Value);
          for (unsigned I = 0; I < 64; ++I)
            Actual.Memory[Stack - 48 + I] = I ^ 0x6d;
          Actual.memory(Destination, 0x11223344, 4);
          Actual.memory(StoreAddress, 0x55667788, 4);
          auto Expected = Actual;
          Expected.memory(Stack - 16, Address + 10, 8);
          Expected.memory(Stack - (DiscardAliasingCell ? 24 : 32),
                          Address + 0x2a, 8);
          Expected.memory(StoreAddress, Value, 4);
          Expected.write(NdVar::reg(x86reg::RAX, 8), 37);
          Expected.write(NdVar::reg(x86reg::RDX, 8), Destination);
          ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
          std::erase_if(Actual.Registers, [](const auto &V) {
            return V.first.first == VnodeSpace::TEMP;
          });
          EXPECT_EQ(Actual.Registers, Expected.Registers);
          EXPECT_EQ(Actual.Memory, Expected.Memory);
        }
    }
  }
}

TEST_F(PEFixedImageTest, RawCodeNativeReturnProofDoesNotInventSeparation) {
  constexpr va_t Address = 0x400000;
  for (unsigned Case = 0; Case != 7; ++Case) {
    SCOPED_TRACE(Case);
    auto O = rawReturnStoreOptions(Address, 0x8000);
    auto &B = O.X64CodeRegions[0].Bytes;
    if (Case == 0)
      O.ProveStoreReturnSlotSeparation = false;
    if (Case == 1)
      O.EntryFrameAlignment.reset();
    if (Case == 2)
      put64(B, 0x42, 0x8008); // possible full cell overlap
    if (Case == 3)
      put64(B, 0x42, 0x8007); // one-byte straddling overlap
    if (Case == 4) {
      // An arbitrary incoming pointer may reach an internal return cell even
      // under the older entry-slot-only external-store domain.
      B.erase(B.begin() + 0x40, B.begin() + 0x4a);
    }
    if (Case == 5) {
      const uint8_t Store[] = {0x89, 0x4c, 0x24, 3}; // mov [rsp+3],ecx
      std::copy(std::begin(Store), std::end(Store), B.begin() + 0x40);
      B.erase(B.begin() + 0x44, B.begin() + 0x4c);
    }
    if (Case == 6)
      // The outer cell is provably disjoint, but the inner cell may alias.
      // Retaining the inner RET must refuse even after proving the outer cell.
      B[0x24] = B[0x2e] = 0;
    const auto R = specializeBinaryInterpreter(Image, Address, O);
    EXPECT_EQ(R.Status, SpecializationStatus::UnresolvedControl)
        << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_FALSE(R.RawEnvironment);
  }
}

TEST_F(PEFixedImageTest,
       RawCodeNativeReturnProofSharesExactWorkAndQueryBudgets) {
  constexpr va_t Address = 0x400000;
  const auto O = rawReturnStoreOptions(Address, 0x8000);
  const auto Good = specializeBinaryInterpreter(Image, Address, O);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  ASSERT_GT(Good.SolverQueries, 0u);
  for (bool Queries : {false, true}) {
    auto Exact = O;
    (Queries ? Exact.MaxSolverQueries : Exact.MaxOperations) =
        Queries ? Good.SolverQueries : Good.EvaluatedOperations;
    const auto R = specializeBinaryInterpreter(Image, Address, Exact);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_FALSE(bool(validateSpecializationRawEnvironment(R, Exact)));
    --(Queries ? Exact.MaxSolverQueries : Exact.MaxOperations);
    const auto Short = specializeBinaryInterpreter(Image, Address, Exact);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded)
        << Short.Diagnostic;
    EXPECT_TRUE(Short.Residual.Blocks.empty());
    EXPECT_FALSE(Short.RawEnvironment);
  }
}

SpecializationOptions rawFrameNormalizationOptions(va_t Address) {
  auto O = rawOptions(Address);
  O.MaxSolverConflicts = 1;
  O.MaxFrameOffsetNormalizationWorkPerProof = 65536;
  auto &Bytes = O.X64CodeRegions[0].Bytes;
  Bytes.assign(0x20, 0xcc);
  Bytes[0] = 0xe8;
  put32(Bytes, 1, 0x1b); // call the independent callee
  Bytes[5] = 0xc3;
  const uint8_t Callee[] = {
      0x48, 0x89, 0xe0,             // mov rax,rsp
      0x48, 0x89, 0xe2,             // mov rdx,rsp
      0x48, 0x83, 0xe0, 0x20,       // and rax,32
      0x48, 0x83, 0xca, 0x20,       // or rdx,32
      0x48, 0x31, 0xd0,             // xor rax,rdx
      0x48, 0x83, 0xf0, 0x20,       // xor rax,32: original RSP
      0x48, 0x8d, 0x40, 0xe0,       // lea rax,[rax-32]
      0x48, 0x89, 0x08,             // mov [rax],rcx
      0x4c, 0x8b, 0x44, 0x24, 0xe0, // mov r8,[rsp-32]
      0x48, 0x8b, 0x14, 0x24,       // mov rdx,[rsp]: physical return word
      0x4d, 0x39, 0xdb,             // cmp r11,r11: all arithmetic flags defined
      0xc3};
  Bytes.insert(Bytes.end(), std::begin(Callee), std::end(Callee));
  return O;
}

TEST_F(PEFixedImageTest,
       RawCodeFrameNormalizationRetainsMemoryAndReturnEffects) {
  constexpr va_t Address = 0x400000;
  const auto O = rawFrameNormalizationOptions(Address);
  auto DisabledOptions = O;
  DisabledOptions.MaxFrameOffsetNormalizationWorkPerProof = 0;
  const auto Disabled =
      specializeBinaryInterpreter(Image, Address, DisabledOptions);
  EXPECT_EQ(Disabled.Status, SpecializationStatus::BudgetExceeded)
      << Disabled.Diagnostic;
  EXPECT_TRUE(Disabled.Residual.Blocks.empty());
  const auto R = specializeBinaryInterpreter(Image, Address, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_FALSE(bool(validateSpecializationRawEnvironment(R, O)));
  for (va_t Stack :
       {va_t{8}, va_t{0x200008}, va_t{0xffff900040000008}, UINT64_MAX - 7})
    for (uint64_t Value :
         {uint64_t{0}, uint64_t{0x123456789abcdef0}, UINT64_MAX}) {
      RawByteOracle Actual;
      for (uint64_t Reg = 0; Reg < 128; Reg += 8)
        Actual.write(NdVar::reg(Reg, 8), 0xabcd000000000000 + Reg);
      for (uint64_t Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                            x86reg::SF, x86reg::DF, x86reg::OF})
        Actual.write(NdVar::reg(Flag, 1), Flag & 1);
      Actual.write(NdVar::reg(x86reg::RSP, 8), Stack);
      Actual.write(NdVar::reg(x86reg::RCX, 8), Value);
      for (unsigned I = 0; I < 64; ++I)
        Actual.Memory[Stack - 48 + I] = I ^ 0x73;
      auto Expected = Actual;
      Expected.memory(Stack - 8, Address + 5, 8);
      Expected.memory(Stack - 40, Value, 8);
      Expected.write(NdVar::reg(x86reg::RAX, 8), Stack - 40);
      Expected.write(NdVar::reg(x86reg::RDX, 8), Address + 5);
      Expected.write(NdVar::reg(x86reg::R8, 8), Value);
      for (uint64_t Flag : {x86reg::CF, x86reg::AF, x86reg::SF, x86reg::OF})
        Expected.write(NdVar::reg(Flag, 1), 0);
      for (uint64_t Flag : {x86reg::PF, x86reg::ZF})
        Expected.write(NdVar::reg(Flag, 1), 1);
      ASSERT_TRUE(Actual.run(R, O)) << Actual.Diagnostic;
      std::erase_if(Actual.Registers, [](const auto &V) {
        return V.first.first == VnodeSpace::TEMP;
      });
      EXPECT_EQ(Actual.Registers, Expected.Registers);
      EXPECT_EQ(Actual.Memory, Expected.Memory);
    }
  auto Exact = O;
  Exact.MaxOperations = R.EvaluatedOperations;
  bool FoundBoundary = false;
  for (unsigned Attempt = 0; Attempt != 128; ++Attempt) {
    const auto Boundary = specializeBinaryInterpreter(Image, Address, Exact);
    ASSERT_TRUE(Boundary.complete()) << Boundary.Diagnostic;
    ASSERT_LE(Boundary.EvaluatedOperations, Exact.MaxOperations);
    ASSERT_GT(Boundary.EvaluatedOperations, 1u);
    auto ShortOptions = Exact;
    ShortOptions.MaxOperations = Boundary.EvaluatedOperations - 1;
    const auto Short =
        specializeBinaryInterpreter(Image, Address, ShortOptions);
    if (!Short.complete()) {
      Exact.MaxOperations = ShortOptions.MaxOperations + 1;
      const auto Tight = specializeBinaryInterpreter(Image, Address, Exact);
      ASSERT_TRUE(Tight.complete()) << Tight.Diagnostic;
      EXPECT_LE(Tight.EvaluatedOperations, Exact.MaxOperations);
      EXPECT_FALSE(bool(validateSpecializationRawEnvironment(Tight, Exact)));
      const auto Repeated =
          specializeBinaryInterpreter(Image, Address, ShortOptions);
      EXPECT_EQ(Repeated.Status, SpecializationStatus::BudgetExceeded)
          << Repeated.Diagnostic;
      EXPECT_EQ(Short.Status, Repeated.Status);
      EXPECT_EQ(Short.EvaluatedOperations, Repeated.EvaluatedOperations);
      EXPECT_TRUE(Short.Residual.Blocks.empty());
      EXPECT_TRUE(Repeated.Residual.Blocks.empty());
      EXPECT_FALSE(Short.RawEnvironment);
      EXPECT_FALSE(Repeated.RawEnvironment);
      FoundBoundary = true;
      break;
    }
    ASSERT_LE(Short.EvaluatedOperations, ShortOptions.MaxOperations);
    Exact.MaxOperations = Short.EvaluatedOperations;
  }
  EXPECT_TRUE(FoundBoundary);
  auto Changed = O;
  ++Changed.MaxFrameOffsetNormalizationWorkPerProof;
  auto Error = validateSpecializationRawEnvironment(R, Changed);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  auto Modified = R;
  auto Domain =
      std::make_shared<SpecializationRawEnvironment>(*R.RawEnvironment->Domain);
  Domain->Declaration.MaxFrameOffsetNormalizationWorkPerProof = 0;
  Modified.RawEnvironment->Domain = Domain;
  Error = validateSpecializationRawEnvironment(Modified, O);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}

TEST_F(PEFixedImageTest, RawCodeFrameNormalizationCannotDropBusinessInput) {
  constexpr va_t Address = 0x400000;
  for (bool ShortWork : {false, true}) {
    auto O = rawFrameNormalizationOptions(Address);
    if (ShortWork) {
      O.MaxFrameOffsetNormalizationWorkPerProof = 1;
    } else {
      const uint8_t AddInput[] = {0x48, 0x8d, 0x04, 0x38}; // lea rax,[rax+rdi]
      auto &Bytes = O.X64CodeRegions[0].Bytes;
      Bytes.insert(Bytes.begin() + 0x20 + 25, std::begin(AddInput),
                   std::end(AddInput));
    }
    const auto R = specializeBinaryInterpreter(Image, Address, O);
    EXPECT_FALSE(R.complete()) << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_FALSE(R.RawEnvironment);
  }
}

TEST_F(PEFixedImageTest, RawCodeCmpxchgReceiptBindsOrdering) {
  constexpr va_t Address = 0x400000;
  auto O = rawOptions(Address);
  // LOCK CMPXCHG qword ptr [rsp-8],rdx; RET. Use the existing complete fixed
  // image plus an owned supplemental code declaration, not a forged receipt.
  O.X64CodeRegions[0].Bytes = {0xf0, 0x48, 0x0f, 0xb1, 0x54, 0x24, 0xf8, 0xc3};
  ASSERT_FALSE(O.ExternalStoresPreserveEntryReturnSlot);
  const auto Good = specializeBinaryInterpreter(Image, Address, O);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  ASSERT_TRUE(Good.RawEnvironment);
  ASSERT_TRUE(Good.RawEnvironment->Domain);
  EXPECT_FALSE(Good.RawEnvironment->Domain->Declaration
                   .ExternalStoresPreserveEntryReturnSlot);
  auto Valid = validateSpecializationRawEnvironment(Good, O);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Good;
    unsigned Atomics = 0;
    for (auto &B : Changed.Residual.Blocks)
      for (auto &Op : B.Ops)
        if (Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
          ++Atomics;
          ASSERT_EQ(Op.NumInputs, 3u);
          ASSERT_EQ(Op.Inputs[2].Size, 8u);
          ASSERT_EQ(Op.MemoryOrdering,
                    NdMemoryOrdering::SequentiallyConsistent);
          ASSERT_EQ(Op.MemoryAddressSpace, NdMemoryAddressSpace::Default);
          if (Mutation == 0)
            Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
          else if (Mutation == 1)
            Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
          else
            Op.Inputs[2] = NdVar::scalar(0x12345678, 8);
        }
    ASSERT_EQ(Atomics, 1u);
    auto Error = validateSpecializationRawEnvironment(Changed, O);
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(
        llvm::toString(std::move(Error)).find("graph differs from its receipt"),
        std::string::npos);
  }
  auto Copy = Good;
  auto ValidCopy = validateSpecializationRawEnvironment(Copy, O);
  EXPECT_FALSE(bool(ValidCopy)) << llvm::toString(std::move(ValidCopy));
}

#include "MachineExitEnvironmentTests.inc"
#include "WideImmutableImageTests.inc"

// Entry-object fixtures reuse the machine-exit byte oracle above.
#include "EntryObjectEnvironmentTests.inc"
