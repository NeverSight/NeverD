//===- AArch64ProjectionTests.cpp - ARM64 architectural MMU execution -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64Machine.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <unicorn/unicorn.h>

namespace neverd::emulation {
namespace {
#define NEVERD_CPU_TEST_CODE(Name, ...)                                        \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#define NEVERD_CPU_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "../CPUArchitectureCases.def"
#undef NEVERD_CPU_TEST_CODE
#undef NEVERD_CPU_TEST_VALUE
class AArch64Projection : public testing::Test {
protected:
  uc_engine *CPU = nullptr;
  std::unique_ptr<MemoryProjection> Memory;
  void TearDown() override {
    if (CPU)
      uc_close(CPU);
  }
  void SetUp() override {
    auto M = MemoryProjection::create(Limit);
    ASSERT_TRUE(bool(M));
    Memory = std::move(*M);
    ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &CPU), UC_ERR_OK);
    // Exercise architectural stage-one translation, not Unicorn's virtual TLB.
    ASSERT_EQ(uc_ctl_tlb_mode(CPU, UC_TLB_CPU), UC_ERR_OK);
    for (const auto &Mapping : Memory->registrations())
      ASSERT_EQ(uc_mem_map_ptr(CPU, Mapping.Physical, Mapping.Size, UC_PROT_ALL,
                               Mapping.Backing),
                UC_ERR_OK);
    ASSERT_EQ(
        llvm::toString(Memory->map(Code, PageSize, Read | Write | Execute)),
        "");
    ASSERT_EQ(llvm::toString(Memory->map(Data, PageSize, Read | Write)), "");
#define NEVERD_CPU_TEST_CP_REGISTER(Name, Op0, Op1, CRn, CRm, Op2, Value)      \
  {                                                                            \
    uc_arm64_cp_reg R{CRn, CRm, Op0, Op1, Op2, Value};                         \
    ASSERT_EQ(uc_reg_write(CPU, UC_ARM64_REG_CP_REG, &R), UC_ERR_OK);          \
  }
#include "../CPUArchitectureCases.def"
#undef NEVERD_CPU_TEST_CP_REGISTER
    uint64_t PState = aarch64::PStateEL1h | aarch64::PStateDAIF;
    ASSERT_EQ(uc_reg_write(CPU, UC_ARM64_REG_PSTATE, &PState), UC_ERR_OK);
#define NEVERD_AARCH64_SYSTEM_REGISTER(Name, Op0, Op1, CRn, CRm, Op2)          \
  auto Name = [&](uint64_t Value) {                                            \
    uc_arm64_cp_reg R{CRn, CRm, Op0, Op1, Op2, Value};                         \
    return uc_reg_write(CPU, UC_ARM64_REG_CP_REG, &R);                         \
  };
#include "arch/aarch64/AArch64SystemRegisters.def"
#undef NEVERD_AARCH64_SYSTEM_REGISTER
    ASSERT_EQ(Ttbr0El1(aarch64::LowRoot), UC_ERR_OK);
    ASSERT_EQ(Ttbr1El1(aarch64::HighRoot), UC_ERR_OK);
    ASSERT_EQ(TcrEl1(aarch64::TCR), UC_ERR_OK);
    ASSERT_EQ(MairEl1(aarch64::MAIR), UC_ERR_OK);
    ASSERT_EQ(SctlrEl1(aarch64::SCTLR), UC_ERR_OK);
    ASSERT_EQ(VbarEl1(aarch64::VectorGPA), UC_ERR_OK);
    std::array<uint8_t, sizeof(Load)> Bytes{};
    for (size_t N = 0; N < std::size(Load); ++N)
      llvm::support::endian::write32le(Bytes.data() + N * sizeof(uint32_t),
                                       Load[N]);
    ASSERT_EQ(llvm::toString(Memory->write(Code, Bytes)), "");
  }
  void project() {
    ASSERT_EQ(llvm::toString(buildAArch64PageTables(*Memory)), "");
    ASSERT_EQ(uc_emu_start(CPU, aarch64::EntryGPA, 0, 0,
                           std::size(aarch64::Maintenance)),
              UC_ERR_OK);
  }
};
TEST_F(AArch64Projection, ExecutesThroughRealPageTablesAndInvalidatesAliases) {
  ASSERT_EQ(llvm::toString(Memory->map(Stack, PageSize, Read | Write)), "");
  std::array<uint8_t, sizeof(uint64_t)> Bytes{};
  llvm::support::endian::write64le(Bytes.data(), Value);
  ASSERT_EQ(llvm::toString(Memory->write(Data, Bytes)), "");
  llvm::support::endian::write64le(Bytes.data(), Updated);
  ASSERT_EQ(llvm::toString(Memory->write(Stack, Bytes)), "");
  ASSERT_EQ(
      llvm::toString(Memory->aliases({}, {{Alias, Data, PageSize, Read}})), "");
  for (unsigned N = 0; N < Remaps; ++N) {
    auto Source = N % 2 ? Stack : Data;
    ASSERT_EQ(llvm::toString(Memory->aliases(
                  {{Alias, PageSize}}, {{Alias, Source, PageSize, Read}})),
              "");
    ASSERT_NO_FATAL_FAILURE(project());
    uint64_t Address = Alias;
    ASSERT_EQ(uc_reg_write(CPU, UC_ARM64_REG_X1, &Address), UC_ERR_OK);
    ASSERT_EQ(uc_emu_start(CPU, Code, 0, 0, 1), UC_ERR_OK);
    uint64_t Actual = 0;
    ASSERT_EQ(uc_reg_read(CPU, UC_ARM64_REG_X0, &Actual), UC_ERR_OK);
    EXPECT_EQ(Actual, N % 2 ? Updated : Value);
  }
}
TEST_F(AArch64Projection, DeniesWritesUsingArchitecturalPagePermissions) {
  std::array<uint8_t, sizeof(Store)> Bytes{};
  for (size_t N = 0; N < std::size(Store); ++N)
    llvm::support::endian::write32le(Bytes.data() + N * sizeof(uint32_t),
                                     Store[N]);
  ASSERT_EQ(llvm::toString(Memory->write(Code, Bytes)), "");
  ASSERT_EQ(llvm::toString(Memory->protect(Data, PageSize, Read)), "");
  ASSERT_NO_FATAL_FAILURE(project());
  uint64_t Address = Data, Payload = Value;
  ASSERT_EQ(uc_reg_write(CPU, UC_ARM64_REG_X1, &Address), UC_ERR_OK);
  ASSERT_EQ(uc_reg_write(CPU, UC_ARM64_REG_X0, &Payload), UC_ERR_OK);
  EXPECT_NE(uc_emu_start(CPU, Code, 0, 0, 1), UC_ERR_OK);
  std::array<uint8_t, sizeof(uint64_t)> Actual{};
  ASSERT_EQ(llvm::toString(Memory->read(Data, Actual)), "");
  EXPECT_EQ(llvm::support::endian::read64le(Actual.data()), 0u);
}
} // namespace
} // namespace neverd::emulation
