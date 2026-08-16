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
    auto stringHeader = tmpFile("string.h");
    std::ofstream shim(stringHeader);
    ASSERT_TRUE(shim.good());
    shim << "void *memcpy(void *, const void *, __SIZE_TYPE__);\n";
    shim.close();

    auto syntax = exec(NEVERD_TEST_CLANG,
                       {"-target", "aarch64-none-elf", "-ffreestanding",
                        "-march=armv8.3-a+rcpc", "-std=gnu11", "-I",
                        tmp().string(), "-fsyntax-only", CFile.string()});
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
