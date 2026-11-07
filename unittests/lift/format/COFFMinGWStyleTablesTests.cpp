//===- COFFMinGWStyleTablesTests.cpp - Linked i386 table ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Loads and lifts fixed CALL/JMP table encodings through the full pipeline.
//===----------------------------------------------------------------------===//

#include "../../TestProcess.h"
#include "gtest/gtest.h"

#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>

#ifndef NEVERD_MINGW_STYLE_TABLES_FIXTURE
#define NEVERD_MINGW_STYLE_TABLES_FIXTURE ""
#endif

#ifndef NEVERD_MINGW_STYLE_TABLES_SOURCE
#define NEVERD_MINGW_STYLE_TABLES_SOURCE ""
#endif

#ifndef NEVERD_TEST_CLANG
#define NEVERD_TEST_CLANG ""
#endif

using namespace neverd;

TEST(COFFMinGWStyleTables, PreservesCallEntriesAndSwitchCasesThroughLLVM) {
  if (NEVERD_MINGW_STYLE_TABLES_FIXTURE[0] == '\0')
    GTEST_SKIP() << "cross-target Clang or lld-link unavailable";

  auto Image = loadBinary(NEVERD_MINGW_STYLE_TABLES_FIXTURE);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = Image->Entry;
  const std::set<va_t> CallSlots{Entry + 0x98, Entry + 0x9c};
  EXPECT_EQ(Image->CallDispatchRelocSlots, CallSlots);
  for (va_t Slot : CallSlots) {
    ASSERT_TRUE(Image->hasExecutableCodeOwnerAt(Slot));
    ASSERT_TRUE(Image->CodePtrRelocSlots.count(Slot));
  }
  EXPECT_EQ(readLE<uint32_t>(Image->readVA(Entry + 0x98, 4)), Entry + 0x80);
  EXPECT_EQ(readLE<uint32_t>(Image->readVA(Entry + 0x9c, 4)), Entry + 0x90);
  ASSERT_EQ(Image->JumpDispatchTableSources.size(), 4u);
  for (const auto &[Slot, Readers] : Image->JumpDispatchTableSources) {
    EXPECT_FALSE(Image->hasExecutableCodeOwnerAt(Slot));
    EXPECT_TRUE(Image->CodePtrRelocSlots.count(Slot));
    EXPECT_EQ(Readers, (std::set<va_t>{Entry + 0x47}));
  }

  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.LiftMode = true;
  Options.EmitDumpOutput = false;
  auto Result = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
  EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
  std::set<va_t> Functions;
  for (const auto &Func : Result.LowFuncs)
    Functions.insert(Func.Entry);
  // main, dispatch, classify and the two callbacks. The four prologue-shaped
  // switch cases must remain blocks of classify, never independent functions.
  EXPECT_EQ(Functions, (std::set<va_t>{Entry, Entry + 0x20, Entry + 0x40,
                                       Entry + 0x80, Entry + 0x90}));
  EXPECT_EQ(Result.MedFuncs.size(), 5u);
  size_t Definitions = 0;
  for (const auto &Func : *Result.LlvmModule)
    Definitions += !Func.isDeclaration();
  EXPECT_EQ(Definitions, 5u);
}

TEST(COFFMinGWStyleTables, NativeI386ResultsMatchOriginalInstructions) {
#if !defined(__linux__) || (!defined(__i386__) && !defined(__x86_64__))
  GTEST_SKIP() << "native i386 execution requires a Linux x86 host";
#else
  if (NEVERD_MINGW_STYLE_TABLES_FIXTURE[0] == '\0' ||
      NEVERD_TEST_CLANG[0] == '\0')
    GTEST_SKIP() << "cross-target Clang or lld-link unavailable";
  auto Image = loadBinary(NEVERD_MINGW_STYLE_TABLES_FIXTURE);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.LiftMode = true;
  Options.EmitDumpOutput = false;
  auto Result = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));

  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-coff-tables", Directory));
  const std::filesystem::path Root(Directory.c_str());
  const auto Read = [](const std::filesystem::path &Path) {
    std::ifstream File(Path);
    return std::string(std::istreambuf_iterator<char>(File), {});
  };
  const auto Run = [&](std::string Command) {
    Command += test::redirectOutput((Root / "stdout").string(),
                                    (Root / "stderr").string());
    return test::systemExitCode(test::runShellCommand(Command));
  };
  const auto Quote = [](const std::filesystem::path &Path) {
    return test::shellQuote(Path.string());
  };
  const std::filesystem::path Source = NEVERD_MINGW_STYLE_TABLES_SOURCE;
  std::string Assembly = Read(Source);
  const std::string COFFSection = ".section .rdata,\"dr\"";
  const size_t Section = Assembly.find(COFFSection);
  ASSERT_NE(Section, std::string::npos);
  // Only the container's section directive changes. Both native oracles use
  // the same independently authored instructions and pointer table entries.
  Assembly.replace(Section, COFFSection.size(), ".section .rodata,\"a\"");
  std::ofstream(Root / "original.s") << Assembly;
  const auto Harness =
      Source.parent_path() / "test_coff_indexed_tables_native.c";
  std::error_code Error;
  llvm::raw_fd_ostream IR((Root / "lifted.ll").string(), Error);
  ASSERT_FALSE(Error);
  Result.LlvmModule->print(IR, nullptr);
  IR.close();
  const std::string Compiler =
      test::shellQuote(NEVERD_TEST_CLANG) +
      " --target=i386-linux-gnu -m32 -fno-pie -fno-stack-protector"
      " -nostdlib -static -Wno-override-module ";
  ASSERT_EQ(Run(Compiler + " -O2 -DORIGINAL " + Quote(Root / "original.s") +
                " " + Quote(Harness) + " -o " + Quote(Root / "original")),
            0)
      << Read(Root / "stderr");
  const int OriginalStatus = Run(Quote(Root / "original"));
  if (OriginalStatus == 126 &&
      Read(Root / "stderr").find("Exec format error") != std::string::npos)
    GTEST_SKIP() << "host kernel cannot execute i386 ELF; artifacts: " << Root;
  ASSERT_EQ(OriginalStatus, 0) << Read(Root / "stderr");
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    ASSERT_EQ(Run(Compiler + Optimization + " " + Quote(Root / "lifted.ll") +
                  " " + Quote(Harness) + " -o " + Quote(Root / "lifted")),
              0)
        << Read(Root / "stderr");
    EXPECT_EQ(Run(Quote(Root / "lifted")), 0) << Read(Root / "stderr");
  }
  if (!::testing::Test::HasFailure())
    std::filesystem::remove_all(Root);
#endif
}
