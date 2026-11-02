//===- X64SparseFrameContinuationTests.inc - Conditional slot observers ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by X64ContinuationExportTests.cpp inside its test namespace.
/// Independently authored native callers retain sparse physical arguments.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest,
       SparseArgumentPointerAliasesRetainBranchesAndPartialWrites) {
  // The caller writes only the low halves of arguments five and seven. The
  // callee never names argument six as a parameter, but RDX can observe its
  // physical bytes. A conditional bit changes the result before the callee
  // replaces only the low half of that same slot.
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x38,               // sub rsp,56
      0xc7, 0x44, 0x24, 0x20, 11,  0, 0, 0, // dword arg5 = 11
      0xc7, 0x44, 0x24, 0x30, 7,   0, 0, 0, // dword arg7 = 7
      0xe8, 0x27, 0,    0,    0,            // call 1040
      0x48, 0x83, 0xc4, 0x38, 0xc3};        // add rsp,56; ret
  Code.resize(0x40, 0x90);
  Code.insert(Code.end(), {0x8b, 0x44, 0x24, 0x38,             // eax = arg7
                           0x83, 0xf9, 7,    0x75, 0x15,       // ecx == 7?
                           0x41, 0x83, 0xf9, 1,    0x75, 0x0f, // r9d == 1?
                           0x48, 0x85, 0xd2, 0x74, 0x0a,       // rdx != 0?
                           0xf6, 0x02, 1,    0x74, 5,          // *rdx & 1?
                           0x0d, 0,    0,    2,    0,          // eax |= 0x20000
                           0x41, 0x89, 0,                      // [r8] = eax
                           0xc7, 0x44, 0x24, 0x30, 0x0d, 0xf0,
                           0xad, 0x0b, 0xc3}); // low arg6 = 0x0badf00d; ret
  std::string Native = "static const unsigned char original_bytes[]={";
  for (uint8_t Byte : Code)
    Native += std::to_string(Byte) + ",";
  Native += R"C(};
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
static uint64_t run_original(void *code, uint64_t *sp, uint64_t tag,
                             void *pointer, uint32_t *output, uint64_t mode,
                             uint64_t *flags) {
  register uint64_t out __asm__("r8")=(uintptr_t)output;
  register uint64_t condition __asm__("r9")=mode;
  uint64_t result,after_flags;
  // This owned fixture does not write R11. The physical test stack, pointer
  // and output addresses are identical in the original and source executions.
  __asm__ volatile("pushq $0x246; popfq\n\t"
                   "mov %%rsp,%%r11; mov %[sp],%%rsp\n\t"
                   "call *%[code]; mov %%r11,%%rsp\n\t"
                   "pushfq; pop %[after_flags]"
                   : "=&a"(result), [after_flags]"=&r"(after_flags)
                   : [code]"r"(code), [sp]"r"(sp), "c"(tag), "d"(pointer),
                     "r"(out), "r"(condition)
                   : "r11", "memory", "cc");
  *flags=after_flags;return result;
}
#endif
)C";
  load(Code);
  for (bool LLVM : {false, true})
    for (bool Direct : {false, true}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(Direct);
      auto [Source, Manifest] = source(
          request(
              std::string(
                  R"("entries":[4096],"callee_depth":1,"follow_internal_calls":true,"dispatch":)") +
              (Direct ? "\"direct\"" : "\"switch\"")),
          LLVM);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed));
      EXPECT_EQ(Parsed->getAsObject()->getInteger("total"), 19);
      EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
      execute(Source, std::string(Seed) + Native + R"C(
int main(void) {
#if defined(__linux__) && defined(__x86_64__)
  void *code=mmap(0,sizeof(original_bytes),PROT_READ|PROT_WRITE,
                  MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(code!=MAP_FAILED);memcpy(code,original_bytes,sizeof(original_bytes));
  CHECK(!mprotect(code,sizeof(original_bytes),PROT_READ|PROT_EXEC));
#endif
  unsigned paths[5]={0};
  for(unsigned i=0;i<96;++i) {
    uint64_t stack[64],before[64],want[64];
    for(unsigned j=0;j<64;++j)stack[j]=0x1234567800000000ULL+j*71+i;
    stack[38]=0xdadacafe00000000ULL+i/12;stack[40]=0x7777;
    memcpy(before,stack,sizeof(stack));memcpy(want,stack,sizeof(stack));
    uint32_t object=i/12,output=0xfeedface;
    void *pointer=(i/4)%3==0?0:(i/4)%3==1?(void *)&object:(void *)&stack[38];
    uint64_t tag=7+(i&1),mode=1+((i>>1)&1);
    unsigned path=(i&1)?0:(i&2)?1:!pointer?2:!(object&1)?3:4;
    ++paths[path];uint64_t result=7|(path==4?0x20000:0);
    want[32]=0x1019;
    want[37]=(want[37]&0xffffffff00000000ULL)|11;
    want[38]=(want[38]&0xffffffff00000000ULL)|0x0badf00d;
    want[39]=(want[39]&0xffffffff00000000ULL)|7;
    neverd_x64_state_v6 s=seed();s.GPR[4]=(uintptr_t)&stack[40];
    s.GPR[1]=tag;s.GPR[2]=(uintptr_t)pointer;s.GPR[8]=(uintptr_t)&output;s.GPR[9]=mode;
    neverd_x64_state_v6 saved=s;
    // Exhaust every prefix, including the internal call, without changing
    // physical storage or assuming the slot was initialized by this caller.
    uint64_t status=9,total=0;
    while(status==9) {
      uint64_t budget=1;status=neverd_x64_continue_v6(&s,&budget);
      CHECK(!budget && ++total<=19);
    }
    CHECK(status==3 && s.PC==0x7777 && s.GPR[0]==result && output==result);
    CHECK(s.GPR[4]==(uintptr_t)&stack[41] && !memcmp(stack,want,sizeof(stack)));
    for(unsigned j=0;j<16;++j)if(j!=0&&j!=4)CHECK(s.GPR[j]==saved.GPR[j]);
    CHECK(!memcmp(s.ZMM,saved.ZMM,sizeof(s.ZMM)));
#if defined(__linux__) && defined(__x86_64__)
    memcpy(stack,before,sizeof(stack));output=0xfeedface;
    uint64_t flags;
    CHECK(run_original(code,&stack[41],tag,pointer,&output,mode,&flags)==result);
    CHECK(output==result && flags==s.RFlags && object==i/12);
    CHECK(stack[32]==(uintptr_t)code+0x19);
    // Only the two native return PCs differ because the owned code is relocated.
    stack[32]=want[32];stack[40]=want[40];
    CHECK(!memcmp(stack,want,sizeof(stack)));
#endif
  }
  for(unsigned i=0;i<5;++i)CHECK(paths[i]);
#if defined(__linux__) && defined(__x86_64__)
  CHECK(!munmap(code,sizeof(original_bytes)));
#endif
  return 0;
}
)C");
    }
}
