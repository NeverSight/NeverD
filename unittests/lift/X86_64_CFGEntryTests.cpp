//===- X86_64_CFGEntryTests.cpp - backward-entry CFG regressions --------===//

#include "NeverDLiftFixture.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

class X86_64_CFGEntry : public NeverDLiftTest {};

namespace {

fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_backward_entry.o"; }

std::string functionDump(const std::string &Dump, const std::string &Name) {
  const std::string Header = "func " + Name + " @ ";
  size_t Begin = Dump.find(Header);
  if (Begin == std::string::npos)
    return {};
  size_t End = Dump.find("\nfunc ", Begin + Header.size());
  return Dump.substr(Begin, End == std::string::npos ? End : End - Begin);
}

std::string llvmFunction(const std::string &IR, const std::string &Name) {
  size_t Symbol = IR.find("@" + Name + "(");
  if (Symbol == std::string::npos)
    return {};
  size_t Begin = IR.rfind("define ", Symbol);
  size_t End = IR.find("\n}", Symbol);
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return IR.substr(Begin, End + 2 - Begin);
}

struct ParsedCfg {
  uint64_t Entry = 0;
  std::map<int, uint64_t> Starts;
  std::map<int, std::vector<int>> Succs;
};

ParsedCfg parseCfg(const std::string &Dump) {
  ParsedCfg Result;
  std::smatch Match;
  const std::regex Header(R"(^func [^ ]+ @ 0x([0-9A-Fa-f]+))");
  if (std::regex_search(Dump, Match, Header))
    Result.Entry = std::stoull(Match[1].str(), nullptr, 16);

  std::stringstream Lines(Dump);
  std::string Line;
  while (std::getline(Lines, Line)) {
    int Id = -1;
    unsigned long long Start = 0;
    if (std::sscanf(Line.c_str(), "  block %d [0x%llx", &Id, &Start) != 2)
      continue;
    Result.Starts[Id] = static_cast<uint64_t>(Start);
    const std::string Prefix = "succs=[";
    size_t SuccBegin = Line.find(Prefix);
    size_t SuccEnd = SuccBegin == std::string::npos
                         ? std::string::npos
                         : Line.find(']', SuccBegin + Prefix.size());
    if (SuccEnd == std::string::npos)
      continue;
    std::stringstream SS(Line.substr(SuccBegin + Prefix.size(),
                                     SuccEnd - SuccBegin - Prefix.size()));
    std::string Field;
    while (std::getline(SS, Field, ','))
      if (!Field.empty())
        Result.Succs[Id].push_back(std::stoi(Field));
  }
  return Result;
}

} // namespace

TEST_F(X86_64_CFGEntry, TrueEntryIsBlockZeroAndBackwardEdgeSurvives) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_backward_entry.o not built";
  auto Run = liftToLowIR(testObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;

  std::string Dump = functionDump(Run.out, "test_backward_entry");
  ASSERT_FALSE(Dump.empty()) << Run.out;
  ParsedCfg Cfg = parseCfg(Dump);
  ASSERT_NE(Cfg.Entry, 0U) << Dump;
  ASSERT_TRUE(Cfg.Starts.count(0)) << Dump;
  EXPECT_EQ(Cfg.Starts.at(0), Cfg.Entry) << Dump;

  for (int Id = 0; Id < static_cast<int>(Cfg.Starts.size()); ++Id)
    EXPECT_TRUE(Cfg.Starts.count(Id)) << "missing block id " << Id << "\n"
                                      << Dump;

  ASSERT_TRUE(Cfg.Succs.count(0)) << Dump;
  bool HasBackwardEdge = false;
  for (int Succ : Cfg.Succs.at(0)) {
    ASSERT_TRUE(Cfg.Starts.count(Succ)) << Dump;
    HasBackwardEdge |= Cfg.Starts.at(Succ) < Cfg.Entry;
  }
  EXPECT_TRUE(HasBackwardEdge) << Dump;
}

TEST_F(X86_64_CFGEntry, MedSsaHasNoCallClobberDefinitionCollision) {
  auto Run = liftToMedIR(testObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;
  EXPECT_EQ(Run.err.find("call clobber duplicates explicit definition"),
            std::string::npos)
      << Run.err;
}

TEST_F(X86_64_CFGEntry, RemovedEmptyTargetLeavesNoOutOfRangeSuccessor) {
  auto Run = liftToMedIR(testObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;
  std::string Dump = functionDump(Run.out, "test_unmapped_branch");
  ASSERT_FALSE(Dump.empty()) << Run.out;
  EXPECT_NE(Dump.find("block 0 succs=[]"), std::string::npos) << Dump;
  EXPECT_EQ(Run.err.find("invalid successor block id"), std::string::npos)
      << Run.err;
}

TEST_F(X86_64_CFGEntry, X87StateOpsAreSideEffectsAndPreserveIntegerResult) {
  auto Run = liftToLLVMIR(testObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;
  EXPECT_EQ(Run.err.find("INTRINSIC unhandled intrinsic"), std::string::npos)
      << Run.err;

  std::string IR = llvmFunction(Run.out, "test_x87_state_ops");
  ASSERT_FALSE(IR.empty()) << Run.out;
  EXPECT_NE(IR.find("fninit"), std::string::npos) << IR;
  EXPECT_NE(IR.find("fnclex"), std::string::npos) << IR;
  EXPECT_EQ(IR.find("ret i64 0"), std::string::npos) << IR;
}

TEST_F(X86_64_CFGEntry, OptimizedAndUnoptimizedLLVMEmissionSucceed) {
  auto Optimized = liftToLLVMIR(testObj());
  ASSERT_EQ(Optimized.exitCode, 0) << Optimized.err;

  auto Unoptimized = liftToLLVMIRUnopt(testObj());
  ASSERT_EQ(Unoptimized.exitCode, 0) << Unoptimized.err;
}
