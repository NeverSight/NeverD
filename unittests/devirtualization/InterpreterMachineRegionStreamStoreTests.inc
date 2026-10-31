//===- InterpreterMachineRegionStreamStoreTests.inc - Native stores ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Complete state and memory oracles for retained MOVNTDQ and SFENCE.

TEST_F(InterpreterMachineRegionSourceTest,
       StreamingStoreRetainsNativeWriteFenceAndPhysicalReturn) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "streaming-store source execution requires clang";
  for (unsigned Offset : {0u, 8u}) {
    // MOVNTDQ [RSP+Offset],XMM0; SFENCE; MOV RAX,[RSP+Offset]; RET.
    // Offset zero overwrites the physical return word with the vector low half.
    const std::vector<uint8_t> Code = {
        0x66, 0x0f, 0xe7, 0x44, 0x24, uint8_t(Offset), 0x0f, 0xae,
        0xf8, 0x48, 0x8b, 0x44, 0x24, uint8_t(Offset), 0xc3};
    auto Image = loadRegion(Code);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    for (bool Kernel : {false, true}) {
      auto Options = regionOptions(Entry + 14, ExitKind::Return);
      Options.EntryFrameBounds = SpecializationEntryFrameBounds{0, 24, {}};
      Options.EntryFrameAlignment = InterpreterEntryAlignment{16, Offset};
      Options.ProveStoreFrameSeparation =
          Options.ProveStoreReturnSlotSeparation = true;
      if (Kernel) {
        Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
        Options.X64FlagsProfile =
            InterpreterMachineStateProfile::KernelX64NoFaultV1;
        Options.X64CR4PGEState = true;
      }
      const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      auto Wrapped =
          Kernel
              ? wrapInterpreterMachineRegionChoiceSourceX64V3(Result, Options)
              : wrapInterpreterMachineRegionSourceX64(Result, Options);
      ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
      Wrapped->Source.Function.Name = "generic_machine_region";
      emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {
 uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
#if KERNEL
 uint64_t choices,choice_count,choice_cursor,fsbase,gsbase,cr4;
#endif
} state_t;
int main(void) {
 for(unsigned trial=0;trial<32;++trial) {
  _Alignas(16) unsigned char stack[128],wanted[128];
  struct {uint64_t before[2];state_t s;uint64_t after[2];} box,want;
  memset(&box,0,sizeof(box));
  box.before[0]=0x12345678;box.before[1]=0xabcdef01;
  box.after[0]=0xfedcba98;box.after[1]=0x87654321;
  for(unsigned i=0;i<128;++i)stack[i]=(unsigned char)(i*23+trial);
  const unsigned sp=48-OFFSET;
  uint64_t resume=0x12345678;memcpy(stack+sp,&resume,8);
  state_t *s=&box.s;
  for(unsigned i=0;i<16;++i) {
   s->gpr[i]=UINT64_C(0x9123456789abcdef)+31*i+trial;
   s->xmm[i][0]=UINT64_C(0xfedcba9876543210)+i+trial*13;
   s->xmm[i][1]=UINT64_C(0x8123456789abcdef)-i-trial*29;
  }
  s->gpr[4]=(uintptr_t)(stack+sp);s->flags=2|((trial&1)?0x8d5:0);
  s->pc=0xabcdef;s->kind=0x987654;
#if KERNEL
  s->fsbase=0x11223344;s->gsbase=0x55667788;s->cr4=0x620;
#endif
  want=box;want.s.gpr[4]+=8;
  want.s.pc=OFFSET?resume:s->xmm[0][0];want.s.kind=3;
  want.s.gpr[0]=s->xmm[0][0];
  memcpy(wanted,stack,128);memcpy(wanted+48,s->xmm[0],16);
  if(generic_machine_region(MACHINE_ARG(s)))return 1;
  if(memcmp(&box,&want,sizeof(box)))return 2;
  if(memcmp(stack,wanted,128))return 3;
 }
 return 0;
}
)C",
                 "#define KERNEL " + std::to_string(Kernel) +
                     "\n#define OFFSET " + std::to_string(Offset) + "\n",
                 nullptr, true, true, BinaryFormat::ELF, {},
                 [](llvm::StringRef Source, bool) {
                   EXPECT_NE(Source.find("movntdq"), llvm::StringRef::npos);
                   EXPECT_NE(Source.find("sfence"), llvm::StringRef::npos);
                 });
    }
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       StreamingStoreRequiresAlignmentAndAnUnmodifiedReceipt) {
  for (unsigned Offset : {8u, 16u}) {
    const std::vector<uint8_t> Code = {
        0x66, 0x0f, 0xe7, 0x44, 0x24, uint8_t(Offset), 0xc3};
    auto Image = loadRegion(Code);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Options = regionOptions(Entry + 6, ExitKind::Return);
    Options.EntryFrameBounds = SpecializationEntryFrameBounds{0, 32, {}};
    Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    if (Offset == 16) {
      EXPECT_FALSE(Result.complete());
      continue;
    }
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      auto Changed = Result;
      unsigned MutatedStores = 0;
      if (!Mutation)
        Changed.RawEnvironment.reset();
      else
        for (auto &Block : Changed.Residual.Blocks)
          for (auto &Op : Block.Ops)
            if (x64::streamstore::isOperation(Op)) {
              ++MutatedStores;
              if (Mutation == 1)
                Op.Inputs[2].Size = 8;
              if (Mutation == 2)
                Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
              if (Mutation == 3)
                Op.Opcode = NdOp::STORE;
            }
      ASSERT_EQ(MutatedStores, Mutation ? 1u : 0u);
      auto Wrapped = wrapInterpreterMachineRegionSourceX64(Changed, Options);
      ASSERT_FALSE(bool(Wrapped)) << Mutation;
      llvm::consumeError(Wrapped.takeError());
    }
  }
  auto Plain = wrapInterpreterMachineStateX64(function(
      {{operation(NdOp::INTRINSIC, {},
                  {NdVar::cst(uint64_t(Intrinsic::X86StreamStore128), 2),
                   NdVar::reg(x86reg::RCX, 8),
                   NdVar::reg(x86reg::vectorReg(0), 16)})},
       {operation(NdOp::RETURN, {}, {})}}));
  ASSERT_FALSE(bool(Plain));
  llvm::consumeError(Plain.takeError());
}
