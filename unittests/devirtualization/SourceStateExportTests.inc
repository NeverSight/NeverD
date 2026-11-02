//===- SourceStateExportTests.inc - Public machine-source layouts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute unchanged CLI output with independent query and vector expectations.
//===----------------------------------------------------------------------===//

TEST_F(DevirtualizationSourceTest, CLIExportsOrderedQueryAndXMMState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "source state execution requires clang";
  const auto Assembly = tmpFile("source-state.S");
  const auto Binary = tmpFile("source-state.elf");
  std::ofstream(Assembly) << R"(
.text
.globl source_state_query
.type source_state_query,@function
source_state_query:
  movl $7,%eax
  movl $3,%ecx
  stc
  cpuid
  ret
.size source_state_query,.-source_state_query
.globl source_state_vector
.type source_state_vector,@function
source_state_vector:
  movdqu %xmm2,%xmm0
  paddb %xmm1,%xmm0
  movl $7,%eax
  movl $3,%ecx
  stc
  cpuid
  ret
.size source_state_vector,.-source_state_vector
)";
  const auto Built = exec(NEVERD_TEST_CLANG,
                          {"-target", "x86_64-linux-gnu", "-fuse-ld=lld",
                           "-nostdlib", "-static", "-Wl,-e,source_state_query",
                           Assembly.string(), "-o", Binary.string()});
  ASSERT_TRUE(Built.ok()) << Built.err;
  for (bool Vector : {false, true})
    for (bool LLVM : {false, true}) {
      const std::string Name =
          Vector ? "source_state_vector" : "source_state_query";
      const auto Source = tmpFile(Name + (LLVM ? "-llvm.c" : "-high.c"));
      const auto Report = tmpFile("source-state.json");
      std::vector<std::string> Args{"decompile",
                                    Binary.string(),
                                    "--func",
                                    Name,
                                    "--devirtualize",
                                    "--vm-machine-state",
                                    Vector ? "--vm-source-abi=xmm"
                                           : "--vm-source-abi=queries",
                                    "--recovery-report=" + Report.string(),
                                    "-o",
                                    Source.string()};
      if (LLVM)
        Args.push_back("--llvm");
      const auto Recovered = exec(ndBin(), Args);
      ASSERT_TRUE(Recovered.ok()) << Recovered.err;
      auto Parsed = llvm::json::parse(readSource(Report));
      ASSERT_TRUE(bool(Parsed));
      EXPECT_EQ(Parsed->getAsObject()->getBoolean("complete"), true);
      EXPECT_EQ(Parsed->getAsObject()->getInteger("sourceStateBytes"),
                Vector ? 416 : 160);
      const auto Harness = tmpFile(Name + "-run.c");
      std::ofstream OS(Harness);
      OS << "#include <stdint.h>\n#include <string.h>\n"
         << readSource(Source) << "\n#define RECOVERED " << Name
         << "\n#define VECTOR_STATE " << Vector << '\n'
         << R"C(
int main(void) {
  for (unsigned mode=0; mode<4; ++mode) {
    uint64_t state[52], expected[52];
    uint32_t tape[6]={7,3,0xffffffffu,0x80000000u,0x87654321u,0x12345678u};
    for (unsigned i=0;i<52;++i) state[i]=0xabcdef1234560000ull+i*0x01010101;
    state[16]=0x246; state[17]=(uintptr_t)tape; state[18]=1; state[19]=0;
    if (mode==1) { state[17]=0; state[18]=0; }
    if (mode==2) tape[0]=8;
    if (mode==3) tape[1]=4;
    memcpy(expected,state,sizeof state);
    expected[0]=7; expected[1]=3; expected[16]=0x247;
    if (VECTOR_STATE) {
      uint8_t *out=(uint8_t *)(expected+20);
      const uint8_t *left=(const uint8_t *)(state+24), *right=(const uint8_t *)(state+22);
      for (unsigned i=0;i<16;++i) out[i]=(uint8_t)(left[i]+right[i]);
    }
    if (!mode) {
      expected[0]=tape[2]; expected[3]=tape[3];
      expected[1]=tape[4]; expected[2]=tape[5]; expected[19]=1;
    }
    if (RECOVERED((void *)state)!=(mode==0?0u:mode==1?3u:4u)) return 1;
    if (memcmp(state,expected,sizeof state)) return 2;
  }
  return 0;
}
)C";
      OS.close();
      for (const char *Opt : {"-O0", "-O2"}) {
        const auto Exe = tmpFile(std::string("source-state-run") +
                                 neverd::test::executableSuffix());
        const auto Compiled =
            exec(NEVERD_TEST_CLANG,
                 {"-std=gnu11", Opt, "-fsanitize=address,undefined",
                  "-fno-sanitize-recover=all", Harness.string(), "-o",
                  Exe.string()});
        ASSERT_TRUE(Compiled.ok()) << Compiled.err;
        const auto Ran = exec(Exe.string(), {});
        EXPECT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode;
      }
    }

  const auto Output = tmpFile("reject-state.c");
  for (std::vector<std::string> Extra :
       {std::vector<std::string>{"--vm-source-abi=queries"},
        {"--devirtualize", "--vm-source-abi=queries"},
        {"--devirtualize", "--vm-machine-state", "--vm-source-abi=unknown"},
        {"--devirtualize", "--vm-machine-state", "--vm-source-abi="},
        {"--devirtualize", "--vm-machine-state", "--vm-source-abi=scalar"}}) {
    std::ofstream(Output) << "retained output";
    std::vector<std::string> Args{"decompile", Binary.string(),
                                  "--func",    "source_state_query",
                                  "-o",        Output.string()};
    Args.insert(Args.end(), Extra.begin(), Extra.end());
    EXPECT_FALSE(exec(ndBin(), Args).ok());
    EXPECT_EQ(readSource(Output), "retained output");
  }
}
