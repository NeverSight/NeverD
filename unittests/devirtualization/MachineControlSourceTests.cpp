//===- MachineControlSourceTests.cpp - Native transfer source execution
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace {
using namespace neverd;

std::string machineControlFile(const fs::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

va_t machineControlSymbol(const BinaryImage &Image, llvm::StringRef Name) {
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.Name == Name)
      return Symbol.Addr;
  return InvalidVA;
}

class MachineControlSourceTest : public NeverDLiftTest {
protected:
  static fs::path fixture(const char *Name) {
    return fs::path(TEST_SOURCE_DIR) / "fixtures" / Name;
  }

  RunResult buildMachine(const fs::path &Output) {
    return exec(NEVERD_TEST_CLANG,
                {"-target", "x86_64-linux-gnu", "-fuse-ld=lld", "-nostdlib",
                 "-static", "-Wl,-e,generic_machine_indirect_call",
                 fixture("generic_machine_control.S").string(),
                 fixture("generic_machine_control_observer.S").string(), "-o",
                 Output.string()});
  }

  RunResult recover(const fs::path &Binary, llvm::StringRef Name, bool LLVM,
                    const fs::path &Source, const fs::path &Report) {
    std::vector<std::string> Args{"decompile",
                                  Binary.string(),
                                  "--func",
                                  Name.str(),
                                  "--devirtualize",
                                  "--vm-machine-state",
                                  "--recovery-report=" + Report.string(),
                                  "-o",
                                  Source.string()};
    if (LLVM)
      Args.push_back("--llvm");
    return exec(ndBin(), Args);
  }
};

struct MachineControlCase {
  const char *Name;
  unsigned Kind;
  bool LLVM;
};

class MachineControlRoundTripTest
    : public MachineControlSourceTest,
      public ::testing::WithParamInterface<MachineControlCase> {};

TEST_P(MachineControlRoundTripTest,
       FiniteTransfersPreserveRegistersFlagsAndExactStackWrites) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public machine control source checks require clang";
  const auto &[Name, Kind, LLVM] = GetParam();
  const auto Binary = tmpFile("machine-control.elf");
  const auto Built = buildMachine(Binary);
  ASSERT_TRUE(Built.ok()) << Built.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Source = tmpFile("recovered.c");
  const auto Report = tmpFile("recovery.json");
  const auto Recovered = recover(Binary, Name, LLVM, Source, Report);
  ASSERT_TRUE(Recovered.ok()) << Recovered.err << machineControlFile(Report);
  auto JSON = llvm::json::parse(machineControlFile(Report));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto *Object = JSON->getAsObject();
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(Object->getBoolean("complete"), true);
  EXPECT_EQ(Object->getBoolean("controlComplete"), true);
  EXPECT_EQ(Object->getBoolean("discoverControlState"), true);
  for (const char *Key : {"controlRegisters", "controlFrameSlots"}) {
    const auto *Fields = Object->getArray(Key);
    ASSERT_NE(Fields, nullptr);
    EXPECT_TRUE(Fields->empty());
  }

  // Expected code addresses must follow the loaded image rather than the
  // linker's current layout. They are data observed by the guest program.
  std::string Addresses;
  for (const char *Symbol :
       {"generic_machine_call_zero", "generic_machine_call_one",
        "generic_machine_call_fallthrough", "generic_machine_return_zero",
        "generic_machine_return_one", "generic_machine_observer_exit"}) {
    const va_t Address = machineControlSymbol(*Image, Symbol);
    ASSERT_NE(Address, InvalidVA) << Symbol;
    Addresses += "#define MACHINE_ADDRESS_" + std::string(Symbol) +
                 " UINT64_C(" + std::to_string(Address) + ")\n";
  }
  const auto Harness = tmpFile("reference.c");
  std::ofstream(Harness) << machineControlFile(Source)
                         << "\n#define GENERIC_MACHINE_SOURCE\n"
                         << "#define GENERIC_MACHINE_FUNCTION " << Name << "\n"
                         << "#define GENERIC_MACHINE_KIND " << Kind << "\n"
                         << Addresses
                         << "#define MACHINE_ADDRESS(s) MACHINE_ADDRESS_##s\n"
                         << machineControlFile(
                                fixture("generic_machine_control_reference.c"));
  std::ofstream(tmpFile("immintrin.h")).close();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Program = tmpFile("recovered-control");
    const auto Compiled =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Optimization, "-fno-inline", "-Werror=return-type",
              "-Werror=implicit-function-declaration", "-fsanitize=undefined",
              "-fsanitize-trap=undefined", "-I", tmp().string(),
              Harness.string(), "-o", Program.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err << machineControlFile(Source);
    const auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << " exit=" << Ran.exitCode << '\n'
                          << machineControlFile(Source);
  }
}

INSTANTIATE_TEST_SUITE_P(
    PublicMachineTransfers, MachineControlRoundTripTest,
    ::testing::Values(
        MachineControlCase{"generic_machine_indirect_call", 0, false},
        MachineControlCase{"generic_machine_indirect_call", 0, true},
        MachineControlCase{"generic_machine_return_thread", 1, false},
        MachineControlCase{"generic_machine_return_thread", 1, true}),
    [](const ::testing::TestParamInfo<MachineControlCase> &Info) {
      return std::string(Info.param.Name) +
             (Info.param.LLVM ? "_LLVMC" : "_HighC");
    });

TEST_F(MachineControlSourceTest, NativeTransfersMatchFullIndependentState) {
#if !defined(__x86_64__) || !defined(__linux__)
  GTEST_SKIP() << "native fixture execution requires an x64 Linux host";
#endif
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "native public machine control checks require clang";
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Program = tmpFile("native-control");
    const auto Compiled = exec(
        NEVERD_TEST_CLANG,
        {"-target", "x86_64-linux-gnu", "-fuse-ld=lld", "-std=c11", "-no-pie",
         Optimization, fixture("generic_machine_control.S").string(),
         fixture("generic_machine_control_observer.S").string(),
         fixture("generic_machine_control_reference.c").string(), "-o",
         Program.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err;
    const auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << " exit=" << Ran.exitCode;
  }
}

TEST_F(MachineControlSourceTest, UnprovedAndMemoryFormTransfersNeverPublish) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public machine control refusal checks require clang";
  const auto Binary = tmpFile("machine-control.elf");
  const auto Built = buildMachine(Binary);
  ASSERT_TRUE(Built.ok()) << Built.err;
  for (const char *Name :
       {"generic_machine_unknown_call", "generic_machine_nonexec_call",
        "generic_machine_missing_call", "generic_machine_nonexec_return",
        "generic_machine_memory_call", "generic_machine_stack_memory_call",
        "generic_machine_unknown_return"})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      const std::string Stem = std::string(Name) + std::to_string(LLVM);
      const auto Source = tmpFile(Stem + ".c");
      const auto Report = tmpFile(Stem + ".json");
      const auto Recovered = recover(Binary, Name, LLVM, Source, Report);
      EXPECT_FALSE(Recovered.ok());
      EXPECT_FALSE(fs::exists(Source));
      auto JSON = llvm::json::parse(machineControlFile(Report));
      ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
      const auto *Object = JSON->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), false);
      EXPECT_EQ(Object->getBoolean("controlComplete"), false);
      const auto Status = Object->getString("status");
      EXPECT_TRUE(Status == "unsupported" || Status == "unresolved-control")
          << machineControlFile(Report);
    }
}

} // namespace
