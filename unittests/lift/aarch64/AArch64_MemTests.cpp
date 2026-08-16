#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/AArch64Regs.h"

#include <algorithm>
#include <array>

using namespace neverd;

class AArch64_Mem : public NeverDLiftTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto syntax = checkHighCClangSyntax(
        CFile, {"-target", "aarch64-none-elf", "-ffreestanding",
                "-march=armv8.4-a", "-std=gnu11"});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }
};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_mem_a64.o";
}

static std::string functionIR(const std::string &IR, const std::string &Name) {
  auto NamePos = IR.find("@" + Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = IR.rfind("define ", NamePos);
  auto End = IR.find("\n}", NamePos);
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return IR.substr(Begin, End + 2 - Begin);
}

static std::string functionC(const std::string &Source,
                             const std::string &Name) {
  auto NamePos = Source.find(Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = Source.rfind('\n', NamePos);
  auto End = Source.find("\n}", NamePos);
  if (End == std::string::npos)
    return {};
  Begin = Begin == std::string::npos ? 0 : Begin + 1;
  return Source.substr(Begin, End + 2 - Begin);
}

TEST_F(AArch64_Mem, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_mem_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_Mem, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_Mem, LdrStrHasLoadStore) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("LOAD") != std::string::npos)
        << "LDR should produce LOAD";
    EXPECT_TRUE(r.out.find("STORE") != std::string::npos)
        << "STR should produce STORE";
}

TEST_F(AArch64_Mem, UxtbLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_AND") != std::string::npos ||
                r.out.find("INT_ZEXT") != std::string::npos ||
                r.out.find("SUBBYTES") != std::string::npos)
        << "UXTB should produce masking/extension";
}

TEST_F(AArch64_Mem, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}

TEST_F(AArch64_Mem, LdaprKeepsAcquireOrdering) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  const struct {
    const char *Name;
    const char *Type;
    unsigned Align;
  } Cases[] = {{"test_ldapr_a64", "i64", 8},
               {"test_ldaprb_a64", "i8", 1},
               {"test_ldaprh_a64", "i16", 2}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto F = functionIR(r.out, Case.Name);
    ASSERT_FALSE(F.empty()) << r.out;
    EXPECT_NE(F.find(std::string("load atomic ") + Case.Type),
              std::string::npos)
        << F;
    EXPECT_NE(F.find("acquire, align " + std::to_string(Case.Align)),
              std::string::npos)
        << F;
  }
}

TEST_F(AArch64_Mem, LdaprHighCUsesAcquireAtomicLoadAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__atomic_load_n"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_ACQUIRE"), std::string::npos) << source;
  for (const char *Name :
       {"test_ldapr_a64", "test_ldaprb_a64", "test_ldaprh_a64"}) {
    SCOPED_TRACE(Name);
    auto F = functionC(source, Name);
    ASSERT_FALSE(F.empty()) << source;
    auto Load = F.find("neverd_mem_load_acquire_");
    ASSERT_NE(Load, std::string::npos) << F;
    EXPECT_EQ(F.find("neverd_mem_load_acquire_", Load + 1), std::string::npos)
        << "LDAPR must be evaluated exactly once:\n"
        << F;
  }

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_Mem, StlurKeepsReleaseOrderingAtEveryAccessWidth) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  const struct {
    const char *Name;
    const char *Type;
    unsigned Align;
  } Cases[] = {{"test_stlur_a64", "i64", 8},
               {"test_stlurb_a64", "i8", 1},
               {"test_stlurh_a64", "i16", 2}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto F = functionIR(r.out, Case.Name);
    ASSERT_FALSE(F.empty()) << r.out;
    EXPECT_NE(F.find(std::string("store atomic ") + Case.Type),
              std::string::npos)
        << F;
    EXPECT_NE(F.find("release, align " + std::to_string(Case.Align)),
              std::string::npos)
        << F;
  }
}

TEST_F(AArch64_Mem, StlurHighCUsesReleaseAtomicStoreAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__atomic_store_n"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_RELEASE"), std::string::npos) << source;

  for (const char *Name :
       {"test_stlur_a64", "test_stlurb_a64", "test_stlurh_a64"}) {
    SCOPED_TRACE(Name);
    auto F = functionC(source, Name);
    ASSERT_FALSE(F.empty()) << source;
    EXPECT_NE(F.find("neverd_mem_store_release_"), std::string::npos) << F;
  }

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_Mem, PostIndexLdrKeepsBothAggregateReturnRegisters) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto F = functionIR(r.out, "test_ldr_post_return_a64");
  ASSERT_FALSE(F.empty()) << r.out;
  EXPECT_NE(F.find("define dso_local { i64, i64 }"), std::string::npos) << F;
  EXPECT_NE(F.find("load i64"), std::string::npos) << F;
  EXPECT_NE(F.find("add i64"), std::string::npos) << F;
  EXPECT_NE(F.find(", 8"), std::string::npos) << F;
  EXPECT_NE(F.find("ret { i64, i64 }"), std::string::npos) << F;
}

