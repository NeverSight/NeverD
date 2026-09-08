//===- SemanticRoundTripFixtureTests.cpp - Differential harness integrity
//---===//
//
// NeverD Decompiler — Semantic Roundtrip Verification Tests
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"
#include "gtest/gtest-spi.h"

namespace {

class SemanticRoundTripIntegrity : public SemanticRoundTripFixture {
protected:
  enum class Fault { None, LiftFailure, NoFunctions };
  Fault InjectedFault = Fault::None;
  bool LiftCalled = false;
  bool FunctionCountCalled = false;

  int liftToObject(neverd_session_t Session, const char *ObjectPath,
                   int NoOpt) override {
    LiftCalled = true;
    if (InjectedFault == Fault::LiftFailure) {
      // Use a real SDK failure, including its native diagnostic. The original
      // object has already compiled and executed before this boundary is hit.
      const std::string MissingPath = std::string(ObjectPath) + ".missing";
      return SemanticRoundTripFixture::liftToObject(Session,
                                                    MissingPath.c_str(), NoOpt);
    }
    return SemanticRoundTripFixture::liftToObject(Session, ObjectPath, NoOpt);
  }

  int roundTripFunctionCount(neverd_session_t Session) override {
    FunctionCountCalled = true;
    if (InjectedFault == Fault::NoFunctions)
      return 0;
    return SemanticRoundTripFixture::roundTripFunctionCount(Session);
  }

  static RoundTripTC sample() {
    RoundTripTC TC;
    TC.Name = "fixture_add";
    TC.CSrc = "unsigned int fixture_add(unsigned int a, unsigned int b) {"
              "  return a + b;"
              "}";
    TC.Args = {17, 25};
    TC.Category = "fixture_integrity";
    TC.OptLevel = 1;
    return TC;
  }

  void expectFatalFailure(Fault Failure, const char *Diagnostic) {
    InjectedFault = Failure;
    ::testing::TestPartResultArray Results;
    {
      ::testing::ScopedFakeTestPartResultReporter Reporter(
          ::testing::ScopedFakeTestPartResultReporter::
              INTERCEPT_ONLY_CURRENT_THREAD,
          &Results);
      roundTripX64(sample());
    }

    if (!LiftCalled && Results.size() == 1 &&
        Results.GetTestPartResult(0).skipped())
      GTEST_SKIP() << Results.GetTestPartResult(0).message();

    ASSERT_TRUE(LiftCalled)
        << "fixture preparation did not reach the SDK: "
        << (Results.size() ? Results.GetTestPartResult(0).message()
                           : "no diagnostic");
    ASSERT_EQ(Results.size(), 1);
    const auto &Result = Results.GetTestPartResult(0);
    EXPECT_EQ(Result.type(), ::testing::TestPartResult::kFatalFailure)
        << Result.message();
    EXPECT_NE(std::string(Result.message()).find(Diagnostic), std::string::npos)
        << Result.message();
    EXPECT_NE(std::string(Result.message()).find("fixture_add"),
              std::string::npos)
        << Result.message();
    EXPECT_EQ(FunctionCountCalled, Failure == Fault::NoFunctions);
  }
};

TEST_F(SemanticRoundTripIntegrity,
       LiftFailureIsFatalAndRetainsNativeDiagnostic) {
  expectFatalFailure(Fault::LiftFailure, ".missing");
}

TEST_F(SemanticRoundTripIntegrity, ZeroRecoveredFunctionsIsFatal) {
  expectFatalFailure(Fault::NoFunctions, "No functions in roundtrip result");
}

TEST_F(SemanticRoundTripIntegrity, X64StillComparesExecutedRoundtrip) {
  roundTripX64(sample());
  if (IsSkipped() || HasFatalFailure())
    return;
  EXPECT_TRUE(FunctionCountCalled);
}

TEST_F(SemanticRoundTripIntegrity, X86StillComparesExecutedRoundtrip) {
  roundTripX86(sample());
  if (IsSkipped() || HasFatalFailure())
    return;
  EXPECT_TRUE(FunctionCountCalled);
}

TEST_F(SemanticRoundTripIntegrity, AArch64StillComparesExecutedRoundtrip) {
  roundTripAArch64(sample());
  if (IsSkipped() || HasFatalFailure())
    return;
  EXPECT_TRUE(FunctionCountCalled);
}

TEST_F(SemanticRoundTripIntegrity, ARM32StillComparesExecutedRoundtrip) {
  roundTripARM32(sample());
  if (IsSkipped() || HasFatalFailure())
    return;
  EXPECT_TRUE(FunctionCountCalled);
}

} // namespace
