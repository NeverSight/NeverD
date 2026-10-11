//===- LLVMInterpreterModelAlignmentTests.cpp - Guest address obligations ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMInterpreterModelTest.h"

#include "llvm/IR/Instructions.h"

namespace neverd::analysis::llvm_model_test {
namespace {
class LLVMGuestAlignment : public LLVMModel {
protected:
  static std::string address() {
    return R"(
      %sp_slot = getelementptr i8, ptr %state, i64 32
      %sp = load i64, ptr %sp_slot, align 8
      %addr = sub i64 %sp, 8
      %p = inttoptr i64 %addr to ptr
    )";
  }

  static LowIRIndependenceContract frame(uint32_t Align = 16,
                                         uint32_t Residue = 8) {
    auto C = llvmInterpreterMachineStateContract();
    C.Frame = LowIRIndependenceFrame{{32, 8}, -64, 8};
    C.Frame->EntryAlignment = InterpreterEntryAlignment{Align, Residue};
    C.PreservedRegisters.push_back({32, 8});
    C.PreservedFrameRanges = {{0, 8}};
    return C;
  }

  static std::string roundTrip(bool Load, llvm::StringRef Alignment,
                               unsigned Bytes = 2) {
    const auto T = "i" + std::to_string(Bytes * 8);
    const auto Suffix = Alignment.str();
    return address() + "%seed = load " + T + ", ptr %state, align 1\n" +
           "store " + T + " %seed, ptr %p" + (Load ? ", align 1" : Suffix) +
           "\n%v = load " + T + ", ptr %p" + (Load ? Suffix : ", align 1") +
           "\n%result = xor " + T + " %v, 61\nstore " + T +
           " %result, ptr %state, align 1\nret i64 0";
  }

  static Oracle roundTripOracle(unsigned Bytes = 2) {
    // Independent full-width address and byte-memory semantics. Upper bytes
    // of the destination word remain symbolic when the access is narrow.
    return Oracle(
        {op(NdOp::INT_SUB, r(256), {r(32), n(8)}),
         op(NdOp::STORE, {}, {r(256), r(0, Bytes)}),
         op(NdOp::INT_XOR, r(0, Bytes), {r(0, Bytes), n(61, Bytes)})});
  }

  void checkAlignment(bool Load) {
    for (uint32_t Align : {1U, 2U, 4U, 8U, 16U}) {
      SCOPED_TRACE(Align);
      parse(roundTrip(Load, ", align " + std::to_string(Align)));
      ASSERT_TRUE(Module);
      auto C = frame(Align, 8 % Align);
      expect(roundTripOracle(), Status::Proved, C);
      if (Align > 1) {
        C.Frame->EntryAlignment->Residue ^= 1;
        expect(roundTripOracle(), Status::ContractViolation, C);
        C.Frame->EntryAlignment.reset();
        expect(roundTripOracle(), Status::ContractViolation, C);
      }
    }
    // The entry congruence API has 32-bit fields. For LLVM's largest legal
    // alignment, constrain only the low root bytes; all high bits stay free.
    SCOPED_TRACE(uint64_t{1} << 32);
    parse(roundTrip(Load, ", align 4294967296"));
    ASSERT_TRUE(Module);
    auto C = frame();
    C.Frame->EntryAlignment.reset();
    C.EntryConstants.push_back({r(32, 4), 8});
    expect(roundTripOracle(), Status::Proved, C);
    C.EntryConstants.back().Value = 9;
    expect(roundTripOracle(), Status::ContractViolation, C);
    C.EntryConstants.pop_back();
    expect(roundTripOracle(), Status::ContractViolation, C);
  }
};
} // namespace

TEST_F(LLVMGuestAlignment, LoadAlignmentRequiresItsReachedAddress) {
  checkAlignment(true);
}

TEST_F(LLVMGuestAlignment, StoreAlignmentRequiresItsReachedAddress) {
  checkAlignment(false);
}

TEST_F(LLVMGuestAlignment, AccessWidthAndHighAddressBitsRemainObservable) {
  for (unsigned Bytes : {1U, 2U, 4U, 8U}) {
    SCOPED_TRACE(Bytes);
    for (bool Load : {false, true}) {
      SCOPED_TRACE(Load);
      parse(roundTrip(Load, ", align 16", Bytes));
      ASSERT_TRUE(Module);
      // No absolute address or data constant: every allowed high root bit
      // and the complete input word remain free in both executions.
      expect(roundTripOracle(Bytes), Status::Proved, frame());
      expect(Oracle(), Status::Different, frame());
    }
  }
}

TEST_F(LLVMGuestAlignment, OmittedTextualAlignmentUsesTheParsedABIAlignment) {
  for (bool Load : {false, true}) {
    parse(roundTrip(Load, ""));
    ASSERT_TRUE(Module);
    const auto Align = Module->getDataLayout()
                           .getABITypeAlign(llvm::Type::getInt16Ty(Context))
                           .value();
    ASSERT_GT(Align, 1U);
    for (const auto &I : function().getEntryBlock()) {
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I);
          Load && L && L->getPointerOperand()->getName() == "p")
        EXPECT_EQ(L->getAlign().value(), Align);
      if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I);
          !Load && S && S->getPointerOperand()->getName() == "p")
        EXPECT_EQ(S->getAlign().value(), Align);
    }
    auto C = frame(Align, 8 % Align);
    expect(roundTripOracle(), Status::Proved, C);
    C.Frame->EntryAlignment->Residue ^= 1;
    expect(roundTripOracle(), Status::ContractViolation, C);
  }
}

