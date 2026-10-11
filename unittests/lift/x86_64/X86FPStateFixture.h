//===- X86FPStateFixture.h - Scalar FP state dump contracts -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_X86FPSTATEFIXTURE_H
#define NEVERD_UNITTESTS_X86FPSTATEFIXTURE_H

#include "../NeverDLiftFixture.h"

#include "neverd/ir/X86FPState.h"

#include "llvm/ADT/StringExtras.h"

#include <optional>

class X86FPStateLiftTest : public NeverDLiftTest {
protected:
  void verifyScalarFPState(const fs::path &Binary, const std::string &Function,
                           neverd::Intrinsic Operation,
                           std::optional<unsigned> Control = std::nullopt) {
    const auto Result = liftToLowIR(Binary, Function);
    ASSERT_EQ(Result.exitCode, 0) << Result.err;
    const auto FunctionBegin = Result.out.find("func " + Function + " @ ");
    ASSERT_NE(FunctionBegin, std::string::npos) << Result.out;
    const auto FunctionEnd = Result.out.find("\nfunc ", FunctionBegin);
    const auto Dump =
        Result.out.substr(FunctionBegin, FunctionEnd == std::string::npos
                                             ? std::string::npos
                                             : FunctionEnd - FunctionBegin);
    size_t Position = 0;
    for (neverd::Intrinsic Id : {neverd::Intrinsic::X86ReadMXCSR, Operation,
                                 neverd::Intrinsic::X86WriteMXCSR}) {
      const std::string Operand =
          " cst:0x" + llvm::utohexstr(static_cast<unsigned>(Id)) + ":2";
      const auto Found = Dump.find(Operand, Position);
      ASSERT_NE(Found, std::string::npos) << Dump;
      const auto Begin = Dump.rfind('\n', Found);
      const auto Intrinsic =
          Dump.find("INTRINSIC", Begin == std::string::npos ? 0 : Begin);
      ASSERT_LE(Intrinsic, Found) << Dump;
      if (Id == Operation && Control) {
        const std::string ControlledOperand =
            Operand + " cst:0x" + llvm::utohexstr(*Control) + ":1";
        EXPECT_EQ(
            Dump.compare(Found, ControlledOperand.size(), ControlledOperand), 0)
            << Dump;
      }
      Position = Found + Operand.size();
    }
  }
};

#endif
