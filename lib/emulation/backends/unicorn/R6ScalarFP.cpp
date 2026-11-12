//===- R6ScalarFP.cpp - Concrete Release 6 scalar FPU ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/arch/mips/R6ScalarFP.h"

#include "unicorn/unicorn.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/intrinsics/MIPS/MIPSR6FPOperations.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x10000;
class ScalarFPU final : public MIPSR6FPExecutor {
public:
  ~ScalarFPU() override {
    if (Initial)
      uc_context_free(Initial);
    if (CPU)
      uc_close(CPU);
  }
  llvm::Error initialize(Arch Target) {
    const bool Wide = isMIPS64(Target), Little = archLittleEndian(Target);
    uc_err Error = uc_open(UC_ARCH_MIPS,
                           uc_mode((Wide ? UC_MODE_MIPS64 : UC_MODE_MIPS32) |
                                   (Little ? 0 : UC_MODE_BIG_ENDIAN)),
                           &CPU);
    if (!Error)
      Error =
          uc_ctl_set_cpu_model(CPU, Wide ? int(UC_CPU_MIPS64_I6400)
                                         : int(UC_CPU_MIPS32_MIPS32R6_GENERIC));
    if (!Error)
      Error = uc_mem_map(CPU, Code, 4096, UC_PROT_ALL);
    uint64_t PC = Code;
    const auto Install = [&](uint32_t Word) {
      uint8_t Data[4];
      for (unsigned I = 0; I < 4; ++I)
        Data[I] = Word >> (8 * (Little ? I : 3 - I));
      if (!Error)
        Error = uc_mem_write(CPU, PC, Data, 4);
      PC += 4;
    };
    for (auto W : {0x40086000u, 0x3c092400u, 0x01094025u, 0x40886000u, 0xc0u})
      Install(W);
    for (const auto &F : mips::R6FPOperations) {
      unsigned FS = F.Arity == 3 ? 1 : 0, FT = F.Arity == 3   ? 2
                                               : F.Arity == 2 ? 1
                                                              : 0;
      Install(F.encoding() | (FS << 11) | (FT << 16));
    }
    ControlPC = PC;
    for (unsigned Sel = 0; Sel < 32; ++Sel) {
      Install(0x44480000u | Sel << 11); // cfc1 t0,sel
      Install(0x44c80000u | Sel << 11); // ctc1 t0,sel
    }
    if (!Error)
      Error = uc_emu_start(CPU, Code, Code + 20, 0, 5);
    if (!Error)
      Error = uc_context_alloc(CPU, &Initial);
    if (!Error)
      Error = uc_context_save(CPU, Initial);
    if (Error)
      return llvm::createStringError("MIPS Release 6 FPU setup: %s",
                                     uc_strerror(Error));
    return llvm::Error::success();
  }
  std::optional<MIPSR6FPResult> control(unsigned Sel, bool Write,
                                        uint32_t Value, uint32_t CSR) override {
    if (!mips::r6FPControlSelector(Sel, Write) || !restore(CSR))
      return std::nullopt;
    // Fields outside the writable FEXR and FENR views are unpredictable.
    if (Write && ((Sel == 26 && (Value & ~0x0003f07cu)) ||
                  (Sel == 28 && (Value & ~0x00000f87u)) ||
                  (Sel == 25 && (Value & ~0xffu))))
      return std::nullopt;
    uint64_t R = Value;
    if (Write && uc_reg_write(CPU, UC_MIPS_REG_8, &R) != UC_ERR_OK)
      return std::nullopt;
    return run(ControlPC + 8 * Sel + 4 * Write, UC_MIPS_REG_8);
  }
  std::optional<MIPSR6FPResult>
  execute(Intrinsic Id, uint32_t CSR,
          llvm::ArrayRef<uint64_t> Inputs) override {
    if (!mips::isR6FPArithmetic(Id) ||
        Inputs.size() != mips::r6FPOperation(Id).Arity || !restore(CSR))
      return std::nullopt;
    for (unsigned I = 0; I < Inputs.size(); ++I)
      if (uc_reg_write(CPU, UC_MIPS_REG_F0 + I, &Inputs[I]) != UC_ERR_OK)
        return std::nullopt;
    const auto PC =
        Code + 20 + 4 * (unsigned(Id) - unsigned(Intrinsic::MIPSFPADD_S));
    return run(PC, UC_MIPS_REG_F0);
  }

private:
  bool restore(uint32_t CSR) {
    uint32_t Observed = 0;
    if (uc_context_restore(CPU, Initial) != UC_ERR_OK)
      return false;
    const auto Written = uc_reg_write(CPU, UC_MIPS_REG_FCSR, &CSR);
    if (Written == UC_ERR_EXCEPTION) {
      // The public register setter refuses a pending enabled Cause. A real
      // CTC1 commits that state before trapping, allowing a handler to read
      // it and clear the cause on a subsequent invocation.
      uint32_t Cleared = CSR & ~0x0003f000u;
      uint64_t Value = CSR;
      if (uc_reg_write(CPU, UC_MIPS_REG_FCSR, &Cleared) != UC_ERR_OK ||
          uc_reg_write(CPU, UC_MIPS_REG_8, &Value) != UC_ERR_OK)
        return false;
      const auto PC = ControlPC + 8 * 31 + 4;
      if (uc_emu_start(CPU, PC, PC + 4, 0, 1) != UC_ERR_EXCEPTION)
        return false;
    } else if (Written != UC_ERR_OK)
      return false;
    return uc_reg_read(CPU, UC_MIPS_REG_FCSR, &Observed) == UC_ERR_OK &&
           Observed == CSR;
  }
  std::optional<MIPSR6FPResult> run(uint64_t PC, int OutputRegister) {
    uint32_t Observed = 0;
    const auto Status = uc_emu_start(CPU, PC, PC + 4, 0, 1);
    if (Status != UC_ERR_OK && Status != UC_ERR_EXCEPTION)
      return std::nullopt;
    if (uc_reg_read(CPU, UC_MIPS_REG_FCSR, &Observed) != UC_ERR_OK)
      return std::nullopt;
    if (Status == UC_ERR_EXCEPTION) {
      if (!((Observed >> 12) & ((Observed >> 7 & 31) | 32)))
        return std::nullopt;
      return MIPSR6FPResult{Observed, std::nullopt};
    }
    uint64_t Result = 0;
    if (uc_reg_read(CPU, OutputRegister, &Result) != UC_ERR_OK)
      return std::nullopt;
    return MIPSR6FPResult{Observed, Result};
  }
  uint64_t ControlPC = 0;
  uc_engine *CPU = nullptr;
  uc_context *Initial = nullptr;
};
} // namespace
llvm::Expected<std::unique_ptr<MIPSR6FPExecutor>>
createR6ScalarFPExecutor(Arch Target) {
  if (!isMIPS(Target))
    return llvm::createStringError(
        "Release 6 scalar FPU requires a MIPS target");
  auto F = std::make_unique<ScalarFPU>();
  if (auto Error = F->initialize(Target))
    return Error;
  return F;
}
} // namespace neverd::emulation