TEST_F(LLVMGuestAlignment, UnusedBadLoadRemainsStickyAfterAlignedAccesses) {
  parse(address() + R"(
    %bad_addr = sub i64 %sp, 7
    %bad_p = inttoptr i64 %bad_addr to ptr
    %unused = load i8, ptr %bad_p, align 16
    store i16 9, ptr %p, align 16
    %good = load i16, ptr %p, align 16
    store i64 0, ptr %state, align 8
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::INT_SUB, r(256), {r(32), n(8)}),
                 op(NdOp::STORE, {}, {r(256), n(9, 2)}),
                 op(NdOp::COPY, r(0), {n(0)})}),
         Status::ContractViolation, frame());
}

TEST_F(LLVMGuestAlignment, OverwrittenBadStoreRemainsSticky) {
  parse(address() + R"(
    %bad_addr = sub i64 %sp, 7
    %bad_p = inttoptr i64 %bad_addr to ptr
    store i16 3, ptr %bad_p, align 16
    store i16 5, ptr %bad_p, align 1
    %good_addr = sub i64 %sp, 24
    %good_p = inttoptr i64 %good_addr to ptr
    store i16 9, ptr %good_p, align 16
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::INT_SUB, r(256), {r(32), n(7)}),
                 op(NdOp::STORE, {}, {r(256), n(5, 2)}),
                 op(NdOp::INT_SUB, r(256), {r(32), n(24)}),
                 op(NdOp::STORE, {}, {r(256), n(9, 2)})}),
         Status::ContractViolation, frame());
}

TEST_F(LLVMGuestAlignment, UnreachableBadAccessDoesNotConstrainEntry) {
  for (bool Load : {false, true}) {
    const auto Bad = Load ? "%unused = load i8, ptr %bad_p, align 16\n"
                          : "store i8 3, ptr %bad_p, align 16\n";
    parse(address() + R"(
      %bad_addr = sub i64 %sp, 7
      %bad_p = inttoptr i64 %bad_addr to ptr
      %same = icmp eq i64 %sp, %sp
      br i1 %same, label %exit, label %bad
    bad:
    )" + Bad +
          "br label %exit\nexit:\nret i64 0");
    ASSERT_TRUE(Module);
    auto C = frame();
    C.Frame->EntryAlignment.reset();
    expect(Oracle(), Status::Proved, C);
  }
}

TEST_F(LLVMGuestAlignment, StateAlignmentAndUnsupportedEffectsStillRefuse) {
  for (const char *Body : {"store i64 3, ptr %state, align 16\nret i64 0",
                           "%p = getelementptr i8, ptr %state, i64 1\n"
                           "store i64 3, ptr %p, align 8\nret i64 0"}) {
    parse(Body);
    ASSERT_TRUE(Module);
    reject("unproved state alignment");
  }
  for (const char *Access : {"%v = load atomic i16, ptr %p monotonic, align 16",
                             "store atomic i16 3, ptr %p monotonic, align 16",
                             "%v = load volatile i16, ptr %p, align 16",
                             "store volatile i16 3, ptr %p, align 16"}) {
    parse(address() + Access + "\nret i64 0");
    ASSERT_TRUE(Module);
    reject("unsupported");
  }
}

TEST_F(LLVMGuestAlignment, EveryGuardChargesThreeOperationsAndFiveWorkUnits) {
  for (bool Load : {false, true}) {
    const std::string Access =
        Load ? "%unused = load i8, ptr %p" : "store i8 3, ptr %p";
    const auto Body = "%p = inttoptr i64 64 to ptr\n" + Access;
    parse(Body + ", align 1\nret i64 0");
    ASSERT_TRUE(Module);
    LLVMInterpreterModelLimits Limits;
    // One cast, one memory access and one return; no alignment guard.
    Limits.MaxOperations = 3;
    uint64_t BaseWork = 0;
    for (Limits.MaxWork = 1; Limits.MaxWork < 1024; ++Limits.MaxWork) {
      auto M = model(Limits);
      if (M) {
        BaseWork = Limits.MaxWork;
        break;
      }
      EXPECT_NE(llvm::toString(M.takeError()).find("budget exhausted"),
                std::string::npos);
    }
    ASSERT_NE(BaseWork, 0U);
    parse(Body + ", align 16\nret i64 0");
    ASSERT_TRUE(Module);
    // AND, comparison and sticky OR, with two allocated temporary values.
    Limits.MaxOperations = 6;
    Limits.MaxWork = BaseWork + 5;
    auto Exact = model(Limits);
    ASSERT_TRUE(static_cast<bool>(Exact)) << llvm::toString(Exact.takeError());
    ASSERT_EQ(Exact->Function.Blocks.size(), 1U);
    EXPECT_EQ(Exact->Function.Blocks.front().Ops.size(), 6U);
    --Limits.MaxOperations;
    auto ShortOps = model(Limits);
    ASSERT_FALSE(static_cast<bool>(ShortOps));
    EXPECT_NE(llvm::toString(ShortOps.takeError()).find("budget exhausted"),
              std::string::npos);
    ++Limits.MaxOperations;
    --Limits.MaxWork;
    auto ShortWork = model(Limits);
    ASSERT_FALSE(static_cast<bool>(ShortWork));
    EXPECT_NE(llvm::toString(ShortWork.takeError()).find("budget exhausted"),
              std::string::npos);
  }
}
} // namespace neverd::analysis::llvm_model_test
