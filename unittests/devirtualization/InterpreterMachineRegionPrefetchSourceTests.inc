//===- InterpreterMachineRegionPrefetchSourceTests.inc - Cache hints ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent state oracles and retained source hints for raw regions.

TEST_F(InterpreterMachineRegionSourceTest,
       ReadPrefetchRetainsKindsAndLeavesArchitecturalStateUnchanged) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "prefetch region source execution requires clang";
  std::vector<uint8_t> Code;
  for (unsigned Hint = 0; Hint != 4; ++Hint) {
    Code.insert(Code.end(), {0x0f, 0x18, uint8_t(1 | (Hint << 3))});
    Code.insert(Code.end(), {0x67, 0x0f, 0x18, uint8_t(1 | (Hint << 3))});
  }
  const auto Return = Entry + Code.size();
  Code.push_back(0xc3);
  auto Image = loadRegion(Code);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (bool Kernel : {false, true}) {
    auto Options = regionOptions(Return, ExitKind::Return);
    if (Kernel) {
      Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
      Options.X64FlagsProfile =
          InterpreterMachineStateProfile::KernelX64NoFaultV1;
      Options.X64CR4PGEState = true;
    }
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    auto Wrapped =
        Kernel ? wrapInterpreterMachineRegionChoiceSourceX64V3(Result, Options)
               : wrapInterpreterMachineRegionSourceX64(Result, Options);
    ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
    Wrapped->Source.Function.Name = "generic_machine_region";
    emitAndRun(
        Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
#if KERNEL
  uint64_t choices,choice_count,choice_cursor,fsbase,gsbase,cr4;
#endif
} state_t;
int main(void) {
  const uint64_t addresses[]={0,1,UINT64_MAX,UINT64_C(0x8000000000000000),
                              UINT64_C(0x1234567800000001)};
  for(unsigned trial=0;trial<7;++trial)for(unsigned pattern=0;pattern<16;++pattern) {
    unsigned char memory[256],wanted[256],stack[128],wanted_stack[128];
    struct {uint64_t before[2];state_t s;uint64_t after[2];} box,want;
    memset(&box,0,sizeof(box));
    box.before[0]=0x12345678;box.before[1]=0xabcdef01;
    box.after[0]=0xfedcba98;box.after[1]=0x87654321;
    for(unsigned i=0;i<256;++i)memory[i]=(unsigned char)(i*29+trial);
    for(unsigned i=0;i<128;++i)stack[i]=(unsigned char)(i*23+pattern);
    uint64_t resume=0x12345678;memcpy(stack+40,&resume,8);
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9123456789abcdef)+31*i+trial;
      s->xmm[i][0]=UINT64_C(0xfedcba9876543210)+i;
      s->xmm[i][1]=UINT64_C(0x8123456789abcdef)-i-pattern;
    }
    s->gpr[1]=trial<5?addresses[trial]:(uintptr_t)(trial==5?memory+pattern:stack+40);
    s->gpr[4]=(uintptr_t)(stack+40);
    s->flags=2|((pattern&1)?0x8d5:0)|((uint64_t)(pattern&6)<<8);
    s->pc=0xabcdef;s->kind=0x987654;
#if KERNEL
    s->fsbase=0x11223344;s->gsbase=0x55667788;s->cr4=0x620|((trial&1)<<7);
#endif
    want=box;want.s.gpr[4]+=8;want.s.pc=resume;want.s.kind=3;
    memcpy(wanted,memory,sizeof(memory));memcpy(wanted_stack,stack,sizeof(stack));
    if(generic_machine_region(MACHINE_ARG(s)))return 1;
    if(memcmp(&box,&want,sizeof(box)))return 2;
    if(memcmp(memory,wanted,sizeof(memory)))return 3;
    if(memcmp(stack,wanted_stack,sizeof(stack)))return 4;
  }
  return 0;
}
)C",
        "#define KERNEL " + std::to_string(Kernel) + "\n", nullptr, true, true,
        BinaryFormat::ELF, {}, [](llvm::StringRef Source, bool LLVM) {
          const llvm::StringRef Hints[] = {"nta", "t0", "t1", "t2"};
          const llvm::StringRef Localities[] = {"NTA", "T0", "T1", "T2"};
          for (unsigned I = 0; I != 4; ++I) {
            const auto Needle =
                LLVM ? "__asm__ volatile(\"prefetch" + Hints[I].str()
                     : "_MM_HINT_" + Localities[I].str();
            size_t Pos = 0;
            unsigned Count = 0;
            while ((Pos = Source.find(Needle, Pos)) != llvm::StringRef::npos) {
              ++Count;
              Pos += Needle.size();
            }
            EXPECT_EQ(Count, 2u) << Needle << '\n' << Source.str();
          }
          EXPECT_EQ(Source.find("prefetchw"), llvm::StringRef::npos);
        });
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       ReadPrefetchSourceRequiresAuthenticatedUnmodifiedRegion) {
  auto Image = loadRegion({0x0f, 0x18, 0x01, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Options = regionOptions(Entry + 3, ExitKind::Return);
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    auto Changed = Result;
    if (!Mutation)
      Changed.RawEnvironment.reset();
    else
      for (auto &Block : Changed.Residual.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
              Op.Inputs[0].Offset == uint64_t(Intrinsic::PrefetchNta)) {
            if (Mutation == 1)
              Op.Inputs[0].Offset = uint64_t(Intrinsic::PrefetchW);
            if (Mutation == 2)
              Op.Inputs[1] = NdVar::scalar(0, 8);
            if (Mutation == 3)
              Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
            if (Mutation == 4)
              Op.Output = NdVar::reg(x86reg::RAX, 8);
            if (Mutation == 5)
              Op.Opcode = NdOp::NOP;
          }
    auto Wrapped = wrapInterpreterMachineRegionSourceX64(Changed, Options);
    ASSERT_FALSE(bool(Wrapped)) << Mutation;
    llvm::consumeError(Wrapped.takeError());
  }
  auto Plain = wrapInterpreterMachineStateX64(
      function({{operation(NdOp::INTRINSIC, {},
                           {NdVar::cst(uint64_t(Intrinsic::PrefetchNta), 2),
                            NdVar::reg(x86reg::RCX, 8)})},
                {operation(NdOp::RETURN, {}, {})}}));
  ASSERT_FALSE(bool(Plain));
  llvm::consumeError(Plain.takeError());
  for (const auto &Code :
       std::vector<std::vector<uint8_t>>{{0x65, 0x0f, 0x18, 0x01, 0xc3},
                                         {0x0f, 0x0d, 0x09, 0xc3},
                                         {0x0f, 0xae, 0x39, 0xc3}}) {
    auto Other = loadRegion(Code);
    ASSERT_TRUE(bool(Other)) << llvm::toString(Other.takeError());
    auto OtherOptions =
        regionOptions(Entry + Code.size() - 1, ExitKind::Return);
    OtherOptions.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
    EXPECT_FALSE(
        specializeBinaryInterpreter(*Other, Entry, OtherOptions).complete());
  }
}
