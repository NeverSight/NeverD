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
    std::string Text = Result.out;
    if (Control) {
      const auto Start = Text.find("func " + Function + " @ ");
      ASSERT_NE(Start, std::string::npos) << Text;
      Text = Text.substr(Start, Text.find("\nfunc ", Start) - Start);
    }
    size_t Position = 0;
    for (neverd::Intrinsic Id : {neverd::Intrinsic::X86ReadMXCSR, Operation,
                                 neverd::Intrinsic::X86WriteMXCSR}) {
      const std::string Operand =
          " cst:0x" + llvm::utohexstr(static_cast<unsigned>(Id)) + ":2";
      const auto Found = Text.find(Operand, Position);
      ASSERT_NE(Found, std::string::npos) << Result.out;
      const auto Begin = Text.rfind('\n', Found);
      const auto Intrinsic =
          Text.find("INTRINSIC", Begin == std::string::npos ? 0 : Begin);
      ASSERT_LE(Intrinsic, Found) << Result.out;
      if (Control && Id == Operation) {
        const std::string Expected =
            " cst:0x" + llvm::utohexstr(*Control) + ":1";
        auto ControlPosition = Found + Operand.size();
        if (Operation == neverd::Intrinsic::X86FPArithMemoryState) {
          // The memory form carries its address before the control operand.
          ControlPosition = Text.find(' ', ControlPosition + 1);
          ASSERT_NE(ControlPosition, std::string::npos) << Text;
          ASSERT_LT(ControlPosition, Text.find('\n', Found)) << Text;
        }
        EXPECT_EQ(Text.substr(ControlPosition, Expected.size()), Expected)
            << Text;
      }
      Position = Found + Operand.size();
    }
  }
};

#endif