TEST_F(AArch64_Mem, LdrLiteralKeepsTheFullVirtualAddress) {
    constexpr va_t InsnVA = 0x100000460ULL;
    constexpr va_t LiteralVA = InsnVA + 8;
    // ldr x0, #8
    constexpr std::array<uint8_t, 4> Bytes = {0x40, 0x00, 0x00, 0x58};

    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), InsnVA, Insn),
              4);

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);

    const auto Load = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD;
    });
    ASSERT_NE(Load, Ops.end());
    ASSERT_EQ(Load->NumInputs, 1);

    const auto Address = std::find_if(
        Ops.begin(), Load, [&](const LowOp &Op) {
            return Op.Opcode == NdOp::COPY && Op.Output == Load->Inputs[0] &&
                   Op.NumInputs == 1 && Op.Inputs[0].isConst();
        });
    ASSERT_NE(Address, Load);
    EXPECT_EQ(Address->Inputs[0].Offset, LiteralVA);
}

TEST_F(AArch64_Mem, Ld64bLoadsEightConsecutiveGPRs) {
  // ld64b x0, [x8]
  constexpr std::array<uint8_t, 4> Bytes = {0x00, 0xD1, 0x3F, 0xF8};

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn), 4);

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);

  std::vector<const LowOp *> Loads;
  for (const auto &Op : Ops)
    if (Op.Opcode == NdOp::LOAD)
      Loads.push_back(&Op);

  ASSERT_EQ(Loads.size(), 8u);
  const NdVar BaseEA = Loads.front()->Inputs[0];
  for (unsigned I = 0; I < Loads.size(); ++I) {
    const LowOp &Load = *Loads[I];
    EXPECT_EQ(Load.Output, NdVar::reg(a64reg::X0 + I * 8, 8));
    ASSERT_EQ(Load.NumInputs, 1);
    if (I == 0) {
      EXPECT_EQ(Load.Inputs[0], BaseEA);
      continue;
    }

    const auto Address =
        std::find_if(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
          return Op.Opcode == NdOp::INT_ADD && Op.Output == Load.Inputs[0] &&
                 Op.NumInputs == 2 && Op.Inputs[0] == BaseEA &&
                 Op.Inputs[1].isConst();
        });
    ASSERT_NE(Address, Ops.end());
    EXPECT_EQ(Address->Inputs[1].Offset, I * 8);
  }
}

TEST_F(AArch64_Mem, StlurImmediateUsesEncodedSourceBaseAndSignedOffset) {
  const struct {
    std::array<uint8_t, 4> Bytes;
    unsigned SourceIndex;
    uint16_t SourceSize;
    uint16_t AccessSize;
    unsigned BaseIndex;
    int64_t Offset;
  } Cases[] = {{{0x09, 0x81, 0x00, 0xD9}, 9, 8, 8, 8, 8},
               {{0x4B, 0xF1, 0x1F, 0x19}, 11, 4, 1, 10, -1},
               {{0x8D, 0x21, 0x00, 0x59}, 13, 4, 2, 12, 2}};

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Offset);
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Case.Bytes.data(), Case.Bytes.size(), 0x1000,
                                   Insn),
              4);

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    EXPECT_EQ(
        std::count_if(Ops.begin(), Ops.end(),
                      [](const LowOp &Op) { return Op.Opcode == NdOp::LOAD; }),
        0u);

    const auto Store =
        std::find_if(Ops.begin(), Ops.end(),
                     [](const LowOp &Op) { return Op.Opcode == NdOp::STORE; });
    ASSERT_NE(Store, Ops.end());
    ASSERT_EQ(Store->NumInputs, 2);
    EXPECT_EQ(Store->MemoryOrdering, NdMemoryOrdering::Release);

    const NdVar Source =
        NdVar::reg(a64reg::X0 + Case.SourceIndex * 8, Case.SourceSize);
    if (Case.SourceSize == Case.AccessSize) {
      EXPECT_EQ(Store->Inputs[1], Source);
    } else {
      const auto Truncate =
          std::find_if(Ops.begin(), Store, [&](const LowOp &Op) {
            return Op.Opcode == NdOp::SUBBYTES &&
                   Op.Output == Store->Inputs[1] && Op.NumInputs == 2 &&
                   Op.Inputs[0] == Source && Op.Inputs[1].isConst() &&
                   Op.Inputs[1].Offset == 0;
          });
      ASSERT_NE(Truncate, Store);
      EXPECT_EQ(Truncate->Output.Size, Case.AccessSize);
    }

    const NdVar Base = NdVar::reg(a64reg::X0 + Case.BaseIndex * 8, 8);
    const auto BaseCopy =
        std::find_if(Ops.begin(), Store, [&](const LowOp &Op) {
          return Op.Opcode == NdOp::COPY && Op.Output == Store->Inputs[0] &&
                 Op.NumInputs == 1 && Op.Inputs[0] == Base;
        });
    ASSERT_NE(BaseCopy, Store);
    const auto Address = std::find_if(Ops.begin(), Store, [&](const LowOp &Op) {
      return Op.Opcode == NdOp::INT_ADD && Op.Output == Store->Inputs[0] &&
             Op.NumInputs == 2 && Op.Inputs[0] == Store->Inputs[0] &&
             Op.Inputs[1].isConst() &&
             Op.Inputs[1].Offset == static_cast<uint64_t>(Case.Offset);
    });
    ASSERT_NE(Address, Store);
  }
}
