//===- WindowsX64ExecutionPolicy.cpp - Driver CPU environment checks ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reject instructions requiring CPU or Windows environment state absent
/// from the configured driver execution profile.
///
//===----------------------------------------------------------------------===//

#include "WindowsX64ExecutionPolicy.h"

#include "../../../arch/x86_64/X64Decoder.h"
#include "../kernel/WindowsKernelLayout.h"

#include "llvm/Support/Error.h"

#include <memory>
#include <string>

namespace neverd::emulation {
namespace {
namespace policy {
#define NEVERD_WINDOWS_EXECUTION_DIAGNOSTIC(Name, Text)                        \
  constexpr char Name[] = Text;
#include "WindowsExecutionDiagnostics.def"
#undef NEVERD_WINDOWS_EXECUTION_DIAGNOSTIC
} // namespace policy

llvm::Error failure(const std::string &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace
WindowsX64ExecutionPolicy::~WindowsX64ExecutionPolicy() {
  if (Handle)
    cs_close(&Handle);
}
llvm::Error WindowsX64ExecutionPolicy::initialize(X64BranchModel Model) {
  if (cs_open(CS_ARCH_X86, x64::decoderMode(Model), &Handle) != CS_ERR_OK ||
      cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return failure(policy::Initialize);
  return llvm::Error::success();
}

llvm::Error WindowsX64ExecutionPolicy::validate(llvm::ArrayRef<uint8_t> Bytes,
                                                uint64_t PC) {
  auto Result = inspect(Bytes, PC);
  if (!Result)
    return Result.takeError();
  return llvm::Error::success();
}

llvm::Expected<std::optional<WindowsX64ExecutionPolicy::Action>>
WindowsX64ExecutionPolicy::inspect(llvm::ArrayRef<uint8_t> Bytes, uint64_t PC) {
  cs_insn *Decoded = nullptr;
  size_t Count = cs_disasm(Handle, Bytes.data(), Bytes.size(), PC, 1, &Decoded);
  if (!Count)
    return failure(policy::Decode);
  std::unique_ptr<cs_insn, void (*)(cs_insn *)> Insn(
      Decoded, [](cs_insn *P) { cs_free(P, 1); });
  if (Insn->size != Bytes.size() || !Insn->detail)
    return failure(policy::Extent);
  auto Rejected = [&]() {
    return failure(std::string(policy::Rejected) + Insn->mnemonic);
  };
  const cs_x86 &X86 = Insn->detail->x86;
  if (Insn->id == X86_INS_CPUID) {
    if (X86.prefix[0] == X86_PREFIX_LOCK || X86.op_count)
      return Rejected();
    return std::optional<Action>{{Action::Kind::ReadCPUID, std::nullopt}};
  }
  if (Insn->id == X86_INS_RDTSC || Insn->id == X86_INS_RDTSCP) {
    if (X86.prefix[0] == X86_PREFIX_LOCK || X86.op_count)
      return Rejected();
    return std::optional<Action>{{Insn->id == X86_INS_RDTSC
                                      ? Action::Kind::ReadTimestamp
                                      : Action::Kind::ReadTimestampAndProcessor,
                                  std::nullopt}};
  }
  // WDK headers inline the IRQL and current-thread queries. Decode only the
  // exact full-width reads whose values belong to the Windows model. Other
  // segment offsets and all writes remain outside this execution profile.
  if (Insn->id == X86_INS_MOV && X86.op_count == 2 &&
      X86.operands[0].type == X86_OP_REG &&
      X86.operands[0].size == sizeof(uint64_t)) {
    const auto &Source = X86.operands[1];
    std::optional<Action::Kind> Kind;
    if (Source.type == X86_OP_REG && Source.reg == X86_REG_CR8)
      Kind = Action::Kind::ReadIRQL;
    if (Kind) {
      if (X86.prefix[0] == X86_PREFIX_LOCK ||
          X86.operands[0].reg == X86_REG_RIP ||
          X86.operands[0].reg == X86_REG_CR8)
        return Rejected();
      switch (X86.operands[0].reg) {
#define NEVERD_X64_REGISTER(Name, DecoderID, BackendID)                        \
  case DecoderID:                                                              \
    return std::optional<Action>{{*Kind, X64Register::Name}};
#include "neverd/emulation/X64Registers.def"
#undef NEVERD_X64_REGISTER
      default:
        return failure(policy::Destination);
      }
    }
  }
  if (cs_insn_group(Handle, Insn.get(), CS_GRP_PRIVILEGE) ||
      cs_insn_group(Handle, Insn.get(), CS_GRP_INT) ||
      cs_insn_group(Handle, Insn.get(), CS_GRP_IRET) ||
      cs_insn_group(Handle, Insn.get(), X86_GRP_VM) ||
      cs_insn_group(Handle, Insn.get(), X86_GRP_SGX) ||
      cs_insn_group(Handle, Insn.get(), X86_GRP_RTM) ||
      cs_insn_group(Handle, Insn.get(), X86_GRP_FSGSBASE))
    return Rejected();
  // These instructions observe or mutate environment state absent from the
  // initialization profile, even where the decoder does not mark privilege.
  switch (Insn->id) {
#define NEVERD_DRIVER_UNSUPPORTED_X64(Instruction) case Instruction:
#include "WindowsX64UnsupportedInstructions.def"
#undef NEVERD_DRIVER_UNSUPPORTED_X64
    return Rejected();
  default:
    break;
  }
  // XLAT reads [RBX + zero_extend(AL)] but has no explicit memory operand in
  // Capstone's detail. Its effective segment override still changes the
  // address, so the ordinary operand walk cannot establish this boundary.
  if (Insn->id == X86_INS_XLATB &&
      (X86.prefix[1] == X86_PREFIX_FS || X86.prefix[1] == X86_PREFIX_GS))
    return failure(policy::ThreadProfile);
  bool ReadsCurrentThread = false;
  for (unsigned I = 0; I < X86.op_count; ++I) {
    const auto &Operand = X86.operands[I];
    if (Operand.type == X86_OP_MEM && (Operand.mem.segment == X86_REG_FS ||
                                       Operand.mem.segment == X86_REG_GS)) {
      if (Operand.mem.segment != X86_REG_GS ||
          Operand.size != sizeof(uint64_t) || Operand.access != CS_AC_READ ||
          Operand.mem.base != X86_REG_INVALID ||
          Operand.mem.index != X86_REG_INVALID ||
          Operand.mem.disp != windows::GSCurrentThreadOffset)
        return failure(policy::ThreadProfile);
      ReadsCurrentThread = true;
    }
    if (Operand.type == X86_OP_REG &&
        (Operand.reg == X86_REG_CS || Operand.reg == X86_REG_DS ||
         Operand.reg == X86_REG_ES || Operand.reg == X86_REG_SS ||
         Operand.reg == X86_REG_FS || Operand.reg == X86_REG_GS))
      return Rejected();
  }
  if (ReadsCurrentThread)
    return std::optional<Action>{
        {Action::Kind::ReadCurrentThread, std::nullopt}};
  return std::optional<Action>{};
}

} // namespace neverd::emulation
