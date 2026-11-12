//===- P5600MSA.cpp - Isolated MSA floating-point execution --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Unicorn exposes scalar GPRs but not the complete MSA register bank. Transfer
/// lanes with guest instructions. Preinstall each typed FP operation once;
/// restore the bound CPU snapshot before each call, including after a trap.
//===----------------------------------------------------------------------===//

#include "neverd/emulation/arch/mips/P5600MSA.h"

#include "unicorn/unicorn.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x10000;
constexpr unsigned SetupWords = 11;
constexpr uint64_t InputPC = Code + 64, OutputPC = InputPC + 64,
                   OperationsPC = OutputPC + 64;

class UnicornMSAExecutor final : public MIPSMSAExecutor {
public:
  ~UnicornMSAExecutor() override {
    if (Initial)
      uc_context_free(Initial);
    if (CPU)
      uc_close(CPU);
  }
  llvm::Error initialize(Arch Target) {
    Wide = isMIPS64(Target);
    const bool Little = archLittleEndian(Target);
    auto Error = uc_open(UC_ARCH_MIPS,
                         uc_mode((Wide ? UC_MODE_MIPS64 : UC_MODE_MIPS32) |
                                 (Little ? 0 : UC_MODE_BIG_ENDIAN)),
                         &CPU);
    if (!Error)
      Error = uc_ctl_set_cpu_model(CPU, Wide ? int(UC_CPU_MIPS64_I6400)
                                             : int(UC_CPU_MIPS32_P5600));
    if (!Error)
      Error = uc_mem_map(CPU, Code, 4096, UC_PROT_ALL);
    const auto Install = [&](uint64_t PC, uint32_t Word) {
      uint8_t Bytes[4];
      for (unsigned I = 0; I < 4; ++I)
        Bytes[I] = Word >> (8 * (Little ? I : 3 - I));
      if (!Error)
        Error = uc_mem_write(CPU, PC, Bytes, 4);
    };
    // Set Config5.MSAEn and Status.CU1/FR, then read MSAIR into t0.
    const uint32_t Setup[SetupWords] = {
        0x40088005, 0x3c090800, 0x01094025, 0x40888005, 0xc0,      0x40086000,
        0x3c092400, 0x01094025, 0x40886000, 0xc0,       0x787e0219};
    for (unsigned I = 0; I < SetupWords; ++I)
      Install(Code + 4 * I, Setup[I]);
    // CTCMSA $1,$16 and INSERT.W W0/W1/W2 from GPR4..15.
    Install(InputPC, 0x783e8059);
    for (unsigned I = 0; I < 12; ++I)
      Install(InputPC + 4 * (I + 1),
              0x79300019 | ((I % 4) << 16) | ((4 + I) << 11) | ((I / 4) << 6));
    // COPY_S.W $4..$7,W0[0..3], CFCMSA $8,$1.
    for (unsigned I = 0; I < 4; ++I)
      Install(OutputPC + 4 * I, 0x78b00019 | (I << 16) | ((4 + I) << 6));
    Install(OutputPC + 16, 0x787e0a19);
    unsigned Index = 0;
    for (const auto &Op : mips::MSANativeOperations) {
      const unsigned WS = Op.Arity == 3 ? 1 : 0, WT = Op.Arity == 3 ? 2 : 1;
      Install(OperationsPC + 4 * Index++,
              Op.Encoding | (WS << 11) | (Op.Arity > 1 ? WT << 16 : 0));
    }
    if (!Error)
      Error = uc_emu_start(CPU, Code, Code + 4 * SetupWords, 0, SetupWords);
    if (!Error)
      Error = readWord(UC_MIPS_REG_8, Implementation);
    if (!Error)
      Error = uc_context_alloc(CPU, &Initial);
    if (!Error)
      Error = uc_context_save(CPU, Initial);
    if (Error)
      return llvm::createStringError("MIPS MSA setup: %s", uc_strerror(Error));
    return llvm::Error::success();
  }
  std::optional<MIPSMSAResult>
  execute(Intrinsic Id, uint32_t Model, uint32_t CSR,
          llvm::ArrayRef<MIPSVector> Inputs) override {
    if (!mips::isMSANativeVector(Id) ||
        Inputs.size() != mips::msaNativeOperation(Id).Arity ||
        Model != Implementation || (CSR & ~0x0107ffffu) ||
        uc_context_restore(CPU, Initial) != UC_ERR_OK)
      return std::nullopt;
    const bool PreserveCSR = mips::isMSADivision(Id) ||
                             Id == Intrinsic::MIPSMSAFCLASS_W ||
                             Id == Intrinsic::MIPSMSAFCLASS_D;
    // FP arithmetic replaces Cause. Reinstalling stale enabled Cause via
    // CTCMSA would itself trap before evaluating the requested instruction.
    uint32_t InitialCSR = CSR & ~0x3f000u;
    if (writeWord(UC_MIPS_REG_16, InitialCSR) != UC_ERR_OK)
      return std::nullopt;
    for (unsigned R = 0; R < Inputs.size(); ++R) {
      for (unsigned I = 0; I < 4; ++I) {
        uint32_t Word = 0;
        for (unsigned J = 0; J < 4; ++J)
          Word |= uint32_t(Inputs[R][4 * I + J]) << (8 * J);
        if (writeWord(UC_MIPS_REG_4 + 4 * R + I, Word) != UC_ERR_OK)
          return std::nullopt;
      }
    }
    const unsigned Setup = 1 + 4 * Inputs.size();
    if (uc_emu_start(CPU, InputPC, InputPC + 4 * Setup, 0, Setup) != UC_ERR_OK)
      return std::nullopt;
    const auto PC =
        OperationsPC + 4 * (unsigned(Id) - unsigned(Intrinsic::MIPSMSAFADD_W));
    const auto Status = uc_emu_start(CPU, PC, PC + 4, 0, 1);
    if (Status != UC_ERR_OK && Status != UC_ERR_EXCEPTION)
      return std::nullopt;
    const auto ReadPC = Status == UC_ERR_OK ? OutputPC : OutputPC + 16;
    if (uc_emu_start(CPU, ReadPC, OutputPC + 20, 0,
                     (OutputPC + 20 - ReadPC) / 4) != UC_ERR_OK)
      return std::nullopt;
    uint32_t State = 0;
    if (readWord(UC_MIPS_REG_8, State) != UC_ERR_OK)
      return std::nullopt;
    const bool Traps = ((State >> 12) & ((State >> 7 & 31) | 32) & 63) != 0;
    if (Status == UC_ERR_EXCEPTION && !Traps)
      return std::nullopt;
    MIPSMSAResult Result{PreserveCSR ? CSR : uint32_t(State), std::nullopt};
    if (Status == UC_ERR_OK) {
      Result.Destination.emplace();
      for (unsigned I = 0; I < 4; ++I) {
        uint32_t Word = 0;
        if (readWord(UC_MIPS_REG_4 + I, Word) != UC_ERR_OK)
          return std::nullopt;
        for (unsigned J = 0; J < 4; ++J)
          (*Result.Destination)[4 * I + J] = Word >> (8 * J);
      }
    }
    return Result;
  }

private:
  uc_err readWord(int Reg, uint32_t &Word) {
    if (!Wide)
      return uc_reg_read(CPU, Reg, &Word);
    uint64_t Value = 0;
    const auto Error = uc_reg_read(CPU, Reg, &Value);
    Word = uint32_t(Value);
    return Error;
  }
  uc_err writeWord(int Reg, uint32_t Word) {
    if (!Wide)
      return uc_reg_write(CPU, Reg, &Word);
    const uint64_t Value = Word;
    return uc_reg_write(CPU, Reg, &Value);
  }
  bool Wide = false;
  uc_engine *CPU = nullptr;
  uc_context *Initial = nullptr;
  uint32_t Implementation = 0;
};
llvm::Expected<std::unique_ptr<MIPSMSAExecutor>> createExecutor(Arch Target) {
  auto Result = std::make_unique<UnicornMSAExecutor>();
  if (auto Err = Result->initialize(Target))
    return Err;
  return Result;
}
} // namespace

llvm::Expected<std::unique_ptr<MIPSMSAExecutor>>
createP5600MSAExecutor(Arch Target) {
  if (Target != Arch::MIPS32 && Target != Arch::MIPS32EL)
    return llvm::createStringError("P5600 MSA requires a MIPS32 target");
  return createExecutor(Target);
}

llvm::Expected<std::unique_ptr<MIPSMSAExecutor>>
createI6400MSAExecutor(Arch Target) {
  if (!isMIPS64(Target))
    return llvm::createStringError("I6400 MSA requires a MIPS64 target");
  return createExecutor(Target);
}
} // namespace neverd::emulation
