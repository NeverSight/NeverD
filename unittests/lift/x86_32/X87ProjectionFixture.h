//===- X87ProjectionFixture.h - Typed x87 native evidence ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain optional per-variant typed x87 artifacts using the native regression
/// recorder's existing destination, without changing normal fixture cleanup.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_X86_32_X87PROJECTIONFIXTURE_H
#define NEVERD_UNITTESTS_LIFT_X86_32_X87PROJECTIONFIXTURE_H

#include "NeverDLiftFixture.h"

#include "neverd/Common.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

class X87ProjectionFixture : public NeverDLiftTest {
protected:
  fs::path CaseDirectory;
  mutable unsigned CommandNumber = 0;

  void selectCase(neverd::Arch Architecture, unsigned Kind) {
    CaseDirectory =
        tmp() / ((Architecture == neverd::Arch::X86 ? "i386-" : "x64-") +
                 std::to_string(Kind));
    fs::create_directories(CaseDirectory);
  }

  fs::path tmpFile(const std::string &Name) const {
    return CaseDirectory / Name;
  }

  RunResult exec(const std::string &Program,
                 const std::vector<std::string> &Arguments) const {
    const auto Result = NeverDLiftTest::exec(Program, Arguments);
    if (const char *Root = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR");
        Root && *Root) {
      llvm::json::Array Invocation;
      Invocation.push_back(Program);
      for (const auto &Argument : Arguments)
        Invocation.push_back(Argument);
      std::string Record;
      llvm::raw_string_ostream Stream(Record);
      Stream << llvm::json::Value(
          llvm::json::Object{{"argv", std::move(Invocation)},
                             {"status", Result.exitCode},
                             {"stdout", Result.out},
                             {"stderr", Result.err}});
      std::ofstream Output(
          tmpFile("command-" + std::to_string(CommandNumber++) + ".json"));
      Output << Record;
      EXPECT_TRUE(Output.good());
    }
    return Result;
  }

  void TearDown() override {
    if (const char *Root = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR");
        Root && *Root) {
      const auto *Info = testing::UnitTest::GetInstance()->current_test_info();
      const fs::path Destination =
          fs::path(Root) / Info->test_suite_name() / Info->name();
      fs::create_directories(Destination.parent_path());
      fs::copy(tmp(), Destination, fs::copy_options::recursive);
    }
    NeverDLiftTest::TearDown();
  }
};

#endif // NEVERD_UNITTESTS_LIFT_X86_32_X87PROJECTIONFIXTURE_H
