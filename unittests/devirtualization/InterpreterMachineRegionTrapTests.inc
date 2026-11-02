//===- InterpreterMachineRegionTrapTests.inc - Pre-trap source exits ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent pre-trap state oracles for checked region source.
// Included after InterpreterMachineRegionSourceTests.inc in its namespace.

TEST_F(InterpreterMachineRegionSourceTest,
       PendingTrapKeepsPreInstructionState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "region source execution requires clang";
  for (const auto &Trap :
       std::vector<std::vector<uint8_t>>{{0xcc}, {0xf1}, {0xcd, 3}}) {
    // MOV [RCX],RAX; MOVDQU XMM0,XMM1; trap. Bytes after the trap are not
    // instructions in this region and must not execute or supply a resume PC.
    std::vector<uint8_t> Code{0x48, 0x89, 0x01, 0xf3, 0x0f, 0x6f, 0xc1};
    Code.insert(Code.end(), Trap.begin(), Trap.end());
    Code.insert(Code.end(), {0x0f, 0x0b});
    auto Image = loadRegion(Code);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Options = regionOptions(Entry + 7, ExitKind::Trap);
    Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    auto Default = wrapInterpreterMachineRegionSourceX64(Result, Options);
    ASSERT_FALSE(bool(Default));
    EXPECT_FALSE(llvm::toString(Default.takeError()).empty());
    InterpreterMachineRegionSourceOptions SourceOptions;
    SourceOptions.StopBeforeTraps = true;
    auto Wrapped =
        wrapInterpreterMachineRegionSourceX64(Result, Options, SourceOptions);
    ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
    EXPECT_EQ(Wrapped->Environment.IdentityDigest,
              Result.RawEnvironment->IdentityDigest);
    Wrapped->Source.Function.Name = "generic_machine_region";
    emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  for(unsigned trial=0;trial<8;++trial) {
    struct {uint64_t before,s[54],after;} box,expected;
    _Alignas(16) unsigned char stack[512],expected_stack[512];
    memset(&box,0,sizeof(box));
    box.before=0x12345678;box.after=0xabcdef01;
    for(unsigned i=0;i<54;++i) box.s[i]=UINT64_C(0x9234567810000000)+i+trial;
    for(unsigned i=0;i<512;++i) stack[i]=(unsigned char)(19*i+trial);
    box.s[1]=(uintptr_t)(stack+257+trial);
    box.s[4]=(uintptr_t)(stack+264+(trial==7));
    box.s[16]=2|(trial&1?0xcd5:0x244);
    box.s[17]=0;box.s[18]=0;box.s[19]=0;
    expected=box;memcpy(expected_stack,stack,sizeof(stack));
    if(trial!=7) {
      for(unsigned i=0;i<8;++i)
        expected_stack[257+trial+i]=(unsigned char)(box.s[0]>>(8*i));
      expected.s[20]=box.s[22];expected.s[21]=box.s[23];
      expected.s[52]=UINT64_C(0x140001007);expected.s[53]=4;
    }
    if(generic_machine_region(MACHINE_ARG(box.s))!=(trial==7?2:0)) return 1;
    if(memcmp(&box,&expected,sizeof(box))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
  }
  return 0;
}
)C",
               {}, nullptr, true, true);
  }
}

