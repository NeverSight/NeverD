//===- KernelInterruptRefinementTests.inc - Mandatory IF observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Supervisor flag changes remain visible to original/residual proofs.

TEST(BinaryLowIRRefinement, KernelInterruptControlsCompareMandatoryFinalIF) {
  for (uint8_t Opcode : {uint8_t{0xfa}, uint8_t{0xfb}}) {
    Program P({Opcode, 0xc3});
    P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile =
        InterpreterMachineStateProfile::KernelX64NoFaultV1;
    auto R = P.recover();
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    P.Contract.ReturnRegisters.clear();
    P.Contract.ObserveWrittenFrameBytes = false;
    const auto Identity = P.check(R.Residual);
    ASSERT_TRUE(Identity.proved()) << Identity.Proof.Diagnostic;
    unsigned Changed = 0;
    for (auto &B : R.Residual.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::INTRINSIC) {
          O.Inputs[0] = NdVar::scalar(
              static_cast<uint64_t>(Opcode == 0xfa ? Intrinsic::Sti
                                                   : Intrinsic::Cli),
              2);
          ++Changed;
        }
    ASSERT_EQ(Changed, 1U);
    const auto Different = P.check(R.Residual);
    refused(Different, Status::Different);
    EXPECT_NE(Different.Proof.Diagnostic.find("final system flags"),
              std::string::npos);
  }
}

TEST(BinaryLowIRRefinement, KernelInterruptControlsRejectMalformedCandidate) {
  Program P({0xfa, 0xc3});
  P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile =
      InterpreterMachineStateProfile::KernelX64NoFaultV1;
  const auto Good = P.recover();
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 6; ++Kind) {
    auto Candidate = Good.Residual;
    unsigned Changed = 0;
    for (auto &B : Candidate.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::INTRINSIC) {
          if (Kind == 0)
            O.Output = NdVar::reg(x86reg::RAX, 8);
          else if (Kind == 1)
            O.addInput(NdVar::scalar(0, 8));
          else if (Kind == 2)
            O.Inputs[0].Size = 1;
          else if (Kind == 3)
            O.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
          else if (Kind == 4)
            O.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
          else
            O.Inputs[0] = NdVar::reg(x86reg::RAX, 2);
          ++Changed;
        }
    ASSERT_EQ(Changed, 1U);
    const auto Bad = P.check(Candidate);
    EXPECT_FALSE(Bad.proved()) << "kind=" << Kind;
    EXPECT_FALSE(Bad.Certificate);
    EXPECT_FALSE(Bad.Proof.Certificate);
  }
}

TEST(BinaryLowIRRefinement, SmapControlsCompareMandatoryFinalAC) {
  for (uint8_t Opcode : {uint8_t{0xca}, uint8_t{0xcb}}) {
    Program P({0x0f, 0x01, Opcode, 0xc3});
    P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile =
        InterpreterMachineStateProfile::KernelX64SmapNoFaultV2;
    auto R = P.recover();
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    P.Contract.ReturnRegisters.clear();
    P.Contract.ObserveWrittenFrameBytes = false;
    const auto Identity = P.check(R.Residual);
    ASSERT_TRUE(Identity.proved()) << Identity.Proof.Diagnostic;
    unsigned Changed = 0;
    for (auto &B : R.Residual.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::INTRINSIC) {
          O.Inputs[0] = NdVar::scalar(
              static_cast<uint64_t>(Opcode == 0xca ? Intrinsic::Stac
                                                   : Intrinsic::Clac),
              2);
          ++Changed;
        }
    ASSERT_EQ(Changed, 1u);
    const auto Different = P.check(R.Residual);
    refused(Different, Status::Different);
    EXPECT_NE(Different.Proof.Diagnostic.find("final system flags"),
              std::string::npos);
  }
}

TEST(BinaryLowIRRefinement, SmapEntryACRemainsAnIndependentInput) {
  // pushfq; pop rax; and eax,40000h; ret. All other input flag bits are
  // removed from the returned value, so only an incoming AC=1 distinguishes
  // this from the deliberately broken candidate returning zero.
  Program P({0x9c, 0x58, 0x25, 0, 0, 4, 0, 0xc3});
  P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile =
      InterpreterMachineStateProfile::KernelX64SmapNoFaultV2;
  auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Identity = P.check(R.Residual);
  ASSERT_TRUE(Identity.proved()) << Identity.Proof.Diagnostic;
  unsigned Changed = 0;
  for (auto &B : R.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::INT_AND && O.NumInputs == 2 &&
          O.Inputs[1].isConst() && O.Inputs[1].Offset == 0x40000) {
        O.Inputs[1] = NdVar::scalar(0, O.Inputs[1].Size);
        ++Changed;
      }
  ASSERT_EQ(Changed, 1u);
  refused(P.check(R.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, SmapControlsRequireTheirDeclaredProfile) {
  for (auto Profile : {InterpreterMachineStateProfile::UserX64NoFaultV1,
                       InterpreterMachineStateProfile::KernelX64NoFaultV1})
    for (uint8_t Opcode : {uint8_t{0xca}, uint8_t{0xcb}}) {
      Program P({0x0f, 0x01, Opcode, 0xc3});
      P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile = Profile;
      const auto R = P.recover();
      EXPECT_FALSE(R.complete());
    }
}

TEST(BinaryLowIRRefinement, SmapPopFlagsRetainsItsACWrite) {
  // mov ecx,40000h; push rcx; popfq; ret. AC is the only changed system bit.
  Program P({0xb9, 0, 0, 4, 0, 0x51, 0x9d, 0xc3});
  P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile =
      InterpreterMachineStateProfile::KernelX64SmapNoFaultV2;
  auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  P.Contract.ReturnRegisters.clear();
  P.Contract.ObserveWrittenFrameBytes = false;
  const auto Identity = P.check(R.Residual);
  ASSERT_TRUE(Identity.proved()) << Identity.Proof.Diagnostic;
  unsigned Changed = 0;
  for (auto &B : R.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::INTRINSIC && O.NumInputs == 2 &&
          O.Inputs[0].isConst() &&
          O.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Popf)) {
        O.Inputs[1] = NdVar::scalar(0, 8);
        ++Changed;
      }
  ASSERT_EQ(Changed, 1u);
  const auto Different = P.check(R.Residual);
  refused(Different, Status::Different);
  EXPECT_NE(Different.Proof.Diagnostic.find("final system flags"),
            std::string::npos);
}

TEST(BinaryLowIRRefinement, SmapControlsRejectMalformedCandidate) {
  Program P({0x0f, 0x01, 0xcb, 0xc3});
  P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile =
      InterpreterMachineStateProfile::KernelX64SmapNoFaultV2;
  const auto Good = P.recover();
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 6; ++Kind) {
    auto Candidate = Good.Residual;
    unsigned Changed = 0;
    for (auto &B : Candidate.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::INTRINSIC) {
          if (Kind == 0)
            O.Output = NdVar::reg(x86reg::RAX, 8);
          else if (Kind == 1)
            O.addInput(NdVar::scalar(0, 8));
          else if (Kind == 2)
            O.Inputs[0].Size = 1;
          else if (Kind == 3)
            O.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
          else if (Kind == 4)
            O.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
          else
            O.Inputs[0] = NdVar::reg(x86reg::RAX, 2);
          ++Changed;
        }
    ASSERT_EQ(Changed, 1u);
    const auto Bad = P.check(Candidate);
    EXPECT_FALSE(Bad.proved()) << "kind=" << Kind;
    EXPECT_FALSE(Bad.Certificate);
    EXPECT_FALSE(Bad.Proof.Certificate);
  }
}
