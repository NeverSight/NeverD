//===- X64AlignmentContinuationTests.inc - Refuse before aligned access ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by X64ContinuationExportTests.cpp inside its test namespace.
/// Invalid linear alignment leaves the instruction, state and budget untouched.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest,
       AlignedMemoryRefusesBeforeAccessAndPreservesTheWholeState) {
  const std::vector<std::vector<uint8_t>> Encodings = {
      {0x0f, 0x28, 0x00},              // movaps xmm0,[rax]
      {0x0f, 0x29, 0x00},              // movaps [rax],xmm0
      {0x66, 0x0f, 0x6f, 0x00},        // movdqa xmm0,[rax]
      {0x66, 0x0f, 0x7f, 0x00},        // movdqa [rax],xmm0
      {0x64, 0x0f, 0x28, 0x00},        // movaps xmm0,fs:[rax]
      {0x65, 0x67, 0x0f, 0x29, 0x00}}; // movaps gs:[eax],xmm0
  for (unsigned Encoding = 0; Encoding < Encodings.size(); ++Encoding) {
    load(Encodings[Encoding]);
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Encoding);
      SCOPED_TRACE(LLVM);
      auto [Source, Manifest] =
          source(request(R"("instructions":[4096],"dispatch":"direct")"), LLVM);
      execute(Source, "#define ENCODING " + std::to_string(Encoding) +
                          "\n#define INSTRUCTION_BYTES " +
                          std::to_string(Encodings[Encoding].size()) + "\n" +
                          Seed + R"C(
int main(void) {
  unsigned char memory[64] __attribute__((aligned(16))),before[64],want[64];
  for(unsigned low=0;low<16;++low) {
    for(unsigned i=0;i<sizeof(memory);++i)memory[i]=i*13+low;
    memcpy(before,memory,sizeof(memory));memcpy(want,memory,sizeof(memory));
    neverd_x64_state_v6 s=seed();
    s.GPR[0]=(uintptr_t)(memory+16+low);
    if(ENCODING==4) {s.FSBase=(uintptr_t)(memory+16)-7;s.GPR[0]=7+low;}
    if(ENCODING==5) {s.GSBase=(uintptr_t)(memory+16)-7;s.GPR[0]=0x1234567800000007ULL+low;}
    neverd_x64_state_v6 expected=s;uint64_t budget=1;
    if(low) {
      CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==1);
      CHECK(!memcmp(&s,&expected,sizeof(s)) && !memcmp(memory,before,sizeof(memory)));
    } else {
      if(ENCODING&1)memcpy(want+16,s.ZMM[0],16);
      else memcpy(expected.ZMM[0],memory+16,16);
      expected.PC+=INSTRUCTION_BYTES;
      CHECK(neverd_x64_continue_v6(&s,&budget)==9 && !budget);
      CHECK(!memcmp(&s,&expected,sizeof(s)) && !memcmp(memory,want,sizeof(memory)));
    }
  }
  // Address 1 is inaccessible: the alignment predicate must run before the
  // guest memory access, including after the live segment base is applied.
  neverd_x64_state_v6 s=seed();s.GPR[0]=1;
  if(ENCODING==4){s.FSBase=2;s.GPR[0]=UINT64_MAX;}
  if(ENCODING==5){s.GSBase=UINT64_MAX-UINT32_MAX+2;s.GPR[0]=0x12345678ffffffffULL;}
  neverd_x64_state_v6 expected=s;uint64_t budget=3;
  CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==3);
  CHECK(!memcmp(&s,&expected,sizeof(s)));
  return 0;
}
)C");
    }
  }
}