TEST_F(InterpreterMachineRegionSourceTest, PendingTrapAndReturnRemainDistinct) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "region source execution requires clang";
  // JRCXZ trap; RET; trap: INT3. No flag or input value is fixed for recovery.
  auto Image = loadRegion({0xe3, 1, 0xc3, 0xcc});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 2, ExitKind::Return);
  Options.X64MachineExits.push_back({1, Entry + 3, ExitKind::Trap});
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  ASSERT_EQ(Result.ExternalExits.size(), 2U);
  InterpreterMachineRegionSourceOptions SourceOptions;
  SourceOptions.StopBeforeTraps = true;
  auto Wrapped =
      wrapInterpreterMachineRegionSourceX64(Result, Options, SourceOptions);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  for(unsigned branch=0;branch<2;++branch) {
    uint64_t state[54],expected[54],stack[8],expected_stack[8];
    for(unsigned i=0;i<54;++i) state[i]=UINT64_C(0x8123456789abc000)+i;
    for(unsigned i=0;i<8;++i) stack[i]=UINT64_C(0x1234567800000000)+i;
    state[1]=branch;state[4]=(uintptr_t)&stack[3];state[16]=0x246;
    state[17]=0;state[18]=0;state[19]=0;
    memcpy(expected,state,sizeof(state));
    memcpy(expected_stack,stack,sizeof(stack));
    expected[52]=branch?stack[3]:UINT64_C(0x140001003);
    expected[53]=branch?3:4;
    if(branch) expected[4]+=8;
    if(generic_machine_region(MACHINE_ARG(state))) return 1;
    if(memcmp(state,expected,sizeof(state))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       PendingTrapRetainsDynamicMemoryAlignment) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "region source execution requires clang";
  // MOVAPS XMM0,[RSP]; MOVAPS [RSP+16],XMM1; INT3. The recovery domain
  // proves alignment, but source must retain the original dynamic guards.
  auto Image =
      loadRegion({0x0f, 0x28, 0x04, 0x24, 0x0f, 0x29, 0x4c, 0x24, 0x10, 0xcc});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 9, ExitKind::Trap);
  Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 0};
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  InterpreterMachineRegionSourceOptions SourceOptions;
  SourceOptions.StopBeforeTraps = true;
  auto Wrapped =
      wrapInterpreterMachineRegionSourceX64(Result, Options, SourceOptions);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  unsigned Guards = 0;
  for (const auto &Block : Wrapped->Source.Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 3 &&
          Op.Inputs[0].isConst() &&
          Op.Inputs[0].Offset == uint64_t(Intrinsic::RequireAligned)) {
        EXPECT_FALSE(Op.Inputs[1].isConst());
        EXPECT_EQ(Op.Inputs[2], NdVar::scalar(16, 8));
        ++Guards;
      }
  EXPECT_EQ(Guards, 2U);
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  for(unsigned trial=0;trial<4;++trial) {
    uint64_t state[54],expected[54];
    _Alignas(32) unsigned char stack[128],expected_stack[128];
    for(unsigned i=0;i<54;++i) state[i]=UINT64_C(0x8123456789abc000)+i;
    for(unsigned i=0;i<128;++i) stack[i]=(unsigned char)(i*19+trial);
    unsigned offset=32+trial*16+(trial==3);
    state[4]=(uintptr_t)(stack+offset);state[16]=0x246;
    state[17]=0;state[18]=0;state[19]=0;
    memcpy(expected,state,sizeof(state));
    memcpy(expected_stack,stack,sizeof(stack));
    if(trial!=3) {
      memcpy(expected+20,stack+offset,16);
      memcpy(expected_stack+offset+16,state+22,16);
      expected[52]=UINT64_C(0x140001009);expected[53]=4;
    }
    if(generic_machine_region(MACHINE_ARG(state))!=(trial==3?2:0)) return 1;
    if(memcmp(state,expected,sizeof(state))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest, QueryFailurePrecedesPendingTrap) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "region source execution requires clang";
  auto Image =
      loadRegion({0xb8, 7, 0, 0, 0, 0xb9, 3, 0, 0, 0, 0x0f, 0xa2, 0xcc});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Options = regionOptions(Entry + 12, ExitKind::Trap);
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  InterpreterMachineRegionSourceOptions SourceOptions;
  SourceOptions.StopBeforeTraps = true;
  auto Wrapped =
      wrapInterpreterMachineRegionSourceX64(Result, Options, SourceOptions);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  for(unsigned trial=0;trial<3;++trial) {
    uint64_t state[54],expected[54],stack[8],expected_stack[8];
    uint32_t tape[6]={7,3,0x80000001u,22,33,0xffffffffu},expected_tape[6];
    for(unsigned i=0;i<54;++i) state[i]=UINT64_C(0x8123456789abc000)+i;
    for(unsigned i=0;i<8;++i) stack[i]=UINT64_C(0x1234567800000000)+i;
    state[4]=(uintptr_t)&stack[3];state[16]=0x246;
    state[17]=(uintptr_t)tape;state[18]=trial!=0;state[19]=0;
    if(trial==1) tape[1]=4;
    memcpy(expected,state,sizeof(state));
    memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_tape,tape,sizeof(tape));
    expected[0]=7;expected[1]=3;
    expected[52]=UINT64_C(0x14000100a);expected[53]=0;
    if(trial==2) {
      const unsigned regs[]={0,3,1,2};
      for(unsigned i=0;i<4;++i) expected[regs[i]]=tape[2+i];
      expected[19]=1;expected[52]=UINT64_C(0x14000100c);expected[53]=4;
    }
    if(generic_machine_region(MACHINE_ARG(state))!=(trial==0?3:trial==1?4:0)) return 1;
    if(memcmp(state,expected,sizeof(state))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
    if(memcmp(tape,expected_tape,sizeof(tape))) return 4;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       PendingTrapStillAuthenticatesEvidence) {
  auto Image = loadRegion({0xcc});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Options = regionOptions(Entry, ExitKind::Trap);
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  InterpreterMachineRegionSourceOptions SourceOptions;
  SourceOptions.StopBeforeTraps = true;
  auto Good =
      wrapInterpreterMachineRegionSourceX64(Result, Options, SourceOptions);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  for (unsigned Kind = 0; Kind != 5; ++Kind) {
    SCOPED_TRACE(Kind);
    auto Changed = Result;
    auto Expected = Options;
    if (Kind == 0)
      Changed.ExternalExits.front().Original.NativeBytes[0] = 0x90;
    else if (Kind == 1)
      Changed.Residual.Blocks.back().Ops.back().Opcode = NdOp::NOP;
    else if (Kind == 2)
      Changed.RawEnvironment->IdentityDigest[0] ^= 1;
    else if (Kind == 3)
      Changed.ExternalExits.front().Original.UndefinedEffects.Coverage =
          LowUndefinedCoverage::Missing;
    else
      Expected.X64MachineExits.front().Kind = ExitKind::Return;
    auto Bad =
        wrapInterpreterMachineRegionSourceX64(Changed, Expected, SourceOptions);
    EXPECT_FALSE(bool(Bad));
    if (!Bad)
      EXPECT_FALSE(llvm::toString(Bad.takeError()).empty());
  }
}
