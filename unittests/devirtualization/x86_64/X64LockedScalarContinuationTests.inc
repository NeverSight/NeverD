//===- X64LockedScalarContinuationTests.inc - Native atomic state ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare complete continuation state and memory against native CMPXCHG/ADD.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationSourceTest,
       LockedScalarsRetainNativeFlagsAccumulatorAndUnalignedMemory) {
#if !defined(__x86_64__)
  GTEST_SKIP() << "native locked scalar oracle requires x64";
#else
  std::vector<uint8_t> Bytes;
  std::vector<va_t> Entries;
  std::string Calls, PCs, Lengths;
  for (bool CompareExchange : {true, false})
    for (bool Segmented : {false, true})
      for (unsigned Width : {1u, 2u, 4u, 8u}) {
        const va_t PC = 0x1000 + Bytes.size();
        Entries.push_back(PC);
        if (!Calls.empty()) {
          Calls += ',';
          PCs += ',';
          Lengths += ',';
        }
        Calls += "neverd_step_v3_" + llvm::utohexstr(PC);
        PCs += "0x" + llvm::utohexstr(PC);
        if (Segmented)
          Bytes.push_back(0x65);
        if (Width == 2)
          Bytes.push_back(0x66);
        Bytes.push_back(0xf0);
        if (Width == 8)
          Bytes.push_back(0x48);
        // LOCK CMPXCHG/ADD [RDI], DL/DX/EDX/RDX, optionally through live GS.
        if (CompareExchange)
          Bytes.insert(Bytes.end(),
                       {0x0f, uint8_t(Width == 1 ? 0xb0 : 0xb1), 0x17});
        else
          Bytes.insert(Bytes.end(), {uint8_t(Width == 1 ? 0 : 1), 0x17});
        Lengths += std::to_string(0x1000 + Bytes.size() - PC);
      }
  const auto Harness = "static __typeof__(&neverd_step_v3_1000) steps[]={" +
                       Calls + "};\nstatic const uint64_t pcs[]={" + PCs +
                       "};\nstatic const unsigned lengths[]={" + Lengths +
                       "};\n" + R"C(
static uint64_t original(unsigned width,unsigned compare,void *address,uint64_t accumulator,
                         uint64_t desired,uint64_t *flags) {
  uint64_t f;
  if(!compare) {
    switch(width) {
    case 1:
      __asm__ volatile("leaq -128(%%rsp),%%rsp; lock addb %b2,(%1); "
                       "pushfq; popq %0; leaq 128(%%rsp),%%rsp"
        : "=&r"(f) : "r"(address),"r"(desired) : "cc","memory");
      break;
    case 2:
      __asm__ volatile("leaq -128(%%rsp),%%rsp; lock addw %w2,(%1); "
                       "pushfq; popq %0; leaq 128(%%rsp),%%rsp"
        : "=&r"(f) : "r"(address),"r"(desired) : "cc","memory");
      break;
    case 4:
      __asm__ volatile("leaq -128(%%rsp),%%rsp; lock addl %k2,(%1); "
                       "pushfq; popq %0; leaq 128(%%rsp),%%rsp"
        : "=&r"(f) : "r"(address),"r"(desired) : "cc","memory");
      break;
    default:
      __asm__ volatile("leaq -128(%%rsp),%%rsp; lock addq %2,(%1); "
                       "pushfq; popq %0; leaq 128(%%rsp),%%rsp"
        : "=&r"(f) : "r"(address),"r"(desired) : "cc","memory");
    }
    *flags=f;return accumulator;
  }
  switch(width) {
  case 1:
    __asm__ volatile("leaq -128(%%rsp),%%rsp; lock cmpxchgb %b3,(%2); "
                     "pushfq; popq %1; leaq 128(%%rsp),%%rsp"
      : "+a"(accumulator),"=&r"(f) : "r"(address),"r"(desired) : "cc","memory");
    break;
  case 2:
    __asm__ volatile("leaq -128(%%rsp),%%rsp; lock cmpxchgw %w3,(%2); "
                     "pushfq; popq %1; leaq 128(%%rsp),%%rsp"
      : "+a"(accumulator),"=&r"(f) : "r"(address),"r"(desired) : "cc","memory");
    break;
  case 4:
    __asm__ volatile("leaq -128(%%rsp),%%rsp; lock cmpxchgl %k3,(%2); "
                     "pushfq; popq %1; leaq 128(%%rsp),%%rsp"
      : "+a"(accumulator),"=&r"(f) : "r"(address),"r"(desired) : "cc","memory");
    break;
  default:
    __asm__ volatile("leaq -128(%%rsp),%%rsp; lock cmpxchgq %3,(%2); "
                     "pushfq; popq %1; leaq 128(%%rsp),%%rsp"
      : "+a"(accumulator),"=&r"(f) : "r"(address),"r"(desired) : "cc","memory");
  }
  *flags=f;return accumulator;
}
int main(void) {
  const uint64_t samples[]={0,1,0x7f,0x80,0x7fff,0x8000,0x7fffffff,
                           0x80000000,0x7fffffffffffffffULL,
                           0x8000000000000000ULL,UINT64_MAX};
  const unsigned flag_bits[]={0,2,4,6,7,11};
  for(unsigned n=0;n<16;++n)for(unsigned offset=8;offset<16;++offset)
  for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);++i)
  for(unsigned success=0;success<2;++success)for(unsigned pattern=0;pattern<64;++pattern) {
    _Alignas(64) unsigned char memory[64],native[64];
    unsigned width=1u<<(n%4);
    uint64_t mask=UINT64_MAX>>(64-width*8),old=samples[i]&mask;
    memset(memory,0xa5,sizeof(memory));memcpy(memory+offset,&old,width);
    memcpy(native,memory,sizeof(memory));
    state_v2_t s=seed_v2(pcs[n]);s.core.flags=0x40202;
    for(unsigned b=0;b<6;++b)s.core.flags|=(uint64_t)((pattern>>b)&1)<<flag_bits[b];
    uint64_t comparison=samples[(i+pattern)%11]&mask;
    if(comparison==old)comparison=(old+1)&mask;
    s.core.gpr[0]=(0x12345678abcddcbaULL&~mask)|(success?old:comparison);
    s.core.gpr[2]=n<8?~samples[i]:samples[(i+pattern+success)%11];
    s.core.gpr[7]=n%8<4?(uintptr_t)(memory+offset):offset;
    s.core.gs=n%8<4?0:(uintptr_t)memory;
    state_v2_t expected=s;uint64_t flags;
    expected.core.gpr[0]=original(width,n<8,native+offset,s.core.gpr[0],s.core.gpr[2],&flags);
    expected.core.flags=(s.core.flags&~UINT64_C(0x8d5))|(flags&0x8d5);
    expected.core.pc+=lengths[n];
    CHECK(steps[n](ARG(&s))==0);
    CHECK(!memcmp(&s,&expected,sizeof(s)));
    CHECK(!memcmp(memory,native,sizeof(memory)));
  }
  return 0;
}
)C";
  run(Bytes, Entries, Harness, false, smapOptions(),
      "__atomic_compare_exchange_n");
#endif
}

TEST_F(X64ContinuationSourceTest, LockedScalarsRequireTheOrderedProfile) {
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0xf0, 0x0f, 0xb1, 0x17}, {0xf0, 0x01, 0x17}}) {
    auto Units = buildX64ContinuationSource(image(Bytes), {0x1000}, options());
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    ASSERT_EQ(Units->size(), 1u);
    EXPECT_TRUE(Units->front().RequiresNativeExecution);
  }
}
