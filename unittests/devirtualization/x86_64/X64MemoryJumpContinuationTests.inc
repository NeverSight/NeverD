//===- X64MemoryJumpContinuationTests.inc - Live indirect jump targets ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by X64ContinuationSourceTests.cpp inside its test namespace.
/// Constant slots, computed addresses and register values retain distinct
/// target semantics without modifying the physical stack or any other state.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationSourceTest, MemoryJumpsReadLiveTargetsExactlyOnce) {
#if !defined(__linux__)
  GTEST_SKIP() << "constant-slot fixture requires Linux mmap";
#else
  std::vector<uint8_t> Bytes(128, 0x90);
  const auto Put = [&](unsigned Offset, std::initializer_list<uint8_t> Code) {
    std::copy(Code.begin(), Code.end(), Bytes.begin() + Offset);
  };
  const auto SlotJump = [&](unsigned Offset, bool Address32) {
    unsigned At = Offset;
    if (Address32)
      Bytes[At++] = 0x67;
    Bytes[At++] = 0xff;
    Bytes[At++] = 0x25;
    const uint32_t Displacement = 0x20000003 - (0x1000 + At + 4);
    for (unsigned I = 0; I < 4; ++I)
      Bytes[At++] = Displacement >> (I * 8);
  };
  SlotJump(0, false);                      // jmp [rip+disp32]
  Put(16, {0xff, 0x20});                   // jmp [rax]
  Put(32, {0xff, 0xe0});                   // jmp rax
  Put(48, {0x64, 0xff, 0x25, 0, 0, 0, 0}); // jmp fs:[rip]
  Put(64, {0x65, 0xff, 0x60, 1});          // jmp gs:[rax+1]
  SlotJump(80, true);                      // jmp [eip+disp32]
  Put(96, {0xe9, 0x10, 0, 0, 0});          // jmp +16
  Put(112, {0x75, 2});                     // jne +2
  const std::vector<va_t> Entries = {0x1000, 0x1010, 0x1020, 0x1030,
                                     0x1040, 0x1050, 0x1060, 0x1070};
  for (bool Controls : {false, true}) {
    std::string Harness = Controls ? "\n#define STATE state_v2_t\n"
                                     "#define CORE(s) ((s).core)\n"
                                     "#define SEED seed_v2\n"
                                   : "\n#define STATE state_t\n"
                                     "#define CORE(s) (s)\n#define SEED seed\n";
    Harness += "static uint64_t (*const steps[])(uint8_t *)={";
    for (va_t Entry : Entries)
      Harness += std::string(Controls ? "neverd_step_v2_" : "neverd_step_") +
                 llvm::utohexstr(Entry) + ",";
    Harness += R"C(};
#include <sys/mman.h>
int main(void) {
  unsigned char *memory=mmap((void *)0x20000000,4096,PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
  CHECK(memory==(void *)0x20000000);
  unsigned char before[4096];
  const uint64_t targets[]={0,1,0x1000,0x20000003,0x123456789abcdef0ULL,UINT64_MAX};
  for(unsigned step=0;step<8;++step)for(unsigned t=0;t<6;++t)
    for(unsigned zero=0;zero<2;++zero) {
      for(unsigned i=0;i<4096;++i)memory[i]=i*19+t;
      memcpy(memory+3,&targets[t],8);memcpy(before,memory,4096);
      STATE s=SEED(0x1000+step*16);
      CORE(s).flags=2|(zero?64:0);
      CORE(s).gpr[0]=step==2?targets[t]:(uintptr_t)(memory+3);
      if(step==3)CORE(s).fs=(uintptr_t)(memory+3)-0x1037;
      if(step==4){CORE(s).gs=(uintptr_t)memory;CORE(s).gpr[0]=2;}
      STATE expected=s;
      CORE(expected).pc=step==6?0x1075:step==7?(zero?0x1072:0x1074):targets[t];
      CHECK(steps[step](ARG(&s))==2);
      CHECK(!memcmp(&s,&expected,sizeof(s)));
      CHECK(!memcmp(memory,before,4096));
    }
  CHECK(!munmap(memory,4096));
  return 0;
}
)C";
    run(Bytes, Entries, Harness, false,
        Controls ? controlsOptions() : options());
  }
#endif
}
