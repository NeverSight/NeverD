//===- InterpreterMachineRegionStringSourceTests.inc - Runtime direction --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent full-state oracles for bounded repeated transfers.

TEST_F(InterpreterMachineRegionSourceTest,
       RepeatedTransfersRetainRuntimeDirectionAndOverlap) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "repeated-transfer source execution requires clang";
  for (unsigned Kind = 0; Kind != 8; ++Kind) {
    const bool Fill = Kind >= 4;
    const unsigned Width = 1u << (Kind % 4);
    for (bool Kernel : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Kind << ':' << Kernel);
      std::vector<uint8_t> Code{0xb9, 3, 0, 0, 0}; // MOV ECX,3
      if (Width == 2)
        Code.push_back(0x66);
      Code.push_back(0xf3);
      if (Width == 8)
        Code.push_back(0x48);
      Code.push_back(Fill ? (Width == 1 ? 0xaa : 0xab)
                          : (Width == 1 ? 0xa4 : 0xa5));
      const va_t Return = Entry + Code.size();
      Code.push_back(0xc3);
      auto Image = loadRegion(Code);
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      auto Options = regionOptions(Return, ExitKind::Return);
      if (Kernel) {
        Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
        Options.X64FlagsProfile =
            InterpreterMachineStateProfile::KernelX64NoFaultV1;
        Options.X64CR4PGEState = true;
      }
      const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
      if (Kernel && (Fill || Width != 8)) {
        // The fresh-choice encoding audit currently owns canonical REP MOVSQ.
        // Runtime direction does not authorize additional native encodings.
        EXPECT_FALSE(Result.complete());
        EXPECT_NE(Result.Diagnostic.find("complete undefined effects"),
                  std::string::npos);
        continue;
      }
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
typedef struct {uint64_t before[2];state_t s;uint64_t after[2];} box_t;
int main(void) {
  const int gaps[]={-32,-8,-1,0,1,8,32};
  for(unsigned backward=0;backward<2;++backward)
  for(unsigned g=0;g<7;++g)for(unsigned trial=0;trial<4;++trial) {
    unsigned char memory[256],wanted[256],stack[128],wanted_stack[128];
    box_t box,want;memset(&box,0,sizeof(box));
    box.before[0]=0x12345678;box.before[1]=0xabcdef01;
    box.after[0]=0xfedcba98;box.after[1]=0x87654321;
    for(unsigned i=0;i<256;++i)memory[i]=(unsigned char)(i*29+i/7+trial);
    for(unsigned i=0;i<128;++i)stack[i]=(unsigned char)(i*23+trial);
    uint64_t resume=0x12345678;memcpy(stack+64,&resume,8);
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9123456789abcdef)+31*i+trial;
      s->xmm[i][0]=UINT64_C(0xfedcba9876543210)+i;
      s->xmm[i][1]=UINT64_C(0x8123456789abcdef)-i-trial;
    }
    int source=backward?160:64,destination=source+gaps[g];
    s->gpr[4]=(uintptr_t)(stack+64);
    s->gpr[6]=(uintptr_t)(memory+source);
    s->gpr[7]=(uintptr_t)(memory+destination);
    s->flags=2|((trial&1)?0x8d5:0x200)|((uint64_t)backward<<10);
    s->pc=0xabcdef;s->kind=0x987654;
#if KERNEL
    s->fsbase=0x11223344;s->gsbase=0x55667788;s->cr4=0x620|(trial<<7);
#endif
    memcpy(wanted,memory,sizeof(memory));memcpy(wanted_stack,stack,sizeof(stack));
    want=box;
    for(unsigned i=0;i<3;++i) {
      int delta=(backward?-1:1)*(int)(i*WIDTH);unsigned char element[8];
      for(unsigned b=0;b<WIDTH;++b)
        element[b]=FILL?(unsigned char)(s->gpr[0]>>(8*b)):wanted[source+delta+(int)b];
      for(unsigned b=0;b<WIDTH;++b)wanted[destination+delta+(int)b]=element[b];
    }
    const uint64_t delta=backward?UINT64_C(0)-3*WIDTH:3*WIDTH;
    if(!FILL)want.s.gpr[6]+=delta;
    want.s.gpr[7]+=delta;want.s.gpr[1]=0;want.s.gpr[4]+=8;
    want.s.pc=resume;want.s.kind=3;
    if(generic_machine_region(MACHINE_ARG(s)))return 1;
    if(memcmp(&box,&want,sizeof(box)))return 2;
    if(memcmp(memory,wanted,sizeof(memory)))return 3;
    if(memcmp(stack,wanted_stack,sizeof(stack)))return 4;
  }
  return 0;
}
)C",
                 "#define KERNEL " + std::to_string(Kernel) +
                     "\n#define FILL " + std::to_string(Fill) +
                     "\n#define WIDTH " + std::to_string(Width) + "\n",
                 nullptr, true, true);
    }
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       RuntimeDirectionDoesNotPermitUnboundedRepeatedCount) {
  auto Image = loadRegion({0xf3, 0x48, 0xa5, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Result = specializeBinaryInterpreter(
      *Image, Entry, regionOptions(Entry + 3, ExitKind::Return));
  EXPECT_FALSE(Result.complete());
  EXPECT_NE(Result.Diagnostic.find("proved count"), std::string::npos);
}
